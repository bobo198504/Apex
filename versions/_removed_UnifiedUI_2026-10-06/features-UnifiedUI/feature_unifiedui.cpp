// ---------------------------------------------------------------------------
// UnifiedUI -- MAKE OLD WINDOWS FOLLOW THE SYSTEM'S LIGHT/DARK, LIKE WINDOWS 11's OWN WINDOWS DO.
//
// WHAT THE USER ASKED FOR, in their own words: "做一个系统全局的UI统一化插件，就是把win32旧UI统一变成新系统UI
// 的样子", then "偏向方案一，兼容性尽量做好，控件不是问题，主要是标题栏和菜单有效果", and finally -- and this
// one decided the whole design -- "有些Win32也会有明暗主题，如果原生有，就不接管明暗，只做材质接管，如果没有，
// 再做明暗。明暗随系统设置。"
//
// ⚠️⚠️ EVERY CLAIM BELOW WAS MEASURED BEFORE A LINE OF THIS WAS WRITTEN. The instrument is _diag/ui_probe.cpp
// (its log and its numbered pictures are the evidence), and the numbers are written up in
// docs/rules/features.md. The five that shape this file:
//
//   1. A DIFFERENT PROCESS CAN DO IT. DwmSetWindowAttribute accepts a window this process does not own, and the
//      resulting picture is byte-for-byte identical to setting it from inside that process. THE ENTIRE PLAN
//      RESTS ON THIS, and it is why there is no injection anywhere in this feature -- no DLL pushed into
//      anybody, no hook in anybody's process.
//   2. IT TAKES ONE CALL AND NOTHING ELSE. The dark flag took effect with NO forced redraw at all:
//      RedrawWindow(RDW_FRAME) and SetWindowPos(SWP_FRAMECHANGED) each produced a picture IDENTICAL to doing
//      neither. That is worth more than the saved line of code -- SWP_FRAMECHANGED sends WM_NCCALCSIZE into
//      the TARGET program, and a program that relayouts on it would flicker or worse. THIS FEATURE NEVER
//      CALLS IT, and the fact that it does not have to is the measurement, not a preference.
//   3. AN OLD PROGRAM REALLY DOES NOT FOLLOW THE SYSTEM BY ITSELF. A window that nothing had touched read back
//      dark=0 while the system was dark, and its title bar was photographed LIGHT. If Windows already did this,
//      this feature would have nothing to do; it does not.
//   4. A DIFFERENT PROCESS CAN ALSO READ IT. DwmGetWindowAttribute answers with the window's CURRENT value
//      across processes. That is what turns "do not touch a program that already has its own light/dark" from
//      a guess into a comparison.
//   5. THE MENU BAR CANNOT BE REACHED FROM HERE. A window's menu bar is not a window -- USER32 draws it as
//      part of the non-client area -- and SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr) changed not one
//      pixel of it (the picture after the call was byte-for-byte the picture before it, with the title bar
//      already dark and the menu bar still white). Popup menus are a different thing and Windows 11 already
//      draws those in its own new style. So this feature does NOT do menus, and that is a measurement rather
//      than an omission: reaching a menu bar needs code inside the other program, which is the injection this
//      whole approach exists to avoid.
//
// WHAT IT THEREFORE DOES: for every top-level window that has a caption, it compares the window's own
// dark-mode value with the system's, and writes the system's value ONLY WHEN THE TWO DISAGREE. A program that
// already follows the system is left completely alone -- which is the user's requirement above, satisfied by
// the comparison rather than by a list of program names.
//
// ⚠️ AND THE ONE CASE A COMPARISON CANNOT RESOLVE, SAID PLAINLY RATHER THAN HIDDEN: a program that HAS its own
// light/dark setting and is deliberately the opposite of the system reads exactly like a program with no
// opinion at all -- both answer "dark=0" while the system is dark. This feature will change the first one,
// because "never set" and "deliberately set to the opposite" are the same value on the wire and there is no
// third source to ask. THAT IS WHAT THE EXCLUDE LIST IS FOR, and it is why the list belongs to this feature
// rather than to the host (AGENTS.md: a list like this is about this feature's job, not about Apex).
//
// WHAT IT DELIBERATELY DOES NOT DO, each for a reason that was measured or is structural:
//   * NO Mica / ACRYLIC. A material is drawn by DWM BEHIND a window, and an ordinary program paints its whole
//     client area -- so on a real old program the material is invisible. The probe put it on a window with a
//     painted client area and photographed no change. Adding it would cost every touched window a DWM material
//     to composite (measured in _diag/mica_probe.cpp) in exchange for nothing visible.
//   * NO ROUNDED CORNERS. Windows 11 already rounds a top-level window by default; asking for DWMWCP_ROUND
//     produced a picture identical to not asking (measured), because the default already IS round.
//   * NO INJECTION, and therefore no menu bar, no control repainting, and nothing that a game's anti-cheat or
//     an antivirus would have an opinion about.
//
// HOW IT WATCHES: SetWinEventHook(EVENT_OBJECT_SHOW) on its own thread, plus one EnumWindows sweep at startup
// and one whenever the SYSTEM's theme changes (a window that was already open still has to follow). The hook
// is OUTOFCONTEXT, so the callback arrives on this feature's own thread, where the message pump is -- nothing
// here runs in the input path and nothing here can slow the wheel down.
// ---------------------------------------------------------------------------

#include "../../apex/abi.h"
#include "../../common/match.h"
#include "../../common/system_theme.h"

#include <windows.h>
#include <stdio.h>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

