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

void AddRange(char *out, int outSize, int &off, bool &first, const char *id, const char *zh,
              const char *en, double lo, double hi, double step, double val, const char *unit,
              double def, const char *hue)
{
  Append(out, outSize, off,
         "%s{\"id\":\"%s\",\"type\":\"range\",\"labelZh\":\"%s\",\"labelEn\":\"%s\","
         "\"min\":%.4g,\"max\":%.4g,\"step\":%.4g,\"value\":%.4g,\"def\":%.4g,\"unit\":\"%s\","
         "\"hue\":\"%s\"}",
         first ? "" : ",", id, zh, en, lo, hi, step, val, def, unit, hue);
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

// A list the user edits -- this feature's blacklist. THE PANEL KNOWS NOTHING ABOUT BLACKLISTS: it draws a
// list, sends "add"/"remove" through ApexFeature::listOp, and re-reads. That is why a future feature with
// no blacklist costs the panel nothing, and one with a different KIND of list needs no panel change either.
void AddList(char *out, int outSize, int &off, bool &first, const char *id, const char *zh,
             const char *en, const char *placeholderZh, const char *placeholderEn)
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
  Append(out, outSize, off, "]}");
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
           "Glide", app::Config::GlideLo(), app::Config::GlideHi(), 5, g_cfg.glideMs, "ms", d.glideMs, kHueGlide);
  AddRange(out, outSize, off, first, "slow", "\xe6\x85\xa2\xe6\xbb\x9a\xe6\xad\xa5\xe9\x95\xbf",
           "Slow step", app::Config::SlowLo(), app::Config::SlowHi(), 0.1, g_cfg.slowStep, "d",
           d.slowStep, kHueSlow);
  AddRange(out, outSize, off, first, "ramp",
           "\xe5\x8a\xa0\xe9\x80\x9f\xe5\x9f\xba\xe5\x87\x86", "Accelerate", app::Config::RampLo(),
           app::Config::RampHi(), 10, g_cfg.rampUp, "d", d.rampUp, kHueRamp);
  AddRange(out, outSize, off, first, "top", "\xe6\x9c\x80\xe9\xab\x98\xe9\x80\x9f\xe5\xba\xa6",
           "Top speed", app::Config::TopLo(), app::Config::TopHi(), 0.05, g_cfg.topSpeed, "x",
           d.topSpeed, kHueTop);
  // THIS FEATURE'S OWN BLACKLIST. It used to live in the HOST's apex.ini (`skip=`), which meant the setting
  // existed in two files with two writers and the copy in this feature was dead code -- SmoothWheel.ini has
  // parsed and saved `skip=` since the standalone app, and nothing ever read it back. A blacklist is not
  // something every feature has, so it does not belong to the host; see ApexFeature::listOp.
  // The placeholder shows a PATTERN rather than a bare name, because that is what the list takes now: a user
  // who sees "game.exe" types exactly that, and one who sees "game*" has learned the whole feature.
  AddList(out, outSize, off, first, "skip", "\xe9\xbb\x91\xe5\x90\x8d\xe5\x8d\x95", "Blacklist",
          "\xe4\xbe\x8b\xe5\xa6\x82 game* \xe6\x88\x96 *tool.exe",
          "e.g. game* or *tool.exe");

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
  Append(out, outSize, off, "}");
  (void)first;
  return off;
}

// ADD OR REMOVE A BLACKLIST ROW. The host forwards the request untouched; everything about what the list
// MEANS is decided here (see ApexFeature::listOp).
static int SwsListOp(const char *id, const char *op, const char *value, int index)
{
  if (!id || strcmp(id, "skip") != 0 || !op)
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

static int SwsApply(const char *id, double v)
{
  if (!id)
    return 0;
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

// ---- THE HOT PATH -------------------------------------------------------------------------------

static int SwsOnWheel(const ApexWheelEvent *ev)
{
  if (!ev)
    return 0;

  // Apex can be asked about every wheel on the system, so this is where a feature says what it does NOT
  // want. SmoothWheel is plain-wheel-only: a modified wheel belongs to the program under the cursor
  // (Ctrl+wheel is zoom nearly everywhere, Alt+wheel is a horizontal scroll on some, and REAPER has its
  // own meanings), and swallowing one would be taking a gesture that was never ours.
  if (ev->key != 0)
    return 0;
  if (ev->delta == 0)
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
    "\xe6\xbb\x91\xe5\x8a\xa8\xe6\xbb\x9a\xe8\xbd\xae",        // 滑动滚轮
    "Smooth Wheel Scroll",
    "1.0.0",
    SwsInit,
    SwsShutdown,
    SwsReload,
    SwsSettingsJson,
    SwsApply,
    SwsListOp,
    SwsSave,
    SwsOnWheel,
    SwsTick,
    SwsFlags,
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
