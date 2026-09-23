#ifndef APEX_SETTINGS_IPC_H
#define APEX_SETTINGS_IPC_H

// ⚠️ A FEATURE MAY NOT INCLUDE THIS FILE -- the guard below is what makes that a CONTRACT rather than a
// convention. A feature is compiled with the same -I paths as the host (see apex/build.sh) and exports a C
// ABI, so nothing else stops it reaching in here and calling host internals the ABI never promised to keep
// stable. Everything a feature is entitled to is in abi.h; this file is the settings protocol between the host and the panel.
#ifndef APEX_BUILDING_HOST
#error "settings_ipc.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif

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
  kIpcSetControl = 3, // "set one control"                          (payload: slot=.. id=<path>.. value=..)
  kIpcSetHost = 4,    // "change a host setting"                    (payload: key=.. value=..)
  kIpcListOp = 5,     // "add/remove a row in a feature's list"      (payload: slot=.. id=.. op=.. value=.. index=..)
  // ⚠️ 6 WAS `kIpcLive` -- "what is this feature doing right now?", the command behind the feature's one-line
  // read-out. The read-out is gone from both ends (see apex/abi.h, ABI 9 -> 10: the user tried it, then twice
  // said it was not needed), so the command is gone too.
  //
  // ⚠️ AND THE NUMBER IS RETIRED RATHER THAN REUSED, WHICH MATTERS MORE HERE THAN IN THE ABI. The panel and the
  // host are two PROCESSES, and a panel left open from an older build keeps talking to a newer host across a
  // deploy. If 6 came back meaning something else, that old panel's request would do the WRONG THING -- quietly,
  // because the codes are just numbers on the wire. A retired number costs one line of comment; a reused one
  // costs a mystery. (The host no longer answers 6 at all, which is the correct answer to a request that has
  // stopped existing.)
  kIpcFeatureOff = 7, // "leave this feature loaded but inactive"   (payload: slot=N value=0|1)
  kIpcSave = 8,       // "persist everything now"
  kIpcOpenFile = 9,   // "open that feature's settings file"        (payload: slot=N)
  kIpcQuitHost = 10,  // "quit Apex" (from the panel's own menu)

  // ---- host -> panel ----
  kIpcAnswer = 100, // the document the panel asked for
  // ⚠️ 101 WAS `kIpcLiveAnswer`, the read-out's own reply code -- retired with the read-out, on the same terms
  // as 6 above and for the same reason.
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

// The two executables, by name. The name IS the identity (see below), so it is spelled once here rather than
// at each of the six lookups -- a lookup that forgot which exe owns which class is exactly the mistake this
// file exists to prevent.
#define APEX_HOST_EXE "apex.exe"
#define APEX_SETTINGS_EXE "apex-settings.exe"

// ---------------------------------------------------------------------------
// ⚠️ THE CLASS NAME IS NOT ENOUGH TO IDENTIFY A WINDOW -- ANY PROCESS CAN REGISTER ONE.
//
// A window class says WHAT a window is, not whose it is. `FindWindowA("ApexSettingsWnd")` returns whichever
// window happens to be first on the machine, which is how a panel came to find another copy's panel, bring
// THAT to the front, and exit -- leaving the user looking at a different installation's settings while being
// told they were the ones they had just opened. Nothing errored anywhere.
//
// THE IDENTITY IS THE EXECUTABLE NAME. That is a deliberate change from the older, narrower rule ("same
// folder as the caller"), which existed because two copies of the portable folder were two installations that
// had to be able to coexist. They no longer are, and cannot be:
//
//   ⚠️ THE USER'S RULE: "进程只能有一个Apex.exe" -- ONE APEX PER MACHINE, WHICHEVER FOLDER IT CAME FROM.
//
// Two hosts at once are not a user-interface annoyance, they are a functional fault. Apex hooks the wheel
// globally, so two hosts are two hooks on every wheel: each swallows what it sees and injects its own output,
// and the second one sees the first one's injected events. That is precisely the "two handlers driving one
// view" that decision.h is built to avoid -- and the folder rule could not prevent it, because two folders
// were exactly the case it allowed.
//
// So a found window is accepted when its process image is the executable that owns that class. The single
// instance check in the host (main.cpp) uses the same lookup, which is what makes the rule true rather than
// merely stated: the second launch finds the first one's window and exits.
//
// IT COSTS ONE QueryFullProcessImageName PER CANDIDATE WINDOW, at startup, on a menu click, or once per
// panel-liveness poll. It is NOT called from the wheel path or the frame.
// ---------------------------------------------------------------------------

// Is this window owned by the process with this image name? ("apex.exe", "apex-settings.exe")
inline bool WindowOwnedByExecutable(HWND w, const char *exeName)
{
  if (!w || !exeName || !*exeName)
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (!pid)
    return false;

  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return false;
  char theirs[MAX_PATH] = {0};
  DWORD n = MAX_PATH;
  const BOOL ok = QueryFullProcessImageNameA(h, 0, theirs, &n);
  CloseHandle(h);
  if (!ok)
    return false;

  // The image name only: the folder is deliberately not part of the test (see the rule above).
  const char *base = strrchr(theirs, '\\');
  base = base ? base + 1 : theirs;
  return _stricmp(base, exeName) == 0;
}

// The window of an Apex process, by class -- or nullptr.
//
// ⚠️ `FindWindowA` CANNOT DO THIS, AND CALLING IT ONCE IS NOT ENOUGH. It takes a class and a title, and the
// title is not a discriminator (the host's is empty and the panel's is "Apex" in every installation). More
// importantly it returns ONE window and cannot check who owns it, so a foreign window that reserved the class
// name first would be reported as ours. The enumeration below walks every window of the class and takes the
// one whose process is the expected executable.
struct ApexWindowSearch
{
  const char *className;
  const char *exeName;
  HWND found;
};

