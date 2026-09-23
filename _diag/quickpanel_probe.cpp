// ---------------------------------------------------------------------------
// THE QUICK PANEL'S DECISIONS, AND WHAT THE FEATURES PUT IN IT.
//
// TWO HALVES, because the flyout can be wrong in two independent ways and neither shows up as a crash:
//
//   * THE ARITHMETIC (quickpanel.h): where each row goes, what a click hits, what a drag is worth, how long
//     the fade lasts, and where the panel lands. None of it needs Windows, so it is checked here directly
//     rather than by clicking at a window -- and the failure mode it guards is nasty: a layout and a hit test
//     that disagree by a few pixels look, from the outside, exactly like "the switch does nothing" (this
//     project has already had that bug once on a settings page; see panel.md).
//
//   * WHAT THE FEATURES ANSWER (ApexFeature::quickItems). The flyout is built from the DLLs' own answers, so
//     this loads the built DLLs exactly as the host does -- version and struct size checked first -- and reads
//     what they would put in the panel. A feature that sends a knob with no range, an empty id or a value
//     outside its own bounds would draw a control that lies.
//
//   * AND THE COLOURS, against the settings page. The flyout (quickpalette.h) and the page (panel.css) are two
//     surfaces the user sees minutes apart; this reads the stylesheet and fails when the two drift.
//
// Build: g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I apex -I common -o build/_quickpanel_probe.exe \
//            _diag/quickpanel_probe.cpp -luser32
// Run:   build/_quickpanel_probe.exe <repo root> <build/apex/Plugins>
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi.h"
#include "quickpalette.h"
#include "quickpanel.h"

using namespace apex::quick;

static int failures = 0;
static int checks = 0;

static void Check(bool ok, const char *what, const char *detail = "")
{
  ++checks;
  printf("  %-60s %s%s%s\n", what, ok ? "ok" : "FAIL", detail[0] ? "  " : "", detail);
  if (!ok)
    ++failures;
}

// ---------------------------------------------------------------------------
// 1. THE LAYOUT
// ---------------------------------------------------------------------------
static Model BuildSampleModel()
{
  Model m;
  const int grid = m.AddSection("\xe6\x8f\x92\xe4\xbb\xb6", "Features");
  const char *ids[3] = {"SmoothWheel", "KeepAwake", "AutoIME"};
  const char *names[3] = {"SmoothWheel", "KeepAwake", "AutoIME"};
  for (int i = 0; i < 3; ++i)
  {
    Item *it = m.AddItem(grid, RowKind::kFeatureSwitch, ids[i]);
    CopyStr(it->labelZh, kLabelLen, names[i]);
    CopyStr(it->labelEn, kLabelLen, names[i]);
    it->on = (i != 2);
  }
  // ⚠️ ONE CONTROL, ONE BLOCK -- the user's rule ("快速面板局部功能分组逻辑不以插件分组，而是以一个开关为一组").
  // Three controls, three blocks, and each of those blocks has NO heading line: the control's own label is the
  // heading, and the assertions further down hold the layout to that.
  {
    const int own = m.AddSection("\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92", "Keep awake");
    Item *it = m.AddItem(own, RowKind::kToggle, "rules[0].awake");
    CopyStr(it->labelZh, kLabelLen, "\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92");
    CopyStr(it->labelEn, kLabelLen, "Keep awake");
    it->on = true;
  }
  {
    const int own = m.AddSection("\xe6\xbb\x91\xe5\x8a\xa8\xe6\x97\xb6\xe9\x95\xbf", "Glide");
    Item *it = m.AddItem(own, RowKind::kSlider, "glide");
    CopyStr(it->labelZh, kLabelLen, "\xe6\xbb\x91\xe5\x8a\xa8\xe6\x97\xb6\xe9\x95\xbf");
    CopyStr(it->labelEn, kLabelLen, "Glide");
    CopyStr(it->unit, kUnitLen, "ms");
    it->min = 100;
    it->max = 300;
    it->step = 5;
    it->value = 200;
  }
  {
    const int own = m.AddSection("\xe6\x9c\x80\xe9\xab\x98\xe9\x80\x9f\xe5\xba\xa6", "Top speed");
    Item *it = m.AddItem(own, RowKind::kKnob, "top");
    CopyStr(it->labelEn, kLabelLen, "Top speed");
    CopyStr(it->unit, kUnitLen, "x");
    it->min = 1.0;
    it->max = 3.0;
    it->step = 0.05;
    it->value = 1.5;
  }
  // ⚠️ AND A FADER WITH A COMPANION SWITCH, BECAUSE THAT IS A SHAPE OF ROW NOW (abi.h: `toggleId`, ABI 14 -> 15).
  // It is the mute button on a volume fader -- "音量在推子右边增加静音按钮" -- and the sample has to contain one or
  // every assertion about that rectangle would be about a rectangle that is always empty. It is also given a LONG
  // label, because a device name is exactly what the user said five characters is enough for.
  {
    const int vol = m.AddSection("\xe9\x9f\xb3\xe9\x87\x8f", "Volume");
    Item *a = m.AddItem(vol, RowKind::kSlider, "sessions[0].volume");
    CopyStr(a->labelZh, kLabelLen, "SDC4190 2880x1800");
    CopyStr(a->labelEn, kLabelLen, "SDC4190 2880x1800");
    CopyStr(a->groupZh, kGroupLen, "\xe9\x9f\xb3\xe9\x87\x8f");
    CopyStr(a->groupEn, kGroupLen, "Volume");
    CopyStr(a->unit, kUnitLen, "%");
    a->min = 0;
    a->max = 100;
    a->step = 1;
    a->value = 75;
    CopyStr(a->toggleId, kIdLen, "sessions[0].mute");
    a->toggleOn = true;
    a->icon = CompanionIcon::kMute;
    Item *b = m.AddItem(vol, RowKind::kSlider, "sessions[1].volume");
    CopyStr(b->labelZh, kLabelLen, "\xe7\xb3\xbb\xe7\xbb\x9f\xe5\xa3\xb0\xe9\x9f\xb3");
    CopyStr(b->labelEn, kLabelLen, "System sounds");
    CopyStr(b->groupZh, kGroupLen, "\xe9\x9f\xb3\xe9\x87\x8f");
    CopyStr(b->groupEn, kGroupLen, "Volume");
    CopyStr(b->unit, kUnitLen, "%");
    b->min = 0;
    b->max = 100;
    b->step = 1;
    b->value = 40;
  }
  // ⚠️ AND A SWITCH ROW WITH A COMPANION BUTTON, WHICH IS THE SHAPE THE USER ASKED FOR THE NEXT DAY (ABI 16 -> 17):
  // "快速面板按列表显示系统和各应用的两个功能开关". One program per row, its own switch for 保持唤醒 and the screen
  // button beside it -- and the sample holds TWO rows on purpose, only the first with a companion, so the
  // "everything in a block lines up" rule is checked where it can actually fail (items 8 and 9).
  {
    const int list = m.AddSection("\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92", "Keep awake");
    Item *a = m.AddItem(list, RowKind::kToggle, "rules[0].awake");
    CopyStr(a->labelZh, kLabelLen, "\xe7\xb3\xbb\xe7\xbb\x9f\xe5\x85\xa8\xe5\xb1\x80"); // 系统全局
    CopyStr(a->labelEn, kLabelLen, "System-wide");
    CopyStr(a->groupZh, kGroupLen, "\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92");
    CopyStr(a->groupEn, kGroupLen, "Keep awake");
    a->on = true;
    CopyStr(a->toggleId, kIdLen, "rules[0].display");
    a->toggleOn = false;
    a->icon = CompanionIcon::kDisplay;
    Item *b2 = m.AddItem(list, RowKind::kToggle, "rules[1].awake");
    CopyStr(b2->labelZh, kLabelLen, "probe-list.exe");
    CopyStr(b2->labelEn, kLabelLen, "probe-list.exe");
    CopyStr(b2->groupZh, kGroupLen, "\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92");
    CopyStr(b2->groupEn, kGroupLen, "Keep awake");
  }
  return m;
}

static bool Overlaps(const Rect &a, const Rect &b)
{
  return a.x < b.Right() && b.x < a.Right() && a.y < b.Bottom() && b.y < a.Bottom();
}

