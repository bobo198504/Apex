// A STAND-IN FOR LERTARO'S SMOOTH-SCROLLING MARKER, so the engine gate can prove the Lertaro half of the note.
//
// WHAT IT IMITATES. Lertaro ports this project's scroll model and publishes a named event while its smoothing is
// on: `Local\Lertaro.SmoothScroll.Active`, session-scoped, present exactly while the behaviour is enabled (see
// Lertaro's App/Helpers/Visuals/SmoothWheelScrollBehavior.cs and its CHANGELOG). That event -- not the process --
// is what Apex probes, so this program only has to hold it open.
//
// ⚠️ IT IS ALSO THE HALF THAT MAKES THE ORDER TESTABLE. Apex orders the engines by their PROCESS start times
// ("谁先运行谁显式在前面"), and it looks for the marker's owner under the name `Lertaro.App.exe`. So the gate
// copies this binary to THAT name before running it, and two stand-ins started a second apart then have the
// start times the order is read from.
//
// usage: lertaro_marker_fake.exe [seconds]   (default 60; prints one line, then holds the event)
//        lertaro_marker_fake.exe --check     print "present"/"absent" and exit 0/1 -- DOES NOT CREATE IT
//
// ⚠️ `--check` IS FOR THE GATES, AND IT IS NOT A CONVENIENCE. A real Lertaro running on this machine holds the
// same event, and a gate that could not tell that from its own stand-in would measure the user's program while
// claiming to measure its fixture -- every order assertion would then be about whatever the user happened to have
// open. With this mode a gate can say so out loud and stand down.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kMarker = "Local\\Lertaro.SmoothScroll.Active";

int main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "--check") == 0)
  {
    HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, kMarker);
    if (ev)
    {
      CloseHandle(ev);
      printf("present\n");
      return 0;
    }
    printf("absent\n");
    return 1;
  }

  const int seconds = (argc > 1) ? atoi(argv[1]) : 60;
  // Manual-reset and initially non-signalled: the marker says "this exists", nothing ever waits on its state --
  // exactly how Lertaro creates it.
  HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, kMarker);
  if (!ev)
  {
    printf("could not create the marker (error %lu)\n", GetLastError());
    return 1;
  }
  printf("marker held for %d s (pid %lu)\n", seconds, GetCurrentProcessId());
  fflush(stdout);
  Sleep((DWORD)(seconds > 0 ? seconds : 60) * 1000u);
  CloseHandle(ev);
  return 0;
}
