// ---------------------------------------------------------------------------
// WHAT THE QUICK PANEL ACTUALLY LOOKS LIKE, AND THE TWO THINGS THE USER HAS ASKED FOR TWICE.
//
// WHY THIS EXISTS. Twice the user has reported something purely about appearance -- "现在看上去是大面板装着小
// 面板，中间的间隙太大", then "外层大板去掉，完全不用，只留下各小板" -- and twice the only instrument was their
// eye, one deploy later. That is the wrong shape for a loop: a question about a picture should be answered by
// LOOKING at the picture. The settings page solved exactly this long ago (`_diag/panel_preview.js`: the real
// page, a real browser, a PNG -- see docs/rules/panel.md), and this is the flyout's half of it.
//
// ⚠️ IT CALLS `PaintPanel` FROM apex/quickpaint.h -- THE SAME FUNCTION THE WINDOW CALLS. A preview that drew its
// own idea of the panel would be a picture of something nobody ships, and every question asked of it would be
// answered about the wrong thing.
//
// ⚠️ AND IT IS A GATE AS WELL AS A PICTURE, because "there is no outer panel" cannot be expressed as arithmetic:
// the layout is IDENTICAL whether or not something paints a background behind it. So the check is done the only
// way that can do it -- by rendering the panel TWICE, once over the bare backdrop and once with the panel on
// top, and comparing:
//
//   * the window's own margin must be PIXEL-IDENTICAL in the two (nothing reached it -- no outer surface);
//   * the gap between two panes must be PIXEL-IDENTICAL too (it is still the desktop: the panes did not merge,
//     and their shadows did not fill the air between them -- which is what the first version did);
//   * a pane must DIFFER where a pane should be.
//
// ⚠️ COMPARING TWO RENDERS IS NOT PEDANTRY -- THE FIRST VERSION OF THIS GATE COMPARED AGAINST THE BACKDROP'S
// COLOUR AND GOT THE ANSWER WRONG IN BOTH THEMES. 78% of a light pane over white IS white; 78% of a dark pane
// over black is nearly black. "Is something painted here?" has no answer in those terms, which is why the
// question is now asked of the same pixel in two pictures.
//
// Build: g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I apex -o build/_quickpanel_preview.exe \
//            _diag/quickpanel_preview.cpp -lgdiplus -lgdi32
// Run:   build/_quickpanel_preview.exe build     (writes _quickpanel_preview_{light,dark}.png, exit 0 = ok)
// ---------------------------------------------------------------------------

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickpaint.h"
#include "quickpanel.h"

using namespace apex::quick;

static int g_failed = 0;

static void Check(bool ok, const char *what, const char *detail)
{
  printf("  %-62s %s%s\n", what, ok ? "ok" : "FAIL", detail);
  if (!ok)
    ++g_failed;
}

static int GetEncoderClsid(const WCHAR *mime, CLSID *out)
{
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (size == 0)
    return -1;
  Gdiplus::ImageCodecInfo *info = (Gdiplus::ImageCodecInfo *)malloc(size);
  if (!info)
    return -1;
  Gdiplus::GetImageEncoders(num, size, info);
  int found = -1;
  for (UINT i = 0; i < num; ++i)
    if (wcscmp(info[i].MimeType, mime) == 0)
    {
      *out = info[i].Clsid;
      found = (int)i;
      break;
    }
  free(info);
  return found;
}