static void TestLayout()
{
  printf("\n== 1. the layout: one place computes it, everything else reads it ==\n");
  Model m = BuildSampleModel();
  Metrics mx = DefaultMetrics(100);
  Layout ly;
  const int h = MeasureModel(m, mx, &ly);

  Check(h > 0 && ly.width == mx.width, "MeasureModel answers a size");
  Check(ly.height > 0, "the panel has a height");

  // Every row inside the body, and inside its own block.
  bool inside = true;
  for (int i = 0; i < m.itemCount; ++i)
  {
    if (ly.rows[i].x < 0 || ly.rows[i].y < 0 || ly.rows[i].Right() > ly.width || ly.rows[i].Bottom() > ly.height)
      inside = false;
    const Rect &b = ly.boxes[m.items[i].section];
    if (ly.rows[i].x < b.x || ly.rows[i].Right() > b.Right() || ly.rows[i].y < b.y ||
        ly.rows[i].Bottom() > b.Bottom())
      inside = false;
  }
  Check(inside, "every row is inside its own block and inside the panel");

  // No two rows share a pixel. A pair that did would be two controls drawn on top of each other, and only one
  // of them would ever be hit.
  bool overlap = false;
  for (int i = 0; i < m.itemCount; ++i)
    for (int j = i + 1; j < m.itemCount; ++j)
      if (Overlaps(ly.rows[i], ly.rows[j]))
        overlap = true;
  Check(!overlap, "no two rows overlap");

  // THE HIT TEST AND THE PAINTER MUST AGREE, which is what the whole "one layout" rule is for: clicking the
  // centre of a row has to answer that row.
  bool hits = true;
  for (int i = 0; i < m.itemCount; ++i)
  {
    const Rect &r = ly.rows[i];
    if (HitTest(m, ly, r.x + r.w / 2, r.y + r.h / 2) != i)
      hits = false;
    // and the corners, which is where an off-by-one would show
    if (HitTest(m, ly, r.x, r.y) != i || HitTest(m, ly, r.Right() - 1, r.Bottom() - 1) != i)
      hits = false;
  }
  Check(hits, "the hit test agrees with the layout, centre and both corners");

  // ⚠️⚠️ ONE CONTROL, ONE BLOCK, AND NO HEADING LINE INSIDE IT -- the user's rule, checked as a fact about the
  // layout rather than as a comment. "快速面板局部功能分组逻辑不以插件分组，而是以一个开关为一组，后面做这个都会
  // 这么设定": a feature's controls are not collected under the feature's name, so each of them is a block of its
  // own -- and such a block is ONE LINE, because a heading above the row would print the same words twice (the
  // block's title and the row's label are the same string by construction; see Rebuild).
  for (int s = 1; s < m.sectionCount; ++s)
  {
    int only = -1, n = 0;
    for (int i = 0; i < m.itemCount; ++i)
      if (m.items[i].section == s)
      {
        if (n == 0)
          only = i;
        ++n;
      }
    // ⚠️ A NAMED GROUP WITH SEVERAL ROWS IS THE OTHER HALF OF THE SAME RULE, so it is skipped here and asserted
    // below: the "no heading" rule is about a block with ONE control AND no group name (see `single` in
    // MeasureModel).
    if (n != 1)
      continue;
    const int wantH = 2 * mx.sectionPad + ly.rows[only].h;
    char what[160], detail[128];
    _snprintf(what, sizeof(what), "block %d is ONE control: no heading line, exactly one row tall", s);
    _snprintf(detail, sizeof(detail), "(items=%d titles.w=%d box.h=%d want %d)", n, ly.titles[s].w,
              ly.boxes[s].h, wantH);
    Check(ly.titles[s].w == 0 && ly.boxes[s].h == wantH, what, detail);
  }
  // ... and the feature-switch grid KEEPS its heading, because that block is the host's own list of features and
  // its title is the only thing saying so.
  Check(ly.titles[0].w > 0, "the feature-switch grid keeps its heading line");

  // THE COMPACT GRID: the feature switches are two to a line, so the first two share a y and differ in x.
  Check(ly.rows[0].y == ly.rows[1].y && ly.rows[0].x != ly.rows[1].x,
        "'compact' means two feature switches per line");
  Check(ly.rows[2].y > ly.rows[0].y, "the third feature switch wraps to the next line");

  // The control sits inside its row, and the read-out is to the right of the control.
  const int slider = 4; // glide
  Check(ly.control[slider].x >= ly.rows[slider].x && ly.control[slider].Right() <= ly.rows[slider].Right(),
        "the fader's track is inside its row");
  Check(ly.values[slider].Right() <= ly.rows[slider].Right(), "the read-out is inside its row");

  // The knob is square and inside its row.
  const int knob = 5;
  Check(ly.control[knob].w == ly.control[knob].h && ly.control[knob].h == mx.knobD, "the knob is a square");
  Check(ly.control[knob].Bottom() <= ly.rows[knob].Bottom(), "the knob is inside its row");

  // A gap or a title is NOT a target: a click there must do nothing rather than toggle a neighbour.
  Check(HitTest(m, ly, ly.width - 1, 1) < 0, "a click on the panel's own padding hits nothing");
  Check(HitTest(m, ly, ly.titles[0].x + 2, ly.titles[0].y + 2) < 0, "a click on a block's title hits nothing");

  // ---------------------------------------------------------------------------------------------
  // ⚠️⚠️ THE FOUR THINGS THE USER ASKED FOR IN THIS ROUND, AS ARITHMETIC (2026-09-23):
  //
  //   "设备或应用名宽度5个汉字就够；推子适当加宽，让它好操作。鼠标点击改变数值只在推子内，现在点到推子左右区域都会改变；
  //    推子可以用鼠标滚轮操作；这条追加适配到快速面板所有推子的操作；音量在推子右边增加静音按钮."
  //
  // Every one of them is a fact about the layout or about `ClickAt`, so every one of them can be checked here --
  // with no window, no mouse and no screen. The two volume rows (6 and 7) are the interesting ones: one has a
  // companion button and one does not, they are in the SAME block, and their tracks must still line up.
  {
    const int vol0 = 6, vol1 = 7;
    char detail[160] = {0};

    // (1) THE LABEL COLUMN IS THE USER'S FIVE CHARACTERS, AND THE SAME IN EVERY FADER ROW.
    _snprintf(detail, sizeof(detail), "labelW=%d (want %d) track=%d/%d (was 120)", ly.labels[slider].w, mx.labelW,
              ly.control[slider].w, ly.control[vol0].w);
    Check(ly.labels[slider].w == mx.labelW && ly.labels[vol0].w == mx.labelW && ly.labels[vol1].w == mx.labelW,
          "the name column is five characters wide, in every fader row", detail);

    // (2) AND THE FADER IS WIDER THAN IT WAS: it used to be a fixed 120.
    Check(ly.control[slider].w >= 160 && ly.control[vol0].w >= 160,
          "the fader is wider than the fixed 120 it used to be", detail);
    // ⚠️ AND THE BUTTON'S SPACE IS RESERVED FOR ITS WHOLE BLOCK, so a group where some rows have one and some do
    // not still has every fader ending at the same x (the row WITHOUT a button keeps the space empty).
    Check(ly.control[vol0].x == ly.control[vol1].x && ly.control[vol0].w == ly.control[vol1].w,
          "two faders in one block still line up, button or no button", detail);

    // (3) THE COMPANION BUTTON: only where the feature sent one, to the RIGHT of the fader, clear of the read-out,
    // and inside its own row.
    _snprintf(detail, sizeof(detail), "extra=%d,%d %dx%d", ly.extras[vol0].x, ly.extras[vol0].y, ly.extras[vol0].w,
              ly.extras[vol0].h);
    Check(ly.extras[vol0].w > 0 && ly.extras[vol1].w == 0,
          "only the row whose feature sent a companion has a button", detail);
    Check(ly.extras[vol0].x >= ly.control[vol0].Right() && ly.extras[vol0].x >= ly.values[vol0].Right() &&
              ly.extras[vol0].Right() <= ly.rows[vol0].Right() &&
              ly.extras[vol0].Bottom() <= ly.rows[vol0].Bottom(),
          "the button is right of the fader and of the read-out, and inside the row", detail);

    // (4) A CLICK CHANGES THE VALUE ONLY INSIDE THE FADER -- the user's report was "点到推子左右区域都会改变".
    const Rect &fh = ly.faderHit[vol0];
    Check(ClickAt(m, ly, fh.x + fh.w / 2, fh.y + fh.h / 2).kind == HitKind::kSlider,
          "a click ON the fader is a fader jump", "");
    Check(ClickAt(m, ly, ly.labels[vol0].x + 4, ly.labels[vol0].y + ly.labels[vol0].h / 2).kind == HitKind::kNone,
          "a click on the NAME does not touch the fader", "");
    Check(ClickAt(m, ly, ly.values[vol0].x + ly.values[vol0].w / 2, ly.values[vol0].y + ly.values[vol0].h / 2).kind ==
              HitKind::kNone,
          "a click on the READ-OUT does not touch the fader", "");
    Check(ClickAt(m, ly, ly.rows[vol0].x + 1, ly.rows[vol0].y + ly.rows[vol0].h / 2).kind == HitKind::kNone,
          "a click at the row's left edge does nothing at all", "");
    Check(ClickAt(m, ly, ly.extras[vol0].x + ly.extras[vol0].w / 2,
                  ly.extras[vol0].y + ly.extras[vol0].h / 2).kind == HitKind::kExtra,
          "a click on the companion is the companion", "");
    // ⚠️ ... AND THE FADER'S TARGET REALLY IS A BOX AROUND THE FADER: it reaches the track's ends (so the thumb at
    // 0% and at 100% is inside it) and no further.
    Check(fh.x <= ly.control[vol0].x && fh.Right() >= ly.control[vol0].Right(),
          "the fader's target covers its whole track", "");
    Check(fh.Right() < ly.values[vol0].x, "and stops before the read-out", "");
    Check(!Overlaps(fh, ly.extraHit[vol0]), "the fader's target and the button's target cannot overlap", "");

    // ⚠️ AND A SWITCH KEEPS THE WHOLE ROW, which is not an inconsistency: a row's switch is the only control on it,
    // so there is nothing for a stray click to mean by accident. (The user's complaint was about a FADER, and a
    // fader is absolute -- that is the whole difference.)
    Check(ClickAt(m, ly, ly.rows[1].x + 2, ly.rows[1].y + 2).kind == HitKind::kToggle,
          "a switch still takes the whole row", "");
    Check(ClickAt(m, ly, ly.rows[vol0].x + 2, ly.rows[vol0].y + 2).kind == HitKind::kNone,
          "and a fader row does not", "");

    // (5) THE WHEEL TURNS THE FADER IT IS OVER, AND NOTHING ELSE (see WheelTarget).
    Check(WheelTarget(m, ly, fh.x + fh.w / 2, fh.y + fh.h / 2) == vol0, "the wheel turns the fader under it", "");
    Check(WheelTarget(m, ly, ly.labels[vol0].x + 4, ly.labels[vol0].y + 4) < 0,
          "and does nothing on the name beside it", "");
    Check(WheelTarget(m, ly, ly.rows[1].x + 2, ly.rows[1].y + 2) < 0, "and nothing on a switch row", "");
    Check(WheelTarget(m, ly, ly.extras[vol0].x + ly.extras[vol0].w / 2, ly.extras[vol0].y + 4) < 0,
          "and nothing on the companion button", "");
  }

  // ---------------------------------------------------------------------------------------------
  // ⚠️⚠️ THE SAME ARITHMETIC ONE ROW KIND OVER: A SWITCH ROW WITH A SECOND SWITCH BESIDE IT (ABI 16 -> 17).
  //
  // The user's request was KeepAwake's list in the flyout -- "快速面板按列表显示系统和各应用的两个功能开关" -- and
  // then their correction once they saw it: "保持唤醒的快速面板…是要用一样的滑动开关". So a switch row's companion is
  // THE SAME SWITCH as the one beside it (a fader row's stays the compact button, where a switch would read as a
  // second thing to drag), and everything that had to be true of the fader's button has to be true here: inside the
  // row, right of the row's own control, its own click target, and no wheel.
  {
    const int list0 = 8, list1 = 9; // the two rows added to the sample for exactly this
    char detail[200] = {0};
    _snprintf(detail, sizeof(detail), "switch %d,%d %dx%d  second switch %d,%d %dx%d", ly.control[list0].x,
              ly.control[list0].y, ly.control[list0].w, ly.control[list0].h, ly.extras[list0].x,
              ly.extras[list0].y, ly.extras[list0].w, ly.extras[list0].h);
    Check(ly.extras[list0].w > 0 && ly.extras[list1].w == 0,
          "only the switch row whose feature sent a second switch has one", detail);
    // ⚠️⚠️ AND IT IS THE SAME SIZE AS THE ROW'S OWN SWITCH -- "一样的滑动开关" is a statement about two rectangles,
    // and a companion measured as the compact button (a different width AND a different height) would be drawn as
    // one shape inside a box measured for the other.
    Check(ly.extras[list0].w == ly.control[list0].w && ly.extras[list0].h == ly.control[list0].h,
          "and it is EXACTLY the same switch as the row's own, not a smaller button", detail);
    Check(ly.extras[list0].x >= ly.control[list0].Right() &&
              ly.extras[list0].Right() <= ly.rows[list0].Right() &&
              ly.extras[list0].Bottom() <= ly.rows[list0].Bottom(),
          "it is to the RIGHT of the row's own switch, and inside the row", detail);
    Check(ly.control[list0].x == ly.control[list1].x && ly.control[list0].w == ly.control[list1].w,
          "two switches in one block still line up, companion or no companion", detail);
    // A click on the switch is the switch, a click on the second one is the second one -- and the rest of the line
    // still belongs to the row's own switch, because a switch is not an absolute control like a fader (see ClickAt).
    Check(ClickAt(m, ly, ly.control[list0].x + 2, ly.control[list0].y + 2).kind == HitKind::kToggle,
          "a click on the switch toggles the row", "");
    Check(ClickAt(m, ly, ly.extras[list0].x + ly.extras[list0].w / 2,
                  ly.extras[list0].y + ly.extras[list0].h / 2).kind == HitKind::kExtra,
          "a click on the second switch is the SECOND switch, not the row's own beside it", "");
    Check(ClickAt(m, ly, ly.rows[list0].x + 2, ly.rows[list0].y + 2).kind == HitKind::kToggle,
          "and the rest of the line is still the row's own switch", "");
    Check(!Overlaps(ly.extraHit[list0], ly.control[list0]),
          "the second switch's target cannot overlap the switch it sits beside", "");
    Check(WheelTarget(m, ly, ly.extras[list0].x + ly.extras[list0].w / 2, ly.extras[list0].y + 4) < 0,
          "and the wheel does nothing on it", "");
    // ⚠️ AND THE FADER ROWS KEPT THE COMPACT BUTTON -- the other half of the same rule, checked where the button is
    // so that "make the companion a switch" cannot quietly turn the mute buttons into switches too. (Item 6 is the
    // first volume row, whose mute button the section above measured.)
    Check(ly.extras[6].w == mx.extraW && ly.extras[6].h == mx.extraH,
          "while a FADER row's companion is still the compact button", detail);
  }

  // AN EMPTY MODEL HAS NO HEIGHT, and the caller draws nothing. A panel with a border and no content is worse
  // than no panel.
  Model empty;
  Layout ly2;
  Check(MeasureModel(empty, mx, &ly2) == 0, "an empty model measures zero (nothing to show)");
}

