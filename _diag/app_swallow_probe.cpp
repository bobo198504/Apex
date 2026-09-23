// THE SWALLOW RULE, swept over EVERY combination -- against the real code.
//
// THE RULE, and why it is the most important one in Apex: an event may be eaten ONLY when a replacement will
// actually be delivered. It has broken once --
//
//     with the runtime switch OFF the hook kept swallowing while the injector stayed silent, so the original
//     notch was eaten and nothing replaced it: scrolling stopped working entirely, and the setting that was
//     supposed to mean "do nothing" was the cause.
//
// -- and a user's reaction to that is not "a setting is wrong", it is "this thing is broken".
//
// ⚠️ THE SWITCH THAT CAUSED IT IS GONE, AND THE SWEEP IS STILL WORTH RUNNING. The host's master switch and the
// tray's runtime toggle were removed at the user's request (a feature owns its own enable), so two of the
// dimensions this used to sweep no longer exist. What remains is not a smaller version of the same test: the
// path that broke is now reachable a DIFFERENT way -- a user disables their only feature, or their only
// feature fails to load -- and that is the case `featuresEnabled = 0` covers. Deleting the test along with
// the switches would have thrown away the check for the one way left to get it wrong.
//
// ⚠️ WHAT CHANGED AT THE SPLIT: this used to test a COPY of the rule, written out again inside the probe
// because the real one was inline in the hook where nothing could reach it. A copy and an original are two
// things that can disagree, and the copy is the one that stays green. The rule is now apex/decision.h -- a
// pure function -- so this includes and runs THE REAL THING.
//
// The sweep is exhaustive rather than sampled: every combination of injected/accept-injected, target known,
// whether the point is over Apex's own panel, handler state, and how many features could deliver.
// 2*2*2*2*3*3 = 144 cases, which is small enough to be complete.
//
// ⚠️ THE COUNT ABOVE IS A CLAIM; THE LAST LINE OF THE RUN IS A MEASUREMENT. They are two different things and
// they have disagreed before: when the blacklist stopped being the host's (it is each feature's now) the sweep
// silently lost a dimension and became 72 cases, and this comment went on saying 144. Nothing failed, because
// the only other place a number appeared was the run's own tally at the bottom. Keeping the claim here and the
// tally there -- rather than writing 144 in both -- is what makes the disagreement visible.
//
// g++ -std=c++17 -O2 -Iapex _diag/app_swallow_probe.cpp -o /tmp/asp && /tmp/asp
#include "decision.h"
#include <cstdio>

using namespace apex;

static int failures = 0;
static int checks = 0;

static void Check(bool ok, const char *what)
{
  ++checks;
  if (!ok)
  {
    printf("  FAIL: %s\n", what);
    ++failures;
  }
}

static const char *HandlerName(int h)
{
  switch (h)
  {
  case APEX_HANDLER_PRESENT: return "present";
  case APEX_HANDLER_ABSENT: return "absent";
  default: return "unknown";
  }
}

