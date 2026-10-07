// ---------------------------------------------------------------------------
// SMOOTH WHEEL SCROLL, as an Apex feature.
//
// THIS FILE IS THE FEATURE, and it is deliberately thin: the model is the shared one (shared/model.h, the
// same header the REAPER plugin compiles) and the behaviour it had as a standalone app is unchanged. What
// is different is only WHERE the pieces come from:
//
//     before (standalone)                now (an Apex feature)
//     ------------------------------     ---------------------------------------------
//     its own WH_MOUSE_LL hook       ->  the host's hook calls onWheel()
//     its own 4 ms waitable timer    ->  the host's clock calls tick()
//     its own SendInput thread       ->  host.injectDeltas()
//     its own target lookup          ->  host.targetAt()
//     its own settings file          ->  Plugins\SmoothWheel\SmoothWheel.ini
//
// EVERYTHING ELSE IS THE SAME CODE, including the two rules that were measured and must not be "tidied":
//
//   * the travel comes from model::Travel with the speed budget, and NOTHING is scaled after it. A gain on
//     the output was tried and removed: it moves the ceiling with the floor and puts "how far a notch
//     goes" in two places.
//   * AppEaseFor() decides the payout shape per message, and it is the app's own rule (the model's is
//     right for a roll that keeps going, wrong for one that stops). Its reasoning is in common/core.h.
//
// ⚠️ THE MODEL IS A COPY, NOT A SHARED FILE, AND THAT IS A DECISION WITH A COST.
//
// Until 2026-09-18 this project lived inside the plugin's repository as app/, so "the same header" meant the
// same bytes -- one file, no drift possible. The split into two projects ended that: shared/ now holds a
// COPY of the plugin's four model headers, taken at the split (see shared/SOURCE.md for the origin and the
// md5s). A model change made on the plugin side has to be copied here by hand.
//
// That is the accepted trade: the model is finished (the user's words: "模型已经近乎完美...有的就是接递层的
// 事"), so the copy is expected to sit still. WHAT MUST NOT HAPPEN is silent divergence -- if the two ever
// disagree, the plugin and Apex stop feeling identical, which is the one promise this design makes. So:
// changing a model header means updating BOTH, and shared/SOURCE.md is where the fingerprints are compared.
// ---------------------------------------------------------------------------

// The ABI comes from apex/; the settings and the core from common/; the model from shared/. All three are on
// the include path (see apex/build.sh), so these are bare names with no ../.. to get wrong.
#include "abi.h"

#include "config.h"
#include "core.h"
#include "model.h"
#include "release.h" // the window the chart stands for, so the picture follows the delivery
#include "device.h"  // which DEVICE sent this wheel -- a touchpad is not ours to smooth

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetTickCount64, and the file calls at the bottom of this file

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

namespace {

const ApexHost *g_host = nullptr;
app::Config g_cfg;
app::Core g_core;
// THE RUNNING EVIDENCE OF WHICH DEVICE THIS GESTURE IS, one per feature instance. It lives here rather than in
// the host for the same reason the exclude list does: "do not smooth a touchpad" is a statement about smoothing.
// See common/device.h (the rule) and doc/rules/wheel.md (why it is the feature's).
app::DeviceTracker g_device;

double g_lastMsgSec = 0.0;
double g_owed = 0.0; // travel the model has produced but not yet handed to the host

char g_dir[512] = {0};

void Log(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[256] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_host->logLine(buf);
}

// ---- the settings file --------------------------------------------------------------------------
//
// In the feature's OWN folder (Plugins\SmoothWheel\), so moving the folder moves the settings and two
// features cannot write over each other's keys. The rules for what is in it and what the ranges are live
// in app/config.h -- that file is shared with the host's own copy of the same logic and is pure, so it is
// tested without a platform (test/check_app_config.sh).

bool ConfigPath(char *out, int outSize)
{
  if (!g_dir[0])
    return false;
  const int n = _snprintf(out, outSize, "%sSmoothWheel.ini", g_dir);
  return n > 0 && n < outSize;
}

bool LoadSettings()
{
  char path[560] = {0};
  if (!ConfigPath(path, (int)sizeof(path)))
    return false;
  FILE *f = fopen(path, "rb");
  if (!f)
    return false; // not an error: the defaults stand and the file is written on the way out
  static char text[8192];
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = 0;
  return app::ParseConfigText(text, g_cfg);
}

bool SaveSettings()
{
  char path[560] = {0};
  if (!ConfigPath(path, (int)sizeof(path)))
    return false;
  static char text[8192];
  app::FormatConfigText(g_cfg, text, (int)sizeof(text));
  char tmp[600] = {0};
  if (_snprintf(tmp, sizeof(tmp), "%s.tmp", path) <= 0)
    return false;
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return false;
  fputs(text, f);
  fclose(f);
  return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}

// ---- the panel's data ---------------------------------------------------------------------------
//
// JSON BY HAND, because this is the only place a feature has to build any and pulling in a library for
// eight numbers would be the largest thing in the DLL. The panel parses it with JSON.parse, so the
// escaping rules are the ones JSON requires: quote and backslash escaped. Nothing here can contain a
// control character, so no further escaping is needed.
void Append(char *out, int outSize, int &off, const char *fmt, ...)
{
  if (off >= outSize)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + off, outSize - off, fmt, ap);
  va_end(ap);
  if (n > 0)
    off += n;
  if (off > outSize)
    off = outSize;
}

// A slider: id, label, range, step, current value, unit, and the value a double-click restores.
// The panel renders whatever it is given, so feature #2 needs no UI work.
//
// `def` IS SENT SO THE PANEL CAN RESET WITHOUT KNOWING ANYTHING. The alternative -- the panel asking the
// feature to reset a control by id -- needs another entry point and another round trip for a number the
// feature is already holding while it describes the control.
// THE FOUR CHANNEL COLOURS, taken from the REAPER plugin (1.7.1) so the two products look like each other.
// Each is the hue of ONE slider, and the panel blends it with its own background by the slider's amount -- so a
// parameter at 30% is a faint version of its hue and one at 100% is the full colour, in BOTH the row and the
// piece of curve it moves. (The blending is the panel's job: the theme is the panel's.)
#define kHueGlide "#78BEFF" // Glide length   -- blue:   the horizontal scale
#define kHueSlow  "#FFBE78" // Slow step      -- orange: the knee
#define kHueRamp  "#B4DC8C" // Ramp-up        -- green:  the climb
#define kHueTop   "#C8AAFF" // Top speed      -- violet: the ceiling

