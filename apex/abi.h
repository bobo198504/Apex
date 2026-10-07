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
// 3 -> 4: `reaperPluginRunning` was added to ApexHost (a feature can ask whether this project's REAPER plugin
//         is running, to explain to the user which of the two is in charge -- see the note on that field).
// 4 -> 5: `featureEnabled` was added to ApexHost (a feature can ask whether the USER has it switched on --
//         which a feature that runs its own threads cannot work out for itself; see the note on that field).
// 6 -> 7: `liveText` was added to ApexFeature (a feature can publish a one-line readout of what it is doing,
//         for a small panel at the top of its settings page -- see the note on that field).
// 7 -> 8: `flags()` gained APEX_FEATURE_USER_VISIBLE -- a feature saying "I am holding something the user
//         cannot see", which the host answers by drawing the tray mark's middle bar in the active ink and adding
//         a line to its tooltip. No struct field changed; the version is bumped because the contract a feature
//         is built against did, and because an old DLL would silently never light the mark up -- precisely the
//         class of failure this version number exists to make loud. (A second bit, TELL_USER, was added in
//         the same change and removed again below when the user dropped the notification it drove.)
// 8 -> 9: `flags()` gained APEX_FEATURE_HOLD_HARD -- the SAME hold, one degree stronger, which the host draws
//         in the stronger ink (the user asked for a mark with two working colours, not one). And `group`
//         gained the four things a list of SWITCHES needs to be described: `layout:"rows"`, a `rowToggle`
//         that may name SEVERAL fields, `items[].locked` (a row the user may configure but not remove), and
//         `addHint*` (the typed-text add box a list of names needs and a group of rules does not). See the
//         notes on each below. The version is bumped for both: the bit is a contract, and the new keys mean an
//         older panel would draw the new description as something else (a `rows` group as a stack of boxes,
//         with its switches nowhere).
// 9 -> 10: `liveText` was REMOVED -- a field this time, hence a version of its own. It was the feature's
//         one-line read-out, drawn by the panel above the feature's page; the user asked for it (7 -> 8), then
//         twice said it was not needed (once for the feature that had it first, once for KeepAwake: "其实这个
//         提示可以完全去掉。并不需要"), so it is gone rather than left undrawn -- a contract nobody reads is a
//         promise nobody keeps. Also `group` items may now carry `titleZh`/`titleEn`, because an item's title
//         can be the FEATURE's own words rather than the user's data (KeepAwake's "系统全局" row must follow the
//         interface language, and the feature does not know which language the page is in).
// 10 -> 11: `group` gained `waiting` -- a feature saying "I am still waiting for something outside the panel,
//         keep asking". It exists because the panel used to INFER the same thing from "the document changed",
//         and that guess fails in both directions: it fires on a change that has nothing to do with the wait
//         (the user's report -- a capture's result not reaching the page until something else re-read it), and
//         it never fires when a wait ends without changing anything. See the note on `waiting`.
// 5 -> 6: `applySetting(id, double)` became `setControl(path, text)`, and three control types were added to
//         settingsJson: `select`, `text` and `group`. The old call could only carry a NUMBER, so a
//         pattern, a name or a chosen option could not be sent at all -- which is why the second feature's
//         settings page could only be empty. See the notes on those controls and on setControl.
// 11 -> 12: `quickItems` was added to ApexFeature (a feature can now put a few of its own controls into the
//         QUICK PANEL -- the small flyout the tray shows -- and ApexQuickItem was added for it). A struct
//         field, hence a version of its own.
// 12 -> 13: `ApexQuickItem` gained `groupZh` / `groupEn` -- the NAME OF THE BLOCK a control belongs in. Two
//         fields in a struct, so a version of its own again, and for the same reason as always: a DLL built
//         against 12 read by a 13 host would have the host take whatever followed the struct for a group
//         name, and a DLL built against 13 read by a 12 host would lose it silently. See the notes on those
//         fields; the short of it is that a feature may now say "these belong together" instead of the host
//         giving every control a pane of its own.
// 13 -> 14: `group` gained `noAdd` -- a group whose items are NOT the user's to create. A monitor, an
//         application that is making sound, a row the machine handed us: the page drew a "+ 添加" button above
//         every rows group because it had no way to be told otherwise, and pressing it asked the feature for an
//         item it would refuse to invent. The same kind of change as `waiting` in 10 -> 11 (a KEY a page acts
//         on), so the same treatment: a version of its own, and a panel older than the feature draws one
//         harmless extra button rather than anything wrong.
// 14 -> 15: `ApexQuickItem` gained `toggleId` / `toggleOn` -- A COMPANION SWITCH BESIDE THE CONTROL, which is
//         the mute button on a volume fader (the user's own request: "音量在推子右边增加静音按钮"). Two struct
//         fields, hence a version of its own, exactly like 12 -> 13; and the shape of the answer is the same
//         one: the user asked for it (they had asked whether it was possible), the request is about a ROW rather
//         than about a feature, and a feature that has no companion sends an empty string and gets exactly the
//         row it used to get.
// 21 -> 22: `ApexHost::activeEngines` hands back `ApexEngine` (a kind PLUS the PROGRAM'S OWN NAME) instead of bare
//         `APEX_ENGINE_*` values -- one day after 20 -> 21, and for a reason the user gave in one line: "包括以后可能
//         会增加的 APP". With kinds alone, adding an engine meant editing the HOST (for its probe) AND EVERY FEATURE
//         (for a kind -> name map), and a feature nobody remembered to update would drop the new engine in silence --
//         the failure mode being "the note is missing" with nothing anywhere saying why. Now the host's own table is
//         the only place a program is named, and a feature writes its sentence around the names it is handed.
//
// 20 -> 21: `ApexHost::reaperPluginRunning` was REPLACED by `activeEngines` -- WHICH EXTERNAL SMOOTHING ENGINES ARE
//         RUNNING, EARLIEST-STARTED FIRST. The old call answered one program's yes/no; the user wanted the same
//         sentence for every program that brings its own smoothing, and there are two now: REAPER, through this
//         project's plugin, and Lertaro, which ports the same model and announces itself with a named marker while
//         it is smoothing ("另一个APP：Lertaro也因为特殊原因单独做了平滑滚动，有给了个显式标记"). The note the user
//         asked for is built from that set -- "REAPER、Lertaro专用引擎已运行", and the one that started FIRST is
//         named first -- so the ABI carries the SET and the ORDER, and the FEATURE writes the sentence. The call
//         is a hint either way: no decision may depend on it (see the field itself).
//
// 19 -> 20: `ApexQuickItem` gained `switchLabelZh`/`switchLabelEn` and `toggleLabelZh`/`toggleLabelEn` -- THE WORDS
//         BESIDE A ROW'S TWO SWITCHES. KeepAwake's flyout row is one program with two switches, drawn identically,
//         so nothing on the panel said which one was "keep the machine awake" and which was "keep the screen on";
//         the user asked for labels by name: "保持唤醒两组开关给个文字标签，注明哪个是防睡，哪个是防熄". Four struct
//         fields, hence a version of its own, and a feature that sends none gets exactly the row it used to get --
//         the panel reserves no column for a label nobody has (see the fields themselves).
//
// 18 -> 19: `ApexWheelEvent` gained `extraInfo` -- THE MESSAGE'S OWN EXTRA-INFO WORD, taken straight off the hook
//         (MSLLHOOKSTRUCT.dwExtraInfo). A struct field, hence a version of its own. IT IS HOW A FEATURE TELLS A
//         TOUCHPAD FROM A WHEEL: Windows tags touch/pen input with the signature 0xFF515700 in that word, which
//         is the one signal the OS states outright, and the delta pattern is only the fallback (the rule is
//         common/device.h, moved there from the plugin project at the user's own instruction: "那个过滤，在
//         SmoothWheelScroll for reaper 这个项目里有实现过，直接搬过来就可以"). Until now the app's ONLY input
//         filter was the injected flag, so a touchpad's wheel was smoothed exactly like a notched mouse's.
//         WHY A FACT AND NOT A VERDICT: the host hands over what only the hook can read, and the feature decides
//         -- "do not smooth a touchpad" is a statement about SMOOTHING, the same argument that moved the exclude
//         list out of decision.h and into the feature. A feature that ignores this field behaves as it did.
//
// 17 -> 18: `group` gained `live` -- THIS GROUP'S ROWS ARE A PICTURE OF SOMETHING OUTSIDE THE PAGE, so the panel
//         keeps asking for the document while such a group is on screen. A KEY a page acts on, like `noAdd`,
//         `waiting` and `quick` before it, and the reason it is a version of its own is the same in every case: a
//         page that does not know the key silently shows a stale list for ever, and "the rows never change" and
//         "nobody asked again" are indistinguishable from the outside. The user's request, one sentence: "媒体控制
//         列表的「刷新应用列表」按钮去掉，这个做成实时自动刷新" -- a button that re-reads a list of the programs
//         that are making sound is a button the user has to press to be told something the program already knows.
//
// 16 -> 17: TWO THINGS AT ONCE, AND THEY ARE ONE REQUEST FROM THE USER -- "快速面板" stops being one switch per
//         CONTROL and becomes one switch per GROUP for the groups whose rows are not the feature's own.
//   * `group` gained `quick` -- THE SWITCH THAT MAPS A WHOLE GROUP INTO THE QUICK PANEL, and the page draws it at
//     the right-hand end of the group's own line (its heading, or the row the user adds entries on) instead of as
//     a row among the parameters. The permission rule above is unchanged; what changed is that it can be asked
//     ONCE. The user asked for it in three places in the same breath: "保持唤醒插件的快速面板给一个开关，放在「添加」
//     按钮右侧", "媒体控制插件的「快速面板」开关，位置移到亮度和音量各自小标题的右侧", and for the wheel feature
//     "只要给一个…位置就放在现在的「最高速度」那边". A KEY a page acts on, like `noAdd` and `waiting` before it.
//   * ... and `ApexQuickItem::toggleId` IS NOW DRAWN ON A TOGGLE ROW TOO. That is a change to the MEANING of an
//     existing field rather than a new one (the note on it below said "only for a range row"), which is exactly
//     the kind of silent difference this version number exists to make loud. KeepAwake's flyout is one row per
//     program with TWO switches on it -- the user's "快速面板按列表显示系统和各应用的两个功能开关" -- and the second
//     one is the companion button, drawn with the display icon (`APEX_QUICK_ICON_DISPLAY`) because that is what it
//     means. A feature that never sets `toggleId` on a toggle is unaffected.
//
// 15 -> 16: ... and `toggleIcon` -- WHICH PICTURE THAT COMPANION IS DRAWN WITH (the same change, one day later:
//         the user wanted the screen-off control on the fader as well -- "快速面板的熄屏功能也像静音按钮一样，
//         做上去"). The panel owns every drawing, but it cannot invent a picture for a meaning nobody told it: a
//         mute button and a screen-off button are both two-state and both belong in a row, and they must not look
//         the same. So the feature names the meaning and the host picks the drawing -- an unknown value falls
//         back to the plain one rather than to a guess.
//
// The version is bumped rather than a field simply appended, because the host requires an EXACT match -- see
// the loader: a DLL built against an older layout would otherwise be read past its own end. Refusing it with a
// log line is the whole point of carrying a version at all.
#define APEX_ABI_VERSION 22u

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
  // THE MESSAGE'S EXTRA-INFO WORD, exactly as the hook read it (MSLLHOOKSTRUCT.dwExtraInfo). It is carried
  // because it is the only place the OS says WHICH DEVICE sent the wheel: touch and pen input is tagged with
  // the signature 0xFF515700 (see common/device.h, which is the rule that reads it). The host does not
  // interpret it and no decision may depend on it -- a feature that ignores this field behaves as it did
  // before the field existed.
  unsigned long long extraInfo;
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

