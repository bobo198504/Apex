#ifndef APEX_SETTINGS_IPC_H
#define APEX_SETTINGS_IPC_H

// ---------------------------------------------------------------------------
// HOW THE SETTINGS WINDOW TALKS TO THE HOOK, now that they are separate processes.
//
// WHY THEY ARE SEPARATE AT ALL: the hook lives in the OS input path. A panel that renders a browser, waits
// on the user, and can be killed from Task Manager must not be able to affect it. Splitting them means the
// panel can be written, rebuilt, hung, or crash-restarted while smoothing keeps running, and it is also
// what lets the panel be exercised on its own.
//
// THE TRANSPORT IS WM_COPYDATA, both ways. Not a pipe, not shared memory, not a socket, and not a
// registered message carrying a pointer.
//
// ⚠️ THE POINTER RULE, WHICH IS THE ONE THING TO GET RIGHT HERE: a pointer means nothing in another
// process. The first version of this protocol sent a request containing the ADDRESS of the caller's answer
// buffer, for the host to fill in -- which would have had the host writing into its own address space and
// the panel reading an untouched buffer. WM_COPYDATA exists precisely to avoid that: the OS COPIES the
// block, so the receiver gets its own copy and neither side ever dereferences the other's memory.
//
// So an exchange is two messages:
//
//     panel  --WM_COPYDATA(request)-->  host
//     panel  <--WM_COPYDATA(answer)---  host      (sent from inside the handler, i.e. before the first
//                                                  SendMessage returns)
//
// Because the host's reply is sent while the panel is still inside its own SendMessage, the answer has
// already arrived by the time that call returns. The panel's window procedure stores it; Send() then hands
// it back. That is why Send() is still usable as a single call at the call site even though two messages
// crossed.
//
// The panel is a CLIENT: it asks, the host answers. The host never initiates except for one notification
// ("something changed on my side"), which the panel can ignore entirely without breaking.
//
// WHAT TRAVELS: control ids, numbers, and JSON text. What does NOT: function pointers, feature structs, or
// any C++ type -- those cannot cross a process boundary, and a protocol that tried would tie the two
// binaries together so tightly that rebuilding one would break the other.
// ---------------------------------------------------------------------------

#include "abi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace apex {

// Requests and replies, in `dwData` of a COPYDATASTRUCT. Explicit values so a mismatch is obvious rather
// than an off-by-one that silently does something else.
enum ApexIpc : unsigned long
{
  // ---- panel -> host ----
  kIpcSnapshot = 1,   // "send me the whole state as JSON"
  kIpcDescribe = 2,   // "send me that feature's controls as JSON"   (payload: slot=N)
  kIpcSetFeature = 3, // "change a feature's control"               (payload: slot=.. id=.. value=..)
  kIpcSetHost = 4,    // "change a host setting"                    (payload: key=.. value=..)
  kIpcListOp = 5,     // "add/remove a row in a feature's list"      (payload: slot=.. id=.. op=.. value=.. index=..)
  kIpcFeatureOff = 7, // "leave this feature loaded but inactive"   (payload: slot=N value=0|1)
  kIpcSave = 8,       // "persist everything now"
  kIpcOpenFile = 9,   // "open that feature's settings file"        (payload: slot=N)
  kIpcQuitHost = 10,  // "quit Apex" (from the panel's own menu)

  // ---- host -> panel ----
  kIpcAnswer = 100, // the document the panel asked for
};

// The REPLY CODE is the handler's return value, i.e. what SendMessage gives back. It says whether the host
// understood the request, and is deliberately separate from the document: a change request produces no
// document, so the code is the only thing that can report success.
enum ApexIpcResult
{
  kIpcOk = 0,
  kIpcBadRequest = 1,
  kIpcNoSuchSlot = 2,
  kIpcRejected = 3,
  kIpcNoHost = 4 // never returned by the host: this side reports it when no host window is there
};

// A small request. Text payloads are NUL-terminated and deliberately short: WM_COPYDATA has no documented
// size limit, but it is delivered on the receiving thread, so a large payload blocks the sender -- and the
// sender is the panel, whose whole job is to feel responsive.
static const int kIpcMaxPayload = 3072;

// A document (a snapshot, or one feature's controls) is larger and gets its own budget.
static const int kIpcMaxDocument = 64 * 1024;

// ---- the two window classes, so each side can find the other ----
//
// A window class name is a stable, readable handle on "the Apex host window" and "the settings window",
// needing no port number to be configured, discovered, or kept in step.
#define APEX_HOST_WND_CLASS "ApexHostWnd"
#define APEX_SETTINGS_WND_CLASS "ApexSettingsWnd"

