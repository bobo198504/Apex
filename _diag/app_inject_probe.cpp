// WHEEL-ONLY: this injects MOUSEEVENTF_WHEEL and nothing else, and it does not touch the cursor.
// (See test/check_apex_modular.sh section 9 for what this marker means and why it is required.)
// WHAT ACTUALLY GETS INJECTED -- not what the model computes.
//
// This distinction is the whole point. The model's output is a smooth curve, and the earlier probes
// show it: a slow notch hands over values from 0.06 up to 1.2 deltas per frame, decaying to 0.06 again.
// But SendInput carries a WHOLE wheel delta, so the app accumulates and sends only when the carry
// crosses 1. The receiver therefore never sees that curve -- it sees a STAIRCASE of 1-delta steps.
//
// That staircase is what decides the feel, and it has a hard floor: one step is 1 delta = 1/120 of a
// notch, and a slow notch only travels ~19 deltas, so the whole motion is at most ~19 steps no matter
// how the curve is shaped. This probe prints the steps and the gaps between them, so the limit can be
// seen rather than argued about.
//
// g++ -std=c++17 -O2 -I. -Isrc _diag/app_inject_probe.cpp -o /tmp/aip && /tmp/aip
#include <cstdio>
#include <cmath>
#include <vector>
#include "core.h"

struct Inject
{
  std::vector<int> step;   // the whole deltas actually sent
  std::vector<int> atFrame; // the frame each was sent on
  double total = 0.0;      // what was sent in total
  double modelTotal = 0.0; // what the model wanted to hand over in total
};

// One notch, drained, with the app's own carry logic (whole deltas out, remainder carried).
static Inject OneNotch(const app::Config &cfg)
{
  app::Core core;
  Inject r;
  const double dt = app::kFrameMs * 0.001;
  const double gapMs = 0.0; // a lone notch

  core.Feed(120, gapMs, cfg);
  double carry = 0.0;
  for (int f = 0; f < 400; ++f)
  {
    const double out = core.Tick(dt, cfg);
    r.modelTotal += out;
    carry += out;
    const double whole = (carry < 0.0) ? -floor(-carry) : floor(carry);
    if (whole != 0.0)
    {
      carry -= whole;
      r.step.push_back((int)whole);
      r.atFrame.push_back(f);
      r.total += whole;
    }
    if (!core.Active() && out == 0.0 && f > 20)
      break;
  }
  return r;
}

static void Show(const char *label, const Inject &r)
{
  printf("\n%s\n", label);
  printf("  steps=%d  sent=%.0f deltas  model wanted %.2f  (unsent remainder %.2f, carried on)\n",
         (int)r.step.size(), r.total, r.modelTotal, r.modelTotal - r.total);
  if (r.step.empty())
    return;

  printf("  step sizes :");
  for (size_t i = 0; i < r.step.size() && i < 24; ++i)
    printf(" %d", r.step[i]);
  printf("\n  at frames  :");
  for (size_t i = 0; i < r.atFrame.size() && i < 24; ++i)
    printf(" %d", r.atFrame[i]);

  // THE GAPS are the shape the receiver feels: one step every N frames, and N must grow smoothly for
  // the motion to read as slowing down.
  printf("\n  gaps (frames between steps):");
  for (size_t i = 1; i < r.atFrame.size() && i < 24; ++i)
    printf(" %d", r.atFrame[i] - r.atFrame[i - 1]);

  // WHAT THE RECEIVER ACTUALLY RECEIVES, in time: a step rate.
  if (r.atFrame.size() >= 2)
  {
    const int first = r.atFrame.front(), last = r.atFrame.back();
    const double spanMs = (last - first) * app::kFrameMs;
    printf("\n  spread over %.0f ms -> a step every %.1f ms on average", spanMs,
            (spanMs > 0.0) ? spanMs / (double)(r.atFrame.size() - 1) : 0.0);
    printf("\n  last step at frame %d, previous at %d -> the final gap is %d frames", last,
            r.atFrame[r.atFrame.size() - 2], last - r.atFrame[r.atFrame.size() - 2]);
  }
}

int main()
{
  printf("WHAT IS INJECTED (whole deltas; SendInput cannot carry a fraction)\n");
  printf("a slow notch, %0.f ms base window, %.0f Hz frames\n", app::Config().glideMs,
         1000.0 / app::kFrameMs);

  app::Config cfg; // the app's defaults: tail 1.0

  app::Config flat = cfg;
  flat.tailGain = 0.0;
  Show("tail 0 (the plugin's fixed window)", OneNotch(flat));

  Show("tail 1 (the app's default)", OneNotch(cfg));

  app::Config much = cfg;
  much.tailGain = 3.0;
  Show("tail 3 (the longest useful)", OneNotch(much));

  printf("\nTHE FLOOR THIS REVEALS: one step is 1 delta, and a slow notch travels only ~19 of them,\n");
  printf("so the whole motion is ~19 steps however the curve is shaped. A longer window spreads the\n");
  printf("SAME ~19 steps further apart, which makes each gap bigger -- smoother to the eye only if the\n");
  printf("receiver interpolates the gaps itself.\n");
  return 0;
}
