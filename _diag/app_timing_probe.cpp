// How expensive is "which program is under the cursor, and is another handler on it?"
//
// The app asks this on every wheel message, and the answer decides whether the app smooths or passes.
// Two of the three lookups are CROSS-PROCESS, which is the thing to measure: if any of them costs
// milliseconds, asking it inside the low-level input hook stalls the very path the app depends on --
// the hook callback has a budget, and the timer that does the smoothing shares the message loop.
//
// Measured here on a live window, so the numbers are real rather than a guess about API cost.
//
// g++ -std=c++17 -O2 -mwindows _diag/app_timing_probe.cpp -o /tmp/atp -luser32
// (prints to a file beside the exe, since -mwindows has no console)
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

static double Now()
{
  static LARGE_INTEGER f = {0};
  if (f.QuadPart == 0)
    QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
}

int main()
{
  FILE *out = fopen("app_timing_probe.txt", "w");
  if (!out)
    return 1;

  POINT pt;
  GetCursorPos(&pt);

  HWND w = WindowFromPoint(pt);
  DWORD pid = 0;
  if (w)
    GetWindowThreadProcessId(w, &pid);

  fprintf(out, "probe point %ld,%ld  window %p  pid %lu\n\n", pt.x, pt.y, (void *)w, pid);

  // ---- 1. the cheap part: which window, and which process owns it ----
  {
    const int N = 2000;
    const double t0 = Now();
    volatile DWORD sink = 0;
    for (int i = 0; i < N; ++i)
    {
      HWND h = WindowFromPoint(pt);
      DWORD p = 0;
      if (h)
        GetWindowThreadProcessId(h, &p);
      sink += p;
    }
    const double us = (Now() - t0) * 1e6 / N;
    fprintf(out, "WindowFromPoint + GetWindowThreadProcessId : %8.2f us/call\n", us);
  }

  // ---- 2. + the exe name (needs opening the process) ----
  if (pid)
  {
    const int N = 200;
    const double t0 = Now();
    int ok = 0;
    for (int i = 0; i < N; ++i)
    {
      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (h)
      {
        char full[MAX_PATH] = {0};
        DWORD n = (DWORD)sizeof(full);
        if (QueryFullProcessImageNameA(h, 0, full, &n))
          ++ok;
        CloseHandle(h);
      }
    }
    const double us = (Now() - t0) * 1e6 / N;
    fprintf(out, "  + OpenProcess + QueryFullProcessImageName : %8.2f us/call  (%d/%d ok)\n", us, ok,
            N);
  }

  // ---- 3. + the module list (the expensive one, if the guess is right) ----
  if (pid)
  {
    const int N = 100;
    const double t0 = Now();
    int modules = 0;
    for (int i = 0; i < N; ++i)
    {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
      if (snap == INVALID_HANDLE_VALUE)
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE32, pid);
      if (snap != INVALID_HANDLE_VALUE)
      {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
          int c = 0;
          do
            ++c;
          while (Module32NextW(snap, &me));
          modules = c;
        }
        CloseHandle(snap);
      }
    }
    const double us = (Now() - t0) * 1e6 / N;
    fprintf(out, "  + CreateToolhelp32Snapshot + modules : %8.2f us/call  (%d modules seen)\n", us,
            modules);
  }

  fprintf(out, "\nrule of thumb from the plugin's own notes: a low-level hook may only record and\n");
  fprintf(out, "return -- work of the order printed above does not belong in that callback.\n");
  fclose(out);
  return 0;
}