// THE NAME OF THE BOARD THE FOUR PARAMETERS SHARE IN THE QUICK PANEL (see `groupZh`/`groupEn` in abi.h). It is
// this feature's own words for the four numbers, in both languages, because the panel draws the reader's one and
// this side does not know which that is. ⚠️ It is NOT the feature's name: a pane in the flyout is a grouping of
// CONTROLS ("快速面板局部功能分组逻辑不以插件分组", the user's rule), and what these four controls have in common is
// that they are the scroll's motion.
#define kQuickGroupZh "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x8f\x82\xe6\x95\xb0"         // 滚动参数
#define kQuickGroupEn "Scroll motion"

// `seg` IS WHICH PIECE OF THE CHART THIS SLIDER OWNS, and it is sent HERE, beside the `hue` -- one place
// writes both, so a slider and the piece of curve it moves cannot come to be different colours.
//
// ⚠️ IT IS `-1` FOR A SLIDER THAT OWNS NO SEGMENT, and there is no such slider in this feature today. The
// panel treats a segment no slider claims as "draw it in the foreground colour", which is honest and visible;
// what was NOT honest was the version before this, where the panel held a hard-coded list of this feature's
// ids (`slow`/`ramp`/`top`) and any other feature's curve came out in a single colour.
void AddRange(char *out, int outSize, int &off, bool &first, const char *id, const char *zh,
              const char *en, double lo, double hi, double step, double val, const char *unit,
              double def, const char *hue, int seg)
{
  Append(out, outSize, off,
         "%s{\"id\":\"%s\",\"type\":\"range\",\"labelZh\":\"%s\",\"labelEn\":\"%s\","
         "\"min\":%.4g,\"max\":%.4g,\"step\":%.4g,\"value\":%.4g,\"def\":%.4g,\"unit\":\"%s\","
         "\"hue\":\"%s\",\"seg\":%d}",
         first ? "" : ",", id, zh, en, lo, hi, step, val, def, unit, hue, seg);
  first = false;
}

// A JSON string literal, escaped. ⚠️ EVERY STRING THIS FEATURE SENDS MUST GO THROUGH HERE, including the ones
// it "knows" are safe.
//
// The list entries below are TEXT A USER TYPED. `NormaliseExe` trims them to a bare file name and lower-cases
// them, but it does not reject a quote -- so one entry containing `"` produces a document that the panel's
// JSON.parse rejects, and the symptom is a feature page that draws nothing and reports nothing. (The host
// escapes its OWN strings -- see Json::Str in settings_host.cpp -- but it forwards this document verbatim: it
// does not know where one of our strings starts or ends, so it cannot fix them for us.)
void AppendJsonString(char *out, int outSize, int &off, const char *s)
{
  Append(out, outSize, off, "\"");
  if (s)
  {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
    {
      const unsigned char c = *p;
      if (c == '"')
        Append(out, outSize, off, "\\\"");
      else if (c == '\\')
        Append(out, outSize, off, "\\\\");
      else if (c < 0x20)
        Append(out, outSize, off, "\\u%04x", c);
      else
        Append(out, outSize, off, "%c", c); // UTF-8 passes through as-is: JSON is defined on the bytes
    }
  }
  Append(out, outSize, off, "\"");
}

// A SWITCH. It is the same shape every other feature's `bool` is, and there are exactly two here: the pair
// that decide whether a control of this feature's appears in the QUICK PANEL (see the fields in
// common/config.h). They are drawn in the middle of this feature's own page, right after the control each one
// governs, because that is where the question is asked.
void AddBool(char *out, int outSize, int &off, bool &first, const char *id, const char *zh, const char *en,
             bool value)
{
  Append(out, outSize, off,
         "%s{\"id\":\"%s\",\"type\":\"bool\",\"labelZh\":\"%s\",\"labelEn\":\"%s\",\"value\":%d}",
         first ? "" : ",", id, zh, en, value ? 1 : 0);
  first = false;
}

// THE SENTENCE BESIDE 「排除」, IN BOTH LANGUAGES, FROM THE ENGINES THE HOST REPORTS (ABI 20 -> 21).
//
// ⚠️ THE WORDS ARE THE USER'S, VERBATIM: one engine is "REAPER专用引擎已运行", and more than one is written as one
// block -- "REAPER、Lertaro专用引擎已运行" -- the names joined with 、and the tail written once. The user's rule for
// the order: "谁先运行谁显式在前面".
//
// ⚠️ THE ORDER IS THE HOST'S AND IS NOT TOUCHED HERE. It is start order, earliest first, and it is the whole
// reason the ABI carries a list instead of a set: sorting (or re-sorting) here would be a second opinion about a
// question that already has an answer, and the answer would then depend on which side you asked.
//
// ⚠️ AND THE PANEL CANNOT DO THIS, which is why the sentence is built here: the panel draws a `noteZh`/`noteEn`
// string it is handed and knows nothing about engines -- it never learns what REAPER or Lertaro is (see cases.md).
static void EngineNote(const ApexEngine *engines, int n, char *zh, int zhSize, char *en, int enSize)
{
  if (zh && zhSize > 0)
    zh[0] = 0;
  if (en && enSize > 0)
    en[0] = 0;
  if (!zh || zhSize <= 0 || !en || enSize <= 0)
    return;

  int zoff = 0, eoff = 0;
  int shown = 0;
  for (int i = 0; i < n; ++i)
  {
    // ⚠️ THE NAME COMES FROM THE HOST, and this feature deliberately keeps NO kind -> word table of its own (see
    // ApexEngine in abi.h). A table here would have to be edited for every program added, and a program nobody
    // remembered to add would be dropped by this very branch -- in silence, on a note whose whole job is to explain
    // smoothing the user cannot account for.
    const char *name = engines[i].name;
    if (!name[0])
      continue;

    // Chinese: the names, joined with 、.
    int wrote = _snprintf(zh + zoff, (size_t)(zhSize - zoff), "%s%s", shown ? "\xe3\x80\x81" : "", name);
    if (wrote > 0)
      zoff += wrote;

    // English: "The A", "The A and B", "The A, B and C".
    wrote = _snprintf(en + eoff, (size_t)(enSize - eoff), "%s%s%s", shown == 0 ? "The " : "",
                      shown == 0 ? "" : (i == n - 1 ? " and " : ", "), name);
    if (wrote > 0)
      eoff += wrote;
    ++shown;
  }
  if (shown == 0)
  {
    zh[0] = 0;
    en[0] = 0;
    return;
  }
  _snprintf(zh + zoff, (size_t)(zhSize - zoff),
            "\xe4\xb8\x93\xe7\x94\xa8\xe5\xbc\x95\xe6\x93\x8e\xe5\xb7\xb2\xe8\xbf\x90\xe8\xa1\x8c"); // 专用引擎已运行
  _snprintf(en + eoff, (size_t)(enSize - eoff), shown == 1 ? " engine is running" : " engines are running");
}

