// ---------------------------------------------------------------------------
// THE QUICK PANEL -- the small flyout the tray shows on a single click, drawn by the host itself.
//
// WHY THE HOST DRAWS IT AND NOT THE SETTINGS PANEL. The settings window is a browser in another process, and
// that is the right shape for a page with a sidebar, a chart and a rule editor. It is the wrong shape for one
// switch: the browser was MEASURED at ~0.55 s to appear (~1.4 s on the first run), and a user who clicks the
// tray wants the thing to be there. This window is created once, hidden, and lives in the host's own process,
// so a click is a SetWindowPos and a fade -- no process, no page, no startup.
//
// WHAT IT IS MADE OF:
//   * quickpanel.h -- WHERE everything goes, WHAT the mouse hit, and HOW LONG the fade takes. No Windows in
//     it, so a probe checks the arithmetic without a screen.
//   * this file     -- the window, the painting (GDI+ into a 32-bit DIB, blitted with UpdateLayeredWindow)
//     and the glue to the host: the feature list, the host's own `off` switches, and setControl.
//
// ⚠️ THE MATERIAL IS "LIKE MICA ALT", NOT MICA ALT, AND THAT IS A DECISION RATHER THAN A SHORTCUT. A DWM
// backdrop (DWMWA_SYSTEMBACKDROP_TYPE) makes the WINDOW's background the system's -- and a layered window,
// which is the only kind whose opacity can be animated, cannot have one. The user asked for
// "类似mica alt材质" and a 0.2-0.5 s fade; the fade is the thing a person actually sees, so the material is
// approximated (a soft vertical gradient, a hairline border, a round corner and a drawn shadow -- the four
// things that make a Mica surface read as one) and the whole window fades as a unit.
//
// ⚠️ AND THE COLOURS ARE THE SETTINGS PAGE'S COLOURS. The palette below is the same set of values as
// `apex/ui/panel.css` -- the user sees the flyout and the page minutes apart, and two near-but-not-equal greys
// look like a mistake. They are written out here because a C++ translation unit cannot read a stylesheet at
// run time; `test/check_apex_quickpanel.sh` reads BOTH files and fails when they drift, which is the only way
// this stays true (the same problem, and the same answer, as `--dot-on` in panel.css).
// ---------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
// ⚠️ objidl.h BEFORE gdiplus.h, AND IT IS NOT OPTIONAL WITH WIN32_LEAN_AND_MEAN. MinGW's gdiplus headers use
// `PROPID`, which is a COM type from propidl.h -- and the lean-and-mean windows.h deliberately does not pull
// the COM headers in, so gdiplus.h then fails with `'PROPID' has not been declared` from inside its own
// inline definitions. Including this one header first is the whole fix.
#include <objidl.h>
#include <gdiplus.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "abi.h"
#include "host.h"
#include "hostconfig.h"
#include "quickpaint.h" // everything about what the panel looks like (see the note further down)
#include "quickpalette.h"
#include "quickpanel.h"
#include "settings_ipc.h" // kStateQuickPanel -- telling the settings page what the user just changed here

namespace host = apex::host;
using apex::HostConfig;
using apex::LoadedFeature;
using apex::Loader;

namespace {

using namespace apex::quick;

const char kWndClass[] = "ApexQuickPanelWnd";
const UINT_PTR kAnimTimer = 1;
const int kAnimStepMs = 15;
// The scrolling label's own clock: 33 ms is a 30 fps marquee, which is smooth for text moving at 40 px/s and
// cheap enough to run only while a pointer rests on one row (see SyncMarqueeTimer). A separate id from the
// fade's, because the two animations answer different questions -- see the WM_TIMER case.
const UINT_PTR kMarqueeTimer = 2;
const int kMarqueeStepMs = 33;
// ... and the values timer, which asks each feature what its controls are at while the flyout is up (see
// PollFeatureValues). 400 ms is the same period the settings page polls at while it is told to keep re-reading.
const UINT_PTR kValueTimer = 3;
const int kValueStepMs = 400;
// ⚠️ AND THE WHEEL'S OWN "THE GESTURE IS OVER" CLOCK (see OnMouseWheel). A drag ends with a button release and a
// wheel has none, so the panel waits this long after the last notch before it rebuilds the row from what the
// feature says and tells the settings page -- one notification per gesture rather than one per notch, which is the
// same rule the drag follows (see OnLButtonUp). 140 ms is short enough to feel immediate and long enough that a
// hand turning a notched wheel does not fire it between two notches.
const UINT_PTR kSettleTimer = 4;
const int kSettleMs = 140;

// ⚠️ THE SHADOW IS PART OF THE WINDOW. A layered window gets no shadow from the DWM (the DWM does not decorate
// one), and a flyout with a hard edge floating over a light desktop looks like a rendering mistake. So the
// window is `kShadow` pixels larger than the panel on every side and the painter spends that margin on a soft
// edge. Everything else works in PANEL coordinates: the painter translates by kShadow, the hit test subtracts
// it, and PlaceAbove is given the panel's size rather than the window's.
const int kShadow = 12;
const int kGapAboveTray = 8; // between the tray icon and the panel's bottom edge

// (kMaxOwnPerFeature MOVED TO apex/quickpanel.h -- the host's glue needs the same bound, see the note there.)

// How long after a dismissal a tray click is still treated as "put that away" rather than "show it". It only
// has to outlast the fade-out plus the same click's own double-click wait (see QuickPanelToggle); it is not a
// user-facing number and nothing else depends on it.
const DWORD kDismissGuardMs = 400;

// ---- WHERE THE PICTURE LIVES ---------------------------------------------------------------------
//
// ⚠️ NOT HERE. The palette and every drawing helper -- the switch, the fader, the dial, a pane's own shadow,
// and the whole of `PaintPanel` -- are in apex/quickpaint.h, and the reason is not tidiness: the user has twice
// reported something purely about what the flyout LOOKS like, and both times the only instrument was their eye
// one deploy later. Putting the picture in a header that needs nothing but GDI+ lets `_diag/quickpanel_preview`
// render the SAME code to a PNG, so a layout question is answered by looking at it (see the note in
// quickpaint.h, and the same rule for the settings page in docs/rules/panel.md).
//
// This file is now only: the window, the drawing surface, the model it is built from, the fade, and the mouse.
// ---- the window's state ------------------------------------------------------------------------
struct Panel
{
  HWND hwnd = nullptr;
  ULONG_PTR gdip = 0;
  bool gdipUp = false;

  bool visible = false;
  bool fadingOut = false;
  DWORD animStart = 0;
  DWORD lastHidden = 0; // when it finished fading out, so the click that dismissed it does not reopen it

  int scale = 100;
  Model model;
  Metrics mx;
  Layout ly;

