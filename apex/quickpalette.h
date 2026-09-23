#ifndef APEX_QUICKPALETTE_H
#define APEX_QUICKPALETTE_H

// ⚠️ A FEATURE MAY NOT INCLUDE THIS FILE -- the guard below is what makes that a CONTRACT rather than a
// convention. A feature is compiled with the same -I paths as the host (see apex/build.sh) and exports a C
// ABI, so nothing else stops it reaching in here and calling host internals the ABI never promised to keep
// stable. Everything a feature is entitled to is in abi.h; this file is the quick panel's colours.
#ifndef APEX_BUILDING_HOST
#error "quickpalette.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif

// ---------------------------------------------------------------------------
// THE QUICK PANEL'S COLOURS, AS DATA, IN THEIR OWN FILE FOR ONE REASON: A GATE HAS TO READ THEM.
//
// The flyout is drawn by the host (quickpanel_win.cpp) and the settings page is a stylesheet
// (apex/ui/panel.css), and the user sees the two minutes apart. Two near-but-not-equal greys look like a
// mistake, so the values have to MATCH -- and "remember to keep them in step" is not a mechanism. With the
// palette as plain data in a header that needs no Windows, a probe can include it, read the stylesheet's own
// `:root` block, and fail when the two drift (test/check_apex_quickpanel.sh); a C++ translation unit cannot
// read a stylesheet at run time, so a check is the only way this stays true.
//
// ⚠️ THE NAMES ON THE RIGHT OF EACH LINE ARE panel.css's OWN CUSTOM PROPERTIES, and they are the definition:
// `border` IS `--line`, `section` IS `--panel`, and so on. If a value here stops matching the stylesheet, one
// of the two is wrong and the gate will not say which -- look at them together.
//
// ⚠️ THE ONE DELIBERATE DIFFERENCE IS `bgTop`/`bgBottom`. panel.css has a FLAT background (`--bg`); the flyout
// draws a very shallow vertical gradient, because a soft top-to-bottom shift is most of what makes a Mica
// surface read as a surface rather than as a fill. Both ends stay within a few steps of `--bg`, and the gate
// checks exactly that rather than equality.
// ---------------------------------------------------------------------------