// ---------------------------------------------------------------------------
// ⚠️ THE CLASS NAME IS NOT ENOUGH TO IDENTIFY A WINDOW -- IT IS NOT UNIQUE ON A MACHINE.
//
// Apex is portable: "整个文件夹拷走 = 整个程序搬走". Two copies of the folder are therefore two installations,
// and BOTH register the same window classes, because the class name says WHAT the window is and has no idea
// which copy it belongs to. `FindWindowA("ApexSettingsWnd")` then returns whichever window happens to be
// first -- the other installation's.
//
// THE BUG THIS FIXES, found while writing the shutdown gate: a panel started from a second copy found the
// FIRST copy's panel, brought it to the front, and exited. So the second copy's panel never appeared, and the
// user was left looking at another installation's settings, being told they were the ones they had just
// opened. Nothing errored anywhere.
//
// THE FIX: a window found by class is only accepted when the process that owns it is running from the
// SAME DIRECTORY as the caller. That is exactly "is this MY other half", and it needs no extra state, no
// port, and no registry -- the path is read from the process itself.
//
// ⚠️ THE DIRECTORY, NOT THE EXE PATH. The two processes of one installation are DIFFERENT EXECUTABLES
// (apex.exe and apex-settings.exe) sitting side by side -- comparing full paths would reject the only window
// we are looking for. The folder is what a portable install IS ("copy the folder, the whole program moves"),
// so the folder is what identifies it.
//
// IT COSTS ONE QueryFullProcessImageName PER CALLER, at startup or on a menu click. It is NOT called from
// the wheel path or the frame.
// ---------------------------------------------------------------------------
// The directory part of a path, with the trailing separator kept. Written out rather than using a string
// helper because the paths here are ANSI Win32 paths and this must not allocate.
inline bool SameInstallDirectory(const char *a, const char *b)
{
  if (!a || !b || !*a || !*b)
    return false;
  const char *sa = strrchr(a, '\\');
  const char *sb = strrchr(b, '\\');
  if (!sa || !sb)
    return false;
  const size_t la = (size_t)(sa - a) + 1;
  const size_t lb = (size_t)(sb - b) + 1;
  if (la != lb)
    return false;
  // Case-insensitive: Windows paths are, and the same folder reached by two spellings is still one folder.
  return _strnicmp(a, b, la) == 0;
}

inline bool WindowBelongsToThisInstall(HWND w)
{
  if (!w)
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (!pid)
    return false;

  char mine[MAX_PATH] = {0};
  if (!GetModuleFileNameA(nullptr, mine, MAX_PATH))
    return false;
  char theirs[MAX_PATH] = {0};
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return false;
  DWORD n = MAX_PATH;
  const BOOL ok = QueryFullProcessImageNameA(h, 0, theirs, &n);
  CloseHandle(h);
  if (!ok)
    return false;
  return SameInstallDirectory(mine, theirs);
}

// The window of one of OUR OWN processes, by class -- or nullptr.
//
// ⚠️ `FindWindowA` CANNOT DO THIS, AND CALLING IT ONCE IS NOT ENOUGH. It takes a class and a title, and the
// title is not a discriminator (the host's is empty and the panel's is "Apex" in every installation). More
// importantly it returns ONE window -- so if another copy's window happens to be found first, a single call
// reports "not mine" and misses the window that IS mine. The enumeration below walks every window of the
// class and takes the one from this install's folder.
//
// A window that fails the folder test is not an error: it belongs to another copy of Apex, which is a normal
// thing on a machine (the program is portable and the gates run private copies of it).
//
// WHAT IT MEANS WHEN IT RETURNS nullptr WHILE ANOTHER COPY IS RUNNING: this copy's other half is simply not
// up. The panel then starts and reports "the host is not running", which is true OF THIS COPY, and the host
// launches the panel beside itself. That is the intended behaviour for a portable program: two copies coexist
// and neither can drive the other.
struct OwnWindowSearch
{
  const char *className;
  HWND found;
};

inline BOOL CALLBACK OwnWindowEnumProc(HWND h, LPARAM lp)
{
  OwnWindowSearch *s = (OwnWindowSearch *)lp;
  char cls[128] = {0};
  GetClassNameA(h, cls, sizeof(cls));
  if (strcmp(cls, s->className) != 0)
    return TRUE;
  if (!WindowBelongsToThisInstall(h))
    return TRUE; // another copy's window: keep looking
  s->found = h;
  return FALSE; // ours: stop
}

inline HWND FindOwnWindow(const char *className)
{
  if (!className)
    return nullptr;
  OwnWindowSearch s = {className, nullptr};
  EnumWindows(OwnWindowEnumProc, (LPARAM)&s);
  return s.found;
}