  int panelW = 0, panelH = 0; // the body, without the shadow margin
  int w = 0, h = 0;           // the whole window, shadow included
  int x = 0, y = 0;

  int hover = -1;
  DWORD hoverSince = 0; // when the pointer arrived on that row (drives the scrolling label)
  bool marquee = false; // is the label-scrolling timer running
  int drag = -1;        // the row being dragged, or -1
  double dragStart = 0; // a knob remembers the value it had when the button went down
  int dragY = 0;
  // ⚠️ THE WHEEL'S LEFTOVER (see WheelValueAfter): a notched mouse sends 120 at a time and this stays at 0, while a
  // trackpad's small deltas accumulate here until they add up to a notch. It is cleared whenever the pointer moves
  // to another row, so a few pixels of scrolling over one fader can never be spent on the next one.
  int wheelAccum = 0;
  int wheelRow = -1;

  HDC memDC = nullptr;
  HBITMAP hbm = nullptr;
  HGDIOBJ oldBmp = nullptr;
  void *bits = nullptr;
  int surfW = 0, surfH = 0;
};

Panel g;

LRESULT CALLBACK QuickWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

// Start or stop the label-scrolling timer to match the row under the pointer. Only a row whose label is too wide
// for it needs one -- the rest of the panel must not pay for an animation it does not have.
//
// ⚠️ DECLARED UP HERE BECAUSE THREE FUNCTIONS CLEAR THE HOVER AND ALL THREE MUST STOP THE TIMER WITH IT:
// `Rebuild` (the rows are about to be different ones), `FinishFadeOut` (nothing is hovered once it is hidden) and
// `OnMouseMove` itself. A hover cleared without this leaves a 33 ms timer redrawing a hidden window for ever.
void SyncMarqueeTimer();

// ---- the surface -------------------------------------------------------------------------------
void FreeSurface()
{
  if (g.memDC)
  {
    if (g.oldBmp)
      SelectObject(g.memDC, g.oldBmp);
    DeleteDC(g.memDC);
    g.memDC = nullptr;
    g.oldBmp = nullptr;
  }
  if (g.hbm)
  {
    DeleteObject(g.hbm);
    g.hbm = nullptr;
  }
  g.bits = nullptr;
  g.surfW = g.surfH = 0;
}

bool EnsureSurface(int w, int h)
{
  if (g.memDC && g.surfW == w && g.surfH == h)
    return true;
  FreeSurface();
  HDC screen = GetDC(nullptr);
  g.memDC = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  if (!g.memDC)
    return false;

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; // TOP-DOWN: GDI+ and UpdateLayeredWindow both want it this way round
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  g.hbm = CreateDIBSection(g.memDC, &bi, DIB_RGB_COLORS, &g.bits, nullptr, 0);
  if (!g.hbm || !g.bits)
  {
    FreeSurface();
    return false;
  }
  g.oldBmp = SelectObject(g.memDC, g.hbm);
  g.surfW = w;
  g.surfH = h;
  return true;
}

// Paint the whole panel at `alpha` and hand it to the window in one blit. UpdateLayeredWindow is the only
// blit that carries per-pixel alpha, and per-pixel alpha is what the round corner and the shadow are made of;
// it also takes the window's opacity in the SAME call, which is what makes the fade one operation instead of
// "repaint everything with a different blend factor".
bool Render(double alpha)
{
  if (!g.hwnd || g.w <= 0 || g.h <= 0)
    return false;
  if (!EnsureSurface(g.w, g.h))
    return false;

  {
    // ⚠️ THE BITMAP DOES NOT OWN THIS MEMORY -- GDI+ is painting straight into the DIB that
    // UpdateLayeredWindow is about to read. That is the point (no copy per frame) and it is also why the
    // Graphics object is destroyed before the blit.
    Gdiplus::Bitmap bmp(g.surfW, g.surfH, g.surfW * 4, PixelFormat32bppPARGB, (BYTE *)g.bits);
    Gdiplus::Graphics gfx(&bmp);
    if (gfx.GetLastStatus() != Gdiplus::Ok)
      return false;
    // ⚠️ THE DIB IS REUSED EVERY FRAME, SO IT IS WIPED HERE AND NOT IN THE PAINTER. The painter deliberately
    // does not clear -- the preview draws a stand-in desktop under the panel so that its transparency is
    // visible in the PNG (see the note in quickpaint.h).
    gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
    // ⚠️ THE PAINTER TAKES EVERYTHING IT NEEDS AS AN ARGUMENT (see PaintCtx in quickpaint.h): the preview probe
    // calls the SAME function with a different model, and a painter that read this file's globals would draw
    // this window's panel into the probe's picture -- silently, because it would still draw something.
    PaintCtx ctx;
    ctx.model = &g.model;
    ctx.layout = &g.ly;
    ctx.panelW = g.panelW;
    ctx.panelH = g.panelH;
    ctx.scale = g.scale;
    ctx.light = apex::ThemeResolvesLight(host::SettingsConfig()->theme, host::SystemIsLightTheme());
    ctx.zh = host::UiIsChinese();
    ctx.hover = g.hover;
    // ⚠️ THE ELAPSED HOVER TIME IS ONLY NON-ZERO WHEN THE PANEL IS FULLY UP. During a fade the label must hold
    // still: two motions at once (a fade and a scroll) is one more than the user asked for, and the label would
    // arrive on screen already half-scrolled past its own beginning.
    ctx.hoverMs = (g.hover >= 0 && !g.fadingOut && alpha >= 1.0) ? (int)(GetTickCount() - g.hoverSince) : 0;
    ctx.shadow = kShadow;
    // ... and the gap between two panes, which the painter uses to decide whether there is room for a shadow
    // (see the reach in PaintPanel); it comes from the same metrics the layout was measured with.
    ctx.sectionGap = g.mx.sectionGap;
    PaintPanel(gfx, ctx, alpha);
    gfx.Flush(Gdiplus::FlushIntentionSync);
  }

  HDC screen = GetDC(nullptr);
  POINT dst = {g.x, g.y};
  POINT src = {0, 0};
  SIZE size = {g.w, g.h};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(alpha * 255.0 + 0.5), AC_SRC_ALPHA};
  const BOOL ok = UpdateLayeredWindow(g.hwnd, screen, &dst, &size, g.memDC, &src, 0, &bf, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  return ok != FALSE;
}

// ---- the model ---------------------------------------------------------------------------------
//
// ⚠️ REBUILT, NOT CACHED, AND ON PURPOSE. Every value in this panel lives in a FEATURE or in the host's own
// settings, and both can change while the flyout is up (the settings page is a separate process and may be
// open; a feature's own worker thread can change one; the user changed one a moment ago in this very panel).
// A cached model would be a picture of the state as it was when the panel opened -- which is exactly the class
// of bug this project has had to fix twice on the settings page.
void Rebuild()
{
  HostConfig *cfg = host::SettingsConfig();
  Loader *ld = host::SettingsLoader();
  // ⚠️ THE ROW UNDER THE POINTER IS REMEMBERED ACROSS A REBUILD, and it now matters: Rebuild also runs every
  // 400 ms while a feature is following something outside the panel (see PollFeatureValues), and clearing the
  // hover there would restart the scrolling label -- and un-highlight the row -- several times a second under a
  // pointer that has not moved.
  char hoverId[kIdLen] = {0};
  int hoverSlot = -1;
  if (g.hover >= 0 && g.hover < g.model.itemCount)
  {
    CopyStr(hoverId, kIdLen, g.model.items[g.hover].id);
    hoverSlot = g.model.items[g.hover].slot;
  }
  g.model = Model();
  g.hover = -1;
  g.drag = -1;
  SyncMarqueeTimer();
  if (!cfg || !ld)
    return;

  // (1) EVERY FEATURE'S OWN ON/OFF SWITCH, in the compact grid. This list is the HOST's, not a feature's (see
  // `off` in hostconfig.h), so this half of the panel works with no feature implementing anything.
  if (cfg->quickCompact)
  {
    int live = 0;
    for (int i = 0; i < ld->Count(); ++i)
      if (ld->At(i).ok && ld->At(i).api)
        ++live;
    if (live > 0)
    {
      // "插件 / Features" -- the panel's own words, not a feature's, so they are written here in both
      // languages rather than sent up from a DLL.
      const int sec = g.model.AddSection("\xe6\x8f\x92\xe4\xbb\xb6", "Features");
      for (int i = 0; i < ld->Count() && sec >= 0; ++i)
      {
        const LoadedFeature &f = ld->At(i);
        if (!f.ok || !f.api || !f.api->id)
          continue;
        Item *it = g.model.AddItem(sec, RowKind::kFeatureSwitch, f.api->id);
        if (!it)
          break;
        it->slot = i;
        CopyStr(it->labelZh, kLabelLen, f.api->nameZh);
        CopyStr(it->labelEn, kLabelLen, f.api->nameEn);
        it->on = !cfg->FeatureOff(f.api->id);
      }
    }
  }

  // (2) WHAT EACH FEATURE ASKS FOR -- ⚠️ ONE CONTROL, ONE BLOCK.
  //
  // ⚠️⚠️ THE GROUPING IS THE USER'S RULE AND IT IS NOT BY FEATURE: "快速面板局部功能分组逻辑不以插件分组，而是以
  // 一个开关为一组，后面做这个都会这么设定". So a feature's controls are NOT collected into one block named after
  // the feature -- each one floats in a pane of its own, and the pane IS the grouping. That is also why such a
  // block has no heading line (see `single` in MeasureModel): the control's own label is the heading.
  if (cfg->quickOwn)
  {
    // ⚠️⚠️ BLOCK BY BLOCK, IN THE ORDER THE USER PUT THEM IN (host.h `QuickBlocks`, `quickOrder` in
    // hostconfig.h). The General page has a small panel that lists exactly these -- what the flyout draws as one
    // pane, and in what order -- and lets the user drag them; the two read the SAME function, so the list the
    // user is looking at cannot disagree with the panel they are looking at.
    //
    // ⚠️ THE UNIT IS THE PANE, NOT THE FEATURE: a feature's controls are one pane per control (or per NAMED group,
    // which is what MediaControl and KeepAwake send), so "which features, in what order" is not the same question
    // -- see the note on QuickBlock in host.h. And `QuickBlocks` has already dropped everything that maps nothing
    // and every feature the user has switched off, which is why the checks below are cheap rather than the question.
    static host::QuickBlock blocks[host::kMaxQuickBlocks];
    const int blockCount = host::QuickBlocks(blocks, host::kMaxQuickBlocks);
    static ApexQuickItem raw[kMaxOwnPerFeature];
    int fetchedSlot = -1;
    int fetchedCount = 0;
    for (int bi = 0; bi < blockCount; ++bi)
    {
      const int i = blocks[bi].slot;
      if (i < 0 || i >= ld->Count())
        continue;
      const LoadedFeature &f = ld->At(i);
      if (!f.ok || !f.api || !f.api->quickItems)
        continue;
      // ONE CALL PER FEATURE, NOT ONE PER BLOCK: a feature with two panes (MediaControl's brightness and volume)
      // must not make it enumerate the machine twice. The blocks of one feature are not necessarily adjacent, so
      // the buffer is refetched whenever the feature changes.
      if (i != fetchedSlot)
      {
        ZeroMemory(raw, sizeof(raw));
        host::SetCurrentFeature(f.api->id);
        const int n = f.api->quickItems(raw, kMaxOwnPerFeature);
        host::SetCurrentFeature(nullptr);
        fetchedSlot = i;
        fetchedCount = n < kMaxOwnPerFeature ? (n > 0 ? n : 0) : kMaxOwnPerFeature;
      }
      // (The feature was switched off AFTER this list was built: impossible within one build, and the skip is
      //  here for the same reason the layout checks its own inputs.)
      const bool off = cfg->FeatureOff(f.api->id);
      for (int k = 0; k < fetchedCount; ++k)
      {
        const ApexQuickItem &q = raw[k];
        if (!q.id[0])
          continue; // nothing to send a value to
        // ⚠️ THIS ITEM'S BLOCK, COMPUTED THE SAME WAY `QuickBlocks` COMPUTED IT (one helper, so the item that
        // goes in the pane and the pane's place in the order cannot come from two different ideas of "which
        // block is this").
        char key[112] = {0};
        apex::quick::QuickBlockKey(f.api->id, q.groupZh, q.id, key, sizeof(key));
        if (strcmp(key, blocks[bi].key) != 0)
          continue; // another pane's item
        RowKind kind;
        switch (q.type)
        {
        case APEX_QUICK_TOGGLE: kind = RowKind::kToggle; break;
        case APEX_QUICK_SLIDER: kind = RowKind::kSlider; break;
        case APEX_QUICK_KNOB: kind = RowKind::kKnob; break;
        // An unknown shape is SKIPPED rather than guessed at: a control drawn as the wrong shape is a control
        // that lies about what it does (see APEX_QUICK_* in abi.h).
        default: continue;
        }

        // ⚠️ A NAMED GROUP SHARES ONE PANE; AN UNNAMED CONTROL GETS ITS OWN (abi.h, ABI 12 -> 13). The second
        // brightness fader must land in the pane the first one made -- `FindOrAddSection` is that, and it is
        // deliberately NOT what the block above uses: a feature's own switch grid is one block because the host
        // made it, not because two features agreed on a name.
        const int sec = q.groupZh[0] ? g.model.FindOrAddSection(q.groupZh, q.groupEn)
                                     : g.model.AddSection(q.labelZh, q.labelEn);
        if (sec < 0)
          break; // out of blocks: the panel is full (see kMaxSections)
        Item *it = g.model.AddItem(sec, kind, q.id);
        if (!it)
          break;
        it->slot = i;
        CopyStr(it->labelZh, kLabelLen, q.labelZh);
        CopyStr(it->labelEn, kLabelLen, q.labelEn);
        CopyStr(it->groupZh, kGroupLen, q.groupZh);
        CopyStr(it->groupEn, kGroupLen, q.groupEn);
        CopyStr(it->unit, kUnitLen, q.unit);
        it->min = q.min;
        it->max = q.max;
        it->step = q.step;
        it->value = q.value;
        it->hue = q.hue;
        // ⚠️ THE COMPANION SWITCH BESIDE THE CONTROL (ABI 14 -> 15) -- empty when the feature sent none, which is
        // what most rows do. It is a control path like `id`, so nothing else here has to know what it means.
        CopyStr(it->toggleId, kIdLen, q.toggleId);
        it->toggleOn = q.toggleOn != 0;
        // ⚠️ AND WHICH PICTURE TO DRAW IT WITH (ABI 15 -> 16). The mapping from the ABI's number to this header's
        // own enum happens HERE, once, and an unknown value becomes the plain mark rather than a guess: the
        // drawing has three icons, and "a control the host does not understand" must not borrow one of them.
        switch (q.toggleIcon)
        {
        case APEX_QUICK_ICON_MUTE: it->icon = CompanionIcon::kMute; break;
        case APEX_QUICK_ICON_DISPLAY: it->icon = CompanionIcon::kDisplay; break;
        default: it->icon = CompanionIcon::kPlain; break;
        }
        it->on = q.value >= 0.5;
        it->featureOff = off;
      }
    }
  }

  // Nothing at all -- which the user can arrange with the two switches in Settings > General. Said in words
  // rather than shown as a blank rectangle: a click that produces an empty box is indistinguishable from a
  // broken program, and the panel is the only place that can explain itself.
  if (g.model.itemCount == 0)
  {
    const int sec = g.model.AddSection("\xe5\xbf\xab\xe9\x80\x9f\xe9\x9d\xa2\xe6\x9d\xbf", "Quick panel");
    Item *it = g.model.AddItem(sec, RowKind::kNote, "");
    if (it)
    {
      CopyStr(it->labelZh, kLabelLen,
              "\xe5\x9c\xa8 \xe8\xae\xbe\xe7\xbd\xae \xe2\x86\x92 \xe9\x80\x9a\xe7\x94\xa8 \xe9\x87\x8c\xe9\x80\x89"
              "\xe6\x8b\xa9\xe8\xbf\x99\xe9\x87\x8c\xe6\x98\xbe\xe7\xa4\xba\xe4\xbb\x80\xe4\xb9\x88");
      CopyStr(it->labelEn, kLabelLen, "Choose what shows here in Settings \xe2\x86\x92 General");
    }
  }

  // ... and the row under the pointer comes back, if it is still there (see the note at the top of Rebuild).
  if (hoverId[0])
  {
    for (int i = 0; i < g.model.itemCount; ++i)
      if (g.model.items[i].slot == hoverSlot && strcmp(g.model.items[i].id, hoverId) == 0)
      {
        g.hover = i;
        break;
      }
  }
  if (g.hover < 0)
    g.hoverSince = 0;
  SyncMarqueeTimer();
}

// ⚠️⚠️ THE FEATURE'S OWN VALUES ARE POLLED WHILE THE FLYOUT IS UP, AND THIS IS THE USER'S OTHER HALF OF THE
// "FOLLOW THE REAL SCREEN" REQUEST. The settings page follows a brightness key because the FEATURE tells it to
// keep re-reading (`waiting`); the flyout has no such loop -- it asks `quickItems` when it opens and after every
// change the user makes IN it, so a fader sitting there while the laptop's brightness keys are pressed shows a
// value from whenever the panel opened. The user's report: "快速面板的推子没跟着动".
//
// ⚠️ IT COMPARES VALUES AND REBUILDS ONLY ON A CHANGE, rather than rebuilding on a timer: the panel is a layered
// window whose whole surface is blitted on every paint, and a rebuild every 400 ms would also restart the
// scrolling label and re-highlight whatever is under the pointer. This asks a cheap question and does nothing
// when the answer is the same.
void PollFeatureValues()
{
  if (!g.visible || g.fadingOut)
    return;
  Loader *ld = host::SettingsLoader();
  if (!ld)
    return;
  ApexQuickItem raw[kMaxOwnPerFeature];
  bool changed = false;
  for (int i = 0; i < ld->Count() && !changed; ++i)
  {
    const LoadedFeature &f = ld->At(i);
    if (!f.ok || !f.api || !f.api->quickItems)
      continue;
    ZeroMemory(raw, sizeof(raw));
    host::SetCurrentFeature(f.api->id);
    const int total = f.api->quickItems(raw, kMaxOwnPerFeature);
    host::SetCurrentFeature(nullptr);
    const int taken = total < kMaxOwnPerFeature ? (total > 0 ? total : 0) : kMaxOwnPerFeature;
    for (int k = 0; k < taken && !changed; ++k)
    {
      if (!raw[k].id[0])
        continue;
      for (int r = 0; r < g.model.itemCount; ++r)
        if (g.model.items[r].slot == i && strcmp(g.model.items[r].id, raw[k].id) == 0)
        {
          // A value that moved, or a control that disappeared: both mean the picture is out of date. The item
          // COUNT is not compared directly -- a stale row is caught by the loop above finding no match for it
          // only if the feature stopped sending it, which is what the total check below is for.
          //
          // ⚠️ AND THE COMPANION SWITCH'S STATE COUNTS TOO (ABI 14 -> 15): a mute that was set somewhere else has
          // to show up here, exactly like a fader that followed the screen.
          if (g.model.items[r].value != raw[k].value || g.model.items[r].toggleOn != (raw[k].toggleOn != 0))
            changed = true;
          break;
        }
    }
  }
  if (changed)
  {
    Rebuild();
    Render(1.0);
  }
}

// ---- geometry ----------------------------------------------------------------------------------
void Relayout()
{
  // ⚠️ THE SCALE IS ASKED OF THE WINDOW, NOT OF THE SCREEN. Apex declares per-monitor DPI awareness (see
  // apex.manifest), so a flyout beside a 150% display on a 100% one has to be sized for the monitor its tray
  // icon is on -- which is the monitor the panel is about to be placed on, not necessarily the primary.
  UINT dpi = 96;
  if (g.hwnd)
  {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32)
    {
      typedef UINT(WINAPI * GetDpiFn)(HWND);
      GetDpiFn fn = (GetDpiFn)(void *)GetProcAddress(user32, "GetDpiForWindow");
      if (fn)
        dpi = fn(g.hwnd);
    }
  }
  g.scale = dpi ? (int)(dpi * 100 / 96) : 100;
  if (g.scale < 50)
    g.scale = 50;
  g.mx = DefaultMetrics(g.scale);
  g.panelH = MeasureModel(g.model, g.mx, &g.ly);
  g.panelW = g.mx.width;
  if (g.panelH <= 0)
    g.panelH = Scaled(40, g.scale); // a window is never zero-sized; the caller decides whether to show it
  g.w = g.panelW + 2 * kShadow;
  g.h = g.panelH + 2 * kShadow;
}

