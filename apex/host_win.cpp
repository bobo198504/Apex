// ---------------------------------------------------------------------------
// THE WINDOWS SIDE OF THE HOST: capture, target identity, injection, the clock, and the two system
// questions the panel asks (is the system light or dark, what language does the user read).
//
// This is the file the previous single-feature app already had, kept and extended rather than rewritten:
// the hook, the target cache and the injection thread were all measured, and the measurements are in the
// comments that explain them. What is NEW here is that they are now host services exposed through
// ApexHost, instead of being wired directly to one feature.
//
// WHAT IS UNCHANGED, AND WHY IT MUST STAY THAT WAY (all of these were measured, not assumed):
//   * injected events are dropped FIRST, before anything else looks at the event. Our own output comes
//     straight back into this callback; without this the app smooths its own output forever.
//   * the swallow decision is made IN THE HOOK, from a cache, because the alternative -- deciding on the
//     clock -- means the original notch has already landed by the time the decision is made.
//   * SendInput is called from ONE dedicated thread. Called from inside a message-driven call it blocks
//     on the loop it is itself blocking: measured at 1001 ms, which produced the "stuck then lurch".
// ---------------------------------------------------------------------------

#include "abi.h"
#include "host.h"
#include "engines.h" // the programs that bring their own smoothing -- ONE table, two uses (see its header)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