namespace {

const char *kId = "UnifiedUI";
const char *kVersion = "1.0.0";

const int kMaxList = 64;   // excluded program patterns, this feature's own list
const int kMaxName = 64;   // one name or pattern, lower-case, without a path
const int kPumpMs = 250;   // how long the worker waits for a message before looking around again
const int kThemePollMs = 2000; // how often the SYSTEM's own theme is re-read (a registry read: not per event)

// ---- the attributes, declared here rather than included -----------------------------------------------
//
// MinGW's dwmapi.h on this toolchain predates them (see _diag/mica_probe.cpp and _diag/ui_probe.cpp, which
// declare the same numbers for the same reason). The caption number differs between Windows builds, so both
// spellings are carried and the old one is tried when the new one fails.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20

// ROUNDING A MENU -- and it is the ONE piece of the system's own look an old program's popup menu can be
// given from outside. MEASURED (_diag/dark_probe.cpp, with pictures): this call changes the menu's pixels,
// while DWMWA_SYSTEMBACKDROP_TYPE and the user32 accent both change EXACTLY ZERO of them -- "round corners +
// material" came out byte-for-byte identical to "round corners alone". The reason is structural and worth
// keeping in mind before anyone tries again: a material is drawn BEHIND a window, and a menu paints its own
// opaque background over its whole rectangle. A corner is drawn by DWM AROUND the window, which is why it is
// the one that survives.
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_ROUND 2

typedef HRESULT(WINAPI *DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT(WINAPI *DwmGetFn)(HWND, DWORD, LPVOID, DWORD);

// Loaded on demand rather than linked: the header that would declare these is missing the attributes anyway,
// and a feature that failed to load on a machine without dwmapi would be a feature that vanished (Windows 8+
// always has it, but the failure mode is worth not having).
DwmSetFn DwmSet()
{
  static DwmSetFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE d = LoadLibraryA("dwmapi.dll"))
      fn = (DwmSetFn)(void *)GetProcAddress(d, "DwmSetWindowAttribute");
  }
  return fn;
}

DwmGetFn DwmGet()
{
  static DwmGetFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE d = LoadLibraryA("dwmapi.dll"))
      fn = (DwmGetFn)(void *)GetProcAddress(d, "DwmGetWindowAttribute");
  }
  return fn;
}

// ---------------------------------------------------------------------------
// THE SETTINGS. Written by the panel's thread (listOp / reloadSettings), read by the worker, so every access
// is under `g_lock` -- and the worker COPIES what it needs out before doing anything slow.
// ---------------------------------------------------------------------------
struct Settings
{
  int n = 0;
  char ex[kMaxList][kMaxName]; // bare lower-case file names or patterns, as the user typed them

  // ROUND THE MENUS. On by default: it is the cheapest thing this feature does, it needs no injection, and
  // it is what makes an old program's popup menu look like the ones Windows 11 draws for itself.
  bool roundMenus = true;

  // ⚠️⚠️ HERE IS THE ONE THAT WAS BUILT, SHIPPED, AND THEN REMOVED -- TOGETHER WITH THE MEASUREMENT THAT
  // REMOVED IT, because "we tried translucent menus and it did not work" is worth more than the code was.
  //
  // A translucent menu was implemented through WS_EX_LAYERED + SetLayeredWindowAttributes, and it did nothing
  // on a real program. The reason is not a parameter. A real menu window carries WS_EX_LAYERED *and* uses
  // UpdateLayeredWindow, where the alpha is baked into the bitmap the menu submits: measured on renamer.exe's
  // own menu, `GetLayeredWindowAttributes` FAILS on it (`layered=1 readable=0`), and SetLayeredWindowAttributes
  // is a mutually exclusive mode. An alpha set from outside only works in the sliver of time before the menu
  // manager takes the window over -- i.e. by winning a race -- and a race is not a feature.
  //
  // The other two paths are settled as well: a MATERIAL (DWMWA_SYSTEMBACKDROP_TYPE) and the user32 accent each
  // change EXACTLY ZERO pixels of a menu, because both are drawn BEHIND the window while a menu paints its own
  // background over its whole rectangle.
  //
  // THE RULE THIS LEAVES BEHIND: a corner can be given to somebody else's menu from outside, because DWM draws
  // it AROUND the window. Transparency cannot, because it IS the window's own pixels. See §3.14.8.
};

Settings g_set;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

const ApexHost *g_host = nullptr;
char g_dir[512] = {0};

HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;
volatile LONG g_stop = 0;
HWINEVENTHOOK g_hook = nullptr;

// ⚠️ THE SYSTEM'S OWN ANSWER, CACHED, BECAUSE THE HOOK CALLBACK MUST NOT READ THE REGISTRY. The callback runs
// once per window that appears anywhere on the machine; a registry read in there would be paid thousands of
// times a session for a value that changes when the user clicks a settings toggle. The worker refreshes it,
// and the callback only reads this word.
volatile LONG g_systemDark = 1;

// Counters for the log's summary line. Not a report the panel draws -- the ABI has no field for it -- but the
// one thing that answers "is this feature doing anything at all", which is otherwise invisible: the effect is
// a title bar colour nobody watches change.
volatile LONG g_seen = 0;
volatile LONG g_changed = 0;

// ⚠️ IS THE HOST'S SWITCH FOR THIS FEATURE ON, AND DID THE SETTINGS CHANGE? Both are written by the panel's
// thread and read by the worker's, which is why they are interlocked.
volatile LONG g_live = 0;       // 1 = the plugin list's switch for this feature is on
volatile LONG g_reevaluate = 0; // 1 = the settings changed; look at every window again

// ---------------------------------------------------------------------------
// WHAT THIS FEATURE HAS CHANGED, SO THAT IT CAN BE CHANGED BACK.
//
// WHY A RECORD HAS TO EXIST, AND WHY THE OBVIOUS ALTERNATIVE IS WRONG. "Put back the windows I changed" sounds
// like something that could be worked out from the windows themselves -- a window whose dark-mode value equals
// the system's looks like a window this feature set. It does not: a program that follows the system BY ITSELF
// reads exactly the same, so that rule would reach into windows this feature never touched and push them the
// other way, which is the one thing the user asked it not to do. The only thing that knows is a record.
//
// ⚠️ THREADING. This array is touched by the WORKER THREAD ONLY -- every write happens inside ApplyTo, and
// every caller of ApplyTo (the event hook, the sweep, the theme re-read) runs on that one thread. The panel's
// thread never reads it: it sets `g_reevaluate` and wakes the worker, which is what makes "the list takes
// effect at once" safe without a second lock. THE ONE EXCEPTION IS SHUTDOWN, which reads it from the host's
// thread AFTER waiting for the worker to stop (see the note there).
// ---------------------------------------------------------------------------
const int kMaxTouched = 1024;
struct Touched
{
  HWND h;
  DWORD pid;
};
Touched g_touched[kMaxTouched];
int g_touchedN = 0;