int main(void)
{
  printf("the swallow rule, over every combination (the REAL apex/decision.h)\n");
  printf("invariant: swallowing implies delivering\n\n");

  const int handlers[] = {APEX_HANDLER_UNKNOWN, APEX_HANDLER_PRESENT, APEX_HANDLER_ABSENT};

  // ---- THE INVARIANT, EXHAUSTIVELY ----
  int swept = 0, passed = 0, asked = 0;
  for (int inj = 0; inj < 2; ++inj)
    for (int acc = 0; acc < 2; ++acc)
      for (int known = 0; known < 2; ++known)
        for (int feats = 0; feats < 3; ++feats)
          for (int h = 0; h < 3; ++h)
            // ⚠️ `ownUi` IS A SWEPT DIMENSION, not an afterthought. It is a NEW WAY for the decision to say
            // PASS, and a branch that returns PASS can never break this invariant by itself -- which is
            // exactly why it has to be in the sweep rather than assumed harmless: the sweep is what proves
            // that, in every combination, including the ones where it is combined with the others. (It also
            // catches the opposite mistake -- a rule written so that ownUi ACCEPTED the event would show up
            // here as a swallow with nothing behind it.) Adding it takes the sweep from 72 cases to 144 --
            // the same size it was when the blacklist was still a dimension, which is a coincidence worth
            // not mistaking for a reason.
            for (int own = 0; own < 2; ++own)
          {
              DecisionInputs in;
              in.injected = inj != 0;
              in.acceptInjected = acc != 0;
              in.targetKnown = known != 0;
              in.featuresEnabled = feats;
              in.handlerState = handlers[h];
              in.ownUi = own != 0;

              const Decision d = DecideWheel(in);
              ++swept;
              if (d == Decision::kPass)
                ++passed;
              else
                ++asked;

              if (!SwallowingImpliesDelivering(in))
              {
                printf("  FAIL: swallowing without delivering: injected=%d acc=%d known=%d "
                       "feats=%d handler=%s ownUi=%d\n",
                       inj, acc, known, feats, HandlerName(handlers[h]), own);
                ++failures;
              }
              // A DECISION IS NEVER ITSELF A SWALLOW. DecideWheel may only say "pass" or "you may ask";
              // a feature claiming the event is what turns the second into a swallow. So no case here can
              // produce anything else, and that is the structural reason the invariant holds rather than a
              // coincidence of the checks below.
              Check(d == Decision::kPass || d == Decision::kAskFeatures,
                    "a decision is never itself a swallow");
            }
  printf("  swept %d cases: %d pass, %d may-ask, 0 swallow-decisions\n\n", swept, passed, asked);

  // ---- THE SPECIFIC CASES, stated one by one so a regression names itself ----
  printf("the cases that must pass, whatever else is true\n");
  {
    // A baseline that IS a candidate, so "must pass" below is a real claim rather than an accident of
    // everything already being off.
    DecisionInputs ok;
    ok.targetKnown = true;
    ok.handlerState = APEX_HANDLER_ABSENT;
    ok.featuresEnabled = 1;
    Check(DecideWheel(ok) == Decision::kAskFeatures,
          "the baseline is a candidate (so the checks below mean something)");

    // 1. nothing to deliver with -- THE WAY THE OLD BUG IS STILL REACHABLE, now that no global switch
    //    exists to cause it. A user disables their only feature; the wheel must pass untouched.
    DecisionInputs empty = ok;
    empty.featuresEnabled = 0;
    Check(DecideWheel(empty) == Decision::kPass,
          "no feature could deliver -> PASS (the way this still goes wrong, and the bug at the top of "
          "this file)");

    // 2. another handler: PRESENT and UNKNOWN behave the same, deliberately
    //
    // ⚠️ THE BLACKLIST CASE THAT USED TO SIT HERE HAS MOVED, not been dropped. The list is each FEATURE's now
    // (see ApexFeature::listOp), so `DecideWheel` has no blacklist flag to consult -- the host never sees the
    // list at all. The guarantee a user relies on is unchanged and is asserted where it is now decided: a
    // listed program's wheel passes through untouched because the feature DECLINES it, and a declined wheel is
    // never swallowed. What this probe can still prove is the half that lives in the rule: the decision never
    // eats anything by itself (checked exhaustively above).
    DecisionInputs pres = ok;
    pres.handlerState = APEX_HANDLER_PRESENT;
    Check(DecideWheel(pres) == Decision::kPass, "another handler owns it -> PASS");
    DecisionInputs unk = ok;
    unk.handlerState = APEX_HANDLER_UNKNOWN;
    Check(DecideWheel(unk) == Decision::kPass,
          "an UNREADABLE process counts as owned -> PASS (two handlers are worse than an unsmoothed wheel)");
    DecisionInputs absent = ok;
    absent.handlerState = APEX_HANDLER_ABSENT;
    Check(DecideWheel(absent) == Decision::kAskFeatures, "an absent handler leaves it to the features");

    // 3. OUR OWN SETTINGS PANEL. A wheel over it must pass, and for a reason unlike any other PASS here:
    //    every other one means "somebody else owns this", while this one means "the wheel has a job to do
    //    that smoothing would destroy". Each slider in the panel takes a wheel as one step of its value, and
    //    a smoothed wheel arrives as dozens of small messages per notch -- which the slider would count as
    //    dozens of steps. It is asserted even with a feature available and no other handler, because those
    //    are the conditions it has to win under.
    DecisionInputs ui = ok;
    ui.ownUi = true;
    Check(DecideWheel(ui) == Decision::kPass,
          "a wheel over OUR OWN PANEL passes, with a feature ready and no other handler");
    //    ... and it is genuinely this rule doing it, not an accident: the same inputs without the flag are a
    //    candidate. Without this line the check above would pass for a decision that passed everything.
    DecisionInputs notUi = ok;
    Check(DecideWheel(notUi) == Decision::kAskFeatures,
          "  and the same target is a candidate when the flag is off (so the check above is not vacuous)");
    //    ... and it also wins over a target the host could NOT resolve, which is the state during the first
    //    frames of a gesture: `targetKnown` false with ownUi true cannot happen through the real path (the
    //    flag is resolved together with the target), so this is asserted to pin the ORDER -- ownUi is
    //    consulted on its own and does not depend on the target's other fields being filled in.
    DecisionInputs uiUnresolved = ok;
    uiUnresolved.ownUi = true;
    uiUnresolved.targetKnown = false;
    uiUnresolved.handlerState = APEX_HANDLER_UNKNOWN;
    Check(DecideWheel(uiUnresolved) == Decision::kPass,
          "  and it passes even when the rest of the target is unknown");

    // 4. our own output, decided before anything else is consulted
    DecisionInputs own = ok;
    own.injected = true;
    own.acceptInjected = false;
    Check(DecideWheel(own) == Decision::kPass, "our own injected wheel passes (no self-loop)");
    //    ... and it still does when the point happens to be over our own panel, so the two rules cannot be
    //    read as one depending on the other.
    DecisionInputs ownUiBoth = own;
    ownUiBoth.ownUi = true;
    ownUiBoth.acceptInjected = true; // the hostile case: injected AND accepted AND over our UI
    Check(DecideWheel(ownUiBoth) == Decision::kPass,
          "  and an injected wheel over our own panel passes too (both rules hold together)");
    DecisionInputs testMode = ok;
    testMode.injected = true;
    testMode.acceptInjected = true;
    Check(DecideWheel(testMode) == Decision::kAskFeatures,
          "and the TEST-ONLY mode lets injected wheels reach the features, so a load test can drive the "
          "real path");
  }

  // ---- the switch that no longer exists ----
  //
  // Kept as a NOTE rather than deleted, because the invariant it was protecting is the reason this file
  // exists and the next reader will want to know where it went. It is not a test: there is nothing left to
  // configure.
  printf("\nthe removed switches\n");
  {
    // What the old rule did, expressed with what is left: "off" from the user now arrives as feats=0,
    // because the only way to switch Apex off is to switch its features off.
    DecisionInputs userOff;
    userOff.targetKnown = true;
    userOff.handlerState = APEX_HANDLER_ABSENT;
    userOff.featuresEnabled = 0;
    Check(DecideWheel(userOff) == Decision::kPass,
          "the user turning every feature off is PASS, which is what the old switches guaranteed");
  }

  // ---- an unknown target ----
  //
  // TWO DIFFERENT "UNKNOWNS", and they are not the same thing:
  //
  //   * the process could not be read at all (targetKnown = false). Nothing can be classified, so the rule
  //     falls through and lets the features be asked -- and a feature that cannot identify the target
  //     declines (SmoothWheel checks targetAt before it takes anything), leaving the wheel alone.
  //   * the process WAS read but its handler state is unknown (targetKnown = true, handler UNKNOWN). That is
  //     rule 2's case, and it PASSES: an unreadable handler is assumed to own the wheel rather than assumed
  //     absent, because two handlers driving one view is worse than one unsmoothed wheel.
  //
  // ⚠️ THE BLACKLIST IS NOT CHECKED HERE ANY MORE -- it is each feature's own list now (see
  // ApexFeature::listOp), so the host has no blacklist flag to consult. The guarantee is unchanged and is
  // asserted where it is now decided: the feature declines, and a declined wheel is never swallowed.
  printf("\nan unknown target\n");
  {
    DecisionInputs noInfo;
    noInfo.targetKnown = false;
    noInfo.handlerState = APEX_HANDLER_UNKNOWN;
    noInfo.featuresEnabled = 1;
    Check(DecideWheel(noInfo) == Decision::kAskFeatures,
          "with nothing read at all, the features are asked (and decline)");

    DecisionInputs readButUnknown = noInfo;
    readButUnknown.targetKnown = true;
    Check(DecideWheel(readButUnknown) == Decision::kPass,
          "a process whose handler cannot be determined is treated as owned -> PASS");
  }

  printf("\n");
  if (failures == 0)
    printf("OK: %d cases swept, %d checks, the invariant holds in every one\n", swept, checks);
  else
    printf("FAILED: %d of %d checks\n", failures, checks);
  return failures == 0 ? 0 : 1;
}