// A list the user edits -- this feature's blacklist. THE PANEL KNOWS NOTHING ABOUT BLACKLISTS: it draws a
// list, sends "add"/"remove" through ApexFeature::listOp, and re-reads. That is why a future feature with
// no blacklist costs the panel nothing, and one with a different KIND of list needs no panel change either.
void AddList(char *out, int outSize, int &off, bool &first, const char *id, const char *zh,
             const char *en, const char *placeholderZh, const char *placeholderEn,
             const char *noteZh, const char *noteEn)
{
  Append(out, outSize, off,
         "%s{\"id\":\"%s\",\"type\":\"list\",\"labelZh\":\"%s\",\"labelEn\":\"%s\","
         "\"placeholderZh\":\"%s\",\"placeholderEn\":\"%s\",\"values\":[",
         first ? "" : ",", id, zh, en, placeholderZh, placeholderEn);
  for (int i = 0; i < g_cfg.skipN; ++i)
  {
    if (i)
      Append(out, outSize, off, ",");
    AppendJsonString(out, outSize, off, g_cfg.skip[i]); // USER TEXT: escaped, see above
  }
  Append(out, outSize, off, "]");
  // THE NOTE BESIDE THE LABEL, and it is sent ONLY WHEN IT IS TRUE. The panel draws whatever it is handed and
  // makes no judgement about it, so the whole decision -- "which external engines are running right now" -- lives
  // where the fact is (EngineNote above), and the panel never learns what REAPER or Lertaro is.
  //
  // ⚠️ THE TEXT IS THE USER'S, VERBATIM: "REAPER专用引擎已运行", and one block for several -- "REAPER、Lertaro专用
  // 引擎已运行". It is a statement of fact in their own words, and paraphrasing a sentence they wrote would be
  // answering a different request.
  if (noteZh && *noteZh)
  {
    Append(out, outSize, off, ",\"noteZh\":");
    AppendJsonString(out, outSize, off, noteZh);
    Append(out, outSize, off, ",\"noteEn\":");
    AppendJsonString(out, outSize, off, noteEn ? noteEn : noteZh);
  }
  Append(out, outSize, off, "}");
  first = false;
}
// ---- the motion chart ---------------------------------------------------------------------------
//
// A PICTURE OF WHAT THE FOUR SLIDERS DO, built so each slider owns something visible and no two share. It is
// the REAPER plugin's chart (1.7.0/1.7.1, where the model and this panel's design came from), sent as numbers
// instead of drawn with GDI.
//
// THE CHANNELS, and this is the whole design -- a slider with no visible consequence is a slider the user
// cannot reason about:
//
//   Glide length -> the HORIZONTAL SCALE. The x axis is real milliseconds and spans the motion's own length,
//                   so stretching Glide stretches the picture and the tick numbers follow it.
//   Slow step    -> the KNEE's HEIGHT: how far a single slow notch gets, against the wheel's own line.
//   Ramp-up      -> WHERE THE CLIMB REACHES THE TOP: a shorter ramp reaches full speed sooner, so the diagonal
//                   is steeper.
//   Top speed    -> the VERTICAL SCALE. The box is `top` times the wheel's own travel, so the wheel's line
//                   sits at 1/top of it: at 1.0 the flat top sits exactly ON that line, at 2.0 the line is
//                   half-way down -- which is what "twice as high" looks like.
//
// THREE LINES, and they answer different questions:
//
//   * the SHAPED path -- the smoothed motion, in three coloured segments (see below), with the corners
//     FILLETED because the motion it stands for has no kink in it;
//   * the NATIVE line -- the wheel's own height, at 1/top of the box;
//   * the WHEEL's own path -- a dotted staircase, one whole step per notch. It spans the same width as the
//     shaped path because the two are two answers to the same input, and they must be read against one time
//     axis. This is also what makes `slowStep` legible: the knee is measured against the wheel's own message
//     size, so on a notched mouse it is `slowStep/120` of the way up and on a free-spinner it is most of it.
//
// THE COLOURS COME FROM THE SLIDERS THEMSELVES (the `hue` on each control): the rise is Slow step's, the
// climb is Ramp-up's, the flat top is Top speed's, and the baseline/frame is Glide's. So a segment and the
// fader that moves it are the same colour by construction rather than by two tables kept in step.
//
// ALL FOUR PARAMETERS HAVE A CHANNEL, and that is a rule rather than a coincidence: a slider with nothing
// visible attached to it is a slider the user cannot reason about. The two scales cover the other two -- Glide
// is the x axis' real length, Top speed is the y axis' real height (and both are labelled in their own units,
// so moving either one moves NUMBERS on the screen).

// A "nice" tick step (1, 2 or 5 times a power of ten) that divides `range` into at most `maxTicks` parts --
// and, among those, the FINEST one that fits.
//
// Finest-that-fits matters: rounding range/maxTicks UP to the next 1/2/5 quantises up, so asking for more ticks
// can land on the same step and give FEWER of them. This scans from the finest candidate instead, so a bigger
// maxTicks really does mean a denser grid.
double NiceStep(double range, int maxTicks)
{
  if (range <= 0.0 || maxTicks < 1)
    return 0.0;
  const double raw = range / (double)maxTicks;
  double mag = pow(10.0, floor(log10(raw)));
  for (int k = 0; k < 30; ++k)
  {
    const double mult[3] = {1.0, 2.0, 5.0};
    for (int i = 0; i < 3; ++i)
    {
      const double step = mag * mult[i];
      if (range / step <= (double)maxTicks + 1e-9)
        return step;
    }
    mag *= 10.0;
  }
  return range;
}

// A straight piece from (x0,y0) to (x1,y1), evaluated at x.
double ChartLineY(double x, double x0, double y0, double x1, double y1)
{
  return (x1 > x0) ? (y0 + (y1 - y0) * ((x - x0) / (x1 - x0))) : y1;
}

// A quadratic through (xc-half, y0) - (xc, yc) - (xc+half, y2), evaluated at x, where the control point IS the
// sharp corner: the curve bends smoothly through where the kink used to be. Only the Y is interpolated this
// way; the X is the linear map t = (x-xc+half)/(2*half), which is exactly the quadratic's own x-component for
// symmetric ends, so the curve is still parameterised by x.
double ChartFilletY(double x, double y0, double xc, double yc, double y2, double half)
{
  if (half <= 0.0)
    return yc;
  double t = (x - xc + half) / (2.0 * half);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  const double u = 1.0 - t;
  return u * u * y0 + 2.0 * t * u * yc + t * t * y2;
}

