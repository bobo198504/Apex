// ---------------------------------------------------------------------------
// THE FEATURE'S CONTROL DOCUMENT, AND ITS MOTION CURVE -- checked on the ARTEFACT, not on the source.
//
// WHY THIS GATE EXISTS. Two classes of failure have already happened here, and the panel cannot tell either
// of them from "the feature sent nothing":
//
//   1. MALFORMED JSON. The document is built by hand (that is the only way a feature builds one -- see the
//      note in the feature). Twice it came out structurally broken: the opening `{"params":[` was written
//      with a bare _snprintf whose return was not counted, so every later write landed at offset 0 and sliced
//      the head off; and the curve was appended inside the params array, a missing `]`. Both times the page
//      reported a JSON error and drew an empty card.
//   2. A CURVE THAT DOES NOT FOLLOW THE CONTROLS. The curve is the only place the user can SEE what four
//      sliders do to the motion, so one that ignores a slider is worse than none: it teaches the wrong
//      lesson confidently.
//
// HOW IT RUNS THE REAL CODE: it loads SmoothWheel.dll exactly as the host does (exported entry point, ABI
// version and struct size checked first) and calls settingsJson -- not a copy of the curve maths.
//
// Build: g++ -std=c++17 -O2 -I apex -o build/_chart_probe.exe _diag/feature_chart_probe.cpp
// Run:   build/_chart_probe.exe build/apex/Plugins/SmoothWheel/SmoothWheel.dll
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "abi.h"
#include "release.h" // for the rolling window the chart stands for (see the scale check at the end)

typedef const ApexFeature *(*EntryFn)(void);

static int failures = 0;
static void Check(bool ok, const char *what, const char *detail = "")
{
  printf("  %-58s %s%s%s\n", what, ok ? "ok" : "FAIL", detail[0] ? "  " : "", detail);
  if (!ok)
    ++failures;
}

// ---- the host stub `init` needs ----------------------------------------------------------------
//
// ⚠️ A FEATURE'S `init` IS ALLOWED TO READ ITS SETTINGS FILE, so the folder it is given points into build/ --
// never at a deployed installation and never at the user's own settings. That is the same rule every other
// probe in this project follows, and it is the reason `--scratch` is a command-line argument rather than a
// constant: the caller decides where a feature's side effects land.
static char g_dir[512] = {0};
static int HostFeatureDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  const int n = _snprintf(out, outSize, "%s", g_dir);
  return n > 0 ? n : 0;
}
static void HostLogLine(const char *text) { printf("      [feature] %s\n", text); }
static int HostFeatureEnabled(const char *) { return 1; }

// IS THE DOCUMENT EVEN WELL FORMED? A bracket/quote walk -- which is all a hand-built JSON document needs,
// and the check that would have caught both of the structural bugs above. A substring search does not: it
// does not care about nesting, and the curve-inside-params failure is exactly a nesting error.
static bool JsonBalanced(const char *s, const char **why)
{
  int depth = 0;
  bool inStr = false;
  for (const char *p = s; *p; ++p)
  {
    if (inStr)
    {
      if (*p == '\\')
        ++p; // an escape: the next character is data
      else if (*p == '"')
        inStr = false;
      continue;
    }
    if (*p == '"')
      inStr = true;
    else if (*p == '{' || *p == '[')
      ++depth;
    else if (*p == '}' || *p == ']')
    {
      if (--depth < 0)
      {
        *why = "a closing bracket with nothing open";
        return false;
      }
      if (depth == 0 && p[1] && p[1] != '\n' && p[1] != '\r')
      {
        *why = "content after the root object closed";
        return false;
      }
    }
  }
  if (inStr)
  {
    *why = "an unterminated string";
    return false;
  }
  if (depth != 0)
  {
    *why = "unclosed brackets";
    return false;
  }
  return true;
}

struct Chart
{
  double sx[256], sy[256];
  int seg[256];
  int n;
  double nx[256], ny[256];
  int nn;
  double spanMs;     // the x axis range, in ms (the drawn span -- it scales with Glide)
  double yTopDeltas; // the y axis range, in wheel deltas (top x six notches -- scales with Top speed)
  double nativeY;    // where the wheel's own line sits, 0..1 of the box (= 1/top)
  double stepX, stepY;
  double peakY;      // the highest point of the shaped path, 0..1 of the box
  // THE TWO JOINTS, derived from the segment tags: where the rise ends and how high it got, and where the
  // climb reaches the top. These are what Slow step and Ramp-up own, so they are what those sliders must
  // move -- "the shape changed" is too coarse to tell a working channel from a broken one.
  double kneeY, reachX;
};