// THE PANEL THE PREVIEW DRAWS -- the shape a real one has on a machine with the three shipped features: the
// host's switch grid, and one feature that has mapped two of its own controls.
static Model SampleModel()
{
  Model m;
  const int grid = m.AddSection("\xe6\x8f\x92\xe4\xbb\xb6", "Features");
  struct Row
  {
    const char *id, *zh, *en;
    bool on;
  };
  const Row rows[] = {
      {"SmoothWheel", "\xe6\xbb\x91\xe5\x8a\xa8\xe6\xbb\x9a\xe8\xbd\xae", "Smooth Wheel Scroll", true},
      {"AutoIME", "\xe8\x87\xaa\xe5\x8a\xa8\xe8\xbe\x93\xe5\x85\xa5\xe6\xb3\x95", "Auto IME", true},
      {"KeepAwake", "\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92", "Keep Awake", true},
  };
  for (int i = 0; i < 3; ++i)
  {
    Item *it = m.AddItem(grid, RowKind::kFeatureSwitch, rows[i].id);
    CopyStr(it->labelZh, kLabelLen, rows[i].zh);
    CopyStr(it->labelEn, kLabelLen, rows[i].en);
    it->on = rows[i].on;
  }
  // ⚠️ ONE CONTROL, ONE BLOCK -- the user's rule ("快速面板局部功能分组逻辑不以插件分组，而是以一个开关为一组"),
  // and the preview has to draw it that way or it is a picture of a panel nobody ships. It is also why these
  // blocks have NO heading line: the control's own label is the heading (see `single` in MeasureModel).
  {
    const int sec = m.AddSection("\xe6\xbb\x91\xe5\x8a\xa8\xe6\x97\xb6\xe9\x95\xbf", "Glide");
    Item *it = m.AddItem(sec, RowKind::kSlider, "glide");
    CopyStr(it->labelZh, kLabelLen, "\xe6\xbb\x91\xe5\x8a\xa8\xe6\x97\xb6\xe9\x95\xbf");
    CopyStr(it->labelEn, kLabelLen, "Glide");
    CopyStr(it->unit, kUnitLen, "ms");
    it->min = 100;
    it->max = 300;
    it->step = 5;
    it->value = 200;
    it->hue = 0x78BEFF; // the same blue the settings page's slider uses
  }
  {
    const int sec = m.AddSection("\xe6\x9c\x80\xe9\xab\x98\xe9\x80\x9f\xe5\xba\xa6", "Top speed");
    Item *it = m.AddItem(sec, RowKind::kKnob, "top");
    CopyStr(it->labelZh, kLabelLen, "\xe6\x9c\x80\xe9\xab\x98\xe9\x80\x9f\xe5\xba\xa6");
    CopyStr(it->labelEn, kLabelLen, "Top speed");
    CopyStr(it->unit, kUnitLen, "x");
    it->min = 1.0;
    it->max = 2.0;
    it->step = 0.05;
    it->value = 1.5;
    it->hue = 0xC8AAFF;
  }
  // ⚠️ AND A VOLUME GROUP WITH THE MUTE BUTTONS, because that is the newest thing the panel draws and a preview that
  // does not contain it cannot answer "what does it look like" -- which is the only reason this file exists (see the
  // note at the top). It is also the widest kind of row there is: a five-character name, a fader, a read-out and a
  // button, so it is the row that would break first if the panel were too narrow.
  {
    const int sec = m.AddSection("\xe9\x9f\xb3\xe9\x87\x8f", "Volume");
    struct Vol
    {
      const char *id, *zh, *en;
      double value;
      bool muted;
    };
    const Vol vols[] = {
        {"sessions[0].volume", "SDC4190 2880x1800", "SDC4190 2880x1800", 75, true},
        {"sessions[1].volume", "\xe7\xb3\xbb\xe7\xbb\x9f\xe5\xa3\xb0\xe9\x9f\xb3", "System sounds", 40, false},
    };
    for (int i = 0; i < 2; ++i)
    {
      Item *it = m.AddItem(sec, RowKind::kSlider, vols[i].id);
      CopyStr(it->labelZh, kLabelLen, vols[i].zh);
      CopyStr(it->labelEn, kLabelLen, vols[i].en);
      CopyStr(it->groupZh, kGroupLen, "\xe9\x9f\xb3\xe9\x87\x8f");
      CopyStr(it->groupEn, kGroupLen, "Volume");
      CopyStr(it->unit, kUnitLen, "%");
      it->min = 0;
      it->max = 100;
      it->step = 1;
      it->value = vols[i].value;
      it->hue = 0x78BEFF;
      char path[64];
      _snprintf(path, sizeof(path), "sessions[%d].mute", i);
      CopyStr(it->toggleId, kIdLen, path);
      it->toggleOn = vols[i].muted;
      it->icon = CompanionIcon::kMute;
    }
  }
  // ⚠️ AND A BRIGHTNESS GROUP WITH THE SCREEN-OFF BUTTON, for the same reason: it is the second thing a companion
  // switch is drawn for, it is the one the user asked for by comparing it with the mute button ("快速面板的熄屏功能
  // 也像静音按钮一样，做上去"), and the two icons are what the eye has to tell apart in this picture.
  {
    const int sec = m.AddSection("\xe4\xba\xae\xe5\xba\xa6", "Brightness");
    struct Mon
    {
      const char *id, *title;
      double value;
      bool off;
    };
    const Mon mons[] = {
        {"displays[0].brightness", "SDC4190 2880x1800", 90, false},
        {"displays[1].brightness", "HKC0000 3840x2160", 55, true},
    };
    for (int i = 0; i < 2; ++i)
    {
      Item *it = m.AddItem(sec, RowKind::kSlider, mons[i].id);
      CopyStr(it->labelZh, kLabelLen, mons[i].title);
      CopyStr(it->labelEn, kLabelLen, mons[i].title);
      CopyStr(it->groupZh, kGroupLen, "\xe4\xba\xae\xe5\xba\xa6");
      CopyStr(it->groupEn, kGroupLen, "Brightness");
      CopyStr(it->unit, kUnitLen, "%");
      it->min = 0;
      it->max = 100;
      it->step = 1;
      it->value = mons[i].value;
      it->hue = 0xFFC24D; // the brightness fader's own amber, the same one the settings page uses
      char path[64];
      _snprintf(path, sizeof(path), "displays[%d].off", i);
      CopyStr(it->toggleId, kIdLen, path);
      it->toggleOn = mons[i].off;
      it->icon = CompanionIcon::kDisplay;
    }
  }
  // ⚠️ AND A KEEP-AWAKE GROUP: SWITCH ROWS WITH THE SCREEN BUTTON BESIDE THEM (ABI 16 -> 17). It is here for the
  // same reason as the two above -- this preview is the only instrument that answers "what does it look like" --
  // and it is the shape the user asked for in so many words ("快速面板按列表显示系统和各应用的两个功能开关").
  // ⚠️ IT IS ALSO THE ONE ROW KIND WHOSE COMPANION IS NEW, so a preview without it could not show that a switch
  // row draws its button at all.
  {
    const int sec = m.AddSection("\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92", "Keep awake");
    struct Ka
    {
      const char *id, *zh, *en;
      bool awake, display;
    };
    const Ka kas[] = {
        {"rules[0].awake", "\xe7\xb3\xbb\xe7\xbb\x9f\xe5\x85\xa8\xe5\xb1\x80", "System-wide", true, false},
        {"rules[1].awake", "chrome.exe", "chrome.exe", true, true},
    };
    for (int i = 0; i < 2; ++i)
    {
      Item *it = m.AddItem(sec, RowKind::kToggle, kas[i].id);
      CopyStr(it->labelZh, kLabelLen, kas[i].zh);
      CopyStr(it->labelEn, kLabelLen, kas[i].en);
      CopyStr(it->groupZh, kGroupLen, "\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92");
      CopyStr(it->groupEn, kGroupLen, "Keep awake");
      it->on = kas[i].awake;
      char path[64];
      _snprintf(path, sizeof(path), "rules[%d].display", i);
      CopyStr(it->toggleId, kIdLen, path);
      it->toggleOn = kas[i].display;
      it->icon = CompanionIcon::kDisplay;
    }
  }
  return m;
}