// The shaped path's height (0..1 of the box) at x (0..1 of the drawing). Three pieces and two fillets:
//
//   0 .. kneeX    the RISE   -- ends at the knee, whose height is Slow step;
//   kneeX..reachX the CLIMB  -- reaches the top; how early is Ramp-up (earlier = steeper);
//   reachX..1.0   the TOP    -- a HORIZONTAL line at the top, whose height is Top speed.
//
// The corners are rounded because drawn with sharp kinks the picture was three straight lines meeting at two
// corners, which reads as something mechanical; the motion it stands for has no kink at all (the model is a
// smooth glide), so each corner is a short quadratic instead. (The plugin's user asked for exactly that:
// "线段两个拐角给个倒角，让线段看起来平滑些，如实际体验".)
double ChartPathY(double x, double kneeX, double reachX, double kneeY, double peakY)
{
  // The corner's half-width, limited by what the neighbouring pieces can spare, so two fillets can never
  // overlap or run off either end of the drawing.
  double d = 0.10; // kChartCorner
  const double riseLen = kneeX;
  const double climbLen = reachX - kneeX;
  const double topLen = 1.0 - reachX;
  if (d > riseLen * 0.5) d = riseLen * 0.5;
  if (d > climbLen * 0.4) d = climbLen * 0.4; // both corners eat into this piece
  if (d > topLen * 0.5) d = topLen * 0.5;
  if (d < 1e-6) d = 0.0;

  if (x < kneeX - d)
    return ChartLineY(x, 0.0, 0.0, kneeX, kneeY);
  if (x < kneeX + d)
    return ChartFilletY(x, ChartLineY(kneeX - d, 0.0, 0.0, kneeX, kneeY), kneeX, kneeY,
                        ChartLineY(kneeX + d, kneeX, kneeY, reachX, peakY), d);
  if (x < reachX - d)
    return ChartLineY(x, kneeX, kneeY, reachX, peakY);
  if (x < reachX + d)
    return ChartFilletY(x, ChartLineY(reachX - d, kneeX, kneeY, reachX, peakY), reachX, peakY, peakY, d);
  return peakY; // the flat top: the highest line, and the only horizontal one
}

// WHICH SLIDER OWNS A GIVEN x. Sent per point so the panel can colour the stroke by segment -- the panel has no
// idea what the pieces mean, which is what keeps it a generic control renderer.
int ChartSegAt(double x, double kneeX, double reachX)
{
  return (x < kneeX) ? 0 : ((x < reachX) ? 1 : 2);
}

// A point of a polyline: "x,y" -- the arrays above are read by a panel that draws numbers, not objects, and a
// flat pair is the smallest thing that can cross that boundary.
void AddPoint(char *out, int outSize, int &off, int i, double x, double y)
{
  Append(out, outSize, off, "%s[%.4g,%.4g]", i ? "," : "", x, y);
}

void AddCurve(char *out, int outSize, int &off, const app::Config &cfg)
{
  // ---- THE BOX IS SIZED SO THE CURVE FILLS IT, AND THE AXES CARRY THE REAL UNITS ----
  //
  // ⚠️ THIS IS THE PLUGIN'S GEOMETRY (1.7.1), AND THE OBVIOUS ALTERNATIVE IS WRONG. Fixing the axes to the
  // model's whole range and drawing the motion inside them looks like the honest thing to do, and it is not:
  // at the shipped defaults the motion is 200 ms of a 400 ms axis and 1080 of a 1440 delta axis, so the curve
  // sits in the corner of an empty box and the picture is hard to read. The user's words for that version:
  // "又空又难读". A chart's frame exists to be filled.
  //
  // So: the box is `top` times the wheel's own travel tall and as wide as the motion is long, and the curve
  // reaches its top and right edge. What the axes give up in fixed reference they give back in LABELS -- the
  // ticks are in real milliseconds and real wheel deltas, so the scale is readable and it MOVES with the
  // settings. (A fixed 1/6 grid would sit in the same place whatever the sliders said; that is decoration.)
  //
  // WHAT EACH SLIDER MOVES, and this is the whole design -- a slider with no visible consequence is a slider
  // the user cannot reason about:
  //
  //   Slow step   -> the KNEE's height, as a fraction of the box.
  //   Ramp-up     -> where along x the climb reaches the top.
  //   Top speed   -> the WHEEL'S OWN LINE, drawn at 1/top of the box. The shaped curve always reaches the top
  //                  (that is the ceiling), and this reference is what it is measured against: at 1.0 the two
  //                  touch, at 2.0 the wheel's line is half-way down -- "twice as high" as a picture.
  //   Glide       -> the X AXIS ITSELF: how many milliseconds the box spans, and therefore every number along
  //                  it, and how long a ball takes to cross. It does NOT reshape the curve, and it cannot: the
  //                  shape of the payout over one window does not depend on how long the window is. Its channel
  //                  is time, and time is what the x axis is. It moves the axis in company with the release
  //                  model's fixed addition -- see where `windowMs` is set below.
  const double notch = 120.0; // the wheel's own message size this picture is drawn for (a notched mouse)
  const int notches = 6;      // the picture's scale: six notches across the box
  const double top = (cfg.topSpeed < app::Config::TopLo()) ? app::Config::TopLo() : cfg.topSpeed;

  // THE WINDOW THIS PICTURE STANDS FOR IS THE ROLL'S. The curve drawn here has a knee and a climb, and those
  // only exist while windows OVERLAP -- so the motion it depicts is a roll's, and a roll's window is the Glide
  // setting plus the release model's fixed addition (see common/release.h). Drawing the lone-message window
  // instead would make the box shorter than the motion it is a picture of. Glide still moves the axis: it is
  // inside this number, and it is the only thing the user can move.
  const double windowMs = app::ReleaseRollWindowMs(cfg.WindowMsFor());
  const double natTop = 1.0 / top;                    // where the wheel's own line sits in the box
  const double yTopDeltas = top * notches * notch;    // the box's height, in wheel deltas
  // The drawn span, in milliseconds. The plugin's own formula: it is a little more than the window, so the
  // motion does not stop exactly at the frame's edge, and it scales with Glide so the box is always used.
  const double spanMs = (150.0 + 2.5 * windowMs) * 0.45;

  // THE SHAPE, as a fraction of the box. `kneeFrac` is slowStep measured against the wheel's OWN message size --
  // not against an arbitrary fraction of the picture: 10 deltas is 10/120 of a notched mouse's notch but most of
  // a free-spinner's much smaller message, and that ratio IS the parameter's meaning.
  double kneeFrac = cfg.slowStep / notch;
  if (kneeFrac > 1.0) kneeFrac = 1.0;
  if (kneeFrac < 0.02) kneeFrac = 0.02; // never flat on the floor, so the segment still has a colour to show
  const double kneeX = 0.20;            // where the rise ends
  const double kneeY = kneeFrac * natTop; // the knee is under the wheel's own line, never above it
  const double rampN = (cfg.rampUp - app::Config::RampLo()) /
                       (app::Config::RampHi() - app::Config::RampLo());
  const double reachX = 0.34 + (0.88 - 0.34) * (rampN < 0 ? 0 : (rampN > 1 ? 1 : rampN));

  const int n = 96; // enough for a smooth fillet at the panel's size, and small in the document
  Append(out, outSize, off,
         "\"curve\":{\"spanMs\":%.4g,\"yTopDeltas\":%.4g,\"nativeY\":%.4g,\"stepX\":%.4g,\"stepY\":%.4g,"
         "\"shape\":[",
         spanMs, yTopDeltas, natTop, NiceStep(spanMs, 8), NiceStep(yTopDeltas, 8));
  for (int i = 0; i < n; ++i)
  {
    const double x = (double)i / (double)(n - 1);
    const double y = ChartPathY(x, kneeX, reachX, kneeY, 1.0);
    // The third value is WHICH SLIDER OWNS THIS POINT, so the panel can colour the stroke by segment. The panel
    // is told nothing else about them -- which is what keeps it a generic renderer.
    Append(out, outSize, off, "%s[%.4g,%.4g,%d]", i ? "," : "", x, y < 0 ? 0 : (y > 1 ? 1 : y),
           ChartSegAt(x, kneeX, reachX));
  }

  // THE WHEEL'S OWN PATH: a staircase, one whole step per message, up to its own line. Whole steps so the
  // picture HOPS rather than climbs -- that is what the raw wheel does.
  Append(out, outSize, off, "],\"native\":[");
  for (int i = 0; i < n; ++i)
  {
    const double x = (double)i / (double)(n - 1);
    const int landed = (int)(x * (double)notches + 1e-9);
    AddPoint(out, outSize, off, i, x, ((double)landed / (double)notches) * natTop);
  }
  Append(out, outSize, off, "]}");
  // ⚠️ THAT LAST `}` CLOSES THE `curve` OBJECT, WHICH THIS FUNCTION OPENED. The rule is "a writer closes what
  // it opens" -- not "the caller closes everything" -- and getting it backwards on this exact brace is what
  // left the document one bracket short while every part of it looked correct.
}

} // namespace


