#ifndef APEX_QUICKPANEL_H
#define APEX_QUICKPANEL_H

// ⚠️ A FEATURE MAY NOT INCLUDE THIS FILE -- the guard below is what makes that a CONTRACT rather than a
// convention. A feature is compiled with the same -I paths as the host (see apex/build.sh) and exports a C
// ABI, so nothing else stops it reaching in here and calling host internals the ABI never promised to keep
// stable. Everything a feature is entitled to is in abi.h; this file is the quick panel's own model.
#ifndef APEX_BUILDING_HOST
#error "quickpanel.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif

// ---------------------------------------------------------------------------
// THE QUICK PANEL'S DECISIONS, WITH NO WINDOWS IN THEM.
//
// WHAT IS HERE AND WHY IT IS SEPARATE. The flyout the tray shows on a single click is a window, and almost
// everything about a window needs Windows: creating it, painting it, fading it in, following the system's
// light/dark setting. Three things do not, and those three are the ones that can be WRONG rather than merely
// ugly, so they live in this header and are exercised by a probe without a screen:
//
//   * WHERE EVERYTHING GOES   -- the model is turned into rectangles by arithmetic (MeasureModel);
//   * WHAT THE MOUSE HIT      -- a point becomes an item index (HitTest), and a pixel becomes a value
//                                (SliderValueAt / KnobValueAt, both snapped to the item's own step);
//   * HOW LONG THE FADE IS    -- one curve, one place, so "200 ms" is a number rather than a habit
//                                (FadeAlpha).
//
// ⚠️ THE SAME RULE AS EVERYWHERE ELSE IN THIS PROJECT: a decision written twice drifts. The layout arithmetic
// in particular is used by BOTH the painter and the hit test -- if the painter worked out a row's rectangle
// for itself, a click would land where the control used to be. So there is exactly one function that turns the
// model into rectangles, and everything else reads its answer.
//
// WHAT IS *NOT* HERE: colours, fonts, the GDI+ calls, the animation timer, the window, and the question of
// which features exist (that is the host's, and it is asked in quickpanel_win.cpp). This header knows about a
// list of rows, a rectangle, and a handful of numbers.
//
// ⚠️ THE SHAPES ARE THE USER'S THREE: "目前可操作类型暂时定为三种，一种是开关，一种是推子，一种是旋钮" -- a
// toggle, a fader and a knob. `RowKind` is that list, and it is deliberately SHORTER than the settings page's
// vocabulary: a quick panel with a list editor, a text box and a drop-down in it would be the settings window
// with the sidebar taken away, which is the thing it exists not to be.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdio>
#include <cstring>

namespace apex {
namespace quick {

// The ABI's own buffer sizes (see ApexQuickItem in abi.h): an item copied out of a feature lands here
// unchanged, so the two must not drift.
static const int kIdLen = 64;
static const int kLabelLen = 64;
static const int kUnitLen = 24;
static const int kGroupLen = 32;

static const int kMaxItems = 64;    // rows in one panel, across every feature
static const int kMaxSections = 64; // ⚠️ ONE PER CONTROL -- OR PER NAMED GROUP -- see the note in MeasureModel

// How many of a feature's own controls the panel will take. A flyout is not a settings page; a feature with
// more than this has sent the wrong thing, and the ABI says the host draws what fits (see `quickItems`).
//
// ⚠️ 12 -> 24 WHEN NAMED GROUPS ARRIVED (ABI 12 -> 13), and the reason is the shape of those controls rather
// than generosity: a per-monitor or per-application control is ONE ROW PER DEVICE, and the number of devices is
// not something the feature can be told to keep small. A media feature with two monitors and eight playing
// applications has ten faders before it has made a single questionable choice. (`kMaxItems` is 64 for the whole
// panel, so the per-feature cap is still the binding one -- deliberately: one feature must not be able to fill
// the flyout on its own.)
//
// ⚠️ IT LIVES HERE RATHER THAN IN THE WINDOW FILE, because TWO callers need the same number: the flyout asking
// a feature for its items, and `QuickBlocks` (main.cpp) asking which panes exist at all -- see host.h. A private
// copy in the second caller would be a second answer to "how many is that".
static const int kMaxOwnPerFeature = 24;

inline void CopyStr(char *dst, int cap, const char *src)
{
  if (!dst || cap <= 0)
    return;
  int n = 0;
  if (src)
    for (; src[n] && n < cap - 1; ++n)
      dst[n] = src[n];
  dst[n] = 0;
}

// ---- the rows ----------------------------------------------------------------------------------
//
// A FEATURE'S ON/OFF SWITCH IS A ROW OF ITS OWN KIND, and that is not a detail: it is the one control the host
// owns rather than being told about (the `off` list in hostconfig.h), it is always present for every loaded
// feature, and it is what the user's "紧凑视图排列的所有插件开关" names. Everything else in the panel came
// from a feature through `quickItems`.
enum class RowKind
{
  kFeatureSwitch = 0, // this feature is on or off -- the host's own switch
  kToggle,            // a feature's own bool
  kSlider,            // a feature's own range, drawn as a fader
  kKnob,              // a feature's own range, drawn as a dial
  // ⚠️ A LINE OF TEXT WITH NO CONTROL, and it exists so that "nothing to show" is still something rather than
  // an empty rectangle. The user can switch BOTH halves of the panel off in the settings (see hostconfig.h),
  // and a single click on the tray that produces a blank flyout -- or worse, no flyout -- is
  // indistinguishable from a broken program. So the panel says what happened and where the switches are.
  kNote
};

// ⚠️ A COMPANION SWITCH IS DRAWN WITH ONE OF THESE (abi.h: `toggleIcon`). THREE, and the third exists because the
// user asked for the screen-off control in the flyout by comparing it with the mute button: "快速面板的熄屏功能也像
// 静音按钮一样，做上去" -- "like the mute button" means it belongs in the row, not that it looks the same.
enum class CompanionIcon
{
  kPlain = 0,  // no opinion: a plain two-state mark
  kMute = 1,   // a speaker; struck through when it is on
  kDisplay = 2 // a monitor; struck through when it is on (the screen is off)
};

struct Item
{
  RowKind kind = RowKind::kToggle;
  int section = 0;         // which feature's block this row is in
  // ⚠️ WHICH FEATURE, WHICH IS NOT THE SAME AS `section`. The compact switch grid is ONE block holding every
  // feature's switch, so a block is not a feature and a row cannot find its own feature by looking at the block
  // it is in. The slot is filled in when the row is built, and it is what a click needs: a row's `id` is a
  // control path that only means something to the feature that named it.
  int slot = -1;           // the host's feature slot (Loader::At), or -1
  char id[kIdLen] = {0};   // a `setControl` path; for kFeatureSwitch it is the feature's ID
  char labelZh[kLabelLen] = {0};
  char labelEn[kLabelLen] = {0};
  char unit[kUnitLen] = {0};
  // ⚠️ THE NAMED GROUP THIS ROW BELONGS TO, OR EMPTY (abi.h, ABI 12 -> 13). Empty keeps the older behaviour --
  // the row is a pane of its own and takes its heading from its own label (which is why such a pane has no
  // heading line at all). A name is the feature saying "these rows are one thing", and the pane then carries
  // that name as its heading even when it holds a single row: the name is information the user asked for
  // ("分组名称为「亮度」"), not a repeat of the row's label.
  char groupZh[kGroupLen] = {0};
  char groupEn[kGroupLen] = {0};

