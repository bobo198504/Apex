// ---------------------------------------------------------------------------
// gamma_probe -- WHAT IS ACTUALLY IN EACH MONITOR'S GAMMA RAMP RIGHT NOW, AND DOES IT MOVE ON ITS OWN?
//
// WHY: "my brightness slider and the panel's own driver both darken the screen, and together they look
// ADDITIVE" is a claim about a piece of state that only one program can win at a time. `SetDeviceGammaRamp`
// writes ONE 256-entry ramp per channel per display; there is no second layer to add. Two tools therefore
// OVERWRITE each other -- and what looks additive is usually a BASELINE problem: a tool that remembers "the
// ramp as it was when I started" as 100% will, if something else had already dimmed the screen, deliver a
// percentage OF that dimmed state.
//
// So this prints, for every monitor:
//   * the ramp's own shape (a few samples, and where its maximum sits relative to full scale),
//   * whether it is the identity ramp (n * 257) or something scaled -- i.e. whether ANY tool has touched it,
//   * and the same numbers again a second later, so a ramp that is being rewritten on a timer shows up.
//
// ⚠️ IT ONLY READS. `GetDeviceGammaRamp` changes nothing, and nothing here calls the setter.
//
// Build: g++ -std=c++17 -O2 -mconsole _diag/gamma_probe.cpp -o build/gamma_probe.exe -luser32 -lgdi32
// Run:   build/gamma_probe.exe
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void Sample(const char *device)
{
  HDC dc = CreateDCA("DISPLAY", device, nullptr, nullptr);
  if (!dc)
  {
    printf("  %-14s could not open a DC\n", device);
    return;
  }
  WORD ramp[3][256];
  memset(ramp, 0, sizeof(ramp));
  if (!GetDeviceGammaRamp(dc, ramp))
  {
    printf("  %-14s GetDeviceGammaRamp FAILED\n", device);
    DeleteDC(dc);
    return;
  }
  // The identity ramp is what an untouched display driver reports: entry i maps to i/255 of full scale, i.e.
  // i * 257 on a 16-bit channel.
  bool identity = true;
  for (int i = 0; i < 256; ++i)
    if (ramp[0][i] != (WORD)(i * 257))
      identity = false;
  // The same for the three channels being in step (a colour cast means a colour tool, not a brightness one).
  bool grey = true;
  for (int i = 0; i < 256; ++i)
    if (ramp[0][i] != ramp[1][i] || ramp[0][i] != ramp[2][i])
      grey = false;
  printf("  %-14s R: 0=%u 64=%u 128=%u 192=%u 255=%u   %s%s\n", device, ramp[0][0], ramp[0][64], ramp[0][128],
         ramp[0][192], ramp[0][255], identity ? "IDENTITY (nobody has touched it)" : "SCALED (something wrote it)",
         grey ? "" : "  [!] channels differ -- a colour tool is involved");
  if (!identity && ramp[0][255] > 0)
    printf("                 -> its white point is at %.1f%% of full scale\n",
           100.0 * ramp[0][255] / 65535.0);
  DeleteDC(dc);
}

struct Mon
{
  char device[32];
};
static int g_count = 0;
static Mon g_mons[8];

static BOOL CALLBACK EnumProc(HMONITOR h, HDC, LPRECT, LPARAM)
{
  MONITORINFOEXA ex;
  memset(&ex, 0, sizeof(ex));
  ex.cbSize = sizeof(ex);
  if (GetMonitorInfoA(h, &ex) && g_count < 8)
    _snprintf(g_mons[g_count++].device, sizeof(g_mons[0].device), "%s", ex.szDevice);
  return TRUE;
}

int main()
{
  SetConsoleOutputCP(CP_UTF8);
  EnumDisplayMonitors(nullptr, nullptr, EnumProc, 0);
  printf("gamma ramps -- read-only, twice, one second apart\n\n");
  printf("FIRST READ\n");
  for (int i = 0; i < g_count; ++i)
    Sample(g_mons[i].device);
  Sleep(1000);
  printf("\nSECOND READ (a ramp that changed here is being rewritten by something)\n");
  for (int i = 0; i < g_count; ++i)
    Sample(g_mons[i].device);
  printf("\ndone.\n");
  return 0;
}