// ---------------------------------------------------------------------------
// 2. VALUES
// ---------------------------------------------------------------------------
static void TestValues()
{
  printf("\n== 2. what a drag is worth ==\n");
  Item it;
  it.kind = RowKind::kSlider;
  it.min = 100;
  it.max = 300;
  it.step = 5;
  it.value = 200;

  Check(SnapToStep(it, 103.0) == 105.0, "a value lands on the item's own step");
  Check(SnapToStep(it, 99.0) == 100.0, "below the range clamps to the minimum");
  Check(SnapToStep(it, 9999.0) == 300.0, "above the range clamps to the maximum");
  // FROM THE MINIMUM, not from zero: a range starting at 100 with a step of 5 has 100, 105 ... and not the
  // multiples of five that happen to fall inside it (which would be the same here -- hence the second case).
  Item odd = it;
  odd.min = 102;
  Check(SnapToStep(odd, 104.0) == 102.0 || SnapToStep(odd, 104.0) == 107.0,
        "steps are counted from the minimum, not from zero");
  Check(SnapToStep(odd, 102.0) == 102.0, "the minimum itself is on the grid");

  const Rect track{10, 0, 101, 8};
  Check(SliderValueAt(it, track, 10) == 100.0, "the fader's left end is the minimum");
  Check(SliderValueAt(it, track, 110) == 300.0, "the fader's right end is the maximum");
  Check(SliderValueAt(it, track, 9) == 100.0 && SliderValueAt(it, track, 999) == 300.0,
        "a drag past either end stays at the end");
  const double mid = SliderValueAt(it, track, 60);
  Check(mid > 100.0 && mid < 300.0, "the middle of the track is the middle of the range");

  // A KNOB IS RELATIVE -- the pointer's vertical travel from where the button went down.
  Item k = it;
  const double start = 200.0;
  Check(KnobValueAt(k, start, 0, 34) == 200.0, "a knob does not move until the pointer does");
  Check(KnobValueAt(k, start, -34 * 4, 34) == 300.0, "four rows of upward travel reach the maximum");
  Check(KnobValueAt(k, start, 34 * 4, 34) == 100.0, "and the same travel down reaches the minimum");
  Check(KnobValueAt(k, start, -34 * 8, 34) == 300.0, "and further travel stays at the maximum");

  Check(FractionFromValue(it) == 0.5, "the fraction a fader draws is the value's place in the range");
  it.value = 0;
  Check(FractionFromValue(it) == 0.0, "a value below the range draws at the start");

  // ---- THE PANE KEY: AN IDENTITY *AND* A TRANSPORT STRING --------------------------------------
  //
  // ⚠️⚠️ THIS IS WHERE THE USER'S "快速面板分组不能调顺序" IS PREVENTED. The General page sends the keys back as ONE
  // comma-separated value, and the request protocol is `key=value` LINES -- so a key containing a newline TRUNCATES
  // the whole list at that point, and a key containing a comma splits into two on the way in. Both characters are
  // replaced with a space HERE, at the only place keys are built, so neither can reach the wire.
  {
    char k1[112] = {0}, k2[112] = {0}, k3[112] = {0};
    QuickBlockKey("KeepAwake", "\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92", "rules[0].awake", k1,
                  sizeof(k1)); // 保持唤醒
    Check(strcmp(k1, "KeepAwake|\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92") == 0,
          "a named group's key is the feature and the group", k1);
    QuickBlockKey("SmoothWheel", "", "glide", k2, sizeof(k2));
    Check(strcmp(k2, "SmoothWheel|glide") == 0,
          "an item that names no group is a pane of its own, keyed by its own control path", k2);
    // The two characters that mean something on the wire, and the reason this function exists.
    QuickBlockKey("M", "a,b\nc\rd", "x", k3, sizeof(k3));
    Check(strcmp(k3, "M|a b c d") == 0,
          "a comma or a newline in a name can never reach the wire", k3);
    // ... and the identity survives it: two different panes cannot fold into one key.
    char k4[112] = {0};
    QuickBlockKey("M", "a b", "x", k4, sizeof(k4));
    Check(strcmp(k3, k4) != 0, "  and two different panes still come out as two different keys", k4);
    // The item id is used only when there is no group name: a feature that names a group does not get a key that
    // moves when it reorders its own items.
    char k5[112] = {0};
    QuickBlockKey("M", "g", "item9", k5, sizeof(k5));
    Check(strcmp(k5, "M|g") == 0, "  and a named group ignores the item id entirely", k5);
  }

  // The read-out is the value and its unit, at the precision the step asks for.
  char buf[64];
  CopyStr(it.unit, kUnitLen, "ms");
  it.step = 5;
  it.value = 200;
  FormatValue(it, buf, sizeof(buf));
  Check(strcmp(buf, "200 ms") == 0, "the read-out is the number and its unit");
  it.step = 0.1;
  it.value = 1.44;
  FormatValue(it, buf, sizeof(buf));
  Check(strcmp(buf, "1.4 ms") == 0, "the precision follows the step");
  it.unit[0] = 0;
  FormatValue(it, buf, sizeof(buf));
  Check(strcmp(buf, "1.4") == 0, "an item with no unit shows just the number");

  // ---- THE WHEEL OVER A FADER ------------------------------------------------------------------
  //
  // ⚠️ THE USER ASKED FOR THIS ("推子可以用鼠标滚轮操作；这条追加适配到快速面板所有推子的操作"), and the arithmetic is
  // the settings page's own -- one `step` per Windows notch (120), accumulated so a trackpad's small deltas add up
  // at the same RATE rather than needing forty flicks. Every case below is one the settings page already has, and
  // that is the point: the same wheel must not move the two surfaces by different amounts.
  {
    Item w = it;
    w.min = 0;
    w.max = 100;
    w.step = 5;
    w.value = 50;
    int acc = 0;
    // ⚠️ EVERY CALL UPDATES `value` FROM WHAT IT RETURNED, because that is what the window does (it writes the
    // answer into the item's `value` and redraws) -- the function reads the item's own value and has no state.
    w.value = WheelValueAfter(w, 120, &acc);
    Check(w.value == 55.0, "one notch up is one step up");
    Check(acc == 0, "and a whole notch leaves nothing in the accumulator");
    w.value = WheelValueAfter(w, -120, &acc);
    Check(w.value == 50.0, "one notch down is one step down");
    // A trackpad reports a few units at a time; four events of 30 are one notch, and nothing moves before that.
    acc = 0;
    Check(WheelValueAfter(w, 30, &acc) == 50.0, "a third of a notch moves nothing");
    Check(WheelValueAfter(w, 30, &acc) == 50.0, "nor does two thirds");
    Check(WheelValueAfter(w, 30, &acc) == 50.0, "nor three thirds");
    w.value = WheelValueAfter(w, 30, &acc);
    Check(w.value == 55.0, "and the fourth is the notch");
    Check(acc == 0, "which again leaves nothing behind");
    // Several notches in one event (a fast flick, or a wheel that reports 240 at a time). The value was 55, so
    // three steps up is 70 -- and it is three steps, not one.
    acc = 0;
    w.value = WheelValueAfter(w, 360, &acc);
    Check(w.value == 70.0, "three notches in one message are three steps");
    // The ends behave like a drag: clamped, and snapped to the item's own grid.
    acc = 0;
    w.value = 98;
    w.value = WheelValueAfter(w, 120, &acc);
    Check(w.value == 100.0, "the last notch stops at the maximum");
    w.value = WheelValueAfter(w, 120, &acc);
    Check(w.value == 100.0, "and further notches change nothing");
    w.value = 3;
    acc = 0;
    w.value = WheelValueAfter(w, -120, &acc);
    Check(w.value == 0.0, "and the way down stops at the minimum");
    // A range that does not start at 0 keeps counting from its own minimum (the same rule as SnapToStep).
    Item odd2 = w;
    odd2.min = 100;
    odd2.max = 300;
    odd2.step = 5;
    odd2.value = 195;
    acc = 0;
    Check(WheelValueAfter(odd2, 120, &acc) == 200.0, "a step lands on the item's grid, from its own minimum");
    // The accumulator is per row: the window clears it when the pointer moves (see OnMouseMove), and this is the
    // function that must not carry it anywhere by itself.
    acc = 0;
    WheelValueAfter(w, 30, &acc);
    Check(acc == 30, "the remainder is handed back to the caller, not remembered here");
  }
}

