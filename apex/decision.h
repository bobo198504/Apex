#ifndef APEX_DECISION_H
#define APEX_DECISION_H

// ⚠️ A FEATURE MAY NOT INCLUDE THIS FILE -- the guard below is what makes that a CONTRACT rather than a
// convention. A feature is compiled with the same -I paths as the host (see apex/build.sh) and exports a C
// ABI, so nothing else stops it reaching in here and calling host internals the ABI never promised to keep
// stable. Everything a feature is entitled to is in abi.h; this file is the host's wheel-swallowing rule.
#ifndef APEX_BUILDING_HOST
#error "decision.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif

// ---------------------------------------------------------------------------
// THE DECISION: may the host take this wheel?
//
// THIS IS THE ONE RULE IN APEX WHOSE FAILURE MODE IS "SCROLLING STOPS WORKING", so it is worth being a named
// function in its own file rather than a sequence of early returns inside a window procedure. It has broken
// exactly once, and this is the shape of that failure:
//
//   With the runtime switch OFF the hook kept swallowing while the injector stayed silent -- the original
//   notch was eaten and nothing replaced it. Scrolling stopped, entirely, and the setting that was supposed
//   to mean "do nothing" was the cause.
//
// Hence the invariant, which is the whole point of this file:
//
//     SWALLOWING IMPLIES DELIVERING.
//
// If the original message is eaten, something must be handed to the receiver in its place. Everything below
// exists to make that true in every combination of the switches.
//
// ⚠️ THE SWITCH THAT CAUSED THAT BUG NO LONGER EXISTS, AND THE INVARIANT STILL HOLDS. The host's master
// switch and the tray's runtime toggle were removed at the user's request: an enable belongs to a feature,
// not to the program. So the "the user turned it off and we ate the wheel anyway" path is gone by
// construction -- but that is not what makes the invariant true. What makes it true is the rule at the
// bottom: with no feature able to deliver, the decision is PASS. A user who disables their only feature is
// the same case as a user whose only feature failed to load, and both are covered.
//
// WHY IT IS PURE AND SEPARATE. It used to live inline in the hook (and a copy of it lived in a probe, which
// is how the copy and the original were able to disagree). As a free function over a small struct it can be
// checked exhaustively -- every combination of the injected flag, the blacklist, the other-handler verdict,
// target presence and the number of features that could deliver -- by a test that runs the REAL code rather
// than a description of it. That is the difference between a gate and a comment.
//
// NOTHING HERE TOUCHES THE OS OR THE FEATURES: it answers one question about one event, from state that is
// handed in. The caller keeps the caches, the locks and the calls into features.
// ---------------------------------------------------------------------------

#include "abi.h"

namespace apex {

// What the host knows about this wheel, boiled down to the facts the decision needs.
struct DecisionInputs
{
  // ---- the event ----
  bool injected = false;     // synthesised by some program (see the host's note on LLVHF_INJECTED)
  bool acceptInjected = false; // the TEST-ONLY mode that also handles injected wheels

  // ---- the target ----
  bool targetKnown = false;   // the host has a resolved answer for what is under the cursor
  int handlerState = APEX_HANDLER_UNKNOWN; // who else is handling wheels there

  // THE CURSOR IS OVER APEX'S OWN SETTINGS PANEL. See the rule below for why this is not simply "another
  // handler's wheel" -- it is ours, and it is the one target where the wheel has a second job to do.
  bool ownUi = false;

