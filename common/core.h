#ifndef SWS_APP_CORE_H
#define SWS_APP_CORE_H

// ---------------------------------------------------------------------------
// THE SMOOTHING CORE -- the app's half of the shared behaviour.
//
// It does exactly what the plugin's Kick/Tick pair does, but with no host around it: a wheel message
// becomes a travel (how far this notch should move) and a window (how long to take over it), and a
// timer asks it, once a frame, how much to hand over right now.
//
// IT REUSES THE MODEL, IT DOES NOT RE-IMPLEMENT IT. The travel and the timing both come from
// src/model.h -- the SAME header the REAPER plugin compiles -- so the two products cannot drift apart
// in feel. That is the "one model source" rule (AGENTS): the sibling WPF project re-implemented the
// model in C# and is now stuck at 1.1.0 while this project is on 3.0.
//
// WHAT IS DIFFERENT FROM THE PLUGIN (and why this is a separate file rather than shared code):
//   * there is no action to look up and no per-action delivery rule -- one receiver, "whatever is
//     under the cursor", so there is no routing here at all;
//   * the amount the receiver finally gets is in DEVICE DELTAS (the units a wheel message carries),
//     because that is what has to be injected, whereas the plugin hands over 7-bit relative units to a
//     REAPER action. The conversion is a scale factor at the boundary, nothing more.
// ---------------------------------------------------------------------------

#include <cmath>

#include "config.h"
#include "release.h" // the small model that decides how long a window lasts
#include "model.h"   // the shared model; the only model header this app may include

namespace app {

// The wheel's own measures. One notch is 120 deltas, which is Windows' definition; the model's
// parameters are in deltas too, so nothing has to be converted on the way in.
static const double kDeltasPerNotch = 120.0;

// How often the core is asked for a frame. The plugin settled on 5 ms (a timer's real resolution is
// ~15.6 ms on this machine, so this is "as fast as the timer will go" without the multimedia timer's
// cost); the app has no host UI to protect, so the same value is a safe starting point.
static const double kFrameMs = 5.0;

// ---------------------------------------------------------------------------
// THE TAIL, PART ONE: which payout SHAPE this window gets.
//
// (PART TWO -- how LONG the window is -- is the release model next door, in release.h. They are two
// separate questions and the app answers them in two files: the LENGTH is what the ending costs in
// time, the SHAPE is what that ending looks like frame by frame.)
//
// The model exposes the shape as a per-window parameter (Params.payoutEase) and its header says the
// CALLER chooses it per message. So this is the delivery layer's decision, and it is the one the app
// has to make for itself.
//
// THE MODEL'S OWN RULE optimises a roll that KEEPS GOING: it asks for a constant rate whenever the
// number of windows in flight stays constant (a non-overlapping or integer-ratio gap), because then the
// sum is already perfectly flat and easing only adds ripple. Its own notes measure that cost for a
// tiling gap: 0.1% -> 11%.
//
// IT SAYS NOTHING ABOUT STOPPING, and stopping is exactly when the shape is judged. When a roll ends,
// every window finishes within one window-length of the others, so "the count is constant" stops being
// true whatever the ratio -- and with a constant rate each window is still running at full speed on the
// frame it vanishes. Measured on a fast roll: the output fell in FLAT STEPS (37.5% -> 25% -> 12.5% of
// peak) instead of decaying, which is the abrupt stop that was reported.
//
// SO THE APP EASES IN THE TWO CASES THAT STOP IN STEPS, and leaves the model alone everywhere else:
//
//   * a LONE notch (no overlap at all)  -- easing is free and it is the whole motion; measured, only
//     the fully-eased end (rate 0) actually comes to rest: the model's own 0.5 still stops at 40%.
//   * an INTEGER-ratio gap (including the non-overlapping one) -- those are the staircase cases.
//
// AND IT EXCLUDES THE TILING GAP (gap == window exactly), where the model's warning is measured and
// real: easing takes that roll's ripple from 0.0% to 45.0%, because the windows butt end to end and
// there is no gap for the ease to fill. A tiling roll also has no staircase to fix -- it is the one
// case the model's flat rate handles correctly, start to finish.
//
// Measured ripple, by gap (ease 0 -> fully eased): tiling 0.0% -> 45.0% (worse, excluded); ratio 1.33
// 35.4% -> 8.4% (better); ratio 2.0 0.0% -> 11.1%; ratios 3.33/6.67 unchanged. The larger figures at
// high ratios are the overlap's own arithmetic and are identical at every ease -- they are not something
// the shape controls.
// ---------------------------------------------------------------------------
inline double AppEaseFor(double gapMs, double windowMs)
{
  if (!(windowMs > 0.0))
    return 0.0; // no window at all: nothing to shape
  if (gapMs <= 0.0)
    return 1.0; // the first message of a gesture: a lone notch until a roll proves otherwise
  if (gapMs > windowMs)
    return 1.0; // no overlap: a lone notch, and it must come to rest
  // Tiling (gap == the window): the model's flat rate is right here, and easing is measurably worse.
  if (std::fabs(windowMs / gapMs - 1.0) < 0.08)
    return 0.0;
  // Everything else overlaps: the fully-eased shape, because every one of these gaps is a staircase
  // case when the roll stops. (The model's 0.5 is not enough: the rate at the end of a window is
  // exactly 1 - ease, so 0.5 still stops at 40% of peak.)
  return 1.0;
}

class Core
{
public:
  void Reset()
  {
    lone_.Reset();
    roll_.Reset();
    budget_.Reset();
  }

