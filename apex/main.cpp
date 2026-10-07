// ---------------------------------------------------------------------------
// APEX -- the host.
//
// 端，体之无序而最前者也。   -- 《墨经》
//
// WHAT THIS PROGRAM IS: a shell that holds features. It owns everything that has to be true for the
// whole program -- the one low-level hook, the one clock, the one injection path, the tray icon and the
// settings panel -- and each feature owns exactly one behaviour. Adding a feature is dropping a folder into
// Plugins\, not editing this file.
//
// THE DECISION ORDER FOR ONE WHEEL, and it is deliberately strict (the rule is in decision.h; this is its
// shape):
//
//   1. is this our own injected wheel?             yes -> let it through untouched (no self-loop)
//   2. is another handler responsible?             yes -> let it through (the REAPER plugin, or an
//                                                         unreadable process: UNKNOWN means taken)
//   3. is there any feature that could deliver?     no -> let it through (nothing to replace it with)
//   4. ask each enabled feature, in order.       the first that says "mine" wins, the original message
//                                                is swallowed, and its motion becomes the only effect.
//
// Rule 4 is "first feature wins" rather than "let every feature act", because two features moving the
// same wheel would add their travel together and neither would be able to predict the result. A feature
// that wants a modified wheel can say so; one that ignores modifiers should NOT swallow them, and that is
// the feature's own call to make (see onWheel in abi.h).
//
// ⚠️ TWO STEPS THAT USED TO BE IN THIS LIST ARE GONE, and the order above is what the code does now: there is
// no host-wide master switch (a feature owns its own enable -- the host's `off` list), and no host-side
// blacklist (each feature owns its list and declines the wheel itself -- see ApexFeature::listOp). A listed
// program still gets its wheel untouched; what changed is who decides.
//
// ---------------------------------------------------------------------------

#include "abi.h"
#include "host.h"
#include "hostconfig.h"
#include "icons.h"
#include "loader.h"
#include "paths.h"
// ⚠️ FOR ONE CONSTANT, AND THAT IS BETTER THAN A SECOND COPY OF IT: `kMaxOwnPerFeature` is how many quick-panel
// items one feature may hand over, and it is defined where the flyout's model is built (apex/quickpanel.h).
// `QuickBlocks` needs the same bound to ask a feature for its items, and a private copy here would be a second
// answer to "how many is that" -- the kind of duplicate this project has been bitten by.
#include "quickpanel.h"
#include "traymark.h"  // the mark with its middle bar in orange (see icons.h)
#include "settings_ipc.h"
#include "decision.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

using namespace apex;