bool Place()
{
  int ax = 0, ay = 0, aw = 0, ah = 0;
  const bool anchored = host::TrayIconScreenRect(&ax, &ay, &aw, &ah);
  if (!anchored)
  {
    // ⚠️ THE SHELL DOES NOT ALWAYS KNOW, and the fallback is not decoration: an icon in the overflow flyout, a
    // tray that has just restarted, or a shell that answers with a failure all mean "no rectangle". Putting the
    // panel at the cursor is where the user's attention already is, which is the honest second-best answer.
    POINT pt;
    GetCursorPos(&pt);
    ax = pt.x;
    ay = pt.y;
    aw = ah = 0;
  }

  HMONITOR mon = MonitorFromPoint(POINT{ax + aw / 2, ay + ah / 2}, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  RECT work;
  if (GetMonitorInfoA(mon, &mi))
    work = mi.rcWork;
  else
  {
    work.left = 0;
    work.top = 0;
    work.right = GetSystemMetrics(SM_CXSCREEN);
    work.bottom = GetSystemMetrics(SM_CYSCREEN);
  }

  const Rect anchor{ax, ay, aw, ah};
  const Rect area{work.left, work.top, work.right - work.left, work.bottom - work.top};
  int px = 0, py = 0;
  PlaceAbove(anchor, area, g.panelW, g.panelH, Scaled(kGapAboveTray, g.scale), &px, &py);
  g.x = px - kShadow; // the window is the panel plus its shadow margin on every side
  g.y = py - kShadow;
  return anchored;
}

// ---- showing and hiding ------------------------------------------------------------------------
// ⚠️⚠️ THERE IS NO ACRYLIC, NO `SetWindowCompositionAttribute`, AND NOTHING ELSE THAT ASKS THE DWM TO PAINT
// ANYTHING. That is not an omission: it was tried and removed, and why it cannot come back is the point.
//
// WHAT HAPPENED. The user asked for more glass ("透明度不够，玻璃感要加强下"), so the panel asked the DWM for
// `ACCENT_ENABLE_ACRYLICBLURBEHIND`. The call was accepted -- and the panel then read, on screen, as
// "还是有**大面板**". The accent is painted over THE WHOLE WINDOW RECTANGLE: the window IS a rectangle (it has
// to be -- see the note on kShadow), so the blur and its tint covered the gaps between the panes and the margin
// around them as well, and every pane ended up lying on one flat sheet. That is precisely the "大板装着小板" the
// user had already asked to be rid of, re-created by a call meant to make the glass better.
//
// ⚠️ AND THE TWO ARE MUTUALLY EXCLUSIVE, WHICH IS THE REAL LESSON: the accent fills the window's rectangle,
// while "the gaps between the panes are the desktop" requires that rectangle to be mostly transparent. No
// arrangement of the two has both. The user chose the panes -- twice -- and a 78% tint with a hairline edge and
// a highlight along the top is what carries the glass.
//
// ⚠️⚠️ AND IT COST A ROUND TRIP BECAUSE THE PREVIEW CANNOT SEE IT. `_diag/quickpanel_preview.cpp` renders
// `PaintPanel` -- the GDI+ drawing -- and the accent is NOT IN that drawing: the window manager applies it to
// the window. So the picture was right and the SCREEN was wrong, and every pixel assertion passed. That
// boundary is written down where the preview is described, and the gate now reads this file to check that the
// API is never called again.

void StartFadeIn()
{
  if (!g.hwnd)
    return;
  Relayout();
  const bool anchored = Place();
  SetWindowPos(g.hwnd, HWND_TOPMOST, g.x, g.y, g.w, g.h, SWP_NOACTIVATE);
  // ⚠️ THE BLIT'S OWN ANSWER, LOGGED, because it is the only evidence this window exists at all: the panel
  // belongs to the window, and nothing outside this process can read it back (short of capturing the screen).
  // An UpdateLayeredWindow that fails leaves an invisible window and a flyout that "does nothing".
  const bool painted = Render(0.0);
  ShowWindow(g.hwnd, SW_SHOW);
  // ⚠️ FOREGROUND, BECAUSE THE ONLY WAY TO LEARN "the user clicked somewhere else" IS TO LOSE IT. A window that
  // is never activated never receives WM_ACTIVATE(WA_INACTIVE), and there is no other reliable signal for a
  // click outside -- so the panel holds the focus while it is up and the fade-out is the answer to losing it.
  // (This is what every tray flyout does, and the call is allowed here because it happens inside the message
  // the user's own click produced.)
  //
  // ⚠️ EXCEPT UNDER THE GATE'S SWITCH (host::QuickPanelSelfTest): taking the focus would pull it away from
  // whatever the person at the machine is typing into, and a test that interrupts them is not a test.
  const BOOL fg = host::QuickPanelSelfTest() ? TRUE : SetForegroundWindow(g.hwnd);
  g.visible = true;
  g.fadingOut = false;
  g.animStart = GetTickCount();
  SetTimer(g.hwnd, kAnimTimer, kAnimStepMs, nullptr);
  // ⚠️ AND THE VALUES ARE POLLED FROM THE MOMENT IT IS UP (see PollFeatureValues): a fader in here has to follow
  // the real screen, which the settings page already does. Started here and stopped in FinishFadeOut, so a hidden
  // flyout costs nothing at all.
  SetTimer(g.hwnd, kValueTimer, kValueStepMs, nullptr);
  host::SettingsLog("quickpanel: shown -- %d rows in %d blocks, %dx%d at %d,%d (%s theme, %s) blit=%s%s%s",
                    g.model.itemCount, g.model.sectionCount, g.w, g.h, g.x, g.y,
                    apex::ThemeResolvesLight(host::SettingsConfig()->theme, host::SystemIsLightTheme()) ? "light"
                                                                                                      : "dark",
                    host::UiIsChinese() ? "zh" : "en", painted ? "ok" : "FAILED",
                    anchored ? "" : " [the shell would not say where the tray icon is -- placed at the cursor]",
                    fg ? "" : " [FOREGROUND REFUSED: clicking outside may not dismiss it]");
}

void StartFadeOut(const char *why)
{
  if (!g.hwnd || !g.visible || g.fadingOut)
    return;
  g.fadingOut = true;
  g.animStart = GetTickCount();
  SetTimer(g.hwnd, kAnimTimer, kAnimStepMs, nullptr);
  host::SettingsLog("quickpanel: hiding -- %s", why);
}

void FinishFadeOut()
{
  if (g.hwnd)
  {
    KillTimer(g.hwnd, kAnimTimer);
    KillTimer(g.hwnd, kValueTimer); // a hidden flyout asks nothing (see PollFeatureValues)
    KillTimer(g.hwnd, kSettleTimer); // ... and a wheel gesture that was in flight is over too
    ShowWindow(g.hwnd, SW_HIDE);
  }
  g.visible = false;
  g.fadingOut = false;
  g.hover = -1;
  g.hoverSince = 0;
  g.drag = -1;
  g.wheelAccum = 0;
  g.wheelRow = -1;
  SyncMarqueeTimer(); // nothing is hovered once it is hidden, and a timer left running would redraw for ever
  g.lastHidden = GetTickCount();
}

// ---- the input ---------------------------------------------------------------------------------
// Which feature a row belongs to. The row was built from ONE feature's answer or from the host's own switch
// grid, and the slot is what says which -- the section index is not it, because the grid is one section for
// every feature (see Rebuild).
const LoadedFeature *FeatureFor(const Item &it)
{
  Loader *ld = host::SettingsLoader();
  if (!ld || it.slot < 0 || it.slot >= ld->Count())
    return nullptr;
  const LoadedFeature &f = ld->At(it.slot);
  return (f.ok && f.api) ? &f : nullptr;
}

void PushValue(int index)
{
  if (index < 0 || index >= g.model.itemCount)
    return;
  const Item &it = g.model.items[index];
  const LoadedFeature *f = FeatureFor(it);
  if (!f || !f->api->setControl)
    return;

  char text[64];
  if (it.kind == RowKind::kToggle)
    _snprintf(text, sizeof(text), "%d", it.on ? 1 : 0);
  else
    _snprintf(text, sizeof(text), "%.6g", it.value);

  host::SetCurrentFeature(f->api->id);
  const int took = f->api->setControl(it.id, text);
  host::SetCurrentFeature(nullptr);
  if (took)
    host::SettingsTouch();
  host::SettingsLog("quickpanel: %s.%s = %s -- %s", f->api->id, it.id, text, took ? "taken" : "REFUSED");
}

void ToggleFeatureSwitch(int index)
{
  HostConfig *cfg = host::SettingsConfig();
  if (!cfg || index < 0 || index >= g.model.itemCount)
    return;
  Item &it = g.model.items[index];
  const bool on = !it.on;
  if (cfg->FeatureSetOff(it.id, !on))
  {
    it.on = on;
    host::SettingsLog("quickpanel: feature %s is now %s", it.id, on ? "on" : "OFF");
    host::SettingsTouch();
    // ⚠️ AND THE SETTINGS PAGE IS TOLD AT ONCE (the user's rule: "快速面板的开关要能实时同步到设置面板上").
    // The feature's page draws this very switch, and the page can be open behind the flyout -- it keeps the
    // focus nowhere, but it is still on screen, and an answer that is one click out of date is the thing this
    // exists to remove.
    host::PanelStateChanged(apex::kStateQuickPanel);
  }
}

// ⚠️ THE COMPANION SWITCH BESIDE A CONTROL (abi.h: `toggleId`, ABI 14 -> 15) -- the mute button on a volume fader,
// which is what it was added for ("音量在推子右边增加静音按钮"), and, since ABI 16 -> 17, the screen button beside a
// keep-awake row's own switch. It is a control path like any other, so the value it sends is "1" or "0" and the
// feature owns what that means; the row is rebuilt from the feature's answer on the next frame, which is what makes
// a feature that refuses the change visible instead of stuck.
//
// ⚠️ AND IT IS THE SAME PATH A TOGGLE ROW TAKES (`PushValue`, which formats the item's own `on`), except that the
// thing being written is not the row's primary control: a fader's value and its mute are two controls in one row,
// and so are a program's two switches -- sending the first one's value to the second one's path would be a value
// the feature has to reject.
void PushExtra(int index)
{
  if (index < 0 || index >= g.model.itemCount)
    return;
  const Item &it = g.model.items[index];
  if (!it.toggleId[0])
    return;
  const LoadedFeature *f = FeatureFor(it);
  if (!f || !f->api->setControl)
    return;

  const char *text = it.toggleOn ? "1" : "0";
  host::SetCurrentFeature(f->api->id);
  const int took = f->api->setControl(it.toggleId, text);
  host::SetCurrentFeature(nullptr);
  if (took)
    host::SettingsTouch();
  host::SettingsLog("quickpanel: %s.%s = %s -- %s", f->api->id, it.toggleId, text, took ? "taken" : "REFUSED");
}

// How wide a label roughly is, without a device context: ASCII ~7 px, CJK ~13 px per character at 100%.
//
// ⚠️ THIS IS ONLY USED TO DECIDE WHETHER TO START A TIMER, and that is why an estimate is allowed here while the
// DRAWING measures for real (MeasureTextW in quickpaint.h). A wrong "no" costs nothing (the label keeps its
// ellipsis, which is what it had before this feature existed); a wrong "yes" costs an animation timer on a label
// that does not move -- and the drawing itself computes an offset of 0 in that case, so nothing moves.
int ApproxLabelW(const char *s, int scale)
{
  int w = 0;
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;)
  {
    if (*p < 0x80)
    {
      w += 7;
      ++p;
    }
    else if ((*p & 0xE0) == 0xC0)
    {
      w += 13;
      p += 2;
    }
    else if ((*p & 0xF0) == 0xE0)
    {
      w += 13;
      p += 3;
    }
    else
    {
      w += 13;
      p += 4;
    }
  }
  return w * scale / 100;
}