  double min = 0.0, max = 1.0, step = 0.01; // kSlider / kKnob only
  double value = 0.0;                       // read from the feature on every rebuild
  unsigned hue = 0;                         // 0 = the panel's own accent colour

  bool on = false;        // kFeatureSwitch and kToggle: is it on right now
  bool featureOff = false; // the feature is switched off by the user, so its own rows are drawn as inactive

  // ⚠️ THE COMPANION SWITCH BESIDE THIS CONTROL (abi.h, ABI 14 -> 15) -- the mute button on a volume fader, and
  // since ABI 16 -> 17 the screen button beside a keep-awake switch. An empty path means the row has none. It is a
  // control path like `id`: a click on it sends "1"/"0" through `setControl` and the row is rebuilt from whatever
  // the feature reports, so a feature that clamps or refuses wins on the next frame (see ClickAt below and
  // PushExtra in quickpanel_win.cpp).
  // ⚠️ WHICH PICTURE THE PANEL DRAWS THAT COMPANION WITH (abi.h: `toggleIcon`, ABI 15 -> 16). The panel owns every
  // drawing -- no feature sends a picture -- but it cannot invent one for a meaning it was never told: a mute
  // button and a screen-off button are both plain two-state switches, and they must not look the same. So the
  // feature names the meaning and the panel has one icon per meaning. ⚠️ THE VALUES MIRROR `APEX_QUICK_ICON_*`
  // (like the buffer sizes above), and the mapping from the ABI's own number to this enum happens ONCE, where the
  // item is copied in -- an unknown value lands on `kPlain` there rather than being guessed at here.
  char toggleId[kIdLen] = {0};
  bool toggleOn = false;
  CompanionIcon icon = CompanionIcon::kPlain;
};

struct Section
{
  char titleZh[kLabelLen] = {0};
  char titleEn[kLabelLen] = {0};
};

struct Model
{
  Item items[kMaxItems];
  int itemCount = 0;
  Section sections[kMaxSections];
  int sectionCount = 0;

  // A new feature's block; returns its index, or -1 when there is no room. A section with no rows in it is
  // dropped by MeasureModel rather than drawn as an empty box (see the note there).
  int AddSection(const char *zh, const char *en)
  {
    if (sectionCount >= kMaxSections)
      return -1;
    const int at = sectionCount++;
    CopyStr(sections[at].titleZh, kLabelLen, zh);
    CopyStr(sections[at].titleEn, kLabelLen, en);
    return at;
  }

  // ⚠️ THE BLOCK FOR `zh`/`en`, MAKING ONE ONLY IF IT IS NOT THERE YET -- which is what a named group needs and
  // is NOT what a feature's own block wants. Two monitors' brightness faders are one group, so the second must
  // find the block the first made; but two features that happen to give a control the same name are two
  // different blocks, so a feature's own block still calls AddSection. (The name is compared in BOTH languages:
  // a feature that sent only one of them would otherwise match itself twice with two different answers.)
  //
  // An empty name never matches: "no group" is not a group called "".
  int FindOrAddSection(const char *zh, const char *en)
  {
    if (!zh || !zh[0])
      return -1;
    for (int i = 0; i < sectionCount; ++i)
      if (strcmp(sections[i].titleZh, zh) == 0 &&
          strcmp(sections[i].titleEn, en ? en : "") == 0)
        return i;
    return AddSection(zh, en);
  }