// WHICH EXTERNAL ENGINE, for ApexHost::activeEngines. A bitmap would answer "who is running", but the ORDER
// carries information too -- the user's rule is "谁先运行谁显式在前面" -- so these travel as a LIST, earliest first.
enum
{
  APEX_ENGINE_REAPER = 1, // this project's plugin, loaded inside a running reaper.exe
  APEX_ENGINE_LERTARO = 2 // Lertaro's own smoothing, announced by its named marker (see host_win.cpp)
};

// ONE RUNNING ENGINE, as ApexHost::activeEngines reports it.
typedef struct ApexEngine
{
  int kind; // APEX_ENGINE_*
  // ⚠️ THE PROGRAM'S OWN NAME, AND IT IS NOT A TRANSLATION: "REAPER" and "Lertaro" are product names, spelled the
  // same in every language, and the panel never shows them on their own -- the SENTENCE around them belongs to the
  // feature. That split is the point: a feature that had to map `kind` to a name would have to be edited every
  // time a program is added, and a feature that was NOT edited would drop the new engine in silence.
  char name[32];
} ApexEngine;

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

  // WHICH EXTERNAL SMOOTHING ENGINES ARE RUNNING ON THIS MACHINE, AND IN THE ORDER THEY STARTED.
  //
  // ⚠️ THIS REPLACED `reaperPluginRunning` (ABI 20 -> 21), WHICH ASKED ABOUT REAPER ALONE. What the user wanted is
  // the same sentence for every program that brings its own smoothing, and there are two: REAPER through this
  // project's plugin, and Lertaro -- which ports the same model and publishes a marker of its own while it is
  // smoothing. So the ABI carries the SET, and the feature writes the sentence in its own words.
  //
  // WHY A FEATURE WANTS TO KNOW: two implementations of smoothing inside one program is one too many -- the
  // external one does it from inside the program it belongs to and does it better. Apex already hands those
  // programs over (see decision.h), but that is invisible: the user sees smoothing they cannot account for, or
  // none where they expected it. So the feature says which engines it is leaving room for, on its own settings
  // page, next to the list of programs it leaves alone.
  //
  // ⚠️ IT IS ABOUT THE MACHINE, NOT ABOUT THE CURSOR. The panel shows this while the user is IN the panel --
  // exactly when none of those programs is under the pointer -- so an answer derived from the current target
  // would be false nearly every time it was read.
  //
  // ⚠️ AND IT IS A HINT, NOT A DECISION. It is not part of the wheel rule, no decision may depend on it, and a
  // feature must not use it to decide whether to take a wheel -- the decision path has its own, per-target answer
  // (ApexTarget::handlerState) which is the one allowed to be believed.
  //
  // `out` receives one ApexEngine per running engine, EARLIEST-STARTED FIRST, and the return value is how many
  // were written. 0 means "nothing is running, or it could not be confirmed": a note that says "running" is a
  // claim, and this program does not make claims it cannot support.
  //
  // ⚠️ THE NAME COMES WITH IT, AND THAT IS WHAT MAKES ADDING A PROGRAM A ONE-PLACE CHANGE. The host has one table
  // of engines (a row per program, with its probe); a feature writes the sentence around whatever names it is
  // handed and never maps a `kind` to a word of its own, so a program added tomorrow appears without touching any
  // feature -- and cannot be dropped in silence by one that was not updated.
  //
  // ⚠️ AN ENGINE THAT IS CONFIRMED BUT WHOSE START TIME COULD NOT BE READ GOES LAST, NOT OUT. Losing the order is
  // worth much less than losing the fact -- the note exists to explain smoothing the user cannot account for.
  //
  // ⚠️ NOT FREE THE FIRST TIME (it may enumerate processes and read a module list), cheap after that -- but it is
  // still a settings-page call, NOT something to call from onWheel or tick.
  int (*activeEngines)(ApexEngine *out, int max);

  // IS THE USER KEEPING THIS FEATURE SWITCHED ON? `id` is the feature's own id; 1 = yes, 0 = switched off.
  //
  // ⚠️ WITHOUT THIS, A FEATURE THAT RUNS ITS OWN THREADS CANNOT BE TURNED OFF. The host's switch works by
  // simply NOT CALLING a feature that is off, which is complete for a feature that only acts when it is
  // called -- the wheel one does: no calls, no motion. A feature that owns a worker thread, a hook or a timer
  // keeps running regardless, and would carry on doing its job while the panel shows it as off. That is not
  // hypothetical; it is what the second feature would have done.
  //
  // ⚠️ WHY IT TAKES AN ID rather than answering for "the caller": the other host calls can assume the caller
  // because the host is inside its own call into a feature (see featureDir). This one is asked from the
  // feature's own thread, where the host has no idea who is talking -- so the feature says so.
  //
  // ⚠️ SAFE TO CALL FROM ANY THREAD, which is the entire point: it is answered from state the host keeps,
  // not by calling back into the host's own machinery. (Nothing else here is thread-safe: everything else
  // belongs inside the host's own calls into the feature.) Do not cache the answer -- the user can flip the
  // switch at any moment -- and it is a lookup by id, so a feature asks it a few times a second at most.
  int (*featureEnabled)(const char *id);

  void *hostUser;
} ApexHost;