namespace apex {
namespace host {

// ---- the sink -----------------------------------------------------------------------------------
namespace {
WheelSink g_sink = nullptr;
void *g_sinkUser = nullptr;
HHOOK g_hook = nullptr;

// Set while we inject. The LLMHF_INJECTED flag is the primary filter; this is the second line of
// defence, because a self-loop is the one failure that would be both invisible and endless.
volatile LONG g_injecting = 0;

// TEST-ONLY (SWS_APP_ACCEPT_INJECTED / APEX_ACCEPT_INJECTED): also process wheels the OS flags as
// injected. A synthetic wheel is indistinguishable from one we sent ourselves -- the flag is the same --
// so without this a load test cannot drive the real path at all and would report a flawless result for
// work that never happened. The g_injecting check stays, so the self-loop cannot start.
volatile LONG g_acceptInjected = 0;
} // namespace

// The sink signature is shared with the feature ABI: the host hands a feature the wheel and asks whether
// to take it. `user` is unused -- there is one sink for the whole host.
bool HostWheelSink(const ApexWheelEvent &ev, void *);

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam)
{
  if (code == HC_ACTION && wParam == WM_MOUSEWHEEL)
  {
    const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lParam;

    // -------------------------------------------------------------------------
    // WHICH WHEELS DO WE LOOK AT AT ALL -- and the bug that was here.
    //
    // ⚠️ A REAL WHEEL IS NEVER SKIPPED. The condition used to be
    //
    //     if (g_injecting == 0 && (acceptInjected || !injected))
    //
    // and `g_injecting` is set for the DURATION OF SendInput. So any wheel that arrived while the host was
    // injecting its own output -- which is most of the time during a roll, because it injects once per engine
    // frame -- fell through to CallNextHookEx and reached the receiver UNTOUCHED.
    //
    // MEASURED, end to end (real program, real injection, receiver timestamps): 12-16 of every 60 notches
    // arrived as raw 120-delta messages -- about 20% -- sitting in a stream whose other messages carry ONE
    // delta each. That is the "跳" the user reported: a whole notch lands in a single message in the middle of
    // a smoothed roll. It is not the model and it is not the release addition: the same 20% was measured with
    // the addition set to zero, and with the feature's window at 200 ms it is the same again.
    //
    // ⚠️ WHY `g_injecting` EXISTS AND WHY IT MUST NOT APPLY TO REAL WHEELS: it is the second line of defence
    // against a SELF-LOOP -- our own injected events coming back through this hook. But our own events are
    // ALREADY identified by `LLMHF_INJECTED`, which is the primary filter and the one the OS guarantees. The
    // timing guard is only load-bearing in the TEST-ONLY mode that deliberately ignores that flag (see
    // APEX_ACCEPT_INJECTED above): with the flag ignored, a synthetic wheel from the test injector is
    // indistinguishable from our own output, and the ONLY thing separating them is "are we injecting right
    // now". So the guard belongs on the synthetic path and nowhere else.
    //
    // A self-loop is still impossible in either mode: production rejects every synthetic event by the flag,
    // and the test mode rejects a synthetic one whenever we are inside SendInput -- which is when our own
    // output comes back, because the hook is called during the OS's dispatch of that very event.
    // -------------------------------------------------------------------------
    const bool synthetic = (ms->flags & LLMHF_INJECTED) != 0;
    const bool ourOutput = InterlockedCompareExchange(&g_injecting, 0, 0) != 0;
    const bool acceptSynthetic = InterlockedCompareExchange(&g_acceptInjected, 0, 0) != 0;
    if (!synthetic || (acceptSynthetic && !ourOutput))
    {
      ApexWheelEvent ev;
      ev.delta = (int)(short)HIWORD(ms->mouseData);
      ev.x = ms->pt.x;
      ev.y = ms->pt.y;
      unsigned k = 0;
      if (GetKeyState(VK_SHIFT) & 0x8000) k |= 1u;
      if (GetKeyState(VK_CONTROL) & 0x8000) k |= 2u;
      if (GetKeyState(VK_MENU) & 0x8000) k |= 4u;
      ev.key = k;
      ev.injected = synthetic;
      // THE MESSAGE'S EXTRA-INFO WORD, passed through untouched. It is the one place the OS says which DEVICE
      // sent this wheel -- touch and pen input carries the signature 0xFF515700 -- and the host deliberately
      // does not read it: the classifier is common/device.h, and the feature that cares calls it. See the field
      // note in abi.h (ABI 18 -> 19) and docs/rules/wheel.md.
      ev.extraInfo = (unsigned long long)ms->dwExtraInfo;
      if (g_sink && g_sink(ev, g_sinkUser))
        return 1; // SWALLOW: the host is driving this wheel, so the original must not also arrive
    }
  }
  return CallNextHookEx(g_hook, code, wParam, lParam);
}

bool CaptureStart(WheelSink sink, void *user)
{
  g_sink = sink;
  g_sinkUser = user;
  if (g_hook)
    return true;
  InterlockedExchange(&g_acceptInjected,
                      (GetEnvironmentVariableA("APEX_ACCEPT_INJECTED", nullptr, 0) > 0) ? 1 : 0);
  g_hook = SetWindowsHookExA(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleA(nullptr), 0);
  return g_hook != nullptr;
}

void CaptureStop()
{
  if (g_hook)
  {
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
  }
  g_sink = nullptr;
  g_sinkUser = nullptr;
}

bool CaptureAcceptsInjected()
{
  return InterlockedCompareExchange(&g_acceptInjected, 0, 0) != 0;
}

// ---- target identity ----------------------------------------------------------------------------
namespace {
void BareExeName(const char *full, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  if (!full)
    return;
  const char *name = full;
  for (const char *p = full; *p; ++p)
    if (*p == '\\' || *p == '/')
      name = p + 1;
  int n = 0;
  for (; *name && n < outSize - 1; ++name)
  {
    char c = *name;
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    out[n++] = c;
  }
  out[n] = 0;
}
} // namespace

// IS THIS PROJECT'S REAPER PLUGIN LOADED INTO THAT PROCESS? It has ONE caller now -- the engine probe that answers
// "is REAPER's own smoothing running", which the settings note is built from (see ReaperProbe at the bottom).
//
// ⚠️ THE WHEEL RULE USED TO ASK THIS TOO, AND DELIBERATELY NO LONGER DOES. It used the answer to decide whether to
// hand REAPER over, so a REAPER with the plugin switched off was smoothed by Apex -- and would stop being smoothed
// the moment the user turned the plugin on. The user's rule is the wider one ("这些有独立引擎的，不管它们有没有打开，
// 都是在排除名单内的，不接管"), so that question is now answered by a name in common/engines.h and needs no scan at
// all (see ExternalHandlerState below).
//
// Returns +1 loaded, 0 not loaded, -1 could not tell. The three-way answer still matters HERE: reading another
// process's module list needs rights this program does not request, so a REAPER running elevated refuses -- and for
// a NOTE, "refused" must not be read as "no plugin" (a claim this program cannot support).
//
// Returns +1 loaded, 0 not loaded, -1 could not tell. The three-way answer is the whole point: reading
// another process's module list needs rights this program does not request, so a REAPER running elevated
// refuses -- and "refused" must never be read as "no plugin", because guessing that way leaves two handlers
// driving one view.
static int ScanForPluginModule(unsigned long pid)
{
  if (!pid)
    return -1;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, (DWORD)pid);
  if (snap == INVALID_HANDLE_VALUE)
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE32, (DWORD)pid);
  if (snap == INVALID_HANDLE_VALUE)
    return -1;

  int found = 0;
  MODULEENTRY32W me;
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me))
  {
    do
    {
      if (wcsstr(me.szModule, L"reaper_smoothwheelscroll"))
      {
        found = 1;
        break;
      }
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
  return found;
}

// DOES SOMEBODY ELSE ALREADY OWN THE WHEEL IN THIS PROGRAM?
//
// ⚠️⚠️ A PROGRAM THAT BRINGS ITS OWN SMOOTHING IS ALWAYS LEFT ALONE -- WHETHER OR NOT ITS ENGINE IS SWITCHED ON.
// The user's rule (2026-10-07): "这些有独立引擎的，不管它们有没有打开，都是在排除名单内的，不接管".
//
// The list is common/engines.h, and it is the SAME table the settings note is built from: a program that has its own
// smoothing is a program Apex must not drive, and one list is what keeps the sentence and the behaviour from
// disagreeing. What was here before was narrower -- REAPER, and only when the plugin was actually LOADED by a module
// scan -- which had two costs the user's rule removes: the answer changed while the user worked (turn the plugin on
// and Apex suddenly stops smoothing REAPER), and a REAPER with no plugin was smoothed by Apex although it is a
// program that plainly has its own engine.
//
// ⚠️ IT IS STILL A THREE-WAY ANSWER, AND "UNKNOWN" STILL MEANS "SOMEBODY ELSE HAS IT": with no name to match on
// (the cursor resolved to nothing, or the process could not be read) the wheel is PASSED rather than taken --
// guessing the other way leaves two handlers driving one view (see decision.h).
int ExternalHandlerState(const char *exeName, unsigned long pid, char *detailOut, int detailSize)
{
  if (detailOut && detailSize > 0)
    detailOut[0] = 0;
  if (!exeName || !exeName[0] || !pid)
    return APEX_HANDLER_UNKNOWN;

  if (app::ProgramBringsOwnEngine(exeName))
  {
    if (detailOut && detailSize > 0)
      _snprintf(detailOut, detailSize, "this program brings its own smoothing engine -- always left alone");
    return APEX_HANDLER_PRESENT;
  }
  if (detailOut && detailSize > 0)
    _snprintf(detailOut, detailSize, "no handler is defined for this program");
  return APEX_HANDLER_ABSENT;
}

// ---------------------------------------------------------------------------
// WHICH EXTERNAL SMOOTHING ENGINES ARE RUNNING ON THIS MACHINE, AND IN THE ORDER THEY STARTED.
//
// Two programs bring their own smoothing today, and Apex hands both of them over (see decision.h) -- invisibly,
// which is exactly why the wheel feature says so on its own page:
//
//   * REAPER, through THIS project's plugin. Probed as it always was: the window class first (0.5 us), with the
//     process walk kept as an authoritative backstop every kReaperFullWalkMs.
//   * LERTARO, which ports the same model and publishes a MARKER while it is smoothing: the named event
//     `Local\Lertaro.SmoothScroll.Active`, session-scoped, present exactly while the behaviour is enabled (see
//     Lertaro's App/Helpers/Visuals/SmoothWheelScrollBehavior.cs, and its CHANGELOG: "新增显式标记：命名事件
//     Local\Lertaro.SmoothScroll.Active，平滑滚动启用期间存在").
//     ⚠️ THE MARKER IS THE PROBE, NOT THE PROCESS. A Lertaro with smooth scrolling switched off does not
//     publish the event, and must not be reported: the note explains smoothing the user cannot account for, and
//     a switched-off Lertaro causes none.
//
// ⚠️ IT IS A DIFFERENT QUESTION FROM ExternalHandlerState ABOVE, which asks about the program UNDER THE CURSOR.
// This one asks about the MACHINE, because the note is read while the user is IN the panel -- i.e. exactly when
// neither program is under the pointer.
//
// ⚠️ IT IS ASKED REPEATEDLY (the panel polls it so the note can appear without the user re-opening a page), SO IT
// HAS TO BE CHEAP: walking every process cost 3.59 ms on this machine (252 processes, measured). The window
// lookup and the marker open are both microseconds, and the walks happen only when the SET changes -- see
// ActiveEngines, which also explains where the ORDER comes from.
//
// ⚠️ AND THE "UNKNOWN" DIRECTION IS THE OPPOSITE OF THE SAFETY QUESTION'S. ExternalHandlerState treats "could not
// read" as "somebody else is there", because guessing the other way leaves two handlers moving one view. Here it
// is "not confirmed", because a note that says "running" is a CLAIM, and this program does not make claims it
// cannot support.
// ---------------------------------------------------------------------------
static const DWORD kReaperFullWalkMs = 30000;
static DWORD g_lastReaperFullWalk = 0;
// ⚠️ THE FIRST CALL ALWAYS TAKES THE SLOW ROUTE. Without this, the due-time comparison starts out satisfied
// or unsatisfied depending on how long the machine has been up (GetTickCount is milliseconds since boot, so
// `now - 0` is huge on a machine that has been on for hours and small on one just started) -- the answer would
// depend on when the program was launched. The flag makes "the first answer is the authoritative one"
// independent of that.
static bool g_reaperWalkedOnce = false;

// LERTARO'S MARKER, spelled once. Session-scoped ("Local\") on Lertaro's side on purpose, so neither program
// needs a privilege to create or to look for it.
static const char *kLertaroMarker = "Local\\Lertaro.SmoothScroll.Active";

// IS THE MARKER THERE? Opening the event is the whole probe: it exists exactly while Lertaro has smooth scrolling
// enabled. A missing open is "not running" (the ordinary case), never an error worth reporting.
static bool LertaroMarkerPresent()
{
  HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, kLertaroMarker);
  if (!ev)
    return false;
  CloseHandle(ev);
  return true;
}

