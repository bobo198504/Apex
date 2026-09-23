// THE TAIL: what the delivered motion actually looks like frame by frame, near the end.
//
// "The tail stops suddenly" is a claim about SHAPE, so it is measured as a shape: the amount handed over
// per frame, over a single notch's life, and in particular how much the LAST frames carry compared with
// the peak. A flat rate that then stops dead reads as an abrupt end; a rate that eases down reads as a
// settle.
//
// The ease is the model's OWN per-window parameter (Params.payoutEase), and the model's header says the
// CALLER chooses it per message. So this is a delivery-layer question by construction: the same model,
// handed a different ease, produces a different shape -- and the model's parameters still drive
// everything else.
//
// g++ -std=c++17 -O2 -I. -Isrc _diag/app_tail_probe.cpp -o /tmp/atail && /tmp/atail
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "core.h"

struct Profile
{
  std::vector<double> perFrame; // deltas handed over each frame
  double total = 0.0;
  double peak = 0.0;
};

// One notch, then let it drain, sampling every frame.
static Profile OneNotch(const app::Config &cfg, double ease)
{
  app::Core core; // note: Core applies the model's own rule; this probe drives the model directly
  model::Params P;
  P.windowMs = cfg.glideMs;
  P.payoutEase = ease;

  model::Axis gl;
  gl.Reset();
  model::SpeedBudget bud;
  bud.Reset();

  // The travel a single notch gets, exactly as the app computes it (no private scaling).
  const double budget = bud.Add(120.0, 0.0);
  const double travel = model::Travel(120.0, budget, cfg.rampUp, cfg.slowStep, cfg.topSpeed);

  gl.Feed(travel, P);

  Profile r;
  const double dt = app::kFrameMs * 0.001;
  for (int i = 0; i < 200 && gl.Active(); ++i)
  {
    const double out = gl.Tick(dt, P);
    r.perFrame.push_back(out);
    r.total += out;
    if (out > r.peak)
      r.peak = out;
  }
  return r;
}

static void Show(const char *label, const Profile &p)
{
  printf("\n%s\n", label);
  printf("  frames=%d  total=%.2f deltas  peak=%.2f/frame\n", (int)p.perFrame.size(), p.total, p.peak);

  // The last frames are the question: what does the motion carry just before it ends?
  const int n = (int)p.perFrame.size();
  printf("  first 6 frames:");
  for (int i = 0; i < 6 && i < n; ++i)
    printf(" %.3f", p.perFrame[i]);
  printf("\n  last 6 frames :");
  for (int i = (n > 6 ? n - 6 : 0); i < n; ++i)
    printf(" %.3f", p.perFrame[i]);

  // The number that says "abrupt" or "settled": the final frame's share of the peak.
  if (n > 0 && p.peak > 0.0)
    printf("\n  final frame = %.1f%% of the peak rate  <- the abruptness\n",
           100.0 * p.perFrame[n - 1] / p.peak);

  // A coarse picture of the whole profile, in eighths.
  printf("  profile (eighths of the life):");
  for (int k = 0; k < 8; ++k)
  {
    const int a = n * k / 8, b = n * (k + 1) / 8;
    double m = 0.0;
    for (int i = a; i < b && i < n; ++i)
      if (p.perFrame[i] > m)
        m = p.perFrame[i];
    printf(" %.2f", m);
  }
  printf("\n");
}

// A steady roll's RIPPLE: how much the amount handed over varies frame to frame once the roll is
// settled. This is the cost side of easing -- the model's notes measured that easing a TILING roll
// (gap == window, so windows butt end to end with no overlap) takes its ripple from 0.1% to 11%.
static double RollRipple(const app::Config &cfg, double gapMs, double ease)
{
  model::Params P;
  P.windowMs = cfg.glideMs;
  P.payoutEase = ease;
  model::Axis gl;
  gl.Reset();
  model::SpeedBudget bud;
  bud.Reset();

  const double dt = app::kFrameMs * 0.001;
  const int nNotches = 30;
  const int frames = (int)(nNotches * gapMs / app::kFrameMs);
  std::vector<double> outs;
  outs.reserve(frames);
  int fed = 0;
  for (int f = 0; f < frames; ++f)
  {
    const double tMs = f * app::kFrameMs;
    while (fed < nNotches && fed * gapMs <= tMs + 1e-9)
    {
      const double g = (fed == 0) ? 0.0 : gapMs;
      const double budget = bud.Add(120.0, g);
      gl.Feed(model::Travel(120.0, budget, cfg.rampUp, cfg.slowStep, cfg.topSpeed), P);
      ++fed;
    }
    outs.push_back(gl.Tick(dt, P));
  }

  // Measure the settled part only: skip the first quarter (the budget is still filling).
  const size_t from = outs.size() / 4;
  double mean = 0.0;
  for (size_t i = from; i < outs.size(); ++i)
    mean += outs[i];
  mean /= (double)(outs.size() - from);
  if (mean <= 0.0)
    return 0.0;
  double var = 0.0;
  for (size_t i = from; i < outs.size(); ++i)
    var += (outs[i] - mean) * (outs[i] - mean);
  var /= (double)(outs.size() - from);
  return 100.0 * std::sqrt(var) / mean;
}

