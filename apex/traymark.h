#ifndef APEX_TRAYMARK_H
#define APEX_TRAYMARK_H

// ---------------------------------------------------------------------------
// THE TRAY MARK WHILE A FEATURE IS WORKING: the artwork's own middle bar, in the hold ink for the degree the
// feature reported -- GREEN for an ordinary hold, RED for the stronger one (icons.h has both colours and the
// measurement that justifies one value per degree on both plates).
//
// (Was `traybadge.h`, which added a green dot to the corner; then a thickened orange bar; then a red bar; now
// two inks and no shape change. The user tried each of them -- "可以把图标中间那个竖点改成橙色" then "应该是视觉
// 错觉了。改回去吧，比较好看。用红色的，这样也比较显示。大红。不管是浅还是暗主题，用一样的颜色", and later
// "保持唤醒（绿色），保持唤醒+防止熄屏（红色）". So: the recolour only, no growth, and the colour is the message.)
//
// ⚠️ THREE FAILURES ARE BURIED HERE, and none of them was visible from the host's log:
//
//   1. `hbmMask = nullptr` DOES NOT MEAN "no mask": CreateIconIndirect refuses the whole thing and returns
//      NULL, so the tray silently kept the plain mark. The user's report was "the dot is not there".
//   2. GDI DOES NOT WRITE THE ALPHA CHANNEL. DrawIconEx/Ellipse/FillRect set the colour bytes and leave alpha
//      at zero, so an icon composed with them draws as nothing (or as a black square): "no dot" and "a black
//      box" are the same bug seen twice.
//   3. THE ARTWORK IS TWO TONES WITH A PAPER TEXTURE, so "the pixels that are not the plate" cannot be found by
//      comparing against one hard-coded colour. The plate's own value is MEASURED (the most common opaque
//      colour) and the bar is everything far enough from it -- which also keeps the bar's anti-aliased edge
//      smooth, because the blend is proportional to that distance.
//
// SO THE PIXELS ARE OURS: the artwork's colour AND alpha come out of the icon with GetDIBits, the bar is
// blended toward the hold ink in that same buffer, and only then is an icon made. No GDI drawing calls in the
// path at all -- which is also why the result can be counted, measured and LOOKED AT by a probe
// (_diag/apex_badge_probe.cpp), instead of being taken on trust from a log line.
// ---------------------------------------------------------------------------

#include <windows.h>

#include "icons.h"

namespace apex {

// ⚠️ THE DEGREE COMES IN AS A NUMBER, NOT AS A COLOUR. The host asks each feature for its state bits and turns
// them into a level (0 = nothing held, 1 = the ordinary hold, 2 = the stronger one); a feature never names a
// colour, and this header never learns what a feature holds. Level 0 is not a valid request -- the caller draws
// the plain mark instead -- and is answered with the ordinary ink so that a mistake is visible rather than
// invisible.
inline COLORREF HoldInk(int level)
{
  return (COLORREF)(level >= 2 ? APEX_HOLD_HARD_INK : APEX_HOLD_INK);
}

// The plate's colour: the most common opaque pixel, quantised to 5 bits per channel. The marks are two-tone, so
// the mode is the plate and nothing else comes close -- and measuring it beats writing it down, because the
// artwork carries a paper texture and may be redrawn.
inline void PlateColorOf(const unsigned char *bgra, int cx, int cy, int *outR, int *outG, int *outB)
{
  int counts[32][32][32];
  memset(counts, 0, sizeof(counts));
  for (int i = 0; i < cx * cy; ++i)
  {
    const unsigned char *p = bgra + (size_t)i * 4;
    if (p[3] < 128)
      continue; // transparent corner: not part of the mark at all
    ++counts[p[2] >> 3][p[1] >> 3][p[0] >> 3];
  }
  int bestR = 0, bestG = 0, bestB = 0, best = -1;
  for (int r = 0; r < 32; ++r)
    for (int g = 0; g < 32; ++g)
      for (int b = 0; b < 32; ++b)
        if (counts[r][g][b] > best)
        {
          best = counts[r][g][b];
          bestR = r;
          bestG = g;
          bestB = b;
        }
  *outR = bestR * 8 + 4;
  *outG = bestG * 8 + 4;
  *outB = bestB * 8 + 4;
}

// Blend the bar toward `ink`, in place. `plate` is what the bar is measured against; the blend is proportional
// to how far a pixel is from it, so the bar's anti-aliased edge stays smooth instead of turning into a hard
// two-colour step (which at 16x16 is the difference between a crisp bar and a jagged one).
inline void RecolourBar(unsigned char *bgra, int cx, int cy, int plateR, int plateG, int plateB, COLORREF ink)
{
  const int inkR = GetRValue(ink), inkG = GetGValue(ink), inkB = GetBValue(ink);
  // How far from the plate a pixel has to be before it counts as "the bar". The texture moves the plate by a
  // few levels; the bar is hundreds of levels away, so anything in between is genuinely the edge.
  const int kInkDistance = 90;
  const int kFullDistance = 260; // and by here it is entirely the bar
  for (int i = 0; i < cx * cy; ++i)
  {
    unsigned char *p = bgra + (size_t)i * 4;
    if (p[3] < 128)
      continue;
    const int d = abs((int)p[2] - plateR) + abs((int)p[1] - plateG) + abs((int)p[0] - plateB);
    if (d < kInkDistance)
      continue; // plate
    int t = (d - kInkDistance) * 100 / (kFullDistance - kInkDistance);
    if (t > 100)
      t = 100;
    p[2] = (unsigned char)((p[2] * (100 - t) + inkR * t) / 100);
    p[1] = (unsigned char)((p[1] * (100 - t) + inkG * t) / 100);
    p[0] = (unsigned char)((p[0] * (100 - t) + inkB * t) / 100);
    p[3] = 255; // ⚠️ THE BYTE NO GDI CALL WOULD HAVE SET
  }
}

// Compose the "working" mark: the same artwork, its bar in the hold ink for `level`. nullptr when anything
// failed, and the caller treats that as "keep the plain mark" -- a plain mark is worth more than a broken or
// invisible one.
inline HICON ComposeActiveMark(HICON base, int cx, int cy, int level)
{
  if (!base || cx <= 0 || cy <= 0)
    return nullptr;

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
  {
    int pr = 0, pg = 0, pb = 0;
    PlateColorOf((const unsigned char *)bits, cx, cy, &pr, &pg, &pb);
    RecolourBar((unsigned char *)bits, cx, cy, pr, pg, pb, HoldInk(level));
  }

  HICON out = nullptr;
  if (read)
  {
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = canvas;
    // ⚠️ A MASK IS REQUIRED EVEN FOR A 32-BIT ICON WITH A REAL ALPHA CHANNEL: all-zero means "nothing is masked
    // out", and the alpha decides the shape. Passing nullptr here was failure number one.
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

#endif // APEX_TRAYMARK_H