  // A row in a block. The caller fills in the rest of the fields; these are the ones every kind has.
  Item *AddItem(int section, RowKind kind, const char *id)
  {
    if (itemCount >= kMaxItems || section < 0 || section >= sectionCount)
      return nullptr;
    Item *it = &items[itemCount++];
    *it = Item();
    it->section = section;
    it->kind = kind;
    CopyStr(it->id, kIdLen, id);
    return it;
  }
};

// ---- a label too long for its row: how far it has scrolled, at time `hoverMs` -------------------
//
// ⚠️ WHAT THIS IS FOR, IN THE USER'S WORDS: "快速面板显示和音量的设备名太长，鼠标移上去，可以滚动设备名". A
// monitor's name ("SDC4190 2880x1800") and an application's ("_mediacontrol_probe") are both long, and the flyout
// is 320 px wide with a fader and a read-out beside them -- so a label that does not fit is the NORMAL case for
// these rows, not an edge case, and an ellipsis would hide exactly the part that tells two similar devices apart.
//
// THE SHAPE: hold at the start (a hover is not a twitch -- the first thing the user wants is to READ it), slide
// to the end, hold there, slide back. Ping-pong rather than a wrap-around loop, because the beginning of a name
// is what identifies it, and a label that jumped from its tail back to its head would be unreadable at exactly
// the moment the user looks back at it.
//
// ⚠️ IT IS A PURE FUNCTION OF (widths, time), AND THAT IS THE POINT: the flyout's own timer drives it, but the
// arithmetic is checkable with no window and no clock (see `check_apex_quickpanel.sh`). Everything here is
// milliseconds and pixels, and nothing here knows what a monitor is.
inline int LabelScrollOffset(int textW, int boxW, int hoverMs)
{
  const int overflow = textW - boxW;
  if (overflow <= 0 || hoverMs <= 0)
    return 0; // it fits: nothing moves, ever
  const int holdMs = 700;  // pause at each end -- long enough to read the part that is showing
  const int pxPerSec = 40; // slow: this is a label being read, not a stock ticker
  int travelMs = (int)((double)overflow * 1000.0 / (double)pxPerSec);
  if (travelMs < 1)
    travelMs = 1;
  const int cycle = 2 * (holdMs + travelMs);
  int t = hoverMs % cycle;
  if (t < holdMs)
    return 0;
  t -= holdMs;
  if (t < travelMs)
    return (int)((double)overflow * (double)t / (double)travelMs);
  t -= travelMs;
  if (t < holdMs)
    return overflow;
  t -= holdMs;
  return overflow - (int)((double)overflow * (double)t / (double)travelMs);
}

// ---- the geometry ------------------------------------------------------------------------------
//
// EVERY LENGTH IS A PARAMETER, so a scaled display is a different Metrics rather than a different layout:
// the host passes sizes in physical pixels worked out from the window's DPI, and this file never asks what
// the DPI is (that question needs Windows).
struct Metrics
{
  // ⚠️⚠️ 320 -> 384, AND THIS TIME THE WIDTH IS FORCED BY THE FADERS RATHER THAN BY THE GRID. 320 was the
  // smallest panel that still fitted the compact grid two switches wide (see the note in DefaultMetrics). The
  // user's next request was about the CONTROLS: "设备或应用名宽度5个汉字就够；推子适当加宽，让它好操作" plus "音量在推子
  // 右边增加静音按钮". A name capped at five characters, a fader that is genuinely wider than it was, and a button
  // beside it do not fit in 320 -- the arithmetic gives a ~90 px track, which is NARROWER than the 120 it had.
  // So the panel grows once, by 64, and the extra width goes where the user asked for it: the fader.
  int width = 384;      // the panel's outer width
  int pad = 14;         // between the panel's edge and the content
  // ⚠️ THE AIR BETWEEN TWO PANES, AND THE USER HAS NOW MOVED IT TWICE. It was 16 after they asked for the blocks
  // to float on their own ("现在看上去是大面板装着小面板，大面板和小面板中间的间隙太大了"), and it is **3** now:
  // "快速面板各分组间距拉小，留三个像素就够".
  //
  // ⚠️⚠️ AND AT 3 THE PANES LOSE THEIR SHADOW, WHICH IS ARITHMETIC RATHER THAN A STYLE CHOICE. The shadow may not
  // reach the middle of the seam -- at 8 px of reach in a 12 px gap the two shadows met in the middle and the
  // blocks read as one surface again, which is the "大板" the user asked to be rid of twice (see
  // DrawCardShadow in quickpaint.h and the pixel gate that catches it). Three pixels leave 1 px on each side of
  // the seam, so the reach has to be zero. What still separates two blocks is the hairline border, the highlight
  // along each pane's top edge, and the 3 px of desktop showing through between them.
  int sectionGap = 3;   // between two features' blocks
  int sectionPad = 10;  // between a block's edge and its rows
  int sectionHead = 22; // the feature's name line at the top of its block
  int rowH = 34;        // a label-and-toggle line, and one grid cell of the switch grid
  int sliderRowH = 34;  // a label, a track and a read-out line
  int knobRowH = 46;    // a label, a read-out and a dial line

  // ⚠️ THE SWITCH IS THE SETTINGS PAGE'S SWITCH, DOWN TO THE PIXEL: "快速面板的开关做小点，跟设置面板一样" (the
  // user). `panel.css`'s `.sw` is **26x15 with an 11-px knob**, and this was 38x21 -- noticeably chunkier beside the
  // same control on the page the user sets it from, which is the one place the two are seen together.
  //
  // ⚠️ AND EVERYTHING THE SWITCH DRAWS IS DERIVED FROM THIS RECTANGLE (see DrawSwitch: track radius = h/2, knob =
  // h-4, 2 px inset), so the knob comes out 11 px and the two controls are the same control -- a second size
  // written down anywhere would be a second thing to keep in step.
  int switchW = 26; // the toggle's own size
  int switchH = 15;
  int knobD = 34;      // the knob's diameter (the row is taller, to leave air around it)
  // ⚠️⚠️ THE LABEL COLUMN IS A FIXED WIDTH NOW, AND THE NUMBER IS THE USER'S OWN: "设备或应用名宽度5个汉字就够".
  // It used to be whatever the row had left after a FIXED track, which came to ~82 px -- about six characters, and
  // it still drifted with the panel's width. Fixed at five characters (13 px each, the size the label is drawn in
  // -- see PaintPanel), the column is the same in every row, two faders line up whatever their labels are, and the
  // width the labels no longer take is width the faders get. A name that does not fit SCROLLS while the pointer
  // rests on it (LabelScrollOffset below), which is why five characters is enough rather than a limitation.
  int labelW = 66;
  int trackW = 120;     // the fader's track -- FIXED, so two faders in a column line up
  int trackH = 8;       // the fader's groove; the thumb is drawn `trackH + 6` across (see DrawFader)
  int valueW = 54;      // the numeric read-out at the right of a slider or knob row
  int labelGap = 8;     // between a label and whatever follows it
  // ⚠️ THE COMPANION SWITCH'S OWN SIZE (see `Item::toggleId`): a BUTTON, not a switch, because it shares its row
  // with a fader -- a 38-px track-and-knob switch beside a fader reads as a second thing to drag.
  int extraW = 26;
  int extraH = 20;
  int extraGap = 6;     // between the read-out and the button