// THE "SOMETHING HAPPENED" NOTIFICATION, host -> panel, carrying a COUNT in the WPARAM.
//
// ⚠️ NOT WM_COPYDATA, AND NOT SENT. This one fires while the user is scrolling, so it has to be the cheapest
// possible hop: PostMessage puts it in the panel's queue and returns, where SendMessage would block the host's
// thread on whatever the panel happens to be doing. (WM_COPYDATA cannot be posted at all -- it is sent-only by
// design -- which is why this is a separate mechanism rather than another kIpc* code.)
//
// It is a REGISTERED message rather than WM_APP+N: both processes ask Windows for the same id by name, so it
// cannot collide with a number either side happens to use for its own purposes (WebView2 posts to the panel's
// window too). Registered once per process; 0 means the registration failed, which costs an animation.
#define APEX_ACTIVITY_MSG_NAME "ApexActivityNotify"

// How often the host looks at the counter and tells the panel. ~30 ms is a frame's worth of lag for an
// animation and keeps the hop off the wheel's critical path entirely; the count is coalesced, so a fast roll
// still arrives as accurate numbers, just in bigger batches.
static const int kActivityNotifyMs = 30;

struct IpcRequest
{
  unsigned long cmd;
  char text[kIpcMaxPayload];
};

struct IpcDocument
{
  unsigned long cmd;
  int length; // bytes of `text`, not counting a terminator
  char text[kIpcMaxDocument];
};

// ===========================================================================
// THE TWO SIDES
// ===========================================================================

// ---- the panel's half: send a request, receive the document ----
//
// INSTANCE STATE, not globals: the panel may one day show two features at once, and a single global answer
// buffer would then be a bug waiting for the second one to be added. It is also what makes this testable.
class IpcClient
{
public:
  // The panel's own window, which is where the host sends the answers. Set before the first Send().
  void Attach(HWND self) { self_ = self; }

  // Send a request. Returns the host's reply code (kIpcOk when it understood).
  //
  // ⚠️ THE ANSWER IS NOT COLLECTED HERE, AND THAT IS A DELIBERATE SIMPLIFICATION. This class used to keep a
  // copy of the last document (OnCopyData/Answer/HaveAnswer, fed from the window procedure). Nothing ever read
  // it: the panel's WndProc handles WM_COPYDATA itself -- it has to, because that is the only place which
  // knows whether a document is a snapshot or a feature's controls -- so the copy was a second path for the
  // same bytes, and a reader of this file could believe the answer arrived through Send(). It does not.
  unsigned long Send(unsigned long cmd, const char *text)
  {
    HWND host = FindOwnWindow(APEX_HOST_WND_CLASS);
    if (!host)
      return (unsigned long)kIpcNoHost;

    IpcRequest req;
    ZeroMemory(&req, sizeof(req));
    req.cmd = cmd;
    if (text)
      _snprintf(req.text, sizeof(req.text), "%s", text);

    COPYDATASTRUCT cds;
    cds.dwData = cmd;
    cds.cbData = sizeof(req);
    cds.lpData = &req;
    return (unsigned long)SendMessageA(host, WM_COPYDATA, (WPARAM)self_, (LPARAM)&cds);
  }

  // Is the host still there? Asked by the panel's liveness poll (see ui_webview.cpp) -- the one question a
  // panel process can answer about another process, and the reason the PAGE cannot ask it: a browser page
  // cannot see other processes' windows at all.
  bool HostIsRunning() const { return FindOwnWindow(APEX_HOST_WND_CLASS) != nullptr; }

private:
  HWND self_ = nullptr;
};

// ---- the host's half ----
namespace host {
// Called from the host's window procedure for WM_COPYDATA. Handles a request and, where one is due, sends
// the document back to `sender` before returning.
LRESULT SettingsIpc(HWND sender, const COPYDATASTRUCT *cds);
// Remember which window the panel is, so the host never has to search for it. Called by SettingsIpc with the
// sender of every request; see the note on g_panelWnd in main.cpp for why the search is worth avoiding.
void SettingsPanelSeen(HWND panel);
// Build the two documents. Exposed for the gate (test/check_apex_settings.sh) so the JSON can be inspected
// without a running panel.
int SettingsBuildSnapshot(char *out, int outSize);
int SettingsBuildDescribe(int slot, char *out, int outSize);
// Tell the panel its view is stale.
} // namespace host

// The request payloads are "key=value" lines -- trivial to generate on the panel's side and trivial to read
// on the host's, which avoids a JSON parser in the host (the one place a malformed document must not be
// possible, because it runs in the input path's process).
inline void IpcField(char *out, int outSize, const char *key, const char *value)
{
  if (!out || outSize <= 0)
    return;
  const int n = (int)strlen(out);
  _snprintf(out + n, outSize - n, "%s%s=%s\n", n ? "" : "", key, value ? value : "");
}

inline void IpcFieldNum(char *out, int outSize, const char *key, double value)
{
  char buf[64] = {0};
  _snprintf(buf, sizeof(buf), "%.10g", value);
  IpcField(out, outSize, key, buf);
}

} // namespace apex

#endif // APEX_SETTINGS_IPC_H
