// WHAT THE RELEASE MODEL DOES TO THE END OF A ROLL -- measured, side by side with the rule it replaces.
//
// The request behind this file is one sentence: "让滚动的时候，收尾拉长的" -- while rolling, the ending should
// be drawn out. Two things follow, and both are measured here rather than argued:
//
//   1. THE ENDING IS ACTUALLY LONGER. That is the whole point, and a change that altered the window without
//      moving this number would be a change that did not do what was asked. The number is
//      "milliseconds of motion after the last notch", and it is the tail's length and nothing else.
//
//   2. A LONE SCROLL IS UNTOUCHED. The release model's other half is that a message which is not part of a
//      roll keeps the Glide setting exactly. A single scroll is the case the Glide slider was tuned on, so
//      if it moved, the slider would have quietly changed meaning.
//
// THE COMPARISON IS RUN AGAINST TWO CORES FED THE SAME MESSAGES: the shipping one (`app::Core`, which decides
// its window through the release model) and a replica of the previous rule (`OldFeed` below, which is the one
// line `windowMs_ = cfg.WindowMsFor()`). The replica is written out here on purpose: it is the thing being
// replaced, and it should stay visible in the probe rather than be reconstructed from a config flag.
//
// g++ -std=c++17 -O2 -I common -I shared _diag/app_release_probe.cpp -o /tmp/arel && /tmp/arel
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "core.h"