FILE *g_log = nullptr;

void Log(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[300] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_host->logLine(buf);
}

// The feature's own log, beside itself, rewritten each run. WHY IT EXISTS: the effect is a colour on somebody
// else's title bar, and "it did nothing" and "it did the wrong thing" look the same from inside the program
// unless the decisions are written down. Only CHANGES are logged (see ApplyTo) -- a line per window looked at
// would bury the handful that matter, which is the mistake AutoIME made once.
void OpenLog()
{
  if (!g_dir[0])
    return;
  char path[560] = {0};
  if (_snprintf(path, sizeof(path), "%sunifiedui.log", g_dir) <= 0)
    return;
  g_log = fopen(path, "w");
  if (g_log)
    setvbuf(g_log, nullptr, _IOLBF, 0); // line-buffered: a crash must not eat the last decision
}

void FileLog(const char *fmt, ...)
{
  if (!g_log)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);

  // ⚠️⚠️ FLUSHED ON EVERY LINE, AND THIS IS NOT TIDINESS -- IT IS THE WHOLE VALUE OF THE FILE.
  //
  // `OpenLog` asks for line buffering (`setvbuf(..., _IOLBF, ...)`) and that is NOT ENOUGH HERE: the case this
  // log exists for is a host that was KILLED rather than closed, and a killed process never runs the `fclose`
  // that would flush a full buffer. Measured on the deployed copy: the file was 0 bytes -- created by `fopen`,
  // and empty because every line was still sitting in the buffer when the process died. That is exactly the
  // "there is no evidence at all" outcome this file is supposed to prevent, and it is why
  // _diag/mica_probe.cpp and features/KeepAwake both flush every line too.
  fflush(g_log);
}

// IS THIS FEATURE SWITCHED ON IN THE PLUGIN LIST? Answered from state the host keeps, so it is safe to call
// from this feature's own thread -- which is the only place that can ask (see the note on `featureEnabled` in
// apex/abi.h).
bool EnabledByHost()
{
  if (!g_host || !g_host->featureEnabled)
    return true; // nothing to ask (a stub host, and the gates) -> behave as if it is on
  return g_host->featureEnabled(kId) != 0;
}

// TELL THE WORKER THAT THE SETTINGS CHANGED, AND WAKE IT so that "at once" means at once rather than within a
// slice. Safe from the panel's thread: it sets a word and signals an event, and touches nothing the worker
// owns.
void AskForReevaluation()
{
  InterlockedExchange(&g_reevaluate, 1);
  if (g_wake)
    SetEvent(g_wake);
}

// ---- the settings file ---------------------------------------------------------------------------------

void LoadSettings()
{
  int n = 0;
  bool roundMenus = true; // the default, unless the file says otherwise
  char tmp[kMaxList][kMaxName];
  char path[560] = {0};
  if (g_dir[0])
    _snprintf(path, sizeof(path), "%sunifiedui.ini", g_dir);
  if (path[0])
  {
    if (FILE *f = fopen(path, "rb"))
    {
      char line[256] = {0};
      while (fgets(line, sizeof(line), f))
      {
        // ⚠️ ONLY `exclude=` IS KNOWN HERE. Anything else is skipped rather than refused: every removed
        // setting in this project's features behaves that way, so an old file keeps working and the user's
        // next save rewrites it in the current shape.
        int rv = -1;
        if (sscanf(line, "round_menus=%d", &rv) == 1)
        {
          roundMenus = (rv != 0);
          continue;
        }

        char raw[kMaxName * 2] = {0};
        if (sscanf(line, "exclude=%127s", raw) == 1 && n < kMaxList)
        {
          char norm[kMaxName] = {0};
          apex::match::NormaliseName(raw, norm, (int)sizeof(norm));
          if (norm[0])
          {
            bool dup = false;
            for (int i = 0; i < n; ++i)
              if (strcmp(tmp[i], norm) == 0)
                dup = true;
            if (!dup)
              _snprintf(tmp[n++], kMaxName, "%s", norm);
          }
        }
      }
      fclose(f);
    }
  }

  EnterCriticalSection(&g_lock);
  g_set.n = n;
  g_set.roundMenus = roundMenus;
  for (int i = 0; i < n; ++i)
    _snprintf(g_set.ex[i], kMaxName, "%s", tmp[i]);
  LeaveCriticalSection(&g_lock);
}

bool SaveSettings()
{
  if (!g_dir[0])
    return false;
  char path[560] = {0};
  _snprintf(path, sizeof(path), "%sunifiedui.ini", g_dir);
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);
  fprintf(f, "# UnifiedUI -- programs this feature leaves alone.\n");
  fprintf(f, "# A bare name must match exactly; '*' and '?' are wildcards, anchored at both ends,\n");
  fprintf(f, "# so `game.exe` does not match `mygame.exe` but `game*` does.\n");
  fprintf(f, "round_menus=%d\n", s.roundMenus ? 1 : 0);
  for (int i = 0; i < s.n; ++i)
    fprintf(f, "exclude=%s\n", s.ex[i]);
  fclose(f);
  return true;
}

// Is this program on the user's list? A COPY of the list is taken first, because this is asked from the
// worker's thread while the panel's thread may be editing it.
bool IsExcluded(const char *exe)
{
  if (!exe || !exe[0])
    return false;
  char pats[kMaxList][kMaxName];
  int n = 0;
  EnterCriticalSection(&g_lock);
  n = g_set.n;
  for (int i = 0; i < n; ++i)
    _snprintf(pats[i], kMaxName, "%s", g_set.ex[i]);
  LeaveCriticalSection(&g_lock);
  for (int i = 0; i < n; ++i)
    if (apex::match::Pattern(pats[i], exe))
      return true;
  return false;
}

