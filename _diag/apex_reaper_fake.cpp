// A FAKE "REAPER WITH THE PLUGIN RUNNING", so the gate can prove the note APPEARS.
//
// The interesting half of the REAPER note is the positive case, and it is the half that cannot be tested by
// looking at a machine where it is false: "the note is absent" is also what a broken detector prints.
//
// So this program does exactly two things, and both are what the host's check looks for:
//
//   1. IT IS NAMED reaper.exe. The host asks by process name (Process32FirstW, _wcsicmp "reaper.exe"); the
//      gate copies this executable to that name before running it. Nothing is spoofed beyond that -- it is a
//      real process with that real name.
//   2. IT LOADS A MODULE WHOSE NAME CONTAINS reaper_smoothwheelscroll. The host's second question is whether
//      that module is in the process's module list, so this loads a DLL with that name.
//   3. IT HAS A WINDOW OF CLASS "REAPERwnd". ⚠️ THIS ONE IS NOT OPTIONAL, and leaving it out is what broke the
//      first version of this stand-in: the host's FAST path identifies REAPER by that window (0.5 microseconds
//      against 3.59 ms for walking every process -- both measured), so a stand-in with no window is invisible
//      to it and the note stayed absent while the stand-in was running. Real REAPER always has this window, so
//      a stand-in without one was not standing in for anything.
//
// ⚠️ WHAT IT IS NOT: it is not a copy of the plugin and it does not smooth anything. It is a STAND-IN for
// "something that satisfies both halves of the check", which is what makes the gate able to tell a working
// detector from a detector that always says no.
//
// It sits idle until killed. The gate kills it by PID.
#include <windows.h>
#include <stdio.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
  // The marker module, built by the gate beside this exe. Its name is what the host scans for, so it must
  // contain that string -- LoadLibrary needs the real file name, and the gate names it accordingly.
  HMODULE m = LoadLibraryA("reaper_smoothwheelscroll_fake.dll");
  if (!m)
  {
    // Still run: the process name alone is a weaker case, and the gate reports which half was satisfied.
    OutputDebugStringA("fake reaper: the marker module did not load\n");
  }

  // THE WINDOW THAT MAKES IT LOOK LIKE REAPER -- see point 3 at the top of the file. It is created hidden: it
  // exists to be FOUND (the host looks it up by class), not to be seen, and a visible window would be a
  // rectangle popping up on the user's screen during a gate, which this project does not do.
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "REAPERwnd"; // ⚠️ the literal class the host looks for; REAPER's own name for it
  if (!RegisterClassA(&wc))
  {
    // A second copy in the same session cannot register the class again, which is fine -- it only has to exist.
    OutputDebugStringA("fake reaper: the window class was already registered\n");
  }
  // NOT HWND_MESSAGE: the host finds it with FindWindowA, which does not see message-only windows. It has to
  // be an ordinary (hidden) window for the lookup to work.
  HWND w = CreateWindowExA(0, "REAPERwnd", "REAPER", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, nullptr, nullptr,
                           GetModuleHandleA(nullptr), nullptr);
  if (!w)
    OutputDebugStringA("fake reaper: the window could not be created\n");

  // Idle until killed. A message loop rather than Sleep so it behaves like a real app (and so a graceful
  // WM_CLOSE would work if anyone sent one).
  MSG msg;
  while (GetMessageA(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return 0;
}