  int switchColMin = 128; // a grid cell narrower than this is not worth two columns
};

// ⚠️ HOW BIG A ROW'S COMPANION IS, WHICH DEPENDS ON WHAT IT IS DRAWN AS (the user's correction: "是要用一样的滑动
// 开关"): on a SWITCH row it is the same switch as the one beside it, and on a fader row it is the compact button
// (a track-and-knob switch next to a fader would read as a second thing to drag -- see quickpaint.h). One pair of
// functions rather than the same ternary in the layout and in the painter: a painter that worked out its own idea
// of the size would draw a switch in a box measured for a button.
inline int CompanionW(const Metrics &mx, RowKind kind)
{
  return kind == RowKind::kToggle ? mx.switchW : mx.extraW;
}
inline int CompanionH(const Metrics &mx, RowKind kind)
{
  return kind == RowKind::kToggle ? mx.switchH : mx.extraH;
}

// ---------------------------------------------------------------------------
// ⚠️⚠️ THE KEY OF THE BLOCK (PANE) AN ITEM BELONGS TO -- AND IT IS A *TRANSPORT* STRING AS WELL AS AN IDENTITY.
//
// The General page lists the panes and lets the user drag them; what comes back is the keys, in their new order
// (see `quickOrder` in hostconfig.h). That travel is what shapes this function:
//
//   * THE SEPARATOR IS A COMMA, so a key may not contain one: a comma inside a key would split it into two on the
//     way back, and the pane it names would lose its place in silence;
//   * AND IT MAY NOT CONTAIN A NEWLINE, WHICH IS WORSE: the request itself is `key=value` LINES (see ParseFields
//     in settings_host.cpp), so a value carrying a newline arrives TRUNCATED AT ITS FIRST LINE. That is not a
//     theory -- it is the bug the user reported as "快速面板分组不能调顺序": the page joined the keys with
//     newlines, the host received exactly one of them, and a drag could move at most the first row.
//   * (AND THE VALUE HAS A FINITE SIZE, which is why the order is a list of KEYS and not of whole descriptions --
//     see the buffers in ui_webview.cpp and Fields in settings_host.cpp, both now sized for it.)
//
// So both characters become a space. ⚠️ THE NAME IS NOT TOUCHED ANYWHERE ELSE: the flyout prints a pane's heading
// from the item itself, never from this key, so a group called 「亮度, 主屏」 still reads exactly like that on
// screen -- only its key says 「亮度  主屏」.
//
// ⚠️ AND IT LIVES IN THIS HEADER, THE PURE MODEL ONE, so `check_apex_quickpanel.sh` can exercise it with no host,
// no window and no GDI+: the sanitising, the feature prefix, and that two different panes cannot collide.
// ---------------------------------------------------------------------------
inline void QuickBlockKey(const char *featureId, const char *groupName, const char *itemId, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  const char *parts[2] = {featureId ? featureId : "",
                          (groupName && groupName[0]) ? groupName : (itemId ? itemId : "")};
  int n = 0;
  for (int p = 0; p < 2; ++p)
  {
    if (p && n < outSize - 1)
      out[n++] = '|';
    for (const char *s = parts[p]; *s && n < outSize - 1; ++s)
      out[n++] = (*s == ',' || *s == '\n' || *s == '\r') ? ' ' : *s;
    out[n] = 0;
  }
}

// A physical-pixel metric set for a scale factor of `scale`/100. The base numbers above are already device
// pixels at 100%; the host passes the monitor's scale, so 150% is half again as wide rather than a second
// hard-coded table.
inline int Scaled(int base, int scalePercent)
{
  if (scalePercent <= 0)
    scalePercent = 100;
  return (int)(base * scalePercent / 100.0 + 0.5);
}

inline Metrics DefaultMetrics(int scalePercent)
{
  Metrics m;
  m.width = Scaled(384, scalePercent);
  m.pad = Scaled(14, scalePercent);
  m.sectionGap = Scaled(3, scalePercent);
  m.sectionPad = Scaled(10, scalePercent);
  m.sectionHead = Scaled(22, scalePercent);
  m.rowH = Scaled(34, scalePercent);
  m.sliderRowH = Scaled(34, scalePercent);
  m.knobRowH = Scaled(46, scalePercent);
  m.switchW = Scaled(26, scalePercent); // the settings page's `.sw` (panel.css) -- see the note on the base value
  m.switchH = Scaled(15, scalePercent);
  m.knobD = Scaled(34, scalePercent);
  m.labelW = Scaled(66, scalePercent);
  m.trackW = Scaled(120, scalePercent);
  m.trackH = Scaled(8, scalePercent);
  m.valueW = Scaled(54, scalePercent);
  m.labelGap = Scaled(8, scalePercent);
  m.extraW = Scaled(26, scalePercent);
  m.extraH = Scaled(20, scalePercent);
  m.extraGap = Scaled(6, scalePercent);
  m.switchColMin = Scaled(128, scalePercent);
  return m;
}

struct Rect
{
  int x = 0, y = 0, w = 0, h = 0;
  int Right() const { return x + w; }
  int Bottom() const { return y + h; }
  bool Holds(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Layout
{
  int width = 0, height = 0;
  Rect boxes[kMaxSections];  // each block's background
  Rect titles[kMaxSections]; // each block's title line
  bool sectionLive[kMaxSections] = {false}; // false when the block had no rows and is not drawn
  Rect rows[kMaxItems];      // the item's whole line -- THE HIT AREA, so a click anywhere on the line works
  Rect control[kMaxItems];   // the control itself: the switch, the track, or the dial
  Rect labels[kMaxItems];    // what the label may use before the control starts
  Rect values[kMaxItems];    // the read-out, right-aligned
  Rect extras[kMaxItems];    // the companion switch (abi.h: `toggleId`), empty when the row has none
  // ⚠️ WHERE A CLICK ON A ROW ACTUALLY ACTS, which is NOT the same as the row's own rectangle (see FaderBox):
  // the click target of a fader is the fader, and nothing else on the line.
  Rect faderHit[kMaxItems];
  Rect extraHit[kMaxItems];
};

// TURN THE MODEL INTO RECTANGLES. Returns the panel's height (0 for an empty model, in which case the caller
// shows nothing at all -- a flyout with a border and no content is worse than no flyout).
//
// ⚠️ THE HIT TEST READS THESE RECTANGLES AND NEVER RECOMPUTES ANY. Two layouts of the same model would differ
// by a pixel here and there, and the symptom -- "the switch is drawn one place and clickable in another" -- is
// the exact failure this project has already had once on a settings page (see panel.md, the pointer-events
// note: the picture moved and the hit test did not).
inline int MeasureModel(const Model &m, const Metrics &mx, Layout *out)
{
  if (!out)
    return 0;
  *out = Layout();
  out->width = mx.width;
  if (m.itemCount <= 0 || m.sectionCount <= 0)
    return 0;

  const int contentX = mx.pad;
  const int contentW = mx.width - 2 * mx.pad;
  if (contentW <= 0)
    return 0;

  // Rows are grouped by section, and a section's rows are laid out in the order they were added -- which is
  // the order the feature sent them, and therefore the order it wants them read in.
  int y = mx.pad;
  bool firstBox = true;
  for (int s = 0; s < m.sectionCount; ++s)
  {
    int first = -1, count = 0;
    int gridRows = 0, gridCols = 1;
    for (int i = 0; i < m.itemCount; ++i)
    {
      if (m.items[i].section != s)
        continue;
      if (first < 0)
        first = i;
      ++count;
    }
    if (count == 0)
      continue; // nothing in this feature: an empty block is dropped rather than drawn (see the header note)

    const int boxX = contentX;
    const int boxW = contentW;
    int innerX = boxX + mx.sectionPad;
    int innerW = boxW - 2 * mx.sectionPad;
    if (innerW < 40)
      innerW = 40;

    // ⚠️ THE SWITCH GRID: the host's own "all the features' switches" block is a GRID and everything else is a
    // column, and that is what "紧凑视图" means -- with a handful of features, one per line is a mostly-empty
    // panel that still scrolls off a small screen.
    const bool grid = (m.items[first].kind == RowKind::kFeatureSwitch);
    if (grid)
    {
      gridCols = (innerW >= 2 * mx.switchColMin) ? 2 : 1;
      gridRows = (count + gridCols - 1) / gridCols;
    }

    if (!firstBox)
      y += mx.sectionGap;
    firstBox = false;

    // ⚠️⚠️ A BLOCK WITH ONE ROW HAS NO TITLE LINE. THE USER'S RULE, AND IT IS ABOUT GROUPING RATHER THAN
    // SPACING: "快速面板局部功能分组逻辑不以插件分组，而是以一个开关为一组，后面做这个都会这么设定".
    //
    // So a feature's own controls are no longer collected into one block named after the FEATURE -- each
    // control is a block of its own, and the pane it floats in IS its grouping. Its label is therefore the
    // block's heading, and drawing a heading line as well would say the same thing twice in two sizes.
    //
    // ⚠️ AND A NAMED GROUP IS THE EXCEPTION, WHICH IS WHY THE TEST ALSO ASKS ABOUT THE GROUP (ABI 12 -> 13).
    // When the feature named the block, the heading is the FEATURE's word for the group and the row's label is
    // the name of one thing inside it ("亮度" over "内置屏" / "副屏") -- two different sentences, so the heading
    // stays even when there is one row. It is also what the user asked for by name: "分组名称为「亮度」".
    const bool single = (!grid && count == 1 && !m.items[first].groupZh[0]);
    const int headH = single ? 0 : mx.sectionHead;

    const int boxY = y;
    out->sectionLive[s] = true;
    out->boxes[s] = Rect{boxX, boxY, boxW, 0};
    // An empty rectangle is how "there is no heading here" is said; the painter skips an empty one (see
    // DrawTextIn), and the row below still carries the label.
    out->titles[s] = single ? Rect{0, 0, 0, 0} : Rect{innerX, boxY + mx.sectionPad, innerW, mx.sectionHead};

    int ry = boxY + mx.sectionPad + headH;
    const int colW = grid ? innerW / gridCols : innerW;
    int inCol = 0;

    // ⚠️⚠️ AND WHETHER THIS BLOCK HAS A COMPANION BUTTON *ANYWHERE* IN IT, DECIDED BEFORE ANY ROW IS LAID OUT. A
    // button takes its 32 px from the fader's side, so a block in which one row has one and the next does not would
    // have two faders of different lengths -- and "two faders in a column line up" is a rule this panel already
    // holds itself to (the read-out and the label are fixed for the same reason). Reserving the space for the whole
    // block answers both: every fader in the block ends at the same x, and a block with no button anywhere (the
    // brightness faders) is not made narrower by a control it never shows.
    //
    // ⚠️ A TOGGLE ROW COUNTS TOO (ABI 16 -> 17). The same alignment argument applies one row up: two switches in a
    // column line up only if the space their neighbours' buttons take is reserved for both. See the toggle branch
    // below, and the note on `toggleId` in abi.h for why a switch row has a companion at all.
    bool blockExtra = false;
    for (int i = first; i < first + count; ++i)
      if (m.items[i].toggleId[0] &&
          (m.items[i].kind == RowKind::kSlider || m.items[i].kind == RowKind::kKnob ||
           m.items[i].kind == RowKind::kToggle))
        blockExtra = true;

    for (int i = first; i < first + count; ++i)
    {
      const Item &it = m.items[i];
      const int rowH = (it.kind == RowKind::kKnob)      ? mx.knobRowH
                       : (it.kind == RowKind::kSlider)  ? mx.sliderRowH
                                                        : mx.rowH;
      const int rx = grid ? innerX + inCol * colW : innerX;
      const int rw = grid ? colW : innerW;
      out->rows[i] = Rect{rx, ry, rw, rowH};

      if (it.kind == RowKind::kFeatureSwitch)
      {
        // Name on the left, switch hard against the right of its cell -- "紧凑" is about the two columns, not
        // about squeezing the switch against the label.
        out->control[i] = Rect{rx + rw - mx.switchW - 2, ry + (rowH - mx.switchH) / 2, mx.switchW, mx.switchH};
        out->labels[i] = Rect{rx, ry, rw - mx.switchW - mx.labelGap - 2, rowH};
        out->values[i] = Rect{rx + rw, ry, 0, 0};
      }
      else if (it.kind == RowKind::kToggle)
      {
        // ⚠️ A SWITCH ROW MAY CARRY A COMPANION BUTTON TOO (ABI 16 -> 17; see `toggleId` in abi.h). KeepAwake's
        // rows ask one program two questions -- "keep the machine awake" / "keep the screen on" -- and the user
        // asked for the flyout to show both: "快速面板按列表显示系统和各应用的两个功能开关". So the row's own
        // switch is the first of the pair and the button at the right-hand end is the second.
        //
        // ⚠️⚠️ AND THE COMPANION ON A SWITCH ROW IS THE SAME SWITCH, NOT THE COMPACT BUTTON -- THE USER'S OWN
        // CORRECTION, AFTER SEEING THE BUTTON: "保持唤醒的快速面板…是要用一样的滑动开关". The button exists for a
        // FADER row, where a 38-px track-and-knob switch beside a fader would read as a second thing to drag; a
        // switch row has no such problem, and two identical switches read as the pair they are (which is exactly
        // what the settings page has drawn for this list all along). So the width reserved here is the SWITCH's,
        // and the painter draws a switch in it (see DrawSwitch/DrawRowButton in quickpaint.h).
        //
        // ⚠️ THE GEOMETRY IS THE FADER ROW'S, READ FROM THE OTHER END: the companion is carved out of the row's
        // right edge and the row's own switch takes the place left over. And the room is reserved BLOCK-WIDE
        // (`blockExtra`), so a row whose feature sent no companion still has its own switch in the same column as
        // its neighbours' -- the same "everything in a block lines up" rule the faders follow.
        const bool hasExtra = it.toggleId[0] != 0;
        const int cw = CompanionW(mx, it.kind), ch = CompanionH(mx, it.kind);
        const int extraX = rx + rw - cw;
        const int rightEdge = blockExtra ? (extraX - mx.extraGap) : (rx + rw);
        if (hasExtra)
        {
          out->extras[i] = Rect{extraX, ry + (rowH - ch) / 2, cw, ch};
          // A few pixels of air around it, the same courtesy the fader row's button gets: a switch is a small
          // thing to aim at, and the air is horizontal only (into space that is already empty beside the read-out).
          const int grow = 3;
          out->extraHit[i] = Rect{extraX - grow, ry, cw + 2 * grow, rowH};
        }
        out->control[i] = Rect{rightEdge - mx.switchW, ry + (rowH - mx.switchH) / 2, mx.switchW, mx.switchH};
        out->labels[i] = Rect{rx, ry, rightEdge - mx.switchW - mx.labelGap - rx, rowH};
        out->values[i] = Rect{rx + rw, ry, 0, 0};
      }
      else if (it.kind == RowKind::kNote)
      {
        // Nothing to click and nothing to read out: the whole line is text.
        out->control[i] = Rect{rx + rw, ry, 0, 0};
        out->labels[i] = Rect{rx, ry, rw, rowH};
        out->values[i] = Rect{rx + rw, ry, 0, 0};
      }
      else
      {
        // A LABEL, THEN THE CONTROL, THEN THE NUMBER -- and, when the row has one, the companion button at the
        // far right (abi.h: `toggleId`; the user's "音量在推子右边增加静音按钮").
        //
        // ⚠️ THE NUMBER SITS AT THE FAR RIGHT because it is what the user watches while dragging, so it must not
        // shift when the label's length changes -- and the label is a FIXED width for the same reason seen from the
        // other side: two faders in a column have to line up. (It used to be "the leftover after a fixed track",
        // which came to about six characters and still moved with the panel's width; the user's own number for it
        // is five characters -- see `labelW`.)
        //
        // ⚠️ THE COMPANION IS CARVED OUT OF THE READ-OUT'S SIDE, NOT OUT OF THE FADER: the fader is what the user
        // asked to widen, so the button takes its 32 px from the space the read-out's own box has spare (it is
        // 54 px wide for a number that is at most "100 %", and it stays right-aligned inside what is left).
        const int gap = mx.labelGap;
        // The companion is only for a RANGE row (a switch row has its own switch already): abi.h says so, and
        // honouring it here is what keeps one rule in one place. The SPACE for it is reserved block-wide (see
        // `blockExtra`), while the button itself is placed only on the rows whose feature sent one.
        const bool isRange = (it.kind == RowKind::kSlider || it.kind == RowKind::kKnob);
        const bool hasExtra = it.toggleId[0] && isRange;
        const bool reserve = blockExtra && isRange;
        const int extraX = rx + rw - mx.extraW;
        // Everything to the left of the button (or the whole row when there is none) is what the rest shares.
        const int rightEdge = reserve ? (extraX - mx.extraGap) : (rx + rw);
        if (hasExtra)
        {
          out->extras[i] = Rect{extraX, ry + (rowH - mx.extraH) / 2, mx.extraW, mx.extraH};
          // ⚠️ THE BUTTON'S CLICK TARGET IS THE BUTTON PLUS A LITTLE AIR, because 26x20 is a small thing to aim
          // at -- and the air it takes is horizontal only, into the gap that is already empty (the read-out is
          // right-aligned in its own box), so it can never overlap the fader's own target.
          const int grow = 3;
          out->extraHit[i] = Rect{extraX - grow, ry, mx.extraW + 2 * grow, rowH};
        }
        if (it.kind == RowKind::kKnob)
        {
          // A dial is small and square, so the label keeps everything to the left of it and the read-out sits
          // just inside the dial rather than at the window's edge.
          out->control[i] = Rect{rightEdge - mx.knobD, ry + (rowH - mx.knobD) / 2, mx.knobD, mx.knobD};
          out->values[i] = Rect{rightEdge - mx.knobD - gap - mx.valueW, ry, mx.valueW, rowH};
          out->labels[i] = Rect{rx, ry, (out->values[i].x - gap) - rx, rowH};
        }
        else
        {
          // ⚠️ THE TRACK TAKES EVERYTHING THE FIXED LABEL AND THE FIXED READ-OUT DO NOT -- which is the whole
          // point of fixing the label: the width a shorter name frees up is width the fader gets, and two faders
          // in one block still have the same track. The read-out keeps its place at the right of what is left, and
          // only if the remainder is too small for both does the label give way (a truncated fader is still usable
          // and a truncated label is not -- but with a five-character label column that should never happen).
          int labelW = mx.labelW;
          int trackX = rx + labelW + gap;
          int trackW = (rightEdge - mx.valueW - gap) - trackX;
          if (trackW < 40)
          {
            labelW = (rightEdge - mx.valueW - gap - 40) - rx - gap;
            if (labelW < 0)
              labelW = 0;
            trackX = rx + labelW + gap;
            trackW = (rightEdge - mx.valueW - gap) - trackX;
            if (trackW < 8)
              trackW = 8;
          }
          out->labels[i] = Rect{rx, ry, labelW, rowH};
          out->control[i] = Rect{trackX, ry + (rowH - mx.trackH) / 2, trackW, mx.trackH};
          out->values[i] = Rect{rightEdge - mx.valueW, ry, mx.valueW, rowH};
        }
      }
      // ⚠️ AND THE FADER'S OWN CLICK TARGET IS COMPUTED HERE, FROM THE TRACK, IN THE SAME PASS. The user's report
      // is why it exists: "鼠标点击改变数值只在推子内，现在点到推子左右区域都会改变" -- a click to the left or right of
      // the fader used to move it, which is a value the user did not ask for and has to put back by hand. The
      // target is the groove PLUS the thumb's overhang at each end (the thumb is drawn `trackH + 6` across, so
      // half of it hangs past the track -- see DrawFader) and the WHOLE ROW VERTICALLY, because an 8-px groove is
      // not something to aim at and the row above and below it belong to nothing else.
      if (it.kind == RowKind::kSlider)
      {
        const int half = (mx.trackH + 6 + 1) / 2;
        out->faderHit[i] = Rect{out->control[i].x - half, ry, out->control[i].w + 2 * half, rowH};
      }
      else if (it.kind == RowKind::kKnob)
      {
        // A dial is the target itself, with the same few pixels of air.
        const int grow = 2;
        out->faderHit[i] = Rect{out->control[i].x - grow, out->control[i].y - grow, out->control[i].w + 2 * grow,
                                out->control[i].h + 2 * grow};
      }

      if (grid)
      {
        if (++inCol >= gridCols)
        {
          inCol = 0;
          ry += rowH;
        }
      }
      else
        ry += rowH;
    }
    if (grid && inCol > 0)
      ry += mx.rowH;

    out->boxes[s].h = ry - boxY + mx.sectionPad;
    y = out->boxes[s].Bottom();
  }

  out->height = y + mx.pad;
  return out->height;
}

// WHICH ROW A POINT IS ON, or -1. The whole line is the target, not just the control: a 21-pixel switch is a
// small thing to hit, and the row it is on is unambiguous (one label, one control). A block's title line and
// the gaps between rows answer -1, so a click there does nothing rather than toggling whatever was nearest --
// and a note is not a target at all, which is why it is skipped here rather than left to the caller.
inline int HitTest(const Model &m, const Layout &ly, int x, int y)
{
  for (int i = 0; i < m.itemCount; ++i)
  {
    if (m.items[i].kind == RowKind::kNote)
      continue;
    if (ly.rows[i].Holds(x, y))
      return i;
  }
  return -1;
}

// ---- what a CLICK does, which is not the same question as "which row is this" -------------------
//
// ⚠️⚠️ THE USER'S OWN REPORT IS THE REASON THIS EXISTS: "鼠标点击改变数值只在推子内，现在点到推子左右区域都会改变".
// The row used to be the click target for every kind, which is right for a switch (a 21-px switch is a small thing
// to aim at, and the line it is on is unambiguous) and wrong for a fader: a fader is ABSOLUTE -- where you click IS
// the value -- so a click that merely landed on the same line as the fader silently moved the value, and the user
// had to put it back by hand. So "the row I am pointing at" (hover, and a switch's own target) and "the control I
// am operating" are two questions, and the second one is answered by the control's own rectangle.
//
// ⚠️ ONE DECISION, ONE PLACE, exactly like the layout: the window's mouse handling asks THIS, and so does the
// probe -- a click that the painter drew one way and this answers another is the failure this file exists to
// prevent.
enum class HitKind
{
  kNone = 0, // the point is on a row, but not on anything that acts
  kToggle,   // a switch: the row's own target
  kExtra,    // the companion switch (abi.h: `toggleId`)
  kSlider,   // the fader's own rectangle: an absolute jump, and the start of a drag
  kKnob      // the dial: the start of a relative drag
};

struct Hit
{
  HitKind kind = HitKind::kNone;
  int index = -1;
};

inline Hit ClickAt(const Model &m, const Layout &ly, int x, int y)
{
  Hit h;
  const int i = HitTest(m, ly, x, y);
  if (i < 0)
    return h; // a gap, a block's title, the panel's padding: nothing at all
  h.index = i;
  const Item &it = m.items[i];

  // The companion first: it is inside the row and to the right of everything else, and a click there must not be
  // read as a fader jump (they do not overlap, but the order says which answer wins if a layout ever makes them).
  // ⚠️ IT IS ASKED BEFORE THE SWITCH ROW'S BLANKET ANSWER BELOW, because a switch row can now HAVE one (ABI 16 ->
  // 17): without this order the companion would be unreachable -- the whole line would answer "toggle the row's own
  // switch" and the button drawn at its right would do the other thing instead.
  if (it.toggleId[0] && ly.extraHit[i].Holds(x, y))
  {
    h.kind = HitKind::kExtra;
    return h;
  }
  if (it.kind == RowKind::kFeatureSwitch || it.kind == RowKind::kToggle)
  {
    h.kind = HitKind::kToggle; // the whole line, as before: one label, one switch, no ambiguity
    return h;
  }
  if (it.kind == RowKind::kSlider && ly.faderHit[i].Holds(x, y))
    h.kind = HitKind::kSlider;
  else if (it.kind == RowKind::kKnob && ly.faderHit[i].Holds(x, y))
    h.kind = HitKind::kKnob;
  return h;
}

// WHICH FADER A WHEEL TURNS, or -1. ⚠️ THE SAME TARGET AS A CLICK, for the same reason: the user's complaint was
// about the value changing from beside the fader, and a wheel is no more welcome there than a click is -- "推子可以
// 用鼠标滚轮操作" is about the fader, not about the row it happens to be on.
inline int WheelTarget(const Model &m, const Layout &ly, int x, int y)
{
  const Hit h = ClickAt(m, ly, x, y);
  return (h.kind == HitKind::kSlider || h.kind == HitKind::kKnob) ? h.index : -1;
}

// ---- values ------------------------------------------------------------------------------------
//
// ⚠️ SNAPPING AND CLAMPING HAPPEN HERE, BEFORE THE VALUE IS EVER SENT, AND THE FEATURE GETS THE LAST WORD
// ANYWAY. The host does not guess what a feature will accept -- it sends the value and then REBUILDS the panel
// from whatever the feature reports (`quickItems`), so a feature that clamps harder than this shows its own
// clamp on the very next frame. What this pass is for is the ordinary case: a step of 5 that would otherwise
// produce 199.99999999999997, and a drag past the end of a track that should stop at the end.
inline double SnapToStep(const Item &it, double v)
{
  if (v < it.min)
    v = it.min;
  if (v > it.max)
    v = it.max;
  const double step = it.step;
  if (step > 0.0)
  {
    // Rounded to the nearest step FROM THE MINIMUM, so a range that starts at 100 with a step of 5 has 100,
    // 105, ... rather than "the multiples of 5 that happen to be inside it".
    const double n = std::floor((v - it.min) / step + 0.5);
    v = it.min + n * step;
    if (v > it.max)
      v = it.max;
    if (v < it.min)
      v = it.min;
  }
  return v;
}

inline double FractionFromValue(const Item &it)
{
  const double span = it.max - it.min;
  if (span <= 0.0)
    return 0.0;
  double f = (it.value - it.min) / span;
  return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

// The value a fader takes when the pointer is at `x` on `track`. A fader is ABSOLUTE: where the pointer is IS
// the value, which is what makes it feel like a fader rather than a scrollbar.
inline double SliderValueAt(const Item &it, const Rect &track, int x)
{
  const int span = track.w > 1 ? track.w - 1 : 1;
  double f = (double)(x - track.x) / (double)span;
  if (f < 0.0)
    f = 0.0;
  if (f > 1.0)
    f = 1.0;
  return SnapToStep(it, it.min + f * (it.max - it.min));
}

// ⚠️ A KNOB IS RELATIVE, AND THAT IS THE DIFFERENCE BETWEEN THE TWO SHAPES RATHER THAN A STYLE CHOICE. A dial
// has no "where the pointer is" -- the pointer starts somewhere on a circle that is only 34 pixels across, and
// an absolute mapping would make the first pixel of the drag jump the value to whatever angle it happened to
// land on. So a knob takes the value it had when the button went down and moves with the pointer's VERTICAL
// travel (up = more), which is the gesture every audio tool teaches.
inline double KnobValueAt(const Item &it, double startValue, int dyPixels, int rowHeight)
{
  const int travel = rowHeight * 4; // a full sweep is about four rows of drag
  if (travel <= 0)
    return SnapToStep(it, startValue);
  const double span = it.max - it.min;
  return SnapToStep(it, startValue - (double)dyPixels / (double)travel * span);
}

// How many decimals the read-out shows, from the item's own step: a step of 5 is "200", a step of 0.1 is
// "1.4", a step of 0.05 is "1.45". Anything finer than 0.01 is shown at 3, which is where a wheel-delta
// parameter lives.
inline int DecimalsFor(const Item &it)
{
  const double step = it.step > 0.0 ? it.step : 0.01;
  if (step >= 1.0)
    return 0;
  if (step >= 0.1)
    return 1;
  if (step >= 0.01)
    return 2;
  return 3;
}

// "200 ms", "1.4 d", "1.20 x" -- the number and its unit, in one buffer, because that is what is drawn.
inline void FormatValue(const Item &it, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  const int d = DecimalsFor(it);
  if (it.unit[0])
    _snprintf(out, outSize, "%.*f %s", d, it.value, it.unit);
  else
    _snprintf(out, outSize, "%.*f", d, it.value);
  out[outSize - 1] = 0;
}

// ---- the wheel over a fader ---------------------------------------------------------------------
//
// ⚠️⚠️ THE USER ASKED FOR THIS -- "推子可以用鼠标滚轮操作", and they added that it should hold for EVERY fader in the
// panel rather than for the one they were looking at. It is worth knowing why it was not already true, because a
// slider looks like a thing that takes a wheel: nothing in this window is a system control. The panel is a layered
// window drawn by hand, so a wheel over it arrives as a WM_MOUSEWHEEL and nothing at all happens unless this code
// is here. (The settings page has had the feature for longer -- see panel.js -- and the arithmetic below is THAT
// arithmetic, deliberately: the same wheel must not move the two surfaces by different amounts.)
//
// THE RULE: ONE `step` PER NOTCH, in the direction of the scroll, clamped and re-snapped exactly like a drag.
// A fine-grained wheel (a trackpad, or a high-resolution wheel) is ACCUMULATED in units of 120 -- Windows' own
// notch -- so a device that reports 3 deltas at a time moves at the same RATE rather than needing forty flicks.
inline double WheelValueAfter(const Item &it, int wheelDelta, int *accum)
{
  const double step = it.step > 0.0 ? it.step : 1.0;
  int a = accum ? *accum : 0;
  // ⚠️ THE SIGN: a POSITIVE wheel delta is away from the user, which is "more" everywhere else in Windows --
  // a page scrolls up, and a fader goes up.
  a += wheelDelta;
  if (accum)
    *accum = a;
  if (a < 120 && a > -120)
    return it.value; // not a whole notch yet: the accumulator keeps the remainder and nothing moves
  const int notches = a / 120;
  if (accum)
    *accum = a - notches * 120;
  return SnapToStep(it, it.value + notches * step);
}

// ---- the fade ----------------------------------------------------------------------------------
//
// ⚠️ ONE CURVE, ONE PLACE. The user asked for a 0.2-0.5 s fade in and out; both numbers and the easing live
// here so that "the panel fades" is one fact rather than an expression repeated in the painter and the timer.
//
// The value returned is the window's opacity, 0..1, for a fade that started `elapsedMs` ago. Easing out on the
// way in (fast, then settling) is what makes a flyout feel like it arrived rather than like it faded up; the
// way out is the mirror image, which is the same curve run backwards.
static const int kFadeInMs = 240;
static const int kFadeOutMs = 200;

inline double EaseOutCubic(double t)
{
  if (t <= 0.0)
    return 0.0;
  if (t >= 1.0)
    return 1.0;
  const double u = 1.0 - t;
  return 1.0 - u * u * u;
}

inline double FadeInAlpha(int elapsedMs)
{
  if (elapsedMs <= 0)
    return 0.0;
  if (elapsedMs >= kFadeInMs)
    return 1.0;
  return EaseOutCubic((double)elapsedMs / (double)kFadeInMs);
}

inline double FadeOutAlpha(int elapsedMs)
{
  if (elapsedMs <= 0)
    return 1.0;
  if (elapsedMs >= kFadeOutMs)
    return 0.0;
  return 1.0 - EaseOutCubic((double)elapsedMs / (double)kFadeOutMs);
}

// ---- where the panel goes ----------------------------------------------------------------------
//
// ABOVE THE TRAY ICON, INSIDE THE MONITOR'S WORK AREA. The anchor is the icon's rectangle as the shell reports
// it (Shell_NotifyIconGetRect), the work area is the monitor's, and the two rules are the obvious ones:
//
//   * horizontally it lines up with the icon and is pulled back inside the work area, so the panel is never
//     half off the side of the screen;
//   * vertically it sits `gap` above the icon -- and when the taskbar is at the TOP of the screen there is no
//     room there, in which case it flips to below the icon. The taskbar can be on any edge (and the user may
//     have moved it a minute ago), so "above" has to be a decision rather than an assumption.
inline void PlaceAbove(const Rect &anchor, const Rect &work, int panelW, int panelH, int gap, int *outX,
                       int *outY)
{
  int x = anchor.x + anchor.w / 2 - panelW / 2;
  if (x + panelW > work.Right())
    x = work.Right() - panelW;
  if (x < work.x)
    x = work.x;

  int y = anchor.y - gap - panelH;
  if (y < work.y)
    y = anchor.Bottom() + gap; // no room above: the taskbar is at the top, so go below it
  if (y + panelH > work.Bottom())
    y = work.Bottom() - panelH;
  if (y < work.y)
    y = work.y;

  if (outX)
    *outX = x;
  if (outY)
    *outY = y;
}

} // namespace quick
} // namespace apex

#endif // APEX_QUICKPANEL_H
