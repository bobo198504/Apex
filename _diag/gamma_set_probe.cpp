// ---------------------------------------------------------------------------
// gamma_set_probe -- WHICH GAMMA RAMPS WILL WINDOWS ACTUALLY ACCEPT ON THIS DISPLAY?
//
// WHY: MediaControl's own log says `SetDeviceGammaRamp REFUSED` on the second screen, while a debug probe
// writing "the value that is already there" was accepted. Those two cannot both be explained by "the adapter
// does not support gamma ramps", so something about the VALUE is being rejected -- and the API says nothing
// about why (a BOOL, no error code that means anything here).
//
// So this walks a ladder of ramps through one display DC and prints, for each: what was asked for, whether the
// call was accepted, GetLastError, and what the ramp reads back as. The ladder matters: identity (the value a
// driver starts at), a mild dim, a strong dim, the exact shape MediaControl computes, and -- the important
// control -- the same value written TWICE, because "accepted once, refused the second time" would be a
// completely different story from "this value is refused".
//
// ⚠️ IT PUTS THE RAMP BACK AT THE END, and every state it passes through is a plain darkened screen (no
// inversion, no colour shift). It never writes a ramp it has not first read.
//
// Build: g++ -std=c++17 -O2 -mconsole _diag/gamma_set_probe.cpp -o build/gamma_set_probe.exe -luser32 -lgdi32
// Run:   build/gamma_set_probe.exe [device]     (default \\.\DISPLAY1)
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void Scale(const WORD base[3][256], double k, WORD out[3][256])
{
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
      out[c][i] = (WORD)(base[c][i] * k + 0.5);
}

static void Report(const char *what, BOOL ok, const WORD ramp[3][256], HDC dc)
{
  char back[64] = {0};
  WORD rb[3][256];
  if (GetDeviceGammaRamp(dc, rb))
    _snprintf(back, sizeof(back), "read-back white=%u", rb[0][255]);
  else
    _snprintf(back, sizeof(back), "read-back FAILED");
  printf("  %-34s %-8s err=%-6lu asked white=%-6u  %s%s\n", what, ok ? "ACCEPTED" : "REFUSED",
         (unsigned long)(ok ? 0 : GetLastError()), ramp[0][255], back,
         (!ok && ramp[0][255] == rb[0][255]) ? "   (the screen never changed)" : "");
}

int main(int argc, char **argv)
{
  SetConsoleOutputCP(CP_UTF8);
  const char *device = argc > 1 ? argv[1] : "\\\\.\\DISPLAY1";
  printf("gamma writes on %s -- read first, written back at the end\n\n", device);

  HDC dc = CreateDCA("DISPLAY", device, nullptr, nullptr);
  if (!dc)
  {
    printf("could not open a DC for %s\n", device);
    return 2;
  }
  WORD original[3][256];
  if (!GetDeviceGammaRamp(dc, original))
  {
    printf("GetDeviceGammaRamp FAILED -- nothing to test\n");
    DeleteDC(dc);
    return 2;
  }
  printf("  original white = %u\n\n", original[0][255]);

  // The value that is already there (what the older probe did, and it was accepted).
  Report("write back the current ramp", SetDeviceGammaRamp(dc, original), original, dc);

  struct Step
  {
    const char *what;
    double k;
  };
  const Step steps[] = {
      {"90% (a mild dim)", 0.90},
      {"80% (what the probe wrote)", 0.80},
      {"60%", 0.60},
      {"48% (0.8 x 0.6)", 0.48},
      {"50%", 0.50},
      {"10% (very dark)", 0.10},
  };
  WORD ramp[3][256];
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i)
  {
    Scale(original, steps[i].k, ramp);
    Report(steps[i].what, SetDeviceGammaRamp(dc, ramp), ramp, dc);
    // Between steps, come back to the original so each step is measured from the same place (and so a refusal
    // cannot be blamed on the previous value).
    SetDeviceGammaRamp(dc, original);
  }

  // Two writes of the SAME value in a row: if the second is refused, the rule is about transitions rather than
  // about values.
  Scale(original, 0.80, ramp);
  Report("80%, first write", SetDeviceGammaRamp(dc, ramp), ramp, dc);
  Report("80%, second write (same value)", SetDeviceGammaRamp(dc, ramp), ramp, dc);

  // And a non-uniform ramp: the SAME white point as an accepted one, but with a curved shape. If this is
  // refused while the linear one was accepted, the constraint is on the shape (monotonicity / step size).
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
    {
      const double x = (double)i / 255.0;
      ramp[c][i] = (WORD)(65535.0 * x * x * 0.8 + 0.5);
    }
  Report("80% white, curved (x^2)", SetDeviceGammaRamp(dc, ramp), ramp, dc);

  printf("\nrestoring the original ramp\n");
  printf("  %s\n", SetDeviceGammaRamp(dc, original) ? "restored" : "COULD NOT RESTORE (!)");
  DeleteDC(dc);
  return 0;
}