// WHEN DID THAT PROCESS START? Read only to ORDER the engines -- the user's rule is "谁先运行谁显式在前面" -- so
// an answer that cannot be read comes back as 0 ("unknown"), which sorts LAST rather than dropping a running
// engine: which one is named first is worth much less than saying that it is running at all.
static unsigned long long ProcessStartMs(unsigned long pid)
{
  if (!pid)
    return 0;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (!h)
    return 0;
  FILETIME create, exitT, kernel, user;
  unsigned long long ms = 0;
  if (GetProcessTimes(h, &create, &exitT, &kernel, &user))
  {
    ULARGE_INTEGER u;
    u.LowPart = create.dwLowDateTime;
    u.HighPart = create.dwHighDateTime;
    // 100-ns ticks since 1601 -> milliseconds since 1970. The epoch shift is the standard constant; only the
    // DIFFERENCE between two of these is ever used, so the unit matters and the epoch does not.
    ms = (u.QuadPart - 116444736000000000ull) / 10000ull;
  }
  CloseHandle(h);
  return ms;
}

// WHICH PROCESS IS LERTARO'S APP? Used ONLY to order it -- never to decide whether it is running, which the marker
// has already answered. ⚠️ THE NAME IS THE APP'S, NOT THE SERVICE'S: this machine also runs Lertaro.Service.exe,
// which is up for as long as the machine is, so a "lertaro*" match would order the engine by the SERVICE's start
// time -- a near-constant that would put Lertaro first for ever.
static unsigned long LertaroAppPid()
{
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;
  unsigned long pid = 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe))
  {
    do
    {
      if (_wcsicmp(pe.szExeFile, L"Lertaro.App.exe") == 0)
      {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return pid;
}

// The authoritative answer: walk every process, find reaper.exe, scan its modules. The pid comes back with it
// because it is what the start time -- and therefore the ORDER the engines are reported in -- is read from.
static int ReaperPluginByProcessWalk(int *sawReaperOut, unsigned long *pidOut)
{
  if (sawReaperOut)
    *sawReaperOut = 0;
  if (pidOut)
    *pidOut = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;

  int running = 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe))
  {
    do
    {
      if (_wcsicmp(pe.szExeFile, L"reaper.exe") != 0)
        continue;
      if (sawReaperOut)
        *sawReaperOut = 1;
      // Found REAPER; now the plugin question. A process that cannot be read answers "unknown", and for a
      // HINT that is treated as "not confirmed" rather than as "yes": the note says the plugin is running, and
      // saying that when it could not be checked would be a claim this program has no evidence for. (The
      // SAFETY question -- whether to hand a wheel over -- faces the other way and treats unknown as present;
      // see ExternalHandlerState. Same scan, opposite default, because the costs of being wrong are opposite.)
      const int found = ScanForPluginModule(pe.th32ProcessID);
      if (found == 1)
      {
        running = 1;
        if (pidOut)
          *pidOut = pe.th32ProcessID;
        break;
      }
      (void)found;
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return running;
}

// REAPER: is the plugin loaded anywhere, and in which process? Both routes exactly as they were -- the cheap one
// first (see the note at the top of this block), the authoritative walk when it is due -- with the pid carried
// out because it is what the order is read from.
static int ReaperProbe(unsigned long *pidOut)
{
  if (pidOut)
    *pidOut = 0;
  const DWORD now = GetTickCount();

  // ---- the fast route: REAPER's window ----
  HWND w = FindWindowA("REAPERwnd", nullptr);
  if (w)
  {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid)
    {
      if (ScanForPluginModule(pid) == 1)
      {
        if (pidOut)
          *pidOut = pid;
        return 1;
      }
      // REAPER is up but the plugin was not found in it (or its modules could not be read). Fall through to
      // the walk, which is due-ordered -- so a REAPER with no plugin costs one walk per half minute, and a
      // REAPER that cannot be read is still checked properly rather than assumed absent.
    }
  }

  // ---- the slow route, only when it is due ----
  if (g_reaperWalkedOnce && (DWORD)(now - g_lastReaperFullWalk) < kReaperFullWalkMs)
    return 0;
  g_lastReaperFullWalk = now;
  g_reaperWalkedOnce = true;

  int sawReaper = 0;
  unsigned long pid = 0;
  const int byWalk = ReaperPluginByProcessWalk(&sawReaper, &pid);
  // ⚠️ THE TWO ROUTES DISAGREEING IS ITSELF WORTH KNOWING -- it means the window class this leans on is no
  // longer how REAPER identifies itself, and the note would have gone quiet. There is no logger here, so the
  // disagreement is reported through the only channel this file has: the answer itself is the walk's (the
  // authoritative one), and the caller's log line will show a result the fast route did not give.
  (void)sawReaper;
  if (byWalk && pidOut)
    *pidOut = pid;
  return byWalk;
}

// ---- the engines: the TABLE is in common/engines.h; this file knows HOW TO PROBE ONE ------------------------
//
// ⚠️ WHY THE SPLIT. The table is the FACT -- "this program has its own smoothing" -- and TWO things need it: the
// wheel rule (such a program is always left alone, see ExternalHandlerState) and this note. So it lives where it
// can be compiled and checked without Windows. What stays here is the OS work: one probe per program, which cannot
// be data, and the order the answer is reported in.
//
// ⚠️ ADDING A PROGRAM: a row in common/engines.h, a probe here, and a case in EngineProbeByKind. No feature is
// touched at all (see ApexEngine in abi.h).
//
// A PROBE ANSWERS TWO THINGS AT ONCE: is it running, and which process should the ORDER be read from. A program
// whose marker is there but whose process cannot be identified reports pid 0, which sorts LAST rather than
// dropping it (see ProcessStartMs).
//
// LERTARO: the named event it publishes while its smoothing is on, plus its App process for the order -- NOT the
// service, which is up for as long as the machine is (see LertaroAppPid).
static bool LertaroProbe(unsigned long *pidOut)
{
  if (pidOut)
    *pidOut = 0;
  if (!LertaroMarkerPresent())
    return false;
  if (pidOut)
    *pidOut = LertaroAppPid();
  return true;
}

static bool EngineProbeByKind(int kind, unsigned long *pidOut)
{
  if (pidOut)
    *pidOut = 0;
  switch (kind)
  {
  case APEX_ENGINE_REAPER:
    return ReaperProbe(pidOut) != 0;
  case APEX_ENGINE_LERTARO:
    return LertaroProbe(pidOut);
  default:
    return false; // an engine this build has no probe for: never claim it is running
  }
}

// ---- the ABI's answer: the engine set, in start order -------------------------------------------
int ActiveEngines(ApexEngine *out, int max)
{
  // NOTE: no logging here -- this file has no logger (LogLine lives in main.cpp, which is the CALLER). There is
  // exactly one caller, the ABI wrapper, and it logs what this returned.
  //
  // ⚠️ EVERY PROBE RUNS ON EVERY CALL, AND THE SET IS WHAT DECIDES WHEN THE ORDER IS RECOMPUTED. Ordering needs
  // each engine's PROCESS START TIME, and finding Lertaro's process means walking the process table (3.59 ms
  // here) -- which the panel's two-second poll would then pay for a sentence that almost never changes. The SET is
  // exactly what changes the sentence, so it is also what makes recomputing the order worth doing.
  int mask = 0;
  for (int i = 0; i < app::kKnownEngineCount; ++i)
    if (EngineProbeByKind(app::kKnownEngines[i].kind, nullptr))
      mask |= 1 << i;

  static int lastMask = -1;
  static ApexEngine ordered[app::kKnownEngineCount];
  static int orderedCount = 0;

  if (mask != lastMask)
  {
    ApexEngine list[app::kKnownEngineCount];
    unsigned long long starts[app::kKnownEngineCount];
    int n = 0;
    for (int i = 0; i < app::kKnownEngineCount; ++i)
    {
      if (!(mask & (1 << i)))
        continue;
      unsigned long pid = 0;
      EngineProbeByKind(app::kKnownEngines[i].kind, &pid); // second call, only when the set changed -- see above
      list[n].kind = app::kKnownEngines[i].kind;
      _snprintf(list[n].name, (int)sizeof(list[n].name), "%s", app::kKnownEngines[i].name);
      starts[n] = ProcessStartMs(pid);
      ++n;
    }
    // EARLIEST FIRST -- the user's rule is "谁先运行谁显式在前面" -- and an unknown start goes LAST. A handful of
    // engines is the whole population, so an insertion sort is all this needs to be.
    for (int i = 1; i < n; ++i)
      for (int j = i; j > 0; --j)
      {
        const bool swap = (starts[j - 1] && starts[j]) ? (starts[j] < starts[j - 1])
                                                       : (starts[j - 1] == 0 && starts[j] != 0);
        if (!swap)
          break;
        const ApexEngine k = list[j - 1];
        list[j - 1] = list[j];
        list[j] = k;
        const unsigned long long t = starts[j - 1];
        starts[j - 1] = starts[j];
        starts[j] = t;
      }
    orderedCount = n;
    for (int i = 0; i < n; ++i)
      ordered[i] = list[i];
    lastMask = mask;
  }

  if (!out || max <= 0)
    return orderedCount;
  const int n = (orderedCount < max) ? orderedCount : max;
  for (int i = 0; i < n; ++i)
    out[i] = ordered[i];
  return n;
}

bool TargetUnderCursor(int x, int y, char *exeOut, int exeSize, unsigned long *pidOut, void **rootOut)
{
  if (exeOut && exeSize > 0)
    exeOut[0] = 0;
  if (pidOut)
    *pidOut = 0;
  if (rootOut)
    *rootOut = nullptr;

  POINT pt = {x, y};
  HWND w = WindowFromPoint(pt);
  if (!w)
    return false;
  // The TOP-LEVEL window the point sits in -- see the note in host.h: for a hosted browser this is a
  // different process from the window WindowFromPoint hands back, and it is the one that names the
  // application the user is pointing at.
  if (rootOut)
    *rootOut = GetAncestor(w, GA_ROOT);
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (!pid)
    return false;
  if (pidOut)
    *pidOut = pid;

  if (exeOut && exeSize > 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h)
    {
      char full[MAX_PATH] = {0};
      DWORD n = (DWORD)sizeof(full);
      if (QueryFullProcessImageNameA(h, 0, full, &n))
        BareExeName(full, exeOut, exeSize);
      CloseHandle(h);
    }
  }
  return exeOut && exeOut[0] != 0;
}