// ---------------------------------------------------------------------------
// THE FEATURE'S ENTRY POINTS
// ---------------------------------------------------------------------------

static int SwsInit(const ApexHost *host)
{
  g_host = host;
  if (host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));
  Log("SmoothWheel: folder %s", g_dir[0] ? g_dir : "(unknown)");
  const bool loaded = LoadSettings();
  Log("SmoothWheel: settings %s (glide %.0f, slow %.1f, ramp %.0f, top %.2f, skip %d)",
      loaded ? "loaded" : "defaults", g_cfg.glideMs, g_cfg.slowStep, g_cfg.rampUp, g_cfg.topSpeed,
      g_cfg.skipN);
  return 0;
}

static void SwsShutdown(void)
{
  SaveSettings();
  Log("SmoothWheel: stopped");
}

static int SwsReload(void) { return LoadSettings() ? 1 : 0; }
static int SwsSave(void) { return SaveSettings() ? 1 : 0; }

// ⚠️ ALWAYS ENABLED, AND THAT IS NOT A MISSING SWITCH. This feature has no master switch of its own any
// more: enabling is the generic control the panel draws for every feature (the host's "off" list), so a
// second switch here would be two answers to one question -- which is what shipped, and what the user
// rejected ("插件页只要一个'启用'就可"). The host does not even ASK a feature that is switched off, so
// there is nothing for this side to check.
static unsigned SwsFlags(void)
{
  unsigned f = APEX_FEATURE_ENABLED;
  if (g_core.Active())
    f |= APEX_FEATURE_ACTIVE;
  return f;
}