namespace {

HostConfig g_cfg;
// ⚠️ WHETHER THIS MACHINE HAD AN apex.ini WHEN WE STARTED -- which is not the same question as "is anything
// configured", and it is the one the defaults hang on. The user's rule (2026-10-07): a fresh install starts with
// EVERY feature switched off and both quick-panel halves off ("用户全新用上时，什么功能也不开，让用户按需打开"),
// while a machine that already has a file keeps every choice in it ("已经有配置过的用户不影响"). So "no file" is
// the only thing that may act on a default, and it is recorded once, by LoadHostConfig, before anything reads it.
bool g_cfgFileExisted = false;
Loader g_loader;
HWND g_wnd = nullptr;

// The frame rate the model is asked for. 4 ms is the plugin's shipping value and what makes a notch come
// out as ~0.75 deltas per frame instead of a visible staircase. It is a named constant because the whole
// reason the app exists is that measurement.
double g_frameMs = 4.0;

// The panel's own process. Settings live in a SEPARATE process on purpose: the hook is in the OS input
// path, and a panel that hangs, crashes, or is killed must not be able to take smoothing down with it.
// (See settings_host.cpp for the other side of this.)
PROCESS_INFORMATION g_panel = {0};

// ---- logging ----
// Beside the exe and truncated per run; the first question about a host is always "which feature took
// which wheel", and that has to be readable without a debugger.
FILE *g_log = nullptr;
CRITICAL_SECTION g_logLock;
// ONE LINE PER WHEEL, for the cases where "which wheel went where" has to be readable without a debugger.
// OFF unless the environment says otherwise -- per-wheel logging at 250 Hz is a lot of log.
//
// ⚠️ IT USED TO BE `bool g_trace = false;` AND NOTHING ELSE: the branch that reads it was unreachable, so the
// feature existed only as a line of code that could never run. (host_win.cpp already reads a test switch from
// the environment the same way -- see APEX_ACCEPT_INJECTED.)
bool g_trace = GetEnvironmentVariableA("APEX_TRACE_WHEELS", nullptr, 0) > 0;

void LogLine(const char *fmt, ...)
{
  if (!g_log)
    return;
  EnterCriticalSection(&g_logLock);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
  LeaveCriticalSection(&g_logLock);
}

// ---- the feature-facing host services ------------------------------------------------------------

// The frame cache's own clock. Defined further down, and needed by the settings protocol's accessors
// (a blacklist change has to drop the cache and force an immediate re-warm).
extern double g_lastWarmSec;

// ---- the target cache -----------------------------------------------------------------------------
//
// The hook may only DECIDE, and it must decide from something already computed: the cross-process lookups
// behind a target cost up to ~1.9 ms, which is far too long to spend in the OS input callback. The cache is
// refreshed on the frame instead.
//
// ⚠️ THE PUBLICATION IS SEQUENCED, AND THAT IS NOT TIDINESS. The struct used to be assigned in place while
// its "valid" flag stayed up, so a reader could take `pid` from the NEW target and `handler` from the OLD
// one. The combination that matters is a program WITH its own wheel handler being read as handler=ABSENT: the
// decision then lets a feature smooth it, which is exactly the "two handlers driving one view" case
// decision.h calls worse than an unsmoothed wheel. It needs the refresh to land inside the few hundred
// nanoseconds the copy takes -- rare, and rare is not a specification in the input path, where being wrong is
// a visible double-scroll.
//
// THE PATTERN (a seqlock, the standard way to publish a struct without a lock): the writer bumps the sequence
// to ODD while the struct is being rewritten and to EVEN when it is whole again; a reader takes the sequence
// before and after its copy and retries if either the odd bit is set or the two reads differ.
struct TargetInfo
{
  unsigned long pid = 0;
  char exe[64] = {0};
  int handler = APEX_HANDLER_UNKNOWN;
  // THE POINT WAS OVER APEX'S OWN SETTINGS PANEL (see decision.h rule 2). Resolved here, in the frame, for the
  // same reason the handler state is: it needs a window query, and doing it in the frame keeps the hook to a
  // cached read.
  bool ownUi = false;
  bool resolved = false;
  double at = 0.0;
};

volatile LONG g_targetSeq = 0; // odd while a write is in progress (see above)
TargetInfo g_target;
static const double kTargetTtlSec = 3.0;

// A CONSISTENT COPY of the cache, or false. On false the caller must treat the target as UNKNOWN: the cache
// is mid-rewrite and nothing in it is trustworthy for this event. That is the safe direction -- unknown means
// the feature declines and the wheel passes through untouched -- and it costs nothing when it does not
// happen, which is the usual case.
bool SnapshotTarget(TargetInfo *out)
{
  for (int attempt = 0; attempt < 4; ++attempt)
  {
    const LONG s1 = InterlockedCompareExchange(&g_targetSeq, 0, 0);
    if (s1 & 1)
      continue; // a write is in progress
    *out = g_target;
    if (InterlockedCompareExchange(&g_targetSeq, 0, 0) == s1)
      return true; // no write started while we were copying, so this is one whole value
  }
  return false;
}

void RefreshTarget(int x, int y)
{
  TargetInfo t;
  void *root = nullptr;
  if (host::TargetUnderCursor(x, y, t.exe, (int)sizeof(t.exe), &t.pid, &root))
  {
    // OUR OWN UI, identified by its WINDOW CLASS -- the settings panel and the quick panel (the flyout).
    //
    // ⚠️ NOT BY EXE NAME, and not by "is the other handler present". Neither can answer this: the page under
    // the cursor is a hosted browser owned by msedgewebview2.exe, a shared component that could belong to any
    // program, while the top-level window it sits in is the panel's own ApexSettingsWnd (measured; see
    // TargetUnderCursor). And ExternalHandlerState would call the panel "absent", which is true of REAPER
    // handlers and says nothing about whether this is ours.
    //
    // THE CLASS IS THE SAME IDENTITY THE REST OF THE PROGRAM USES for this window -- settings_main.cpp's
    // "already open?" check and OpenSettings()'s "bring it forward" both use FindWindowA on this name. Using
    // anything else here would be a second answer to "where is the panel", which is the shape of bug this
    // project has paid for before.
    //
    // (An earlier draft compared against g_panelWnd, the handle learned from IPC. That works, but only after
    // the panel's first request has arrived, so a wheel in the first moments would be smoothed -- and the
    // handle can go stale. The class name has neither problem and needs no state.)
    //
    // ⚠️ AND THE FLYOUT IS IN HERE FOR A REASON THAT IS NOW LOAD-BEARING: its faders take the wheel (the user
    // asked for it: "推子可以用鼠标滚轮操作"), and a SMOOTHED wheel arrives as dozens of small messages per notch
    // -- each of which the fader would count as a notch of its own, so the value would run away. The class name
    // comes from the file that creates the window rather than being written here a second time.
    char rootClass[64] = {0};
    if (root)
      GetClassNameA((HWND)root, rootClass, sizeof(rootClass));
    t.ownUi = (strcmp(rootClass, APEX_SETTINGS_WND_CLASS) == 0 ||
               strcmp(rootClass, host::QuickPanelWndClass()) == 0);

    char detail[160] = {0};
    t.handler = host::ExternalHandlerState(t.exe, t.pid, detail, (int)sizeof(detail));
    t.resolved = true;
    t.at = host::NowSecondsPublic();
    if (detail[0])
      LogLine("target exe=\"%s\" pid=%lu handler=%s (%s)%s", t.exe, t.pid,
              t.handler == APEX_HANDLER_PRESENT   ? "present"
              : t.handler == APEX_HANDLER_ABSENT ? "absent"
                                                 : "unknown",
              detail, t.ownUi ? " [our own UI: wheels pass]" : "");
  }
  else
  {
    t.pid = 0;
    t.at = host::NowSecondsPublic();
  }
  // ODD, WRITE, EVEN -- in that order, so a reader never sees a half-written struct (see the note above).
  InterlockedIncrement(&g_targetSeq);
  g_target = t;
  InterlockedIncrement(&g_targetSeq);
}

// Cheap lookup for the hook: one WindowFromPoint and a comparison.
//
// ⚠️ IT READS THE CACHE THROUGH SnapshotTarget, and that is why: everything below decides from ONE whole
// value. Reading `pid` for the comparison and then `exe`/`handler` for the answer -- which is what this did --
// can mix two targets, and the feature would then smooth a program whose handler state was never checked.
int TargetAt(int x, int y, ApexTarget *out)
{
  TargetInfo t;
  if (!SnapshotTarget(&t))
    return 0; // the cache is mid-rewrite: unknown, so nothing may act on it
  if (t.pid == 0)
    return 0;
  POINT pt = {x, y};
  HWND w = WindowFromPoint(pt);
  if (!w)
    return 0;
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (pid != t.pid)
    return 0; // the cursor moved to another program: unknown, so let through
  if (out)
  {
    out->pid = t.pid;
    memcpy(out->exe, t.exe, sizeof(out->exe));
    out->handlerState = t.handler;
  }
  return 1;
}

void HostInjectDeltas(double deltas)
{
  // THE CARRY LIVES HERE, at the boundary, because a wheel carries whole deltas and the model does not.
  // Rounding per frame instead would quietly throw away the slow end of every glide -- which is most of
  // the smoothness this program exists to provide.
  static double carry = 0.0;
  carry += deltas;
  const double whole = (carry < 0.0) ? -floor(-carry) : floor(carry);
  if (whole == 0.0)
    return;
  carry -= whole;
  host::InjectQueuePush((int)whole);
}

// The feature whose call is in progress, so a feature can ask the host where its own folder is without
// having to pass its id back for every call. Set for the duration of each call into a feature -- including
// init(), which is where a feature reads its settings and therefore asks first.
const char *g_currentFeatureId = nullptr;

void HostLog(const char *text)
{
  if (text)
    LogLine("  [feature] %s", text);
}

int HostFeatureDir(char *out, int outSize)
{
  return g_currentFeatureId ? (FeatureDir(g_currentFeatureId, out, outSize) ? 1 : 0) : 0;
}

// Defined further down, beside the other ABI wrappers. Declared here because InitHostApi installs it and
// comes first -- the same reason the tray helpers are declared before their definitions.
int HostFeatureEnabled(const char *id);

// RAII for the id: every call into a feature brackets it with this, so an early return inside the feature
// cannot leave the host pointing at the wrong one.
struct FeatureScope
{
  const char *prev;
  explicit FeatureScope(const char *id) : prev(g_currentFeatureId) { g_currentFeatureId = id; }
  ~FeatureScope() { g_currentFeatureId = prev; }
};

ApexHost g_hostApi = {};

// ---- "something worth showing just happened" ------------------------------------------------------
//
// A feature calls ApexHost::activity() from its own onWheel, which runs IN THE OS INPUT PATH.
//
// ⚠️ IT RUNS ON THIS THREAD -- the main one. A low-level hook is called on the thread that installed it (see
// host_win.cpp), and that is WinMain's, the same thread this window procedure and the timer live on. So no
// cross-thread machinery is needed here and, in particular, PostMessage is just an enqueue: it does not wait
// for the panel, which may be busy, and it cannot block on it. (The earlier note here said the opposite --
// "no sends" -- which was written before that was checked; the honest version is "one post, on the edge".)
//
// TWO PATHS OUT, for two different reasons:
//
//   * THE EDGE (idle -> active) is posted IMMEDIATELY. That is the moment the user feels: they turn the wheel
//     and the picture has to move. Waiting for the next tick costs up to kActivityNotifyMs of pure lag on
//     exactly the event that is being watched.
//   * THE REST IS COALESCED by the timer. A sustained roll, or a free-spinning device reporting hundreds of
//     events a second, would otherwise mean one cross-process script call per event -- and each of those is an
//     IPC into the browser process. The panel cannot show more than a frame's worth anyway.
//
// The count is CLAIMED with an exchange on both paths, so an event is delivered exactly once whichever path
// gets to it first (the edge and the tick can race, and a plain read-then-write would double-count).
volatile LONG g_activity = 0;    // every call a feature has made (only deltas matter)
volatile LONG g_activitySent = 0; // how much of that already reached the panel
UINT g_activityMsg = 0;          // the registered message; 0 = registration failed
UINT g_stateMsg = 0;             // "a fact the panel draws has changed" -- registered, so both sides agree by name
UINT g_panelShowMsg = 0;         // "the user really did ask for the settings" -- for a warmed-up panel
UINT g_panelDropMsg = 0;         // "the menu closed without asking" -- the warm-up is not needed
#define APEXWM_ACTIVITY_TIMER 1  // the host window's low-rate timer id
#define APEXWM_SAVE_TIMER 2      // writes the settings shortly after an edit settles (see SettingsTouch)
#define APEXWM_STATE_TIMER 3     // watches the facts that can change while the panel is open (see NotifyStateChanges)
#define APEXWM_TRAY_CLICK_TIMER 4 // tells a single tray click from the first half of a double click (see WndProc)
#define APEXWM_FLYOUT_TIMER 5     // TEST-ONLY: shows the quick panel once at start-up (see FlyoutSelfTest)

// ---------------------------------------------------------------------------
// SETTINGS ARE WRITTEN SOON AFTER THEY CHANGE, not only on the way out.
//
// WHY THIS EXISTS, and it is worth stating because "save on exit" sounds adequate: the exit path is a CLEAN
// exit (WM_CLOSE -> DestroyWindow), and the ways this program actually ends are not clean. A `taskkill /F`,
// a crash, a machine that is reset, or the user simply leaving it running for a week all skip it -- and what
// is skipped is every parameter the user tuned, which is the one thing in this folder that cannot be
// rebuilt. Measured, that is exactly what happened: settings were adjusted, the process was killed, and the
// next start came up on the defaults.
//
// The debounce is what keeps this from becoming a file write per mouse move. A slider reports EVERY position
// (`setFeature` is sent per pixel -- see the panel's rangeRow), so a save on each one would be hundreds of
// writes for one drag. Instead each edit pushes the timer back, and the write happens once the user has
// paused for a moment. A drag is one write; a stream of edits is one write per pause.
//
// THE DELAY IS SHORT ON PURPOSE. It is not a performance knob: it only has to outlast a drag, and every
// millisecond of it is a millisecond of editing that a forced kill could still lose.
#define APEXWM_SAVE_DELAY_MS 1000

// The panel's window, once it has said who it is. See SettingsPanelSeen below.
HWND g_panelWnd = nullptr;

void HostActivity()
{
  const LONG now = InterlockedIncrement(&g_activity);
  if (now != 1 || !g_activityMsg || !g_panelWnd)
    return; // not the edge, or nowhere to send it: the timer will pick it up
  const LONG claimed = InterlockedExchange(&g_activitySent, now);
  if (now > claimed)
    PostMessageA(g_panelWnd, g_activityMsg, (WPARAM)(now - claimed), 0);
}

// "REAPER, Lertaro" -- the engines in the order the platform layer gave them, for the log lines below.
//
// ⚠️ ONE PLACE SPELLS THIS, and that is not tidiness: the ABI wrapper and the change watcher both name the same
// list, and two spellings of it is how a log comes to disagree with what the panel was actually told. The ORDER
// is part of what is being reported (see ApexHost::activeEngines), so it is printed, not sorted.
//
// ⚠️ THE NAMES COME OFF `ApexEngine`, not out of a table here: a program added to the host's engine table must not
// need this file edited too (see ApexEngine in abi.h).
static void EnginesText(const ApexEngine *engines, int n, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  int off = 0;
  for (int i = 0; i < n; ++i)
  {
    const char *name = engines[i].name[0] ? engines[i].name : "?";
    const int left = outSize - off;
    if (left <= 1)
      break;
    const int wrote = _snprintf(out + off, (size_t)left, "%s%s", i ? ", " : "", name);
    if (wrote <= 0)
      break;
    off += wrote;
  }
}

// WHICH EXTERNAL SMOOTHING ENGINES ARE RUNNING? The ABI's answer, with a log line -- see the note where it is
// installed into the host API for why the logging belongs HERE and not in the platform layer.
//
// ⚠️ THE LOG PRINTS THE ORDER, because that is the part a reader cannot recover from the set alone: "Lertaro,
// REAPER" and "REAPER, Lertaro" are the same set and two different sentences.
int HostActiveEngines(ApexEngine *out, int max)
{
  const int n = host::ActiveEngines(out, max);
  char names[96] = {0};
  EnginesText(out, n, names, (int)sizeof(names));
  LogLine("engines: %s", n > 0 ? names : "none running (or not confirmed) -- no note");
  return n;
}

// Watch the facts that can change while the panel is open, and tell it when one does. Called from the timer,
// on the main thread.
//
// ⚠️ ONLY ON A CHANGE. Polling a value and posting it every tick would make the panel redraw its page once a
// second forever, which would throw away the user's scroll position and re-send every control for nothing --
// the same mistake the "answer every command with a snapshot" rule was fixed for. The post happens on the
// EDGE, so a steady state costs one comparison per tick and nothing else.
//
// The comparison is kept here rather than read back from the feature, because the thing being watched must not
// depend on any feature being loaded: the panel may have no plugin at all.
void NotifyStateChanges()
{
  // ⚠️ `-1` MEANS "NOTHING OBSERVED YET", AND THE FIRST TICK ALWAYS POSTS. Without it the panel would open
  // showing the state from before it existed: the host observes the value from the moment it starts, so a
  // REAPER that was ALREADY RUNNING when the host started is not a change and would never be announced -- the
  // note would be missing until the user happened to start REAPER a second time. (Found by running the
  // stand-in BEFORE the host: the host logged nothing at all, correctly, and the panel stayed blank.)
  //
  // The cost of always posting once is one message at start-up. The cost of not doing it is a state the user
  // can see on screen never being sent.
  static int lastFingerprint = -1; // -1 = nothing reported yet
  // ⚠️ THE ENGINE SET **AND THE ORDER INSIDE IT**, because the sentence the panel draws names the engines in that
  // order: a set that stayed the same while the order changed would leave the note showing the old order for
  // ever. Building and comparing this is a couple of dozen arithmetic operations, which is what a two-second
  // timer can afford -- the expensive part is inside ActiveEngines, and only when the set changes (see there).
  ApexEngine engines[4];
  ZeroMemory(engines, sizeof(engines));
  const int count = host::ActiveEngines(engines, 4);
  int fingerprint = count;
  for (int i = 0; i < count; ++i)
    fingerprint = fingerprint * 31 + engines[i].kind;

  // ⚠️ NOTHING IS REMEMBERED UNTIL SOMETHING WAS ACTUALLY SENT, AND THIS IS THE BUG THAT COST A ROUND TRIP.
  // The first version recorded the value and then tried to post -- so on the ticks before the panel had opened,
  // the value was "learned" and the post was dropped on the floor. The panel then connected and never heard
  // anything: there was no CHANGE left to notice, because the host had already, silently, decided it had
  // reported. The result was exactly the symptom the whole feature exists to remove -- a note that only
  // appears after the user does something.
  //
  // So the state is remembered only when the panel was actually told. Until then every tick is "unreported",
  // and the first tick after the panel says hello sends it. (The panel learns where to post by SENDING, which
  // it does once at boot -- see SettingsPanelSeen -- so `g_panelWnd` being set is precisely "there is somebody
  // to tell".)
  if (!g_stateMsg || !g_panelWnd)
    return; // nowhere to send it: treat the value as not yet reported

  if (fingerprint == lastFingerprint)
    return;
  const bool first = (lastFingerprint < 0);
  lastFingerprint = fingerprint;
  char names[96] = {0};
  EnginesText(engines, count, names, (int)sizeof(names));
  // ⚠️ THE MESSAGE KIND KEEPS ITS OLD NAME (`kStateReaper`): it is a KEY the panel acts on -- "re-read the
  // controls, something outside the page changed" -- and renaming it would mean touching the IPC header, the page
  // and the gates for a word. What changed is what the host WATCHES, not what the panel does about it.
  LogLine("state: the engines are %s -- telling the panel%s", count > 0 ? names : "none",
          first ? " (the first report)" : "");
  PostMessageA(g_panelWnd, g_stateMsg, (WPARAM)kStateReaper, 0);
}

// Hand the panel whatever has happened since the last look. Called from the timer, on the main thread.
void NotifyActivity()
{
  if (!g_activityMsg || !g_panelWnd)
    return;
  const LONG now = InterlockedCompareExchange(&g_activity, 0, 0);
  const LONG claimed = InterlockedExchange(&g_activitySent, now);
  if (now <= claimed)
    return; // nothing new, or the edge already took it
  PostMessageA(g_panelWnd, g_activityMsg, (WPARAM)(now - claimed), 0);
}

// (The panel's window handle is remembered by SettingsPanelSeen, declared in settings_ipc.h and defined down
// in the apex::host block at the end of this file -- NOT here: an anonymous namespace has internal linkage, and
// settings_host.cpp has to be able to call it.)



void InitHostApi()
{
  g_hostApi.abiVersion = APEX_ABI_VERSION;
  g_hostApi.structSize = sizeof(ApexHost);
  g_hostApi.targetAt = TargetAt;
  g_hostApi.injectDeltas = HostInjectDeltas;
  g_hostApi.logLine = HostLog;
  g_hostApi.featureDir = HostFeatureDir;
  g_hostApi.activity = HostActivity;
  g_hostApi.activeEngines = HostActiveEngines;
  g_hostApi.featureEnabled = HostFeatureEnabled;
  g_hostApi.hostUser = nullptr;
}

// IS THIS FEATURE SWITCHED ON? The question a feature with its own threads has to ask, because the host's
// switch works by not calling it -- which a worker thread never notices (see ApexHost::featureEnabled).
//
// ⚠️ IT IS READ WITHOUT THE LOADER'S LOCK AND FROM ANY THREAD, so it must stay this small: find the id, read
// the off-list flag. It walks the feature list (at most 16 entries, a pointer comparison each) and calls
// `FeatureOff`, which is a substring-free scan of a short list. That is cheap enough to be asked a few times
// a second from a feature's own loop, and it is NOT cheap enough to be hammered -- a feature that asked it
// per frame would be doing real work per frame, which is its own decision to make and not the host's.
//
// An unknown id answers 0 ("not enabled"): a feature that cannot be found is certainly not one the user is
// running, and answering "yes" would be the dangerous direction (it would let a stale or misspelled id keep a
// worker thread alive).
int HostFeatureEnabled(const char *id)
{
  if (!id || !*id)
    return 0;
  for (int i = 0; i < g_loader.Count(); ++i)
  {
    const LoadedFeature &f = g_loader.At(i);
    if (f.ok && f.api && f.api->id && strcmp(f.api->id, id) == 0)
      return g_cfg.FeatureOff(f.api->id) ? 0 : 1;
  }
  return 0;
}


// ---- the one decision ---------------------------------------------------------------------------
//
// RUNS IN THE OS INPUT PATH. It may only decide from the cache and record; every expensive thing is on
// the frame. The single exception is the features' own onWheel, which the ABI requires to be cheap for
// the same reason.
struct Pending
{
  ApexWheelEvent ev;
};

const LONG kQueueMax = 256;
Pending g_queue[256];
volatile LONG g_qHead = 0;
volatile LONG g_qCount = 0;

// NOTE ON WHAT THE QUEUE IS FOR, since two of its fields were write-only until a review caught it: the feature
// is told about a wheel IN THE HOOK (that is how it decides to take one), so nothing here needs to carry a
// verdict or a time -- what the frame does with a wheel is log it. `swallow`/`t` used to be recorded and never
// read, which reads as "this is where the swallow is decided" to anyone looking for that decision. It is not:
// decision.h decides, in the hook, and the answer is used there.

bool OnWheel(const ApexWheelEvent &ev, void *user)
{
  (void)user;

  // THE DECISION IS IN apex/decision.h, as a pure function over the facts. It lives there for one reason: its
  // failure mode is "scrolling stops working" (it has happened once -- with the switch off, the hook kept
  // swallowing while nothing replaced the notch), and a pure function can be swept exhaustively by a test
  // that runs the REAL code. A COPY of this logic used to live in that probe, which is exactly how a copy and
  // its original come to disagree.
  //
  // What stays here is everything that needs the host: reading the caches, counting the features, and calling
  // into them.
  DecisionInputs in;
  in.injected = ev.injected != 0;
  in.acceptInjected = host::CaptureAcceptsInjected();

  // ⚠️ ONE WHOLE SNAPSHOT OF THE CACHE, and the old comment here claimed that is what this did -- while
  // reading `pid` and `handler` as two separate loads from a struct another thread rewrites in place. Those
  // two loads could straddle a refresh, and the dangerous mix is a program that HAS a wheel handler being read
  // as ABSENT (the cache still holding the previous target's verdict), because then a feature is allowed to
  // smooth it. A failed snapshot means the cache is mid-rewrite: the target is simply UNKNOWN for this event,
  // which is the safe direction (the decision passes and nothing is swallowed).
  {
    TargetInfo t;
    if (SnapshotTarget(&t))
    {
      in.targetKnown = t.pid != 0;
      in.handlerState = t.handler;
      in.ownUi = t.ownUi; // a wheel over our own panel passes -- see decision.h rule 2
    }
  }

  // How many features could actually deliver something: the same three conditions the loop below applies,
  // counted instead of acted on. THIS IS WHAT MAKES THE INVARIANT TRUE -- the rule in decision.h refuses to
  // eat an event when this is zero, because there would be nothing to replace it with.
  in.featuresEnabled = 0;
  for (int i = 0; i < g_loader.Count(); ++i)
  {
    const LoadedFeature &f = g_loader.At(i);
    if (f.ok && f.api && f.api->onWheel && !g_cfg.FeatureOff(f.api->id) &&
        (!f.api->flags || (f.api->flags() & APEX_FEATURE_ENABLED)))
      ++in.featuresEnabled;
  }

  if (DecideWheel(in) == Decision::kPass)
    return false; // let it through untouched

  // THE FEATURES, in load order; the first that says "mine" wins. A feature that declines leaves the message
  // untouched, which is the other half of the invariant: reaching here only means "you may ask", never "eat
  // it".
  int taken = -1;
  for (int i = 0; i < g_loader.Count(); ++i)
  {
    const LoadedFeature &f = g_loader.At(i);
    if (!f.ok || !f.api || !f.api->onWheel)
      continue;
    if (g_cfg.FeatureOff(f.api->id))
      continue;
    if (f.api->flags && !(f.api->flags() & APEX_FEATURE_ENABLED))
      continue;
    // The id is bracketed around the call so the feature can reach its own folder; see FeatureScope.
    FeatureScope scope(f.api->id);
    const int took = f.api->onWheel(&ev);
    if (took)
    {
      taken = i;
      break;
    }
  }

  if (taken < 0)
    return false;

  // Recorded for the frame. The queue is bounded and the oldest is dropped rather than the newest: a
  // wheel produces at most a few messages per frame, so this is a guard against a pathological burst, not
  // a buffer anything expects to fill.
  if ((int)InterlockedCompareExchange(&g_qCount, 0, 0) < (int)kQueueMax)
  {
    const LONG i = (g_qHead + g_qCount) % kQueueMax;
    g_queue[i].ev = ev;
    InterlockedIncrement(&g_qCount);
  }
  return true; // SWALLOW: the host is driving this wheel
}

// ---- the frame ----------------------------------------------------------------------------------
double g_lastTickSec = 0.0;
double g_lastWarmSec = 0.0;
static const double kWarmEverySec = 0.25;

void WarmTargetCache()
{
  const double now = host::NowSecondsPublic();
  if (now - g_lastWarmSec < kWarmEverySec)
    return;
  g_lastWarmSec = now;

  POINT pt;
  GetCursorPos(&pt);
  HWND w = WindowFromPoint(pt);
  DWORD pid = 0;
  if (w)
    GetWindowThreadProcessId(w, &pid);
  // The cache is still good if it is the SAME program and young enough -- decided from one whole snapshot, so
  // `pid` and `at` cannot come from two different refreshes. (A failed snapshot falls through to a refresh,
  // which is the right answer either way: it means a write is in flight, so the value is about to change.)
  {
    TargetInfo t;
    if (SnapshotTarget(&t) && pid == t.pid && (now - t.at) < kTargetTtlSec)
      return;
  }
  RefreshTarget(pt.x, pt.y);
}

void Tick(void *)
{
  const double now = host::NowSecondsPublic();
  if (g_lastTickSec == 0.0)
    g_lastTickSec = now;
  double dt = now - g_lastTickSec;
  g_lastTickSec = now;
  if (dt <= 0.0)
    return;
  if (dt > 0.25)
    dt = 0.25; // a stalled frame must not fling the receiver

  // Drain the wheel queue. The feature was already told about these in the hook (that is how it decided
  // to take them); this is only where the bookkeeping they imply is settled.
  LONG n = InterlockedCompareExchange(&g_qCount, 0, 0);
  while (n > 0)
  {
    const LONG h = InterlockedCompareExchange(&g_qHead, 0, 0);
    const Pending p = g_queue[h % kQueueMax];
    InterlockedIncrement(&g_qHead);
    InterlockedDecrement(&g_qCount);
    if (g_trace)
      LogLine("wheel delta=%+5d at %d,%d", p.ev.delta, p.ev.x, p.ev.y);
    n = InterlockedCompareExchange(&g_qCount, 0, 0);
  }

  // THE FEATURES' FRAMES. Only a feature that says it is ACTIVE is asked: ticking every feature at 250 Hz
  // while nothing is moving would be the whole program's cost for no effect at all.
  for (int i = 0; i < g_loader.Count(); ++i)
  {
    const LoadedFeature &f = g_loader.At(i);
    if (!f.ok || !f.api || !f.api->tick || !f.api->flags)
      continue;
    if (g_cfg.FeatureOff(f.api->id))
      continue;
    const unsigned fl = f.api->flags();
    if ((fl & APEX_FEATURE_ENABLED) == 0 || (fl & APEX_FEATURE_ACTIVE) == 0)
      continue;
    // The id is bracketed around the call so the feature can reach its own folder; see FeatureScope.
    FeatureScope scope(f.api->id);
    const double out = f.api->tick(dt);
    if (out != 0.0)
      HostInjectDeltas(out);
  }

  if (g_qCount == 0)
    WarmTargetCache();
}

// ---- the tray -----------------------------------------------------------------------------------
//
// THE TRAY SPEAKS THE USER'S LANGUAGE, resolved here rather than taken from the panel: the tray exists
// whether or not the panel has ever been opened, and a menu that is only correct after visiting the
// settings would be the wrong way round. `lang = auto` follows the system, an explicit choice overrides it,
// and both are the SAME rule the panel applies -- the two must not be able to disagree, since the user sees
// them side by side.
//
// WIDE APIs, NOT ANSI. The strings are UTF-8 in this file, and the A variants of these calls interpret
// their argument in the process's ANSI codepage -- on a non-Chinese Windows that turns 滑动滚轮 into
// mojibake. The W variants take UTF-16, so every string goes through one conversion (U8) at the point of
// use and nothing downstream has to know about codepages. (The plugin's notes record the same trap.)
#define IDM_SETTINGS 40102
#define IDM_OPEN_DIR 40103
#define IDM_SHOW_LOG 40104
#define IDM_QUIT 40105
#define APEXWM_TRAY (WM_APP + 1)

// "TaskbarCreated" is broadcast by the shell when Explorer starts or restarts. It has no fixed message
// number, so it is registered at run time; the zero returned if that fails is harmless because the message
// then simply never arrives (the tray icon would keep working until Explorer next restarts).
UINT g_taskbarCreated = 0; // set in WinMain; see the note there

NOTIFYICONDATAW g_tray = {};
bool g_trayUp = false;

// THE ICON IDS LIVE IN icons.h, shared with the .rc files -- see that file for the bug that put them there.
// (In short: they used to be `#define`d HERE, where the RESOURCE COMPILER could not see them, so the
// artwork was registered under string names and `MAKEINTRESOURCE(IDI_APEX_DARK)` found nothing at all.)

// UTF-8 -> a static UTF-16 buffer. One buffer is enough because these are only ever used to build a string
// for a single call; the call copies it (the shell and the menu both do), so nothing holds the pointer.
const wchar_t *U8(const char *utf8)
{
  static wchar_t buf[512];
  buf[0] = 0;
  if (utf8)
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, (int)(sizeof(buf) / sizeof(buf[0])) - 1);
  return buf;
}