// Start or stop the label-scrolling timer to match the row under the pointer. Only a row whose label is too wide
// for it needs one -- the rest of the panel must not pay for an animation it does not have.
void SyncMarqueeTimer()
{
  bool want = false;
  if (g.visible && !g.fadingOut && g.hover >= 0 && g.hover < g.model.itemCount)
  {
    const Item &it = g.model.items[g.hover];
    if (it.kind == RowKind::kSlider || it.kind == RowKind::kKnob || it.kind == RowKind::kToggle)
      want = ApproxLabelW(LabelOf(it, host::UiIsChinese()), g.scale) > g.ly.labels[g.hover].w;
  }
  if (want == g.marquee)
    return;
  g.marquee = want;
  if (want)
    SetTimer(g.hwnd, kMarqueeTimer, kMarqueeStepMs, nullptr);
  else
    KillTimer(g.hwnd, kMarqueeTimer);
}

void OnMouseMove(int cx, int cy, bool dragging)
{
  if (g.fadingOut)
    return; // on the way out: nothing to hover, and re-rendering at full opacity would fight the fade
  const int px = cx - kShadow, py = cy - kShadow;
  if (!dragging)
  {
    const int hit = HitTest(g.model, g.ly, px, py);
    if (hit != g.hover)
    {
      g.hover = hit;
      // ⚠️ THE CLOCK STARTS WHEN THE POINTER ARRIVES, not when the panel opened: a label that was already
      // half-scrolled when the user's eye reached it would be unreadable. See LabelScrollOffset in quickpanel.h.
      g.hoverSince = GetTickCount();
      // ... and the wheel's own remainder belongs to the row it was collected on (see `wheelAccum`).
      g.wheelAccum = 0;
      g.wheelRow = hit;
      SyncMarqueeTimer();
      Render(1.0);
    }
    return;
  }
  if (g.drag < 0 || g.drag >= g.model.itemCount)
    return;
  Item &it = g.model.items[g.drag];
  double v = it.value;
  if (it.kind == RowKind::kSlider)
    v = SliderValueAt(it, g.ly.control[g.drag], px);
  else if (it.kind == RowKind::kKnob)
    v = KnobValueAt(it, g.dragStart, py - g.dragY, g.ly.rows[g.drag].h);
  else
    return;
  if (v == it.value)
    return;
  it.value = v;
  PushValue(g.drag);
  Render(1.0);
}

