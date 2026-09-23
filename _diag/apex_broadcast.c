// ---------------------------------------------------------------------------
// apex_broadcast -- tell every window that a system setting changed, the way Windows itself does.
//
// WHY THIS IS NEEDED. Changing the light/dark theme from the Settings app makes Windows broadcast
// WM_SETTINGCHANGE with lParam "ImmersiveColorSet"; every top-level window hears it and re-reads what it
// cares about (Apex's host re-picks its tray mark, the panel re-reads its caption and its icon). Writing the
// registry value directly changes the STORED setting and broadcasts NOTHING -- so a test that only writes
// the key is testing half of the mechanism, and the half it skips is the one that turns a stored value into
// a visible change. That is exactly the path the user reported as broken, so it has to be driven for real.
//
// Build: gcc -O2 -o build/apex_broadcast.exe _diag/apex_broadcast.c -luser32
// Run:   apex_broadcast.exe [lParam]      (default "ImmersiveColorSet")
//
// Exit: 0 if the broadcast was dispatched, 1 if it timed out (some window was hung -- which is not this
// program's business to fix, but is worth reporting rather than swallowing).
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
  const wchar_t *what = L"ImmersiveColorSet";
  wchar_t wide[128] = {0};
  if (argc >= 2)
  {
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wide, 127);
    what = wide;
  }

  DWORD_PTR result = 0;
  SetLastError(0);
  const LRESULT ok = SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)what,
                                         SMTO_ABORTIFHUNG, 5000, &result);
  if (!ok)
  {
    printf("the broadcast did not complete (error %lu)\n", GetLastError());
    return 1;
  }
  printf("WM_SETTINGCHANGE lParam=\"%ls\" broadcast to every top-level window\n", what);
  return 0;
}