static int SwsSettingsJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  int off = 0;
  // ⚠️ THE RETURN VALUE IS COUNTED. This used to be a bare _snprintf -- which writes the text but leaves
  // `off` at 0, so every Append/AddRange after it thought it was writing at the start of the buffer and
  // chopped off the opening `{"params":[`. The document that came out parsed as garbage ("Unexpected
  // non-whitespace character after JSON"), and the page reported nothing wrong beyond an empty card.
  off += _snprintf(out, outSize, "{\"params\":[");
  bool first = true; // the parameters, in the order they are shown
  // THE FIVE NUMBERS, with the Chinese and English labels beside them so the panel does not carry a
  // translation table that would have to be updated for every feature.
  //
  // `def` IS THE SHIP DEFAULT, read from a default-constructed Config so it cannot drift from the value the
  // feature actually starts with -- a double-click on a slider restores exactly what a fresh install has.
  const app::Config d;
  AddRange(out, outSize, off, first, "glide", "\xe6\xbb\x91\xe5\x8a\xa8\xe6\x97\xb6\xe9\x95\xbf",
           "Glide", app::Config::GlideLo(), app::Config::GlideHi(), 5, g_cfg.glideMs, "ms", d.glideMs, kHueGlide,
           -1); // no segment: Glide is the chart x AXIS, see the chart note
  // ⚠️ THE QUICK-PANEL SWITCH IS **NOT** HERE ANY MORE. It used to be a per-control switch right under this one
  // ("快速面板：滑动时长"), and the user replaced the pair with a single switch that governs the whole feature --
  // see the note where it is emitted, after 最高速度.
  AddRange(out, outSize, off, first, "slow", "\xe6\x85\xa2\xe6\xbb\x9a\xe6\xad\xa5\xe9\x95\xbf",
           "Slow step", app::Config::SlowLo(), app::Config::SlowHi(), 0.1, g_cfg.slowStep, "d",
           d.slowStep, kHueSlow, 0); // the rise
  AddRange(out, outSize, off, first, "ramp",
           "\xe5\x8a\xa0\xe9\x80\x9f\xe5\x9f\xba\xe5\x87\x86", "Accelerate", app::Config::RampLo(),
           app::Config::RampHi(), 10, g_cfg.rampUp, "d", d.rampUp, kHueRamp, 1); // the climb
  AddRange(out, outSize, off, first, "top", "\xe6\x9c\x80\xe9\xab\x98\xe9\x80\x9f\xe5\xba\xa6",
           "Top speed", app::Config::TopLo(), app::Config::TopHi(), 0.05, g_cfg.topSpeed, "x",
           d.topSpeed, kHueTop, 2); // the flat top
  // ⚠️ THE ONE QUICK-PANEL SWITCH SITS RIGHT AFTER 最高速度, WHICH IS WHERE THE USER PUT IT: "滑动滚轮插件的
  // 「快速面板」开关也只要给一个，位置就放在现在的「最高速度」那边，它是这个插件的快速面板总开关". It replaced two
  // per-control switches (`quick_glide` under 滑动时长 and `quick_top` here), because what it answers -- "may the
  // flyout reach this feature" -- is one answer, and the four sliders it maps are one set.
  //
  // ⚠️ IT IS AN ORDINARY `bool` PARAMETER, NOT A GROUP SWITCH (`group.quick` in abi.h): this feature's page has no
  // groups at all -- it is a flat list of five numbers and a list of names -- so there is no heading or add row for
  // a group switch to be drawn on. The panel draws this one like any other switch, and the same `setControl` that
  // moves a slider sets it.
  //
  // ⚠️ AND ITS DEFAULT IS OFF. The user's rule is that a control reaches the flyout only when they have SAID SO --
  // see common/config.h for the whole argument.
  AddBool(out, outSize, off, first, "quick_panel",
          "\xe5\xbf\xab\xe9\x80\x9f\xe9\x9d\xa2\xe6\x9d\xbf", "Quick panel", g_cfg.quickPanel);
  // THIS FEATURE'S OWN LIST OF PROGRAMS TO LEAVE ALONE. It used to live in the HOST's apex.ini (`skip=`),
  // which meant the setting existed in two files with two writers and the copy in this feature was dead code
  // -- SmoothWheel.ini has parsed and saved the list since the standalone app, and nothing ever read it back.
  // A list is not something every feature has, so it does not belong to the host; see ApexFeature::listOp.
  //
  // ⚠️ IT IS CALLED "排除 / Exclude", AND THE CHANGE IS THE USER'S. It was "黑名单 / Blacklist". A blacklist
  // is something you are ON, and being on one is a judgement about you; this list is a set of programs this
  // FEATURE is told to leave alone. Same behaviour, different sentence -- and the sentence is what a user
  // reads while deciding whether to add a program to it.
  //
  // ⚠️ AND THE CONTROL'S ID CHANGED WITH IT (`skip` -> `exclude`), because an id called `skip` printing the
  // word 排除 is a mismatch the next reader has to stop and decode. ⚠️ AN OLD SETTINGS FILE KEEPS WORKING:
  // a `skip=` line is read by the unknown-key path in common/config.h, the same rule that let the removed
  // `tail=` keep loading. Nothing needs to rewrite the file -- it converts when the user next saves.
  //
  // The placeholder shows a PATTERN rather than a bare name, because that is what the list takes: a user who
  // sees "game.exe" types exactly that, and one who sees "game*" has learned the whole feature.
  //
  // ⚠️ THE ENGINE NOTE IS ASKED FOR HERE, AND THIS IS THE ONE PLACE IT CAN BE ASKED FROM. The host's answer is a
  // machine-wide fact read from a module list and a named event -- the panel process cannot see other processes
  // at all, and the host cannot push it (it never initiates except for the activity count). A settings document
  // is built when the panel asks for one, which is exactly when the user is looking at this page.
  char noteZh[160] = {0};
  char noteEn[192] = {0};
  if (g_host && g_host->activeEngines)
  {
    // ⚠️ THE HOST'S ORDER IS KEPT AS GIVEN (start order, earliest first) and the NAMES COME WITH IT -- this side
    // names no engine itself (see EngineNote).
    ApexEngine engines[4];
    ZeroMemory(engines, sizeof(engines));
    const int got = g_host->activeEngines(engines, 4);
    EngineNote(engines, got, noteZh, (int)sizeof(noteZh), noteEn, (int)sizeof(noteEn));
  }
  AddList(out, outSize, off, first, "exclude", "\xe6\x8e\x92\xe9\x99\xa4", "Exclude",
          "\xe4\xbe\x8b\xe5\xa6\x82 game* \xe6\x88\x96 *tool.exe",
          "e.g. game* or *tool.exe",
          noteZh[0] ? noteZh : nullptr,
          noteEn[0] ? noteEn : nullptr);

  // ⚠️ THE `]` CLOSES `params` AND THE `}` CLOSES THE ROOT, AND THEY BELONG HERE. This document has been
  // broken twice by a bracket that was emitted by the wrong function: once because a bare _snprintf's return
  // was not counted (so the head was sliced off), and once because AddCurve emitted its own `]}` -- which
  // worked only while it was the last thing written, and silently put the curve INSIDE the params array as
  // soon as anything followed it. The element writers do not close what they did not open; the closers live
  // at the end of the function that opened the containers, in the order they were opened.
  // ⚠️ THE `]` CLOSES `params` AND THE `}` WILL CLOSE THE ROOT. `curve` is a SIBLING of `params` in the root
  // object, not another parameter: writing it inside the array makes a document whose brackets still BALANCE
  // (the nesting is merely shifted by one) and which a bracket-counting check therefore accepts. It is
  // rejected by anything that PARSES the document, which is what the gate now does.
  //
  // The element writers (AddRange, AddList, AddCurve) never close a container they did not open: the closers
  // belong to the function that opened them. That rule is here because a bracket emitted by the wrong
  // function has broken this document twice.
  // `],` closes `params`, and the chart follows as a SIBLING key (see the note above: a closer belongs to
  // whoever opened the container). AddCurve emits the whole `"curve":{...}` object, brackets included.
  Append(out, outSize, off, "],");
  AddCurve(out, outSize, off, g_cfg);
  // ⚠️ ONE LINE ABOUT WHAT THIS FEATURE IS FOR, in both languages, beside the version at the top of its page
  // (see `summaryZh`/`summaryEn` in abi.h). It is a sibling of `params` and `curve` in the root object, so it
  // goes before the final `}` -- appended here, by the function that opened the root, like every other closer.
  Append(out, outSize, off,
         ",\"summaryZh\":\""
         "\xe5\x85\xa8\xe5\xb1\x80\xe6\x8e\xa5\xe7\xae\xa1\xe9\xbc\xa0\xe6\xa0\x87\xe6\xbb\x9a\xe8\xbd\xae"
         "\xef\xbc\x9a\xe6\xbb\x9a\xe5\x8a\xa8\xe6\x9b\xb4\xe5\xb9\xb3\xe6\xbb\x91\xef\xbc\x8c\xe5\xb8\xa6"
         "\xe5\x8a\xa0\xe9\x80\x9f\xe4\xb8\x8e\xe6\x94\xb6\xe5\xb0\xbe"
         "\",\"summaryEn\":\"Takes over the mouse wheel everywhere: smoother scrolling, with acceleration and a "
         "settle\"");
  Append(out, outSize, off, "}");
  (void)first;
  return off;
}

