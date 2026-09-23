// ⚠️⚠️ MANUAL-ONLY: this MOVES THE USER'S CURSOR (SetCursorPos before each burst) and drives real wheel
// events at the running app. Never run it from a gate, a build script, or any automatic flow -- the user's
// standing rule is that tests do not touch their input devices (test/check_apex_modular.sh section 9).
// It is kept because a leak that only appears under load is worth being able to reproduce by hand.
// LEAK TEST UNDER LOAD: drive real wheels at the running app and watch its counters across many bursts.
//
// An idle app holding its numbers steady (app_soak_probe) only proves nothing leaks BETWEEN wheels. A
// leak in the per-wheel path -- a handle opened per target lookup, a queued injection that is never
// removed, a GDI object per log line -- would only show up as a climb that stays after the wheels stop.
//
// So this does three things in order:
//   1. IDLE BASELINE, a few seconds with no input at all;
//   2. LOAD, N bursts of real wheel notches at a chosen point (the app swallows these and re-injects);
//   3. SETTLE, a few seconds after the last wheel, so anything held per-gesture has been released.
//
// The verdict is about the DIFFERENCE between 1 and 3: an allocation that is truly per-wheel cannot
// come back down, and that is what a leak looks like here.
//
// g++ -std=c++17 -O2 -mconsole _diag/app_load_soak.cpp -o build/loadsoak.exe -lpsapi
// usage: app_load_soak <pid> <x> <y> [bursts] [notchesPerBurst]
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

struct Sample
{
  double privateMB = 0.0;
  DWORD handles = 0, gdi = 0, user = 0, threads = 0;
  double cpuSec = 0.0;
};

static bool Take(DWORD pid, Sample *s)
{
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return false;
  PROCESS_MEMORY_COUNTERS_EX pmc;
  ZeroMemory(&pmc, sizeof(pmc));
  if (!GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)))
  {
    CloseHandle(h);
    return false;
  }
  s->privateMB = (double)pmc.PrivateUsage / (1024.0 * 1024.0);
  GetProcessHandleCount(h, &s->handles);
  FILETIME c, e, k, u;
  if (GetProcessTimes(h, &c, &e, &k, &u))
  {
    ULARGE_INTEGER kk, uu;
    kk.LowPart = k.dwLowDateTime;
    kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime;
    uu.HighPart = u.dwHighDateTime;
    s->cpuSec = (double)(kk.QuadPart + uu.QuadPart) / 1e7;
  }
  CloseHandle(h);

  HANDLE h2 = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (h2)
  {
    s->gdi = (DWORD)GetGuiResources(h2, GR_GDIOBJECTS);
    s->user = (DWORD)GetGuiResources(h2, GR_USEROBJECTS);
    CloseHandle(h2);
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap != INVALID_HANDLE_VALUE)
  {
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
      do
      {
        if (te.th32OwnerProcessID == pid)
          ++s->threads;
      } while (Thread32Next(snap, &te));
    CloseHandle(snap);
  }
  return true;
}

static void WheelAt(int x, int y, int delta)
{
  SetCursorPos(x, y);
  INPUT in = {0};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = MOUSEEVENTF_WHEEL;
  in.mi.mouseData = (DWORD)delta;
  SendInput(1, &in, sizeof(in));
}

