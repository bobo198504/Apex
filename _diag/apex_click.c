// apex_click -- click (or double-click) a point INSIDE a named window, then put the mouse back.
//
// ⚠️⚠️ MANUAL-ONLY. NEVER RUN THIS FROM A GATE, A BUILD SCRIPT, OR ANY AUTOMATIC FLOW.
//
// It moves the user's cursor and clicks their mouse, and the user's standing rule is that UI interaction
// testing belongs to THEM: "一切UI交互测试都由我来做，我会反馈给你。能直接跑的算法数据可以由你来测."
//
// This header used to justify the tool on the grounds that it restores the cursor afterwards. Restoring is
// better than not restoring, but it is not the standard -- the standard is that an automated check does not
// touch the user's input device at all. This project already lost the user's trust once on exactly that
// ("该项目每次部署都在抢用户鼠标"), and a tool that puts the cursor back a second later is still a tool that
// yanked it away for a second.
//
// ⚠️ THE MECHANICAL CHECK LIVES IN test/check_apex_modular.sh (section 8): a file that moves the cursor or
// clicks must carry this MANUAL-ONLY marker, and no script named in run_all.sh may do either. Deleting the
// marker does not make a tool safe -- it makes the gate fail.
//
// WHY IT EXISTS AT ALL. The panel's views are switched by clicking, and more than one reported bug was a click
// that went nowhere ("开关不能用", "列表和通用还不能正常的切换"). No amount of reading the script finds
// that class of failure: the script is correct and the WIRING, or the hit-test, is not. So it stays in the
// toolbox -- for the user, who owns UI testing, to run deliberately when a click fault is suspected.
//
// ⚠️ AND IT FINDS THE WINDOW BY PROCESS, NOT BY CLASS NAME. FindWindowA does not reliably match a window
// registered with RegisterClassA on this machine (recorded in AGENTS.md, found while measuring the caption),
// so the window is located the same way the other panel tools do it: enumerate the top-level windows, match
// the exe name, then the class.
//
// Build: gcc -O2 -o build/apex_click.exe _diag/apex_click.c -luser32
// Run:   apex_click.exe <exe-name> <class-name> <clientX,clientY|x2:clientX,clientY> [...]
//
// Each point is clicked in turn with a short pause between. A point written as `x2:10,20` is DOUBLE-clicked,
// which is how a slider's "restore the default" is tested -- two separate clicks are not a double-click, and
// the interval is what makes it one. Exit 0 if every click was dispatched.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *g_exe = nullptr;
static const char *g_cls = nullptr;
static HWND g_found = nullptr;

static BOOL CALLBACK Visit(HWND h, LPARAM)
{
  if (g_found)
    return FALSE;
  if (!IsWindowVisible(h))
    return TRUE;
  char cls[128] = {0};
  GetClassNameA(h, cls, sizeof(cls) - 1);
  if (_stricmp(cls, g_cls) != 0)
    return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return TRUE;
  char path[MAX_PATH] = {0};
  DWORD n = sizeof(path);
  const BOOL ok = QueryFullProcessImageNameA(p, 0, path, &n);
  CloseHandle(p);
  if (!ok)
    return TRUE;
  const char *base = strrchr(path, '\\');
  base = base ? base + 1 : path;
  if (_stricmp(base, g_exe) == 0)
    g_found = h;
  return TRUE;
}

int main(int argc, char **argv)
{
  if (argc < 4)
  {
    printf("usage: apex_click.exe <exe-name> <class-name> <x,y> [x,y ...]\n");
    return 2;
  }
  g_exe = argv[1];
  g_cls = argv[2];

  for (int i = 0; i < 40 && !g_found; ++i)
  {
    EnumWindows(Visit, 0);
    if (!g_found)
      Sleep(250);
  }
  if (!g_found)
  {
    printf("no window: %s / %s\n", g_exe, g_cls);
    return 1;
  }

  POINT saved;
  GetCursorPos(&saved);

  ShowWindow(g_found, SW_RESTORE);
  SetForegroundWindow(g_found);
  Sleep(300);

  POINT origin = {0, 0};
  ClientToScreen(g_found, &origin);

  int clicked = 0;
  for (int i = 3; i < argc; ++i)
  {
    const char *spec = argv[i];
    int times = 1;
    // "x2:10,20" -- a double-click. Written this way rather than as a flag so a caller can mix them.
    if (_strnicmp(spec, "x2:", 3) == 0)
    {
      times = 2;
      spec += 3;
    }
    int x = 0, y = 0;
    if (sscanf(spec, "%d,%d", &x, &y) != 2)
      continue;
    const int sx = origin.x + x, sy = origin.y + y;

    // What is actually at that point? If it is not this window, the click would land somewhere else in the
    // user's desktop -- worth reporting rather than doing.
    POINT probe = {sx, sy};
    HWND under = WindowFromPoint(probe);
    if (under != g_found && !IsChild(g_found, under))
    {
      printf("  (%d,%d) -> %p is not the target window (%p); skipping\n", x, y, (void *)under,
             (void *)g_found);
      continue;
    }

    SetCursorPos(sx, sy);
    Sleep(150);
    for (int k = 0; k < times; ++k)
    {
      mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
      Sleep(30);
      mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
      if (k + 1 < times)
        Sleep(60); // inside the double-click interval, and short enough to stay inside it
    }
    printf("  %s client(%d,%d)\n", times == 2 ? "double-clicked" : "clicked", x, y);
    fflush(stdout);
    ++clicked;
    Sleep(700); // let the page redraw before the next click (and before the caller screenshots)
  }

  // The cursor goes back where it was found, whatever happened above.
  SetCursorPos(saved.x, saved.y);
  printf("clicked %d point(s); cursor restored to %ld,%ld\n", clicked, saved.x, saved.y);
  return clicked > 0 ? 0 : 1;
}