// Is the interface to be Chinese? `auto` resolves by asking Windows for the user's preferred UI language,
// which is the same answer the panel gets (both call into system_win.cpp), so the two cannot disagree.
//
// ⚠️ IT IS DEFINED ONCE, IN apex::host, even though this file is its only caller today. It has a SECOND caller
// the moment the quick panel exists (quickpanel_win.cpp draws feature names and section titles), and a second
// copy of "which language are we in" is how a flyout ends up in one language above a tray icon in the other.
// (See host.h: the tray's strings and the flyout both resolve through this.)
// (The definition itself lives below, with the other apex::host functions -- it reads g_cfg, which is
// file-scope here.)

// Every string the tray can show, in both languages. Chosen by UiIsChinese() at the moment of use rather
// than cached, so a language change takes effect on the next menu without a restart.
//
// ⚠️ THERE IS NO "Enabled / Paused" ITEM ANY MORE, and no tooltip that says either. Apex had a master switch
// (saved) and a runtime toggle (the tray), and both are gone: enabling is a FEATURE's own business, and a
// user who wants to stop smoothing turns the feature off in its own page. Two places to say "off" was one
// place too many -- see apex/decision.h for what replaced them (nothing: the rule's last step covers it).
struct TrayText
{
  const char *tip, *settings, *openDir, *showLog, *quit;
  // Appended to the tooltip while a feature is holding something on the user's behalf (see
  // APEX_FEATURE_USER_VISIBLE). Short on purpose: the tooltip is one line and the mark already says
  // "something"; this says WHAT. TWO LINES, because the mark has two working colours and a colour on its own
  // is not readable -- the tooltip is where the degree is spelled out (see FeatureHoldLevel and icons.h).
  const char *holding;
  const char *holdingHard;
};