int ListOp(const char *id, const char *op, const char *value, int index)
{
  if (!id || strcmp(id, "exclude") != 0 || !op)
    return 0;

  if (strcmp(op, "add") == 0)
  {
    char norm[kMaxName] = {0};
    apex::match::NormaliseName(value, norm, (int)sizeof(norm));
    if (!norm[0])
      return 0;
    EnterCriticalSection(&g_lock);
    bool dup = false;
    for (int i = 0; i < g_set.n; ++i)
      if (strcmp(g_set.ex[i], norm) == 0)
        dup = true;
    const bool room = g_set.n < kMaxList;
    if (!dup && room)
      _snprintf(g_set.ex[g_set.n++], kMaxName, "%s", norm);
    LeaveCriticalSection(&g_lock);
    // ⚠️ A NAME GOING ON TO THE LIST HAS TO REACH THE WINDOWS ALREADY CHANGED, and this is that. Without it,
    // "the exclude list takes effect at once" would be true only of windows that had not appeared yet -- and
    // the window the user is looking at, the one that made them open the settings page, would keep its new
    // title bar until it was reopened.
    const bool changed = (!dup && room);
    if (changed)
      AskForReevaluation();
    return changed ? 1 : 0;
  }

  if (strcmp(op, "remove") == 0)
  {
    bool ok = false;
    EnterCriticalSection(&g_lock);
    if (index >= 0 && index < g_set.n)
    {
      for (int i = index; i < g_set.n - 1; ++i)
        _snprintf(g_set.ex[i], kMaxName, "%s", g_set.ex[i + 1]);
      --g_set.n;
      ok = true;
    }
    LeaveCriticalSection(&g_lock);
    // A name coming OFF the list needs no restoring: the sweep that follows treats that program like any
    // other and will set it again if it disagrees with the system.
    if (ok)
      AskForReevaluation();
    return ok ? 1 : 0;
  }

  // Reordering an exclude list is not meaning: the answer is "is this name on the list", and that does not
  // depend on the order. So move-up / move-down are refused rather than quietly accepted.
  return 0;
}

// ---- which windows are worth touching ------------------------------------------------------------------

// ⚠️ THE FILTER IS THE WHOLE OF "兼容性尽量做好", so each rule is here for a reason rather than for symmetry.
//
// The order matters too: the cheap window-style questions are asked before the process is opened, because a
// sweep over every window on the machine happens at startup and on every theme change.

// Whole-monitor windows are skipped: a full-screen game or video is exactly the thing whose title bar nobody
// wants touched, and it is the one case where a wrong decision is loud.
//
// ⚠️ `IsZoomed` IS PART OF THE TEST AND NOT AN AFTERTHOUGHT. On Windows 10 and 11 a MAXIMISED window's
// GetWindowRect extends past the work area by the width of its invisible resize border, so "the rectangle
// covers the monitor" is true for both a maximised window and a full-screen one. Maximised windows are the
// most ordinary thing on a desktop and must NOT be skipped, so the maximised state is excluded explicitly.
bool CoversWholeMonitor(HWND h)
{
  if (IsZoomed(h) || IsIconic(h))
    return false;
  RECT r;
  if (!GetWindowRect(h, &r))
    return false;
  const HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {0};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi))
    return false;
  const RECT &m = mi.rcMonitor;
  return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
}

// The bare lower-case executable name of the process owning `pid`, or false when it cannot be read.
//
// ⚠️ "CANNOT BE READ" IS NOT "NO NAME": an unreadable process is almost always one at a higher integrity
// level, and a window of such a process cannot be restyled from here anyway (the same permission boundary
// stops the write). So the honest answer is to leave it alone, and the caller does.
bool ProcessName(DWORD pid, char *out, int outSize)
{
  out[0] = 0;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return false;
  wchar_t wide[MAX_PATH] = {0};
  DWORD n = MAX_PATH;
  const bool ok = QueryFullProcessImageNameW(p, 0, wide, &n) != FALSE;
  CloseHandle(p);
  if (!ok)
    return false;
  char utf8[MAX_PATH * 2] = {0};
  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)sizeof(utf8), nullptr, nullptr) <= 0)
    return false;
  apex::match::NormaliseName(utf8, out, outSize);
  return out[0] != 0;
}

bool ShouldTouch(HWND h, char *exe, int exeSize, DWORD *pidOut)
{
  exe[0] = 0;
  if (pidOut)
    *pidOut = 0;

  // Top-level only. The hook reports child windows too (a button appearing is an EVENT_OBJECT_SHOW), and a
  // child has no title bar of its own to speak of.
  if (GetAncestor(h, GA_ROOT) != h)
    return false;
  if (!IsWindowVisible(h))
    return false;

  // A window has to HAVE a caption: DWMWA_USE_IMMERSIVE_DARK_MODE is about the title bar, and setting it on a
  // borderless tool window is asking for something that is not there.
  const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
  if ((style & WS_CAPTION) != WS_CAPTION)
    return false;

  // The desktop and the shell's own surfaces are not windows in the sense meant here.
  if (h == GetShellWindow() || h == GetDesktopWindow())
    return false;

  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;

  // ⚠️ OUR OWN TWO PROGRAMS ARE NEVER TOUCHED, AND THE PANEL IS THE ONE THAT MATTERS. apex-settings.exe can be
  // PINNED to light or dark AGAINST the system -- _diag/mica_probe.cpp exists because of that question -- so a
  // feature that "helpfully" made the panel follow the system would be overruling a setting the user had just
  // made inside it. The host's own window has no caption, but it is named here as well so that the rule reads
  // as "our programs", not as a coincidence about one of them.
  if (!ProcessName(pid, exe, exeSize))
    return false; // unreadable -> leave it alone (see the note on ProcessName)
  if (strcmp(exe, "apex.exe") == 0 || strcmp(exe, "apex-settings.exe") == 0)
    return false;

  if (IsExcluded(exe))
    return false;

  if (CoversWholeMonitor(h))
    return false;

  // The pid is handed back so the caller can REMEMBER it: putting a window back later needs to know that the
  // handle still belongs to the same process, because a window handle is reused after its window is destroyed.
  if (pidOut)
    *pidOut = pid;
  return true;
}

// ---- putting windows back, which is what makes the switch and the list take effect at once ---------------