// ---------------------------------------------------------------------------
// 3. THE FADE
// ---------------------------------------------------------------------------
static void TestFade()
{
  printf("\n== 3. the fade the user asked for (0.2-0.5 s) ==\n");
  Check(FadeInAlpha(0) == 0.0, "a fade-in starts invisible");
  Check(FadeInAlpha(kFadeInMs) == 1.0, "and ends fully opaque");
  Check(FadeOutAlpha(0) == 1.0, "a fade-out starts opaque");
  Check(FadeOutAlpha(kFadeOutMs) == 0.0, "and ends invisible");

  bool rising = true, falling = true;
  double prev = -1.0;
  for (int t = 0; t <= kFadeInMs; t += 5)
  {
    const double a = FadeInAlpha(t);
    if (a < prev)
      rising = false;
    prev = a;
  }
  prev = 2.0;
  for (int t = 0; t <= kFadeOutMs; t += 5)
  {
    const double a = FadeOutAlpha(t);
    if (a > prev)
      falling = false;
    prev = a;
  }
  Check(rising, "the fade-in never goes backwards");
  Check(falling, "the fade-out never goes backwards");

  char detail[96];
  const bool inRange = kFadeInMs >= 200 && kFadeInMs <= 500;
  const bool outRange = kFadeOutMs >= 200 && kFadeOutMs <= 500;
  _snprintf(detail, sizeof(detail), "(in %d ms, out %d ms)", kFadeInMs, kFadeOutMs);
  Check(inRange && outRange, "both are between 0.2 s and 0.5 s, as asked", detail);
}

// ---------------------------------------------------------------------------
// 4. WHERE IT LANDS
// ---------------------------------------------------------------------------
static void TestPlacement()
{
  printf("\n== 4. above the tray icon, inside the work area ==\n");
  const Rect work{0, 0, 1920, 1040}; // a 1080 screen minus a 40-pixel taskbar at the bottom
  const Rect icon{1700, 1040, 24, 24};
  int x = 0, y = 0;
  PlaceAbove(icon, work, 300, 400, 8, &x, &y);
  Check(y + 400 == 1040 - 8, "the panel sits just above the icon");
  Check(x + 300 <= work.Right() && x >= work.x, "and is pulled back inside the screen on the right");
  Check(y >= work.y, "and never leaves the top of the work area");

  // A TASKBAR AT THE TOP HAS NO ROOM ABOVE IT, so the panel flips below the icon rather than off the screen.
  const Rect topIcon{900, 0, 24, 24};
  PlaceAbove(topIcon, work, 300, 400, 8, &x, &y);
  Check(y >= topIcon.Bottom(), "with the taskbar at the top, the panel goes below the icon");

  // A panel taller than the work area cannot fit inside it; what matters is that it starts at the TOP of the
  // work area, so its first row is on screen rather than above the top edge where nothing can be read.
  PlaceAbove(icon, work, 300, 4000, 8, &x, &y);
  Check(y == work.y, "an over-tall panel starts at the top of the work area");

  // A LEFT-EDGE icon must not push the panel off the left.
  const Rect leftIcon{0, 1040, 24, 24};
  PlaceAbove(leftIcon, work, 300, 400, 8, &x, &y);
  Check(x >= work.x, "an icon at the left edge does not push the panel off-screen");
}

// ---------------------------------------------------------------------------
// 5. THE COLOURS, AGAINST THE SETTINGS PAGE
// ---------------------------------------------------------------------------
struct VarVals
{
  char name[40];
  unsigned vals[8];
  int n;
};