// `"key":<number>` inside the chart object.
static double Num(const char *from, const char *key)
{
  const char *p = strstr(from, key);
  return p ? atof(p + strlen(key)) : 0.0;
}

// Read `[[a,b]]` (pairs) or `[[a,b,c]]` (triples) at `key`. Returns how many were read; `az` may be null.
static int ReadPairs(const char *from, const char *key, double *ax, double *ay, int *az, int cap)
{
  const char *p = strstr(from, key);
  if (!p)
    return 0;
  p = strchr(p, '[');
  if (!p)
    return 0;
  ++p;
  int n = 0;
  while (*p && n < cap)
  {
    while (*p == ' ' || *p == ',')
      ++p;
    if (*p != '[')
      break;
    double a = 0, b = 0;
    int c = 0;
    const int got = sscanf(p, "[%lf,%lf,%d]", &a, &b, &c);
    if (got < 2)
      break;
    ax[n] = a;
    ay[n] = b;
    if (az)
      az[n] = c;
    ++n;
    const char *close = strchr(p, ']');
    if (!close)
      break;
    p = close + 1;
  }
  return n;
}

static bool ReadChart(const char *json, Chart *c)
{
  memset(c, 0, sizeof(*c));
  const char *p = strstr(json, "\"curve\"");
  if (!p)
    return false;
  c->spanMs = Num(p, "\"spanMs\":");
  c->yTopDeltas = Num(p, "\"yTopDeltas\":");
  c->nativeY = Num(p, "\"nativeY\":");
  c->stepX = Num(p, "\"stepX\":");
  c->stepY = Num(p, "\"stepY\":");
  c->n = ReadPairs(p, "\"shape\"", c->sx, c->sy, c->seg, 256);
  c->nn = ReadPairs(p, "\"native\"", c->nx, c->ny, nullptr, 256);
  c->kneeY = c->reachX = -1.0;
  for (int i = 0; i < c->n; ++i)
  {
    if (c->seg[i] == 1 && c->kneeY < 0)
      c->kneeY = c->sy[i];
    if (c->seg[i] == 2 && c->reachX < 0)
      c->reachX = c->sx[i];
  }
  return c->n >= 2 && c->nn >= 2;
}

static bool GetChart(ApexFeature *f, Chart *c)
{
  static char buf[64 * 1024];
  if (f->settingsJson(buf, (int)sizeof(buf)) <= 0)
    return false;
  return ReadChart(buf, c);
}

static double MaxY(const Chart &c)
{
  double m = 0;
  for (int i = 0; i < c.n; ++i)
    if (c.sy[i] > m)
      m = c.sy[i];
  return m;
}

static void SetAll(ApexFeature *f, double glide, double slow, double ramp, double top)
{
  // ⚠️ THE VALUE IS TEXT NOW (ABI 6, setControl), so the numbers are formatted first. The probe is asking the
  // feature for a chart at chosen slider positions, and the feature parses what it is given -- exactly as it
  // does for the panel.
  {
    char v[64];
    _snprintf(v, sizeof(v), "%.6g", glide);
    f->setControl("glide", v);
    _snprintf(v, sizeof(v), "%.6g", slow);
    f->setControl("slow", v);
    _snprintf(v, sizeof(v), "%.6g", ramp);
    f->setControl("ramp", v);
    _snprintf(v, sizeof(v), "%.6g", top);
    f->setControl("top", v);
  }
}

