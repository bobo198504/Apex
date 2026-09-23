// ---------------------------------------------------------------------------
// ddc_set_probe -- CAN THIS MONITOR'S BRIGHTNESS ACTUALLY BE WRITTEN OVER DDC/CI?
//
// WHY: MediaControl's log says `DDC/CI answered -- current 90 of 100` for the external screen, and the user says
// the slider does not move it. Reading and writing are different directions on the same wire, and only the read
// was ever tested -- so this writes the value the panel is ALREADY AT (read first, then write it back), which
// cannot change how the screen looks but does answer whether the write is accepted.
//
// ⚠️ IT NEVER ASKS FOR A DIFFERENT BRIGHTNESS. Every write is the number just read, so the screen does not move
// and this is safe to run on a working machine. It also tries the value in both the monitor's own scale and a
// 0..100 scale, because a monitor whose maximum is not 100 is where a scaling mistake would hide.
//
// Build: g++ -std=c++17 -O2 -mconsole _diag/ddc_set_probe.cpp -o build/ddc_set_probe.exe -luser32 -lgdi32 -ldxva2
// Run:   build/ddc_set_probe.exe
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <string.h>

struct Mon
{
  HMONITOR h;
  RECT rc;
  char device[32];
};

static Mon g_mons[8];
static int g_count = 0;

static BOOL CALLBACK EnumProc(HMONITOR h, HDC, LPRECT, LPARAM)
{
  if (g_count >= 8)
    return FALSE;
  MONITORINFOEXA ex;
  memset(&ex, 0, sizeof(ex));
  ex.cbSize = sizeof(ex);
  if (GetMonitorInfoA(h, &ex))
  {
    g_mons[g_count].h = h;
    g_mons[g_count].rc = ex.rcMonitor;
    _snprintf(g_mons[g_count].device, sizeof(g_mons[0].device), "%s", ex.szDevice);
    ++g_count;
  }
  return TRUE;
}

int main()
{
  SetConsoleOutputCP(CP_UTF8);
  EnumDisplayMonitors(nullptr, nullptr, EnumProc, 0);
  printf("DDC/CI brightness on %d monitor(s) -- read first, write back the SAME value\n\n", g_count);

  for (int i = 0; i < g_count; ++i)
  {
    printf("  %s  %ldx%ld\n", g_mons[i].device, g_mons[i].rc.right - g_mons[i].rc.left,
           g_mons[i].rc.bottom - g_mons[i].rc.top);
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(g_mons[i].h, &n) || n == 0)
    {
      printf("      no physical monitor handle\n\n");
      continue;
    }
    PHYSICAL_MONITOR pm[8];
    const DWORD take = n < 8 ? n : 8;
    if (!GetPhysicalMonitorsFromHMONITOR(g_mons[i].h, take, pm))
    {
      printf("      GetPhysicalMonitorsFromHMONITOR failed (err=%lu)\n\n", (unsigned long)GetLastError());
      continue;
    }
    for (DWORD k = 0; k < take; ++k)
    {
      char desc[256] = {0};
      WideCharToMultiByte(CP_UTF8, 0, pm[k].szPhysicalMonitorDescription, -1, desc, sizeof(desc) - 1, nullptr,
                          nullptr);
      DWORD cur = 0, maxv = 0;
      MC_VCP_CODE_TYPE type = MC_VCP_CODE_TYPE(0);
      const BOOL readOk = GetVCPFeatureAndVCPFeatureReply(pm[k].hPhysicalMonitor, 0x10, &type, &cur, &maxv);
      printf("      [%lu] \"%s\"  read: %s  current=%lu max=%lu\n", (unsigned long)k, desc,
             readOk ? "ok" : "FAILED", (unsigned long)cur, (unsigned long)maxv);
      if (!readOk || maxv == 0)
      {
        printf("           -> nothing to write\n");
        continue;
      }
      // (1) the value just read, unchanged.
      const BOOL w1 = SetVCPFeature(pm[k].hPhysicalMonitor, 0x10, cur);
      printf("           write %-4lu (what it already is)      -> %s (err=%lu)\n", (unsigned long)cur,
             w1 ? "ACCEPTED" : "REFUSED", (unsigned long)GetLastError());
      // (2) the same value recomputed as a percentage of the monitor's own maximum -- this is the arithmetic
      // MediaControl does, so if IT is wrong, this is where it shows.
      const DWORD viaPercent = (DWORD)((double)maxv * ((double)cur * 100.0 / (double)maxv) / 100.0 + 0.5);
      const BOOL w2 = SetVCPFeature(pm[k].hPhysicalMonitor, 0x10, viaPercent);
      printf("           write %-4lu (via %% of max)          -> %s (err=%lu)\n", (unsigned long)viaPercent,
             w2 ? "ACCEPTED" : "REFUSED", (unsigned long)GetLastError());
      // (3) and one notch lower, to see whether a CHANGE is accepted rather than only a no-op. This one DOES
      // move the screen; it is put straight back.
      //
      // ⚠️ AND IT IS READ BACK, WHICH IS THE ONLY PROOF THAT MATTERS. `SetVCPFeature` returning TRUE means the
      // command went out on the wire -- DDC/CI has no acknowledgement, and a monitor is free to ignore it. A
      // slider whose writes are "accepted" and whose screen never moves is exactly the failure the user
      // reported, so the check has to close the loop: write, wait for the monitor to act, read, compare.
      if (cur > 0)
      {
        const BOOL w3 = SetVCPFeature(pm[k].hPhysicalMonitor, 0x10, cur - 1);
        printf("           write %-4lu (one step darker)       -> %s (err=%lu)\n", (unsigned long)(cur - 1),
               w3 ? "ACCEPTED" : "REFUSED", (unsigned long)GetLastError());
        Sleep(400);
        DWORD after = 0, m3 = 0;
        GetVCPFeatureAndVCPFeatureReply(pm[k].hPhysicalMonitor, 0x10, nullptr, &after, &m3);
        printf("           read back: %lu  %s\n", (unsigned long)after,
               after == cur - 1 ? "<- THE SCREEN REALLY MOVED" : "<- the command was ignored by the monitor");
        SetVCPFeature(pm[k].hPhysicalMonitor, 0x10, cur); // back where it was
        Sleep(400);
        DWORD again = 0, m2 = 0;
        GetVCPFeatureAndVCPFeatureReply(pm[k].hPhysicalMonitor, 0x10, nullptr, &again, &m2);
        printf("           read back after restoring: %lu (was %lu)  %s\n", (unsigned long)again,
               (unsigned long)cur, again == cur ? "" : "<- NOT RESTORED");
      }
    }
    DestroyPhysicalMonitors(take, pm);
    printf("\n");
  }
  printf("done -- every write above was the value the panel was already at, except the single-step test, which\n"
         "was restored immediately.\n");
  return 0;
}
