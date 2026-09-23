#ifndef APEX_RELEASE_H
#define APEX_RELEASE_H

// ---------------------------------------------------------------------------
// THE RELEASE MODEL -- the small model that sits between the main model and the delivery layer.
//
// WHAT IT DECIDES: how long the window opened by one wheel message lasts. Nothing else. It does not
// touch how far a notch goes (that is the main model's speed budget) and it does not touch what the
// receiver gets (that is the delivery layer). It answers one question -- "how long should this
// message's motion take" -- and hands the answer to the main model as `Params.windowMs`.
//
// WHY IT IS A SEPARATE MODEL AND NOT TWO LINES IN THE CORE. The window does TWO jobs at once, and they
// want opposite things:
//
//   * it is the RAMP -- how long one notch takes to build up to its speed. A LONE notch wants this
//     SHORT: nothing is overlapping it, so its whole motion IS the ramp, and a long ramp reads as lag
//     (the wheel moved, and the screen keeps drifting afterwards).
//
//   * it is the TAIL -- how long the motion keeps running after a message opens. When a roll ENDS,
//     every window in flight finishes within one window-length of the others, so the window length IS
//     the length of the ending. A ROLL wants this LONG: measured, the last notch of a 25 ms roll is
//     followed by 170 ms of motion at a 200 ms window, and the user's note on it is
//     "让滚动的时候，收尾拉长的" (while rolling, the ending should be drawn out).
//
// ONE NUMBER CANNOT BE BOTH SHORT AND LONG, which is the whole reason this file exists. And a roll is
// exactly the case that can afford the length: its messages already overlap, so the OVERLAP supplies
// the ramp and the window is free to be long.
//
// THE RULE:
//
//   * a LONE message (its window would not overlap the one before it) -> the Glide setting, unchanged.
//     This is the user's own slider and it goes on meaning exactly what it meant.
//   * a message IN A ROLL (its window would overlap its predecessor) -> the Glide setting PLUS
//     `kReleaseMs`. The ending is drawn out by that fixed amount.
//
// ⚠️ REL IS A CONSTANT, AND THAT IS THE POINT -- "200ms，并且是固定值". The version of this idea that
// was removed scaled the extra length by how fast the wheel was going
// (`windowMs = glide * (1 + tail * u)`, with `tail` a user-facing slider). Scaling makes the tail a
// speed readout as well as a length, and a slider for it means re-tuning it for every Glide value. A
// FIXED addition does one thing only: while the wheel is being turned, the ending is `kReleaseMs`
// longer than the same scroll made slowly. There is nothing to tune and nothing to fall out of step
// with Glide.
//
// WHY THE TEST IS "WOULD IT OVERLAP" AND NOT A SPEED: overlap is what makes a roll a roll in this
// model -- it is the same condition the main model uses to choose its payout shape
// (anim3::PayoutEaseFor, whose whole argument is `gapMs` against `windowMs`). So "rolling" means here
// exactly what it means one layer down, and the two cannot disagree about which case a message is in.
//
// ⚠️ THE STEP AT THE BOUNDARY IS DELIBERATE. At a gap just under the base window the ending is
// `kReleaseMs` longer than at a gap just over it. Fading the extra length in with speed is precisely
// the scaled version that was removed, so the edge is what "固定值" buys. It sits at the gap where
// overlap begins, which is where the main model changes its own behaviour anyway.
//
// NO INCLUDES, NO WINDOWS: plain numbers in, a plain number out, so it can be exercised on its own
// (_diag/app_tail_probe.cpp pins it).
// ---------------------------------------------------------------------------

namespace app {

// How much longer a message's window is while the wheel is being turned. FIXED: not a setting, not in
// the settings file, not on the panel, and not scaled by anything.
static const double kReleaseMs = 200.0;

// Is this message part of a roll -- i.e. will its window still be running when the next one opens?
// `gapMs` is the time since the previous message on this target, or <= 0 for the first of a gesture.
inline bool ReleaseIsRoll(double gapMs, double baseWindowMs)
{
  return gapMs > 0.0 && baseWindowMs > 0.0 && gapMs < baseWindowMs;
}

// The window a message gets WHILE ROLLING. The chart draws a roll (its shape has a knee and a climb,
// which only exist when windows overlap), so this is the window that picture stands for.
inline double ReleaseRollWindowMs(double baseWindowMs)
{
  return (baseWindowMs > 0.0) ? (baseWindowMs + kReleaseMs) : baseWindowMs;
}

// THE WINDOW this message should open. `baseWindowMs` is the Glide setting (already clamped to the
// model's envelope by Config::WindowMsFor).
inline double ReleaseWindowMs(double baseWindowMs, double gapMs)
{
  return ReleaseIsRoll(gapMs, baseWindowMs) ? ReleaseRollWindowMs(baseWindowMs) : baseWindowMs;
}

} // namespace app

#endif // APEX_RELEASE_H