// Remember one window. Called only after a write SUCCEEDED, so the list is exactly "windows this feature has
// changed and has not put back".
void RememberTouched(HWND h, DWORD pid)
{
  for (int i = 0; i < g_touchedN; ++i)
    if (g_touched[i].h == h)
      return; // already known: the same window is re-judged after a theme change
  if (g_touchedN >= kMaxTouched)
  {
    // ⚠️ A FULL ARRAY IS NOT "FORGET THE OLDEST". Dropping a record would leave that window changed with
    // nothing left that knows how to change it back -- exactly the leftover this list exists to prevent. The
    // NEWEST is left unrecorded instead, and it is said once rather than silently.
    static bool said = false;
    if (!said)
    {
      said = true;
      FileLog("NOTE: the record is full (%d windows); windows changed from now on will not be put back "
              "automatically",
              kMaxTouched);
    }
    return;
  }
  g_touched[g_touchedN].h = h;
  g_touched[g_touchedN].pid = pid;
  ++g_touchedN;
}

// Forget one window: its window is being destroyed, so the handle is about to become reusable by something
// else, and a stale record is a record pointing at a stranger.
void ForgetTouched(HWND h)
{
  for (int i = 0; i < g_touchedN; ++i)
    if (g_touched[i].h == h)
    {
      g_touched[i] = g_touched[--g_touchedN];
      return;
    }
}

bool RestoreOne(HWND h, DWORD pid)
{
  // A window handle is REUSED once its window is gone, so "is this still the window I changed" is two
  // questions: does it still exist, and is it still that process's.
  if (!IsWindow(h))
    return false;
  DWORD now = 0;
  GetWindowThreadProcessId(h, &now);
  if (now != pid)
    return false;

  DwmGetFn get = DwmGet();
  DwmSetFn set = DwmSet();
  if (!get || !set)
    return false;

  BOOL cur = (BOOL)-1;
  HRESULT hr = get(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &cur, sizeof(cur));
  if (FAILED(hr))
  {
    cur = (BOOL)-1;
    hr = get(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &cur, sizeof(cur));
  }
  if (FAILED(hr))
    return false;

  const BOOL want = (InterlockedCompareExchange(&g_systemDark, 0, 0) != 0) ? TRUE : FALSE;

  // ⚠️⚠️ ONLY IF THE WINDOW STILL HOLDS WHAT THIS FEATURE PUT THERE. If it does not, what is on it now is
  // somebody else's decision -- the program set its own value, or the user switched the system theme and the
  // program followed -- and writing over that would be this feature reaching in a SECOND time, which is exactly
  // what the comparison in ApplyTo exists to avoid.
  if (cur != want)
    return false;

  const BOOL back = want ? FALSE : TRUE; // what it held before this feature touched it
  HRESULT sh = set(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &back, sizeof(back));
  if (FAILED(sh))
    sh = set(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &back, sizeof(back));
  return SUCCEEDED(sh);
}

// Everything this feature changed. Used when it is switched off, and when the program is closing.
int RestoreAll()
{
  int n = 0;
  for (int i = 0; i < g_touchedN; ++i)
    if (RestoreOne(g_touched[i].h, g_touched[i].pid))
      ++n;
  g_touchedN = 0;
  return n;
}

// Only the windows whose program has JUST BEEN EXCLUDED -- the half of "the list takes effect at once" that
// adding a name is about. The other half (a name REMOVED) needs no restoring: the sweep that follows reaches
// those windows like any other.
int RestoreExcluded()
{
  int n = 0;
  for (int i = 0; i < g_touchedN;)
  {
    char exe[kMaxName] = {0};
    // An unreadable name is not a reason to put a window back: this feature only ever changed windows whose
    // program it could name, so the name is normally there.
    if (!ProcessName(g_touched[i].pid, exe, (int)sizeof(exe)) || !IsExcluded(exe))
    {
      ++i;
      continue;
    }
    if (RestoreOne(g_touched[i].h, g_touched[i].pid))
      ++n;
    g_touched[i] = g_touched[--g_touchedN];
  }
  return n;
}

// ---- THE MENU WINDOW, WHICH IS A WINDOW LIKE ANY OTHER ------------------------------------------------
//
// A popup menu is a real top-level window of class "#32768" -- so the same DWM calls that restyle an ordinary
// window can be aimed at it, from this process, with no injection at all. What they DO is a separate question
// and it has been measured: corners work, materials do not (see the note on DWMWA_WINDOW_CORNER_PREFERENCE).
//
// ⚠️ THIS RUNS BEFORE THE WINDOW FILTER, NOT THROUGH IT. ShouldTouch insists on WS_CAPTION -- "only windows
// with a title bar" -- and a menu has none, so the filter would reject every menu on the machine before this
// ever ran. The two kinds of window want two different things, so they get two different paths.
bool IsMenuWindow(HWND h)
{
  wchar_t cls[32] = {0};
  if (GetClassNameW(h, cls, (int)(sizeof(cls) / sizeof(cls[0]))) <= 0)
    return false;
  return wcscmp(cls, L"#32768") == 0;
}

void ApplyToMenu(HWND h)
{
  bool roundIt = false;
  EnterCriticalSection(&g_lock);
  roundIt = g_set.roundMenus;
  LeaveCriticalSection(&g_lock);

  // ---- the corners -----------------------------------------------------------------------------------
  DwmSetFn set = DwmSet();
  if (set && roundIt)
  {
    const int round = DWMWCP_ROUND;
    if (SUCCEEDED(set(h, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round))))
      InterlockedIncrement(&g_changed);
    else
    {
      static bool said = false;
      if (!said)
      {
        said = true;
        FileLog("menu: the corner request was refused -- menus will stay square");
      }
    }
  }

}

// ---- the decision, and the one call it leads to --------------------------------------------------------

