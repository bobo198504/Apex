// Does the app's core conserve travel -- and does it attenuate the way the model says?
//
// TWO DIFFERENT THINGS, easily confused, and confusing them makes a working app look broken:
//
//   1. ATTENUATION (by design). A slow roll is supposed to move LESS than the wheel reported: the
//      model's Travel() ramps from `slowStep` deltas up to the message's own size as the wheel speeds
//      up. So "75 notches in, 9000 deltas, but only ~1900 handed over" is the model WORKING. This was
//      misread once from a raw log, which is why the numbers are pinned here.
//
//   2. CONSERVATION (must hold exactly). Whatever the core is FED, it must hand over in full: the
//      timing spreads an amount without changing it. If a slow roll is fed 5 deltas it hands over 5,
//      not 4. A shortfall here would be a real bug (a lost remainder, a dropped window).
//
// It includes the REAL app/core.h, so it measures the shipping smoothing.
//
// g++ -std=c++17 -O2 -I. -Isrc _diag/app_core_probe.cpp -o /tmp/acore && /tmp/acore
#include <cstdio>
#include <cmath>
#include "core.h"

static int failures = 0;
static void Check(bool ok, const char *what)
{
  printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++failures;
}

// Run the core over a scripted roll and total what it was fed against what it handed over.
struct Run
{
  double fed = 0.0;     // what the core was handed to spread
  double handed = 0.0;  // what came out of Tick over the whole run
  int messages = 0;
};

static Run ScriptedRoll(const app::Config &cfg, int notches, double gapMs)
{
  app::Core core;
  const app::Config &c = cfg; // the caller's config is used as-is (incl. the master switch)
  Run r;
  const double dt = app::kFrameMs * 0.001;
  for (int i = 0; i < notches; ++i)
  {
    const double gap = (i == 0) ? 0.0 : gapMs;
    r.fed += core.Feed(120, gap, c);
    ++r.messages;
    // Advance one gap's worth of frames before the next message, so the timing is exercised.
    const int frames = (int)(gapMs / app::kFrameMs + 0.5);
    for (int f = 0; f < frames; ++f)
      r.handed += core.Tick(dt, c);
  }
  // Drain: the last windows still owe their remainder.
  for (int f = 0; f < 20000 && core.Active(); ++f)
    r.handed += core.Tick(dt, c);
  return r;
}