void OnLButtonDown(int cx, int cy)
{
  const int px = cx - kShadow, py = cy - kShadow;
  // ⚠️ WHAT THE CLICK DOES IS ASKED OF `ClickAt`, NOT OF `HitTest`: "which row is this" and "which control am I
  // operating" are two different questions, and the user's report -- "鼠标点击改变数值只在推子内，现在点到推子左右区域都会
  // 改变" -- is what happens when one answer is used for both. A click on a fader's own rectangle is an absolute
  // jump and starts a drag; a click on the row but beside the fader does NOTHING.
  const Hit hit = ClickAt(g.model, g.ly, px, py);
  if (hit.index < 0)
    return;
  Item &it = g.model.items[hit.index];
  if (hit.kind == HitKind::kToggle)
  {
    if (it.kind == RowKind::kFeatureSwitch)
    {
      ToggleFeatureSwitch(hit.index);
      Render(1.0);
      return;
    }
    it.on = !it.on;
    PushValue(hit.index);
    Render(1.0);
    // A switch is one click, so the page can be told on this very click (see ToggleFeatureSwitch).
    host::PanelStateChanged(apex::kStateQuickPanel);
    return;
  }
  if (hit.kind == HitKind::kExtra)
  {
    it.toggleOn = !it.toggleOn;
    PushExtra(hit.index);
    Render(1.0);
    // One click, so the page hears about it now -- the same rule as a switch (see OnLButtonUp for why a DRAG is
    // different).
    host::PanelStateChanged(apex::kStateQuickPanel);
    return;
  }
  if (hit.kind == HitKind::kSlider || hit.kind == HitKind::kKnob)
  {
    g.drag = hit.index;
    g.dragStart = it.value;
    g.dragY = py;
    SetCapture(g.hwnd);
    if (hit.kind == HitKind::kSlider)
    {
      const double v = SliderValueAt(it, g.ly.control[hit.index], px);
      if (v != it.value)
      {
        it.value = v;
        PushValue(hit.index);
        Render(1.0);
      }
    }
  }
}

