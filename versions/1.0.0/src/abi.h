#ifndef APEX_ABI_H
#define APEX_ABI_H

// ---------------------------------------------------------------------------
// THE FEATURE ABI -- the only thing apex.exe and a feature DLL share.
//
// WHY THIS EXISTS. Apex is a host plus a set of features (PowerToys' shape): the host owns everything
// that must be true for the whole program -- one low-level hook, one clock, one injection path, one
// tray icon, one settings window -- and a feature owns exactly one behaviour and the parameters it is
// tuned with. Adding a feature must therefore be "drop a folder into Plugins/", not "edit the host".
//
// C, NOT C++. A DLL boundary carries no C++ types: no std::string, no exceptions, no vtables this
// header would have to keep in step with a compiler version. Every call here is a plain function with
// plain arguments, and every string is a caller-owned char buffer. That is what makes the boundary
// survivable across a MinGW/MSVC mix, or a feature rebuilt years later.
//
// WHO OWNS WHAT:
//   HOST  -- the hook, the decision cache, the target lookup, injection, the clock, the tray, the
//            settings window, and the GLOBAL rules (which apps are excluded, whether smoothing is on).
//   FEATURE -- one behaviour: does this wheel concern me, how far should it travel, and how much of
//            that travel is due right now. Plus its own numbers and its own file.
//
// THE HOST INJECTS, THE FEATURE DOES NOT. A feature says how many deltas are owed; the host carries the
// fraction and does the SendInput on its own thread (SendInput from inside a message-driven call stalls
// -- measured at 1001 ms -- so there is exactly one place allowed to call it, and it is not in a DLL).
//
// VERSION AND SIZE. A feature declares the ABI it was built against and the size of its own struct; the
// host checks both BEFORE reading further. A stale or foreign DLL in Plugins/ must fail with a line in
// the log, never by reading past the end of a shorter struct.
// ---------------------------------------------------------------------------

// 1 -> 2: `listOp` was added to ApexFeature (a feature can now own a list the user edits).
// 2 -> 3: `activity` was added to ApexHost (a feature can tell the host that something worth showing just
//         happened -- see the note on that field).
//
// The version is bumped rather than a field simply appended, because the host requires an EXACT match -- see
// the loader: a DLL built against an older layout would otherwise be read past its own end. Refusing it with a
// log line is the whole point of carrying a version at all.
#define APEX_ABI_VERSION 3u