// A STAND-IN DESKTOP, IN TWO HALVES. ⚠️ ONE HALF NEAR-WHITE AND ONE HALF NEAR-BLACK, WITH A HARD EDGE DOWN THE
// MIDDLE, because that is what makes a translucent pane legible AS translucent: the same pane reads light on one
// side and dark on the other, and nothing else in the picture could tell you that. (A pretty gradient was the
// first attempt and it was useless here -- a 78%-opaque pane over a soft gradient is indistinguishable from a
// solid one.) The red band at the bottom is one more colour for the eye to judge the tint against.
static void PaintDesktop(Gdiplus::Graphics &gfx, int w, int h)
{
  Gdiplus::SolidBrush white(Gdiplus::Color(255, 252, 252, 252));
  Gdiplus::SolidBrush black(Gdiplus::Color(255, 8, 8, 10));
  Gdiplus::SolidBrush red(Gdiplus::Color(255, 210, 60, 50));
  gfx.FillRectangle(&white, 0, 0, w / 2, h);
  gfx.FillRectangle(&black, w / 2, 0, w - w / 2, h);
  gfx.FillRectangle(&red, 0, h - 22, w / 2, 22);
}

static bool SamePixel(Gdiplus::Bitmap &a, Gdiplus::Bitmap &b, int x, int y)
{
  Gdiplus::Color ca, cb;
  if (a.GetPixel(x, y, &ca) != Gdiplus::Ok || b.GetPixel(x, y, &cb) != Gdiplus::Ok)
    return false;
  return ca.GetR() == cb.GetR() && ca.GetG() == cb.GetG() && ca.GetB() == cb.GetB();
}