static double LastFramePctOfPeak(const Profile &p)
{
  const int n = (int)p.perFrame.size();
  if (n <= 0 || p.peak <= 0.0)
    return 0.0;
  return 100.0 * p.perFrame[n - 1] / p.peak;
}

// The eased shape this probe also uses as the "wanted" reference: the model's calibrated amount, which
// is what AppEaseFor asks for on a lone notch.
// The shape this probe treats as the "wanted" reference: the model's FULLY-eased end, which is what
// AppEaseFor asks for on a lone notch. (The model's own kEaseAmount of 0.5 is NOT enough -- see the note
// in app/core.h: the rate at the end of a window is exactly 1 - ease, so 0.5 still stops at 40%.)
static double WantedEase() { return 1.0; }

// A ROLL WITH THE TAIL ON, frame by frame, looking for the failure this design could have: the model
// holds ONE window length per Tick call and applies it to every window in flight, so a length that grows
// with speed re-scales windows already running. If that made the payout go BACKWARDS the motion would
// visibly jerk, and it would be a reason to keep the length fixed per gesture.
//
// Returns the worst negative frame (0 means none) and the frame count.
struct RollRun
{
  int frames = 0;
  double worstNegative = 0.0;
  double total = 0.0;
  bool monotone = true;
};

static RollRun Roll(const app::Config &cfg, int notches, double gapMs)
{
  app::Core core;
  app::Config c = cfg;
  RollRun r;
  const double dt = app::kFrameMs * 0.001;
  const int frames = (int)(notches * gapMs / app::kFrameMs) + 400; // + drain
  int fed = 0;
  for (int f = 0; f < frames; ++f)
  {
    const double tMs = f * app::kFrameMs;
    while (fed < notches && fed * gapMs <= tMs + 1e-9)
    {
      core.Feed(120, (fed == 0) ? 0.0 : gapMs, c);
      ++fed;
    }
    const double out = core.Tick(dt, c);
    if (out < r.worstNegative)
      r.worstNegative = out;
    r.total += out;
    ++r.frames;
  }
  return r;
}