// ---- injection ----------------------------------------------------------------------------------
//
// WHY A THREAD AND NOT THE CLOCK: SendInput does not return until the event it injected has been
// dispatched, and dispatching runs on the target's message loop. Called from a message-driven call, it
// therefore waits for a loop that is itself blocked, and the wait ends in a timeout -- measured here at
// 1001 ms with the calling code using 0.0 ms of its own. That single fact produced the whole "stuck then
// lurch" symptom. On its own thread there is no loop to block: the producer pushes and returns.
namespace {
const int kInjectQueueMax = 64;
int g_injectQ[kInjectQueueMax];
int g_injectHead = 0;
int g_injectCount = 0;
long g_injectDropped = 0;
HANDLE g_injectEvent = nullptr;
HANDLE g_injectThread = nullptr;
volatile LONG g_injectStop = 0;
volatile LONG g_lastInjectUs = 0;

double NowSeconds()
{
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (f.QuadPart > 0) ? (double)c.QuadPart / (double)f.QuadPart : 0.0;
}

bool InjectWheel(int delta)
{
  if (delta == 0)
    return false;
  INPUT in;
  ZeroMemory(&in, sizeof(in));
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = MOUSEEVENTF_WHEEL;
  in.mi.mouseData = (DWORD)(LONG)delta;
  const double t0 = NowSeconds();
  InterlockedExchange(&g_injecting, 1);
  const UINT sent = SendInput(1, &in, sizeof(in));
  InterlockedExchange(&g_injecting, 0);
  InterlockedExchange(&g_lastInjectUs, (LONG)((NowSeconds() - t0) * 1e6));
  return sent == 1;
}

DWORD WINAPI InjectThreadProc(LPVOID)
{
  for (;;)
  {
    WaitForSingleObject(g_injectEvent, INFINITE);
    if (InterlockedCompareExchange(&g_injectStop, 0, 0) != 0)
      return 0;
    for (;;)
    {
      int d = 0;
      bool have = false;
      if (g_injectCount > 0)
      {
        d = g_injectQ[g_injectHead];
        g_injectHead = (g_injectHead + 1) % kInjectQueueMax;
        --g_injectCount;
        have = true;
      }
      if (!have)
        break;
      InjectWheel(d);
    }
  }
}
} // namespace

