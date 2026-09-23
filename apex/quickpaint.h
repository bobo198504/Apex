#ifndef APEX_QUICKPAINT_H
#define APEX_QUICKPAINT_H

// ⚠️ A FEATURE MAY NOT INCLUDE THIS FILE -- the guard below is what makes that a CONTRACT rather than a
// convention. A feature is compiled with the same -I paths as the host (see apex/build.sh) and exports a C
// ABI, so nothing else stops it reaching in here and calling host internals the ABI never promised to keep
// stable. Everything a feature is entitled to is in abi.h; this file is how the host DRAWS the flyout.
#ifndef APEX_BUILDING_HOST
#error "quickpaint.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif

// ---------------------------------------------------------------------------
// HOW THE QUICK PANEL LOOKS, IN ONE PLACE, WITH NO HOST IN IT.
//
// ⚠️ WHY IT IS ITS OWN FILE, AND THE REASON IS NOT TIDINESS. Twice now the user has reported something that is
// purely about what the flyout LOOKS like -- "大面板装着小面板，中间的间隙太大", then "外层大板去掉，完全不用，只
// 留下各小板" -- and both times the only instrument was their eye, one deploy later. The settings page solved
// exactly this problem long ago: `_diag/panel_preview.js` renders the REAL page with a real browser and writes a
// PNG, so a layout question is answered by LOOKING at the thing instead of by reasoning about the code that
// draws it (`docs/rules/panel.md`: "布局问题不许猜 ... 用真浏览器渲染出来量").
//
// This header is the flyout's half of that. The painting depends on nothing but GDI+ and the model, so it is
// callable from two places: the window (quickpanel_win.cpp) and `_diag/quickpanel_preview.cpp`, which renders
// the same code into a PNG. ⚠️ NOTHING ABOUT THE PICTURE MAY BE DUPLICATED IN THE WINDOW, or the preview would
// be a picture of something else -- which is the whole failure this file exists to prevent.
//
// WHAT IS *NOT* HERE: the window, the DIB, UpdateLayeredWindow, the animation, the model building, and the
// question of which theme is in force. The caller answers that last one and passes it in (`ctx.light`), because
// only a process with a config file and a Windows call can.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <math.h>

#include "quickpalette.h"
#include "quickpanel.h"