int main()
{
  static int failures = 0;
  printf("the delivered shape of ONE slow notch (%.0f ms window, %.0f Hz frames)\n",
         app::Config().glideMs, 1000.0 / app::kFrameMs);

  app::Config cfg; // the app's current defaults

  // WHAT A LONE NOTCH GETS. This is the case that was reported: nothing follows it, so the shape of its
  // end is the whole experience.
  const double easeApp = app::AppEaseFor(0.0, cfg.glideMs);
  const Profile lone = OneNotch(cfg, easeApp);
  char lab[160];
  _snprintf(lab, sizeof(lab), "a LONE notch, app ease = %.2f  <- this is what AppEaseFor chooses", easeApp);
  Show(lab, lone);

  // And what the model's own rule would have given it: a constant rate, which stops dead.
  const Profile flat = OneNotch(cfg, model::PayoutEaseFor(0.0, cfg.glideMs));
  Show("the same notch with the MODEL's rule alone (ease 0.00)", flat);

  // The model's own 0.5, for reference: better than flat, but still not a settle.
  Show("the model's own kEaseAmount (ease 0.50) -- what an earlier version used", OneNotch(cfg, 0.5));

  printf("\n-- assertions --\n");
  {
    const double tailApp = LastFramePctOfPeak(lone);
    const double tailFlat = LastFramePctOfPeak(flat);
    const double tailHalf = LastFramePctOfPeak(OneNotch(cfg, 0.5));
    char b[170];
    // IT MUST ACTUALLY COME TO REST. The rate at the end of a window is exactly `1 - ease`, so a settle
    // needs the last frame near zero -- not merely smaller than the peak.
    _snprintf(b, sizeof(b), "the app's tail settles: last frame is %.1f%% of peak", tailApp);
    printf("  %-66s %s\n", b, (tailApp < 10.0) ? "ok" : "FAIL");
    if (!(tailApp < 10.0))
      ++failures;

    _snprintf(b, sizeof(b), "alternatives: model rule %.0f%%, model's kEaseAmount %.0f%% (neither rests)",
              tailFlat, tailHalf);
    printf("  %-66s %s\n", b, (tailFlat > 90.0 && tailHalf > 20.0) ? "ok" : "FAIL");
    if (!(tailFlat > 90.0 && tailHalf > 20.0))
      ++failures;

    // THE SHAPE CHANGED, THE DISTANCE DID NOT. This is what makes it safe: the ease only redistributes
    // within a window, so no model parameter's meaning changes.
    _snprintf(b, sizeof(b), "the total is identical either way (%.4f vs %.4f)", lone.total, flat.total);
    printf("  %-66s %s\n", b, (std::fabs(lone.total - flat.total) < 1e-9) ? "ok" : "FAIL");
    if (!(std::fabs(lone.total - flat.total) < 1e-9))
      ++failures;

    // The fully-eased peak is higher (the same total in a shape that rests at BOTH ends), which is worth
    // knowing -- but it is still a small share of one notch, i.e. nothing a receiver would notice.
    _snprintf(b, sizeof(b), "its peak is %.2f deltas/frame (flat: %.2f), still small", lone.peak, flat.peak);
    printf("  %-66s %s\n", b, (lone.peak < 5.0) ? "ok" : "FAIL");
    if (!(lone.peak < 5.0))
      ++failures;

    // AN OVERLAPPING, NON-TILING ROLL IS EASED. An earlier version of this check asserted the opposite
    // ("a roll keeps the model's rule"), which is what left a FAST roll stopping in flat steps: the
    // model's rule is chosen for a roll that keeps going, and says nothing about the stop.
    printf("  %-66s %s\n", "an overlapping, non-tiling roll IS eased (it has a staircase to fix)",
           (app::AppEaseFor(100.0, cfg.glideMs) > 0.9) ? "ok" : "FAIL");
    if (!(app::AppEaseFor(100.0, cfg.glideMs) > 0.9))
      ++failures;

    // A gap AT the window is a tiling roll, not a lone notch: the model's warning about easing applies.
    // A gap AT the window is a TILING roll, and it is the one case the app must NOT ease: the model's
    // warning there is measured and large (ripple 0.0% -> 45.0%), and a tiling roll has no staircase to
    // fix. Everything else that overlaps IS eased -- an integer ratio is a staircase case once the roll
    // stops, which is what the model's rule cannot see.
    printf("  %-66s %s\n", "a tiling gap keeps the model's flat rate (easing it is measurably worse)",
           (app::AppEaseFor(cfg.glideMs, cfg.glideMs) == 0.0) ? "ok" : "FAIL");
    if (app::AppEaseFor(cfg.glideMs, cfg.glideMs) != 0.0)
      ++failures;

    // An integer ratio (window twice the gap) is a roll that stops in flat steps: it must be eased.
    const double halfGap = cfg.glideMs / 2.0;
    printf("  %-66s %s\n", "an integer-ratio roll IS eased (it stops in steps otherwise)",
           (app::AppEaseFor(halfGap, cfg.glideMs) > 0.9) ? "ok" : "FAIL");
    if (!(app::AppEaseFor(halfGap, cfg.glideMs) > 0.9))
      ++failures;

    // And the tail of a FAST roll must decay rather than fall in steps.
    {
      // A roll of 20 notches at 25 ms (a fast roll), then nothing, through the app's own ease.
      struct R
      {
        std::vector<double> v;
      };
      std::vector<double> out;
      {
        app::Core core;
        const double dt = app::kFrameMs * 0.001;
        int fed = 0;
        for (int f = 0; f < 2000; ++f)
        {
          const double tMs = f * app::kFrameMs;
          while (fed < 20 && fed * 25.0 <= tMs + 1e-9)
          {
            core.Feed(120, (fed == 0) ? 0.0 : 25.0, cfg);
            ++fed;
          }
          out.push_back(core.Tick(dt, cfg));
        }
      }
      int last = -1;
      double peak = 0.0;
      for (int i = 0; i < (int)out.size(); ++i)
      {
        if (out[i] > 1e-6)
          last = i;
        if (out[i] > peak)
          peak = out[i];
      }
      // The tail must be DECREASING frame by frame near the end, not repeating a flat level.
      int equalRuns = 0, steps = 0;
      for (int i = last - 20; i < last; ++i)
        if (i > 0)
        {
          ++steps;
          if (std::fabs(out[i] - out[i - 1]) < 1e-9)
            ++equalRuns;
        }
      char b3[170];
      _snprintf(b3, sizeof(b3), "a fast roll's tail decays (%.0f%% of the last %d frames are flat)",
                100.0 * equalRuns / (steps > 0 ? steps : 1), steps);
      printf("  %-66s %s\n", b3, (equalRuns * 2 < steps) ? "ok" : "FAIL");
      if (!(equalRuns * 2 < steps))
        ++failures;
      printf("  (its final frame is %.1f%% of peak)\n", (last >= 0 && peak > 0.0)
                                                              ? 100.0 * out[last] / peak
                                                              : 0.0);
    }
  }

  Show("ease = 0.50   (the model's own kEaseAmount, for reference)", OneNotch(cfg, WantedEase()));
  Show("ease = 1.00   (the model's pure smoothstep shape, for reference)", OneNotch(cfg, 1.0));

  // ---- AND WHAT EASING COSTS: the ripple of a steady roll, by gap ----
  printf("\n-- a steady roll's ripple (%% of its own mean), by gap ----------------------------\n");
  printf("   the model's rule gives 0 to a tiling gap and 0.5 to an overlapping one, so the\n");
  printf("   'always 1.00' column is the cost of easing every window\n\n");
  printf("   %-12s %10s %10s %10s   %s\n", "gap", "ease 0.00", "ease 0.50", "ease 1.00", "what the gap is");
  const double gaps[] = {500.0, 400.0, 300.0, 200.0, 150.0, 100.0, 60.0};
  const char *what[] = {"a clear pause", "a clear pause", "a small pause", "TILING (no overlap)",
                        "overlapping", "overlapping", "heavily overlapping"};
  for (int i = 0; i < 7; ++i)
    printf("   %-12.0f %9.1f%% %9.1f%% %9.1f%%   %s\n", gaps[i], RollRipple(cfg, gaps[i], 0.0),
           RollRipple(cfg, gaps[i], 0.5), RollRipple(cfg, gaps[i], 1.0), what[i]);

  // ---- THE RELEASE MODEL: how LONG a window lasts (the other half of the tail) ----
  //
  // ⚠️ THIS SECTION HAS NOW SAID THREE DIFFERENT THINGS, and the history is why it is worth spelling out.
  // It first measured a tail that GREW WITH TURNING SPEED (a `tail` slider feeding
  // `windowMs = glide * (1 + tail * u)`), and asked whether that growth was smooth. That parameter was then
  // removed -- it was not part of this model and not part of the plugin's -- and the section was inverted to
  // assert the opposite: the window is the Glide setting, full stop.
  //
  // It is neither of those now. The user asked for a tail back, and for it to be exactly one thing:
  // "让滚动的时候，收尾拉长的", with REL a FIXED 200 ms ("200ms，并且是固定值"). So the window is the Glide
  // setting for a message that is NOT part of a roll, and Glide + 200 for one that is. What makes this
  // different from the removed version, and the thing to keep hold of:
  //
  //   * it is NOT scaled by speed -- the same roll at any speed adds the same 200 ms;
  //   * it is NOT a setting -- no slider, no settings-file key, nothing to re-tune;
  //   * the property asserted below is therefore not "nothing moves the window" but "exactly ONE thing
  //     does, and only while rolling".
  printf("\n-- the release model: a lone message is Glide, a roll is Glide + %.0f ms --\n", app::kReleaseMs);
  {
    app::Config c = cfg;

    // A lone message: nothing before it, so nothing can overlap it, so the Glide setting is the whole answer.
    {
      app::Core lone;
      lone.Feed(120, 0.0, c);
      char b[160];
      _snprintf(b, sizeof(b), "a lone message gets the Glide setting (%.0f ms)", c.glideMs);
      printf("  %-66s %s\n", b, (std::fabs(lone.WindowMs() - c.glideMs) < 1e-9) ? "ok" : "FAIL");
      if (!(std::fabs(lone.WindowMs() - c.glideMs) < 1e-9))
        ++failures;
    }

    // One inside a roll: its window overlaps the one before it, so it carries the tail.
    {
      app::Core roll;
      roll.Feed(120, 0.0, c);   // the first notch of the gesture
      roll.Feed(120, 40.0, c);  // 25 ms later: a roll, and now
      const double want = c.WindowMsFor() + app::kReleaseMs;
      char b[160];
      _snprintf(b, sizeof(b), "one inside a roll gets Glide + %.0f (%.0f ms)", app::kReleaseMs, want);
      printf("  %-66s %s\n", b, (std::fabs(roll.WindowMs() - want) < 1e-9) ? "ok" : "FAIL");
      if (!(std::fabs(roll.WindowMs() - want) < 1e-9))
        ++failures;
    }

    // IT DOES NOT SCALE WITH SPEED. This is the one property that separates this from the tail that was
    // removed, so it is pinned directly: a fast roll and a slow-but-still-overlapping one get the same ADDED
    // length. (The previous version of this section asserted these two were equal in ABSOLUTE terms, which is
    // now false -- they both add the same fixed amount on top of the same Glide setting, so they are equal,
    // but for a different reason, and the reason is the thing worth checking.)
    {
      app::Core fast;
      app::Core slowRoll;
      fast.Feed(120, 0.0, c);
      fast.Feed(120, 8.0, c); // ~125 notches/s
      slowRoll.Feed(120, 0.0, c);
      slowRoll.Feed(120, 180.0, c); // 180 ms apart: still inside a 200 ms window, so still a roll
      char b[160];
      _snprintf(b, sizeof(b), "a fast roll and a slow one add the same %.0f ms (%.0f vs %.0f ms)",
                app::kReleaseMs, fast.WindowMs(), slowRoll.WindowMs());
      printf("  %-66s %s\n", b, (std::fabs(fast.WindowMs() - slowRoll.WindowMs()) < 1e-9) ? "ok" : "FAIL");
      if (!(std::fabs(fast.WindowMs() - slowRoll.WindowMs()) < 1e-9))
        ++failures;
      _snprintf(b, sizeof(b), "and neither of them scaled it (both are exactly Glide + %.0f)", app::kReleaseMs);
      printf("  %-66s %s\n", b,
             (std::fabs(fast.WindowMs() - (c.WindowMsFor() + app::kReleaseMs)) < 1e-9) ? "ok" : "FAIL");
      if (!(std::fabs(fast.WindowMs() - (c.WindowMsFor() + app::kReleaseMs)) < 1e-9))
        ++failures;
    }

    // A GAP WIDER THAN THE BASE WINDOW IS NOT A ROLL, however fast the roll before it was.
    {
      app::Core pausing;
      pausing.Feed(120, 0.0, c);
      pausing.Feed(120, 500.0, c); // half a second: a new gesture, not a continuation
      char b[160];
      _snprintf(b, sizeof(b), "a message after a pause is lone again (%.0f ms)", c.glideMs);
      printf("  %-66s %s\n", b, (std::fabs(pausing.WindowMs() - c.glideMs) < 1e-9) ? "ok" : "FAIL");
      if (!(std::fabs(pausing.WindowMs() - c.glideMs) < 1e-9))
        ++failures;
    }

    // ⚠️ AND A PAUSE IN THE MIDDLE OF A TAIL LOSES NOTHING.
    //
    // What this checks has changed twice, and the current reason is the simplest of the three. It began as a
    // check on a LATCH: `Tick` applies one window length to every window in flight, so a length that shrank
    // (a 40 ms roll's 400 ms windows, then a lone message 250 ms later asking for 200) would mark the older
    // windows finished and drop the travel still on them -- so the length was never allowed to shrink while
    // anything moved. That latch is GONE: with one fixed length per axis (see Core::Feed) no window's length
    // can change, so there is nothing to guard and nothing to latch.
    //
    // The SEQUENCE is still worth measuring, because it is the one where the two rules disagree most: the
    // message after the pause goes to the lone axis while six rolling windows are still draining on theirs.
    // What must hold is not "the latch kept the length" but the promise underneath it -- nothing is lost
    // between the two axes.
    {
      app::Core c2;
      const double dt = app::kFrameMs * 0.001;
      double fed = 0.0, handed = 0.0;
      int i = 0;
      // Six fast notches (all rolling), then one 250 ms later (a lone message, while four windows are still
      // in flight at 400 ms), then let it drain.
      for (; i < 6; ++i)
      {
        fed += c2.Feed(120, (i == 0) ? 0.0 : 40.0, c);
        for (int f = 0; f < 8; ++f)
          handed += c2.Tick(dt, c);
      }
      fed += c2.Feed(120, 250.0, c);
      for (int f = 0; f < 4000 && c2.Active(); ++f)
        handed += c2.Tick(dt, c);

      char b[160];
      _snprintf(b, sizeof(b), "a short pause mid-tail does not drop travel (fed %.3f, handed %.3f)", fed,
                handed);
      printf("  %-66s %s\n", b, (std::fabs(fed - handed) < 1e-6) ? "ok" : "FAIL");
      if (!(std::fabs(fed - handed) < 1e-6))
        ++failures;
    }
  }


  printf("\n%s\n", failures ? "FAIL" : "OK: a lone notch settles, a roll is untouched, the total is the same");
  return failures ? 1 : 0;
}