void InjectQueuePush(int delta)
{
  if (delta == 0 || !g_injectThread)
    return;
  if (g_injectCount >= kInjectQueueMax)
  {
    // The target is slower than the motion. Dropping the OLDEST keeps what is delivered current rather
    // than replaying a stale backlog; the count is kept so this is visible rather than silent.
    g_injectHead = (g_injectHead + 1) % kInjectQueueMax;
    --g_injectCount;
    ++g_injectDropped;
  }
  g_injectQ[(g_injectHead + g_injectCount) % kInjectQueueMax] = delta;
  ++g_injectCount;
  SetEvent(g_injectEvent);
}

bool InjectThreadStart()
{
  if (g_injectThread)
    return true;
  g_injectEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr); // auto-reset
  if (!g_injectEvent)
    return false;
  g_injectStop = 0;
  g_injectThread = CreateThread(nullptr, 0, InjectThreadProc, nullptr, 0, nullptr);
  return g_injectThread != nullptr;
}

void InjectThreadStop()
{
  if (!g_injectThread)
    return;
  InterlockedExchange(&g_injectStop, 1);
  SetEvent(g_injectEvent); // wake it so it sees the flag
  WaitForSingleObject(g_injectThread, 2000);
  CloseHandle(g_injectThread);
  g_injectThread = nullptr;
  if (g_injectEvent)
  {
    CloseHandle(g_injectEvent);
    g_injectEvent = nullptr;
  }
}