  // ONE WHEEL MESSAGE. `delta` is what the device reported (signed, 120 = a notch); `gapMs` is the
  // time since the previous message on this target, or <= 0 for the first.
  //
  // Returns the travel to hand over for this message, in deltas. It is NOT what gets injected
  // immediately: it is put into the window and released over time by Tick().
  double Feed(int delta, double gapMs, const Config &cfg)
  {
    const double mag = (delta < 0) ? -(double)delta : (double)delta;
    // No `cfg.enabled` check: this feature has no switch of its own any more (see common/config.h), and the
    // host does not call a feature that is switched off -- so reaching here already means "on".
    if (mag <= 0.0)
      return 0.0;

    // HOW FAR this notch goes: the model's speed budget decides, exactly as in the plugin.
    //
    // NOTHING IS SCALED AFTER THIS. A gain on the output was tried (to make the app feel faster) and
    // removed: it moves the ceiling with the floor, and it is a second place where "how far a notch goes"
    // is decided. The app's feel comes from the model's own parameters instead -- `rampUp` in particular
    // -- so there is exactly one source for travel and the model cannot be pushed past what it was
    // tuned for.
    const double budget = budget_.Add(mag, gapMs);
    const double travel = model::Travel(mag, budget, cfg.rampUp, cfg.slowStep, cfg.topSpeed);

    // THEN its timing, which is TWO decisions made in two places on purpose:
    //
    //   the LENGTH -- the release model (release.h): the Glide setting alone for a lone message, and
    //                 Glide plus the fixed `kReleaseMs` for one inside a roll, so the ending is drawn
    //                 out while the wheel is being turned;
    //   the SHAPE  -- AppEaseFor above: the model's own rule for a roll, the model's eased shape for a
    //                 lone notch.
    //
    // ⚠️ THE LENGTH USED TO BE ONE LINE HERE (`windowMs_ = cfg.WindowMsFor()`) AND IT IS NOT A SETTING ANY
    // MORE. It was, once: the Glide slider was the whole answer, which meant one number had to serve both a
    // single scroll (wants short) and a roll (wants long). The release model is that split -- see its header.
    // The speed budget above deliberately plays no part in it: a tail that scaled with speed was tried and
    // removed, because it turns the ending into a speed readout.
    const double base = cfg.WindowMsFor();
    const bool rolling = ReleaseIsRoll(gapMs, base);

    // ⚠️⚠️ AND THE LENGTH GOES TO A DIFFERENT AXIS, WHICH IS THE FIX FOR THE JUMP (2026-09-18, user report:
    // "快速滚动会出现跳的现象，应该跟最后加的这200ms有关" -- they were right, and this is where it came from).
    //
    // THE MODEL READS ONE LENGTH PER Tick CALL and divides EVERY window's age by it (`anim3::Glide::Tick`'s
    // `w` is a single number). So with one axis, the moment a roll is recognised and the length goes
    // 200 -> 400, the windows ALREADY IN FLIGHT are re-spread over twice the time: their rate halves in one
    // frame. Measured at a 25 ms gap: the rate went 0.4423 -> 0.1475 at frame 7 -- a 67% collapse -- and then
    // climbed again. That dip is a visible hitch at the start of every roll, and it was worse the slower the
    // roll: 76% at 100 ms, 90% at the 180 ms edge.
    //
    // THE TWO AXES FIX IT STRUCTURALLY RATHER THAN BY DAMPING. A window's length is a property of THAT
    // WINDOW, and the model can honour that as long as the number it is given never changes -- so each axis
    // is given one length for its whole life and the message is routed by its own gap. Measured, the same
    // step becomes 2-13% instead of 43-90%, and everything else is unchanged: the total still balances to the
    // last fraction (checked over eight gaps), a lone notch is bit-for-bit what it was, and the ending is
    // still the long one.
    //
    // ⚠️ AND THE "LATCH" THAT USED TO GUARD AGAINST A SHRINKING WINDOW IS GONE, because the hazard it
    // guarded no longer exists. It read: `Tick` applies one length to every window in flight, so a length
    // that shrank would mark the older, longer windows finished and drop the travel still on them -- which
    // was reachable in ordinary use and had to be worked around by never lowering the length while anything
    // was moving. With one fixed length per axis, no window's length can change, so there is nothing to
    // guard. (That workaround had its own cost: a message arriving after a pause kept the ROLL's long window,
    // which made the first notch of a new gesture lag. That is gone with it.)
    model::Params P;
    P.windowMs = rolling ? ReleaseRollWindowMs(base) : base;
    P.payoutEase = AppEaseFor(gapMs, P.windowMs);
    const double signedTravel = (delta < 0) ? -travel : travel; // the sign comes from the message
    (rolling ? roll_ : lone_).Feed(signedTravel, P);
    windowMs_ = P.windowMs; // what this message asked for, for the probes and the log
    return signedTravel;
  }