void OnLButtonUp()
{
  if (g.drag < 0)
    return;
  ReleaseCapture();
  g.drag = -1;
  // ⚠️ THE FEATURE HAS THE LAST WORD, AND THIS IS WHERE IT GETS IT. Every step of the drag went through
  // setControl, but a feature may clamp, snap to its own grid or refuse -- so the panel REBUILDS from what the
  // feature says it has now, rather than leaving the mouse's last position on the screen (see `quickItems`).
  Rebuild();
  Relayout();
  Render(1.0);
  host::SettingsTouch();
  // ⚠️ ONE NOTIFICATION PER DRAG, NOT ONE PER PIXEL. Every mouse position sent a value (as it must -- that is
  // what dragging is), but the settings page redraws itself when it is told, and a redraw per pixel is a page
  // that cannot be read. The drag has ended: this is the moment the answer is final, and the moment to send it.
  host::PanelStateChanged(apex::kStateQuickPanel);
}

// ⚠️ THE WHEEL OVER A FADER (the user's "推子可以用鼠标滚轮操作；这条追加适配到快速面板所有推子的操作").
//
// WHERE THE POINT COMES FROM, AND WHY THAT IS NOT A DETAIL. `WM_MOUSEWHEEL` carries SCREEN coordinates, because
// Windows sends the message to the FOCUSED window rather than to the window under the pointer -- and this panel
// deliberately holds the focus while it is up (that is how it learns that the user clicked elsewhere, see
// StartFadeIn). So the message can arrive while the pointer is somewhere else entirely, and the position in it is
// the only thing that says which control the user is pointing at. It is therefore converted and tested against the
// panel's own rows, and a wheel that is not over this panel's fader does nothing at all -- which also means the
// panel can never swallow a scroll meant for the window behind it.
void OnMouseWheel(int cx, int cy, int delta)
{
  if (!g.visible || g.fadingOut)
    return;
  const int px = cx - kShadow, py = cy - kShadow;
  // A wheel over a row that is NOT a fader (the label of one, a switch, the gaps) does nothing -- the same rule a
  // click follows (see ClickAt): the fader takes the wheel, not the row it happens to be on.
  const int row = WheelTarget(g.model, g.ly, px, py);
  if (row < 0)
    return;
  if (row != g.wheelRow)
  {
    // ⚠️ THE REMAINDER IS DROPPED RATHER THAN CARRIED ACROSS ROWS: half a notch collected over one fader must not
    // complete itself over the next one when the pointer passes over it.
    g.wheelRow = row;
    g.wheelAccum = 0;
  }
  Item &it = g.model.items[row];
  const double v = WheelValueAfter(it, delta, &g.wheelAccum);
  if (v == it.value)
    return; // not a whole notch yet, or already at the end: nothing to report and nothing to redraw
  it.value = v;
  // ⚠️ THE SAME PATH A DRAG TAKES, deliberately: one `setControl` per notch, and the row is rebuilt from the
  // feature's answer when the wheel stops (see OnLButtonUp's note on the one notification per gesture).
  PushValue(row);
  Render(1.0);
  // ⚠️ AND THE PAGE IS TOLD ONCE, after the last notch of this gesture. The wheel has no "button up" to hang that
  // on, so it is the wheel timer below that decides the gesture is over -- see kSettleTimer.
  SetTimer(g.hwnd, kSettleTimer, kSettleMs, nullptr);
}

} // namespace