static int failures = 0;
static void Check(bool ok, const char *what)
{
  printf("  %-68s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++failures;
}

struct Shape
{
  double totalMs = 0.0;   // how long motion lasted, first frame to last
  double tailMs = 0.0;    // time from the LAST NOTCH to the last frame -- the ending's length
  double peak = 0.0;
  double total = 0.0;     // deltas handed over
  double lastPct = 0.0;   // the final frame as a % of the peak rate
  // THE ONSET LURCH: the step at the frame where a ROLL IS RECOGNISED (the second message), divided by the
  // average of the steps beside it. That single frame is where the release model used to re-scale the windows
  // already in flight and halve the rate -- see section 5 in main(). A smooth curve gives ~1.0 here whatever
  // its slope, because neighbours on a smooth curve move alike.
  double onsetLurch = 0.0;
  int onsetAt = -1;
  double onsetStep = 0.0;
  double onsetNeighbours = 0.0;
  int frames = 0;
};

// THE OLD RULE, kept here to be measured against: one window length, the Glide setting, for every message.
struct OldCore
{
  model::Axis glide;
  model::SpeedBudget budget;
  double Feed(int delta, double gapMs, const app::Config &cfg)
  {
    const double mag = (delta < 0) ? -(double)delta : (double)delta;
    const double b = budget.Add(mag, gapMs);
    const double travel = model::Travel(mag, b, cfg.rampUp, cfg.slowStep, cfg.topSpeed);
    model::Params P;
    P.windowMs = cfg.WindowMsFor();
    P.payoutEase = app::AppEaseFor(gapMs, P.windowMs);
    const double signedTravel = (delta < 0) ? -travel : travel;
    glide.Feed(signedTravel, P);
    return signedTravel;
  }
  bool Active() const { return glide.Active(); }
  double Tick(double dt, const app::Config &cfg)
  {
    model::Params P;
    P.windowMs = cfg.WindowMsFor();
    P.payoutEase = 0.0;
    return glide.Tick(dt, P);
  }
};

// Feed `notches` messages `gapMs` apart, then drain. `lastNotchMs` is when the final message arrived, which
// is the instant the ending starts from.
template <typename Core>
static Shape Run(const app::Config &cfg, int notches, double gapMs, bool release)
{
  Core core;
  const double dt = app::kFrameMs * 0.001;
  std::vector<double> out;
  double lastNotchMs = 0.0;
  int fed = 0;
  for (int f = 0; f < 400000; ++f)
  {
    const double tMs = f * app::kFrameMs;
    while (fed < notches && fed * gapMs <= tMs + 1e-9)
    {
      const double g = (fed == 0) ? 0.0 : gapMs;
      if (release)
        core.Feed(120, g, cfg);
      else
        core.Feed(120, g, cfg); // same signature; the difference is which Core type
      lastNotchMs = tMs;
      ++fed;
    }
    out.push_back(core.Tick(dt, cfg));
    if (fed >= notches && !core.Active())
      break;
  }

  Shape s;
  s.frames = (int)out.size();
  s.totalMs = (s.frames > 0) ? (s.frames - 1) * app::kFrameMs : 0.0;
  int last = -1, first = -1;
  for (int i = 0; i < (int)out.size(); ++i)
  {
    if (out[i] > 1e-12)
    {
      if (first < 0)
        first = i;
      last = i;
    }
  }
  for (double x : out)
  {
    s.total += x;
    if (x > s.peak)
      s.peak = x;
  }
  s.tailMs = (last >= 0) ? (last * app::kFrameMs - lastNotchMs) : 0.0;
  s.lastPct = (s.peak > 0.0 && last >= 0) ? 100.0 * out[last] / s.peak : 0.0;

  // ---- THE ONSET LURCH: the one frame that was broken ----
  //
  // ⚠️ THIS MEASURES ONE SPECIFIC FRAME, AND THAT IS THE POINT. Three earlier attempts tried to detect "a
  // jump" anywhere in the profile, and each reported its own metric instead of the motion:
  //
  //   1. a step against the rate BEFORE it flagged every taper's final frame -- the rate heads to zero by
  //      design there, so 0.121 -> 0.041 reads as 66%, and the same 66.3% came out at three different gaps.
  //   2. a step against its neighbourhood MEDIAN flagged ordinary acceleration -- on a ramp the steps grow
  //      steadily, so the median sits below them and smooth rolls scored 110-398%.
  //   3. the neighbour-average version (kept, but aimed) still caught the very last frame, where the model's
  //      own accounting makes the final payout smaller than the trend by construction.
  //
  // THE DEFECT WAS NONE OF THOSE. It was ONE frame: the moment a roll is recognised, where the window grew
  // and the model re-spread the windows already in flight, collapsing the rate 67% in a single step. That
  // frame is known exactly -- it is the second message -- so this measures THAT frame and nothing else, which
  // is robust by construction: it never has to decide what a jump looks like in general.
  {
    const int n = (int)out.size();
    if (n >= 4 && fed >= 2)
    {
      // The frame the second message arrived on, from the schedule rather than by searching the output.
      //
      // ⚠️ NO `+ 1`, AND GETTING THAT WRONG MADE THIS CHECK DECORATIVE. Messages arrive at t = 0, gap, 2*gap,
      // ...; the loop feeds one BEFORE ticking, so the frame whose tick first sees the longer window is
      // round(gap / frame) -- the same frame the message landed on. An off-by-one put the measurement on the
      // following frame, where the rate is already recovering, and the broken single-axis core then scored
      // 1.32x and PASSED. (Found by reverting the fix on purpose and watching the gate stay green: a check
      // that cannot fail is worse than no check, because it is counted as one.)
      const int onset = (int)(gapMs / app::kFrameMs + 0.5);
      if (onset >= 2 && onset < n - 1)
      {
        std::vector<double> step(n, 0.0);
        for (int i = 1; i < n; ++i)
          step[i] = std::fabs(out[i] - out[i - 1]);
        const double nb = 0.5 * (step[onset - 1] + step[onset + 1]);
        s.onsetAt = onset;
        s.onsetStep = step[onset];
        s.onsetNeighbours = nb;
        if (nb > 1e-5)
          s.onsetLurch = step[onset] / nb;
      }
    }
  }
  return s;
}

static void Row(const char *name, const Shape &oldS, const Shape &newS)
{
  printf("  %-30s old %7.0f ms  ->  new %7.0f ms   (%+.0f ms)\n", name, oldS.tailMs, newS.tailMs,
         newS.tailMs - oldS.tailMs);
}

int main()
{
  printf("the release model: what it does to the end of a motion\n");
  printf("(tail = the motion that follows the LAST notch; the window's own length, not the travel)\n\n");

  app::Config cfg; // the shipped defaults: glide 200 ms, slow 5, ramp 500, top 1.5

  printf("-- 1. a ROLL: the ending must be longer --\n");
  {
    const double gaps[] = {8.0, 25.0, 60.0, 100.0, 150.0, 180.0};
    const char *names[] = {"very fast (8 ms apart)", "fast (25 ms)",      "brisk (60 ms)",
                           "steady (100 ms)",       "slow (150 ms)",      "at the edge (180 ms)"};
    for (int i = 0; i < 6; ++i)
    {
      const Shape o = Run<OldCore>(cfg, 20, gaps[i], false);
      const Shape n = Run<app::Core>(cfg, 20, gaps[i], true);
      Row(names[i], o, n);
    }
    // The promise, exactly: every one of those rolls ends 200 ms later than it used to.
    bool all = true;
    for (int i = 0; i < 6; ++i)
    {
      const Shape o = Run<OldCore>(cfg, 20, gaps[i], false);
      const Shape n = Run<app::Core>(cfg, 20, gaps[i], true);
      if (std::fabs((n.tailMs - o.tailMs) - app::kReleaseMs) > app::kFrameMs + 1e-9)
        all = false;
    }
    char b[160];
    _snprintf(b, sizeof(b), "every rolling gap ends exactly %.0f ms later", app::kReleaseMs);
    Check(all, b);
  }

  printf("\n-- 2. a LONE scroll: nothing may change --\n");
  {
    const Shape o = Run<OldCore>(cfg, 1, 0.0, false);
    const Shape n = Run<app::Core>(cfg, 1, 0.0, true);
    Row("one notch", o, n);

    char b[160];
    _snprintf(b, sizeof(b), "the tail is identical (%.0f vs %.0f ms)", o.tailMs, n.tailMs);
    Check(std::fabs(o.tailMs - n.tailMs) < 1e-9, b);
    _snprintf(b, sizeof(b), "and so is the travel handed over (%.4f vs %.4f)", o.total, n.total);
    Check(std::fabs(o.total - n.total) < 1e-9, b);
    _snprintf(b, sizeof(b), "its peak rate is unchanged too (%.4f vs %.4f)", o.peak, n.peak);
    Check(std::fabs(o.peak - n.peak) < 1e-9, b);
  }

  printf("\n-- 3. the change is FIXED, not proportional --\n");
  {
    // The removed version of this idea scaled the tail by turning speed, so a fast roll and a slow one got
    // different additions. This one adds the same amount to every roll, which is what "固定值" asks for.
    double addFast = 0.0, addSlow = 0.0;
    {
      const Shape o = Run<OldCore>(cfg, 20, 8.0, false);
      const Shape n = Run<app::Core>(cfg, 20, 8.0, true);
      addFast = n.tailMs - o.tailMs;
    }
    {
      const Shape o = Run<OldCore>(cfg, 20, 180.0, false);
      const Shape n = Run<app::Core>(cfg, 20, 180.0, true);
      addSlow = n.tailMs - o.tailMs;
    }
    char b[160];
    _snprintf(b, sizeof(b), "a very fast roll and a slow one both add %.0f ms (%.0f vs %.0f)", app::kReleaseMs,
              addFast, addSlow);
    Check(std::fabs(addFast - addSlow) < 1e-9, b);
    _snprintf(b, sizeof(b), "and neither is scaled by speed (both exactly %.0f)", app::kReleaseMs);
    Check(std::fabs(addFast - app::kReleaseMs) < 1e-9 && std::fabs(addSlow - app::kReleaseMs) < 1e-9, b);
  }

  printf("\n-- 4. it follows the Glide slider, and adds to it --\n");
  {
    // Glide goes on meaning what it meant: the model's envelope (100..400 ms) still applies to the base, and
    // the addition sits on top of it rather than replacing anything.
    bool ok = true;
    for (int g = 100; g <= 300; g += 50)
    {
      app::Config c = cfg;
      c.glideMs = (double)g;
      const double base = c.WindowMsFor();
      const double roll = app::ReleaseRollWindowMs(base);
      printf("  Glide %3d ms -> lone %3.0f ms, rolling %3.0f ms\n", g, base, roll);
      if (std::fabs(roll - (base + app::kReleaseMs)) > 1e-9)
        ok = false;
    }
    Check(ok, "the rolling window is always the Glide setting plus the fixed addition");
  }

  // ---- 5. NO JUMP: the rate must not lurch when a roll is recognised ----
  //
  // ⚠️ THIS SECTION EXISTS BECAUSE THE FEATURE WAS SHIPPED WRONG ONCE. The first version of the release model
  // kept ONE axis and changed its window under the windows already in flight -- and because the model applies
  // one length per Tick call to every window (`anim3::Glide::Tick`'s `w`), that re-spread them: the rate
  // HALVED in a single frame the moment a roll began. The user's report was "快速滚动会出现跳的现象，应该跟最后加的
  // 这200ms有关" -- correct on both counts. Measured at a 25 ms gap, the rate went 0.4423 -> 0.1475 at frame 7
  // (a 67% collapse), and it was worse the slower the roll: 76% at 100 ms and 90% at the 180 ms edge.
  //
  // THE FIX WAS STRUCTURAL: one fixed length per axis, so no window's length ever changes. The check below is
  // therefore not a nicety -- it is the property that separates the working version from the broken one, and
  // the numbers are pinned rather than merely printed, because a future "small simplification" back to one
  // axis would otherwise pass every other check in this file (the ending would still be long and the totals
  // would still balance -- the latch even kept them from being lost).
  printf("\n-- 5. no lurch at the onset: the frame where a roll is recognised --\n");
  {
    // A 0 ms column is deliberately absent: with no addition there is no window change at the onset, so there
    // was never anything to lurch -- which is exactly why the defect arrived with this feature and not before.
    const double gaps[] = {8.0, 16.0, 25.0, 40.0, 60.0, 100.0, 150.0, 180.0};
    const char *names[] = {"a flick (8 ms)",  "very fast (16 ms)", "fast (25 ms)",    "brisk (40 ms)",
                           "steady (60 ms)",  "slow (100 ms)",     "slower (150 ms)", "at the edge (180 ms)"};
    double worst = 0.0;
    const char *worstAt = "";
    for (int i = 0; i < 8; ++i)
    {
      const Shape sh = Run<app::Core>(cfg, 20, gaps[i], true);
      printf("   %-18s onset step %.4f, steps beside it %.4f  ->  %.2fx\n", names[i], sh.onsetStep,
             sh.onsetNeighbours, sh.onsetLurch);
      if (sh.onsetLurch > worst)
      {
        worst = sh.onsetLurch;
        worstAt = names[i];
      }
    }
    char b[220];
    // ⚠️ WHAT SEPARATES FIXED FROM BROKEN IS THIS RATIO, and the two are far apart: with one axis the onset
    // frame's step ran to 3x its neighbours and beyond (the rate halved against neighbours that were only
    // ramping), and with two axes it is ~1.0-1.3x, because the onset is then just the next step of a smooth
    // ramp. 2.0 sits between them with room on both sides, and it is stated with that evidence rather than as
    // a magic number.
    _snprintf(b, sizeof(b), "no lurch at the onset (worst %.2fx at %s; the one-axis version was 3x and up)",
              worst, worstAt);
    Check(worst < 2.0, b);
  }

  printf("\n-- 6. the envelope still holds at the top of the Glide range --\n");
  {
    // ⚠️ AT GLIDE 300 THE ROLLING WINDOW IS 500 ms, WHICH IS PAST THE MODEL'S OWN 100..400 ENVELOPE, AND THAT
    // IS EXPECTED RATHER THAN A BUG. The envelope is stated by the MODEL for how long one amount may be
    // spread, and it is applied to the BASE window -- the thing the user's slider controls. The release
    // model's addition is the app's own decision about a roll, and the model has no opinion about it: 500 ms
    // of window at 250 notches/s is 125 windows in flight, well inside the model's own 2048 cap. What is
    // worth pinning is that it stays far from that cap, because that is the only hard limit down here.
    app::Config c = cfg;
    c.glideMs = 300.0;
    const double roll = app::ReleaseRollWindowMs(c.WindowMsFor());
    const double fastestGap = 8.0; // a very fast roll
    const int inFlight = (int)(roll / fastestGap) + 1;
    char b[160];
    _snprintf(b, sizeof(b), "even Glide 300 + release is %.0f ms = ~%d windows in flight (cap %d)", roll,
              inFlight, model::Axis::kMaxEnv);
    Check(inFlight < model::Axis::kMaxEnv / 4, b);
  }

  printf("\n%s\n", failures ? "FAIL" : "OK: rolling ends 200 ms later, a lone scroll is untouched");
  return failures ? 1 : 0;
}