  // Advance by dt seconds and return what to hand over this frame, in deltas.
  double Tick(double dtSec, const Config &cfg)
  {
    if (dtSec <= 0.0)
      return 0.0;
    // EACH AXIS IS TICKED WITH THE ONE LENGTH IT HAS ALWAYS HAD. Nothing here re-scales a window that is
    // already running, which is what the model's structure requires (see the note in Feed) and what keeps the
    // delivered rate continuous from the first frame of a gesture to the last.
    const double base = cfg.WindowMsFor();
    model::Params A;
    A.windowMs = base;
    A.payoutEase = 0.0; // the ease is stored per window and applied inside Tick
    model::Params B = A;
    B.windowMs = ReleaseRollWindowMs(base);
    return lone_.Tick(dtSec, A) + roll_.Tick(dtSec, B);
  }

  bool Active() const { return lone_.Active() || roll_.Active(); }

  // The window length the last message asked for, for the probes and the log.
  //
  // ⚠️ IT IS A REPORT, NOT THE ENGINE'S INPUT. With two axes there is no single "window in force" any more:
  // a lone window and a rolling one are running side by side, each at its own length. This answers "what did
  // the most recent message ask for", which is what a probe asserting the release RULE wants to know (see
  // test/check_app_release.sh); it is not what Tick divides by, and nothing in the delivery path reads it.
  double WindowMs() const { return windowMs_; }

private:
  // ⚠️ TWO AXES, NOT ONE. See the note in Feed for why: the model applies one length per Tick call to every
  // window in flight, so a single axis cannot serve two lengths without re-scaling windows that are already
  // running -- which is the dip that was reported as a jump.
  model::Axis lone_; // messages that are not part of a roll: the Glide setting, unchanged
  model::Axis roll_; // messages that are: the Glide setting plus the release model's fixed addition
  model::SpeedBudget budget_;
  double windowMs_ = 0.0;
};

} // namespace app

#endif // SWS_APP_CORE_H