// ---- THE GATE: the same pixel, in two pictures -----------------------------------------------
static void GatePixels(Gdiplus::Bitmap &bare, Gdiplus::Bitmap &panel, const Layout &ly, const Metrics &mx,
                       int shadow, const char *name)
{
  const int w = bare.GetWidth(), h = bare.GetHeight();
  char detail[160];

  // (1) THE WINDOW'S OWN MARGIN IS UNTOUCHED. Comparing the two renders rather than a colour is what makes this
  // exact: whatever the backdrop happens to be there, the panel must not have changed it.
  int painted = 0, atX = -1, atY = -1;
  for (int y = 0; y < shadow - 2 && !painted; ++y)
    for (int x = 0; x < w; ++x)
      if (!SamePixel(bare, panel, x, y))
      {
        painted = 1;
        atX = x;
        atY = y;
        break;
      }
  for (int x = 0; x < shadow - 2 && !painted; ++x)
    for (int y = 0; y < h; ++y)
      if (!SamePixel(bare, panel, x, y))
      {
        painted = 1;
        atX = x;
        atY = y;
        break;
      }
  if (painted)
    _snprintf(detail, sizeof(detail), "(%s: painted at %d,%d)", name, atX, atY);
  else
    _snprintf(detail, sizeof(detail), "(%s)", name);
  Check(!painted, "the window's margin is untouched -- there is no outer surface", detail);

  if (ly.boxes[0].h > 0 && ly.boxes[1].h > 0)
  {
    // (2) THE GAP BETWEEN TWO PANES IS STILL THE DESKTOP -- the whole of "只留下各小板". Read across the gap's
    // width, because a shadow that reached too far would show at its edges rather than its middle.
    const int seamY = shadow + ly.boxes[0].Bottom() + mx.sectionGap / 2;
    int changed = 0;
    for (int x = shadow; x < w - shadow; ++x)
      if (seamY >= 0 && seamY < h && !SamePixel(bare, panel, x, seamY))
        ++changed;
    _snprintf(detail, sizeof(detail), "(%s: %d of %d pixels changed)", name, changed, w - 2 * shadow);
    Check(changed == 0, "the gap between two panes is air, not shadow", detail);

    // (3) AND A PANE IS INDEED PAINTED. Without this the two checks above would pass on a panel that draws
    // nothing at all.
    const int cx = shadow + ly.boxes[1].x + ly.boxes[1].w / 2;
    const int cy = shadow + ly.boxes[1].y + ly.boxes[1].h / 2;
    _snprintf(detail, sizeof(detail), "(%s: %d,%d)", name, cx, cy);
    Check(!SamePixel(bare, panel, cx, cy), "a pane IS painted where a pane should be", detail);
  }
}

static bool Render(const char *outDir, bool light, bool zh, const char *name)
{
  Model m = SampleModel();
  Metrics mx = DefaultMetrics(100);
  Layout ly;
  const int contentH = MeasureModel(m, mx, &ly);
  if (contentH <= 0)
  {
    printf("FAIL: the sample model measured nothing\n");
    return false;
  }
  const int shadow = 12;
  const int w = mx.width + 2 * shadow;
  const int h = contentH + 2 * shadow;
  const int before = g_failed;

  // TWO PICTURES OF THE SAME MOMENT: the backdrop alone, and the backdrop with the panel over it.
  Gdiplus::Bitmap bare(w, h, PixelFormat32bppARGB);
  {
    Gdiplus::Graphics g(&bare);
    PaintDesktop(g, w, h);
    g.Flush(Gdiplus::FlushIntentionSync);
  }

  Gdiplus::Bitmap shot(w, h, PixelFormat32bppARGB);
  {
    Gdiplus::Graphics g(&shot);
    PaintDesktop(g, w, h);
    PaintCtx ctx;
    ctx.model = &m;
    ctx.layout = &ly;
    ctx.panelW = mx.width;
    ctx.panelH = contentH;
    ctx.scale = 100;
    ctx.light = light;
    ctx.zh = zh;
    ctx.hover = -1;
    ctx.shadow = shadow;
    ctx.sectionGap = mx.sectionGap; // the seam the shadow must not reach into (see PaintPanel)
    PaintPanel(g, ctx, 1.0);
    g.Flush(Gdiplus::FlushIntentionSync);
  }

  CLSID png;
  if (GetEncoderClsid(L"image/png", &png) < 0)
  {
    printf("FAIL: no PNG encoder\n");
    return false;
  }
  char path[600];
  _snprintf(path, sizeof(path), "%s/_quickpanel_preview_%s.png", outDir, name);
  wchar_t wide[600];
  ToWide(path, wide, 600); // the file APIs want UTF-16; the same helper the painter uses for its labels
  const Gdiplus::Status st = shot.Save(wide, &png, nullptr);
  printf("  -- %s: %dx%d, %d bytes of content -> %s (%s)\n", name, w, h, contentH, path,
         st == Gdiplus::Ok ? "written" : "FAILED");

  GatePixels(bare, shot, ly, mx, shadow, name);
  return st == Gdiplus::Ok && g_failed == before;
}

int main(int argc, char **argv)
{
  const char *dir = argc > 1 ? argv[1] : "build";
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartupInput in;
  if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok)
  {
    printf("FAIL: GDI+ did not start\n");
    return 1;
  }
  printf("quick panel preview: %s\n", dir);
  Render(dir, true, true, "light");
  Render(dir, false, true, "dark");
  Gdiplus::GdiplusShutdown(token);
  printf("%s\n", g_failed ? "FAILED: the quick panel's picture" : "OK: there is no outer panel, and the gaps are air");
  return g_failed ? 1 : 0;
}