long LastInjectMicros()
{
  return (long)InterlockedCompareExchange(&g_lastInjectUs, 0, 0);
}

// ---- the clock ----------------------------------------------------------------------------------
//
// A HIGH-RESOLUTION WAITABLE TIMER, not SetTimer: a plain window timer is clamped to the system tick,
// measured at 15.6 ms on this machine no matter what period is requested. At 4 ms the model hands over
// ~0.75 deltas per frame, which is what makes the motion read as continuous; at 15.6 ms it is 13-18 and
// the steps are visible.
namespace {
EngineFn g_engineFn = nullptr;
void *g_engineUser = nullptr;
HANDLE g_engineTimer = nullptr;
HANDLE g_engineThread = nullptr;
HANDLE g_engineStopEvent = nullptr;
double g_enginePeriodMs = 4.0;
volatile LONG g_engineStop = 0;

DWORD WINAPI EngineProc(LPVOID)
{
  LARGE_INTEGER due;
  due.QuadPart = 0;
  SetWaitableTimer(g_engineTimer, &due, (LONG)g_enginePeriodMs, nullptr, nullptr, FALSE);
  HANDLE waits[2] = {g_engineTimer, g_engineStopEvent};
  for (;;)
  {
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0 + 1)
      break;
    if (w == WAIT_OBJECT_0)
    {
      if (InterlockedCompareExchange(&g_engineStop, 0, 0) != 0)
        break;
      if (g_engineFn)
        g_engineFn(g_engineUser);
    }
  }
  return 0;
}
} // namespace