namespace apex {
namespace quick {

// The radius of a pane's corners. ⚠️ IT LIVES HERE, WITH THE PICTURE, rather than beside the window's own
// constants: the preview renders the same panes, and it must round their corners by the same amount -- a second
// constant in the probe would be a preview of a panel nobody ships.
static const int kSectionRadius = 8;

// Everything the picture needs, gathered so that the painter never reaches for a global: the window has one
// model and the preview has another, and a painter that read the window's would draw the wrong panel in the
// probe -- silently, because it would still draw something.
struct PaintCtx
{
  const Model *model = nullptr;
  const Layout *layout = nullptr;
  int panelW = 0;
  int panelH = 0;
  int scale = 100;
  bool light = true;
  bool zh = true;
  int hover = -1;    // the row under the pointer, or -1
  // ⚠️ HOW LONG THE POINTER HAS BEEN ON THAT ROW, in milliseconds -- the input to the scrolling label (see
  // `LabelScrollOffset` in quickpanel.h). Without it a label too long for its row would read as "SDC4190 288…"
  // for ever, and the user's request was "快速面板显示和音量的设备名太长，鼠标移上去，可以滚动设备名".
  int hoverMs = 0;
  int shadow = 12;   // the window's own margin around the content, in content-space terms
  // ⚠️ THE AIR BETWEEN TWO PANES, COPIED FROM THE LAYOUT'S OWN METRICS rather than assumed: the panes' shadow is
  // sized from it (see the reach below), and a painter that carried its own idea of the gap would draw a shadow
  // that reaches into a seam it thinks is wider than it is.
  int sectionGap = 3;
};

inline Gdiplus::Color Col(const Rgb &c, int a = 255) { return Gdiplus::Color(a, c.r, c.g, c.b); }
inline Gdiplus::Color Shade(const Gdiplus::Color &c, int a)
{
  return Gdiplus::Color(a, c.GetR(), c.GetG(), c.GetB());
}

inline void ToWide(const char *utf8, wchar_t *out, int cap)
{
  if (!out || cap <= 0)
    return;
  out[0] = 0;
  if (utf8)
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap - 1);
}

// ⚠️ CHECKED, NOT ASSUMED. GDI+ substitutes a default family when the named one is missing, and
// "Microsoft YaHei UI" is absent on an install without the Chinese fonts -- where the substitution would be a
// silent surprise rather than a crash. The fallback is the family Windows uses for its own UI text.
inline const wchar_t *FontFamilyName()
{
  static const wchar_t *chosen = nullptr;
  if (!chosen)
  {
    Gdiplus::FontFamily yahei(L"Microsoft YaHei UI");
    chosen = yahei.IsAvailable() ? L"Microsoft YaHei UI" : L"Segoe UI";
  }
  return chosen;
}

inline void AddRoundRect(Gdiplus::GraphicsPath &path, const Gdiplus::Rect &r, int radius)
{
  int d = radius * 2;
  if (d > r.Width)
    d = r.Width;
  if (d > r.Height)
    d = r.Height;
  if (d <= 0)
  {
    path.AddRectangle(r);
    return;
  }
  path.AddArc(r.X, r.Y, d, d, 180, 90);
  path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  path.CloseFigure();
}

inline Gdiplus::Rect GRect(const Rect &r) { return Gdiplus::Rect(r.x, r.y, r.w, r.h); }

// A feature's own colour for a control, or the panel's ink when it sent none (see ApexQuickItem::hue).
inline Gdiplus::Color InkFor(const Item &it, const Palette &p)
{
  if (it.hue == 0)
    return Col(p.accent);
  return Gdiplus::Color(255, (BYTE)((it.hue >> 16) & 0xFF), (BYTE)((it.hue >> 8) & 0xFF), (BYTE)(it.hue & 0xFF));
}

// ⚠️ A GLYPH THAT WILL BE DRAWN ON TOP OF `fill` HAS TO CONTRAST WITH IT, AND THE ANSWER IS NOT ALWAYS WHITE.
// The companion button's engaged state is filled with the row's own ink, and that ink is the panel's ACCENT when
// the feature sent no `hue` of its own -- near-black in the light theme (white reads) and NEAR-WHITE in the dark
// one (white does not: the button came out as a blank square). The rule itself lives in quickpalette.h, where it
// has no GDI+ in it and can be checked without a screen; this is the wrapper.
inline int Luma(const Gdiplus::Color &c)
{
  return LumaOf(c.GetR(), c.GetG(), c.GetB());
}

inline Gdiplus::Color LitInk(const Gdiplus::Color &fill, int alpha)
{
  return FillTakesDarkGlyph(Luma(fill)) ? Gdiplus::Color(alpha, 20, 20, 20)
                                        : Gdiplus::Color(alpha, 255, 255, 255);
}

inline const char *LabelOf(const Item &it, bool zh) { return zh ? it.labelZh : it.labelEn; }

inline void DrawTextIn(Gdiplus::Graphics &gfx, const char *utf8, const Rect &rc, const Gdiplus::Font &font,
                       const Gdiplus::Color &color, int align, bool ellipsis)
{
  if (rc.w <= 0 || rc.h <= 0)
    return;
  wchar_t w[256];
  ToWide(utf8, w, 256);
  if (!w[0])
    return;
  Gdiplus::RectF rf((Gdiplus::REAL)rc.x, (Gdiplus::REAL)rc.y, (Gdiplus::REAL)rc.w, (Gdiplus::REAL)rc.h);
  Gdiplus::StringFormat sf;
  sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
  sf.SetAlignment(align == 1   ? Gdiplus::StringAlignmentFar
                  : align == 2 ? Gdiplus::StringAlignmentCenter
                               : Gdiplus::StringAlignmentNear);
  sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
  if (ellipsis)
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  Gdiplus::SolidBrush br(color);
  gfx.DrawString(w, -1, &font, rf, &sf, &br);
}

// How wide `utf8` is in `font`, or 0 when it cannot be measured.
//
// ⚠️ THIS IS WHAT DECIDES WHETHER A LABEL NEEDS TO SCROLL AT ALL, and it has to be the SAME font the drawing
// uses -- measuring with a guess would scroll a label that fits, or cut one that does not.
inline int MeasureTextW(Gdiplus::Graphics &gfx, const char *utf8, const Gdiplus::Font &font)
{
  wchar_t w[256];
  ToWide(utf8, w, 256);
  if (!w[0])
    return 0;
  Gdiplus::RectF box(0, 0, 4096, 4096);
  Gdiplus::StringFormat sf;
  sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
  gfx.MeasureString(w, -1, &font, box, &sf, &box);
  return (int)(box.Width + 0.5f);
}

// A LABEL THAT IS TOO WIDE FOR ITS ROW, DRAWN AT `offset` PIXELS TO THE LEFT AND CLIPPED TO THE ROW.
//
// ⚠️ THE CLIP IS NOT OPTIONAL: without it the part that has scrolled past the row's left edge would be painted
// over whatever is to the left of it -- another block's label, or the switch at the end of a short row. Clipping
// is also what makes the movement read as "the text is sliding inside its box" rather than "the panel is moving".
inline void DrawScrollingLabel(Gdiplus::Graphics &gfx, const char *utf8, const Rect &rc,
                               const Gdiplus::Font &font, const Gdiplus::Color &color, int offset)
{
  if (rc.w <= 0 || rc.h <= 0)
    return;
  Gdiplus::Region clip(Gdiplus::Rect(rc.x, rc.y, rc.w, rc.h));
  const Gdiplus::GraphicsState st = gfx.Save();
  gfx.SetClip(&clip);
  Rect moved = rc;
  moved.x -= offset;
  // The box is widened by the offset so the text is never trimmed while it scrolls: trimming is for a label that
  // does NOT fit, and this one is being shown in full, a window at a time.
  moved.w += offset;
  DrawTextIn(gfx, utf8, moved, font, color, 0, false);
  gfx.Restore(st);
}

inline void DrawSwitch(Gdiplus::Graphics &gfx, const Rect &rc, bool on, int alpha, const Palette &p)
{
  Gdiplus::GraphicsPath path;
  AddRoundRect(path, Gdiplus::Rect(rc.x, rc.y, rc.w, rc.h), rc.h / 2);
  Gdiplus::SolidBrush track(Col(on ? p.dotOn : p.dotOff, alpha));
  gfx.FillPath(&track, &path);

  const int kd = rc.h - 4;
  const int kx = on ? rc.x + rc.w - kd - 2 : rc.x + 2;
  Gdiplus::SolidBrush knob(Gdiplus::Color(alpha, 255, 255, 255));
  gfx.FillEllipse(&knob, kx, rc.y + 2, kd, kd);
}

inline void DrawFader(Gdiplus::Graphics &gfx, const Rect &rc, const Item &it, int alpha, const Palette &p)
{
  const double f = FractionFromValue(it);
  const int h = rc.h;
  Gdiplus::GraphicsPath track;
  AddRoundRect(track, Gdiplus::Rect(rc.x, rc.y, rc.w, h), h / 2);
  Gdiplus::SolidBrush empty(Col(p.track, alpha));
  gfx.FillPath(&empty, &track);

  const int filled = (int)(f * rc.w + 0.5);
  if (filled > 2)
  {
    Gdiplus::GraphicsPath done;
    AddRoundRect(done, Gdiplus::Rect(rc.x, rc.y, filled, h), h / 2);
    Gdiplus::SolidBrush ink(Shade(InkFor(it, p), alpha));
    gfx.FillPath(&ink, &done);
  }

  const int thumb = h + 6;
  const int txc = rc.x + (int)(f * (rc.w - 1) + 0.5);
  Gdiplus::SolidBrush knob(Gdiplus::Color(alpha, 255, 255, 255));
  gfx.FillEllipse(&knob, txc - thumb / 2, rc.y + h / 2 - thumb / 2, thumb, thumb);
  Gdiplus::Pen edge(Gdiplus::Color(alpha / 3, 0, 0, 0), 1.0f);
  gfx.DrawEllipse(&edge, txc - thumb / 2, rc.y + h / 2 - thumb / 2, thumb, thumb);
}

// ⚠️ THE COMPANION SWITCH IS A BUTTON, NOT A SWITCH, AND THE SHAPE IS THE POINT: it shares its row with a fader, and
// a 38-px track-and-knob switch beside a fader reads as a second thing to drag. So it is a small rounded button --
// and the glyph inside it is what the user reads. (The panel draws the picture and the feature owns the meaning --
// see `toggleId`/`toggleIcon` in abi.h; what the button says here is only "on" and "off", plus which of the small
// set of things it is.)
//
// ⚠️ TWO STATES AND THREE MEANINGS, AND THE GLYPH IS WHAT TELLS THEM APART. The first one asked for was the mute
// button on a volume fader ("音量在推子右边增加静音按钮"), and the second was the screen-off control on a brightness
// fader, asked for BY COMPARISON with the first ("快速面板的熄屏功能也像静音按钮一样，做上去") -- so they have to look
// like each other in every way except the one that matters:
//   * "on" is always the ENGAGED state, filled with the row's own ink (`hue`, the same colour as the fader beside
//     it) and struck through: muted, screen off;
//   * "off" is the quiet outlined well, with the plain glyph: sound going through, screen on.
// Drawn from the same facts as the fader beside it so the row reads as one control rather than two.
inline void DrawRowButton(Gdiplus::Graphics &gfx, const Rect &rc, bool on, int alpha, const Palette &p,
                          const Item &it)
{
  const int radius = rc.w / 3;
  Gdiplus::GraphicsPath path;
  AddRoundRect(path, Gdiplus::Rect(rc.x, rc.y, rc.w, rc.h), radius);

  // The OFF state is a quiet well in the pane's own surface; the ON state is the row's colour, filled. It is the
  // same language as the switch (which fills its track when on) at a size that fits beside a fader.
  Gdiplus::SolidBrush back(Col(p.track, (alpha * 150) / 255));
  const Gdiplus::Color fill = InkFor(it, p);
  Gdiplus::SolidBrush lit(Shade(fill, alpha));
  gfx.FillPath(on ? (Gdiplus::Brush *)&lit : (Gdiplus::Brush *)&back, &path);
  Gdiplus::Pen edge(on ? Shade(fill, alpha) : Col(p.border, alpha), 1.0f);
  gfx.DrawPath(&edge, &path);

  // ⚠️ THE GLYPH IS DESIGNED ON A 24-UNIT BOX AND SCALED TO THE BUTTON, so the same drawing works at 125% and at
  // 200% without a second set of numbers.
  const double cx = rc.x + rc.w / 2.0, cy = rc.y + rc.h / 2.0;
  const double s = rc.w / 24.0;
  // ⚠️⚠️ AND ITS COLOUR FOLLOWS THE FILL, WHICH "ALWAYS WHITE" DID NOT. The engaged state is filled with the ROW's
  // OWN ink, and a row that sent no `hue` gets the panel's accent -- near-black in the light theme (a white glyph
  // reads) and NEAR-WHITE in the dark one, where a white glyph is invisible: the button came out as a blank white
  // square. That is the user's report, in their words: "保持唤醒的快速面板少了防止熄屏开关，现在只有保持唤醒开关" --
  // the awake switch is there (it is a switch, drawn in `--dot-on` green), and the screen button beside it was a
  // featureless blob. `LitInk` picks the end of the range that the fill does NOT sit at, so it holds for the
  // accent, for a feature's own hue (amber, blue, anything) and in both themes.
  const Gdiplus::Color ink = on ? LitInk(fill, alpha) : Col(p.fg, alpha);
  Gdiplus::SolidBrush glyph(ink);
  Gdiplus::Pen mark(ink, (Gdiplus::REAL)(1.6 * s));

  switch (it.icon)
  {
  case CompanionIcon::kMute:
  {
    // A box with a cone: the speaker's body, in one closed polygon.
    Gdiplus::PointF body[6] = {{(Gdiplus::REAL)(cx - 6.5 * s), (Gdiplus::REAL)(cy - 2.2 * s)},
                               {(Gdiplus::REAL)(cx - 3.0 * s), (Gdiplus::REAL)(cy - 2.2 * s)},
                               {(Gdiplus::REAL)(cx + 0.6 * s), (Gdiplus::REAL)(cy - 6.0 * s)},
                               {(Gdiplus::REAL)(cx + 0.6 * s), (Gdiplus::REAL)(cy + 6.0 * s)},
                               {(Gdiplus::REAL)(cx - 3.0 * s), (Gdiplus::REAL)(cy + 2.2 * s)},
                               {(Gdiplus::REAL)(cx - 6.5 * s), (Gdiplus::REAL)(cy + 2.2 * s)}};
    gfx.FillPolygon(&glyph, body, 6);
    if (on)
    {
      // MUTED: the slash, across the corner of the speaker -- the universal "no sound" mark, legible at 26 px.
      gfx.DrawLine(&mark, (Gdiplus::REAL)(cx - 4.0 * s), (Gdiplus::REAL)(cy + 6.5 * s),
                   (Gdiplus::REAL)(cx + 7.5 * s), (Gdiplus::REAL)(cy - 6.5 * s));
    }
    else
    {
      // SOUND: two arcs opening to the right, centred at the cone's mouth. GDI+ measures angles from 3 o'clock,
      // clockwise -- so -50..+50 is the right-hand side of each circle, which is the direction sound leaves in.
      const double mouth = cx + 1.0 * s;
      const double radii[2] = {4.0 * s, 7.0 * s};
      for (int k = 0; k < 2; ++k)
      {
        const Gdiplus::REAL r = (Gdiplus::REAL)radii[k];
        const Gdiplus::RectF arc((Gdiplus::REAL)mouth - r, (Gdiplus::REAL)cy - r, 2 * r, 2 * r);
        gfx.DrawArc(&mark, arc, -50.0f, 100.0f);
      }
    }
  }
  break;

  case CompanionIcon::kDisplay:
  {
    // A monitor: the screen and its stand. The screen is drawn as an outline rather than a filled block, so the
    // slash has something to cross -- a filled rectangle with a line over it reads as one smudge at 26 px.
    const Gdiplus::RectF screen((Gdiplus::REAL)(cx - 8.0 * s), (Gdiplus::REAL)(cy - 6.0 * s),
                                (Gdiplus::REAL)(16.0 * s), (Gdiplus::REAL)(10.5 * s));
    gfx.DrawRectangle(&mark, screen);
    gfx.DrawLine(&mark, (Gdiplus::REAL)cx, (Gdiplus::REAL)(cy + 4.5 * s), (Gdiplus::REAL)cx,
                 (Gdiplus::REAL)(cy + 7.0 * s)); // the stand
    if (on)
    {
      // SCREEN OFF: the same slash -- and it crosses the whole screen so it reads as "not this one".
      gfx.DrawLine(&mark, (Gdiplus::REAL)(cx - 9.5 * s), (Gdiplus::REAL)(cy + 8.0 * s),
                   (Gdiplus::REAL)(cx + 9.5 * s), (Gdiplus::REAL)(cy - 8.0 * s));
    }
  }
  break;

  default:
  {
    // PLAIN: a dot. On is filled, off is a ring -- the smallest honest picture of "this is a two-state thing whose
    // meaning the panel was not told".
    const Gdiplus::REAL r = (Gdiplus::REAL)(4.0 * s);
    if (on)
      gfx.FillEllipse(&glyph, (Gdiplus::REAL)cx - r, (Gdiplus::REAL)cy - r, 2 * r, 2 * r);
    else
      gfx.DrawEllipse(&mark, (Gdiplus::REAL)cx - r, (Gdiplus::REAL)cy - r, 2 * r, 2 * r);
  }
  break;
  }
}

// A dial: a ring with the travelled part in ink, and a pointer. It starts at the 7:30 position and sweeps 270
// degrees clockwise, which is where every audio tool has put it for thirty years -- and it is why a knob is
// worth having beside a fader at all: the whole range is legible in one glance at one small square.
inline void DrawKnob(Gdiplus::Graphics &gfx, const Rect &rc, const Item &it, int alpha, const Palette &p)
{
  const double f = FractionFromValue(it);
  const int inset = 3;
  const int pw = rc.w / 9 > 0 ? rc.w / 9 : 2;
  const Gdiplus::Rect ring(rc.x + inset, rc.y + inset, rc.w - 2 * inset, rc.h - 2 * inset);

  // ⚠️ THE UNTRAVELLED PART OF THE RING IS DRAWN IN THE FOREGROUND INK AT A THIRD STRENGTH, NOT IN `--track`.
  // The preview showed why: `--track` (#292929 on a dark theme) is a step away from the pane itself, so the
  // ring that says "this is a dial, and here is how far round it goes" was invisible in the dark theme -- the
  // knob read as a lone pointer. `--track` is right for a fader, whose empty part is a long bar with a filled
  // end to compare against; a dial has no such second half.
  Gdiplus::Pen back(Shade(Col(p.fg), alpha / 3), (Gdiplus::REAL)pw);
  gfx.DrawArc(&back, ring, 135.0f, 270.0f);

  if (f > 0.0)
  {
    Gdiplus::Pen done(Shade(InkFor(it, p), alpha), (Gdiplus::REAL)pw);
    gfx.DrawArc(&done, ring, 135.0f, (Gdiplus::REAL)(270.0 * f));
  }

  const double ang = (135.0 + 270.0 * f) * 3.14159265358979 / 180.0;
  const double cx = rc.x + rc.w / 2.0, cy = rc.y + rc.h / 2.0;
  const double rr = rc.w / 2.0 - inset - pw / 2.0;
  Gdiplus::Pen pointer(Col(p.fg, alpha), 2.0f);
  gfx.DrawLine(&pointer, (Gdiplus::REAL)cx, (Gdiplus::REAL)cy, (Gdiplus::REAL)(cx + cos(ang) * rr),
               (Gdiplus::REAL)(cy + sin(ang) * rr));
}

// ONE PANE'S OWN EDGE -- and there is no outer surface: each pane is its own, and the gaps between panes are
// meant to be AIR.
//
// ⚠️ THE REACH IS SMALLER THAN THE HALF-GAP, AND THE GATE IS WHAT TAUGHT ME THAT. The first version of the
// floating panes used 8 px with a 12 px gap, and the preview probe -- which renders the panel twice and compares
// the pixels -- showed 146 of the 320 pixels across the gap CHANGED. The two panes' shadows met in the middle
// and the gap read as one continuous surface: the very "大板装着小板" the user had asked to be rid of, drawn a
// second time in shadow instead of in colour. The rule that falls out of it is arithmetic rather than taste:
// THE SHADOW MUST NOT REACH THE MIDDLE OF THE GAP, so `reach` stays under half of `sectionGap` -- and the gate
// reads the gap's centre line to hold it there.
//
// It is also why the shadow is not offset downward. An offset one looks slightly more like a real card, and it
// adds `reach / 3` to how far the shadow travels -- which is exactly the margin this has none of to spare.
//
// Not a blur: a real blur would be a second pass over the whole bitmap on every animation frame, and at 15 ms a
// frame it would be the only expensive thing this window does. Concentric rounded outlines at falling alpha
// read as a soft edge at this size and cost one path stroke each.
inline void DrawCardShadow(Gdiplus::Graphics &gfx, const Gdiplus::Rect &card, double alpha, int reach)
{
  for (int i = reach; i >= 1; --i)
  {
    const int sa = (int)(alpha * 7.0 * (reach - i + 1) / reach);
    if (sa <= 0)
      continue;
    Gdiplus::GraphicsPath edge;
    AddRoundRect(edge, Gdiplus::Rect(card.X - i, card.Y - i, card.Width + 2 * i, card.Height + 2 * i),
                 kSectionRadius + i);
    Gdiplus::Pen pen(Gdiplus::Color(sa, 0, 0, 0), 1.0f);
    gfx.DrawPath(&pen, &edge);
  }
}

// THE WHOLE PICTURE, at `alpha` opacity. Coordinates are CONTENT coordinates: the shadow margin is added by one
// translation at the top, and every rectangle below comes from MeasureModel (see quickpanel.h).
inline void PaintPanel(Gdiplus::Graphics &gfx, const PaintCtx &ctx, double alpha)
{
  if (!ctx.model || !ctx.layout)
    return;
  const Model &m = *ctx.model;
  const Layout &ly = *ctx.layout;
  const Palette &p = QuickPalette(ctx.light);
  const int a = (int)(alpha * 255.0 + 0.5);

  gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  // ⚠️ NOT ClearType. ClearType needs an opaque background to sub-pixel against, and this whole surface is an
  // alpha channel; asking for it here is how a flyout ends up with coloured fringes on every glyph.
  gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
  // ⚠️ THIS DOES NOT CLEAR THE SURFACE, AND THAT IS ON PURPOSE. The window clears its DIB before calling (it
  // must -- the buffer is reused every frame), while the PREVIEW draws a stand-in desktop first so that the
  // panel's own transparency is visible in the PNG. A painter that cleared would wipe that background out, and
  // the preview would show a panel over black -- which is exactly the picture nobody needs.
  gfx.TranslateTransform((Gdiplus::REAL)ctx.shadow, (Gdiplus::REAL)ctx.shadow);

  const Gdiplus::REAL base = (Gdiplus::REAL)ctx.scale;
  Gdiplus::Font itemFont(FontFamilyName(), (Gdiplus::REAL)(13.0 * base / 100.0), Gdiplus::FontStyleRegular,
                         Gdiplus::UnitPixel);
  Gdiplus::Font smallFont(FontFamilyName(), (Gdiplus::REAL)(11.0 * base / 100.0), Gdiplus::FontStyleRegular,
                          Gdiplus::UnitPixel);
  Gdiplus::Font headFont(FontFamilyName(), (Gdiplus::REAL)(11.0 * base / 100.0), Gdiplus::FontStyleBold,
                         Gdiplus::UnitPixel);

  // ⚠️ THE SHADOW IS SIZED BY THE GAP, NOT BY TASTE. See the note on DrawCardShadow: its fringe must not reach
  // the middle of the seam between two panes, or the two shadows meet there and the blocks read as one surface
  // -- which is the "大板装着小板" the user has now asked to be rid of twice. So the reach is capped at half the
  // gap, and with the 3 px the user asked for that leaves ZERO: the panes have no shadow at all, and their edge
  // is carried by the hairline border and the top highlight instead.
  //
  // ⚠️ AND IT IS DERIVED RATHER THAN WRITTEN DOWN, so the two numbers cannot drift apart: a future change to
  // either one either keeps the rule or takes the shadow away, and neither can quietly put the shadow back into
  // the seam. (The pixel gate in check_apex_quickpanel.sh reads that seam's centre line and would catch it.)
  const int halfGap = Scaled(ctx.sectionGap, ctx.scale) / 2;
  int reach = Scaled(6, ctx.scale); // what a pane's own shadow wants, if there is room for it
  if (reach > halfGap - 1)
    reach = halfGap - 1;
  if (reach < 0)
    reach = 0;

  for (int s = 0; s < m.sectionCount; ++s)
  {
    if (!ly.sectionLive[s])
      continue;
    const Gdiplus::Rect card = GRect(ly.boxes[s]);

    // (1) THE PANE'S OWN SHADOW -- this is what puts it above the desktop rather than on it.
    DrawCardShadow(gfx, card, alpha, reach);

    // (2) THE GLASS: a shallow gradient at `bgAlpha`, so what is behind the panel shows through as a tint.
    {
      Gdiplus::GraphicsPath path;
      AddRoundRect(path, card, kSectionRadius);
      const int ba = (int)(p.bgAlpha * alpha);
      Gdiplus::LinearGradientBrush bg(Gdiplus::Point(card.X, card.Y),
                                      Gdiplus::Point(card.X, card.Y + card.Height), Col(p.bgTop, ba),
                                      Col(p.bgBottom, ba));
      gfx.FillPath(&bg, &path);

      // (3) THE EDGE, and the light it catches along the top. The highlight is most of what separates "a
      // translucent rectangle" from "a pane" -- and it is drawn over the top straight run only, so it follows
      // the rounded corner instead of cutting across it.
      Gdiplus::Pen edge(Col(p.border, a), 1.0f);
      gfx.DrawPath(&edge, &path);
      const int inset = kSectionRadius; // where the top edge stops being a curve
      if (card.Width > 2 * inset + 4)
      {
        Gdiplus::Pen shine(Col(p.highlight, (int)(a * 0.7)), 1.0f);
        gfx.DrawLine(&shine, (Gdiplus::REAL)(card.X + inset), (Gdiplus::REAL)(card.Y + 1),
                     (Gdiplus::REAL)(card.X + card.Width - inset), (Gdiplus::REAL)(card.Y + 1));
      }
    }

    const char *title = ctx.zh ? m.sections[s].titleZh : m.sections[s].titleEn;
    DrawTextIn(gfx, title, ly.titles[s], headFont, Col(p.sub, a), 0, true);

    for (int i = 0; i < m.itemCount; ++i)
    {
      const Item &it = m.items[i];
      if (it.section != s)
        continue;
      const bool dim = it.featureOff && it.kind != RowKind::kFeatureSwitch;
      const int rowA = dim ? (a * 130) / 255 : a;

      // A hovered row lifts with the shade the settings page uses for a hovered list row -- and NOT the feature
      // switches, whose rows are a grid: a highlight that stops halfway across a two-column grid reads as a
      // selection rather than as "the pointer is here".
      if (i == ctx.hover && it.kind != RowKind::kNote && it.kind != RowKind::kFeatureSwitch)
      {
        Gdiplus::GraphicsPath hp;
        AddRoundRect(hp, GRect(ly.rows[i]), 6);
        Gdiplus::SolidBrush hb(Col(p.track, a / 2));
        gfx.FillPath(&hb, &hp);
      }

      if (it.kind == RowKind::kNote)
      {
        DrawTextIn(gfx, LabelOf(it, ctx.zh), ly.labels[i], smallFont, Col(p.sub, a), 0, true);
        continue;
      }

      // ⚠️ A LABEL TOO LONG FOR ITS ROW SCROLLS WHILE THE POINTER IS ON THAT ROW (see LabelScrollOffset in
      // quickpanel.h). Measured here, with the font it is about to be drawn in, because "does it fit" is a
      // question about THIS font and THIS width -- and only for the hovered row, so the other rows keep their
      // ellipsis and the panel is still readable at a glance.
      //
      // ⚠️ AND ONLY WHILE IT IS AT FULL OPACITY: during a fade the alpha is moving, and a label sliding under a
      // fading panel is two motions where the user asked for one. (ctx.hoverMs is 0 unless the window is settled
      // and hovering -- see OnMouseMove.)
      {
        const char *label = LabelOf(it, ctx.zh);
        int offset = 0;
        if (i == ctx.hover && ctx.hoverMs > 0)
          offset = LabelScrollOffset(MeasureTextW(gfx, label, itemFont), ly.labels[i].w, ctx.hoverMs);
        if (offset > 0)
          DrawScrollingLabel(gfx, label, ly.labels[i], itemFont,
                             dim ? Col(p.sub, rowA) : Col(p.fg, rowA), offset);
        else
          DrawTextIn(gfx, label, ly.labels[i], itemFont, dim ? Col(p.sub, rowA) : Col(p.fg, rowA), 0, true);
      }
      switch (it.kind)
      {
      case RowKind::kFeatureSwitch: DrawSwitch(gfx, ly.control[i], it.on, a, p); break;
      case RowKind::kToggle: DrawSwitch(gfx, ly.control[i], it.on, rowA, p); break;
      case RowKind::kSlider: DrawFader(gfx, ly.control[i], it, rowA, p); break;
      case RowKind::kKnob: DrawKnob(gfx, ly.control[i], it, rowA, p); break;
      default: break;
      }
      // ⚠️⚠️ THE COMPANION (abi.h: `toggleId`) IS DRAWN AS WHATEVER THE ROW'S OWN CONTROL IS: a SWITCH row gets a
      // second SWITCH -- the user's correction, after seeing the compact button there: "保持唤醒的快速面板…是要用
      // 一样的滑动开关" -- and a fader row gets the compact button (a track-and-knob switch beside a fader reads as
      // a second thing to drag, which is why the button exists at all).
      //
      // ⚠️ THE RECTANGLE DECIDES *WHERE*, THE ROW KIND DECIDES *WHAT*, and the two must not be swapped: MeasureModel
      // sizes the box by the kind (see CompanionW) and this draws the matching picture, so a switch can never be
      // drawn in a box measured for a button.
      if (ly.extras[i].w > 0)
      {
        if (it.kind == RowKind::kToggle)
          DrawSwitch(gfx, ly.extras[i], it.toggleOn, rowA, p);
        else
          DrawRowButton(gfx, ly.extras[i], it.toggleOn, rowA, p, it);
      }
      if (it.kind == RowKind::kSlider || it.kind == RowKind::kKnob)
      {
        char num[64];
        FormatValue(it, num, sizeof(num));
        DrawTextIn(gfx, num, ly.values[i], smallFont, Col(p.sub, rowA), 1, false);
      }
    }
  }
  gfx.ResetTransform();
}

} // namespace quick
} // namespace apex

#endif // APEX_QUICKPAINT_H