void ApplyTo(HWND h)
{
  // ⚠️ THE HOST'S SWITCH, CHECKED BEFORE ANY WORK IS DONE. The host turns a feature off by simply not calling
  // it, which is complete for a feature that only acts when called -- and useless for this one, which owns a
  // thread and an event hook of its own. Without this line the plugin list's switch is a picture of a switch:
  // the panel says "off" while every window that appears keeps being restyled. (KeepAwake's header records
  // this same lesson; it was learned on AutoIME's monitor thread first.)
  if (InterlockedCompareExchange(&g_live, 0, 0) == 0)
    return;

  // The menu path first: ShouldTouch would reject a menu outright (no WS_CAPTION), and the two kinds of
  // window want different things anyway.
  if (IsMenuWindow(h))
  {
    ApplyToMenu(h);
    return;
  }

  char exe[kMaxName] = {0};
  DWORD pid = 0;
  if (!ShouldTouch(h, exe, (int)sizeof(exe), &pid))
    return;

  InterlockedIncrement(&g_seen);

  DwmGetFn get = DwmGet();
  if (!get)
    return;

  // -1 rather than 0 as the starting value ON PURPOSE. "The read failed" and "the window is light" mean
  // opposite things here -- one is a reason to leave the window alone, the other is a reason to change it --
  // and a zero-initialised variable cannot tell them apart.
  BOOL cur = (BOOL)-1;
  HRESULT hr = get(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &cur, sizeof(cur));
  if (FAILED(hr))
  {
    cur = (BOOL)-1;
    hr = get(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &cur, sizeof(cur));
  }
  if (FAILED(hr))
    return; // could not ask -> do not touch

  const BOOL want = (InterlockedCompareExchange(&g_systemDark, 0, 0) != 0) ? TRUE : FALSE;

  // ⚠️ THE COMPARISON IS THE USER'S REQUIREMENT, IMPLEMENTED. "如果原生有，就不接管明暗" -- a program that
  // already follows the system reads back the system's own value, so it falls out here and is never written
  // to, without this feature needing to know one program name.
  if (cur == want)
    return;

  DwmSetFn set = DwmSet();
  if (!set)
    return;

  HRESULT sh = set(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &want, sizeof(want));
  if (FAILED(sh))
    sh = set(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &want, sizeof(want));
  if (FAILED(sh))
  {
    // ⚠️ A REFUSED WRITE IS WORTH A LOG LINE, because the refusal has a cause the user can act on that the
    // silence does not: the usual one is a program running at a higher integrity level than Apex, which is
    // fixed by running Apex elevated, not by trying again.
    FileLog("refused: %s (was %d, wanted %d) hr=0x%08lx -- a program running elevated can only be restyled "
            "by an elevated Apex",
            exe, (int)cur, (int)want, (unsigned long)sh);
    return;
  }

  InterlockedIncrement(&g_changed);
  RememberTouched(h, pid);
  FileLog("%s: %s -> %s", exe, cur ? "light" : "dark", want ? "dark" : "light");
}

// ---- the two ways a window is noticed ------------------------------------------------------------------

// TWO EVENTS, AND BOTH ARE NEEDED. EVENT_OBJECT_SHOW is one window becoming visible anywhere on the machine;
// EVENT_OBJECT_DESTROY is one going away, and it is here so that the record of what was changed does not keep
// a handle that is about to be handed to somebody else. OUTOFCONTEXT, so this is called on THIS feature's own
// thread -- the one running the message pump below -- and never inside the process that created the window.
// Nothing here can slow another program down.
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
  // The object has to be the window itself, not one of its children or one of its controls.
  if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
    return;
  if (!hwnd)
    return;

  if (event == EVENT_OBJECT_DESTROY)
  {
    ForgetTouched(hwnd);
    return;
  }
  if (event == EVENT_OBJECT_SHOW)
  {
    // ⚠️ ONE LINE PER RUN, AND IT EXISTS BECAUSE OF A REAL REPORT. The user said the menu corners worked and
    // the transparency did not, and Apex's log had NO menu line at all -- neither a success nor a failure.
    // That is equally consistent with two very different worlds: "the hook never saw a menu from another
    // process" and "it saw one and quietly declined". This line tells them apart. (A hook installed in the
    // process that OWNS the menu does see these events -- measured, _diag/dark_probe.cpp -- but that is a
    // different question from one watching somebody else's.)
    {
      wchar_t cls[64] = {0};
      if (GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0]))) > 0 &&
          wcscmp(cls, L"#32768") == 0)
      {
        static bool said = false;
        if (!said)
        {
          said = true;
          FileLog("menu: the hook DID see a menu appear (hwnd=%p, another process's) -- said once per run",
                  (void *)hwnd);
        }
      }
    }
    ApplyTo(hwnd);
  }
}

BOOL CALLBACK EnumProc(HWND h, LPARAM)
{
  ApplyTo(h);
  return TRUE; // every top-level window, including the ones already open when this feature started
}

// ---- the worker ----------------------------------------------------------------------------------------