const TrayText kTrayEn = {
    "running", "Settings...", "Open the Apex folder", "Show log...", "Quit",
    " -- keeping the machine awake", " -- keeping the machine awake, screen stays on"};
const TrayText kTrayZh = {
    "运行中", "设置...", "打开 Apex 文件夹", "查看日志...", "退出",
    " · 保持唤醒", " · 保持唤醒 · 屏幕常亮"};

// ⚠️ THE HOST WRITES THESE WORDS, AND IT IS A DEBT WORTH NAMING. The degree is all the host is told (a bit);
// the sentence is the host describing the strongest hold it knows about, which today is KeepAwake's. If a
// second feature ever holds hard, this line is where "the host knows one feature's vocabulary" becomes real,
// and the fix is for the ABI to carry the tooltip text with the bit -- not for the host to guess better.
const char *HoldLine(const TrayText &t, int level) { return level >= 2 ? t.holdingHard : t.holding; }

const TrayText &TrayStrings() { return host::UiIsChinese() ? kTrayZh : kTrayEn; }

// Which of the two marks the CURRENT THEME calls for -- the effective one, not the system's: a pinned
// light/dark in the settings wins over the system, exactly as it does for the page and the caption.
//
// The theme question and the mark question are two separate rules, each with one home: `ThemeResolvesLight`
// (hostconfig.h) resolves the appearance, and `ApexMarkForAppearance` (icons.h) turns it into a resource id.
// Neither is written out again here -- see icons.h for why that matters.
int TrayIconId()
{
  return ApexMarkForAppearance(ThemeResolvesLight(g_cfg.theme, host::SystemIsLightTheme()));
}

// The mark for a GIVEN id. The id is a parameter rather than chosen here, and that is on purpose: the
// caller can then report the same number it loaded. An earlier version had this function pick its own id
// while the caller computed one separately for the log line, so the log could describe a different mark
// than the one that was handed to the shell -- the same "the evidence is about something else" failure
// that let this bug survive a whole session.
HICON LoadMark(int id)
{
  // Windows asks for a 16x16 icon in the tray and a 32x32 on a scaled display; LoadImage picks the closest
  // size out of the multi-size .ico, which is why the file carries all of them. LR_SHARED is deliberately
  // NOT used: it would hand back the same shared handle for both variants, and the two are different
  // resources -- the whole point is to swap between them.
  //
  // NULL IS A POSSIBLE ANSWER and the callers check it. It is what happens when the resource is not in this
  // build at all, which is exactly the failure that went unnoticed for a whole session: the ids lived in
  // main.cpp where the resource compiler could not see them, so the second mark was never registered.
  return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), IMAGE_ICON,
                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
}

// The tray's caption and icon, from the current state. Called on every change that can affect either --
// the language and a system theme change.
//
// ⚠️ THE LOG LINE DESCRIBES THE LOAD, NOT THE INTENTION. It used to print the mark that had been SELECTED
// ("icon=dark-mark"), which reads as proof that the switch happened while proving nothing of the kind:
// LoadImage was returning NULL for that id, the shell was never given an icon, and this line said
// everything was fine. So the id that is logged is the id that was PASSED TO LoadImage, and the load's own
// result is reported beside it. The two now come from one call, not from two calculations that could drift.
// ---- what the features are holding ---------------------------------------------------------------
//
// A FEATURE CAN BE DOING SOMETHING THE USER CANNOT SEE (KeepAwake holds a power request). How MUCH it is
// holding is asked once a second, from the same timer that watches REAPER, and the answer is shown by THE MARK
// ITSELF: its middle bar is drawn in the ink for that degree (traymark.h). See APEX_FEATURE_USER_VISIBLE and
// APEX_FEATURE_HOLD_HARD in abi.h.
//
// 0 = nothing is held (the plain mark), 1 = an ordinary hold (GREEN), 2 = the stronger kind (RED).
//
// ⚠️ THE STRONGEST ANSWER WINS, and that is the whole of the rule: two features holding, or one feature that
// reports the stronger bit, all end in one mark, and a mark cannot say two things at once. It is the same
// direction as the feature's own list logic (the strongest row decides), so the tray and the feature cannot
// disagree about which of them is "the" state.
int FeatureHoldLevel()
{
  int level = 0;
  for (int i = 0; i < g_loader.Count(); ++i)
  {
    const LoadedFeature &f = g_loader.At(i);
    if (!f.ok || !f.api || !f.api->flags)
      continue;
    const unsigned bits = f.api->flags();
    if (!(bits & (APEX_FEATURE_USER_VISIBLE | APEX_FEATURE_HOLD_HARD)))
      continue;
    // The stronger bit on its own is still the stronger hold -- the host asks the two questions separately so
    // that a feature cannot accidentally say "I am holding nothing, but hard" and get a plain mark.
    const int mine = (bits & APEX_FEATURE_HOLD_HARD) ? 2 : 1;
    if (mine > level)
      level = mine;
  }
  return level;
}

// THE MARK WHILE A FEATURE IS WORKING: the same artwork with its middle bar in the ink for `level` -- green
// for an ordinary hold, red for the stronger one. `level` is 1 or 2; 0 means "draw the plain mark", which the
// caller does instead of calling this.
//
// ⚠️ THE LEVEL IS PASSED IN because it IS the difference between the two working marks, and choosing it here
// would put the choice in a second place (see the note on LoadMark above: the id is a parameter for the same
// reason). And it is COMPOSED rather than drawn again: the artwork is the user's, and a second drawing of it
// would be a second thing to keep in step.
HICON LoadMarkActive(int id, int level)
{
  const HICON base = LoadMark(id);
  if (!base)
    return nullptr; // no artwork, no recolouring: the caller keeps the plain mark
  return apex::ComposeActiveMark(base, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), level);
}
void TrayUpdate()
{
  if (!g_trayUp)
    return;
  const TrayText &t = TrayStrings();
  const bool light = ThemeResolvesLight(g_cfg.theme, host::SystemIsLightTheme());
  const int iconId = ApexMarkForAppearance(light);
  // Whether the mark says "working" -- and how hard -- is decided in the same one place the mark is: a feature
  // holding something is not a second kind of tray icon, it is the same icon saying more.
  const int level = FeatureHoldLevel();
  const HICON fresh = level ? LoadMarkActive(iconId, level) : LoadMark(iconId);
  // Logged because NOTHING ELSE can show what the tray was given: it is the shell that displays it, so from
  // outside this process the only evidence is this line. It is what makes "the tray did not change language"
  // distinguishable from "the tray changed and looked the same".
  //
  // The theme in this line is DERIVED FROM THE ID rather than recomputed, so the two halves cannot
  // disagree: the id IS the appearance, now that the mapping is the direct one. (Recomputing it from the
  // settings is how a log comes to say "light theme" next to a mark chosen for a dark one.)
  //
  // ⚠️ AND THE DEGREE IS NAMED, FOR THE SAME REASON: green and red are the two working marks, a reader of the
  // log cannot see the tray, and "+working" (which this line used to say) is exactly the word that cannot tell
  // them apart.
  LogLine("tray: lang=%s icon=%s%s (%s theme, %s) load=%s tip=\"%s%s\"", host::UiIsChinese() ? "zh" : "en",
          iconId == IDI_APEX_DARK ? "dark-mark" : "light-mark",
          level >= 2 ? "+working-hard" : (level == 1 ? "+working" : ""),
          iconId == IDI_APEX_DARK ? "dark" : "light",
          g_cfg.theme == Theme::kAuto ? "following the system" : "pinned in the settings",
          fresh ? "ok" : "FAILED -- the resource is not in this exe; the tray keeps the mark it has", t.tip,
          level ? HoldLine(t, level) : "");
  // The old icon is destroyed only after the shell has taken the new one: freeing it first would leave the
  // tray pointing at a deleted handle for the moments in between. A FAILED LOAD CHANGES NOTHING AT ALL --
  // the previous handle stays in place and is not destroyed, because an icon of the wrong shade is worth
  // more to the user than no icon, and a NULL here followed by DestroyIcon is how a tray ends up drawing
  // freed memory until Explorer next repaints.
  HICON previous = g_tray.hIcon;
  if (fresh)
    g_tray.hIcon = fresh;
  g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  g_tray.uCallbackMessage = APEXWM_TRAY;
  _snwprintf(g_tray.szTip, sizeof(g_tray.szTip) / sizeof(g_tray.szTip[0]), L"Apex -- %ls%ls", U8(t.tip),
             level ? U8(HoldLine(t, level)) : L"");
  // ⚠️ THE SHELL'S ANSWER IS LOGGED, and that is not decoration: this whole line used to report what was
  // LOADED while saying nothing about whether it was ACCEPTED, so "the mark never changed" and "the icon was
  // never taken" looked identical from outside. (The user's report -- "the dot is not there" -- is what
  // happens when only the load half is visible.)
  const BOOL taken = Shell_NotifyIconW(NIM_MODIFY, &g_tray);
  if (!taken)
    LogLine("tray: the shell REFUSED the mark (NIM_MODIFY failed, error %lu) -- it is still showing the old one",
            (unsigned long)GetLastError());
  if (fresh && previous && previous != fresh)
    DestroyIcon(previous);
}

// WATCH THE FEATURES' OWN STATE, once a second, on the timer that already runs (see FeatureHoldLevel).
//
// ⚠️ THE EDGE IS OURS, NOT THE FEATURE'S. A feature reports a STATE ("I am holding this much"), because a
// feature that had to work out its own edges would have to know whether the host was listening and when.
//
// ⚠️ AND THE EDGE IS ON THE DEGREE, NOT ON "HOLDING". Watching a boolean here is what would leave the mark
// green while the feature moved from "the machine will not sleep" to "the screen will not turn off" -- the
// user asked for those two to be told apart, and a level that changed without the flag changing is exactly
// the transition that a boolean edge misses.
//
// The tray is redrawn only when the ANSWER changes -- the same rule as the REAPER note below and for the same
// reason: a NIM_MODIFY every second, for ever, is work the shell does not need and a log that fills up.
//
// (This used to also show a one-line notification the first time a feature started holding something. The user
// removed it after trying it -- "弹出提示这个可以去掉" -- so the mark is the whole signal now, which is also
// why the mark had to be made to work: it is no longer a second channel beside a popup, it is the only one.)
void WatchFeatures()
{
  static int wasLevel = 0;
  const int level = FeatureHoldLevel();
  if (level == wasLevel)
    return;
  wasLevel = level;
  LogLine("tray: the features' hold is now %s -- the mark shows it",
          level == 0 ? "nothing (plain mark)" : (level == 1 ? "an ordinary hold (green bar)"
                                                            : "the stronger hold (red bar)"));
  TrayUpdate();
}
// TEST-ONLY (APEX_NO_TRAY): this host does not put an icon in the tray.
//
// ⚠️ WHY THE PRODUCT HAS A SWITCH FOR THIS. Every gate in the assembly layer starts ITS OWN copy of the whole
// program -- a gate must never experiment on the copy the user is running -- and a delivery runs eighteen of
// them. Each copy used to put an icon in the tray and take it away seconds later, so the user watched their
// tray flicker for four minutes and reported it as "每次部署都反复运行又杀掉（托盘图标反复出现几次）".
//
// ⚠️ AND IT COSTS THE GATES NOTHING, WITH ONE EXCEPTION. No gate reads the tray back -- it belongs to the shell
// (see the note in TrayUpdate) -- EXCEPT check_apex_icons, which asserts on the `tray:` log line to prove which
// mark the host asked for. So that gate leaves this unset and keeps its icon; every other gate is silent.
//
// Read once, never written, and not a setting: it is not in apex.ini, the panel cannot set it, and the deployed
// copy never has it set. Same family as APEX_ACCEPT_INJECTED (host_win.cpp).
bool TraySuppressed()
{
  static const bool suppressed = GetEnvironmentVariableA("APEX_NO_TRAY", nullptr, 0) > 0;
  return suppressed;
}