inline BOOL CALLBACK ApexWindowEnumProc(HWND h, LPARAM lp)
{
  ApexWindowSearch *s = (ApexWindowSearch *)lp;
  char cls[128] = {0};
  GetClassNameA(h, cls, sizeof(cls));
  if (strcmp(cls, s->className) != 0)
    return TRUE;
  if (!WindowOwnedByExecutable(h, s->exeName))
    return TRUE; // not the process we are looking for: keep looking
  s->found = h;
  return FALSE; // ours: stop
}

inline HWND FindApexWindow(const char *className, const char *exeName)
{
  if (!className || !exeName)
    return nullptr;
  ApexWindowSearch s = {className, exeName, nullptr};
  EnumWindows(ApexWindowEnumProc, (LPARAM)&s);
  return s.found;
}

// The two lookups the rest of the program reads as sentences.
inline HWND FindApexHost() { return FindApexWindow(APEX_HOST_WND_CLASS, APEX_HOST_EXE); }
inline HWND FindApexPanel() { return FindApexWindow(APEX_SETTINGS_WND_CLASS, APEX_SETTINGS_EXE); }

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

// ---- host -> panel: THE PANEL WAS WARMED UP, AND WHETHER IT WAS ACTUALLY WANTED ----
//
// WHY THESE EXIST. The panel cannot show anything until WebView2 has started -- measured at ~0.55 s with a warm
// browser data folder (~1.4 s on the very first run), and that wait is what the user feels when opening the
// settings. The window is therefore created hidden and appears when the page has drawn (see ShowPanelOnce in
// ui_webview.cpp) -- correct, but it means a visible pause between the click and the window.
//
// The pause is bought back by starting the panel BEFORE it is asked for: on the tray's right-click, while the
// user is still moving to the menu item. That warmed panel is hidden and must NOT appear on its own -- the user
// has opened a menu, not the settings -- so the host tells it which of the two things happened when the menu
// closes:
//
//   SHOW -- the user picked Settings: appear as soon as you have something to show.
//   DROP -- anything else, including dismissing the menu: you were not needed, exit.
//
// ⚠️ WHY A MESSAGE RATHER THAN ShowWindow FROM THE HOST. A warmed panel may still be loading its page, and a
// cross-process ShowWindow would put that flat rectangle on screen -- exactly what the hidden window exists to
// avoid. Only the PAGE knows when it has drawn, so only the panel may decide to appear; this message is how it
// learns that it should.
#define APEX_PANEL_SHOW_MSG_NAME "ApexPanelShow"
#define APEX_PANEL_DROP_MSG_NAME "ApexPanelDrop"

// The command line that says "you are a warm-up": start hidden and wait to be told (see settings_main.cpp).
#define APEX_PANEL_WARM_ARG "--warm"

// How often the host looks at the counter and tells the panel. ~30 ms is a frame's worth of lag for an
// animation and keeps the hop off the wheel's critical path entirely; the count is coalesced, so a fast roll
// still arrives as accurate numbers, just in bigger batches.
static const int kActivityNotifyMs = 30;

// ---- host -> panel: A FACT WORTH REDRAWING FOR ----
//
// ⚠️ A PAGE THAT ONLY RE-READS ITS CONTROLS WHEN A FEATURE IS SELECTED CANNOT SHOW ANYTHING THAT CHANGES --
// and the REAPER note does change while the panel is open (the user starts REAPER, or closes it, with the
// settings page already up). The first version of that note appeared only after switching features and back,
// which the user rightly called out: that is not detecting, it is "refreshing if you happen to ask again".
//
// THE HOST CHECKS, THE PANEL IS TOLD ONLY WHEN THE ANSWER CHANGES. Same rule as the count above (a healthy
// session produces no messages). It has to be this way round because the PAGE cannot see other processes at
// all -- the question can only be asked on the host's side.
//
// The WPARAM says WHAT changed; the page decides what to re-read.
#define APEX_STATE_MSG_NAME "ApexStateNotify"

// How often the host looks. ONE SECOND, and the interval is a measured trade rather than a habit:
//   * the check is ~0.5 MICROSECONDS in the common case -- a window lookup, on this machine 7000x cheaper than
//     walking 252 processes (3.59 ms, both measured; see host_win.cpp), and the authoritative walk runs every
//     30 s instead of on every tick;
//   * a user who starts REAPER and then looks at the panel sees the note within about a second, which is the
//     difference between "it noticed" and "it noticed when I flipped pages";
//   * one second is also under the ~2 s a person gives a screen before deciding it is stale.
static const int kStateNotifyMs = 1000;

// WPARAM values for APEX_STATE_MSG_NAME.
static const unsigned kStateReaper = 1u; // the REAPER-plugin answer may have changed

// ⚠️ SOMETHING THE USER JUST CHANGED IN THE QUICK PANEL. The flyout and the settings window are two views of
// one state -- the host's feature list, and the values inside each feature -- and the two can be on screen at
// the same time: the flyout takes the focus when it opens, but the settings page stays behind it, showing what
// was true when it last read. The user's words: "快速面板的开关要能实时同步到设置面板上".
//
// It is the SAME message and the same page entry point as kStateReaper (`window.__apexStateChanged`), and the
// page does what it does for REAPER: re-read the controls. It does not need to know WHICH control changed --
// teaching it that would mean the page knowing the features' ids, which is the coupling the controls document
// exists to avoid (see the note in panel.js).
static const unsigned kStateQuickPanel = 2u;

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
    HWND host = FindApexHost();
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
  bool HostIsRunning() const { return FindApexHost() != nullptr; }

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
