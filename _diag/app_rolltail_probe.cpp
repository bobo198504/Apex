// THE TAIL OF A FAST ROLL -- the case that still stops abruptly.
//
// The earlier fix eased a LONE notch (nothing overlapping it). A fast roll overlaps, so it kept the
// model's own rule, and the model's rule hands a window at the end of a roll a rate of `1 - ease`
// (40% at its kEaseAmount, 100% at 0). When several such windows decay together the SUM still stops
// dead, which is the reported "a fast roll stops suddenly".
//
// So the question is what the roll's TAIL looks like, in the numbers the receiver gets, under:
//   (a) the model's rule per window   -- what the app does now
//   (b) every window fully eased      -- the obvious alternative
//
// and what (b) costs: the model warns that easing a TILING roll (gap == window) takes its ripple from
// 0.1% to 11%, because tiling windows are already flat.
//
// g++ -std=c++17 -O2 -I. -Isrc _diag/app_rolltail_probe.cpp -o /tmp/art && /tmp/art
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "core.h"

// A roll of `notches` at `gapMs`, then nothing: returns the per-frame output.
static std::vector<double> RollThenStop(const app::Config &cfg, int notches, double gapMs, double easeOverride)
{
  model::Params P;
  P.windowMs = cfg.glideMs;
  model::Axis gl;
  gl.Reset();
  model::SpeedBudget bud;
  bud.Reset();

  std::vector<double> out;
  const double dt = app::kFrameMs * 0.001;
  const int feedFrames = (int)(notches * gapMs / app::kFrameMs);
  const int drainFrames = 400;
  int fed = 0;
  for (int f = 0; f < feedFrames + drainFrames; ++f)
  {
    const double tMs = f * app::kFrameMs;
    while (fed < notches && fed * gapMs <= tMs + 1e-9)
    {
      const double g = (fed == 0) ? 0.0 : gapMs;
      const double budget = bud.Add(120.0, g);
      const double travel = model::Travel(120.0, budget, cfg.rampUp, cfg.slowStep, cfg.topSpeed);
      // The ease for this window: the model's rule, or the override when one is given.
      P.payoutEase = (easeOverride >= 0.0) ? easeOverride : app::AppEaseFor(g, cfg.glideMs);
      gl.Feed(travel, P);
      ++fed;
    }
    out.push_back(gl.Tick(dt, P));
  }
  return out;
}

static void Tail(const char *label, const std::vector<double> &v, int showFrom)
{
  printf("\n%s\n", label);
  double peak = 0.0;
  for (double x : v)
    if (x > peak)
      peak = x;
  if (peak <= 0.0)
  {
    printf("  (nothing)\n");
    return;
  }

  // The last frames that carry anything: the tail.
  int last = -1, first = -1;
  for (int i = 0; i < (int)v.size(); ++i)
    if (v[i] > 1e-6)
    {
      if (first < 0)
        first = i;
      last = i;
    }

  printf("  peak %.2f/frame at frame %d; motion ends at frame %d (%d frames of motion)\n", peak,
         (int)(std::max_element(v.begin(), v.end()) - v.begin()), last, last - first + 1);
  printf("  the last 12 frames that carry anything:");
  for (int i = last - 11; i <= last; ++i)
    if (i >= 0)
      printf(" %.3f", v[i]);
  printf("\n  each as %% of peak:");
  for (int i = last - 11; i <= last; ++i)
    if (i >= 0)
      printf(" %4.1f%%", 100.0 * v[i] / peak);
  printf("\n  final frame = %.1f%% of peak  <- the abruptness\n", 100.0 * v[last] / peak);
  (void)showFrom;
}

int main()
{
  app::Config cfg; // the app's defaults

  printf("the tail of a FAST roll that then stops (windows overlap)\n");
  printf("roll: 20 notches at 25 ms apart = 40 notches/s, then nothing. window %.0f ms\n", cfg.glideMs);

  Tail("(a) the model's rule per window  <- what the app does now",
       RollThenStop(cfg, 20, 25.0, -1.0), 0);
  Tail("(b) every window fully eased (ease 1.0)", RollThenStop(cfg, 20, 25.0, 1.0), 0);

  printf("\nand a SLOW roll for contrast (windows do NOT overlap)\n");
  Tail("(a) the model's rule per window", RollThenStop(cfg, 4, 400.0, -1.0), 0);
  Tail("(b) every window fully eased", RollThenStop(cfg, 4, 400.0, 1.0), 0);

  printf("\nthe ripple cost of easing, by gap (this decides how far the rule may go)\n");
  {
    struct Row { double gap; const char *desc; };
    const Row rows[] = {
        {400.0, "no overlap"},          {200.0, "TILING (ratio 1.0)"},
        {150.0, "ratio 1.33"},          {100.0, "ratio 2.0 (integer)"},
        {60.0, "ratio 3.33"},           {50.0, "ratio 4.0 (integer)"},
        {30.0, "ratio 6.67"},           {25.0, "ratio 8.0 (integer)"},
    };
    printf("   %-22s %10s %10s %10s\n", "gap", "ease 0", "model 0.5", "ease 1.0");
    for (const Row &row : rows)
    {
      double r[3] = {0.0, 0.0, 0.0};
      const double es[3] = {0.0, 0.5, 1.0};
      for (int i = 0; i < 3; ++i)
      {
        const std::vector<double> v = RollThenStop(cfg, 40, row.gap, es[i]);
        const int a = (int)(v.size() / 3), b = (int)(v.size() * 2 / 3); // the settled middle
        double mean = 0.0;
        for (int k = a; k < b; ++k)
          mean += v[k];
        mean /= (b - a);
        double var = 0.0;
        for (int k = a; k < b; ++k)
          var += (v[k] - mean) * (v[k] - mean);
        var /= (b - a);
        r[i] = (mean > 0.0) ? 100.0 * std::sqrt(var) / mean : 0.0;
      }
      printf("   %-22s %9.1f%% %9.1f%% %9.1f%%\n", row.desc, r[0], r[1], r[2]);
    }
  }
  return 0;
}
