#ifndef APEX_TRAYBADGE_H
#define APEX_TRAYBADGE_H

// ---------------------------------------------------------------------------
// THE TRAY MARK WITH THE BADGE -- composed pixel by pixel, and on its own so a probe can LOOK at the result.
//
// ⚠️ TWO REAL FAILURES ARE BURIED HERE, and both were invisible from the host's log:
//
//   1. `hbmMask = nullptr` DOES NOT MEAN "no mask". CreateIconIndirect refuses the whole thing and returns
//      NULL -- so the tray quietly kept the plain mark and the user's report was "the dot is not there".
//   2. GDI DOES NOT WRITE THE ALPHA CHANNEL. DrawIconEx/Ellipse/FillRect on a 32-bit bitmap set the colour
//      bytes and leave alpha at whatever it was (zero), so an icon composed with them draws as NOTHING or as a
//      black square depending on how the shell picks the pass: "the dot is not there" and "a black box
//      appeared" are the same bug seen twice.
//
// SO THE PIXELS ARE OURS: the artwork's colour AND alpha are read out of the icon with GetDIBits, the circle is
// written into the same buffer (colour + alpha 255), and only then is an icon made. No GDI drawing calls at
// all in the path, which is also why the result can be counted and measured in a probe.
//
// ⚠️ AND IT LIVES IN A HEADER because "load=ok" is not evidence that anybody can see anything. The probe
// (_diag/apex_badge_probe.cpp) composes the same icons and writes them out as an image, and the icon gate
// turns that into a PNG and counts the badge pixels: a dot that failed to compose, a dot with no alpha and a
// dot that is simply too small are three different things that a log line cannot tell apart.
// ---------------------------------------------------------------------------

#include <windows.h>

#include "icons.h"

namespace apex {

// The badge's geometry, in one place so the probe and the tray cannot disagree: a circle whose diameter is
// `percent` of the icon, at the bottom-right corner. Half the icon, because it has to be noticed in a 16x16
// slot beside a dozen other tray icons -- this is the number that decides whether the whole idea works.
static const int kBadgePercent = 52;

// Write a filled circle of `color` (alpha 255) into a top-down 32-bit BGRA buffer.
inline void DrawBadgePixels(unsigned char *bgra, int cx, int cy, COLORREF color)
{
  const int d = (cx * kBadgePercent) / 100;
  const int r = d / 2;
  const int ccx = cx - r;   // the circle's centre: the corner it is anchored to, pulled in by its radius
  const int ccy = cy - r;
  const unsigned char b = (unsigned char)GetBValue(color);
  const unsigned char g = (unsigned char)GetGValue(color);
  const unsigned char rr = (unsigned char)GetRValue(color);
  for (int y = ccy - r; y <= ccy + r; ++y)
  {
    if (y < 0 || y >= cy)
      continue;
    for (int x = ccx - r; x <= ccx + r; ++x)
    {
      if (x < 0 || x >= cx)
        continue;
      const int dx = x - ccx, dy = y - ccy;
      if (dx * dx + dy * dy > r * r)
        continue;
      unsigned char *p = bgra + ((size_t)y * cx + x) * 4;
      p[0] = b;
      p[1] = g;
      p[2] = rr;
      p[3] = 255; // ⚠️ THE ALPHA BYTE IS THE ONE NO GDI CALL WOULD HAVE SET
    }
  }
}

// Compose `base` plus the badge. nullptr when anything failed, which the caller treats as "keep the plain
// mark" -- an unbadged mark is worth more than a broken or invisible one.
inline HICON ComposeBadgeIcon(HICON base, int cx, int cy, COLORREF color)
{
  if (!base || cx <= 0 || cy <= 0)
    return nullptr;

  // The artwork's own pixels, alpha included.
  ICONINFO src;
  memset(&src, 0, sizeof(src));
  if (!GetIconInfo(base, &src))
    return nullptr;

  BITMAPINFO bi;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = cx;
  bi.bmiHeader.biHeight = -cy; // top-down, matching the buffer maths above
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  void *bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP canvas = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HDC mem = CreateCompatibleDC(screen);
  const bool read = canvas && bits && mem &&
                    GetDIBits(mem, src.hbmColor, 0, cy, bits, &bi, DIB_RGB_COLORS) == cy;
  if (read)
    DrawBadgePixels((unsigned char *)bits, cx, cy, color);

  HICON out = nullptr;
  if (read)
  {
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = canvas;
    // ⚠️ A MASK IS REQUIRED, EVEN FOR A 32-BIT ICON WITH A REAL ALPHA CHANNEL. All-zero means "nothing is
    // masked out"; the alpha channel is what decides the shape. Passing nullptr here was failure number one.
    ii.hbmMask = CreateBitmap(cx, cy, 1, 1, nullptr);
    out = CreateIconIndirect(&ii);
    if (ii.hbmMask)
      DeleteObject(ii.hbmMask);
  }

  if (src.hbmColor)
    DeleteObject(src.hbmColor);
  if (src.hbmMask)
    DeleteObject(src.hbmMask);
  if (canvas)
    DeleteObject(canvas);
  if (mem)
    DeleteDC(mem);
  if (screen)
    ReleaseDC(nullptr, screen);
  return out;
}

} // namespace apex

#endif // APEX_TRAYBADGE_H