// ADD OR REMOVE A ROW OF "排除". The host forwards the request untouched; everything about what the list
// MEANS is decided here (see ApexFeature::listOp).
static int SwsListOp(const char *id, const char *op, const char *value, int index)
{
  if (!id || strcmp(id, "exclude") != 0 || !op)
    return 0;
  if (strcmp(op, "add") == 0)
  {
    // SkipAdd normalises: a pasted path becomes a bare file name and everything is lower-cased, so
    // "C:\Games\DOOM.exe" and "doom.exe" are the same entry and cannot both be in the list. It refuses
    // duplicates and refuses an empty name.
    const bool added = g_cfg.SkipAdd(value);
    return added ? 1 : 0;
  }
  if (strcmp(op, "remove") == 0)
  {
    if (index < 0 || index >= g_cfg.skipN)
      return 0;
    g_cfg.SkipRemove(index);
    return 1;
  }
  return 0;
}

// ⚠️ THE VALUE ARRIVES AS TEXT NOW (ABI 6), so this parses it. Every control this feature has is a slider,
// so that is one atof -- and this is the honest place for the conversion: the host used to do it on the way
// through, which meant the transport was text all along and anything non-numeric was silently turned into a
// number here rather than there. A feature that knows its own control types cannot be surprised that way.
//
// (atof on garbage yields 0.0, which the clamp below pins to the range's floor -- visible and recoverable.
// What matters is that the decision is made HERE, by the feature that knows what the control is.)
static int SwsApply(const char *id, const char *text)
{
  if (!id || !text)
    return 0;
  // THE ONE QUICK-PANEL SWITCH IS A BOOLEAN, not a number, so it is parsed by the same rule the settings file
  // uses (app::ParseBool) rather than through atof -- "true" is a value a panel may send, and atof would read it
  // as 0, which is the one answer that silently means "off".
  if (strcmp(id, "quick_panel") == 0)
  {
    bool b = false;
    if (!app::ParseBool(text, &b))
      return 0;
    g_cfg.quickPanel = b;
    return 1;
  }
  const double v = atof(text);
  // There is no "enabled" here any more: the panel's generic switch does that through the host, and a
  // value arriving under that id is now an unknown one (see SwsFlags).
  if (strcmp(id, "glide") == 0)
    g_cfg.glideMs = v;
  else if (strcmp(id, "slow") == 0)
    g_cfg.slowStep = v;
  else if (strcmp(id, "ramp") == 0)
    g_cfg.rampUp = v;
  else if (strcmp(id, "top") == 0)
    g_cfg.topSpeed = v;
  else
    return 0; // including "tail": that parameter is gone, so a value for it is not accepted

  // THE CLAMP IS THE SINGLE SOURCE for the ranges (app/config.h), so a panel that sends 9999 cannot put
  // the model somewhere it was never tuned for. Nothing else clamps.
  g_cfg.Clamp();
  return 1;
}

// ---- THE QUICK PANEL ----------------------------------------------------------------------------
//
// ALL FOUR NUMBERS, IN ONE BOARD -- and the two halves of that sentence are both the user's:
//
//   * "映射到快速面板是4个推子的参数拉杆" -- what the switch maps is the four parameters, as FADERS (`top` used to
//     be a dial down here; the user asked for four 推子, and four of one shape in one board is also what makes them
//     readable as one set);
//   * and they share a group name, so they land in ONE pane with one heading instead of four floating panes (see
//     `groupZh` in abi.h). The name is this feature's own words for the four -- 滚动参数 -- and not the feature's
//     name, because a group in the flyout is a grouping of CONTROLS ("不以插件分组", the user's rule).
//
// ⚠️ THE VALUES ARE READ FROM g_cfg, THE SAME STRUCTURE `SwsApply` WRITES, so a fader in the flyout and the slider
// on the settings page are two views of ONE number. `SwsApply` clamps, the host rebuilds the panel from what this
// returns, and the two therefore cannot disagree -- which is why there is no clamp here either.
static void QuickCopy(char *dst, int cap, const char *src)
{
  int n = 0;
  if (src)
    for (; src[n] && n < cap - 1; ++n)
      dst[n] = src[n];
  dst[n] = 0;
}

// "#78BEFF" -> 0x78BEFF. The colours live as CSS strings because the settings page hands them straight to a
// stylesheet, and the ABI carries a number -- so the conversion happens here rather than the same colour being
// written down twice in two shapes. A string that is not a colour answers 0, which the panel reads as "use your
// own ink" (see ApexQuickItem::hue).
static unsigned HueFromHex(const char *hex)
{
  if (!hex || hex[0] != '#')
    return 0;
  unsigned v = 0;
  for (int i = 1; i <= 6; ++i)
  {
    const char c = hex[i];
    unsigned d;
    if (c >= '0' && c <= '9')
      d = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = (unsigned)(c - 'A' + 10);
    else
      return 0;
    v = (v << 4) | d;
  }
  return v;
}

// One fader. `unit` is what the read-out at its right is measured in, and the strings are the SAME ones the
// settings page uses (the panel draws the reader's language; this side does not know which that is).
static void QuickFader(ApexQuickItem *q, const char *id, const char *zh, const char *en, const char *unit,
                       double lo, double hi, double step, double value, const char *hueHex)
{
  QuickCopy(q->id, (int)sizeof(q->id), id);
  QuickCopy(q->labelZh, (int)sizeof(q->labelZh), zh);
  QuickCopy(q->labelEn, (int)sizeof(q->labelEn), en);
  QuickCopy(q->unit, (int)sizeof(q->unit), unit);
  // The group name is written on every row (the host compares both languages to decide which rows share a pane).
  QuickCopy(q->groupZh, (int)sizeof(q->groupZh), kQuickGroupZh);
  QuickCopy(q->groupEn, (int)sizeof(q->groupEn), kQuickGroupEn);
  q->type = APEX_QUICK_SLIDER;
  q->min = lo;
  q->max = hi;
  q->step = step;
  q->value = value;
  q->hue = HueFromHex(hueHex);
}