int main(int argc, char **argv)
{
  const DWORD pid = (argc > 1) ? (DWORD)atoi(argv[1]) : 0;
  const int x = (argc > 2) ? atoi(argv[2]) : 0;
  const int y = (argc > 3) ? atoi(argv[3]) : 0;
  const int bursts = (argc > 4) ? atoi(argv[4]) : 40;
  const int per = (argc > 5) ? atoi(argv[5]) : 3;
  // How long between notches. The default is a human pace; a small value gives a SUSTAINED fast scroll,
  // whose CPU per wall second is the cost number worth reporting (a wheel costs the same whether or not
  // the next one is close behind, so the per-notch figure alone hides how much of the time it runs).
  const int notchMs = (argc > 6) ? atoi(argv[6]) : 90;
  if (!pid || !x || !y)
  {
    printf("usage: app_load_soak <pid> <x> <y> [bursts] [notchesPerBurst] [notchMs]\n");
    return 2;
  }

  FILE *out = fopen("app_load_soak.txt", "w");
  if (!out)
    return 1;
  fprintf(out, "pid %lu  target %d,%d  bursts %d x %d notches, %d ms apart\n\n", pid, x, y, bursts, per,
          notchMs);
  fprintf(out, "%8s %10s %8s %6s %6s %8s %9s\n", "phase", "priv(MB)", "handles", "gdi", "user",
          "threads", "cpu(s)");

  Sample base = {};
  { // 1. idle baseline, including a moment for the app to settle after the probe started
    Sleep(2500);
    Take(pid, &base);
    fprintf(out, "%8s %10.2f %8lu %6lu %6lu %8lu %9.2f\n", "idle", base.privateMB, base.handles,
            base.gdi, base.user, base.threads, base.cpuSec);
    fflush(out);
  }

  const int every = (bursts / 12 > 0) ? bursts / 12 : 1; // a readable number of rows for any size
  Sample loadStart = {};
  { // 2. the load: real notches, at the requested pace, with the app swallowing and re-injecting each
    Take(pid, &loadStart);
    const DWORD loadT0 = GetTickCount();
    for (int b = 0; b < bursts; ++b)
    {
      for (int i = 0; i < per; ++i)
      {
        WheelAt(x, y, (b % 2) ? WHEEL_DELTA : -WHEEL_DELTA);
        if (notchMs > 0)
          Sleep(notchMs);
      }
      Sleep(160); // let the glide drain: the point is to exercise the per-gesture path, not to stack it
      if ((b % every) == (every - 1))
      {
        Sample s;
        if (Take(pid, &s))
        {
          char lbl[32];
          _snprintf(lbl, sizeof(lbl), "load%d", b + 1);
          fprintf(out, "%8s %10.2f %8lu %6lu %6lu %8lu %9.2f\n", lbl, s.privateMB, s.handles, s.gdi,
                  s.user, s.threads, s.cpuSec);
          fflush(out);
        }
      }
    }
    Sample loadEnd;
    if (Take(pid, &loadEnd))
    {
      const double wall = (GetTickCount() - loadT0) / 1000.0;
      fprintf(out, "%8s %10.2f %8lu %6lu %6lu %8lu %9.2f\n", "loadend", loadEnd.privateMB, loadEnd.handles,
              loadEnd.gdi, loadEnd.user, loadEnd.threads, loadEnd.cpuSec);
      fprintf(out, "\nCOST WHILE SCROLLING: %.3f s of one core over %.1f s of wall clock = %.2f%% of one\n",
              loadEnd.cpuSec - loadStart.cpuSec, wall,
              100.0 * (loadEnd.cpuSec - loadStart.cpuSec) / (wall > 0 ? wall : 1));
      fprintf(out, "  core, at %d notches in that time (%.0f notches/s) -- the model is stepped at 250 Hz\n",
              bursts * per, (double)(bursts * per) / (wall > 0 ? wall : 1));
      fprintf(out, "  throughout, whether or not that particular notch needed it.\n");
    }
  }

  Sample after = {};
  { // 3. settle: nothing at all for a few seconds, so anything per-gesture is released
    Sleep(4000);
    Take(pid, &after);
    fprintf(out, "%8s %10.2f %8lu %6lu %6lu %8lu %9.2f\n", "settled", after.privateMB, after.handles,
            after.gdi, after.user, after.threads, after.cpuSec);
  }

  fprintf(out, "\nIDLE vs SETTLED (the leak verdict -- a per-wheel allocation cannot come back down):\n");
  fprintf(out, "  private bytes : %+8.2f MB\n", after.privateMB - base.privateMB);
  fprintf(out, "  handles       : %+8ld\n", (long)after.handles - (long)base.handles);
  fprintf(out, "  GDI objects   : %+8ld\n", (long)after.gdi - (long)base.gdi);
  fprintf(out, "  USER objects  : %+8ld\n", (long)after.user - (long)base.user);
  fprintf(out, "  threads       : %+8ld\n", (long)after.threads - (long)base.threads);

  const double wheels = (double)(bursts * per);
  fprintf(out, "\nTOTAL: %d wheels used %.3f s of one core (%.2f ms each, including the 250 Hz engine time\n",
          (int)wheels, after.cpuSec - base.cpuSec,
          (after.cpuSec - base.cpuSec) * 1000.0 / (wheels > 0 ? wheels : 1));
  fprintf(out, "       each one's glide spends), and the process settled at %.2f MB private.\n",
          after.privateMB);
  fclose(out);
  return 0;
}
