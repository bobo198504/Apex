// ---------------------------------------------------------------------------
// name_width_probe -- HOW WIDE IS A DEVICE NAME IN THE PANEL'S OWN FONT, AND WHY DOES IT GET AN ELLIPSIS?
//
// The user reports that `PLATANE UP2 SE` is shown as `PLATANE UP2 S...` -- so the name column is narrower than
// the text it holds. The measurement lives in `panel.js` (`fitNameColumn`), and the cut happens through
// `.skiprow .nm { overflow: hidden; text-overflow: ellipsis; }`, which turns ANY shortfall into an ellipsis.
//
// This probe answers the two questions that separate the possible causes, WITHOUT a browser:
//
//   1. HOW WIDE IS THE STRING in the font the page uses? (Measured with GDI, at the page's own size.)
//   2. WHAT DOES `fitNameColumn` COMPUTE, AND WHAT ARE ITS LIMITS? -- the floor (70) and the ceiling (420) are
//      both hard-coded, and a name WIDER THAN THE CEILING is exactly the case that produces the symptom: the
//      column is clamped BELOW the text, and the ellipsis silently eats the difference.
//
// ⚠️ THE PROBE LOOKS AT THE LIMITS FIRST, because a shortfall can only come from one of three places: the
// probe-measurement (a `span` with the cell's font metrics), the floor, or the ceiling. Printing all three
// against the real string says which, instead of guessing at a stylesheet.
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/name_width_probe.cpp \
//       -o build/temp/name_width_probe.exe -lgdi32
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

// ⚠️ THE PAGE'S OWN NUMBERS, COPIED FROM panel.css -- NOT INVENTED. If any of these drifts, this probe stops
// answering the question it was written for, so they are quoted with the file they come from.
static const int kFloorPx = 70;  // panel.js: `if (widest < 70) widest = 70;`
static const int kCeilPx = 420;  // panel.js: `if (widest > 420) widest = 420;`

struct FontCase
{
  const char *label;
  int px;
  int weight; // FW_NORMAL / FW_SEMIBOLD
};

int main(int argc, char **argv)
{
  const char *names[] = {
      "PLATANE UP2 SE",
      "MOTU Pro Audio",
      "Realtek(R) Audio",
      "EarPods",
      "适用于蓝牙® 音频的英特尔® 智音技术",
      "HD Audio Driver for Display Audio",
  };
  const int nameCount = (int)(sizeof(names) / sizeof(names[0]));

  printf("==== DEVICE NAME WIDTH, IN THE PANEL'S OWN FONT ====\n\n");
  printf("fitNameColumn's limits: floor %d px, ceiling %d px\n\n", kFloorPx, kCeilPx);

  // ⚠️ THE FONT SIZES COME FROM panel.css's TYPE SCALE. Several are tried because the answer depends on the
  // one in force, and the user's screenshot is the arbiter -- this probe is here to show the SHAPE of the
  // problem (a name wider than the ceiling is cut, always), not to guess the exact px.
  FontCase cases[] = {
      {"13px normal", 13, FW_NORMAL},
      {"13.5px normal", 14, FW_NORMAL},
      {"14px normal", 14, FW_NORMAL},
      {"12px normal", 12, FW_NORMAL},
  };

  HDC dc = GetDC(nullptr);
  for (const FontCase &fc : cases)
  {
    HFONT f = CreateFontA(-fc.px, 0, 0, 0, fc.weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    printf("---- %s (Segoe UI) ----\n", fc.label);
    for (int i = 0; i < nameCount; ++i)
    {
      const wchar_t *w = nullptr;
      wchar_t buf[256] = {0};
      MultiByteToWideChar(CP_UTF8, 0, names[i], -1, buf, 255);
      w = buf;
      SIZE sz = {0, 0};
      GetTextExtentPoint32W(dc, w, (int)wcslen(w), &sz);
      const int clamped = sz.cx < kFloorPx ? kFloorPx : (sz.cx > kCeilPx ? kCeilPx : sz.cx);
      printf("  %-42s %4d px", names[i], (int)sz.cx);
      if (sz.cx > kCeilPx)
        printf("   *** WIDER THAN THE CEILING -> clamped to %d, so the text is CUT ***", clamped);
      else if (sz.cx < kFloorPx)
        printf("   (below the floor; widened to %d -- room to spare)", clamped);
      printf("\n");
    }
    SelectObject(dc, old);
    DeleteObject(f);
    printf("\n");
  }
  ReleaseDC(nullptr, dc);

  printf("==== WHAT THIS MEANS ====\n");
  printf("  * a name WIDER than the ceiling is clamped to it, and `.skiprow .nm`'s ellipsis eats the rest --\n");
  printf("    which is the reported symptom;\n");
  printf("  * a name NARROWER than the ceiling gets its measured width, so it should NOT be cut;\n");
  printf("  * ⚠️ SO IF THE LINE ABOVE SHOWS A NAME UNDER 420 px AND THE PAGE STILL CUTS IT, the shortfall is\n");
  printf("    NOT the ceiling -- it is the measurement in `fitNameColumn`, and the probe's span does not carry\n");
  printf("    the cell's own box model (padding/border), which a `span` cannot have.\n");
  return 0;
}