  // ---- the features ----
  int featuresEnabled = 0;    // features that are loaded, on, and not disabled by the user
};

enum class Decision
{
  kPass,       // let the event through untouched
  kAskFeatures // the event is a candidate: ask them, and swallow only if one takes it
};

// ---------------------------------------------------------------------------
// THE RULE, in the order it must be applied.
//
// THE ORDER MATTERS AND IS NOT ARBITRARY:
//
//   1. our own output first, before anything else can look at the event. Not because it would be decided
//      wrongly below -- it would be PASSED, which is right -- but because it is the one case that must not
//      depend on the state of anything else. Our injected wheels come back through the hook, and a
//      self-loop that depends on a switch is a self-loop waiting for that switch to be flipped.
//   2. our own settings panel. See the note below -- it is the one target that must be passed for a reason
//      other than "somebody else owns it".
//   3. another handler's wheel. PRESENT and UNKNOWN are treated the SAME: an unreadable process is assumed
//      to have a handler, because guessing the other way leaves two handlers moving one view, and that is
//      far worse than leaving one wheel unsmoothed.
//   4. is there anyone to ask? With no enabled feature there is nothing that could replace the event, so
//      swallowing would break the invariant above.
//
// TWO RULES HAVE MOVED OUT OF THIS FILE, and neither left a gap:
//
//   * the master switch and the tray's runtime toggle -- removed at the user's request (an enable belongs
//     to a feature, not to the program). A user turning Apex off now does it by turning their features
//     off, which is rule 3.
//   * THE BLACKLIST. It used to be here, checked before the features so a listed program "cost nothing and
//     could not be reached by a feature's own rules". It is now each FEATURE's own list -- "do not smooth
//     in this program" is a statement about smoothing, not about Apex, and a feature that has nothing to do
//     with wheels has no reason to inherit one. The guarantee that mattered is unchanged: a listed program
//     gets its wheel untouched, because a feature that declines leaves the message alone (this function can
//     only ever say "pass" or "you may ask").
// ---------------------------------------------------------------------------
inline Decision DecideWheel(const DecisionInputs &in)
{
  // 1. our own output
  if (in.injected && !in.acceptInjected)
    return Decision::kPass;

  // 2. OUR OWN SETTINGS PANEL. A wheel there is not a scroll to be smoothed -- it is the user's hand on a
  //    CONTROL. Every slider in the panel takes the wheel as "raise or lower me" (the panel's own handler),
  //    and that only works if the wheel arrives as the OS sent it: one message per notch, deltaY carrying
  //    the notch's size and direction.
  //
  //    ⚠️ SMOOTHING OVER OUR OWN UI WOULD BREAK THAT, AND NOT SUBTLY. A smoothed wheel is the host's
  //    re-injected stream -- dozens of small messages per notch -- so one notch of the user's hand would
  //    arrive as dozens of separate wheel events, each of which the slider would count as a notch of its
  //    own. The value would run away. Passing is also simply right: the user is driving a control, not a
  //    document, and there is nothing here that wants glide.
  //
  //    WHY IT IS A RULE AND NOT A BLACKLIST ENTRY: the panel is not a program the user added to a list, it
  //    is part of Apex, and it is identified as such -- the top-level window under the cursor is the panel's
  //    own window (see TargetUnderCursor). A list entry would be a second answer to the same question, and
  //    one the user could delete by accident.
  if (in.ownUi)
    return Decision::kPass;

  // 3. another handler owns this. Unknown counts as owned.
  if (in.targetKnown &&
      (in.handlerState == APEX_HANDLER_PRESENT || in.handlerState == APEX_HANDLER_UNKNOWN))
    return Decision::kPass;

  // 4. nobody to ask = nobody to deliver = do not swallow. THIS is the check that would have prevented the
  //    bug at the top of this file: with every feature off, the event passes instead of disappearing.
  if (in.featuresEnabled <= 0)
    return Decision::kPass;

  // The event is a CANDIDATE. It is not swallowed here: a feature still has to claim it (see the caller),
  // and a feature that declines leaves the message untouched. That is the other half of the invariant --
  // this function can only ever return "pass" or "you may ask", never "eat it".
  return Decision::kAskFeatures;
}

// The invariant, as a function, so a test can assert it rather than restate it.
//
// ⚠️ IT IS THE DECISION THAT IS CHECKED, NOT THE INPUTS RESTATED. The first version of this re-derived the
// rule from the struct ("features > 0 and not listed and handler absent and both switches on...") and the
// exhaustive sweep promptly failed 30 cases -- because that restatement was WRONG in a way the real rule was
// not: it forgot that an UNKNOWN TARGET short-circuits the blacklist and the handler check entirely, since
// neither can be consulted without a target. A predicate that re-implements the thing it is checking is just
// a second copy of the bug, so this asks the decision instead.
//
// WHAT IT SAYS: if the decision is kAskFeatures, at least one feature must be able to deliver. That is the
// whole content of "swallowing implies delivering" -- a decision to ask only becomes a swallow when a
// feature claims the event, and a feature can only claim it if one is available. kPass swallows nothing, so
// it is trivially satisfied.
inline bool SwallowingImpliesDelivering(const DecisionInputs &in)
{
  if (DecideWheel(in) == Decision::kPass)
    return true; // nothing is eaten, so there is nothing to replace
  return in.featuresEnabled > 0;
}

} // namespace apex

#endif // APEX_DECISION_H