// ---- THE QUICK PANEL: the few controls a feature wants one click away ---------------------------------
//
// THE QUICK PANEL IS A SMALL FLYOUT THE HOST DRAWS ITSELF, above the tray icon, on a single click of it. It
// carries exactly two kinds of thing:
//
//   1. A COMPACT LIST OF EVERY FEATURE'S ON/OFF SWITCH. The host builds that one on its own -- the switch is
//      the host's own `off` list (see hostconfig.h), the one the settings page also shows -- and it needs
//      nothing from a feature at all. That is the "紧凑视图排列的所有插件开关" half.
//   2. THE CONTROLS A FEATURE ASKS FOR, which is what this structure is for. A feature that wants a knob, a
//      fader or a switch in there describes it once per call and the host draws it.
//
// WHY IT EXISTS AT ALL, RATHER THAN "JUST OPEN THE SETTINGS": the settings window is a browser. It was
// MEASURED at ~0.55 s to appear with a warm profile (~1.4 s on the first run), it is a whole window with a
// sidebar and pages, and it opens in the middle of what the user was doing. The things a user reaches for
// most often are one switch or one number. A flyout that is already in memory is instant, and it goes away
// when they click anywhere else.
//
// ⚠️ WHY THE FEATURE DESCRIBES ITS CONTROLS HERE INSTEAD OF THE HOST REUSING `settingsJson`. Three reasons,
// and the third is the one that decides it:
//   * THE HOST HAS NO JSON PARSER, DELIBERATELY. It is the process the wheel hook lives in (see decision.h),
//     and `settingsJson` is a string a feature assembles by hand; reading it in the host would put a parser
//     -- and every malformed document a third-party feature can produce -- inside that process. This contract
//     is C structures instead, which cannot be malformed.
//   * THE TWO SURFACES ARE NOT THE SAME PICTURE. The quick panel is ~320 px wide with one line per control,
//     so its labels are SHORT and it shows few items; a settings page has room for a paragraph and a unit.
//   * AND NOTHING IS DUPLICATED THAT MATTERS: the VALUE has one home and one setter. Whatever `id` names here
//     is the same path `setControl` takes, so the knob and the slider on the settings page are two views of
//     ONE stored number, clamped and saved by the same feature code. What is written twice is only "what this
//     control is called and what shape it is down here".
//
// ⚠️ THE FEATURE OWNS THE CHOICE, THE HOST OWNS THE PICTURE. A feature never draws, never picks a colour
// (unless it sends `hue` to match its settings-page slider) and never learns where the panel is. It says what
// it has; the host decides how a toggle, a fader and a knob look, because that is a property of the PANEL.

// The three shapes the quick panel can draw. THREE, AND THEY ARE THE USER'S OWN LIST: "目前可操作类型暂时定为
// 三种，一种是开关，一种是推子，一种是旋钮". A shape not listed here is not drawn -- an unknown `type` is
// skipped rather than guessed at, because a control drawn as the wrong shape is a control that lies about
// what it does.
enum
{
  APEX_QUICK_TOGGLE = 0, // on/off; `value` is 0 or 1
  APEX_QUICK_SLIDER = 1, // a horizontal fader over min..max
  APEX_QUICK_KNOB = 2    // the same range, drawn as a dial
};