void TrayAdd(HWND h)
{
  if (TraySuppressed())
  {
    // One line, because "the tray did not appear" and "the tray was never asked for" must be distinguishable
    // from the log alone -- the same reason TrayUpdate logs what it handed the shell.
    LogLine("tray: suppressed (APEX_NO_TRAY is set -- a scratch copy, not the user's program)");
    return;
  }
  const TrayText &t = TrayStrings();
  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = h;
  g_tray.uID = 1;
  g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  g_tray.uCallbackMessage = APEXWM_TRAY;
  // The id is asked for ONCE and used for both the load and the report, so the message cannot end up
  // describing a different mark than the one that failed. (This used to re-derive the appearance from the
  // settings here, which is a second answer to a question that already has one -- see icons.h.)
  const int iconId = TrayIconId();
  g_tray.hIcon = LoadMark(iconId);
  // A NULL here means the exe has no such resource, and NIM_ADD then adds a tray entry with no icon --
  // which the shell draws as a blank slot. Said out loud for the same reason the line in TrayUpdate is:
  // this failure has no other symptom.
  if (!g_tray.hIcon)
    LogLine("tray: the %s mark (id %d) is missing from this exe (LoadImage failed, error %lu) -- the shell "
            "is being given no icon",
            iconId == IDI_APEX_DARK ? "dark" : "light", iconId, GetLastError());
  _snwprintf(g_tray.szTip, sizeof(g_tray.szTip) / sizeof(g_tray.szTip[0]), L"Apex -- %ls", U8(t.tip));
  g_trayUp = Shell_NotifyIconW(NIM_ADD, &g_tray) != 0;
  if (g_trayUp)
    TrayUpdate(); // the same path every other refresh uses, so the log line appears here too
}

void TrayRemove()
{
  if (g_trayUp)
  {
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    g_trayUp = false;
  }
  if (g_tray.hIcon)
  {
    DestroyIcon(g_tray.hIcon);
    g_tray.hIcon = nullptr;
  }
}

// THE WINDOW'S OWN ICON, in the variant that belongs to the current appearance. Set on the class so both
// the title bar and alt-tab use it, and re-set on a theme change -- the class icon is a cached property, so
// it has to be written again rather than followed automatically.
void ApplyWindowIcon(HWND h)
{
  const int id = TrayIconId();
  HINSTANCE inst = GetModuleHandleW(nullptr);
  const HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(id), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
  const HICON small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(id), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  // The previous ones were loaded by this function for this window, so replacing them frees nothing the
  // shell still needs; destroy them explicitly rather than leaking one pair per theme change.
  HICON oldBig = (HICON)SendMessageW(h, WM_SETICON, ICON_BIG, 0);
  HICON oldSmall = (HICON)SendMessageW(h, WM_SETICON, ICON_SMALL, 0);
  // ONLY A HANDLE THAT LOADED IS SET. Passing NULL on would clear the window's icon rather than leave the
  // previous one, which turns "this build is missing an artwork" into "this window has no icon at all".
  if (big)
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
  if (small)
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small);
  if (oldBig && oldBig != big)
    DestroyIcon(oldBig);
  if (oldSmall && oldSmall != small)
    DestroyIcon(oldSmall);
}

// ---- the warm-up: start the panel before it is asked for ------------------------------------------
//
// WHY. The panel's window cannot show anything until WebView2 has started, and that is MEASURED at ~0.55 s with
// a warm browser data folder (~1.4 s on the very first run). The window is created hidden and appears once the
// page has drawn, so what the user sees is a pause between the click and the window -- the price of not showing
// a flat rectangle.
//
// THAT PAUSE IS BOUGHT BACK BY SPENDING IT EARLIER. The tray's right-click is the earliest moment that means
// anything: the user has decided to open the menu, and the time they spend moving to the item is spent starting
// the browser behind a hidden window. If they pick Settings it is already drawn and appears at once; if they
// pick anything else, or dismiss the menu, it is told to go away (see the DROP message in settings_ipc.h).
//
// ⚠️ IT IS NOT LEFT RUNNING. A hidden WebView2 is a browser process tree, and a portable utility has no business
// holding one while it is idle -- so a warmed panel lives only for as long as one tray menu is open, plus a
// timeout of its own in case this process dies mid-menu (see kWarmTimeoutMs).
//
// ⚠️ AND IT IS ONLY WARMED WHEN THERE IS NOTHING TO WARM. A panel that is open is already instant to bring
// forward, and a panel that is already starting must not be joined by a second one.
void PrewarmPanel()
{
  if (FindApexPanel())
    return; // one is up (or warming) already
  if (g_panel.hProcess)
  {
    DWORD rc = 0;
    GetExitCodeProcess(g_panel.hProcess, &rc);
    if (rc == STILL_ACTIVE)
      return; // a panel process exists but has not made its window yet: leave it alone
    CloseHandle(g_panel.hProcess);
    CloseHandle(g_panel.hThread);
    g_panel.hProcess = nullptr;
    g_panel.hThread = nullptr;
  }

  char exeDir[512] = {0};
  char panel[560] = {0};
  if (!ApexModuleDir(exeDir, (int)sizeof(exeDir)) ||
      !JoinPath(exeDir, "apex-settings.exe", panel, (int)sizeof(panel)))
    return;
  if (GetFileAttributesA(panel) == INVALID_FILE_ATTRIBUTES)
    return; // OpenSettings reports this properly, when somebody actually asked for the settings

  STARTUPINFOA si = {0};
  si.cb = sizeof(si);
  char cmd[600] = {0};
  _snprintf(cmd, sizeof(cmd), "\"%s\" %s", panel, APEX_PANEL_WARM_ARG);
  if (CreateProcessA(panel, cmd, nullptr, nullptr, FALSE, 0, nullptr, exeDir, &si, &g_panel))
  {
    LogLine("settings: warming the panel up (pid %lu)", g_panel.dwProcessId);
    // ⚠️ THE HANDLE STAYS IN `g_panel`, which is what the shutdown path waits on: a panel that was warmed and
    // never asked for is still a process this program must see off (see the note at the end of WinMain).
    CloseHandle(g_panel.hThread);
    g_panel.hThread = nullptr;
  }
  else
  {
    LogLine("settings: could not warm the panel up (error %lu) -- it will be started on demand",
            GetLastError());
  }
}

// The menu closed without anybody asking for the settings: the warm-up is not needed, and it goes away.
// A panel that is already on screen ignores this (it is not a warm-up), so this is safe to send blindly.
void DropWarmPanel()
{
  if (!g_panelDropMsg)
    return;
  if (HWND h = FindApexPanel())
    PostMessageA(h, g_panelDropMsg, 0, 0);
}

// ---- settings, in their own process --------------------------------------------------------------
//
// THE PANEL MUST NOT BE ABLE TO STOP THE HOOK. It runs as a second process: the host launches it and the
// two talk through a small window-message protocol. That also means the panel can be written, tested and
// even crash-restarted without touching the thing that is holding the system's wheel input.
void OpenSettings()
{
  // ⚠️ ASK IT TO SHOW ITSELF -- DO NOT SHOW IT FROM HERE. A panel that exists is not necessarily a panel that
  // has drawn: a warmed one may still be starting, and a cross-process ShowWindow would put up the flat
  // rectangle this whole arrangement exists to avoid. The panel shows itself when it has something to show;
  // this only tells it that the user really did ask (see ShowPanelOnce in ui_webview.cpp).
  //
  // ⚠️ NO SECOND PROCESS EITHER. Creating one would end in `settings_main.cpp`'s "bring the existing panel
  // forward" path, which is a ShowWindow -- the same flat rectangle, one indirection later.
  if (HWND up = FindApexPanel())
  {
    if (g_panelShowMsg)
    {
      LogLine("settings: asking the panel that is already up to show itself");
      PostMessageA(up, g_panelShowMsg, 0, 0);
      return;
    }
    // The registration failed (it does not, but a program may not assume): fall back to the old behaviour.
    ShowWindow(up, SW_SHOW);
    SetForegroundWindow(up);
    return;
  }

  if (g_panel.hProcess)
  {
    // ⚠️ THERE IS NO "BRING THE EXISTING PANEL FORWARD" HERE ANY MORE, and its removal is the point: that is
    // the ShowWindow-fall-through this function must not do (see the note at the top -- a panel that exists may
    // not have drawn yet). A panel that has a WINDOW was handled above; a panel process without one yet is
    // either a cold start (which shows itself when its page is ready) or an unasked warm-up (which will time
    // itself out), and either way this is the one path that must still be able to answer the user's click.
    DWORD rc = 0;
    GetExitCodeProcess(g_panel.hProcess, &rc);
    if (rc == STILL_ACTIVE)
      LogLine("settings: a panel process is starting and its window is not up yet -- starting a fresh one");
    CloseHandle(g_panel.hProcess);
    CloseHandle(g_panel.hThread);
    g_panel.hProcess = nullptr;
    g_panel.hThread = nullptr;
  }

  char exeDir[512] = {0};
  if (!ApexModuleDir(exeDir, (int)sizeof(exeDir)))
    return;
  char panel[560] = {0};
  if (!JoinPath(exeDir, "apex-settings.exe", panel, (int)sizeof(panel)))
    return;
  if (GetFileAttributesA(panel) == INVALID_FILE_ATTRIBUTES)
  {
    LogLine("settings: apex-settings.exe is not next to apex.exe -- the panel needs it");
    return;
  }

  STARTUPINFOA si = {0};
  si.cb = sizeof(si);
  char cmd[600] = {0};
  _snprintf(cmd, sizeof(cmd), "\"%s\"", panel);
  if (CreateProcessA(panel, cmd, nullptr, nullptr, FALSE, 0, nullptr, exeDir, &si, &g_panel))
  {
    LogLine("settings: launched the panel (pid %lu)", g_panel.dwProcessId);
    CloseHandle(g_panel.hThread);
    g_panel.hThread = nullptr;
  }
  else
  {
    LogLine("settings: could not launch the panel (error %lu)", GetLastError());
  }
}

