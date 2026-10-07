// TEMPORARY diagnostic (not a project gate): does the host's order probe actually READ a start time for the
// engines that are running? `ActiveEngines` compares two of these values, and a 0 (unknown) sorts LAST -- so a
// process whose time cannot be read is silently demoted to the end of the sentence, which is what a wrong order
// looks like from the outside (and the platform layer has no logger to say so).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static unsigned long long ProcessStartMs(unsigned long pid)
{
  if (!pid)
    return 0;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (!h)
  {
    printf("      OpenProcess(%lu) FAILED, error %lu\n", pid, GetLastError());
    return 0;
  }
  FILETIME create, exitT, kernel, user;
  unsigned long long ms = 0;
  if (GetProcessTimes(h, &create, &exitT, &kernel, &user))
  {
    ULARGE_INTEGER u;
    u.LowPart = create.dwLowDateTime;
    u.HighPart = create.dwHighDateTime;
    ms = (u.QuadPart - 116444736000000000ull) / 10000ull;
  }
  else
    printf("      GetProcessTimes(%lu) FAILED, error %lu\n", pid, GetLastError());
  CloseHandle(h);
  return ms;
}

int main()
{
  printf("processes named Lertaro.App.exe / reaper.exe, and whether a start time comes back:\n");
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
  {
    printf("  CreateToolhelp32Snapshot FAILED, error %lu\n", GetLastError());
    return 1;
  }
  int found = 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe))
  {
    do
    {
      if (_wcsicmp(pe.szExeFile, L"Lertaro.App.exe") == 0 || _wcsicmp(pe.szExeFile, L"reaper.exe") == 0)
      {
        const unsigned long long ms = ProcessStartMs(pe.th32ProcessID);
        printf("  %-20ls pid %-6lu start=%llu ms (%s)\n", pe.szExeFile, pe.th32ProcessID, ms,
               ms ? "read" : "UNKNOWN");
        ++found;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (!found)
    printf("  (neither is running)\n");
  return 0;
}