bool EngineStart(EngineFn fn, void *user, double periodMs)
{
  if (g_engineThread)
    return true;
  g_engineFn = fn;
  g_engineUser = user;
  g_enginePeriodMs = periodMs;
  if (g_enginePeriodMs < 1.0)
    g_enginePeriodMs = 1.0;

  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is Windows 10 1803+. Where it is missing, the ordinary timer
  // is used: coarser, but the panel still moves and the failure is a feel regression rather than a
  // broken program.
  g_engineTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  if (!g_engineTimer)
    g_engineTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  if (!g_engineTimer)
    return false;

  g_engineStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_engineStopEvent)
  {
    CloseHandle(g_engineTimer);
    g_engineTimer = nullptr;
    return false;
  }
  g_engineStop = 0;
  g_engineThread = CreateThread(nullptr, 0, EngineProc, nullptr, 0, nullptr);
  if (!g_engineThread)
  {
    CloseHandle(g_engineStopEvent);
    g_engineStopEvent = nullptr;
    CloseHandle(g_engineTimer);
    g_engineTimer = nullptr;
    return false;
  }
  return true;
}

void EngineStop()
{
  if (!g_engineThread)
    return;
  InterlockedExchange(&g_engineStop, 1);
  SetEvent(g_engineStopEvent);
  WaitForSingleObject(g_engineThread, 2000);
  CloseHandle(g_engineThread);
  g_engineThread = nullptr;
  if (g_engineTimer)
  {
    CloseHandle(g_engineTimer);
    g_engineTimer = nullptr;
  }
  if (g_engineStopEvent)
  {
    CloseHandle(g_engineStopEvent);
    g_engineStopEvent = nullptr;
  }
}

double NowSecondsPublic() { return NowSeconds(); }

} // namespace host
} // namespace apex