#ifdef __cplusplus
extern "C" {
#endif

// ---- what a wheel message looks like on the way in ---------------------------------------------
typedef struct ApexWheelEvent
{
  int delta;      // signed, 120 = one notch
  int x, y;       // screen coordinates
  unsigned key;   // 1 = shift, 2 = ctrl, 4 = alt
  int injected;   // the OS said this was synthesised (see the host: normally ignored)
} ApexWheelEvent;

// How the program under the cursor relates to this project's own handler. `kUnknown` is NOT "absent":
// an unreadable process (REAPER running elevated) means "assume somebody else has it" -- guessing the
// other way leaves two handlers moving one view.
enum
{
  APEX_HANDLER_UNKNOWN = 0, // could not be determined -- treat as taken
  APEX_HANDLER_PRESENT = 1, // a dedicated handler is loaded there; leave it alone
  APEX_HANDLER_ABSENT = 2   // determined to have none; the feature may act
};

typedef struct ApexTarget
{
  unsigned long pid;
  char exe[64];   // bare lower-case file name, no path -- what a blacklist matches on
  int handlerState;
} ApexTarget;

// ---- what the host offers a feature ------------------------------------------------------------
typedef struct ApexHost
{
  unsigned abiVersion;
  unsigned structSize;

  // The program under a screen point, answered from the host's cache. 1 = filled, 0 = nothing known
  // (in which case the caller must NOT act: acting on an unknown target is how a wheel goes missing).
  // Safe to call from the hook: a hit is one WindowFromPoint.
  int (*targetAt)(int x, int y, ApexTarget *out);

  // Hand deltas to whatever is under the cursor. FRACTIONAL IS EXPECTED -- the host accumulates and
  // sends whole deltas only, so a feature must not round to integers itself (that would throw away the
  // slow end of every glide, which is most of the smoothness).
  void (*injectDeltas)(double deltas);

  // One line in the host's log.
  void (*logLine)(const char *text);

  // The folder this feature owns (Plugins/<id>/), with a trailing separator. Its settings file, and
  // anything else it wants to keep, belongs there and nowhere else -- that is what makes the whole
  // program portable, and what keeps two features from writing over each other.
  int (*featureDir)(char *out, int outSize);

  // SOMETHING WORTH SHOWING JUST HAPPENED. A feature calls this when it takes a wheel, and the host passes the
  // count on to the settings panel, which moves its motion curve.
  //
  // WHY THIS EXISTS: the panel is a SEPARATE PROCESS and cannot see the wheel. Without a signal from here its
  // chart can only guess -- and guessing means animating on a timer, which is a picture of nothing: it runs
  // while the user is idle and keeps running while they scroll, so it says the same thing in every state. (The
  // user's words for the timer version: "曲线动画也不对，是有鼠标滚轮事件才有，没有就没有".)
  //
  // CHEAP ENOUGH FOR THE HOT PATH: the host's implementation is one interlocked increment -- no I/O, no
  // allocation, no messages. The hop to the panel is posted later, from the host's own thread, at a fixed low
  // rate. A feature may therefore call this from onWheel.
  void (*activity)(void);

  void *hostUser;
} ApexHost;

// ---- what a feature must provide ---------------------------------------------------------------
//
// EVERY CALL EXCEPT `onWheel` AND `tick` HAPPENS ON THE MAIN THREAD, where taking a lock or allocating
// is fine. Those two are on the hot path (the OS input callback and the frame clock); they must not
// allocate, block, or do I/O.
typedef struct ApexFeature
{
  unsigned abiVersion;
  unsigned structSize;

  const char *id;      // "SmoothWheel" -- also the folder name and the settings file's stem
  const char *nameZh;  // display name, Simplified Chinese
  const char *nameEn;  // display name, English
  const char *version; // the feature's own version, shown in the panel

  // Called once after loading. Return 0 to refuse (the host unloads and logs it). Everything the
  // feature needs -- its settings file, its state -- is set up here.
  int (*init)(const ApexHost *host);

  // Called before the host unloads the feature, and on the way out. Optional.
  void (*shutdown)(void);

  // Re-read the feature's own settings file. Sent when the settings window saves, which is how a
  // parameter change takes effect without a restart. Return 0 on failure (the old values stay).
  int (*reloadSettings)(void);

  // The feature's controls as JSON, for a panel that knows nothing about this feature:
  //
  //   {"params":[{"id":"glide","type":"range","min":100,"max":300,"step":5,"value":200,"def":200,
  //               "labelZh":"滑动时长","labelEn":"Glide","unit":"ms","hue":"#78BEFF"}, ...
  //              {"id":"enabled","type":"bool","labelZh":"启用","labelEn":"Enabled","value":1},
  //              {"id":"skip","type":"list","labelZh":"黑名单","labelEn":"Blacklist",
  //               "placeholderZh":"game.exe","values":["game.exe"]}],
  //    "curve":{"spanMs":400,"yTopDeltas":1080,"notch":120,"notches":6,
  //             "shape":[[x, y, seg], ...],   // 0..1, and which segment owns the point
  //             "native":[[x, y], ...],       // the raw wheel's own path, same axes
  //             "nativeY":0.667,              // where the raw wheel tops out
  //             "stepX":50, "stepY":200}}
  //
  // The panel renders whatever comes back, so feature #2 needs no UI work.
  //
  //   * `range`  -- a slider. `def` is what a DOUBLE-CLICK on it restores, and the feature must accept
  //                 that value back like any other. A slider with no `def` simply does not reset.
  //                 `hue` is the slider's own colour, and the panel uses it for BOTH the control and whatever
  //                 part of the chart this parameter owns (see the curve block) -- one colour per parameter,
  //                 written once, so a slider and its piece of curve cannot end up different colours.
  //                 `seg` says WHICH SEGMENT that is; see the note under `curve`.
  //   * `bool`   -- a switch.
  //   * `list`   -- a list of strings the user edits. THE FEATURE OWNS THE LIST: where it lives, what it
  //                 means, whether it is used at all. Apex provides the editor and nothing else -- a
  //                 blacklist is not something every feature has, so it is not something the host knows
  //                 about. Edits arrive through `listOp` below, never through `applySetting`.
  //   * `curve`  -- OPTIONAL, and the panel draws exactly what it is given:
  //                 `shape` is the motion (x and y in 0..1 of the box, `seg` says which parameter owns that
  //                 point so the stroke can be coloured per segment), `native` is the raw wheel's own path on
  //                 the same axes, `nativeY` is where that tops out, and `stepX`/`stepY` are the tick steps.
  //
  //                 ⚠️ AND A `range` MUST CARRY THE `seg` IT COLOURS, whenever the feature sends a curve.
  //                 `shape` names a segment as a NUMBER, and this is where a number becomes a parameter: the
  //                 panel reads each range's `seg` and colours that segment with that range's `hue`. Without
  //                 it the two halves of the contract do not meet -- the panel has a number on the curve and
  //                 a colour on the slider and no way to join them, so it would have to GUESS (an earlier
  //                 version looked for the literal ids `slow`/`ramp`/`top`, which worked for exactly one
  //                 feature and would have silently drawn every later feature's chart in one colour).
  //                 A segment no range claims is drawn in the panel's foreground colour, which is visible and
  //                 honest; a feature may therefore ship fewer sliders than segments.
  //                 A feature that sends no curve may omit `seg` entirely.
  //                 The two axis RANGES come as real numbers (`spanMs` in milliseconds, `yTopDeltas` in wheel
  //                 deltas) rather than as fractions, because the panel draws their labels: a grid with no
  //                 numbers on it cannot show that a slider changed the scale. A feature that wants no chart
  //                 sends no `curve` and gets no chart.
  int (*settingsJson)(char *out, int outSize);

  // Set ONE control by id. Called for every slider move, so it must be cheap -- it writes the value
  // into memory and reports whether anything changed; persisting is `saveSettings`'s job.
  // Returns 1 if the value was accepted, 0 if the id or value was rejected.
  int (*applySetting)(const char *id, double value);

  // EDIT A `list` CONTROL (optional; a feature with no lists leaves this null).
  //
  // `id`    the control, as it appeared in settingsJson.
  // `op`    "add" (uses `value`) or "remove" (uses `index`).
  // `value` the text to add, trimmed by the panel. THE FEATURE decides what is acceptable -- it
  //         lower-cases, turns a path into a bare file name, refuses duplicates, or refuses outright.
  //         The panel does not validate, because it does not know what the list means.
  // `index` the row to remove, for "remove".
  //
  // Returns 1 if the list changed, 0 if it did not (refused, duplicate, index out of range). The panel
  // re-reads the list either way, so a refusal shows up as "the row did not appear".
  //
  // WHY THIS IS NOT `applySetting`: that takes a double, and a list row is text. Taking a string apart
  // into an index would mean the host holding a table it has to keep in step with the feature, which is
  // the kind of shared mutable state this ABI exists to avoid.
  int (*listOp)(const char *id, const char *op, const char *value, int index);

  // Persist whatever `applySetting` has accumulated. Called when the panel closes and when the host
  // decides the change has settled -- never per slider move.
  int (*saveSettings)(void);

  // ---- the hot path ----
  //
  // THE WHEEL. Return 1 to take it: the host swallows the original message and the feature's motion
  // becomes its only effect. Return 0 to let it through untouched. Must not allocate or block.
  int (*onWheel)(const ApexWheelEvent *ev);

  // THE FRAME. How many deltas are owed right now, given the time that actually passed. Fractional.
  // Only called while `flags()` reports APEX_FEATURE_ACTIVE, so an idle feature costs nothing.
  double (*tick)(double dtSec);

  // State bits. The host asks for this before doing anything with the feature, so it must be cheap and
  // must not allocate.
  unsigned (*flags)(void);
} ApexFeature;

// Bit 0: the feature's own master switch. The host will not ask an unenabled feature about a wheel.
#define APEX_FEATURE_ENABLED 1u
// Bit 1: motion is in flight, so `tick` should be called this frame. A feature that reports 0 here is
// skipped entirely -- which is what keeps a host full of features from ticking all of them at 250 Hz
// while nothing is moving.
#define APEX_FEATURE_ACTIVE 2u

// The one symbol a feature DLL must export.
typedef const ApexFeature *(*ApexFeatureEntryFn)(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // APEX_ABI_H