// ---- the window --------------------------------------------------------------------------------
namespace {

LRESULT CALLBACK QuickWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  case WM_TIMER:
    if (wp == kAnimTimer)
    {
      const int elapsed = (int)(GetTickCount() - g.animStart);
      const double a = g.fadingOut ? FadeOutAlpha(elapsed) : FadeInAlpha(elapsed);
      Render(a);
      if (!g.fadingOut && a >= 1.0)
        KillTimer(h, kAnimTimer);
      else if (g.fadingOut && a <= 0.0)
        FinishFadeOut();
      return 0;
    }
    // ⚠️ THE SCROLLING LABEL'S OWN TIMER, SEPARATE FROM THE FADE'S. They can both want to run (the pointer can
    // arrive while the panel is still fading in), and they want different things from `Render`: the fade is
    // drawing at a changing alpha, this one at full opacity. Sharing one timer would make the label's position
    // depend on the fade's clock, which is exactly the kind of coupling that shows up later as "the label jumps".
    if (wp == kMarqueeTimer)
    {
      if (!g.visible || g.fadingOut || g.hover < 0)
      {
        SyncMarqueeTimer(); // it has stopped being wanted: shut it down on the next tick
        return 0;
      }
      Render(1.0);
      return 0;
    }
    // ⚠️ AND THE VALUES A FEATURE IS FOLLOWING FROM OUTSIDE (see PollFeatureValues): this is what makes a fader
    // in the flyout move while the laptop's brightness keys are being pressed.
    if (wp == kValueTimer)
    {
      PollFeatureValues();
      return 0;
    }
    // ⚠️ THE WHEEL HAS STOPPED (see OnMouseWheel): the gesture is over, so the row is rebuilt from what the
    // feature says it now holds -- and only now is the settings page told, once, rather than per notch.
    if (wp == kSettleTimer)
    {
      KillTimer(h, kSettleTimer);
      if (g.visible && !g.fadingOut)
      {
        Rebuild();
        Relayout();
        Render(1.0);
        host::SettingsTouch();
        host::PanelStateChanged(apex::kStateQuickPanel);
      }
      return 0;
    }
    break;