DWORD WINAPI Worker(LPVOID)
{
  // ⚠️ THE HOOK IS INSTALLED FIRST, AND THE SWEEP SECOND. The other order has a gap in it: a window that
  // appeared between the sweep and the hook would be missed by both and stay wrong until it was reopened.
  //
  // The range covers DESTROY as well as SHOW (0x8001..0x8002): DESTROY is what keeps the record of changed
  // windows from outliving the windows themselves.
  g_hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_SHOW, nullptr, WinEventProc, 0, 0,
                           WINEVENT_OUTOFCONTEXT);
  if (!g_hook)
    Log("unifiedui: SetWinEventHook failed (%lu) -- only the windows already open will be handled",
        (unsigned long)GetLastError());

  // The system's own answer, before anything is judged by it.
  InterlockedExchange(&g_systemDark, apex::common::SystemIsLightTheme() ? 0 : 1);
  FileLog("start: the system is %s", g_systemDark ? "dark" : "light");

  // ⚠️ THE HOST'S SWITCH IS READ BEFORE THE FIRST WINDOW IS JUDGED, not after: ApplyTo refuses to act while
  // this is 0, so a feature that starts switched off does nothing at all until it is switched on.
  const bool startEnabled = EnabledByHost();
  InterlockedExchange(&g_live, startEnabled ? 1 : 0);
  FileLog("the plugin switch is %s", startEnabled ? "on" : "off");

  if (startEnabled)
  {
    EnumWindows(EnumProc, 0);
    FileLog("start-up sweep: %ld window(s) considered, %ld changed", (long)g_seen, (long)g_changed);
  }

  DWORD lastPoll = GetTickCount();
  for (;;)
  {
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
      break;

    // ⚠️ WAIT FOR A MESSAGE **OR** THE SLICE, THEN PUMP. A hook callback registered with WINEVENT_OUTOFCONTEXT
    // is delivered by the message queue, so a thread that only slept would never receive one -- the hook would
    // be installed and silent. (Same lesson as _diag/ui_probe.cpp, where the missing pump cost a run.)
    HANDLE waits[1] = {g_wake};
    MsgWaitForMultipleObjects(1, waits, FALSE, kPumpMs, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    // ---- the host's switch, which nothing else would notice ------------------------------------------
    //
    // ⚠️⚠️ THIS IS THE BLOCK THAT MAKES THE PLUGIN LIST'S SWITCH REAL, AND ITS ABSENCE WAS A REAL BUG -- found
    // by the user asking for "开关切换...有实时效果", which it did not have. A feature that owns a thread keeps
    // running whatever the list says, so "off" has to MEAN something here: stop judging new windows, AND put
    // back the ones already changed, so that switching it off looks like the feature was never on.
    const bool enabled = EnabledByHost();
    const LONG wasLive = InterlockedExchange(&g_live, enabled ? 1 : 0);
    if (!enabled && wasLive)
    {
      const int back = RestoreAll();
      FileLog("switched OFF -- %d window(s) put back; nothing new will be touched", back);
    }
    else if (enabled && !wasLive)
    {
      FileLog("switched ON -- re-reading every open window");
      EnumWindows(EnumProc, 0);
    }

    // ---- the panel asked for a re-read (the exclude list changed) ------------------------------------
    //
    // Adding a name has to do something to the windows ALREADY changed, or "the list takes effect at once"
    // would only be true of windows that had not appeared yet. Removing a name needs nothing special: the
    // sweep below reaches those windows like any other.
    if (InterlockedExchange(&g_reevaluate, 0) != 0)
    {
      const int back = RestoreExcluded();
      if (back)
        FileLog("%d window(s) put back -- their program is on the exclude list now", back);
      if (enabled)
        EnumWindows(EnumProc, 0);
    }

    // The system can be switched light/dark while windows are open, and every one of them has to follow --
    // "明暗随系统设置" is about the system's CURRENT setting, not about the one that was in force when a window
    // happened to open.
    const DWORD now = GetTickCount();
    if (now - lastPoll >= (DWORD)kThemePollMs)
    {
      lastPoll = now;
      const LONG dark = apex::common::SystemIsLightTheme() ? 0 : 1;
      if (dark != InterlockedCompareExchange(&g_systemDark, 0, 0))
      {
        InterlockedExchange(&g_systemDark, dark);
        FileLog("the system switched to %s -- re-reading every open window", dark ? "dark" : "light");
        if (enabled)
          EnumWindows(EnumProc, 0);
      }
    }
  }

  if (g_hook)
  {
    UnhookWinEvent(g_hook);
    g_hook = nullptr;
  }
  FileLog("stopped: %ld window(s) considered, %ld changed", (long)g_seen, (long)g_changed);
  return 0;
}

// ---- the panel's data ----------------------------------------------------------------------------------

void AppendJsonString(char *out, int outSize, int &off, const char *s)
{
  if (off >= outSize - 1)
    return;
  if (!s)
    s = "";
  out[off++] = '"';
  for (const char *p = s; *p && off < outSize - 2; ++p)
  {
    if (*p == '"' || *p == '\\')
    {
      out[off++] = '\\';
      out[off++] = *p;
    }
    else
      out[off++] = *p;
  }
  out[off++] = '"';
  out[off] = 0;
}

int SettingsJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  out[0] = 0;
  int off = 0;
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  off += _snprintf(out + off, outSize - off,
                   "{\"params\":["
                   "{\"id\":\"exclude\",\"type\":\"list\","
                   "\"labelZh\":\"\xe6\x8e\x92\xe9\x99\xa4\",\"labelEn\":\"Exclude\","
                   "\"placeholderZh\":\"game.exe\",\"placeholderEn\":\"game.exe\","
                   "\"values\":[");
  for (int i = 0; i < s.n && off < outSize - 8; ++i)
  {
    if (i)
      out[off++] = ',';
    AppendJsonString(out, outSize, off, s.ex[i]);
  }
  off += _snprintf(out + off, outSize - off, "]},");
  off += _snprintf(out + off, outSize - off,
                   "{\"id\":\"round_menus\",\"type\":\"bool\","
                   "\"labelZh\":\"\xe8\x8f\x9c\xe5\x8d\x95\xe5\x9c\x86\xe8\xa7\x92\","
                   "\"labelEn\":\"Round menu corners\",\"value\":%d}],", s.roundMenus ? 1 : 0);

  // THE ONE-LINE DESCRIPTION OF THE FEATURE, in both languages (apex/abi.h). The panel cannot write it: it has
  // no idea what any feature does.
  off += _snprintf(out + off, outSize - off, "\"summaryZh\":\"%s\",",
                   "\xe8\xae\xa9\xe6\x97\xa7\xe7\xaa\x97\xe5\x8f\xa3\xe7\x9a\x84\xe6\xa0\x87\xe9\xa2\x98\xe6\xa0\x8f"
                   "\xe8\xb7\x9f\xe9\x9a\x8f\xe7\xb3\xbb\xe7\xbb\x9f\xe7\x9a\x84\xe6\x98\x8e\xe6\x9a\x97");
  off += _snprintf(out + off, outSize - off,
                   "\"summaryEn\":\"Lets an old window's title bar follow the system's light/dark\"}");

  if (off >= outSize)
    off = outSize - 1;
  out[off] = 0;
  return off;
}

// ---- the ABI surface -----------------------------------------------------------------------------------