namespace apex {
namespace quick {

struct Rgb
{
  unsigned char r, g, b;
};

struct Palette
{
  Rgb bgTop, bgBottom; // a gradient around panel.css's --bg (see the note above)
  Rgb border;          // --line
  Rgb section;         // --panel: the block a feature's controls sit in
  Rgb fg;              // --fg
  Rgb sub;             // --sub: a section title, a read-out
  Rgb track;           // --track: an off switch, a fader's empty part, a hovered row
  Rgb accent;          // --accent: the ink when a feature sent no colour of its own
  Rgb dotOn;           // --dot-on
  Rgb dotOff;          // --dot-off
  Rgb knob;            // the switch's own dot; panel.css writes this one as a literal #fff
  // ⚠️ THE GLASS HIGHLIGHT, AND IT HAS NO panel.css EQUIVALENT. A pane of glass catches the light along its top
  // edge, and that one line is most of what separates "a translucent rectangle" from "a surface". It is drawn at
  // a fraction of the body's alpha; on the light theme it is white on a near-white card, i.e. barely there --
  // which is exactly what a highlight on light glass looks like.
  Rgb highlight;
  // ⚠️ THE BODY'S OPACITY, AND IT HAS BEEN MOVED TWICE BY THE SAME EYE. It started at 246 (96%, a solid card), went
  // to 200 (78%) when the user asked for more glass ("快速面板的透明度不够，玻璃感要加强下"), and is now **230 (90%)**
  // because 78% turned out to be the wrong KIND of see-through: with nothing behind the panel blurred, 78% does not
  // read as glass at all -- it reads as a sticker, and the desktop showing through it is legible enough to fight
  // with the labels ("底下内容完全显示出来，视觉上有干扰", the user's words). At 90% what is behind is a tint and
  // nothing more, while the pane is still visibly not opaque.
  //
  // ⚠️ AND REAL BLUR IS NOT AN OPTION HERE, WHICH IS WHY THIS IS A NUMBER RATHER THAN AN EFFECT. Both of Windows'
  // blur paths are out, and both were tried: the acrylic accent (`SetWindowCompositionAttribute`) paints the WHOLE
  // WINDOW RECTANGLE, gaps and margins included, which is exactly the "大面板" the user asked to be rid of twice
  // (see docs/rules/quickpanel.md, §3.12.1); and the DWM's own backdrops (Mica) do not apply to a layered window,
  // which this one must be, because the fade is the thing the user can actually see. So the glass is carried by
  // the top-edge highlight, the hairline border, the per-pane shadow and this opacity -- nothing else is available.
  //
  // The gate checks the RANGE rather than the exact number, because the value is a judgement and "opaque" versus
  // "glass" is not.
  int bgAlpha;
};

const Palette kLightQuick = {
    {0xFC, 0xFC, 0xFC}, {0xF4, 0xF4, 0xF4}, // --bg is #fafafa; these are the gradient's two ends
    {0xE5, 0xE5, 0xE5},                     // --line
    {0xF2, 0xF2, 0xF2},                     // --panel
    {0x1A, 0x1A, 0x1A},                     // --fg
    {0x67, 0x67, 0x67},                     // --sub
    {0xDD, 0xDD, 0xDD},                     // --track
    {0x1A, 0x1A, 0x1A},                     // --accent
    {0x2E, 0x7D, 0x32},                     // --dot-on
    {0x76, 0x76, 0x76},                     // --dot-off
    {0xFF, 0xFF, 0xFF},                     // the switch's dot
    {0xFF, 0xFF, 0xFF},                     // the highlight along the top edge
    230};

const Palette kDarkQuick = {
    {0x1C, 0x1C, 0x1C}, {0x14, 0x14, 0x14}, // --bg is #141414
    {0x2C, 0x2C, 0x2C},                     // --line
    {0x1A, 0x1A, 0x1A},                     // --panel
    {0xF2, 0xF2, 0xF2},                     // --fg
    {0x9A, 0x9A, 0x9A},                     // --sub
    {0x29, 0x29, 0x29},                     // --track
    {0xF2, 0xF2, 0xF2},                     // --accent
    {0x2E, 0x7D, 0x32},                     // --dot-on (deliberately NOT per theme: see panel.css)
    {0x76, 0x76, 0x76},                     // --dot-off
    {0xFF, 0xFF, 0xFF},                     // the switch's dot
    {0x46, 0x46, 0x46},                     // the highlight: lighter than the card, not white
    230};

inline const Palette &QuickPalette(bool light) { return light ? kLightQuick : kDarkQuick; }

// ---------------------------------------------------------------------------
// ⚠️⚠️ A GLYPH DRAWN ON TOP OF A FILLED CONTROL HAS TO CONTRAST WITH IT, AND THE ANSWER IS NOT ALWAYS WHITE.
//
// WHY THIS IS HERE RATHER THAN IN THE PAINTER (quickpaint.h): it is the same rule the palette values are subject
// to -- "is this readable on that" -- and it is a pure function of three numbers, so it can be exercised by the
// probe that has no GDI+ in it at all (test/check_apex_quickpanel.sh). The painter then has a three-line wrapper.
//
// WHAT WENT WRONG WITHOUT IT: the companion button's engaged state is filled with the ROW's OWN ink, and a row
// whose feature sent no `hue` gets the panel's accent -- near-black in the light theme (a white glyph reads) and
// NEAR-WHITE in the dark one, where a white glyph is invisible. The user's report of the result: "保持唤醒的快速
// 面板少了防止熄屏开关，现在只有保持唤醒开关" -- the awake switch was there (it is drawn in the switch colours),
// and the screen button beside it was a featureless pale blob.
// ---------------------------------------------------------------------------

// Rec. 601 luma: how bright a colour reads, 0..255.
inline int LumaOf(unsigned char r, unsigned char g, unsigned char b)
{
  return (299 * r + 587 * g + 114 * b) / 1000;
}

// Would a fill at this luma take a DARK glyph? 140 is the midpoint with a small bias towards "dark glyph", because
// a mid-tone fill in this panel is more often a light-ish surface (the accent, the amber and blue the features
// send) than a dark one. Either way the two ends are ~135 luma apart, which is the contrast the check in
// check_apex_quickpanel.sh asks for.
inline bool FillTakesDarkGlyph(int luma)
{
  return luma > 140;
}


} // namespace quick
} // namespace apex

#endif // APEX_QUICKPALETTE_H