  case WM_MOUSEMOVE:
    OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp), (wp & MK_LBUTTON) != 0);
    return 0;

  // ⚠️ THE WHEEL OVER A FADER TURNS IT (the user's own request: "推子可以用鼠标滚轮操作"). It arrives here
  // because this window holds the focus while it is up -- and it carries SCREEN coordinates, because Windows
  // sends the message to the FOCUSED window rather than to the one under the pointer; see OnMouseWheel for why
  // that matters and what the point is checked against.
  case WM_MOUSEWHEEL:
  {
    POINT pt;
    pt.x = (short)LOWORD(lp);
    pt.y = (short)HIWORD(lp);
    ScreenToClient(h, &pt);
    OnMouseWheel(pt.x, pt.y, (short)HIWORD(wp));
    return 0;
  }

  case WM_LBUTTONDOWN:
    OnLButtonDown((short)LOWORD(lp), (short)HIWORD(lp));
    return 0;

  case WM_LBUTTONUP:
    OnLButtonUp();
    return 0;

  // CLICKED SOMEWHERE ELSE. This is the whole dismissal rule the user asked for ("鼠标点击其它位置时快速面板消失"),
  // and it is why the panel takes the foreground when it appears: losing the activation IS the click.
  case WM_ACTIVATE:
    if (LOWORD(wp) == WA_INACTIVE && g.visible && !g.fadingOut)
      StartFadeOut("the user clicked somewhere else");
    return 0;

  // The system's light/dark setting or its language changed while the flyout was up. (The theme is usually
  // changed FROM a window the user just clicked, which would have dismissed the panel -- but a scheduled switch
  // lands here, and so does the language.)
  case WM_SETTINGCHANGE:
  case WM_THEMECHANGED:
    if (g.visible)
      apex::host::QuickPanelRefresh();
    return 0;

  case WM_DESTROY:
    FreeSurface();
    g.hwnd = nullptr;
    g.visible = false;
    return 0;
  }
  return DefWindowProcA(h, msg, wp, lp);
}

} // namespace

namespace apex {
namespace host {

void QuickPanelInit()
{
  if (g.hwnd)
    return;
  if (!g.gdipUp)
  {
    Gdiplus::GdiplusStartupInput in;
    g.gdipUp = Gdiplus::GdiplusStartup(&g.gdip, &in, nullptr) == Gdiplus::Ok;
  }
  WNDCLASSA wc = {};
  wc.lpfnWndProc = QuickWndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = kWndClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassA(&wc);
  // ⚠️ LAYERED (the fade and the round corner), TOOLWINDOW (never in alt-tab -- this is not a window the user
  // switches to), TOPMOST (it sits above the tray, which is itself always on top).
  g.hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWndClass, "Apex quick panel",
                           WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  SettingsLog("quickpanel: window %s (GDI+ %s)", g.hwnd ? "created" : "COULD NOT BE CREATED",
              g.gdipUp ? "up" : "FAILED TO START");
}

bool QuickPanelVisible() { return g.visible && !g.fadingOut; }

const char *QuickPanelWndClass() { return kWndClass; }

void QuickPanelHide(const char *why) { StartFadeOut(why ? why : "asked to"); }

void QuickPanelShow()
{
  if (g.visible && !g.fadingOut)
    return;
  QuickPanelInit();
  if (!g.hwnd)
    return;
  Rebuild();
  StartFadeIn();
}

// ⚠️ A SINGLE CLICK ON THE TRAY, ARRIVING ONE DOUBLE-CLICK TIMEOUT AFTER THE CLICK. main.cpp waits, because
// the same icon opens the settings on a double click and one press cannot mean both -- so this is called only
// once the wait is over and no second click came.
//
// ⚠️ AND IT REFUSES TO REOPEN WHAT THE USER JUST DISMISSED. Clicking the tray while the panel is up lands
// OUTSIDE the panel (the tray is not part of it), so the panel starts fading before this call arrives -- and
// without the guard below it would close and immediately reappear, which reads as "the tray button does
// nothing". The guard covers the case where the fade has already finished; while it is still running,
// `g.visible` is true and this takes the hide branch instead.
void QuickPanelToggle()
{
  if (g.visible)
  {
    StartFadeOut("the tray was clicked again");
    return;
  }
  if (g.lastHidden != 0 && GetTickCount() - g.lastHidden < kDismissGuardMs)
    return;
  QuickPanelShow();
}

// The language or the theme changed under a panel that is up: redraw it in the new one.
void QuickPanelRefresh()
{
  if (!g.hwnd || !g.visible)
    return;
  Rebuild();
  Relayout();
  Render(g.fadingOut ? 0.0 : 1.0);
}

} // namespace host
} // namespace apex