int main()
{
  printf("app core: attenuation is by design, conservation is exact\n\n");

  app::Config cfg; // the shipped defaults: glide 200 ms, slow step 5, ramp-up 1000, top 1.5

  // ---- CONSERVATION: what was fed is handed over in full, whatever the speed ----
  printf("-- conservation (fed must equal handed, to a fraction) --\n");
  {
    struct Case { const char *name; int notches; double gapMs; };
    const Case cases[] = {
        {"one notch", 1, 0.0},
        {"a slow roll (400 ms apart)", 10, 400.0},
        {"a steady roll (100 ms apart)", 10, 100.0},
        {"a fast roll (25 ms apart)", 20, 25.0},
        {"a very fast roll (8 ms apart)", 40, 8.0},
        // ⚠️ AND THE GAPS THAT STRADDLE THE RELEASE MODEL'S BOUNDARY. The window a message asks for now
        // depends on whether it is part of a roll (see common/release.h), so the gaps whose window changes
        // by 200 ms from one message to the next are exactly where travel could be lost if the rule were
        // written carelessly -- a shrinking window drops whatever an in-flight window still holds. A gap
        // just under the base window is a roll; just over it is not, so these two sit on either side of a
        // 200 ms step in the window, which is the worst case the rule has.
        {"a roll right at the boundary (180 ms apart)", 10, 180.0},
        {"a roll that keeps pausing (250 ms apart)", 10, 250.0},
    };
    for (const Case &k : cases)
    {
      const Run r = ScriptedRoll(cfg, k.notches, k.gapMs);
      const double diff = std::fabs(r.handed - r.fed);
      char line[128];
      _snprintf(line, sizeof(line), "%s: fed %.3f, handed %.3f, diff %.6f", k.name, r.fed, r.handed,
                diff);
      Check(diff < 1e-6, line);
    }
  }

  // ---- CONSERVATION UNDER AN ALTERNATING PATTERN ----
  //
  // The scripted rolls above each hold one gap, so each one settles into a single window length and stays
  // there. The release model only ever changes the length when a message's gap crosses the base window, so a
  // pattern that crosses it on EVERY message is the worst case for it: the window asks to grow, then shrink,
  // then grow, with windows always still in flight.
  //
  // (It must not shrink under a running window -- `Tick` applies one length to all of them, so a shorter one
  // marks the older windows finished and drops their remaining travel. See Core::Feed.)
  printf("\n-- conservation under a pattern that crosses the release boundary every message --\n");
  {
    struct Pattern { const char *name; const double *gaps; int n; };
    const double rollPause[] = {40.0, 250.0, 40.0, 250.0, 40.0, 250.0, 40.0, 250.0, 40.0, 250.0};
    const double sweep[] = {40.0, 300.0, 60.0, 210.0, 20.0, 500.0, 80.0, 120.0, 350.0, 90.0};
    const Pattern pats[] = {
        {"roll, pause, roll, pause ...", rollPause, 10},
        {"an erratic sweep across the boundary", sweep, 10},
    };
    for (const Pattern &p : pats)
    {
      app::Core core;
      const double dt = app::kFrameMs * 0.001;
      double fed = 0.0, handed = 0.0;
      for (int i = 0; i < p.n; ++i)
      {
        fed += core.Feed(120, (i == 0) ? 0.0 : p.gaps[i], cfg);
        // Advance this gap's worth of frames before the next message, so windows really do overlap.
        const int frames = (i == 0) ? 0 : (int)(p.gaps[i] / app::kFrameMs + 0.5);
        for (int f = 0; f < frames; ++f)
          handed += core.Tick(dt, cfg);
      }
      for (int f = 0; f < 20000 && core.Active(); ++f)
        handed += core.Tick(dt, cfg);
      char line[160];
      _snprintf(line, sizeof(line), "%s: fed %.3f, handed %.3f, diff %.6f", p.name, fed, handed,
                std::fabs(fed - handed));
      Check(std::fabs(fed - handed) < 1e-6, line);
    }
  }

  // ---- ATTENUATION: a slow roll moves less than the wheel reported, on purpose ----
  printf("\n-- attenuation (by design; see Travel in model.h) --\n");
  {
    // One notch on its own: the budget is only that notch's own deltas, so the travel is between
    // slowStep and 120 -- never more than the message itself.
    const Run one = ScriptedRoll(cfg, 1, 0.0);
    Check(one.fed > 0.0 && one.fed < 120.0, "a single slow notch is attenuated below a full notch");
    Check(one.fed >= cfg.slowStep - 1e-9, "and never below the Slow step floor");

    // A sustained roll builds the speed budget, so its per-notch travel must be LARGER than a lone
    // notch's -- that is the ramp doing its job.
    const Run roll = ScriptedRoll(cfg, 12, 60.0);
    const double perMsgRoll = roll.fed / 12.0;
    Check(perMsgRoll > one.fed, "a sustained roll moves more per notch than a lone one (the ramp)");

    // And it must not exceed the wheel's own size times the Top speed ceiling.
    Check(perMsgRoll <= 120.0 * cfg.topSpeed + 1e-9, "and never past the Top speed ceiling");
  }

  // ---- there is no master switch any more ----
  //
  // ⚠️ THE SECTION THAT USED TO BE HERE SET `cfg.enabled = false` AND ASSERTED THE CORE FED NOTHING. That
  // field is gone: this feature has no switch of its own (enabling is the host's generic per-feature switch,
  // and the host does not even call a feature that is switched off -- see common/config.h). What is still
  // worth asserting is the other side of the same promise, which did NOT move: an idle core is silent, so a
  // feature that is on but has not been fed invents no motion.
  printf("\n-- an idle core is silent --\n");
  {
    app::Core idle;
    double handed = 0.0;
    for (int i = 0; i < 40; ++i)
      handed += idle.Tick(0.004, cfg);
    Check(handed == 0.0, "a core that was never fed hands nothing over");
    Check(!idle.Active(), "and reports itself idle");
  }

  // ---- the four parameters each move the number they claim to ----
  printf("\n-- the parameters do what they say --\n");
  {
    const Run base = ScriptedRoll(cfg, 12, 60.0);

    app::Config slow = cfg;
    slow.slowStep = 10.0; // a bigger Slow step -> more travel at the slow end
    const Run slowRun = ScriptedRoll(slow, 12, 60.0);
    Check(slowRun.fed > base.fed, "raising Slow step raises the travel");

    app::Config ramp = cfg;
    ramp.rampUp = 2000.0; // a longer ramp -> less travel at the same speed
    const Run rampRun = ScriptedRoll(ramp, 12, 60.0);
    Check(rampRun.fed < base.fed, "lengthening Ramp-up lowers it (the budget fills slower)");

    app::Config glide = cfg;
    glide.glideMs = 300.0; // timing only: must NOT change how far, only how long
    const Run glideRun = ScriptedRoll(glide, 12, 60.0);
    Check(std::fabs(glideRun.fed - base.fed) < 1e-9, "Glide length changes the timing, not the travel");

    app::Config top = cfg;
    top.topSpeed = 2.0; // only bites at full speed
    const Run topRun = ScriptedRoll(top, 12, 60.0);
    Check(topRun.fed >= base.fed, "Top speed never lowers the travel");
  }

  // ---- the defaults, and WHY they are what they are ----
  //
  // ⚠️ ALL FOUR DEFAULTS ARE THE PLUGIN'S, AND THIS HAS NOW BEEN BOTH WAYS. The history is kept because the
  // last reversal is the one a future reader will otherwise repeat:
  //
  //   * Ramp-up was halved to 500 here, on my own feel argument (a REAPER-scale ramp reads as sluggish when
  //     this program is the only thing moving a view);
  //   * the user said the slider's DEFAULT was wrong ("拉杆的默认值不是原来的"), and this check was written to
  //     assert 500 -- which was a misreading of that remark, since what they meant was the original value;
  //   * and they have now said it outright: "加速基准，默认应该是1000".
  //
  // The rule that keeps this from happening a third time: THE SHARED MODEL COMES WITH SHARED NUMBERS. The
  // headers in shared/ are the plugin's byte for byte, so the two products should also START from the same
  // tuning; a deliberate difference needs the user asking for it, not a maintainer's judgement about feel.
  // The range is untouched (60..2000), so anyone who wants the old behaviour moves one slider.
  printf("\n-- the defaults match the plugin's --\n");
  {
    Check(cfg.glideMs == 200.0, "Glide is 200 ms (the plugin's)");
    Check(cfg.slowStep == 5.0, "Slow step is 5 (the plugin's)");
    Check(cfg.rampUp == 1000.0, "Ramp-up is 1000 (the plugin's too -- see the note above)");
    Check(cfg.topSpeed == 1.5, "Top speed is 1.5x (the plugin's)");

    // A LONGER RAMP MOVES LESS AT A GIVEN SPEED -- asserted as the RELATIONSHIP between two values rather
    // than against the default, so it keeps meaning something whichever one the app ships. (The first version
    // of this check compared "shorter" against `cfg`, which stopped being able to differ the moment the
    // default changed -- a comparison that cannot fail is not a check.)
    app::Config longRamp = cfg;
    longRamp.rampUp = 1000.0;
    app::Config shortRamp = cfg;
    shortRamp.rampUp = 500.0;
    const Run withLong = ScriptedRoll(longRamp, 12, 250.0); // 4 notches/s
    const Run withShort = ScriptedRoll(shortRamp, 12, 250.0);
    char line[160];
    _snprintf(line, sizeof(line), "at 4 notches/s: ramp 1000 moves %.1f deltas, ramp 500 moves %.1f (%.1fx)",
              withLong.fed / 12.0, withShort.fed / 12.0,
              (withLong.fed > 0.0) ? (withShort.fed / withLong.fed) : 0.0);
    Check(withShort.fed > withLong.fed * 1.3, line);

    // AND THE CEILING IS THE MODEL'S, NOT A SCALED ONE. A shorter ramp reaches full speed at a LOWER
    // turning speed, so at one given speed the app's travel can be higher -- that is the point of it.
    // What must hold is different, and stronger: the travel must never exceed what the model itself
    // allows, cap x topSpeed = 120 x 1.5 = 180, at ANY speed and ANY ramp. A gain on the output could
    // break that (2 x 120 x 1.5 = 360); the model's own parameter cannot. (An earlier version of this
    // check asserted the two ramps reach the SAME value at one speed -- which is simply false, because
    // the budget grows with speed and a shorter ramp saturates sooner. The mistake was mine, in the
    // claim, not in the code.)
    app::Config rampLo = cfg;
    rampLo.rampUp = app::Config::RampLo(); // the shortest ramp the slider allows: the worst case
    const double kCeiling = 120.0 * cfg.topSpeed;
    bool anyOver = false;
    double worst = 0.0;
    const double gaps[] = {400.0, 250.0, 120.0, 60.0, 25.0, 8.0}; // slow .. very fast
    for (int i = 0; i < 6; ++i)
    {
      const Run r = ScriptedRoll(rampLo, 40, gaps[i]);
      const double per = r.fed / 40.0;
      if (per > worst)
        worst = per;
      if (per > kCeiling + 1e-9)
        anyOver = true;
    }
    char line2[160];
    _snprintf(line2, sizeof(line2), "even the shortest ramp never passes the model's ceiling (%.1f <= %.1f)",
              worst, kCeiling);
    Check(!anyOver, line2);
    Check(worst > 0.0, "and it does move (the check is not vacuous)");
  }

  printf("\n%s\n", failures ? "FAIL" : "OK: conservation exact, attenuation as designed, values behave");
  return failures ? 1 : 0;
}
