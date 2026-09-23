// LEAK AND COST CHECK for the standalone app, sampled from outside over time.
//
// Run it beside a RUNNING copy of the app. What matters is not any absolute number (a process holds
// whatever it needs) but whether a counter GROWS while nothing is happening -- that is what a leak
// looks like.
//
// Sampled once a second: private bytes (the process's own memory), kernel handles, GDI objects (the
// classic Windows leak: a brush or icon created per event), USER objects (windows/menus), thread count
// (the app runs three: main, engine, injection -- a leak would show one per roll), and CPU seconds
// (whose slope is the app's idle cost, since the engine ticks at 250 Hz whether or not a wheel comes).
//
// g++ -std=c++17 -O2 -mwindows _diag/app_soak_probe.cpp -o /tmp/asoak -lpsapi
// usage: app_soak_probe <pid> [seconds]      (writes app_soak.txt)
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
  bool ok = false;
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

  // GDI/USER counts need a stronger handle; report 0 when it cannot be opened rather than failing.
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
  s->ok = true;
  return true;
}

int main(int argc, char **argv)
{
  const DWORD pid = (argc > 1) ? (DWORD)atoi(argv[1]) : 0;
  const int seconds = (argc > 2) ? atoi(argv[2]) : 30;
  if (!pid)
  {
    printf("usage: app_soak_probe <pid> [seconds]\n");
    return 2;
  }

  FILE *out = fopen("app_soak.txt", "w");
  if (!out)
    return 1;
  fprintf(out, "sampling pid %lu for %d s\n\n", pid, seconds);
  fprintf(out, "%5s %10s %8s %6s %6s %8s %9s\n", "sec", "priv(MB)", "handles", "gdi", "user",
          "threads", "cpu(s)");

  Sample first, last;
  bool haveFirst = false;
  int i = 0;
  for (; i <= seconds; ++i)
  {
    Sample s;
    if (!Take(pid, &s) || !s.ok)
    {
      fprintf(out, "process %lu is gone after %d s\n", pid, i);
      break;
    }
    fprintf(out, "%5d %10.2f %8lu %6lu %6lu %8lu %9.2f\n", i, s.privateMB, s.handles, s.gdi, s.user,
            s.threads, s.cpuSec);
    fflush(out);
    if (!haveFirst)
    {
      first = s;
      haveFirst = true;
    }
    last = s;
    if (i < seconds)
      Sleep(1000);
  }

  const int elapsed = (i > 0) ? (i - 1) : 0;
  fprintf(out, "\nTREND over %d s:\n", elapsed);
  fprintf(out, "  private bytes : %+8.2f MB\n", last.privateMB - first.privateMB);
  fprintf(out, "  handles       : %+8ld\n", (long)last.handles - (long)first.handles);
  fprintf(out, "  GDI objects   : %+8ld\n", (long)last.gdi - (long)first.gdi);
  fprintf(out, "  USER objects  : %+8ld\n", (long)last.user - (long)first.user);
  fprintf(out, "  threads       : %+8ld\n", (long)last.threads - (long)first.threads);
  fprintf(out, "  CPU used      : %8.3f s  (%.3f%% of one core while idle)\n",
          last.cpuSec - first.cpuSec,
          (elapsed > 0) ? 100.0 * (last.cpuSec - first.cpuSec) / (double)elapsed : 0.0);
  fprintf(out, "\na leak shows as a counter that climbs and does not come back. The CPU figure is the\n");
  fprintf(out, "engine ticking at 250 Hz whether or not a wheel arrives, plus the hook.\n");
  fclose(out);
  return 0;
}