// ⚠️ `init` RETURNS **0 FOR "I AM READY"** AND NON-ZERO TO REFUSE. The direction is spelled out because
// abi.h once said the opposite and that single inverted line cost an afternoon (see the note in
// features/KeepAwake/feature_keepawake.cpp, which lived through it).
int UiInit(const ApexHost *host)
{
  g_host = host;
  if (host && host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));
  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  OpenLog();
  LoadSettings();

  // ⚠️⚠️ THE SWITCH THAT EXISTS ONLY FOR THE GATE, AND IT IS THE GATE'S SAFETY RATHER THAN A FEATURE OPTION.
  //
  // Calling init() starts a hook that restyles every window on the machine. A gate that loads this DLL to
  // check its settings file would therefore be RESTYLING THE USER'S DESKTOP as a side effect of a test -- and
  // "测试不许打扰用户" is one of this project's four hard gate rules. With APEX_UNIFIEDUI_DRY set, the feature
  // loads, reads and writes its settings, and WATCHES NOTHING.
  //
  // ⚠️ IT IS AN ENVIRONMENT VARIABLE AND NOT A FIELD OF THE ABI, deliberately: a feature-visible setting for
  // "do not do your job" would be a setting a user could find and turn on, and then the plugin would look
  // broken in the one way that is hardest to explain. (The project already carries two switches of exactly
  // this kind for the tray and the window -- see docs/rules/gates.md.)
  {
    char v[8] = {0};
    if (GetEnvironmentVariableA("APEX_UNIFIEDUI_DRY", v, (DWORD)sizeof(v)) > 0)
    {
      Log("unifiedui: APEX_UNIFIEDUI_DRY is set -- settings only, nothing is watched");
      FileLog("dry run: settings loaded, no hook, no sweep");
      return 0; // READY -- the settings half is what a dry run is for
    }
  }

  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (!g_thread)
  {
    // NO THREAD MEANS NOTHING IS EVER WATCHED, so this is a refusal rather than a quiet failure: a feature
    // that looked switched on and did nothing would be worse than one the panel says is not there. And a
    // refusal must not leave anything running -- nothing has been started at this point but the critical
    // section and the log, and Shutdown is called by the host on the refusal path.
    Log("unifiedui: could not start the watcher thread (error %lu) -- refusing to load",
        (unsigned long)GetLastError());
    return 1; // NON-ZERO MEANS REFUSE
  }

  Log("unifiedui: watching for windows (%d excluded pattern(s), menu corners %s)", g_set.n,
      g_set.roundMenus ? "on" : "off");
  return 0; // READY
}

void UiShutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_wake)
    SetEvent(g_wake);
  if (g_thread)
  {
    // The worker waits at most one slice, so this is milliseconds rather than a second.
    const bool stopped = WaitForSingleObject(g_thread, 3000) == WAIT_OBJECT_0;
    if (!stopped)
      Log("unifiedui: the watcher did not stop in time; the hook goes with the process");
    CloseHandle(g_thread);
    g_thread = nullptr;

    // ⚠️ PUT THE WINDOWS BACK, AND IT IS NOT OPTIONAL -- the same rule KeepAwake follows for its power
    // request: a setting changed by a program the user then closed must not outlive the program. Without this,
    // closing Apex would leave every old window with a dark title bar and nothing running that knows why.
    //
    // ⚠️ ONLY AFTER THE WORKER HAS STOPPED, because `g_touched` belongs to that thread (see the note on it).
    // If it did not stop, the record is left alone rather than read while it may be being written.
    if (stopped)
    {
      const int back = RestoreAll();
      if (back)
        FileLog("shutting down -- %d window(s) put back", back);
    }
  }
  if (g_wake)
  {
    CloseHandle(g_wake);
    g_wake = nullptr;
  }
  if (g_log)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  if (g_lockReady)
  {
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
  }
}

int UiReload()
{
  LoadSettings();
  // ⚠️ AND THE WINDOWS ALREADY CHANGED ARE LOOKED AT AGAIN, which is what makes the list take effect "at once"
  // rather than "for windows that have not appeared yet". The first version of this feature claimed the
  // opposite -- that a window this feature had put back could not be told from one that had always agreed with
  // the system -- and that is true only WITHOUT a record of what was changed. With one (see g_touched) both the
  // question and the answer are exact, and the user asked for the live behaviour: "排除名单有实时效果".
  //
  // ⚠️ THE WORK IS NOT DONE HERE. This is the panel's thread, and a sweep over every window on the machine
  // does not belong on it; the worker is woken instead.
  AskForReevaluation();
  return 1;
}

// THIS FEATURE HAS NO NUMBERS AND NO SWITCHES OF ITS OWN. The plugin list's switch is the host's, and the
// feature honours it in its worker (nothing is watched while it is off) -- see the note in the worker. So
// there is nothing for setControl to accept, and saying so is better than accepting a value nothing reads.
int UiSetControl(const char *path, const char *value)
{
  if (!path || !value)
    return 0;
  if (strcmp(path, "round_menus") == 0)
  {
    EnterCriticalSection(&g_lock);
    g_set.roundMenus = (atoi(value) != 0);
    LeaveCriticalSection(&g_lock);
    return 1;
  }
  return 0;
}

int UiListOp(const char *id, const char *op, const char *value, int index)
{
  return ListOp(id, op, value, index);
}

int UiSave() { return SaveSettings() ? 1 : 0; }

unsigned UiFlags()
{
  // ⚠️ ALWAYS ENABLED, AND THE HOST'S OWN SWITCH IS HONOURED IN THE WORKER. This bit says "this feature is
  // loaded and usable", not "its switch in the plugin list is on" -- that one is the host's, and a feature
  // whose worker is still watching windows must not report itself as absent.
  return APEX_FEATURE_ENABLED;
}

} // namespace

// ---------------------------------------------------------------------------
// THE EXPORTED STRUCTURE. `structSize` is checked by the host BEFORE anything below is read.
//
// NO onWheel AND NO tick: this feature has nothing to do with the wheel and nothing to do with the frame
// clock. It owns one hook of its own (a window-event hook, not an input hook) on one thread of its own, so
// the input path this program is built around is untouched by it.
// ---------------------------------------------------------------------------
static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    kId,
    "\xe7\xbb\x9f\xe4\xb8\x80\xe5\xa4\x96\xe8\xa7\x82", // 统一外观
    "Unified Look",
    kVersion,
    UiInit,
    UiShutdown,
    UiReload,
    SettingsJson,
    UiSetControl,
    UiListOp,
    nullptr, // quickItems -- nothing of this feature belongs in the flyout
    UiSave,
    nullptr, // onWheel
    nullptr, // tick
    UiFlags,
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
