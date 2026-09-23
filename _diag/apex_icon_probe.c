// ---------------------------------------------------------------------------
// apex_icon_probe -- what the icon resources in an exe ACTUALLY are, in numbers.
//
// WHY THIS EXISTS. The tray icon did not follow the system theme, and every layer reported success:
//
//   * apex.rc wrote `IDI_APEX_LIGHT ICON "..."` -- a resource NAME. The numbers the code asked for
//     (`MAKEINTRESOURCE(2)` on a light system) had never been registered, because the resource compiler
//     never sees a C++ `#define`. LoadImage returned NULL and the shell kept the mark it had.
//   * The host's log line printed the mark that was CHOSEN, not the one that LOADED, so it read as proof
//     that the switch had happened.
//   * No gate looked at the resources at all. test/check_apex_language.sh asserted the language rules, and
//     its header claimed the icon variant was checked too -- it was not.
//
// So the check is: load the two ids THE WAY THE PRODUCT DOES (LoadImage with MAKEINTRESOURCE), convert the
// result to pixels and MEASURE. A mark that cannot be loaded is a failure; two marks that are not clearly
// distinguishable from each other are also a failure, because "the icon follows the theme" is meaningless
// if both ids hold the same picture.
//
// It is also a diagnostic: run it on any exe to see its icon table.
//
// Build:  gcc -O2 -o build/apex_icon_probe.exe _diag/apex_icon_probe.c -luser32 -lgdi32
// Run:    build/apex_icon_probe.exe <exe> [id ...]
//
// Exit codes (so it can be used as a gate): 0 = every id loaded and the two differ; 1 = a failed check;
// 2 = usage / cannot open the file.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

// The mean luma of an icon's own pixels, weighted by alpha so the transparent margin does not drag the
// average toward black. Returns -1 when the icon could not be loaded or has no pixels at all.
static double LumaOfIcon(HINSTANCE inst, int id, int cx, int cy, int *outR, int *outG, int *outB,
                         int *outOpaquePct)
{
  HICON icon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(id), IMAGE_ICON, cx, cy, 0);
  if (!icon)
    return -1;

  ICONINFO ii = {0};
  if (!GetIconInfo(icon, &ii))
  {
    DestroyIcon(icon);
    return -1;
  }

  BITMAP bm = {0};
  if (!GetObject(ii.hbmColor, sizeof(bm), &bm))
  {
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    return -1;
  }

  const int w = bm.bmWidth, h = bm.bmHeight;
  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; // negative: top-down rows
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  unsigned char *px = (unsigned char *)calloc((size_t)w * h * 4, 1);
  if (!px)
  {
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    return -1;
  }
  HDC dc = GetDC(NULL);
  const int got = GetDIBits(dc, ii.hbmColor, 0, h, px, &bi, DIB_RGB_COLORS);
  ReleaseDC(NULL, dc);

  double sumR = 0, sumG = 0, sumB = 0, sumW = 0;
  int opaque = 0;
  if (got)
  {
    for (int i = 0; i < w * h; ++i)
    {
      const double a = px[i * 4 + 3] / 255.0;
      sumR += px[i * 4 + 2] * a; // BGRA in memory
      sumG += px[i * 4 + 1] * a;
      sumB += px[i * 4 + 0] * a;
      sumW += a;
      if (a > 0.5)
        opaque++;
    }
  }
  free(px);
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  DestroyIcon(icon);

  if (!got || sumW <= 0)
    return -1;

  const double r = sumR / sumW, g = sumG / sumW, b = sumB / sumW;
  *outR = (int)(r + 0.5);
  *outG = (int)(g + 0.5);
  *outB = (int)(b + 0.5);
  *outOpaquePct = (int)(100.0 * opaque / (w * h) + 0.5);
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

// Every RT_GROUP_ICON in the module, by name AND by ordinal. This is what makes the "the artwork is
// registered under a string name" failure visible: it is the difference between a resource the code can ask
// for by number and one it cannot.
static BOOL CALLBACK CountIcon(HMODULE mod, LPCSTR type, LPSTR name, LONG_PTR param)
{
  (void)mod; (void)type;
  int *n = (int *)param;
  if (IS_INTRESOURCE(name))
    printf("  RT_GROUP_ICON  ordinal=%d\n", (int)(INT_PTR)name);
  else
    printf("  RT_GROUP_ICON  NAME=\"%s\"   <- not reachable by MAKEINTRESOURCE\n", name);
  (*n)++;
  return TRUE;
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("usage: apex_icon_probe.exe <module-with-icons> [id ...]\n");
    return 2;
  }
  HMODULE inst = LoadLibraryA(argv[1]);
  if (!inst)
  {
    printf("cannot load %s (err %lu)\n", argv[1], GetLastError());
    return 2;
  }

  int nIcons = 0;
  printf("icon resources in %s:\n", argv[1]);
  EnumResourceNamesA(inst, RT_GROUP_ICON, CountIcon, (LONG_PTR)&nIcons);
  if (nIcons == 0)
    printf("  (none)\n");
  printf("\n");

  int ids[16];
  int n = 0;
  if (argc >= 3)
  {
    for (int i = 2; i < argc && n < 16; ++i)
      ids[n++] = atoi(argv[i]);
  }
  else
  {
    ids[n++] = 1;
    ids[n++] = 2;
  }

  double luma[16];
  int failures = 0;
  for (int i = 0; i < n; ++i)
  {
    int r = 0, g = 0, b = 0, cov = 0;
    const int size = GetSystemMetrics(SM_CXSMICON) > 0 ? GetSystemMetrics(SM_CXSMICON) : 16;
    const double l = LumaOfIcon(inst, ids[i], size, size, &r, &g, &b, &cov);
    luma[i] = l;
    if (l < 0)
    {
      // THE FAILURE THIS PROBE WAS WRITTEN FOR: the id the code asks for is not in the module.
      printf("  id=%d  %dx%d  NOT LOADABLE -- there is no resource with this id\n", ids[i], size, size);
      failures++;
    }
    else
    {
      // No field-width padding in these numbers: the gate parses them, and "luma= 228.2" is one more thing
      // for the parser to get right for no benefit.
      printf("  id=%d  %dx%d  meanRGB=%d,%d,%d  luma=%.1f  opaque=%d%%  -> %s mark\n", ids[i], size,
             size, r, g, b, l, cov, l > 128 ? "LIGHT" : "DARK");
    }
  }

  // Two ids that hold the same picture are as broken as one that cannot be loaded: the tray would be
  // "following the theme" while looking identical either way. The threshold is deliberately loose --
  // this is a check that they are DIFFERENT, not a check that they are any particular shade.
  if (n == 2 && luma[0] >= 0 && luma[1] >= 0)
  {
    const double diff = luma[0] > luma[1] ? luma[0] - luma[1] : luma[1] - luma[0];
    if (diff < 32.0)
    {
      printf("\n  the two marks are not distinguishable (luma differs by %.1f) -- a swap would be invisible\n",
             diff);
      failures++;
    }
  }

  FreeLibrary(inst);
  if (failures)
  {
    printf("\nFAILED (%d)\n", failures);
    return 1;
  }
  printf("\nok\n");
  return 0;
}
