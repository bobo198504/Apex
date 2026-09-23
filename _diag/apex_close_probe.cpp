// ASK THE HOST TO CLOSE, the same way the panel's own "Quit" does.
//
// Used by test/check_apex_shutdown.sh, which verifies the promise the user made explicit:
// "主界面在托盘进程退出，也要跟着关闭，插件进程什么的要退干净".
//
// It sends WM_CLOSE to the host's window -- the SAME message the tray's Quit and the panel's Quit both
// produce (see IDM_QUIT in main.cpp and kIpcQuitHost in settings_host.cpp) -- so the test exercises the real
// exit path rather than a kill. That matters: a `taskkill /F` skips every teardown step, which is exactly
// what this gate is checking.
//
// IT TAKES A PID, so it can only ever close the copy the test started. There is a real apex.exe belonging to
// the user on this machine, and a test that closed THAT would be the same class of mistake as the
// `taskkill /IM apex.exe` that used to be here.
//
// usage: apex_close_probe.exe <pid>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The class comes from the product's own header rather than being spelled again here -- it is the identity
// the host, the panel and the IPC all use, and a copy of the literal is a copy that can drift.
#include "settings_ipc.h"

struct Find { DWORD pid; HWND wnd; };

static BOOL CALLBACK OnTop(HWND h, LPARAM lp)
{
  Find *f = (Find *)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != f->pid)
    return TRUE;
  char cls[128] = {0};
  GetClassNameA(h, cls, sizeof(cls));
  if (strcmp(cls, APEX_HOST_WND_CLASS) == 0)
  {
    f->wnd = h;
    return FALSE; // stop
  }
  return TRUE;
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("usage: apex_close_probe.exe <pid>\n");
    return 2;
  }
  Find f = {(DWORD)strtoul(argv[1], nullptr, 10), nullptr};
  EnumWindows(OnTop, (LPARAM)&f);
  if (!f.wnd)
  {
    printf("no ApexHostWnd in pid %lu\n", (unsigned long)f.pid);
    return 1;
  }
  printf("asking pid %lu (hwnd %p) to close\n", (unsigned long)f.pid, (void *)f.wnd);
  PostMessageA(f.wnd, WM_CLOSE, 0, 0);
  return 0;
}