void TrayMenu(HWND h)
{
  // BUILT FRESH ON EVERY OPEN, and that is what makes the language change work: the menu is destroyed when
  // it closes (below), so the next right-click rebuilds it from whatever the language resolves to NOW.
  // Caching it would mean a language change that only takes effect after a restart.
  //
  // ⚠️ THERE IS NO "Enabled / Paused" ITEM. It was the first line of this menu and it is gone at the user's
  // request: a global switch duplicates what a feature's own page already says, and having two of them means
  // "why is nothing happening" has two answers. Enabling is a feature's business -- turn the feature off.
  const TrayText &t = TrayStrings();

  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, IDM_SETTINGS, U8(t.settings));
  AppendMenuW(m, MF_STRING, IDM_OPEN_DIR, U8(t.openDir));
  AppendMenuW(m, MF_STRING, IDM_SHOW_LOG, U8(t.showLog));
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, IDM_QUIT, U8(t.quit));

  // ⚠️ THE WARM-UP GOES HERE, BEFORE THE MENU APPEARS, and this line is the whole timing argument: whatever the
  // user does next -- reading the items, moving the mouse to one, clicking -- happens while the panel's browser
  // is starting behind a hidden window. It costs this process one CreateProcess (~30-50 ms, and the menu is
  // modal from here anyway) and it is skipped when a panel already exists.
  PrewarmPanel();

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(h);
  // THE MENU HAS NO ANSI/UNICODE VARIANT, and that is not an oversight in the headers: a Win32 menu stores
  // its items as UTF-16 internally (there is no CreateMenuA/W either), so AppendMenuW above put real Unicode
  // in and TrackPopupMenu renders it. Confirmed against the exports of user32.dll -- it exposes
  // `TrackPopupMenu` with no suffix at all, and no `TrackPopupMenuA`.
  const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
  DestroyMenu(m);

  // ANYTHING BUT SETTINGS MEANS THE WARM-UP WAS NOT WANTED -- including `cmd == 0`, which is the menu being
  // dismissed (a click outside it, or Escape). Same rule as everywhere else in this file: what is not needed
  // does not stay running.
  if (cmd != IDM_SETTINGS)
    DropWarmPanel();

  switch (cmd)
  {
  case IDM_SETTINGS:
    OpenSettings();
    break;
  case IDM_OPEN_DIR:
  {
    char dir[512] = {0};
    if (ApexModuleDir(dir, (int)sizeof(dir)))
      ShellExecuteA(nullptr, "open", dir, nullptr, nullptr, SW_SHOWNORMAL);
    break;
  }
  case IDM_SHOW_LOG:
  {
    char dir[512] = {0};
    if (ApexModuleDir(dir, (int)sizeof(dir)))
    {
      char p[560] = {0};
      if (JoinPath(dir, "apex.log", p, (int)sizeof(p)))
        ShellExecuteA(nullptr, "open", p, nullptr, nullptr, SW_SHOWNORMAL);
    }
    break;
  }
  case IDM_QUIT:
    DestroyWindow(h);
    break;
  }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  // THE LOW-RATE LOOK AT THE ACTIVITY COUNTER. ~30 ms, on this thread, for one reason: the features call
  // ApexHost::activity() from the INPUT PATH, so the message to the panel must happen somewhere else -- see the
  // note on the counter above, and kActivityNotifyMs.
  //
  // ⚠️ SetTimer IS FINE HERE AND ONLY HERE. This project's rule against it (AGENTS.md) is about THE FRAME
  // CLOCK, where its ~15.6 ms floor turns a 4 ms model into a visible staircase -- measured. A UI notification
  // has no such requirement: 30 ms of jitter on an animation nobody is timing is invisible, and this costs the
  // engine's timer thread nothing.
  case WM_TIMER:
    if (wp == APEXWM_ACTIVITY_TIMER)
    {
      NotifyActivity();
      return 0;
    }
    // The debounced settings write: the edit settled, so the file is brought up to date. One-shot by
    // construction -- see SettingsTouch.
    if (wp == APEXWM_SAVE_TIMER)
    {
      KillTimer(h, APEXWM_SAVE_TIMER);
      host::SettingsSaveAll();
      return 0;
    }
    // The facts that can change while the panel is open. It posts ONLY when one changes, so a steady state
    // costs a comparison per tick and nothing else -- see NotifyStateChanges.
    if (wp == APEXWM_STATE_TIMER)
    {
      NotifyStateChanges();
      // AND THE FEATURES' OWN STATES, from the same tick: this is where "a feature is holding something the
      // user cannot see" turns into the mark's orange bar (see WatchFeatures / traymark.h).
      WatchFeatures();
      return 0;
    }
    // THE SINGLE TRAY CLICK, once the double-click timeout has passed without a second one (see APEXWM_TRAY).
    if (wp == APEXWM_TRAY_CLICK_TIMER)
    {
      KillTimer(h, APEXWM_TRAY_CLICK_TIMER);
      host::QuickPanelToggle();
      return 0;
    }
    // TEST-ONLY (APEX_QUICKPANEL_ONCE): one show, then one hide, then nothing. See QuickPanelSelfTest.
    if (wp == APEXWM_FLYOUT_TIMER)
    {
      static bool shown = false;
      if (!shown)
      {
        shown = true;
        LogLine("flyout selftest: showing the quick panel now");
        host::QuickPanelShow();
        SetTimer(h, APEXWM_FLYOUT_TIMER, 1200, nullptr);
      }
      else
      {
        KillTimer(h, APEXWM_FLYOUT_TIMER);
        host::QuickPanelHide("the selftest is over");
        LogLine("flyout selftest: hiding it again -- the run is over");
      }
      return 0;
    }
    break;

  case APEXWM_TRAY:
    if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
      TrayMenu(h);
    // ⚠️ ONE PRESS, TWO MEANINGS, SO THE PRESS ONLY ARMS A TIMER. A single click opens the quick panel and a
    // double click opens the settings -- the same button on the same icon -- and neither can be answered on the
    // press, because until the double-click timeout has passed there is no way to know which one it was. So the
    // press starts a one-shot timer; the second click kills it, and the timer fires only for a genuine single
    // click. The cost is that a single click waits one double-click timeout (the user's own setting) before the
    // panel appears, which is the trade the user chose over "the panel appears and then the settings window
    // opens on top of it".
    else if (LOWORD(lp) == WM_LBUTTONDOWN)
    {
      UINT ms = GetDoubleClickTime();
      if (!ms)
        ms = 400; // a zero would make SetTimer fail, and the click would then do nothing at all
      SetTimer(h, APEXWM_TRAY_CLICK_TIMER, ms, nullptr);
    }
    else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
    {
      KillTimer(h, APEXWM_TRAY_CLICK_TIMER);
      // The flyout must not sit under the settings window. It is almost always already gone (the first click of
      // the double click landed outside it, so it lost the foreground) -- this is for the case where it was
      // opened by a click that never reached the tray.
      host::QuickPanelHide("the user asked for the settings");
      OpenSettings();
    }
    return 0;

  // THE SETTINGS PANEL, which is a SEPARATE PROCESS. Everything it asks for arrives here, on this thread --
  // the same one the tray lives on, and NOT the one the wheels are decided on. That separation is the
  // reason a hung panel cannot stop smoothing: the hook and the engine never wait for this.
  //
  // ⚠️ THE SENDER IS `wp`, NOT `h`. In WM_COPYDATA the WPARAM carries the handle of the window that SENT
  // the message -- which is the panel. Passing `h` (this window) had the host answering ITSELF: the reply
  // went to the host's own window procedure, which ignored it, and the panel waited for an answer that
  // never came. That is why the panel reported "the host is not running" while the host ran fine beside it.
  case WM_COPYDATA:
    return host::SettingsIpc((HWND)wp, (const COPYDATASTRUCT *)lp);

  // THE SYSTEM CHANGED UNDER US. Three things arrive here and all three have to be re-read, because each
  // was chosen for contrast or for the user's own settings rather than fixed at start-up:
  //
  //   * the app light/dark theme -- the tray icon and this window's icon are picked by it;
  //   * the taskbar was re-created (Explorer restarting) -- the icon has to be re-added, not modified;
  //   * the UI language -- the tray's own text is resolved by it.
  //
  // The lParam names WHICH setting changed ("ImmersiveColorSet" for the theme, "intl" for the language) but
  // none of the three is expensive, so all of them are refreshed together. Re-reading is what makes this
  // correct rather than clever: a language change does not always arrive as a message we can anticipate.
  case WM_SETTINGCHANGE:
  case WM_THEMECHANGED:
    TrayUpdate();
    ApplyWindowIcon(h);
    return 0;

  case WM_DESTROY:
    TrayRemove();
    PostQuitMessage(0);
    return 0;
  }

  // Explorer was restarted: the old tray entry is gone with it, so the icon must be ADDED again rather than
  // modified (NIM_MODIFY against a shell that no longer knows us does nothing at all).
  //
  // Checked HERE rather than in a case label because "TaskbarCreated" has no fixed message number -- it is
  // registered at run time, so it is not a compile-time constant and a case label cannot hold it.
  if (g_taskbarCreated != 0 && msg == g_taskbarCreated)
  {
    g_trayUp = false;
    TrayAdd(h);
    return 0;
  }

  return DefWindowProcA(h, msg, wp, lp);
}

bool LoadHostConfig()
{
  char path[560] = {0};
  if (!HostConfigPath(path, (int)sizeof(path)))
    return false;
  FILE *f = fopen(path, "rb");
  // ⚠️ "THERE IS NO FILE YET" IS ITSELF A FACT THIS PROGRAM USES, and it is recorded BEFORE the early return
  // below: it is what tells a machine nobody has configured from one whose user has made choices (see the
  // fresh-install seeding in WinMain). Reading it anywhere else would be a second answer to the same question.
  g_cfgFileExisted = (f != nullptr);
  if (!f)
  {
    LogLine("host settings: none yet (%s) -- a fresh install: defaults now, every feature off", path);
    return false;
  }
  static char text[8192];
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = 0;
  const bool ok = ParseHostConfig(text, g_cfg);
  LogLine("host settings: %s (%s)", ok ? "loaded" : "kept defaults", path);
  return ok;
}

void SaveHostConfig()
{
  char path[560] = {0};
  if (!HostConfigPath(path, (int)sizeof(path)))
    return;
  static char text[8192];
  FormatHostConfig(g_cfg, text, (int)sizeof(text));
  char tmp[600] = {0};
  if (_snprintf(tmp, sizeof(tmp), "%s.tmp", path) <= 0)
    return;
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return;
  fputs(text, f);
  fclose(f);
  MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

} // namespace