static int HexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static bool IsNameChar(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// Every `--name: #rrggbb` in the stylesheet, with ALL the values that name takes (the light block and the dark
// one). A name that appears once is a colour that does not change with the theme, which is itself a fact worth
// being able to state (see --dot-on).
static int CollectVars(const char *text, VarVals *out, int max)
{
  int count = 0;
  for (const char *p = text; *p; ++p)
  {
    if (p[0] != '-' || p[1] != '-')
      continue;
    char name[40];
    int nn = 0;
    const char *q = p;
    while (*q && IsNameChar(*q) && nn < 39)
      name[nn++] = *q++;
    name[nn] = 0;
    while (*q == ' ' || *q == '\t')
      ++q;
    if (*q != ':')
      continue;
    ++q;
    while (*q == ' ' || *q == '\t')
      ++q;
    if (*q != '#')
      continue;
    ++q;
    unsigned v = 0;
    int digits = 0;
    while (digits < 6)
    {
      const int d = HexDigit(*q);
      if (d < 0)
        break;
      v = (v << 4) | (unsigned)d;
      ++q;
      ++digits;
    }
    if (digits != 6)
      continue;
    int at = -1;
    for (int i = 0; i < count; ++i)
      if (strcmp(out[i].name, name) == 0)
        at = i;
    if (at < 0)
    {
      if (count >= max)
        continue;
      at = count++;
      strncpy(out[at].name, name, sizeof(out[at].name) - 1);
      out[at].name[sizeof(out[at].name) - 1] = 0;
      out[at].n = 0;
    }
    if (out[at].n < 8)
      out[at].vals[out[at].n++] = v;
  }
  return count;
}

static bool VarHas(const VarVals *vars, int n, const char *name, unsigned value)
{
  for (int i = 0; i < n; ++i)
    if (strcmp(vars[i].name, name) == 0)
      for (int k = 0; k < vars[i].n; ++k)
        if (vars[i].vals[k] == value)
          return true;
  return false;
}

static unsigned RgbNum(const Rgb &c) { return ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | (unsigned)c.b; }

// Perceived brightness, for telling "the light --bg" from "the dark --bg" without relying on the order the
// stylesheet happens to define them in.
static unsigned Luma(unsigned v)
{
  return ((v >> 16) & 0xFF) * 30 + ((v >> 8) & 0xFF) * 59 + (v & 0xFF) * 11;
}

static int RgbDist(const Rgb &c, unsigned v)
{
  const int dr = (int)c.r - (int)((v >> 16) & 0xFF);
  const int dg = (int)c.g - (int)((v >> 8) & 0xFF);
  const int db = (int)c.b - (int)(v & 0xFF);
  int m = dr < 0 ? -dr : dr;
  if (dg < 0 ? -dg : dg > m)
    m = dg < 0 ? -dg : dg;
  if (db < 0 ? -db : db > m)
    m = db < 0 ? -db : db;
  return m;
}

static void TestPalette(const char *root)
{
  printf("\n== 5. the flyout's colours are the settings page's colours ==\n");
  // ⚠️⚠️ AND THE ONE COLOUR RULE THAT IS NOT A PALETTE VALUE: A GLYPH MUST CONTRAST WITH THE FILL IT SITS ON.
  // The companion button's engaged state is filled with the row's own ink, and for a row with no `hue` that ink is
  // the panel's ACCENT -- near-black in the light theme (a white glyph reads) and NEAR-WHITE in the dark one, where
  // the old "always white" glyph was invisible: the button came out as a blank square, which is the user's report
  // ("保持唤醒的快速面板少了防止熄屏开关，现在只有保持唤醒开关"). The rule is a pure function of three numbers
  // (quickpalette.h), so it is checked here with no screen and no GDI+.
  {
    struct Fill
    {
      const char *what;
      unsigned rgb;
    };
    const Fill fills[] = {
        {"the light theme's accent", 0x1A1A1A},
        {"the dark theme's accent", 0xF2F2F2},
        {"a brightness fader's amber", 0xFFC24D},
        {"a volume fader's blue", 0x78BEFF},
        {"mid grey", 0x808080},
    };
    for (int i = 0; i < (int)(sizeof(fills) / sizeof(fills[0])); ++i)
    {
      const int fr = (int)((fills[i].rgb >> 16) & 0xFF), fg = (int)((fills[i].rgb >> 8) & 0xFF),
                fb = (int)(fills[i].rgb & 0xFF);
      const int luma = LumaOf((unsigned char)fr, (unsigned char)fg, (unsigned char)fb);
      // The glyph is one end of the range, chosen by that luma (see LitInk in quickpaint.h): 20 or 255.
      const int glyph = FillTakesDarkGlyph(luma) ? LumaOf(20, 20, 20) : LumaOf(255, 255, 255);
      const int gap = luma - glyph;
      const int dist = gap < 0 ? -gap : gap;
      char d[160];
      _snprintf(d, sizeof(d), "%s: fill %02X%02X%02X luma=%d, glyph luma=%d, gap=%d", fills[i].what, fr, fg, fb,
                luma, glyph, dist);
      Check(dist >= 90, "a companion button's glyph is legible on its own fill", d);
    }
  }

  char path[600];
  _snprintf(path, sizeof(path), "%s/apex/ui/panel.css", root);
  FILE *f = fopen(path, "rb");
  if (!f)
  {
    Check(false, "the stylesheet could be read", path);
    return;
  }
  static char text[128 * 1024];
  const size_t got = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[got] = 0;

  VarVals vars[64];
  const int nv = CollectVars(text, vars, 64);
  Check(nv > 8, "the stylesheet's colour variables were found");

  struct Map
  {
    const char *var;
    size_t off;
  };
  const Map map[] = {
      {"--line", offsetof(Palette, border)},  {"--panel", offsetof(Palette, section)},
      {"--fg", offsetof(Palette, fg)},        {"--sub", offsetof(Palette, sub)},
      {"--track", offsetof(Palette, track)},  {"--accent", offsetof(Palette, accent)},
      {"--dot-on", offsetof(Palette, dotOn)}, {"--dot-off", offsetof(Palette, dotOff)},
  };
  const int nmap = (int)(sizeof(map) / sizeof(map[0]));
  const Palette *pals[2] = {&kLightQuick, &kDarkQuick};
  const char *names[2] = {"light", "dark"};

  // THE PAGE HAS ONE `--bg` PER THEME, and which is which is a fact about COLOUR rather than about order: the
  // light block happens to come first in the stylesheet, and a check that relied on that would pass while a
  // reordered file pointed the dark palette at the light background. So the brightest and the darkest are
  // picked.
  unsigned bgLight = 0, bgDark = 0;
  bool haveBg = false;
  for (int i = 0; i < nv; ++i)
  {
    if (strcmp(vars[i].name, "--bg") != 0)
      continue;
    for (int k = 0; k < vars[i].n; ++k)
    {
      const unsigned v = vars[i].vals[k];
      if (!haveBg || Luma(v) > Luma(bgLight))
        bgLight = v;
      if (!haveBg || Luma(v) < Luma(bgDark))
        bgDark = v;
      haveBg = true;
    }
  }
  Check(haveBg && bgLight != bgDark, "the stylesheet defines --bg once per theme");

  for (int t = 0; t < 2; ++t)
  {
    for (int i = 0; i < nmap; ++i)
    {
      const Rgb *c = (const Rgb *)((const char *)pals[t] + map[i].off);
      char what[96], detail[64];
      _snprintf(what, sizeof(what), "%s palette: %s", names[t], map[i].var);
      _snprintf(detail, sizeof(detail), "#%06x", RgbNum(*c));
      Check(VarHas(vars, nv, map[i].var, RgbNum(*c)), what, detail);
    }
    // THE BACKGROUND IS THE ONE DELIBERATE DIFFERENCE: the page is flat, the flyout is a shallow gradient.
    // Both ends have to stay close to the page's own --bg for that theme, or the two surfaces read as
    // different materials.
    const Palette &p = *pals[t];
    const unsigned bg = (t == 0) ? bgLight : bgDark;
    char what[96], detail[96];
    _snprintf(what, sizeof(what), "%s palette: the gradient sits around --bg", names[t]);
    _snprintf(detail, sizeof(detail), "(%d and %d steps from #%06x)", RgbDist(p.bgTop, bg), RgbDist(p.bgBottom, bg),
              bg);
    Check(RgbDist(p.bgTop, bg) <= 12 && RgbDist(p.bgBottom, bg) <= 12, what, detail);
  }

  // The switch's own dot is a literal in the stylesheet (`.sw .kn { background: #fff; }`), so it is checked as
  // one -- and it is the same in both palettes, which is the point: the dot is drawn ON the track.
  Check(kLightQuick.knob.r == 0xFF && kLightQuick.knob.g == 0xFF && kLightQuick.knob.b == 0xFF &&
            kDarkQuick.knob.r == 0xFF && kDarkQuick.knob.g == 0xFF && kDarkQuick.knob.b == 0xFF,
        "the switch's dot is white in both palettes");
  Check(pals[0]->dotOn.r == pals[1]->dotOn.r && pals[0]->dotOn.g == pals[1]->dotOn.g &&
            pals[0]->dotOn.b == pals[1]->dotOn.b,
        "\"on\" is one colour in both themes (the stylesheet says why: it must read on both)");
  // ⚠️ A RANGE, NOT A NUMBER. How much glass is enough is a judgement, and the user moved it once already
  // ("透明度不够，玻璃感要加强下"). What the gate can hold onto is the two ends: fully opaque is not glass, and
  // past ~150 the labels stop being readable over a busy wallpaper.
  Check(pals[0]->bgAlpha >= 150 && pals[0]->bgAlpha <= 235 && pals[1]->bgAlpha == pals[0]->bgAlpha,
        "the body is glass rather than a solid fill, in both themes");
}

// ---------------------------------------------------------------------------
// 6. WHAT THE FEATURES ANSWER
// ---------------------------------------------------------------------------
static const char *g_featureDir = nullptr;

static int StubTargetAt(int, int, ApexTarget *) { return 0; }
static void StubInject(double) {}
static void StubLog(const char *t) { printf("        [feature] %s\n", t ? t : ""); }
static int StubDir(char *out, int cap)
{
  _snprintf(out, cap, "%s", g_featureDir ? g_featureDir : "");
  return 1;
}
static void StubActivity(void) {}
static int StubReaper(void) { return 0; }
static int StubEnabled(const char *) { return 1; }

static void BuildStubHost(ApexHost *h)
{
  ZeroMemory(h, sizeof(*h));
  h->abiVersion = APEX_ABI_VERSION;
  h->structSize = sizeof(ApexHost);
  h->targetAt = StubTargetAt;
  h->injectDeltas = StubInject;
  h->logLine = StubLog;
  h->featureDir = StubDir;
  h->activity = StubActivity;
  h->reaperPluginRunning = StubReaper;
  h->featureEnabled = StubEnabled;
  h->hostUser = nullptr;
}

typedef const ApexFeature *(*EntryFn)(void);

// Load one DLL the way the host does -- entry point by name, then VERSION AND SIZE BEFORE ANYTHING IS READ --
// and hand it to `body`. Returns false (and says why) when the DLL cannot be used at all.
static const ApexFeature *LoadFeature(const char *pluginsDir, const char *id, HMODULE *outMod)
{
  char path[600];
  _snprintf(path, sizeof(path), "%s/%s/%s.dll", pluginsDir, id, id);
  HMODULE mod = LoadLibraryA(path);
  if (!mod)
  {
    char what[128], detail[600];
    _snprintf(what, sizeof(what), "%s.dll loads", id);
    _snprintf(detail, sizeof(detail), "%s (error %lu)", path, (unsigned long)GetLastError());
    Check(false, what, detail);
    return nullptr;
  }
  EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  if (!entry)
  {
    char what[128];
    _snprintf(what, sizeof(what), "%s.dll exports ApexFeatureEntry", id);
    Check(false, what);
    FreeLibrary(mod);
    return nullptr;
  }
  const ApexFeature *api = entry();
  char what[128];
  _snprintf(what, sizeof(what), "%s: the ABI it was built against is this host's", id);
  if (!api || api->abiVersion != APEX_ABI_VERSION)
  {
    Check(false, what);
    FreeLibrary(mod);
    return nullptr;
  }
  _snprintf(what, sizeof(what), "%s: its struct size matches the header's", id);
  if (api->structSize != sizeof(ApexFeature))
  {
    Check(false, what);
    FreeLibrary(mod);
    return nullptr;
  }
  Check(true, what);
  *outMod = mod;
  return api;
}

// How many times `needle` occurs in `hay`. Used to count ROWS in a feature's own settings document, which is
// the independent side of the one comparison that matters below: what the flyout offers versus what the page
// shows (see `perDevice` in the table).
static int CountOf(const char *hay, const char *needle)
{
  int n = 0;
  for (const char *p = hay; (p = strstr(p, needle)) != nullptr; p += strlen(needle))
    ++n;
  return n;
}

// WHAT THE PANEL WOULD DRAW. Every rule here is a rule the flyout depends on, and a violation would draw a
// control that lies rather than crash -- the class of failure this project keeps having to write gates for.
static int CheckItems(const char *id, const ApexFeature *api)
{
  if (!api->quickItems)
  {
    Check(true, "no quick-panel controls (the field is null)");
    return 0;
  }
  // ⚠️ 32 AND NOT 12, SINCE NAMED GROUPS ARRIVED (ABI 12 -> 13). A feature's controls are no longer a handful it
  // chose: MediaControl draws one fader per MONITOR and one per APPLICATION, and how many of those exist is a
  // fact about the machine. The host's own per-feature ceiling is 24 (`kMaxOwnPerFeature` in quickpanel_win.cpp);
  // this buffer is that plus room to see the answer, because the two-part contract is exactly what is checked
  // here -- a feature with more items than fit must SAY so rather than be cut off silently.
  ApexQuickItem buf[32];
  ZeroMemory(buf, sizeof(buf));
  const int total = api->quickItems(buf, 32);
  if (total < 0 || total > 32)
  {
    char what[128];
    _snprintf(what, sizeof(what), "%s: quickItems reports a sane total", id);
    Check(false, what);
    return total;
  }
  // ASK WITH NO BUFFER TOO: the two-part answer has to be true both ways round, or a host with a smaller
  // buffer would believe it had read everything.
  const int probe = api->quickItems(nullptr, 0);
  Check(probe == total, "asking with no buffer reports the same total");

  for (int i = 0; i < total; ++i)
  {
    const ApexQuickItem &q = buf[i];
    char what[160], detail[160];
    _snprintf(what, sizeof(what), "item %d: id and both labels are filled in", i);
    _snprintf(detail, sizeof(detail), "id=\"%s\"", q.id);
    Check(q.id[0] && q.labelZh[0] && q.labelEn[0], what, detail);

    _snprintf(what, sizeof(what), "item %d: the shape is one of the three", i);
    const bool shapeOk = q.type == APEX_QUICK_TOGGLE || q.type == APEX_QUICK_SLIDER || q.type == APEX_QUICK_KNOB;
    Check(shapeOk, what);

    if (!shapeOk)
      continue;
    if (q.type == APEX_QUICK_TOGGLE)
    {
      _snprintf(what, sizeof(what), "item %d: a toggle is 0 or 1", i);
      Check(q.value == 0.0 || q.value == 1.0, what);
    }
    else
    {
      _snprintf(what, sizeof(what), "item %d: a range has min < max, a positive step and a value inside it", i);
      _snprintf(detail, sizeof(detail), "min=%g max=%g step=%g value=%g", q.min, q.max, q.step, q.value);
      Check(q.min < q.max && q.step > 0.0 && q.value >= q.min - 1e-9 && q.value <= q.max + 1e-9, what, detail);
    }

    // ⚠️⚠️ AND THE COMPANION SWITCH, WHEN THERE IS ONE (ABI 14 -> 15 -- the mute button beside a volume fader).
    // WHAT A HOST CAN CHECK ABOUT IT, AND WHAT IT CANNOT: the panel sends "1"/"0" to that path and knows nothing
    // else about it -- what it means is the feature's own word. So the invariant checked here is the one that makes
    // the button safe rather than the one that makes it meaningful: the path must be NON-EMPTY when it is there,
    // it must DIFFER from the row's own control (a button that writes the fader's value would move the fader), and
    // the state must be a 0 or a 1 (it is drawn as a two-state button).
    //
    // ⚠️ AND NOTHING HERE IS CLICKED. Toggling a real mute would silence a program on the machine of whoever runs
    // the gate -- "测试不许打扰用户" is a rule about exactly this, so the feature's OWN mute path is checked against
    // its own settings document instead (see check_feature_mediacontrol.sh), which is a read.
    if (q.toggleId[0])
    {
      _snprintf(what, sizeof(what), "item %d: a companion switch names a DIFFERENT control", i);
      _snprintf(detail, sizeof(detail), "id=\"%s\" toggleId=\"%s\"", q.id, q.toggleId);
      Check(strcmp(q.toggleId, q.id) != 0, what, detail);
      _snprintf(what, sizeof(what), "item %d: a companion switch is a 0 or a 1", i);
      Check(q.toggleOn == 0 || q.toggleOn == 1, what);
      // ⚠️ AND IT BELONGS ON A ROW THAT HAS A CONTROL OF ITS OWN, WHICH SINCE ABI 16 -> 17 INCLUDES A TOGGLE. The
      // first version of this check said "a range row only" -- the rule at the time. The user then asked for
      // KeepAwake's list in the flyout, ONE ROW PER PROGRAM WITH BOTH OF ITS SWITCHES ("快速面板按列表显示系统和各
      // 应用的两个功能开关"), so the row's own switch and the companion are two answers to two different questions.
      // What is still refused is a companion on a row with no control at all (`kNote`) or on the host's own
      // feature switch: there would be nothing for the pair to be a pair OF.
      _snprintf(what, sizeof(what), "item %d: a companion sits on a row that has a control of its own", i);
      Check(q.type == APEX_QUICK_SLIDER || q.type == APEX_QUICK_KNOB || q.type == APEX_QUICK_TOGGLE, what);
      // ⚠️ AND IT SAYS WHICH PICTURE IT IS DRAWN WITH (ABI 15 -> 16). The panel has exactly three icons and draws
      // anything else as the plain one, so a feature sending a number it invented gets a dot rather than a
      // picture that means something else -- which is the safe behaviour and also a bug in the feature. Checked
      // here so the safe behaviour is not the only thing that happens.
      _snprintf(what, sizeof(what), "item %d: the companion names a known icon", i);
      _snprintf(detail, sizeof(detail), "icon=%d", q.toggleIcon);
      Check(q.toggleIcon == APEX_QUICK_ICON_PLAIN || q.toggleIcon == APEX_QUICK_ICON_MUTE ||
                q.toggleIcon == APEX_QUICK_ICON_DISPLAY,
            what, detail);
    }
  }
  return total;
}

static void TestFeatures(const char *pluginsDir, const char *scratchDir)
{
  printf("\n== 6. what each feature puts in the quick panel ==\n");

  // ⚠️ THE FEATURES GET A SCRATCH FOLDER OF THEIR OWN. These are the REAL DLLs -- loading one runs its init(),
  // which reads that feature's settings file and (for KeepAwake) starts its worker. Pointing `featureDir` at
  // this probe's own empty folder is what keeps a gate from reading, or writing, the user's settings -- the
  // same rule every other gate in this project follows.
  CreateDirectoryA(scratchDir, nullptr);
  g_featureDir = scratchDir;
  ApexHost stub;
  BuildStubHost(&stub);

  struct Expected
  {
    const char *id;
    int shipped;                 // -1 = the feature has no quickItems at all
    const char *mapping[2];      // the switches on the feature's own page that permit a control
    int mapped;                  // ... and how many items it answers with once they are on
    bool slider;
    bool knob;
    bool toggle;
    // ⚠️ TRUE FOR A FEATURE WHOSE COUNT IS A FACT ABOUT THE MACHINE rather than a number of controls it ships.
    // MediaControl draws one fader per MONITOR (first switch) and one per APPLICATION that is making sound
    // (second), so "how many" cannot be written here -- it would be a number that is right on one desk. The
    // expectation is read from the feature's OWN SETTINGS DOCUMENT instead, which turns the check into the
    // comparison that actually matters: the flyout must offer exactly as many faders as the page has rows.
    bool perDevice;
    // ⚠️ AND TRUE FOR A FEATURE WHOSE MAPPED CONTROLS ARE ONE SET RATHER THAN SEVERAL (ABI 16 -> 17). The count
    // alone does not say that: "four faders" is satisfied by four blocks of their own, and a flyout that drew
    // KeepAwake's list as one floating pane per program would say "these are unrelated" about one list. So the
    // pane's own name is checked too -- every item must name the SAME one, in both languages.
    bool onePane;
  };
  // ⚠️ `shipped` IS ZERO FOR ALL OF THEM, AND THAT IS THE USER'S RULE RATHER THAN A PLACEHOLDER: "插件自己的控件要
  // 明确有开关映射到快速面板，才给". A feature does NOT reach the flyout by deciding it would be useful there;
  // it reaches it because the user turned on a switch on that feature's own page. So the FIRST thing checked
  // below is that a fresh install puts nothing in the panel -- a feature that shipped with its controls already
  // mapped would be answering for the user.
  //
  // ⚠️ AND SINCE ABI 16 -> 17 THAT SWITCH IS ONE PER FEATURE OR PER GROUP, NOT ONE PER CONTROL (the user's own
  // correction: "滑动滚轮插件的「快速面板」开关也只要给一个", "保持唤醒插件…给一个开关", "媒体控制插件…位置移到亮度和
  // 音量各自小标题的右侧"). So `mapped` is no longer "one item per switch": SmoothWheel's single switch offers
  // all FOUR of its numbers, and KeepAwake's offers its whole list.
  //
  // SmoothWheel then offers glide / slow / ramp / top as four faders in one pane; KeepAwake offers one row per
  // program (plus 系统全局) with two switches on each; MediaControl offers one fader per device; AuditStub
  // deliberately has nothing (it is the control case for the null field).
  const Expected want[] = {
      {"SmoothWheel", 0, {"quick_panel", nullptr}, 4, true, false, false, false, true},
      {"KeepAwake", 0, {"quick_panel", nullptr}, 1, false, false, true, false, true},
      {"MediaControl", 0, {"quick_brightness", "quick_volume"}, 0, true, false, false, true, false},
      {"AuditStub", -1, {nullptr, nullptr}, 0, false, false, false, false, false},
  };

  const int n = (int)(sizeof(want) / sizeof(want[0]));
  for (int i = 0; i < n; ++i)
  {
    // ⚠️ THE AUDIT STUB IS OPTIONAL AND THE OTHERS ARE NOT. It is the project's `.dev-only` feature -- the
    // second feature written to prove "adding one is one folder" -- so a release build REMOVES it from the
    // artefact folder (see apex/build.sh), and a gate that insisted on it would be green in the edit loop and
    // red in a delivery. Its case (quickItems is null) is also covered by AutoIME, which is in every build.
    if (strcmp(want[i].id, "AuditStub") == 0)
    {
      char p[600];
      _snprintf(p, sizeof(p), "%s/%s/%s.dll", pluginsDir, want[i].id, want[i].id);
      if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES)
      {
        printf("  -- AuditStub: not in this build (.dev-only, and a release build removes it) -- skipped\n");
        continue;
      }
    }
    printf("  -- %s\n", want[i].id);
    HMODULE mod = nullptr;
    const ApexFeature *api = LoadFeature(pluginsDir, want[i].id, &mod);
    if (!api)
      continue;

    if (want[i].shipped < 0)
    {
      Check(api->quickItems == nullptr, "no quick-panel controls (the field is null)");
      FreeLibrary(mod);
      continue;
    }

    char dir[600];
    _snprintf(dir, sizeof(dir), "%s/%s/", scratchDir, want[i].id);
    CreateDirectoryA(dir, nullptr);
    g_featureDir = dir;
    const int ready = api->init ? api->init(&stub) : 0;
    char what[160];
    _snprintf(what, sizeof(what), "%s: init reports ready", want[i].id);
    Check(ready == 0, what);

    // (1) A FRESH INSTALL PUTS NOTHING IN THE PANEL. This is the half that catches a feature which never asked.
    if (!api->quickItems)
    {
      Check(false, "the feature describes quick-panel controls at all");
      if (api->shutdown)
        api->shutdown();
      FreeLibrary(mod);
      continue;
    }
    int total = CheckItems(want[i].id, api);
    char what2[160], detail[96];
    _snprintf(what2, sizeof(what2), "%s: with every mapping switch OFF it offers nothing", want[i].id);
    _snprintf(detail, sizeof(detail), "(got %d)", total);
    Check(total == want[i].shipped, what2, detail);

    // HOW MANY ROWS THE PAGE HAS, for a feature whose controls are one per device. Read from the feature's own
    // settings document -- a different answer to a different question, which is what makes it usable as the
    // expectation for the flyout's count.
    int pageRows[2] = {0, 0};
    if (want[i].perDevice && api->settingsJson)
    {
      static char page[64 * 1024];
      if (api->settingsJson(page, (int)sizeof(page)) > 0)
      {
        pageRows[0] = CountOf(page, "\"values\":{\"brightness\":"); // the monitor rows
        pageRows[1] = CountOf(page, "\"values\":{\"volume\":");     // the application rows
      }
      _snprintf(detail, sizeof(detail), "(%d monitor row(s), %d application row(s))", pageRows[0], pageRows[1]);
      Check(pageRows[0] >= 1, "  the page has a row for at least one device to compare against", detail);
    }

    // (2) TURNING THE SWITCHES ON IS WHAT PUTS THEM THERE, and it goes through the SAME setControl the
    // settings page uses -- there is no separate path for the flyout to get out of step with.
    //
    // ⚠️ WHAT IS CHECKED AFTER EACH STEP CHANGED WITH ABI 16 -> 17. A group switch maps a SET, so "one more item
    // per switch" stopped being a rule anyone can be held to -- SmoothWheel's one switch offers four numbers.
    // What still has to hold at every step is that the switch is ACCEPTED; the TOTAL is checked once they are all
    // on (just below). A per-device feature is the exception and keeps its per-step count, because its two
    // switches map two different groups whose sizes the feature's own document has already stated.
    int mapCount = 0;
    while (mapCount < 2 && want[i].mapping[mapCount])
      ++mapCount;
    for (int k = 0; k < mapCount; ++k)
    {
      _snprintf(what, sizeof(what), "%s: setControl(\"%s\", 1) is accepted", want[i].id, want[i].mapping[k]);
      const int took = api->setControl ? api->setControl(want[i].mapping[k], "1") : 0;
      Check(took == 1, what);

      if (want[i].perDevice)
      {
        const int after = api->quickItems(nullptr, 0);
        // One control per ROW for a per-device feature: the monitors first, then the applications.
        const int expectAfter = (k == 0) ? pageRows[0] : pageRows[0] + pageRows[1];
        _snprintf(what2, sizeof(what2), "%s: after mapping \"%s\" it offers %d", want[i].id, want[i].mapping[k],
                  expectAfter);
        _snprintf(detail, sizeof(detail), "(got %d)", after);
        Check(after == expectAfter, what2, detail);
      }
    }

    total = CheckItems(want[i].id, api);
    const int expectAll =
        want[i].perDevice ? pageRows[0] + pageRows[1] : want[i].mapped;
    _snprintf(what2, sizeof(what2), "%s: with every mapping on it offers %d quick-panel control(s)", want[i].id,
              expectAll);
    _snprintf(detail, sizeof(detail), "(got %d)", total);
    Check(total == expectAll, what2, detail);

    // ⚠️ AND ONE MORE THING ABOUT THE MAPPED ITEMS, WHICH IS A CONTRACT WITH THE HOST AND NOT WITH THIS
    // FEATURE: a control that names a GROUP must name it in BOTH languages (apex/abi.h: `groupZh`/`groupEn`),
    // because the host draws the reader's one and has no way to pick for a feature that sent only one. The
    // other direction is checked too -- a feature that names no group must leave both empty, or the host would
    // collect unrelated controls into a pane called "".
    {
      ApexQuickItem buf[32];
      ZeroMemory(buf, sizeof(buf));
      const int got = api->quickItems(buf, 32);
      int named = 0, broken = 0;
      for (int k = 0; k < got && k < 32; ++k)
      {
        const bool zh = buf[k].groupZh[0] != 0, en = buf[k].groupEn[0] != 0;
        if (zh || en)
        {
          ++named;
          if (!zh || !en)
            ++broken;
        }
      }
      _snprintf(what2, sizeof(what2), "%s: every mapped control names its pane in both languages", want[i].id);
      _snprintf(detail, sizeof(detail), "(%d named, %d half-named)", named, broken);
      Check(broken == 0, what2, detail);
      // ⚠️ AND "THEY ARE ALL NAMED" IS NOT THE SAME AS "THEY ARE ALL IN ONE PANE". Every item naming A pane is
      // satisfied by one pane each, which is exactly the failure the grouping exists to prevent: the user asked
      // for KeepAwake's LIST ("快速面板按列表显示系统和各应用的两个功能开关") and for the wheel feature's four
      // numbers, and a flyout that floated each row in a box of its own would have answered "these are unrelated"
      // about one set. So the NAME is compared too, against the first item's -- in both languages, because the
      // host compares both to decide what belongs together (see FindOrAddSection in quickpanel.h).
      // (A feature with two groups -- MediaControl's brightness and volume -- is not this case: it says `onePane`
      // false, and its "named in both languages" line above is the check that applies to it.)
      if (want[i].onePane)
      {
        int same = (got > 0) ? 1 : 0;
        for (int k = 1; k < got && k < 32; ++k)
          if (strcmp(buf[k].groupZh, buf[0].groupZh) == 0 && strcmp(buf[k].groupEn, buf[0].groupEn) == 0)
            ++same; // the ones that agree; the detail line says how many strayed
        _snprintf(what2, sizeof(what2), "%s: and they share ONE pane rather than one pane each", want[i].id);
        _snprintf(detail, sizeof(detail), "(%d of %d in the first item's pane, named \"%s\"/\"%s\")", same, got,
                  buf[0].groupZh, buf[0].groupEn);
        Check(got > 0 && same == got, what2, detail);
      }
      if (want[i].perDevice)
      {
        _snprintf(what2, sizeof(what2), "%s: and they are IN those panes rather than one pane each",
                  want[i].id);
        _snprintf(detail, sizeof(detail), "(%d of %d named)", named, got);
        Check(got > 0 && named == got, what2, detail);
      }
    }

    bool hasSlider = false, hasKnob = false, hasToggle = false;
    {
      ApexQuickItem buf[32];
      ZeroMemory(buf, sizeof(buf));
      const int got = api->quickItems(buf, 32);
      for (int k = 0; k < got && k < 32; ++k)
      {
        if (buf[k].type == APEX_QUICK_SLIDER)
          hasSlider = true;
        if (buf[k].type == APEX_QUICK_KNOB)
          hasKnob = true;
        if (buf[k].type == APEX_QUICK_TOGGLE)
          hasToggle = true;
      }
    }
    if (want[i].slider)
      Check(hasSlider, "it offers a fader");
    if (want[i].knob)
      Check(hasKnob, "it offers a dial");
    if (want[i].toggle)
      Check(hasToggle, "it offers a switch");

    // ⚠️⚠️ AND FOR THE FEATURE WHOSE MAPPED SET IS A USER-MADE LIST, THE ROWS THE USER MADE ARE THE POINT (ABI
    // 16 -> 17). "One row" is what a fresh scratch folder contains (the 系统全局 row alone), so a count check
    // would pass for a feature that mapped nothing but the master row -- which is exactly what this feature did
    // BEFORE the change. What the user asked for is the LIST: "快速面板按列表显示系统和各应用的两个功能开关".
    // So a program is added through the same `listOp` the settings page uses, and the new row must appear with
    // BOTH of its switches, the second one being that row's own `display` path and the screen icon.
    if (strcmp(want[i].id, "KeepAwake") == 0)
    {
      Check(api->listOp && api->listOp("rules", "add", "probe-list.exe", -1) == 1,
            "a program can be added to the feature's list");
      static ApexQuickItem rows[32];
      ZeroMemory(rows, sizeof(rows));
      const int got = api->quickItems(rows, 32);
      Check(got == 2, "  and the flyout offers one row per program plus 系统全局", "");
      // The second row is the program that was just added -- and it is the PROGRAM row by the feature's own
      // numbering (rules[0] is the global row; see ApplyRowSwitch), which is what makes the switch it carries the
      // same control the settings page draws.
      int found = -1;
      for (int k = 0; k < got && k < 32; ++k)
        if (strcmp(rows[k].id, "rules[1].awake") == 0)
          found = k;
      _snprintf(what2, sizeof(what2), "  and it is the list's own row (rules[1]), with both of its switches");
      if (found >= 0)
      {
        _snprintf(detail, sizeof(detail), "id=\"%s\" companion=\"%s\" icon=%d label=\"%s\"", rows[found].id,
                  rows[found].toggleId, rows[found].toggleIcon, rows[found].labelZh);
        // ⚠️ AND NO ICON, WHICH IS THE USER'S CORRECTION RATHER THAN AN OMISSION: the companion of a SWITCH row is
        // drawn as a second switch ("是要用一样的滑动开关"), and `toggleIcon` is the picture of a BUTTON -- which the
        // host only consults for a fader row. A feature that sent one here would be sending a field nothing reads.
        Check(rows[found].type == APEX_QUICK_TOGGLE && strcmp(rows[found].toggleId, "rules[1].display") == 0 &&
                  rows[found].toggleIcon == APEX_QUICK_ICON_PLAIN,
              what2, detail);
      }
      else
        Check(false, what2, "(no item for rules[1] -- the list is not what got mapped)");
    }

    // (3) AND TURNING THEM OFF TAKES THEM BACK OUT. The whole point of a permission is that it can be
    // withdrawn; a switch that only ever adds is not one.
    for (int k = 0; k < 2 && want[i].mapping[k]; ++k)
      if (api->setControl)
        api->setControl(want[i].mapping[k], "0");
    _snprintf(what2, sizeof(what2), "%s: switching them off again empties the panel", want[i].id);
    _snprintf(detail, sizeof(detail), "(got %d)", api->quickItems(nullptr, 0));
    Check(api->quickItems(nullptr, 0) == 0, what2, detail);

    if (api->shutdown)
      api->shutdown();
    FreeLibrary(mod);
  }

  // AutoIME IS CHECKED WITHOUT init(). Its quickItems is null (its settings are rules in a JSON file, and its
  // "switch" is the host's own list), so there is nothing to call -- and init() would start its input-method
  // monitor, which is a real thing on a real desktop and has no business running inside a gate. What matters
  // here is the one thing that would make the whole feature vanish: the ABI it was built against.
  printf("  -- AutoIME (loaded, not started)\n");
  HMODULE ime = nullptr;
  char p[600];
  _snprintf(p, sizeof(p), "%s/AutoIME/AutoIME.dll", pluginsDir);
  ime = LoadLibraryA(p);
  if (!ime)
    Check(false, "AutoIME.dll loads");
  else
  {
    EntryFn entry = (EntryFn)(void *)GetProcAddress(ime, "ApexFeatureEntry");
    const ApexFeature *api = entry ? entry() : nullptr;
    Check(api && api->abiVersion == APEX_ABI_VERSION, "AutoIME: the ABI it was built against is this host's");
    Check(api && api->structSize == sizeof(ApexFeature), "AutoIME: its struct size matches the header's");
    Check(api && api->quickItems == nullptr, "AutoIME: no quick-panel controls (its settings are rules)");
    FreeLibrary(ime);
  }
}

