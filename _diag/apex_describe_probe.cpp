// ASK THE HOST FOR ONE FEATURE'S CONTROLS, the way the panel does, and print the document.
//
// This exists so a gate can read what a feature actually sends WITHOUT opening the panel and clicking into a
// feature page -- which is UI interaction, and UI interaction in this project belongs to the user.
//
// It is the real protocol (settings_ipc.h): a `describe` request over WM_COPYDATA, and the answer arrives as a
// second WM_COPYDATA addressed to this process's own window. The window is what makes the reply possible --
// WM_COPYDATA sends TO a window, so this has to have one.
//
// ⚠️ IT ONLY TALKS TO THE HOST IN THE FOLDER IT IS TOLD TO. The host window is found by class AND by the
// directory the owning process runs from -- the same rule the product uses, because another copy of Apex
// registers the same window class (see settings_ipc.h).
//
// usage: apex_describe_probe.exe <install-dir> <slot>
//        apex_describe_probe.exe <install-dir> snapshot        <- the WHOLE state, not one feature's controls
//        apex_describe_probe.exe <install-dir> off=<slot>,<0|1> <- the user's own on/off switch for that feature
//
// ⚠️ THE TWO EXTRA MODES EXIST BECAUSE NOTHING PARSED THE HOST'S MAIN DOCUMENT OR CHECKED WHAT IT SAYS, AND BOTH
// COST A BUILD-AND-DEPLOY CYCLE. The snapshot is the one thing the page cannot work without -- `JSON.parse`
// failing on it makes the panel say "the Apex host is not running" while the host answers every message -- and
// there was no gate that read it (the note in settings_host.cpp says so out loud). And a feature's master switch
// is the user's rule for what the flyout may show ("当插件总开关关闭后…其插件的局部快速面板功能要隐藏"), which is
// a statement about the SNAPSHOT's `quickOrder` -- so it can be checked by asking for one. Both are read by
// check_apex_persist.sh, where a real host is already running.
#include "settings_ipc.h"

#include <stdio.h>

using namespace apex;

static char g_answer[kIpcMaxDocument];
static int g_answerLen = 0;

static LRESULT CALLBACK ProbeWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_COPYDATA)
  {
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)l;
    if (cds && cds->dwData == kIpcAnswer && cds->lpData)
    {
      const IpcDocument *d = (const IpcDocument *)cds->lpData;
      const int n = (cds->cbData >= sizeof(IpcDocument))
                        ? d->length
                        : (int)cds->cbData - (int)offsetof(IpcDocument, text);
      if (n > 0 && n < (int)sizeof(g_answer))
      {
        memcpy(g_answer, d->text, (size_t)n);
        g_answer[n] = 0;
        g_answerLen = n;
      }
    }
    return 1;
  }
  return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    printf("usage: apex_describe_probe.exe <install-dir> <slot>\n");
    return 2;
  }

  // A window of our own, because that is where the answer is sent.
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = ProbeWndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "ApexDescribeProbeWnd";
  RegisterClassA(&wc);
  HWND self = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              GetModuleHandleA(nullptr), nullptr);
  if (!self)
  {
    printf("could not create a message window\n");
    return 2;
  }

  // The host, in the folder asked for.
  struct S
  {
    const char *dir;
    HWND found;
  } s = {argv[1], nullptr};
  struct Helper
  {
    static BOOL CALLBACK Proc(HWND h, LPARAM lp)
    {
      S *st = (S *)lp;
      char cls[128] = {0};
      GetClassNameA(h, cls, sizeof(cls));
      if (strcmp(cls, APEX_HOST_WND_CLASS) != 0)
        return TRUE;
      DWORD pid = 0;
      GetWindowThreadProcessId(h, &pid);
      HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!p)
        return TRUE;
      char path[MAX_PATH] = {0};
      DWORD n = MAX_PATH;
      const BOOL ok = QueryFullProcessImageNameA(p, 0, path, &n);
      CloseHandle(p);
      if (!ok)
        return TRUE;
      for (char *c = path; *c; ++c)
        if (*c == '\\')
          *c = '/';
      if (_strnicmp(path, st->dir, strlen(st->dir)) != 0)
        return TRUE;
      st->found = h;
      return FALSE;
    }
  };
  EnumWindows(Helper::Proc, (LPARAM)&s);
  if (!s.found)
  {
    printf("no host window found for \"%s\"\n", argv[1]);
    return 1;
  }

  // THE WHOLE STATE, when the caller asks for it (see the usage note above): the same request shape with the
  // other command, and no slot -- the snapshot is not about one feature. And the master switch, which is a
  // request of its own (`kIpcFeatureOff`).
  const bool snapshot = (strcmp(argv[2], "snapshot") == 0);
  const bool featureOff = (strncmp(argv[2], "off=", 4) == 0);
  int offSlot = 0, offValue = 0;
  if (featureOff)
    sscanf(argv[2] + 4, "%d,%d", &offSlot, &offValue);

  char text[128] = {0};
  if (featureOff)
    _snprintf(text, sizeof(text), "slot=%d\nvalue=%d\n", offSlot, offValue);
  else if (!snapshot)
    _snprintf(text, sizeof(text), "slot=%s\n", argv[2]);

  const unsigned cmd = featureOff ? (unsigned)kIpcFeatureOff : (snapshot ? (unsigned)kIpcSnapshot : (unsigned)kIpcDescribe);

  IpcRequest req;
  ZeroMemory(&req, sizeof(req));
  req.cmd = cmd;
  _snprintf(req.text, sizeof(req.text), "%s", text);

  COPYDATASTRUCT cds;
  cds.dwData = cmd;
  cds.cbData = sizeof(req);
  cds.lpData = &req;

  // The answer is sent from inside this call, so by the time it returns it has already been handled (the reply
  // is a SENT message, and SendMessage delivers synchronously -- see the protocol's note).
  SendMessageA(s.found, WM_COPYDATA, (WPARAM)self, (LPARAM)&cds);

  if (g_answerLen <= 0)
  {
    printf("no answer\n");
    return 1;
  }
  fwrite(g_answer, 1, (size_t)g_answerLen, stdout);
  return 0;
}