// ---------------------------------------------------------------------------
// apex::host:: -- the two pieces of the host's state that OTHER translation units need to reach.
//
// They are defined here, OUTSIDE the anonymous namespace above and qualified as apex::host (not just
// host), because that is the namespace host.h declares them in. An unqualified `namespace host` at this
// point would be a global one, and the calls in this file would then be ambiguous between it and
// apex::host -- which is exactly the error it produced.
//
// The state itself stays file-scope: nothing outside this file touches g_currentFeatureId directly.
// ---------------------------------------------------------------------------
namespace apex {
namespace host {

// IS THE INTERFACE TO BE CHINESE? (declared in host.h; the reasoning is where the tray's strings are)
//
// ⚠️ ONE IMPLEMENTATION, THREE SURFACES. The tray, the quick panel and (by the same rule, in its own process)
// the settings page all have to agree about this, and the only way that stays true is if there is one function
// answering it. `auto` asks Windows (system_win.cpp), which is the same answer the panel's own copy resolves.
bool UiIsChinese()
{
  if (g_cfg.lang == Lang::kZh)
    return true;
  if (g_cfg.lang == Lang::kEn)
    return false;
  char tag[64] = {0};
  host::PreferredUiLanguage(tag, (int)sizeof(tag));
  return host::LanguageTagIsChinese(tag);
}

// WHERE THE TRAY ICON IS (declared in host.h). The quick panel is placed above it, and the icon's identity --
// the host window and uID 1 -- is this file's, so the lookup belongs here rather than in the flyout: a second
// copy of "which icon is ours" is how a panel ends up above somebody else's.
//
// ⚠️ IT CAN LEGITIMATELY FAIL, and the caller has a fallback for it: an icon parked in the overflow flyout, or
// a shell that has just restarted, both answer with a failure. `Shell_NotifyIconGetRect` is Windows 7 and up
// and is resolved at run time, so a system without it simply never anchors the panel (the cursor is used).
bool TrayIconScreenRect(int *x, int *y, int *w, int *h)
{
  if (!g_trayUp || !g_wnd)
    return false;
  HMODULE shell = GetModuleHandleA("shell32.dll");
  if (!shell)
    return false;
  typedef HRESULT(WINAPI * GetRectFn)(const NOTIFYICONIDENTIFIER *, RECT *);
  GetRectFn fn = (GetRectFn)(void *)GetProcAddress(shell, "Shell_NotifyIconGetRect");
  if (!fn)
    return false;

  NOTIFYICONIDENTIFIER id = {};
  id.cbSize = sizeof(id);
  id.hWnd = g_wnd;
  id.uID = g_tray.uID;
  RECT r = {};
  if (FAILED(fn(&id, &r)))
    return false;
  if (x) *x = r.left;
  if (y) *y = r.top;
  if (w) *w = r.right - r.left;
  if (h) *h = r.bottom - r.top;
  return true;
}

// TELL THE PANEL THAT SOMETHING IT IS DRAWING HAS CHANGED (declared in host.h).
//
// WHY IT EXISTS. The settings page and the quick panel are two views of ONE state -- the host's feature list
// and the values inside each feature -- and the user can have the page open while they click a switch in the
// flyout. The page is then showing what was true when it last read, and the user's rule is explicit about it:
// "快速面板的开关要能实时同步到设置面板上".
//
// ⚠️ IT IS NOT NotifyStateChanges, and the difference is the point. That one runs on a one-second timer and
// watches a MACHINE fact (is REAPER running); this one is "the user just did something", which must not wait
// for a tick and must not be folded into a poll -- the user is looking straight at the result.
//
// ⚠️ AND A DRAG DOES NOT COME THROUGH HERE ONCE PER PIXEL. The flyout sends a value per mouse position, and one
// redraw of the page per pixel would be one redraw per pixel; the caller sends this once, when the drag ends
// (see quickpanel_win.cpp).
//
// The page's own entry point already does the right thing for any `what` (`window.__apexStateChanged`), which
// is why this is one PostMessage and no new plumbing in the panel at all.
void PanelStateChanged(unsigned what)
{
  if (!g_stateMsg || !g_panelWnd)
    return;
  PostMessageA(g_panelWnd, g_stateMsg, (WPARAM)what, 0);
}

// TEST-ONLY (APEX_QUICKPANEL_ONCE): SHOW THE QUICK PANEL ONCE AND THEN HIDE IT.
//
// ⚠️ WHY THE PRODUCT CARRIES A SWITCH FOR THIS. The flyout is the one part of Apex drawn by hand -- GDI+ into a
// 32-bit DIB, then UpdateLayeredWindow -- and none of it can be checked from the outside: the panel belongs to
// the window, and reading it back would mean screen-capturing the user's desktop. Everything else about it IS
// verified by arithmetic (quickpanel.h, via test/check_apex_quickpanel.sh), but "does the blit actually succeed
// on a real window, and does building the model from live features run without falling over" is only answered by
// doing it. Without this switch the first time that code runs is the first time the USER clicks the tray -- and
// if it is wrong, the host is what falls over with it.
//
// ⚠️ AND IT DOES NOT STEAL THE FOCUS (see StartFadeIn in quickpanel_win.cpp). A gate is meant to be invisible to
// whoever is at the machine; a flyout appearing over their work is not a test, it is an interruption.
//
// Same family as APEX_NO_TRAY (see TraySuppressed) and APEX_ACCEPT_INJECTED (host_win.cpp): read once, never
// written, not in apex.ini, and never set in a deployment.
bool QuickPanelSelfTest()
{
  static const bool on = GetEnvironmentVariableA("APEX_QUICKPANEL_ONCE", nullptr, 0) > 0;
  return on;
}

// THE TRAY FOLLOWS THE PANEL. The tray's icon and text are drawn from the host settings, and the panel can
// change them, so this is the route back: settings_host.cpp calls it, and it touches the tray and nothing
// else. Declared in host.h; defined below, where the tray lives.
void TrayRefreshFromSettings()
{
  TrayUpdate();
  if (g_wnd)
    ApplyWindowIcon(g_wnd);
}


// THE PANEL'S WINDOW, learned from the sender of every IPC request (see settings_ipc.h). The host needs it
// for the activity relay, and the reason it is remembered rather than searched for is that the search
// (FindWindowA by class) ENUMERATES WINDOWS -- and the caller of this is the wheel hook. A stale handle is
// harmless: PostMessage to a closed window fails, and the count is dropped, which is what "no panel to show
// it to" means anyway.
void SettingsPanelSeen(HWND panel)
{
  if (panel)
    g_panelWnd = panel;
}

void SetCurrentFeature(const char *id) { g_currentFeatureId = id; }
const char *CurrentFeature() { return g_currentFeatureId; }

// (QuickBlockKey itself is an inline in apex/quickpanel.h -- see the note there: it is a pure function of a feature
// id and an item, and it is a TRANSPORT string as well as an identity, so the probe that has no host can check it.)

// WHICH BLOCKS THE QUICK PANEL WOULD DRAW, IN THE ORDER THE FLYOUT DRAWS THEM (declared in host.h -- the flyout
// and the General page's reorder list both read this one answer).
int QuickBlocks(QuickBlock *out, int max)
{
  HostConfig *cfg = SettingsConfig();
  Loader *ld = SettingsLoader();
  if (!cfg || !ld)
    return 0;

  static QuickBlock found[kMaxQuickBlocks];
  static ApexQuickItem raw[apex::quick::kMaxOwnPerFeature];
  int total = 0;
  for (int i = 0; i < ld->Count(); ++i)
  {
    const LoadedFeature &f = ld->At(i);
    if (!f.ok || !f.api || !f.api->id || !f.api->quickItems)
      continue;
    // ⚠️ A FEATURE THE USER SWITCHED OFF CONTRIBUTES NOTHING (see the note in host.h): its rows are hidden, and
    // its per-control mapping switches are untouched, so switching it back on restores exactly what was there.
    if (cfg->FeatureOff(f.api->id))
      continue;
    ZeroMemory(raw, sizeof(raw));
    SetCurrentFeature(f.api->id);
    const int n = f.api->quickItems(raw, apex::quick::kMaxOwnPerFeature);
    SetCurrentFeature(nullptr);
    const int taken = n < apex::quick::kMaxOwnPerFeature ? (n > 0 ? n : 0) : apex::quick::kMaxOwnPerFeature;
    for (int k = 0; k < taken; ++k)
    {
      if (!raw[k].id[0])
        continue; // nothing to send a value to: not drawn, so not a block either
      char key[112] = {0};
      apex::quick::QuickBlockKey(f.api->id, raw[k].groupZh, raw[k].id, key, sizeof(key));
      int at = -1;
      for (int b = 0; b < total; ++b)
        if (strcmp(found[b].key, key) == 0)
        {
          at = b;
          break;
        }
      if (at < 0)
      {
        if (total >= kMaxQuickBlocks)
          continue; // full: the flyout is capped at the same number of panes
        at = total++;
        found[at].slot = i;
        _snprintf(found[at].key, sizeof(found[at].key), "%s", key);
        // THE PANE'S HEADING: the group's own words, or the item's label when it floats alone.
        const char *zh = raw[k].groupZh[0] ? raw[k].groupZh : raw[k].labelZh;
        const char *en = raw[k].groupEn[0] ? raw[k].groupEn : raw[k].labelEn;
        _snprintf(found[at].nameZh, sizeof(found[at].nameZh), "%s", zh ? zh : "");
        _snprintf(found[at].nameEn, sizeof(found[at].nameEn), "%s", en ? en : "");
      }
      // (A key already seen is the same block, and the next item of that group lands in it too. Two FEATURES
      //  naming one group are two keys and two blocks -- they only share a pane by accident of naming, which is a
      //  cosmetic merge the layout has always had.)
    }
  }

  // THE ORDER: the keys the user named first, in the order they named them, and then the rest in natural order.
  int written = 0;
  static bool used[kMaxQuickBlocks];
  ZeroMemory(used, sizeof(used));
  for (int k = 0; k < cfg->quickOrderN; ++k)
    for (int b = 0; b < total; ++b)
      if (!used[b] && strcmp(found[b].key, cfg->quickOrder[k]) == 0)
      {
        used[b] = true;
        if (out && written < max)
          out[written] = found[b];
        ++written;
        break;
      }
  for (int b = 0; b < total; ++b)
    if (!used[b])
    {
      if (out && written < max)
        out[written] = found[b];
      ++written;
    }
  return written;
}

// Note that something worth persisting changed. Called by the IPC commands that alter a setting (see the call
// sites in settings_host.cpp), so this is deliberately NOT a feature's business: the host owns the files, and
// a future feature gets the same durability without doing anything.
//
// The window handle is checked because this can be reached before the host window exists -- the loader's
// start-up path reads settings through the same functions -- and a timer armed with no window to deliver to
// would simply never fire, silently, which is the failure this whole mechanism exists to prevent.
void SettingsTouch()
{
  if (!g_wnd)
    return;
  // Re-arming the same id REPLACES the pending timer rather than adding a second one, which is exactly the
  // debounce wanted: the clock restarts at every edit and only the last one fires a write.
  SetTimer(g_wnd, APEXWM_SAVE_TIMER, APEXWM_SAVE_DELAY_MS, nullptr);
}

// The settings protocol's window onto the host's state. It reaches the SAME objects the tray does, so the
// panel cannot do anything the tray could not -- and the alternatives (a second config instance, or a
// copy of the loader) would each be a second source of truth for values the user can see in both places.
HostConfig *SettingsConfig() { return &g_cfg; }
Loader *SettingsLoader() { return &g_loader; }

// ---- START WITH WINDOWS: THE ONE WRITE OUTSIDE THIS FOLDER ------------------------------------------
//
// ⚠️⚠️ WHY IT EXISTS AT ALL, when the program is portable and writes nothing anywhere else. Because the user
// asked for it ("通用设置增加选项'开机启动'，默认不启用"), and because Windows gives no other option: a program
// cannot register itself for logon from inside its own directory. The choices are the per-user Run key
// (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`) or a shortcut in the Startup folder, and both live
// outside. This takes the Run key: one value, this user only, no administrator, and deleting it undoes
// everything. NOTHING ELSE in Apex writes outside its folder -- see hostconfig.h on the `autostart` field.
//
// ⚠️ THE PATH IS QUOTED. "D:\App protable\Apex\apex.exe" is two arguments to anything that splits a command
// line, and an unquoted entry is a startup item that silently does not start.
//
// ⚠️ AND IT IS REWRITTEN ON EVERY START WHILE THE SETTING IS ON, which is what makes it survive the program
// being a portable folder that gets moved: the entry follows the copy that is running, instead of pointing at
// where it used to be. (That is also why this is called from WinMain and not only from the switch.)
void AutostartApply()
{
  static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  static const wchar_t *kValueName = L"Apex";

  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS)
  {
    LogLine("autostart: could not open HKCU\\...\\Run (error %lu)", GetLastError());
    return;
  }