static void TestMarquee()
{
  printf("\n== 4b. a label too long for its row: it scrolls while the pointer rests on it ==\n");
  // ⚠️ THE USER ASKED FOR THIS IN SO MANY WORDS: "快速面板显示和音量的设备名太长，鼠标移上去，可以滚动设备名". The
  // arithmetic is a pure function of (widths, time) precisely so that it can be checked HERE -- no window, no
  // clock, no monitor. What matters is that it HOLDS at the ends (a name is read from its beginning), that it
  // comes back rather than looping, and that it never reports an offset outside `0..overflow` (an offset that
  // overshot would show blank space or cut the tail off).
  const int boxW = 100;
  Check(LabelScrollOffset(80, boxW, 0) == 0 && LabelScrollOffset(80, boxW, 99999) == 0,
        "a label that fits never moves", "");
  Check(LabelScrollOffset(300, boxW, -5) == 0, "  and neither does one before the hover has started", "");

  const int textW = 300, overflow = textW - boxW; // 200 px of travel
  const int hold = 700, travelMs = overflow * 1000 / 40; // 40 px/s, as the function's own note says
  char d[128];
  _snprintf(d, sizeof(d), "hold=%d travel=%d ms", hold, travelMs);
  Check(LabelScrollOffset(textW, boxW, 100) == 0, "  it holds at the START while the user begins to read", d);
  {
    const int mid = LabelScrollOffset(textW, boxW, hold + travelMs / 2);
    _snprintf(d, sizeof(d), "half way -> %d px", mid);
    Check(mid > 0 && mid < overflow, "  then slides", d);
  }
  Check(LabelScrollOffset(textW, boxW, hold + travelMs + 100) == overflow,
        "  holds at the END, so the tail is readable too", "");
  {
    const int back = LabelScrollOffset(textW, boxW, hold + travelMs + hold + travelMs / 2);
    _snprintf(d, sizeof(d), "coming back -> %d px", back);
    Check(back > 0 && back < overflow, "  and comes back rather than jumping to the start", d);
  }
  {
    const int cycle = 2 * (hold + travelMs);
    Check(LabelScrollOffset(textW, boxW, cycle + 100) == LabelScrollOffset(textW, boxW, 100),
          "  the cycle repeats", "");
    bool inRange = true;
    for (int t = 0; t < cycle * 2; t += 37)
    {
      const int o = LabelScrollOffset(textW, boxW, t);
      if (o < 0 || o > overflow)
        inRange = false;
    }
    Check(inRange, "  and the offset is always inside 0..overflow", "");
  }
  // A label one pixel too wide must not be a special case: it scrolls one pixel.
  Check(LabelScrollOffset(boxW + 1, boxW, hold + travelMs) == 1,
        "a label one pixel too wide scrolls exactly that one pixel", "");
}

int main(int argc, char **argv)
{
  const char *root = argc > 1 ? argv[1] : ".";
  const char *plugins = argc > 2 ? argv[2] : "build/apex/Plugins";
  char scratch[600];
  _snprintf(scratch, sizeof(scratch), "%s/build/_quickpanel_probe_dir", root);
  printf("quickpanel probe: root=%s plugins=%s\n", root, plugins);

  // ⚠️ COM IS INITIALISED FOR THE FEATURES' SAKE, since ABI 13. MediaControl enumerates WASAPI sessions on
  // whichever thread calls it -- the host's UI thread in the real program, and this probe's main thread here.
  // Without this the feature would answer with zero application rows, and the check that "the flyout offers
  // exactly as many faders as the page has rows" would pass by having nothing on either side.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  TestLayout();
  TestValues();
  TestFade();
  TestPlacement();
  TestMarquee();
  TestPalette(root);
  TestFeatures(plugins, scratch);

  CoUninitialize();
  printf("\n%s (%d checks, %d failed)\n", failures ? "FAILED" : "ALL OK", checks, failures);
  return failures ? 1 : 0;
}