static int SwsQuickItems(ApexQuickItem *out, int max)
{
  // ⚠️ ONLY WHAT THE USER HAS MAPPED, AND THIS LINE IS THE WHOLE OF THE RULE. The `quick_panel` switch on this
  // feature's own page is the permission (see the argument in common/config.h): with it off, nothing is merely
  // hidden by the panel -- it is NOT SENT. The panel can only draw a row somebody described, and a feature that
  // put one there on its own would be deciding for the user, which is the thing the switch exists to prevent.
  const int total = g_cfg.quickPanel ? 4 : 0;
  // THE TOTAL IS RETURNED EVEN WHEN THERE IS NO ROOM, which is the two-part answer the ABI asks for: a caller
  // with a smaller buffer learns there was more rather than getting a silent half.
  if (!out || max <= 0)
    return total;
  ZeroMemory(out, sizeof(ApexQuickItem) * (size_t)max);
  if (!g_cfg.quickPanel)
    return total;

  int n = 0;
  // THE ORDER IS THE PAGE'S ORDER: the four are read in the order the settings page shows them, so the board and
  // the page can be read against each other.
  if (n < max)
    QuickFader(&out[n++], "glide", "\xe6\xbb\x91\xe5\x8a\xa8\xe6\x97\xb6\xe9\x95\xbf", "Glide", "ms",
               app::Config::GlideLo(), app::Config::GlideHi(), 5, g_cfg.glideMs, kHueGlide);
  if (n < max)
    QuickFader(&out[n++], "slow", "\xe6\x85\xa2\xe6\xbb\x9a\xe6\xad\xa5\xe9\x95\xbf", "Slow step", "d",
               app::Config::SlowLo(), app::Config::SlowHi(), 0.1, g_cfg.slowStep, kHueSlow);
  if (n < max)
    QuickFader(&out[n++], "ramp", "\xe5\x8a\xa0\xe9\x80\x9f\xe5\x9f\xba\xe5\x87\x86", "Accelerate", "d",
               app::Config::RampLo(), app::Config::RampHi(), 10, g_cfg.rampUp, kHueRamp);
  if (n < max)
    QuickFader(&out[n++], "top", "\xe6\x9c\x80\xe9\xab\x98\xe9\x80\x9f\xe5\xba\xa6", "Top speed", "x",
               app::Config::TopLo(), app::Config::TopHi(), 0.05, g_cfg.topSpeed, kHueTop);
  return total;
}

// ---- THE HOT PATH -------------------------------------------------------------------------------

static int SwsOnWheel(const ApexWheelEvent *ev)
{
  if (!ev)
    return 0;

  // WHICH DEVICE SENT THIS WHEEL, and it is fed BEFORE anything here can decline the message. The classifier
  // is a state machine over the delta stream (common/device.h), so skipping a message it would have seen is
  // exactly how a gesture stops being recognisable.
  const app::Device device = g_device.Feed(ev->delta, ev->extraInfo);

  // Apex can be asked about every wheel on the system, so this is where a feature says what it does NOT
  // want. SmoothWheel is plain-wheel-only: a modified wheel belongs to the program under the cursor
  // (Ctrl+wheel is zoom nearly everywhere, Alt+wheel is a horizontal scroll on some, and REAPER has its
  // own meanings), and swallowing one would be taking a gesture that was never ours.
  if (ev->key != 0)
    return 0;
  if (ev->delta == 0)
    return 0;

  // ⚠️ A TOUCHPAD IS NOT OURS TO SMOOTH, AND A WHEEL WE CANNOT IDENTIFY YET IS NOT EITHER. A continuous surface
  // is already smooth in the OS -- it reports what the finger is doing rather than a step -- so a second,
  // feature-side easing on top would be easing something that was never stepped in the first place. This is
  // the plugin project's rule, moved here at the user's own instruction ("那个过滤，在 SmoothWheelScroll for
  // reaper 这个项目里有实现过，直接搬过来就可以"); the rule itself is common/device.h and the cases it must get
  // right are in _diag/app_device_probe.cpp.
  //
  // ⚠️ `kUnknown` DECLINING IS THE HALF THAT MATTERS MOST. It is the first messages of EVERY gesture (a whole
  // notch is identified at once, a sub-notch step only after kDeviceMinSamples), and animating those leaked a
  // touchpad into the model at the start of each gesture -- measured in the plugin, see its
  // _diag/device_swipe_probe.cpp. Waiting a couple of messages costs a free-spinning wheel the first fraction
  // of a turn and costs a notched mouse nothing.
  //
  // ⚠️ AND THE DECLINE MUST STAY A DECLINE: returning 0 here leaves the message untouched, which is the same
  // direction as every other refusal in this function (the host only swallows what a feature claims).
  if (device != app::Device::kNotched && device != app::Device::kFreeSpin)
    return 0;

  // THE PROGRAM UNDER THE CURSOR decides whether this is ours at all.
  ApexTarget target;
  if (!g_host->targetAt || !g_host->targetAt(ev->x, ev->y, &target))
    return 0; // nothing known about this target: not ours to take

  // ⚠️ THIS FEATURE'S OWN BLACKLIST, CHECKED HERE. It used to be the HOST's list (apex.ini `skip=`), which
  // is the wrong place for it: "do not smooth in this program" is a statement about SMOOTHING, not about
  // Apex, and a future feature that has nothing to do with wheels has no reason to inherit it. The host
  // knows nothing about this list now -- see ApexFeature::listOp, which is how the panel edits it.
  //
  // Declining here has the same effect the host-side check had: the wheel passes through untouched (the
  // host only swallows what a feature claims).
  if (g_cfg.SkipHas(target.exe))
    return 0;

  // THE GAP between messages, which the model needs: it is how the speed budget knows a roll from a
  // single notch. The feature has no clock of its own (the host owns that), and GetTickCount64 is the one
  // time source available with no setup. Its resolution is the system tick, which is far finer than the
  // gaps that matter here (tens of milliseconds).
  static unsigned long long lastMs = 0;
  const unsigned long long nowMs = GetTickCount64();
  const double gap = (lastMs == 0) ? 0.0 : (double)(nowMs - lastMs);
  lastMs = nowMs;

  g_core.Feed(ev->delta, gap, g_cfg);

  // TELL THE HOST, so the panel's curve can show this wheel. ONE CALL, and that is the point: it runs in the OS
  // input path, so it must not do anything but set a flag -- the host owns the messaging (see
  // ApexHost::activity). Without it the panel can only animate on a timer, which is a picture of nothing:
  // running while the user is idle and still while they scroll.
  if (g_host->activity)
    g_host->activity();
  return 1;
}

static double SwsTick(double dtSec)
{
  // THE HOST CARRIES THE FRACTION (see host.injectDeltas): what is returned here is added to nothing and
  // rounded by nobody.
  return g_core.Tick(dtSec, g_cfg);
}

// ---------------------------------------------------------------------------
// THE EXPORTED STRUCTURE. `structSize` is checked by the host BEFORE any of this is read, so it must be
// the real size of the struct this DLL was built with.
// ---------------------------------------------------------------------------
static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    "SmoothWheel",
    "\xe4\xb8\x9d\xe6\xbb\x91\xe6\xbb\x9a\xe5\x8a\xa8",        // 丝滑滚动
    "Silky Scroll",
    // ⚠️ THE VERSION OF THE REAPER PLUGIN THIS FEATURE CAME FROM (1.7.1), at the user's request: the two are the
    // same thing in two hosts, so a version that says "1.1.0" here would tell the user they are running
    // something older than the plugin they already have. (AutoIME's is 1.2.1 for the same reason.)
    "1.7.1",
    SwsInit,
    SwsShutdown,
    SwsReload,
    SwsSettingsJson,
    SwsApply,
    SwsListOp,
    SwsQuickItems,
    SwsSave,
    SwsOnWheel,
    SwsTick,
    SwsFlags,
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
