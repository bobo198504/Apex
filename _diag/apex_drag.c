// apex_drag -- press at one client point, move to another, release. For testing a slider.
//
// apex_click only presses and releases in place, which is enough for a button and useless for a slider: a
// range input reports `input` events while the mouse is HELD DOWN and moving, so the drag has to be a real
// press-move-release with the button still held. That is what this does.
//
// ⚠️⚠️ MANUAL-ONLY. NEVER RUN THIS FROM A GATE, A BUILD SCRIPT, OR ANY AUTOMATIC FLOW.
//
// It takes over the user's pointer for the duration of the drag -- worse than a click, because a held button
// means anything the user does meanwhile lands on whatever this is dragging. The user's standing rule is that
// UI interaction testing belongs to THEM: "一切UI交互测试都由我来做，我会反馈给你."
//
// ⚠️ THE MECHANICAL CHECK LIVES IN test/check_apex_modular.sh (section 8): a file that moves the cursor or
// clicks must carry this MANUAL-ONLY marker, and no script named in run_all.sh may do either.
//
// Restoring the cursor afterwards is better than not restoring, but it is not the standard -- see the longer
// note in apex_click.c. The tool is kept for the user, who owns UI testing, to run deliberately.
//
// Build: gcc -O2 -o build/apex_drag.exe _diag/apex_drag.c -luser32
// Run:   apex_drag.exe <exe-name> <class-name> <fromX,fromY> <toX,toY> [steps]
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
  if (argc < 5)
  {
    printf("usage: apex_drag.exe <exe-name> <class-name> <fromX,fromY> <toX,toY> [steps]\n");
    return 2;
  }
  g_exe = argv[1];
  g_cls = argv[2];
  int fx = 0, fy = 0, tx = 0, ty = 0;
  if (sscanf(argv[3], "%d,%d", &fx, &fy) != 2 || sscanf(argv[4], "%d,%d", &tx, &ty) != 2)
  {
    printf("bad coordinates\n");
    return 2;
  }
  const int steps = (argc >= 6) ? atoi(argv[5]) : 12;

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

  SetCursorPos(origin.x + fx, origin.y + fy);
  Sleep(200);
  mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);

  // SLOWLY, with the button down: a range input reports a value for each of these, which is the whole point
  // (and a single jump would not prove the drag path works).
  for (int i = 1; i <= steps; ++i)
  {
    const int x = fx + (tx - fx) * i / steps;
    const int y = fy + (ty - fy) * i / steps;
    SetCursorPos(origin.x + x, origin.y + y);
    Sleep(60);
  }
  Sleep(150);
  mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
  Sleep(300);

  SetCursorPos(saved.x, saved.y);
  printf("dragged client(%d,%d) -> (%d,%d) in %d steps; cursor restored to %ld,%ld\n", fx, fy, tx, ty, steps,
         saved.x, saved.y);
  return 0;
}