int main(int argc, char **argv)
{
  const char *dll = (argc >= 2) ? argv[1] : "build/apex/Plugins/SmoothWheel/SmoothWheel.dll";
  // OPTIONAL third argument: write the controls document HERE, for _diag/panel_preview.js to render. A page's
  // layout cannot be judged from a DOM stub (no layout, no hit testing), and a hand-written "document that looks
  // about right" is how a probe ends up reporting a page the feature does not send -- so the dump is the real one.
  const char *dump = (argc >= 3) ? argv[2] : nullptr;
  // OPTIONAL fourth argument: the folder this feature is told it owns (see the host stub above).
  if (argc >= 4)
    _snprintf(g_dir, sizeof(g_dir), "%s", argv[3]);
  HMODULE mod = LoadLibraryA(dll);
  if (!mod)
  {
    printf("cannot load %s (err %lu)\n", dll, GetLastError());
    return 2;
  }
  EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  if (!entry)
  {
    printf("no ApexFeatureEntry in %s\n", dll);
    return 2;
  }
  ApexFeature *f = (ApexFeature *)entry();
  printf("the control document of %s (ABI %u)\n", f->id, f->abiVersion);
  if (f->abiVersion != APEX_ABI_VERSION || f->structSize < sizeof(ApexFeature))
  {
    printf("  FAIL: the DLL does not match this host's ABI\n");
    return 1;
  }
  // ⚠️⚠️ `init()` **IS** CALLED NOW, AND THE REASON IS THAT NOT CALLING IT WAS A LIE ABOUT THE CONTRACT. The
  // note here used to say it was unnecessary because "settingsJson works from the feature's defaults" -- which
  // is true of exactly one feature. The ABI says `init` runs once after loading and sets up "its settings file,
  // its state"; a feature is entitled to build its state there, and two of them (KeepAwake and MediaControl)
  // create the lock their `settingsJson` takes. Calling it without `init` was therefore not a read-only check,
  // it was a SEGFAULT -- measured, on both of them, the first time this probe was pointed at every feature.
  //
  // The scratch folder keeps the side effect honest: the feature's own settings file lands under build/, never
  // in a deployed or hand-made installation.
  ApexHost host;
  memset(&host, 0, sizeof(host));
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLogLine;
  host.featureEnabled = HostFeatureEnabled;
  if (f->init)
  {
    const int rc = f->init(&host);
    Check(rc == 0, "  the feature starts (init returns 0 = ready)", rc == 0 ? "" : "non-zero means refusal");
    if (rc != 0)
    {
      FreeLibrary(mod);
      return 1;
    }
  }

  // ---- 1. the document ----
  static char buf[64 * 1024];
  const int n = f->settingsJson(buf, (int)sizeof(buf));
  Check(n > 0, "the feature describes its controls", "");
  if (dump && n > 0)
  {
    FILE *fp = fopen(dump, "wb");
    if (fp)
    {
      fwrite(buf, 1, (size_t)n, fp);
      fclose(fp);
      printf("  [document written to %s, for panel_preview.js]\n", dump);
    }
  }
  const char *why = "";
  const bool balanced = n > 0 && JsonBalanced(buf, &why);
  Check(balanced, "  and its brackets balance", why);
  if (!balanced)
  {
    FreeLibrary(mod);
    return 1;
  }
  {
    // The questions the PAGE asks. A balanced document can still fail all of them, and the page's symptom is
    // the same empty card in every case.
    const char *params = strstr(buf, "\"params\":[");
    const char *curve = strstr(buf, "\"curve\":{");
    const char *close = params ? strchr(params, ']') : nullptr;
    char d[128];
    _snprintf(d, sizeof(d), "params@%d curve@%d params-closed@%d", (int)(params ? params - buf : -1),
              (int)(curve ? curve - buf : -1), (int)(close ? close - buf : -1));
    Check(params && close, "  and `params` is a closed array", d);

    // ⚠️⚠️ THE CHART CHECKS APPLY ONLY TO A FEATURE THAT SENDS A CHART, AND UNTIL THIS SPLIT THE WHOLE GATE
    // RAN FOR ONE FEATURE. Both facts matter and both were learned the hard way:
    //   * this probe was written for SmoothWheel and asked for `curve`, `hue` and `seg` unconditionally -- so
    //     pointing it at any other feature would have failed it for not being SmoothWheel;
    //   * and `check_feature_chart.sh` loaded exactly ONE dll, so every other feature's document was never
    //     parsed at all. MediaControl shipped a document with a doubled comma in it (`]},,{`) and the settings
    //     page showed an empty feature page -- the exact symptom this gate exists for, unfired, because the
    //     gate was looking at a different feature. (Bracket balance -- what `JsonBalanced` above checks -- is
    //     not JSON validity: `]},,{` balances perfectly.)
    if (curve)
    {
      Check(close < curve, "  and `curve` is a sibling of `params`, not inside it", d);
      Check(strstr(buf, "\"hue\":") != nullptr,
            "  and the sliders carry a colour for the chart to use", "");

      // ⚠️ AND THEY MUST SAY WHICH SEGMENT THAT COLOUR BELONGS TO. `hue` alone is only half the contract: the
      // curve names a segment as a number and the slider owns a colour, and without `seg` the panel has no way
      // to join the two -- so it would have to guess. It used to guess by looking for this feature's own ids,
      // which is exactly the kind of hidden coupling that makes "a feature is a folder" false; the page would
      // have kept working for THIS feature and drawn every later one's chart in one colour, silently.
      // Checked here rather than in the page probe because this is the FEATURE's half: the ids, the hues and the
      // segment numbers are all in the document it builds.
      int ranges = 0, segs = 0;
      for (const char *p = buf; (p = strstr(p, "\"type\":\"range\"")) != nullptr; ++p)
        ++ranges;
      for (const char *p = buf; (p = strstr(p, "\"seg\":")) != nullptr; ++p)
        ++segs;
      char d2[128];
      _snprintf(d2, sizeof(d2), "%d range(s), %d with a seg", ranges, segs);
      Check(ranges > 0 && ranges == segs, "  and each slider says which chart segment it colours (`seg`)", d2);
    }
    else
    {
      printf("  -- no chart in this document: the chart checks do not apply to this feature\n");
    }
  }

  // ---- 2. the chart ----
  if (!strstr(buf, "\"curve\":{"))
  {
    // Nothing below is about this feature: it described its controls and that is the whole of its half of the
    // contract. The document has already been checked (and, by the caller, parsed) above.
    //
    // ⚠️ THE NAME IS READ BEFORE THE LIBRARY GOES: `f->id` points into the DLL's own memory, and printing it
    // after FreeLibrary is a read of unmapped pages.
    char who[64] = {0};
    _snprintf(who, sizeof(who), "%s", f->id ? f->id : "?");
    if (f->shutdown)
      f->shutdown();
    FreeLibrary(mod);
    printf("\n");
    if (failures == 0)
      printf("OK: the document is valid and describes %s's controls\n", who);
    else
      printf("FAILED: %d\n", failures);
    return failures == 0 ? 0 : 1;
  }
  Chart base;
  const bool got = GetChart(f, &base);
  Check(got, "it sends a motion chart", got ? "" : "no shape/native arrays in settingsJson");
  if (!got)
  {
    FreeLibrary(mod);
    return 1;
  }
  {
    char d[160];
    _snprintf(d, sizeof(d), "span=%.0fms yTop=%.0fd nativeY=%.3f stepX=%.0f stepY=%.0f", base.spanMs,
              base.yTopDeltas, base.nativeY, base.stepX, base.stepY);
    Check(base.spanMs > 0 && base.yTopDeltas > 0 && base.nativeY > 0, "  with both axis ranges", d);
    Check(base.stepX > 0 && base.stepY > 0, "  and a tick step for each axis", d);
  }
  {
    const bool startsLow = base.sy[0] < 1e-3;
    const bool endsTop = fabs(MaxY(base) - 1.0) < 1e-3;
    bool monotonic = true;
    for (int i = 1; i < base.n; ++i)
      if (base.sy[i] < base.sy[i - 1] - 1e-9)
        monotonic = false;
    char d[128];
    _snprintf(d, sizeof(d), "%d points, y %.5f -> %.3f", base.n, base.sy[0], MaxY(base));
    Check(startsLow && endsTop && monotonic, "  and the shaped path rises to the top, never backwards", d);
  }
  {
    // THREE SEGMENTS, in order, each non-empty: the colours are per segment, so an empty segment is a slider
    // whose colour never appears anywhere.
    int count[8] = {0};
    bool ordered = true;
    int hi = 0;
    for (int i = 0; i < base.n; ++i)
    {
      if (base.seg[i] < hi)
        ordered = false;
      hi = base.seg[i];
      if (base.seg[i] >= 0 && base.seg[i] < 8)
        ++count[base.seg[i]];
    }
    char d[128];
    _snprintf(d, sizeof(d), "segments 0/1/2 have %d/%d/%d points", count[0], count[1], count[2]);
    Check(ordered && count[0] > 0 && count[1] > 0 && count[2] > 0,
          "  in three segments, one per coloured slider", d);
  }
  {
    double top = 0;
    for (int i = 0; i < base.nn; ++i)
      if (base.ny[i] > top)
        top = base.ny[i];
    char d[128];
    _snprintf(d, sizeof(d), "staircase tops at %.4f, native line at %.4f", top, base.nativeY);
    Check(fabs(top - base.nativeY) < 1e-3, "  and the wheel's staircase tops out on the native line", d);
  }

  // ---- 3. the chart follows the sliders, EACH IN ITS OWN CHANNEL ----
  //
  // This is the whole design of the picture (see the feature's chart note): Glide is the horizontal scale, Top
  // speed the vertical one, Slow step the height the rise ends at, Ramp-up how far along x the climb reaches
  // the top. So the check asks each slider the question ITS channel answers -- "the shape changed somewhere"
  // is too coarse: a working channel and a broken one both look like motion, and a slider whose own channel
  // does nothing is worse than a missing chart, because it looks like the control is not wired up.
  struct Case
  {
    const char *id;
    double v;
    const char *owns;
  };
  const Case cases[] = {
      {"glide", 130, "horizontal"},
      {"slow", 9, "knee"},
      {"ramp", 1900, "climb"},
      {"top", 1.95, "vertical"},
  };
  for (const Case &c : cases)
  {
    SetAll(f, 200, 5.0, 500, 1.5);
    Chart before;
    GetChart(f, &before);
    // (the same text conversion as above -- see ABI 6 in apex/abi.h)
    {
      char v[64];
      _snprintf(v, sizeof(v), "%.6g", c.v);
      f->setControl(c.id, v);
    }
    Chart after;
    const bool ok = GetChart(f, &after);

    bool moved = false;
    char d[192];
    if (strcmp(c.owns, "horizontal") == 0)
    {
      moved = fabs(after.spanMs - before.spanMs) > 1e-9;
      _snprintf(d, sizeof(d), "span %.0f -> %.0f ms (the x axis)", before.spanMs, after.spanMs);
    }
    else if (strcmp(c.owns, "vertical") == 0)
    {
      moved = fabs(after.yTopDeltas - before.yTopDeltas) > 1e-9 &&
              fabs(after.nativeY - before.nativeY) > 1e-9;
      _snprintf(d, sizeof(d), "range %.0f -> %.0f deltas, wheel's line %.4f -> %.4f", before.yTopDeltas,
                after.yTopDeltas, before.nativeY, after.nativeY);
    }
    else if (strcmp(c.owns, "knee") == 0)
    {
      moved = fabs(after.kneeY - before.kneeY) > 1e-6;
      _snprintf(d, sizeof(d), "knee at %.4f -> %.4f of the box", before.kneeY, after.kneeY);
    }
    else
    {
      moved = fabs(after.reachX - before.reachX) > 1e-6;
      _snprintf(d, sizeof(d), "climb reaches the top at x %.3f -> %.3f", before.reachX, after.reachX);
    }
    char what[112];
    _snprintf(what, sizeof(what), "%s moves its own channel (%s)", c.id, c.owns);
    Check(ok && moved, what, d);
  }
  SetAll(f, 200, 5.0, 500, 1.5);

  // ---- 4. the corners are filleted, not sharp ----
  {
    Chart c;
    GetChart(f, &c);
    int curvedRun = 0, best = 0;
    for (int i = 1; i + 1 < c.n; ++i)
    {
      const double d2 = fabs(c.sy[i + 1] - 2 * c.sy[i] + c.sy[i - 1]);
      if (d2 > 1e-5)
      {
        if (++curvedRun > best)
          best = curvedRun;
      }
      else
        curvedRun = 0;
    }
    char d[96];
    _snprintf(d, sizeof(d), "longest rounded shoulder = %d samples", best);
    Check(best >= 3, "the corners are rounded, not sharp", d);
  }

  // ---- 5. what the scales MEAN, at the default settings ----
  {
    Chart c;
    GetChart(f, &c);
    char d[160];
    // The y axis is a plain product: the ceiling times the six notches the picture is scaled to. The x axis is
    // the DRAWN span, which the feature derives from the window (150 + 2.5*window, times the plugin's own pace
    // factor) -- so it is checked as a relationship rather than a constant, which is what it is.
    //
    // ⚠️ AND THE WINDOW THE CHART STANDS FOR IS THE ROLL'S, NOT THE GLIDE SETTING. The drawn curve has a knee
    // and a climb, and those only exist while windows overlap, so the picture depicts a roll -- and a roll's
    // window is the Glide setting plus the release model's fixed addition (common/release.h). At the shipped
    // Glide of 200 that is a 400 ms window. The assertion is written against the release model's own function
    // rather than against the arithmetic, so the chart and the delivery layer cannot drift apart.
    const double rollWindowMs = app::ReleaseRollWindowMs(200.0);
    const double wantSpan = (150.0 + 2.5 * rollWindowMs) * 0.45;
    _snprintf(d, sizeof(d), "yTop=%.0f (1.5 x 6 x 120), span=%.1f ms for a %.0f ms rolling window",
              c.yTopDeltas, c.spanMs, rollWindowMs);
    Check(fabs(c.yTopDeltas - 1080.0) < 1.0 && fabs(c.spanMs - wantSpan) < 1.0,
          "the two scales are the real numbers the sliders imply", d);
  }

  if (f->shutdown)
    f->shutdown();
  FreeLibrary(mod);
  printf("\n");
  if (failures == 0)
    printf("OK: the document is valid, both scales and all three coloured segments are there, and every\n"
           "    slider moves the chart\n");
  else
    printf("FAILED: %d\n", failures);
  return failures == 0 ? 0 : 1;
}