typedef struct ApexQuickItem
{
  // ⚠️ FIXED-SIZE BUFFERS, NOT `const char *` -- AND THAT IS THE ONE PLACE THIS CONTRACT DIFFERS FROM THE
  // REST OF THE HEADER. Everywhere else a feature hands over a pointer to a string it owns (ApexFeature::nameZh
  // and friends) and the host reads it in the same call. The quick panel is different: it KEEPS its items for
  // as long as its window is up and redraws them on every animation frame, so a pointer would have to stay
  // valid across calls -- something this ABI has never promised, and something a feature that builds its
  // labels per call would break without any warning. So the strings are COPIED into the caller's structure,
  // exactly as `ApexTarget::exe` is. They are UTF-8.
  char id[64];      // the `setControl` path: "glide", or "rules[3].awake"
  char labelZh[64];
  char labelEn[64];
  char unit[24];    // may be empty ("ms", "x", "d")

  // ⚠️ THE BLOCK THIS CONTROL BELONGS IN -- AND AN EMPTY ONE MEANS "A BLOCK OF MY OWN" (ABI 12 -> 13).
  //
  // The host used to give every control its own pane, because the user's rule was "以一个开关为一组" (see
  // quickpanel.h): a pane IS a grouping, and a control that floats alone needs no heading. That is still what an
  // empty `group` gets. What changed is that a feature may now say several of its controls belong together, and
  // then ONE pane holds them and `groupZh`/`groupEn` is its heading.
  //
  // ⚠️ THE USER'S OWN WORDS, and they are why this exists at all: "这一组做一个快速面板开关，只要亮度控制映射到
  // 快速面板，熄屏不用，分组名称为「亮度」". A media feature's brightness controls are per MONITOR -- two monitors
  // means two faders -- and a flyout with one floating pane per monitor says "these are unrelated" about a set of
  // controls that are one thing. The grouping is still not by FEATURE (a feature may send two groups, or none);
  // it is by whatever the feature says belongs together, and the name is the feature's own words because the
  // feature is the only thing that knows what the group is.
  //
  // ⚠️ THE ORDER OF THE ITEMS IS THE ORDER INSIDE THE BLOCK, and blocks are drawn in the order their first item
  // was sent. A feature that sends two groups therefore decides which is above the other by sending one first.
  //
  // ⚠️ AND A FEATURE MAY STILL BE SPARSE WITH IT: the permission rule above is unchanged -- a control is in the
  // flyout because the USER mapped it (one switch per control on the feature's own page), never because the
  // feature judged it useful there. `group` says where a mapped control goes, not whether it goes.
  char groupZh[32];
  char groupEn[32];

  int type;               // APEX_QUICK_*
  double min, max, step;  // ignored for a toggle
  double value;           // THE CURRENT VALUE, read from the feature's own settings every call

  // ⚠️ 0 MEANS "THE HOST'S OWN COLOUR", NOT BLACK. The settings page's sliders carry their own `hue` (see the
  // `range` control below), and a feature that wants the same colour in both places sends the same number
  // here. A feature with no opinion sends 0 and gets the panel's accent -- which is also what keeps the
  // flyout looking like one surface rather than a bag of feature-specific colours.
  unsigned hue; // 0xRRGGBB, or 0

  // ⚠️⚠️ A COMPANION SWITCH AT THE RIGHT OF THIS ROW (ABI 14 -> 15), or an EMPTY STRING FOR "there is none".
  //
  // THE USER ASKED FOR IT IN ONE SENTENCE: "音量在推子右边增加静音按钮". A volume fader and its mute are one
  // decision about one application, and putting the mute on a row of its own would double the height of the
  // pane for a control that is one bit -- so it goes IN the row, and the feature says which path it writes to
  // exactly as it does for the control itself.
  //
  // ⚠️ IT IS A SWITCH, NOT A BUTTON WITH A MEANING: the host draws it as a compact button (the panel owns its
  // picture -- see the note at the top of this block), and it knows only "on" and "off". Whether "on" means
  // muted, hidden, locked or anything else is the FEATURE's word for its own control, and `setControl` is what
  // receives it -- `toggleId` is a normal control path like `id`, so it is clamped, validated and stored by the
  // same code that owns the value. The panel sends "1" or "0" and then REBUILDS from what the feature reports.
  //
  // ⚠️ AND IT IS DRAWN FOR A ROW THAT HAS A CONTROL OF ITS OWN -- a slider, a knob, OR (since ABI 16 -> 17) a
  // toggle. The first version allowed a RANGE row only, on the reasoning that "a toggle row has its own switch
  // already, so a second one beside it would be two answers to one question". That is true of one question and
  // false of two: KeepAwake's rows ask a program two things ("keep the machine awake" / "keep the screen on"),
  // the settings page has drawn them as two switches per line for a while, and the user asked for the flyout to
  // match -- "快速面板按列表显示系统和各应用的两个功能开关". So the line is: the row's own switch answers the
  // row's own control and the companion answers ITS OWN path, and both are stored by the feature that named
  // them. A `kNote` row (and the host's own feature switch) still ignores it: there is no control to sit beside.
  char toggleId[64]; // the `setControl` path of the companion, or "" for none
  int toggleOn;      // 0 or 1, read from the feature every call -- exactly like `value`

  // ⚠️ ... AND WHAT THAT COMPANION MEANS, SO THE PANEL CAN DRAW IT (ABI 15 -> 16). The panel owns every drawing
  // here -- no feature sends a colour it has not been given a slot for, and none sends a picture at all -- but
  // "the host decides how it looks" only works while the host knows what the thing IS. It cannot: a mute button
  // and a screen-off button are both plain two-state switches as far as `toggleOn` is concerned, and they must
  // not be drawn the same (the user asked for the second one BY NAME, meaning "like the mute button": "快速面板的
  // 熄屏功能也像静音按钮一样，做上去").
  //
  // ⚠️ SO THIS IS A HINT ABOUT MEANING, NOT A PICTURE: the feature says which of the small set of things this is
  // (see APEX_QUICK_ICON_*), and the panel draws the icon it keeps for that thing -- in both themes, at every
  // scale. An unknown value is drawn as APEX_QUICK_ICON_PLAIN rather than guessed at.
  int toggleIcon; // APEX_QUICK_ICON_*

  // ⚠️ THE WORD BESIDE EACH OF A ROW'S TWO SWITCHES (ABI 19 -> 20). A switch row can carry two switches that are
  // drawn identically -- KeepAwake asks one program "keep the machine awake" and "keep the screen on" -- and until
  // now nothing on the panel said which was which. The user's request: "保持唤醒两组开关给个文字标签，注明哪个是
  // 防睡，哪个是防熄".
  //
  // ⚠️ THE PANEL CANNOT INVENT THESE WORDS, which is why they are fields at all: the panel owns every drawing
  // (see the note above), but "防睡" is a statement about THIS feature's own control and about nothing else. A
  // feature that sends nothing gets exactly the row it used to get -- an empty label is not drawn, and the column
  // it would need is not reserved either.
  //
  // ⚠️ THEY ARE SHORT BY CONSTRUCTION. The panel draws them in its small font inside a FIXED column (the layout is
  // pure arithmetic with no font in it -- see MeasureModel in quickpanel.h -- so the width cannot follow the
  // words). A long sentence here is therefore not "a longer label", it is a truncated one: two characters of
  // Chinese, or about six latin characters.
  //
  // `switchLabel*` describes the row's OWN switch (the one whose value is `value`); `toggleLabel*` describes the
  // companion (`toggleId`). Both are drawn on a TOGGLE row only: a fader or a knob has a numeric read-out beside
  // it, which names itself.
  char switchLabelZh[24];
  char switchLabelEn[24];
  char toggleLabelZh[24];
  char toggleLabelEn[24];
} ApexQuickItem;

  // What a companion switch (`toggleId`) is, for the purpose of choosing its icon. THREE, AND THEY ARE THE ONLY
  // THREE THE PANEL HAS A PICTURE FOR: anything else would be a picture the host made up about a control it does
  // not understand, which is worse than a dot.
  //
  // ⚠️ AND IT IS ONLY CONSULTED FOR A COMPANION ON A RANGE ROW (a fader or a knob), because that is the only place a
  // companion is drawn as a BUTTON. A toggle row's companion is drawn as a SECOND SWITCH -- the same sliding control
  // as the one beside it, which is what the user asked for once they saw the button there: "保持唤醒的快速面板…
  // 是要用一样的滑动开关" -- and a switch carries no glyph, so a feature with such a row sends `APEX_QUICK_ICON_PLAIN`
  // (sending a picture nothing draws would be a field nobody reads).
  enum
  {
    APEX_QUICK_ICON_PLAIN = 0,   // no opinion: a plain two-state mark
    APEX_QUICK_ICON_MUTE = 1,    // sound on / muted: a speaker, struck through when it is on
    APEX_QUICK_ICON_DISPLAY = 2  // a screen on / off: a monitor, struck through when it is on
  };

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

  // Called once after loading. ⚠️ RETURN **0** WHEN READY, NON-ZERO TO REFUSE (the host unloads it, says so in
  // its log, and the panel shows the reason). Everything the feature needs -- its settings file, its state --
  // is set up here.
  //
  // ⚠️ THIS COMMENT SAID "RETURN 0 TO REFUSE" AND THE HOST HAS ALWAYS DONE THE OPPOSITE (`if (rc != 0)` in
  // loader_win.cpp, and both shipped features return 0 when ready). Reading the comment instead of the code
  // cost a whole debugging session: KeepAwake returned 1, the host read that as a refusal, unloaded the DLL --
  // and the feature's own worker thread, started a moment earlier, kept running inside unmapped memory and
  // took the host down with SIGSEGV a second after startup. A REFUSED FEATURE MUST NOT HAVE LIVE THREADS: if
  // init has already started something, either stop it before refusing or do not refuse.
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
  // `settingsFile` -- OPTIONAL, top level, and it is the NAME of the file this feature keeps its settings in,
  // beside itself ("config.json"). The panel's "open settings file" button asks the HOST to open a path, and the
  // host cannot know which file a feature uses: the ABI fixes the FOLDER (`Plugins/<id>/`) and says nothing about
  // a name. Without this key the host falls back to the `<id>.ini` convention, which is right for a feature that
  // ships one and opens nothing at all for a feature that does not. ⚠️ IT IS A NAME, NOT A PATH: the host joins
  // it to the feature's own folder, and a feature has no business naming a file outside the folder it owns.
  //
  // `summaryZh` / `summaryEn` -- OPTIONAL, top level: ONE SHORT LINE saying what this feature is for, shown small
  // beside the version at the top of its page ("插件页最上方标题位，版本号后，可以小字简单说明插件的主要功能").
  // The panel cannot write it: it has no idea what any feature does, and a sentence written in the page would be
  // the page describing something it cannot see. Same shape as every other label here -- both languages, and the
  // reader's one is used, with the other as a fallback.
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
  //                 about. Edits arrive through `listOp` below, never through `setControl`.
  //   * `select` -- ONE OF NAMED CHOICES. `options` is `[{"value":"ime","labelZh":"...","labelEn":"..."}]`
  //                 and `value` is the chosen `value` string (NOT an index -- an index would break the
  //                 moment a feature reordered its own list, and it would put the feature's vocabulary in
  //                 the page). Sent back through `setControl` as the same string.
  //   * `text`   -- a single line of text. `value` is what it holds; `placeholderZh/En` is the greyed hint
  //                 an empty field shows. THE FEATURE VALIDATES: reply 0 from `setControl` to refuse, and the
  //                 page re-reads the control so a refused value is replaced by what the feature actually has
  //                 (the same rule as `listOp`, and for the same reason -- the page cannot know what is valid).
  //
  //                 OPTIONAL `toggle`: the id of a `bool` FIELD of the same item, drawn as a CHECKBOX at the left
  //                 of the text box. The two are one row because they are one decision ("use this value" and the
  //                 value), and the checkbox writes its own field through `setControl` like any other control --
  //                 `rules[3].use_process` beside `rules[3].process_pattern`.
  //
  //                 ⚠️ WHAT IT IS FOR: keeping a value while switching it off. Without it, stopping a condition
  //                 means DELETING the pattern -- and a captured pattern is usually the most valuable thing in a
  //                 rule, so the only way to try the rule without it is to destroy it. The feature owns both
  //                 halves (in this feature's file they are two fields, `use_process` and `process_pattern`, and
  //                 the ENGINE gates on both); the panel only draws them together.
  //   * `hotkey` -- A KEY COMBINATION THE USER RECORDS RATHER THAN TYPES. `value` is the combination in the same
  //                 text the feature's own grammar uses ("Ctrl+Space"). Clicking the control puts it into "press
  //                 the keys now" state; the combination the user then presses is sent back through `setControl`
  //                 as that same text, and there is nothing to type.
  //
  //                 ⚠️ WHY IT IS A CONTROL TYPE AND NOT A TEXT FIELD WITH A HINT. A combination is not text:
  //                 nobody can spell "Ctrl+Space" reliably, and a typed one can be a combination the program
  //                 cannot actually send -- which the user then discovers as "the shortcut does nothing". The
  //                 recording makes the keyboard itself the input, so nothing unspellable can be entered.
  //
  //                 ⚠️ THE PAGE REFUSES TWO THINGS ON ITS OWN, and they are properties of the PAGE rather than of
  //                 any feature:
  //                   * a press that is only modifiers -- there is no combination yet, so it keeps waiting;
  //                   * a combination that is ALREADY ANOTHER CONTROL'S SHORTCUT on this page: `actions[].key`
  //                     is what the panel itself draws and the host registers, so two controls must not share
  //                     one. The page says so instead of sending it.
  //                 Everything else is the feature's to decide: reply 0 from `setControl` to refuse, exactly as
  //                 for `text`. A feature that requires a modifier says so by refusing (this one does: a toggle
  //                 bound to a bare letter would fire on every letter typed).
  //   * `group`  -- A REPEATABLE BLOCK: the same fields over and over, which is what a rule, a profile or a
  //                 shortcut is. It carries NO `value`; instead:
  //                   `fields` -- the schema of one item, an array of controls of any type above EXCEPT
  //                               `group` (nesting is refused, not unsupported: it would make the path
  //                               syntax ambiguous and the page's layout unbounded);
  //                   `items`  -- the values, one object per item: `{"<field id>":<value>, ...}`.
  //                 The panel pairs them by field id. Structural edits (add, remove, move) go through
  //                 `listOp`; a FIELD inside an item is set through `setControl` with a path (below).
  //
  //                 ⚠️ THE FEATURE DECIDES WHAT A NEW ITEM IS. `listOp(id,"add")` asks for one and the feature
  //                 supplies its defaults -- the page has no business inventing a rule, and a default written
  //                 in the page would be a second place a feature's settings are defined.
  //
  //                 `layout` -- OPTIONAL, and it is the FEATURE's request for how its own items are drawn:
  //                   "stack"  (the default) one box per item, one under the other, its fields READ-ONLY (see
  //                            the note there -- a stack has no Edit button, so it cannot have a draft);
  //                   "master" a LIST of items, with only the SELECTED one's fields shown beside it;
  //                   "rows"   one LINE per item, every field live, the `rowToggle` switches and a remove
  //                            button in the line. See its own note below.
  //
  //                 ⚠️ AND "rows" IS A THIRD KIND OF DATA, NOT A COMPACT "stack". A rule has a dozen fields, a
  //                 name you type once and a draft worth having; a LIST OF PROGRAMS WITH SWITCHES has no draft
  //                 at all -- there is nothing to type but the name the row was added under, and the only two
  //                 questions about it ("keep awake?" / "keep the screen on?") are switches. Pressing Edit to
  //                 answer either would be a form standing in the way of a checkbox. So in "rows" every field
  //                 is live, there is no selection and no Edit/Save, there is no Edit draft, and each line
  //                 carries its own remove button.
  //
  //                 ⚠️ AND NO DRAG REORDERING IN "rows": the order of such a list is not meaning (KeepAwake's
  //                 answer is the maximum over its rows, and which row said so does not depend on the order).
  //                 A feature that does need order asks for "master" or "stack", which have it.
  //
  //                 ⚠️ WHY THE FEATURE GETS A SAY. A group of two or three items reads fine stacked; a group of
  //                 twenty does not -- the user scrolls past every rule to reach the last one, and what they
  //                 are actually doing (pick a rule, change it) is a two-part act that a stack makes serial.
  //                 That is a property of THIS feature's data -- how many items its users really have -- and
  //                 not of the panel, so the panel must be TOLD rather than decide. The user's words for the
  //                 stacked version: "现在这样做是变好看了，但是不方便，规则一多，要不断往下翻".
  //
  //                 ⚠️ THE SELECTION IS THE PAGE'S OWN STATE. It is not sent anywhere and does not survive a
  //                 redraw: switching to master layout must not cost a round trip per click, and the feature
  //                 has no opinion about which item the user is looking at.
  //
  //                 ⚠️ AN ITEM'S TITLE IN THE LIST is `items[i].title` -- the same string the stacked header
  //                 shows, and in "rows" it is the LINE'S OWN LABEL. A feature that sends none gets a number,
  //                 which is exactly why filling it in matters: "规则 3" tells the user nothing about which one
  //                 to click.
  //
  //                 `titleZh` / `titleEn` -- OPTIONAL, and they are `title` for a feature's OWN words: when both
  //                 are given the panel draws the reader's one and ignores `title`.
  //
  //                 ⚠️ WHY ONE TITLE IS NOT ENOUGH, AND WHY THE FEATURE CANNOT JUST SEND THE RIGHT ONE. Every
  //                 other string in this contract comes in pairs (`labelZh`/`labelEn`) for the same reason: the
  //                 FEATURE does not know which language the page is in, and it must not have to. An item's
  //                 `title` is usually the USER's data -- a rule's name, a captured program -- and that has no
  //                 translation; but a title can also be the feature's own word for a row it built itself, and
  //                 then it is a label like any other. KeepAwake's undeletable "系统全局" row is exactly that,
  //                 and the user found it: "系统全局，这几个字，要随 中/英文切换".
  //
  //                 `actions` -- OPTIONAL BUTTONS THE FEATURE NEEDS, described rather than drawn. An array of
  //                 `{"op":"...","labelZh":"...","labelEn":"..."}`; pressing one calls
  //                 `listOp(<group id>, <op>, "", <the selected item's index>)`.
  //
  //                 `rowToggle` -- OPTIONAL, and it is ONE FIELD ID OR AN ARRAY OF THEM: the panel draws each as
  //                 a switch at the right-hand end of every list row instead of among the fields, so an item can
  //                 be switched on or off without being opened. Each field stays in `fields` and in each item's
  //                 `values` -- they are the same controls, drawn in the row -- and the row's own click does not
  //                 select them, so a switch is one click rather than two.
  //
  //                 ⚠️ WHY IT MAY BE SEVERAL, AND WHY THAT IS STILL THE FEATURE'S DECISION. KeepAwake asks two
  //                 questions of every listed program ("keep the machine awake" / "keep the screen on"), and
  //                 both belong on the line: they are one choice seen from two sides, and opening the row to
  //                 answer the second would be the form-in-the-way-of-a-checkbox again. Which fields are
  //                 switches is still the feature's to say -- the panel is told ids and draws switches.
  //
  //                 ⚠️ THE ORDER OF AN ARRAY IS THE ORDER THEY ARE DRAWN, and a feature sending two should send
  //                 the WEAKER FIRST: the pair is read left to right, and the feature -- not the panel -- is
  //                 what knows which of its own switches is the bigger commitment.
  //
  //                 `addHintZh` / `addHintEn` -- OPTIONAL, and only for a "rows" group: the text its add box's
  //                 placeholder shows. A rows group that declares one gets the LIST-STYLE add control -- a text
  //                 box and a button, as the `list` control has -- and the typed text is passed to
  //                 `listOp(<group id>, "add", <text>, <index>)`.
  //
  //                 `noAdd` -- OPTIONAL, `true` on a group whose items the USER CANNOT CREATE. The page then
  //                 draws no Add control at all. Without it the page has to assume the group is growable -- and
  //                 it did, so every rows group carried an Add button whether or not `listOp("add")` meant
  //                 anything to that feature: MediaControl's monitor and application rows come from the machine,
  //                 and its shortcut rows are one per monitor, so the three buttons on that page could only ever
  //                 ask for items the feature would refuse to invent.
  //
  //                 ⚠️ IT IS NOT "READ-ONLY", AND THE DIFFERENCE IS THE WHOLE REASON IT IS A SEPARATE KEY FROM
  //                 `locked`: `locked` (on an ITEM) says "this row cannot be removed"; `noAdd` (on the GROUP)
  //                 says "no new rows". A group may have either, both or neither -- MediaControl's rows are
  //                 locked AND unaddable, while a list of programs the user curates is neither.
  //
  //                 ⚠️ AND A FEATURE MUST STILL REFUSE the op if it arrives anyway (`listOp` answers 0), because
  //                 the page is not a gatekeeper and an older page will send it.
  //
  //                 ⚠️ WHY THIS EXISTS. `add` on a group means "append an item and YOU supply the defaults",
  //                 which is right for a rule (every field of it is editable afterwards) and useless for a list
  //                 of NAMES: the name IS the item's identity, there is no field to type it into, and a row
  //                 titled "第 3 项" that matches no program would be a row the user can neither fix nor
  //                 understand. So the feature may ask for a text box, exactly as the `list` control has one.
  //
  //                 `quick` -- OPTIONAL, AND IT IS THE ONE SWITCH THAT MAPS THIS WHOLE GROUP INTO THE QUICK PANEL
  //                 (ABI 16 -> 17): `{"id":"...","labelZh":"...","labelEn":"...","value":<0|1>}`. `id` is an
  //                 ordinary control path -- it travels through `setControl` like any other switch -- and the
  //                 value is read back on every render, exactly as a `bool` parameter's is.
  //
  //                 ⚠️ IT IS THE SAME PERMISSION AS EVERY OTHER QUICK-PANEL SWITCH, ASKED ONCE. The rule above
  //                 ("插件自己的控件要明确有开关映射到快速面板，才给") has not changed: nothing reaches the flyout
  //                 until the user says so. What changed is that a group whose rows are NOT the feature's own -- a
  //                 monitor, an application that is making sound, the programs a user listed -- used to need one
  //                 switch per CONTROL for a decision that is one decision about the whole group, and those
  //                 switches ended up as a stack of rows under the group they belonged to. The user's words:
  //                 "这一组做一个快速面板开关" for a media group, and for the keep-awake list "给一个开关…快速面板
  //                 按列表显示系统和各应用的两个功能开关".
  //
  //                 ⚠️ WHERE THE PAGE DRAWS IT IS THE PAGE'S BUSINESS, AND IT IS NOT A ROW. It goes at the
  //                 right-hand end of the line that already carries this group's own controls -- the row the user
  //                 adds entries on when the group has one (the user asked for it "放在「添加」按钮右侧"), and
  //                 otherwise the group's heading ("位置移到亮度和音量各自小标题的右侧"). The feature supplies the
  //                 label and nothing else about the picture, like every other control here.
  //
  //                 ⚠️ AND A GROUP THAT HAS ONE STILL LISTS ITS FIELDS: `quick` says where the group can be
  //                 reached FROM, not what is in it. Whether the flyout then holds one row or twenty is the
  //                 FEATURE's answer (`quickItems`), which is also where the values live -- so the switch and the
  //                 rows can never disagree about what is mapped.
  //
  //                 `live` -- OPTIONAL, `true` on a group WHOSE ROWS ARE A PICTURE OF SOMETHING OUTSIDE THE PAGE
  //                 (ABI 17 -> 18). The panel then keeps asking for the document while that group is on screen, so
  //                 a row that appeared a moment ago appears here too.
  //
  //                 ⚠️ WHAT IT REPLACES IS A BUTTON, AND THAT IS THE WHOLE ARGUMENT: this feature's volume group used
  //                 to declare an action ("刷新应用列表" / "Refresh applications") that re-read the machine's audio
  //                 sessions. The user's words: "媒体控制列表的「刷新应用列表」按钮去掉，这个做成实时自动刷新". A
  //                 list of the programs that are making sound is not a document with a version -- it is a fact
  //                 about the machine that changes while the user is looking at it, and a button that says so is a
  //                 button that asks the user to keep pressing it.
  //
  //                 ⚠️ IT IS A PROPERTY OF THE GROUP, NOT OF THE PAGE, because "these rows are live" is something
  //                 only the feature knows: the page cannot tell a list that changes by itself from a list the
  //                 user typed (KeepAwake's program list is the second kind, and re-reading it forever would be
  //                 work nobody asked for).
  //
  //                 ⚠️ AND THE PANEL'S RATE IS SLOWER THAN THE FEATURE'S OWN THROTTLE, on purpose: it re-reads about
  //                 once a second, and a feature is expected to keep its own enumeration cheap (MediaControl
  //                 caches its session list for 800 ms). `live` is for "a person is watching this number", not for
  //                 a hot path -- the ABI's cheap, high-rate channel is `activity` in the other direction.
  //
  //                 ⚠️ IT IS NOT `waiting`: `waiting` is a WAIT (it starts when the feature is waiting for
  //                 something and ENDS when that arrives), while a live group is simply never stale. A feature that
  //                 used `waiting` for this would leave the page polling for ever with no way to say "done", and
  //                 the two mean different things to the page: one re-reads at its wait rate, the other at its
  //                 watching rate.
  //
  //                 `items[i].locked` -- OPTIONAL, `true` on an item that is the FEATURE's own (a built-in
  //                 entry rather than one the user made): the panel draws NO remove button for it and refuses
  //                 to drag it. Its FIELDS stay live -- a locked row is locked against DELETION, not against
  //                 configuration, which is exactly KeepAwake's "系统全局" line (the two master switches, with
  //                 no way to delete the entry they live in). The feature must refuse a remove of a locked
  //                 index as well: a missing button is a courtesy, and a page is not a gatekeeper.
  //
  //                 ⚠️ WHY THE FEATURE DECIDES WHICH FIELD: only the feature knows which of its fields means "this
  //                 item is in use" (a rule's switch is not a profile's, and a third feature may have none). The
  //                 panel is told an id and draws a switch; it is not asked to recognise one.
  //
  //                 ⚠️ WHY THIS IS PART OF THE CONTRACT RATHER THAN THE FEATURE'S OWN WIDGET. Some settings are
  //                 about the world OUTSIDE the page: "capture the control I click on next" cannot be typed,
  //                 because the answer is a live window, a control class and a process name that exist only
  //                 while the user is pointing at them. The page cannot invent such a button -- it would be the
  //                 panel knowing one feature's vocabulary, which is the mistake §3.10 records -- so the
  //                 feature declares it and the panel draws it, exactly as it does for a slider.
  //
  //                 A button is disabled while the feature is switched off, like every other control in the
  //                 group; the feature still validates, because a page is not a gatekeeper.
  //
  //                 `waiting` -- OPTIONAL, and it is how an action that CANNOT ANSWER YET says so. A STRING
  //                 naming what is being waited for (the panel treats it as opaque text and draws it nowhere):
  //                 while it is there the panel keeps asking for the controls and does not treat what it holds
  //                 as final; the moment it is gone, the document in hand is the answered one.
  //
  //                 ⚠️⚠️ WHY THE FEATURE HAS TO SAY IT RATHER THAN LET THE PAGE WORK IT OUT. "capture" is the
  //                 action this exists for: the feature waits for a click in ANOTHER program, so the panel can
  //                 only poll. The first version of that poll decided the wait was over when the document
  //                 CHANGED -- and the document changes for reasons that have nothing to do with the capture
  //                 (the host answers every `listOp` with a snapshot, and the page re-reads after it). The
  //                 result was the user's report: "捕获事件进行时，鼠标点击后结果要马上给到参数页，目前没有，
  //                 要等到点击新建的规则条才会出现" -- the panel had stopped watching. The mirror image is
  //                 just as real: a capture that fills in exactly what was already there changes nothing, so
  //                 that version would poll forever. Both come from the same guess, and `waiting` replaces it
  //                 with a fact.
  //
  //                 ⚠️ AND WHILE THE PANEL ONLY POLLS WHEN IT ARMED THE ACTION ITSELF, IT IS TOLD. A page
  //                 reopened in the middle of a wait sees `waiting` in the document it loads and starts
  //                 polling again -- the feature is the one that knows the wait is still on, so the feature is
  //                 what the page asks.
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

  // SET ONE CONTROL. `path` names it; `value` is always TEXT, and the feature parses what it expects.
  //
  // ⚠️ TEXT FOR A SLIDER TOO, AND THAT IS A DELIBERATE SIMPLIFICATION RATHER THAN A LOSS. The field used to
  // be a `double`, which could not carry a pattern, a name, a hotkey or a chosen option -- so three of the
  // five control types could not be set at all, and the two that could would have needed a SECOND setter
  // beside this one. Two setters for one job is two implementations of "set a control", which is the thing
  // this project's own rules forbid; and the transport was already text (the panel's IPC carries a string,
  // and the host was converting it to a number on the way through, throwing away what the page had sent).
  // A feature that wants a number calls `atof`, which is what a settings parser does anyway.
  //
  // `path` is:
  //   * a control's id               -- "glide", or "hotkey";
  //   * a field inside a group item  -- "rules[3].process_pattern".
  //
  // ⚠️ SO A CONTROL ID MAY NOT CONTAIN '[' OR '.'. That is the whole of the syntax, it needs no parser
  // library, and a feature reads an index with `atoi` and the field id by taking the rest.
  //
  // Called for every slider move, so it must be cheap: write the value into memory and report. Persisting is
  // `saveSettings`'s job. Returns 1 if the value was accepted, 0 to REJECT it -- and a rejection is shown by
  // the panel re-reading the control, so the page ends up displaying what the feature really has.
  int (*setControl)(const char *path, const char *value);

  // STRUCTURAL EDITS TO A LIST-LIKE CONTROL (optional; a feature with none leaves this null).
  //
  // `id`    the control, as it appeared in settingsJson.
  // `op`    structural, never "set a value" (that is `setControl`):
  //           "add"        -- append a new item. For `list`, `value` is the text; for `group`, the feature
  //                           supplies the new item's defaults and `value` is ignored.
  //           "remove"     -- drop the item at `index`.
  //           "move-up"    -- move the item at `index` one place earlier.
  //           "move-down"  -- ... one place later.
  //           "move"       -- move the item at `index` to the position in `value` (a decimal number). This is
  //                           what a DRAG sends: the page knows where the row was grabbed and where it was
  //                           dropped, and a move of several places as a chain of "move-up" would be one message
  //                           and one full re-read of the page PER PLACE -- the list would visibly walk there.
  //                           (`value` carries the destination because the op needs two numbers and the call has
  //                           one numeric slot; it is text like every other value on this wire.)
  //           and any op the feature declared in its own `actions` array (see settingsJson). The panel passes
  //           it through unchanged: a feature that declares a button is the only thing that can interpret it,
  //           which is why the panel never has to know what the op means.
  //         ⚠️ REORDERING EXISTS BECAUSE ORDER IS MEANING for a rule list: the engine takes the first match, so
  //         two rules that could both match are decided by which comes first. A page that could only add and
  //         remove would force the user to delete and re-enter a rule to reorder it.
  // `value` the text to add, trimmed by the panel. THE FEATURE decides what is acceptable -- it lower-cases,
  //         turns a path into a bare file name, refuses duplicates, or refuses outright. The panel does not
  //         validate, because it does not know what the list means.
  // `index` the item acted on.
  //
  // Returns 1 if it changed, 0 if it did not (refused, duplicate, index out of range). The panel re-reads the
  // control either way, so a refusal shows up as "the row did not appear".
  int (*listOp)(const char *id, const char *op, const char *value, int index);

  // THE CONTROLS THIS FEATURE PUTS IN THE QUICK PANEL (optional; a feature that wants none leaves this null,
  // and so does every feature written before ABI 12). Fills `out` with at most `max` items and RETURNS HOW MANY
  // IT HAS IN TOTAL -- the two-part answer Win32 uses everywhere, so a feature with more items than fit is not
  // an error and is not silently cut to nothing: the host draws what fits and knows there was more. Returning 0
  // means "this feature has nothing to put there", and the host then draws no section for it rather than an
  // empty one.
  //
  // ⚠️ THE VALUES MUST BE READ FRESH ON EVERY CALL, from whatever the feature is actually using. This is not a
  // form the user is filling in: the panel shows the feature's CURRENT state, the settings page can change it
  // behind the panel's back, and the wheel handshake (or a feature's own worker) can change it too. An item
  // list built once at init would be a picture of the settings as they were when the program started.
  //
  // ⚠️ IT IS CALLED WHEN THE PANEL OPENS AND AGAIN AFTER EVERY CHANGE THE USER MAKES IN IT, WHICH IS THE POINT:
  // a feature that clamps, snaps to a step or refuses a value outright gets to answer with what it really has,
  // and the knob jumps to the clamped position instead of showing what the mouse asked for. The same rule as
  // the settings page (see setControl): THE PAGE IS NOT THE GATEKEEPER.
  //
  // ⚠️ AND NOTHING HERE ALLOCATES OR BLOCKS either -- it runs on the host's UI thread while a window is being
  // painted. It is not the wheel path, so a slow answer costs a stutter rather than a lost wheel, but there is
  // still no reason to read a file per frame.
  //
  // ⚠️⚠️ A CONTROL REACHES THE PANEL ONLY IF THE USER MAPPED IT, AND THIS IS THE RULE THE WHOLE THING TURNS
  // ON. It is the user's own: "插件自己的控件要明确有开关映射到快速面板，才给". A feature does NOT put a control
  // in the flyout because it judges the control useful there -- it puts it there because the user turned on a
  // switch on that feature's own settings page, one switch per control. So:
  //
  //   * THE DEFAULT IS OFF. A fresh install shows nothing of a feature's own in the panel;
  //   * THE SWITCH IS AN ORDINARY `bool` CONTROL the feature describes in settingsJson, so the panel draws it,
  //     the settings file keeps it and `setControl` sets it -- like every other control. The host knows nothing
  //     about it and has no field for it;
  //   * AND `quickItems` SIMPLY DOES NOT SEND an item whose switch is off. Not "sends it and the host hides
  //     it": the panel can draw only what it was given, and hiding would leave the decision inside the host,
  //     which has no business knowing which of a feature's controls are the interesting ones.
  //
  // ⚠️ WHY IT IS A PERMISSION RATHER THAN A GOOD IDEA. The flyout is small, and the user reaches into it without
  // looking -- every row in it is a row that has to be read past. Which of a feature's controls deserve that
  // place is the user's call, exactly as the two host switches make the same call for the panel as a whole
  // (`quickcompact` / `quickown` in hostconfig.h).
  //
  // ⚠️ `id` IS A `setControl` PATH AND NOTHING ELSE. That is what makes the two surfaces one control: a knob
  // dragged in the flyout sends exactly what the settings page's slider sends, to the same place.
  int (*quickItems)(ApexQuickItem *out, int max);

  // Persist whatever `setControl` has accumulated. Called when the panel closes and when the host
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
// Bit 2: THE FEATURE IS HOLDING SOMETHING ON THE USER'S BEHALF RIGHT NOW, and they cannot see it. KeepAwake
// holds a power request (the machine will not sleep); a future feature might hold a capture open. The host
// shows this IN THE TRAY MARK ITSELF -- the middle bar of the icon is drawn in the hold ink, and the tooltip
// gains a line -- so "why will my PC not sleep?" has an answer that costs the user a glance rather than
// opening anything, and so the answer is still there minutes later.
//
// ⚠️ IT IS A STATE, NOT AN EVENT: report it for as long as it is true. The host watches for the edge.
//
// ⚠️ AND IT HAS TWO DEGREES, because the mark does. This bit is the ordinary one (the mark's bar in the
// ordinary hold ink); HOLD_HARD below is the stronger one. A feature that reports BOTH is telling the host
// "the stronger kind", which is what the bit below is for.
#define APEX_FEATURE_USER_VISIBLE 4u
// Bit 3: THE HOLD ABOVE, ONE DEGREE STRONGER -- the user is being kept from something they WOULD notice if it
// were wrong, not merely from something invisible. KeepAwake's case is the only one so far: "the machine will
// not sleep" is invisible, "the screen will not turn off" is the kind of thing that is either wanted or
// infuriating, and the user asked for the two to be told apart at a glance ("托盘图标再加一种状态: ... 保持唤醒
// （绿色）, 保持唤醒+防止熄屏（红色）"). The host draws the stronger ink for it and the ordinary one otherwise.
//
// ⚠️ SET BOTH BITS, OR THIS ONE ALONE -- the host asks "is anything held" and "is it the stronger kind"
// separately, so HOLD_HARD without USER_VISIBLE still draws the stronger mark. Setting only USER_VISIBLE is the
// ordinary hold. A feature may therefore send the two degrees as "4" and "4|8" and never think about it again.
//
// ⚠️ THIS NUMBER IS REUSED, AND THE VERSION BUMP IS WHAT MAKES THAT SAFE. Bit 3 was APEX_FEATURE_TELL_USER --
// "tell the user about this in words, once" -- which was built, tried and removed by the user inside version 8
// ("弹出提示这个可以去掉"), so NO SHIPPED DLL EVER SET IT. It is reused rather than left dead because a bit is
// the scarce resource here; and a DLL built against 8 is refused outright by a 9 host (the loader requires an
// exact match), so no host will ever read a bit 3 that meant "show a notification".
#define APEX_FEATURE_HOLD_HARD 8u

// The one symbol a feature DLL must export.
typedef const ApexFeature *(*ApexFeatureEntryFn)(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // APEX_ABI_H
