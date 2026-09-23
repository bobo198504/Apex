// HOW FAST THE APP FEELS, in numbers: how far one notch moves at a given turning speed.
//
// Written because "it feels a bit slow, just double it" turned out to be the wrong instruction to take
// literally, and the reason is visible here rather than arguable:
//
//   * the model's travel is  travel = start + (cap - start) * u,  u = budget / rampUp
//   * multiplying the OUTPUT by 2 scales that whole line, so the ceiling moves too: a fast roll then
//     delivers 2 x cap x topSpeed = 3 x the wheel's OWN notch, well past anything the model was tuned
//     for, while a slow roll is still tiny in absolute terms. The felt difference therefore lands almost
//     entirely at the fast end -- which is what "not linear" means.
//   * lowering `rampUp` instead keeps the line and its ceiling where they are and reaches them SOONER,
//     which is the model's own parameter for exactly this.
//
// It prints the per-notch travel (in deltas, where the wheel's own notch is 120) at several turning
// speeds, for the plugin's shipped settings and for candidates, so the app's defaults can be CHOSEN
// from numbers instead of guessed.
//
// g++ -std=c++17 -O2 -I. -Isrc _diag/app_feel_probe.cpp -o /tmp/afp && /tmp/afp
#include <cstdio>
#include <cmath>
#include "core.h"

// A steady roll: feed notches at a fixed rate and report the travel per notch once the budget has
// settled. The budget's own time constant is 300 ms (model::SpeedBudget), and it settles at
// rate * tau, so a second of rolling is plenty.
static double TravelPerNotch(const app::Config &cfg, double notchesPerSec)
{
  app::Core core;
  const double gapMs = 1000.0 / notchesPerSec;
  double last = 0.0;
  for (int i = 0; i < 40; ++i)
    last = core.Feed(120, (i == 0) ? 0.0 : gapMs, cfg);
  return std::fabs(last);
}

static void Row(const char *label, const app::Config &c)
{
  printf("  %-26s", label);
  const double speeds[] = {1.0, 2.0, 4.0, 8.0, 16.0};
  for (int i = 0; i < 5; ++i)
    printf(" %6.1f", TravelPerNotch(c, speeds[i]));
  printf("   |");
  for (int i = 0; i < 5; ++i)
  {
    const double t = TravelPerNotch(c, speeds[i]);
    printf(" %5.0f%%", 100.0 * t / 120.0);
  }
  printf("\n");
}

int main()
{
  printf("travel per notch, in deltas (the wheel's own notch is 120)\n");
  printf("columns: turning speed 1, 2, 4, 8, 16 notches/s, then the same as %% of a native notch\n\n");
  printf("  %-26s %6s %6s %6s %6s %6s   %6s %6s %6s %6s %6s\n", "setting", "1/s", "2/s", "4/s", "8/s",
         "16/s", "1/s", "2/s", "4/s", "8/s", "16/s");

  // EVERY ROW SETS gain EXPLICITLY. An earlier version of this probe left it at the config default and
  // so measured every row at the same gain -- both "plugin defaults" and "output x2" came out identical,
  // which is the kind of thing that would have sent tuning in the wrong direction.
  app::Config base; // the plugin's shipped values
  base.gain = 1.0;  // the plugin's own pace, for an honest reference
  Row("plugin defaults", base);

  // The blunt answer: scale the output. Note where the fast end lands.
  app::Config gain2 = base;
  gain2.gain = 2.0;
  Row("output x2 (felt wrong)", gain2);

  // The model's own parameter: a shorter ramp reaches the same ceiling sooner.
  app::Config ramp500 = base;
  ramp500.rampUp = 500.0;
  Row("rampUp 1000 -> 500", ramp500);

  app::Config ramp600 = base;
  ramp600.rampUp = 600.0;
  Row("rampUp 1000 -> 600", ramp600);

  // A higher floor: the slowest end moves more, the ceiling is untouched.
  app::Config slow10 = base;
  slow10.slowStep = 10.0;
  Row("slowStep 5 -> 10", slow10);

  // Both: the floor lifted AND the ramp shortened.
  app::Config both = base;
  both.slowStep = 10.0;
  both.rampUp = 500.0;
  Row("slowStep 10 + rampUp 500", both);

  printf("\n  native (for reference)                                         120 deltas = 100%%\n");
  printf("\nwhat to look at: a column that is roughly 2x the plugin column AND still ends at the\n");
  printf("same ceiling is a doubled FEEL without overshoot; the output-x2 row instead shows the top\n");
  printf("end at 2x the plugin's, which is 3x the wheel's own notch.\n");
  return 0;
}