  if (!g_cfg.autostart)
  {
    // Gone, not emptied: an entry that points nowhere is worse than none, because the shell still tries it.
    const LSTATUS st = RegDeleteValueW(key, kValueName);
    LogLine("autostart: off%s", (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? "" : " (delete failed)");
    RegCloseKey(key);
    return;
  }

  // ⚠️ THE RUNNING EXE, ASKED FOR IN UTF-16 -- not built from the host's own ANSI paths (see the note in
  // paths_win.cpp): this string goes into the registry as a wide value, and a Chinese folder name would have
  // been mangled by a conversion through the ANSI code page.
  wchar_t exe[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
  if (n == 0 || n >= (DWORD)(sizeof(exe) / sizeof(exe[0])))
  {
    LogLine("autostart: could not read this program's own path (error %lu)", GetLastError());
    RegCloseKey(key);
    return;
  }
  wchar_t quoted[MAX_PATH + 4] = {0};
  _snwprintf(quoted, (sizeof(quoted) / sizeof(quoted[0])) - 1, L"\"%s\"", exe);
  const LSTATUS st = RegSetValueExW(key, kValueName, 0, REG_SZ, (const BYTE *)quoted,
                                    (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
  LogLine("autostart: %s (%ls)", st == ERROR_SUCCESS ? "set" : "FAILED", quoted);
  RegCloseKey(key);
}

void SettingsLog(const char *fmt, ...)
{
  if (!g_log)
    return;
  char buf[512] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  LogLine("%s", buf);
}

// Written on demand (the panel's Close, or its Save): the panel applies values in memory as the user moves
// a control, so the file is only touched when the editing session ends.
void SettingsSaveAll()
{
  SaveHostConfig();
  for (int i = 0; i < g_loader.Count(); ++i)
  {
    const LoadedFeature &f = g_loader.At(i);
    if (f.ok && f.api && f.api->saveSettings)
    {
      FeatureScope scope(f.api->id);
      f.api->saveSettings();
    }
  }
  LogLine("settings: saved");
}

// ⚠️ TWO FUNCTIONS WERE DELETED HERE, AND WHY THEY WERE WRONG RATHER THAN MERELY UNUSED:
//
//   SettingsRefreshTarget() -- it dropped the target cache so a blacklist change would bite at once. The
//   blacklist is the FEATURE's now (see ApexFeature::listOp) and it is checked inside the feature's own
//   onWheel, so there is no host-side list to invalidate. Nothing called it.
//
//   SettingsOpenFileForSlot() -- a second implementation of "open a feature's settings file", alongside the
//   one the IPC actually uses (settings_host.cpp). They did NOT agree: this one indexed with Count()/At(),
//   which counts only successfully loaded features, while the panel's slot numbers come from CountAll()/Seen(),
//   which includes failed rows. Adding a feature that fails to load would have shifted every slot and opened
//   the wrong file. One rule, one implementation -- and the surviving one is the one the panel talks to.

} // namespace host
} // namespace apex

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
  // -------------------------------------------------------------------------
  // ONE HOST PER INSTALLATION -- a second launch is IGNORED, not refused.
  //
  // The user's requirement: "APP运行唯一化，不允许多个实例，多次运行不理会". Silently exiting is the right
  // reading of 不理会: the program a user wanted is already running, and a message box telling them so would
  // be a dialog they have to dismiss to reach the state they already had.
  //
  // ⚠️ IT IS CHECKED BEFORE THE LOG IS OPENED, and that is not tidiness. The log is opened with "w" --
  // TRUNCATE -- so a second launch would wipe the running instance's log and then exit, leaving a fresh empty
  // file and no trace of what happened. (That was already true before this check existed; it is the reason
  // the check has to be the first thing in WinMain rather than the first thing after start-up.)
  //
  // ⚠️ AND "ONE" MEANS ONE PER MACHINE, WHICHEVER FOLDER IT CAME FROM.
  //
  // This used to be one per INSTALLATION: Apex is portable, so the folder was treated as the installation and
  // two copies of the folder were two programs that could coexist. The user has since made the rule stricter
  // and it is the better rule -- "进程只能有一个Apex.exe". Two hosts are not two windows, they are two global
  // wheel hooks: each swallows what it sees and injects its own output, and the second one sees the first
  // one's injected events. That is the "two handlers driving one view" that decision.h exists to avoid, and
  // the folder rule could not prevent it because two folders were exactly what it allowed.
  //
  // ⚠️ SO THE GATES CANNOT RUN A PRIVATE COPY ALONGSIDE THE USER'S ANY MORE. They must stop the running host
  // first (test/lib_procs.sh, apex_clear_the_field) -- which the user has explicitly authorised, including
  // when it is their own copy.
  // -------------------------------------------------------------------------
  if (FindApexHost())
    return 0;

  char dir[512] = {0};
  if (ApexModuleDir(dir, (int)sizeof(dir)))
  {
    char p[560] = {0};
    if (JoinPath(dir, "apex.log", p, (int)sizeof(p)))
    {
      InitializeCriticalSection(&g_logLock);
      g_log = fopen(p, "w");
    }
  }
  LogLine("Apex -- starting");

  LoadHostConfig();
  // ⚠️ AND THE MACHINE IS PUT IN STEP WITH IT, BEFORE ANYTHING ELSE CAN GO WRONG. If the setting is on, this
  // re-points the Run entry at the copy that is running now (the folder is portable and may have moved); if it
  // is off, it makes sure no stale entry is left behind. Doing nothing here would mean a user who moved the
  // folder was left with a startup item pointing at a path that no longer exists.
  host::AutostartApply();
  // No `skip=` any more: the blacklist belongs to whichever feature owns one, and is read there (a
  // feature that has a list logs it itself -- see the SmoothWheel settings line).
  LogLine("host: lang=%s theme=%s off=%d", LangName(g_cfg.lang), ThemeName(g_cfg.theme), g_cfg.offN);

  // The shell2019s "taskbar was re-created" broadcast, registered before the window exists. A zero here means
  // the message will never arrive, which only costs the tray icon a restart of Explorer to come back.
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  // The "something happened" notification both processes know by name (see settings_ipc.h). Registered here,
  // before any feature is loaded, so a feature's very first wheel can already be reported.
  g_activityMsg = RegisterWindowMessageA(APEX_ACTIVITY_MSG_NAME);
  g_stateMsg = RegisterWindowMessageA(APEX_STATE_MSG_NAME);
  // The warm-up's two answers (see PrewarmPanel / DropWarmPanel): registered by name for the same reason --
  // two processes, one id, no number to keep in step.
  g_panelShowMsg = RegisterWindowMessageA(APEX_PANEL_SHOW_MSG_NAME);
  g_panelDropMsg = RegisterWindowMessageA(APEX_PANEL_DROP_MSG_NAME);

  InitHostApi();
  const int n = g_loader.LoadAll(&g_hostApi);

  // ⚠️⚠️ A FRESH INSTALL STARTS WITH EVERY FEATURE SWITCHED OFF, AND THIS IS WHERE "FRESH" IS ACTED ON. The user's
  // rule: "所有插件的开关默认值改成关…就是用户全新用上时，什么功能也不开，让用户按需打开。已经有配置过的用户不影响。"
  //
  // ⚠️ SO THE DEFAULT IS NOT A VALUE -- IT IS A ONE-TIME SEEDING OF THE `off` LIST, and only for a machine with no
  // apex.ini. An existing file keeps every choice in it, including an EMPTY off list: that still means "everything
  // on", exactly as it did before this existed, which is what makes "an existing user is unaffected" true rather
  // than hoped for.
  //
  // ⚠️ WHY IT IS WRITTEN OUT RATHER THAN REMEMBERED AS "nothing was configured yet": the list has to exist BEFORE
  // the user's first real edit. A user who then switches one feature on must end up with the OTHER three named in
  // `off` -- and that falls out of this for free, because by then the list is already the whole set. Keeping it in
  // memory instead would need the save path to know about first runs, which is a second implementation of the
  // same rule.
  //
  // ⚠️ IT RUNS AFTER LoadAll ON PURPOSE: the ids are the ones actually loaded, so a feature that failed to load is
  // not written into a list the user would have to clean up by hand.
  if (!g_cfgFileExisted)
  {
    for (int i = 0; i < g_loader.CountAll(); ++i)
    {
      const LoadedFeature &f = g_loader.Seen(i);
      if (f.ok && f.api && f.api->id[0])
        g_cfg.FeatureSetOff(f.api->id, true);
    }
    LogLine("host settings: fresh install -- every feature starts switched off (%d seeded)", g_cfg.offN);
    SaveHostConfig();
  }

  for (int i = 0; i < g_loader.CountAll(); ++i)
  {
    const LoadedFeature &f = g_loader.Seen(i);
    if (f.ok && f.api)
      LogLine("feature: %s (%s) v%s %s", f.api->id, f.api->nameEn, f.api->version,
              g_cfg.FeatureOff(f.api->id) ? "DISABLED by the user" : "enabled");
    else
      LogLine("feature: FAILED -- %s", f.why);
  }
  LogLine("features live: %d", n);

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = APEX_HOST_WND_CLASS;
  RegisterClassA(&wc);
  // A real (hidden) top-level window, not a message-only one: the tray icon posts its mouse messages here
  // and a message-only window never receives posted messages.
  g_wnd = CreateWindowExA(0, wc.lpszClassName, "Apex", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst,
                          nullptr);
  if (!g_wnd)
  {
    LogLine("FATAL: no window");
    return 1;
  }
  // The window's icon, in the variant that belongs to the current appearance. The window is hidden, so this
  // costs nothing here -- it matters because the same code path is the one a visible window would use, and
  // because alt-tab can list even a hidden window's owner.
  ApplyWindowIcon(g_wnd);

  if (!host::CaptureStart(OnWheel, nullptr))
  {
    LogLine("FATAL: the wheel hook could not be installed (error %lu)", GetLastError());
    return 1;
  }
  LogLine("wheel hook installed%s", host::CaptureAcceptsInjected()
                                       ? "  [TEST MODE: also handling INJECTED wheels]"
                                       : "");

  if (!host::InjectThreadStart())
  {
    LogLine("FATAL: the injection thread could not be started (error %lu)", GetLastError());
    return 1;
  }
  if (!host::EngineStart(Tick, nullptr, g_frameMs))
  {
    LogLine("FATAL: the engine clock could not be started (error %lu)", GetLastError());
    return 1;
  }
  LogLine("engine running, period %.1f ms (~%.0f Hz)", g_frameMs, 1000.0 / g_frameMs);

  TrayAdd(g_wnd);
  // THE QUICK PANEL'S WINDOW, CREATED NOW AND KEPT HIDDEN. It is made here rather than on the first click
  // because the whole reason it exists is that the answer to "the user clicked the tray" has to be immediate --
  // creating a window, starting GDI+ and building a client area on the click would be a visible pause for no
  // benefit. It draws nothing and costs nothing until it is shown (see quickpanel_win.cpp).
  host::QuickPanelInit();
  // ⚠️ THREE ANSWERS, NOT TWO. With APEX_NO_TRAY set the tray was never asked for (a scratch copy -- see
  // TraySuppressed), and reporting that as "FAILED" is a log line that sends the next reader after a bug that
  // does not exist. The suppressed case was found by reading this line in a gate's log.
  LogLine("tray icon %s", g_trayUp ? "added" : (TraySuppressed() ? "suppressed (APEX_NO_TRAY)" : "FAILED"));

  // The timer that hands the panel its animation cue (see the counter's note). Started once, here: it lives as
  // long as the host does, and costs one no-op comparison when nothing is happening.
  SetTimer(g_wnd, APEXWM_ACTIVITY_TIMER, kActivityNotifyMs, nullptr);
  // The same kind of low-rate timer, and for the same reason (see the note on APEXWM_ACTIVITY_TIMER): this is
  // a notification, not the frame clock, so SetTimer's ~15.6 ms floor is irrelevant here.
  SetTimer(g_wnd, APEXWM_STATE_TIMER, kStateNotifyMs, nullptr);
  // TEST-ONLY: fire the quick-panel self test once, a moment after start-up so the log's own order is readable
  // (see QuickPanelSelfTest). Never set in a deployment.
  if (host::QuickPanelSelfTest())
    SetTimer(g_wnd, APEXWM_FLYOUT_TIMER, 1200, nullptr);

  MSG msg;
  while (GetMessageA(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }

  LogLine("shutting down");
  host::EngineStop();
  host::InjectThreadStop();
  host::CaptureStop();
  g_loader.UnloadAll();
  SaveHostConfig();

  // THE PANEL IS ASKED TO GO, THEN MADE TO. The user's request is "插件进程什么的要退干净" -- nothing of ours
  // left behind -- and the panel is the only other process Apex owns.
  //
  // WHY ASK AT ALL, when a kill is faster: the panel is a WebView2 host, and its browser processes are torn
  // down by the controller's own Close() during WM_DESTROY (see ui_webview.cpp). A TerminateProcess skips that,
  // so the msedgewebview2.exe children are left to notice their parent died -- which they do, but not
  // instantly, and the delay is visible as processes that outlive the program. Asking is also the path the
  // user's own close takes, so the exit route is one route.
  //
  // The wait is short and the kill is the fallback, so a panel that is hung (which is a thing this
  // architecture explicitly allows -- it is why the panel is a separate process at all) cannot hold up the
  // host's exit. A hung panel is killed exactly as it was before.
  if (g_panel.hProcess)
  {
    HWND h = FindApexPanel();
    if (h)
      PostMessageA(h, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(g_panel.hProcess, 2000) != WAIT_OBJECT_0)
    {
      LogLine("shutdown: the panel did not close in time -- ending it");
      TerminateProcess(g_panel.hProcess, 0);
    }
    CloseHandle(g_panel.hProcess);
  }
  if (g_log)
  {
    LogLine("exiting");
    fclose(g_log);
    g_log = nullptr;
  }
  return 0;
}
