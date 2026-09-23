// ---------------------------------------------------------------------------
// KeepAwake -- STOP THE SYSTEM SLEEPING AND/OR THE SCREEN TURNING OFF.
//
// WHAT IT IS FOR, in the user's own words: "可以阻止系统睡眠 / 可以阻止系统熄屏 / 可以加入指定进程名单生效".
//
// ⚠️ AND THE SHAPE OF IT IS THE USER'S SECOND DESIGN. The first version had two independent master switches
// plus a separately-switched program list ("进程名单给个开关功能，关的时候，以上两个功能直接生效，开的时候，检查到
// 运行的进程有在名单里生效"), and it did work. It was replaced because the two mechanisms together made for a
// page that had to be explained rather than read:
//
//   "统一做成列表式，每个进程有「保持唤醒」和「阻止熄屏」两项，和一个移除按钮。原来的「阻止系统睡眠」和「阻止屏幕关闭」
//    功能直接做进列表中，加入一个「系统全局」，这项不可删除。"
//
// So there is ONE list. It always contains a row called 系统全局 -- what the two old master switches did, in the
// place where everything else lives, and undeletable. Every other row names a program and applies only while
// that program is running.
//
// ⚠️ A ROW SAYS ONE OF TWO THINGS, AND THAT IS WINDOWS' OWN MODEL RATHER THAN A SIMPLIFICATION: "每条设置，要么
// 只有保持唤醒，要么是保持唤醒+防止熄屏，因为按系统的逻辑，防止熄屏肯定不能睡眠". A screen that stays on is a
// machine that is not sleeping, so the two switches are COUPLED (see ApplyRowSwitch) instead of independent --
// which is the one place this design deliberately differs from the first version's "睡眠和熄屏两个独立分开控制".
//
// ⚠️ AND THE ROWS COMBINE BY TAKING THE STRONGEST: "列表功能叠加：比如一个进程是只有保持唤醒，另一个两个都有，
// 就以另一个为主". Which is also why the order of the list is not meaning, and why the panel's "rows" layout has
// no drag (see abi.h). 系统全局's row always counts; a program's row counts while that program runs.
//
// HOW WINDOWS IS ASKED: SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED).
//   * ES_SYSTEM_REQUIRED resets the SYSTEM idle timer, ES_DISPLAY_REQUIRED the DISPLAY one -- two different
//     requests, which is exactly why the two switches exist rather than one "keep awake".
//   * ES_CONTINUOUS means "until I say otherwise", so the state is not a per-tick thing.
//   * ⚠️ THE REQUEST BELONGS TO THE THREAD THAT MADE IT. That is the whole reason this feature owns a thread:
//     call it from the host's message thread and the request dies with whatever that thread is doing, call it
//     from a thread that exits and it is cleared on the way out. The worker below makes the call, keeps itself
//     alive, re-asserts it periodically (a sleep/resume can drop it), and clears it on the way out.
//
// WHY A THREAD AT ALL, rather than the host's `tick`: `tick` only runs while the feature reports
// APEX_FEATURE_ACTIVE, and this feature has no motion -- it must be right while nothing at all is happening.
// (AutoIME owns a monitor thread for the same kind of reason.) The host's input path is untouched: nothing
// here is called from it.
//
// THE LIST IS CHECKED ONCE A SECOND (a Toolhelp process walk, measured at a few milliseconds), and only when a
// PROGRAM row is switched on -- a feature that walks 250 processes while every program row is off would be
// paying for nothing.
// ---------------------------------------------------------------------------

#include "../../apex/abi.h"
#include "../../common/match.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

namespace {

const char *kId = "KeepAwake";
const char *kVersion = "2.0.0";

const int kMaxList = 32;      // program rows, this feature's own list (nothing else reads it)
const int kMaxName = 64;      // one name, lower-case, without a path -- enough for a long file name
const int kCycleMs = 1000;    // how often the decision is re-made
const int kReassertMs = 30000; // how often an unchanged request is re-sent (a resume can drop it)
const int kLogCap = 256 * 1024;

// THE ONE ROW THAT IS ALWAYS THERE, in both languages: the page draws the reader's one (see `titleZh`/`titleEn`
// in abi.h). It is written here rather than by the page because it also has to be READABLE IN THE SETTINGS FILE's
// own comment, and because the feature is what decided the row exists.
const char *kGlobalTitleZh = "系统全局";
const char *kGlobalTitleEn = "System-wide";

// THE PANE THE WHOLE LIST SHARES IN THE QUICK PANEL (see `groupZh`/`groupEn` in abi.h). One name, both languages,
// because the flyout draws the reader's one and this feature does not know which that is. ⚠️ It is the FEATURE's
// own words for what the rows have in common -- and the rows carry their own labels (系统全局, chrome.exe), so the
// heading and a row are two different sentences rather than the same one twice.
#define kQuickGroupZh "\xe4\xbf\x9d\xe6\x8c\x81\xe5\x94\xa4\xe9\x86\x92" // 保持唤醒
#define kQuickGroupEn "Keep awake"

// ---------------------------------------------------------------------------
// THE SETTINGS. Written by the panel's thread (setControl / listOp / reloadSettings), read by the worker, so
// every access is under `g_lock` -- and the worker COPIES them out before doing anything slow (the process
// walk, the file) so the lock is never held across a syscall.
//
// ⚠️ ROW 0 OF THE PAGE IS THE GLOBAL ROW, BUT IT IS NOT `entries[0]`: the global row has no program name and
// no matching, so it is two booleans of its own. The page's item index is mapped to the two halves in
// ApplyRowSwitch, and that mapping is the only place the two indexings have to agree.
// ---------------------------------------------------------------------------
struct Entry
{
  char name[kMaxName];
  bool awake;
  bool display;
};

struct Settings
{
  bool globalAwake = false;   // the 系统全局 row: applies always
  bool globalDisplay = false;
  int n = 0;                  // program rows, in the order the user added them
  Entry e[kMaxList];

  // ---- MAY THE QUICK PANEL REACH THIS FEATURE'S LIST ---------------------------------------------
  //
  // ⚠️ THE USER'S RULE, AND IT IS ABOUT PERMISSION RATHER THAN CONVENIENCE: "插件自己的控件要明确有开关映射到
  // 快速面板，才给". A control reaches the flyout because the USER said so, on this feature's own page -- and since
  // 2026-09-23 that is ONE switch for the whole list rather than one per control. The user's words: "保持唤醒插件的
  // 快速面板给一个开关，放在「添加」按钮右侧…快速面板按列表显示系统和各应用的两个功能开关". Default OFF: "nothing
  // appears until it is asked for" is the entire point of having the switch.
  //
  // ⚠️ IT IS NOT THE ROW SWITCHES. `global_awake` is whether the machine is kept awake; `quick_panel` is whether
  // that list is ALSO drawn in the flyout. Two questions, two answers, and conflating them would mean a user who
  // wanted the panel tidy turned their machine's sleep behaviour off.
  //
  // ⚠️ AND IT REPLACED TWO SWITCHES (`quick_awake` / `quick_display`, one per master switch). They are read by
  // nothing now and an old file simply carries two keys that no longer mean anything -- which is how every removed
  // setting here behaves (see the note on unknown keys in LoadSettings).
  bool quickPanel = false;
};

Settings g_set;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

const ApexHost *g_host = nullptr;
char g_dir[512] = {0};

HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;     // signalled to end the cycle early (shutdown / a settings change)
volatile LONG g_stop = 0;

// ⚠️ NOTES ON WHAT IS *NOT* HERE ANY MORE: this feature used to publish a one-line read-out for the panel
// (`liveText`, "保持唤醒 · 屏幕常亮（dsh desktop.exe）") and owned a second lock for it. The read-out is gone from
// the whole program (apex/abi.h, ABI 9 -> 10: the user tried it, then said "其实这个提示可以完全去掉。并不需要"),
// so the copy, the lock and the publishing call went with it -- and the worker is now purely "decide and ask
// Windows", with nothing to report to anybody but the tray bits below.

// WHAT THE HOST NEEDS TO KNOW ABOUT, published by the worker for `flags()` to read. Plain words rather than
// a lock: they are single bits of state that the host asks about once a second from its own thread.
//
// ⚠️ IT IS A LEVEL AND NOT A BOOLEAN, because the mark has three states and the user asked for all three
// (see abi.h): 0 = nothing held, 1 = an ordinary hold (the tray bar goes green), 2 = the stronger one, which
// also keeps the screen on (the bar goes red).
volatile LONG g_level = 0;

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

// The feature's own log, beside itself. Why it exists at all: this feature's effect is INVISIBLE -- the user
// cannot see a power request, and the only other evidence is whether their screen stayed on overnight. If it
// does not, the first question is "did we even ask?", and there has to be something that answers it.
//
// ⚠️ REWRITTEN EACH RUN ("w") AND WRITTEN ONLY WHEN THE DECISION CHANGES. Per-cycle logging was a real bug
// once (AutoIME wrote a line per poll); a log that grows while nothing happens hides the one transition that
// matters.
void OpenLog()
{
  if (!g_dir[0])
    return;
  char path[560] = {0};
  if (_snprintf(path, sizeof(path), "%skeepawake.log", g_dir) <= 0)
    return;
  g_log = fopen(path, "w");
}

// A timestamp for the log, because "when did it stop asking" is the question a reader has.
void Stamp(char *out, int outSize)
{
  SYSTEMTIME st;
  GetLocalTime(&st);
  _snprintf(out, outSize, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// THE EXACT CALL, RECORDED. `powercfg /requests` would be the other way to see this and it needs elevation
// (measured), so the flags passed to SetThreadExecutionState are written down here -- and the gate that reads
// them asserts the bit pattern. That is the closest thing to "Windows was actually asked" a test without
// administrator rights can have.
void LogTransition(int level, DWORD flags, DWORD prev, const char *why)
{
  if (!g_log)
    return;
  char ts[32] = {0};
  Stamp(ts, (int)sizeof(ts));
  fprintf(g_log, "%s  level=%d sleep=%d display=%d (flags=0x%lx, previous=0x%lx)  %s\n", ts, level,
          level >= 1 ? 1 : 0, level >= 2 ? 1 : 0, (unsigned long)flags, (unsigned long)prev, why);
  // The cap is a backstop, not a schedule: transitions are rare and the file is rewritten each run.
  if (ftell(g_log) > kLogCap)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  else
    fflush(g_log);
}

// ---- the two switches of one row, and the rule that ties them ------------------------------------
//
// ⚠️ THE COUPLING IS THE FEATURE'S, AND IT IS ENFORCED HERE RATHER THAN DRAWN IN THE PAGE. Windows cannot keep
// a screen on while letting the machine sleep, so "keep the screen on" implies "keep awake" -- and a row that
// said otherwise would be a row whose two switches contradict each other. Switching the screen ON therefore
// switches the awake switch ON with it; switching awake OFF takes the screen switch down with it. The page
// re-reads its controls after every edit (see sendControl in panel.html), so the switch the user did not touch
// corrects itself in front of them -- which is how a rule the user cannot see becomes one they can.
bool ApplyRowSwitch(const char *path, bool v, bool *changed)
{
  // `path` is "rules[<i>].<field>", where index 0 is the GLOBAL row and 1.. are the program rows -- the page's
  // own numbering, which is the numbering the feature publishes (see SettingsJson).
  if (strncmp(path, "rules[", 6) != 0)
    return false;
  const int idx = atoi(path + 6);
  const char *dot = strchr(path, '.');
  if (!dot)
    return false;
  const char *fld = dot + 1;
  const bool isAwake = (strcmp(fld, "awake") == 0);
  const bool isDisplay = (strcmp(fld, "display") == 0);
  if (!isAwake && !isDisplay)
    return false;

  bool *awake = nullptr;
  bool *display = nullptr;
  if (idx == 0)
  {
    awake = &g_set.globalAwake;
    display = &g_set.globalDisplay;
  }
  else if (idx >= 1 && idx <= g_set.n)
  {
    awake = &g_set.e[idx - 1].awake;
    display = &g_set.e[idx - 1].display;
  }
  else
  {
    return false; // a row that is not there: refuse rather than invent one
  }

  const bool beforeAwake = *awake, beforeDisplay = *display;
  if (isAwake)
  {
    *awake = v;
    if (!v)
      *display = false; // no screen-on without awake (see the note above)
  }
  else
  {
    *display = v;
    if (v)
      *awake = true;
  }
  *changed = (*awake != beforeAwake || *display != beforeDisplay);
  return true;
}

// ---- the settings file ---------------------------------------------------------------------------
//
// ⚠️ THIS LIST IS THIS FEATURE'S OWN, AND IT SHARES NOTHING WITH ANY OTHER FEATURE'S. That is a rule of the
// product, not a detail: every feature keeps its own file in its own folder (`Plugins\<id>\<id>.ini`), its own
// control ids, and its own names. The wheel feature's "exclude" list and AutoIME's per-rule process fields are
// different lists with different meanings, and mixing them would mean a name typed for one purpose silently
// changing the behaviour of another -- with no screen anywhere that shows the two together.
//
// The one thing shared with the exclude list is the MATCHING RULE (common/match.h: fold, strip the path,
// anchored `*`/`?`) -- it is a pure function over two strings and holds no names of its own.
//
// `key=value`, one per line, `#` starts a comment, unknown keys are IGNORED (leaving the default) -- the same
// contract as every other settings file here, so a hand-edited file cannot produce nonsense and an older file
// stays readable.
//
// THE FILE STARTS WITH THE GLOBAL ROW OFF AND NO PROGRAM ROWS. No default program names: a name that arrived
// from somewhere else would be a decision this feature made on the user's behalf about a program it knows
// nothing about.

bool ConfigPath(char *out, int outSize)
{
  if (!g_dir[0])
    return false;
  const int n = _snprintf(out, outSize, "%sKeepAwake.ini", g_dir);
  return n > 0 && n < outSize;
}

bool ParseBool(const char *v, bool *out)
{
  if (!v || !out)
    return false;
  if (strcmp(v, "1") == 0 || _stricmp(v, "true") == 0 || _stricmp(v, "on") == 0 || _stricmp(v, "yes") == 0)
  {
    *out = true;
    return true;
  }
  if (strcmp(v, "0") == 0 || _stricmp(v, "false") == 0 || _stricmp(v, "off") == 0 || _stricmp(v, "no") == 0)
  {
    *out = false;
    return true;
  }
  return false;
}

// Adds one normalised name, refusing an empty one and a duplicate. `false` when nothing was added.
//
// ⚠️ TRIMMED FIRST, because "   " is not a program name. The panel's add box sends the text exactly as typed,
// and an entry of spaces would sit in the list looking like an empty row, matching nothing, and refusing to be
// added a second time -- a row the user cannot get rid of and cannot explain.
bool AddEntry(Settings &s, const char *raw, bool awake, bool display)
{
  if (!raw)
    return false;
  char trimmed[kMaxName * 2] = {0};
  const char *b = raw;
  while (*b == ' ' || *b == '\t')
    ++b;
  int n = 0;
  while (b[n] && n < (int)sizeof(trimmed) - 1)
  {
    trimmed[n] = b[n];
    ++n;
  }
  while (n > 0 && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t' || trimmed[n - 1] == '\r' ||
                   trimmed[n - 1] == '\n'))
    --n;
  trimmed[n] = 0;

  char name[kMaxName] = {0};
  apex::match::NormaliseName(trimmed, name, (int)sizeof(name));
  if (!name[0])
    return false;
  for (int i = 0; i < s.n; ++i)
    if (strcmp(s.e[i].name, name) == 0)
      return false; // already there: a list with the same name twice would be a list that lies about its length
  if (s.n >= kMaxList)
    return false;
  strcpy(s.e[s.n].name, name);
  // A screen that stays on is a machine that is not sleeping (see the header): the file may say otherwise --
  // it is hand-editable -- and the rule is applied on the way in, so no in-memory row can contradict itself.
  s.e[s.n].awake = awake || display;
  s.e[s.n].display = display;
  ++s.n;
  return true;
}

// THE OLD FORMAT, READ SO THAT AN UPGRADE COSTS THE USER NOTHING.
//
// The first version wrote `sleep=`, `display=`, `listOnly=` and `process=<name>`. Dropping those keys silently
// would lose whatever the user had set up -- and the two formats do not map onto each other in an obvious way,
// which is why the migration is written out rather than folded into the parse:
//
//   * `listOnly=1` was "the two switches only apply while a listed program runs" -- which is a PROGRAM row in
//     the new model, so each old name becomes a row carrying the old switches, and the global row stays off;
//   * `listOnly=0` meant the names did NOTHING (the switches applied always). So the global row takes the two
//     switches, and the names are kept WITH BOTH SWITCHES OFF: the user's typing survives, and nothing starts
//     holding a power request that was not holding one a moment before the upgrade. That second half is the
//     point -- a migration that switches behaviour on is worse than one that loses a setting the user can see
//     is missing.
void LoadSettings()
{
  Settings fresh;
  // The old format's four keys, collected and applied after the parse.
  bool legacySleep = false, legacyDisplay = false, legacyListOnly = false, sawLegacy = false;
  char legacyNames[16][kMaxName] = {{0}};
  int legacyN = 0;

  char path[560] = {0};
  if (ConfigPath(path, (int)sizeof(path)))
  {
    FILE *f = fopen(path, "rb");
    if (f)
    {
      char line[512] = {0};
      while (fgets(line, sizeof(line) - 1, f))
      {
        char *hash = strchr(line, '#');
        if (hash)
          *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq)
          continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        while (*key == ' ' || *key == '\t')
          ++key;
        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t'))
          *--kend = 0;
        while (*val == ' ' || *val == '\t')
          ++val;
        char *vend = val + strlen(val);
        while (vend > val && (vend[-1] == '\r' || vend[-1] == '\n' || vend[-1] == ' ' || vend[-1] == '\t'))
          *--vend = 0;
        if (!key[0])
          continue;
        if (_stricmp(key, "global_awake") == 0)
          ParseBool(val, &fresh.globalAwake);
        else if (_stricmp(key, "global_display") == 0)
          ParseBool(val, &fresh.globalDisplay);
        // May the quick panel reach this feature's list at all (see the field above). One key, one answer; the
        // two per-control keys it replaced (`quick_awake` / `quick_display`) are ignored like any other unknown
        // key, which is what keeps an older settings file loadable.
        else if (_stricmp(key, "quick_panel") == 0)
          ParseBool(val, &fresh.quickPanel);
        else if (_stricmp(key, "item") == 0)
        {
          // `name`, `name|awake` or `name|awake|display`. A bare name is the useful default ("keep the machine
          // awake for this program"), and the file says so in its own header.
          char name[kMaxName * 2] = {0};
          int k = 0;
          while (val[k] && val[k] != '|' && k < (int)sizeof(name) - 1)
          {
            name[k] = val[k];
            ++k;
          }
          name[k] = 0;
          bool awake = true, display = false;
          const char *bar1 = strchr(val, '|');
          if (bar1)
          {
            ParseBool(bar1 + 1, &awake); // an unparseable one leaves the default rather than failing the file
            const char *bar2 = strchr(bar1 + 1, '|');
            if (bar2)
              ParseBool(bar2 + 1, &display);
          }
          AddEntry(fresh, name, awake, display);
        }
        // ---- the first version's own keys (see the note above this function) ----
        else if (_stricmp(key, "sleep") == 0)
        {
          ParseBool(val, &legacySleep);
          sawLegacy = true;
        }
        else if (_stricmp(key, "display") == 0)
        {
          ParseBool(val, &legacyDisplay);
          sawLegacy = true;
        }
        else if (_stricmp(key, "listOnly") == 0)
        {
          ParseBool(val, &legacyListOnly);
          sawLegacy = true;
        }
        else if (_stricmp(key, "process") == 0)
        {
          sawLegacy = true;
          if (legacyN < 16)
          {
            apex::match::NormaliseName(val, legacyNames[legacyN], kMaxName);
            if (legacyNames[legacyN][0])
              ++legacyN;
          }
        }
        // anything else: ignored on purpose (see the note above)
      }
      fclose(f);
    }
  }

  if (sawLegacy)
  {
    if (legacyListOnly)
    {
      for (int i = 0; i < legacyN; ++i)
        AddEntry(fresh, legacyNames[i], legacySleep, legacyDisplay);
    }
    else
    {
      fresh.globalAwake = legacySleep;
      fresh.globalDisplay = legacyDisplay;
      for (int i = 0; i < legacyN; ++i)
        AddEntry(fresh, legacyNames[i], false, false); // kept, switched off -- see the note above
    }
    Log("keepawake: read the older settings format (sleep/display/listOnly/process) and translated it: "
        "global_awake=%d global_display=%d, %d program row(s)",
        fresh.globalAwake ? 1 : 0, fresh.globalDisplay ? 1 : 0, fresh.n);
  }
  // The same rule as AddEntry applies to the global pair: hand-edited or legacy, a screen-on row is awake.
  if (fresh.globalDisplay)
    fresh.globalAwake = true;

  EnterCriticalSection(&g_lock);
  g_set = fresh;
  LeaveCriticalSection(&g_lock);
  if (g_wake)
    SetEvent(g_wake); // re-decide now rather than up to a second from now
}

bool SaveSettings()
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  char path[560] = {0};
  if (!ConfigPath(path, (int)sizeof(path)))
    return false;
  char tmp[600] = {0};
  if (_snprintf(tmp, sizeof(tmp), "%s.tmp", path) <= 0)
    return false;
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return false;
  fprintf(f, "# KeepAwake -- this feature's settings. Written by the panel, and hand-editable.\n");
  fprintf(f, "#\n");
  // (The row is named in English here because this file's own comments are English -- the row's two titles are
  // for the PAGE, which picks the reader's one; see kGlobalTitleZh/kGlobalTitleEn.)
  fprintf(f, "# global_awake / global_display: the \"%s\" row -- it applies always, from the moment Apex\n",
          kGlobalTitleEn);
  fprintf(f, "#   runs. display=1 also means awake=1: a screen that stays on cannot be a machine that sleeps.\n");
  fprintf(f, "# item=<program>|<awake>|<display>: one line per program, applied only while it is running.\n");
  fprintf(f, "#   `item=chrome.exe` is the same as `item=chrome.exe|1|0`.\n");
  fprintf(f, "# The strongest row wins: if one row asks only for awake and another for awake+display, the\n");
  fprintf(f, "# machine is kept awake AND the screen stays on.\n");
  fprintf(f, "# quick_panel puts this feature's list in the quick panel (the flyout the tray icon shows);\n");
  fprintf(f, "#   0 -- the default -- keeps it out of there. It is NOT the switches above: one decides what the\n");
  fprintf(f, "#   machine does, the other decides where it can be reached from.\n");
  fprintf(f, "global_awake=%d\n", s.globalAwake ? 1 : 0);
  fprintf(f, "global_display=%d\n", s.globalDisplay ? 1 : 0);
  fprintf(f, "quick_panel=%d\n", s.quickPanel ? 1 : 0);
  for (int i = 0; i < s.n; ++i)
    fprintf(f, "item=%s|%d|%d\n", s.e[i].name, s.e[i].awake ? 1 : 0, s.e[i].display ? 1 : 0);
  fclose(f);
  return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}

// ---- what the list is asking for right now -------------------------------------------------------
//
// THE DECISION, IN ONE PLACE, because the panel's list editor and the interface's four places that describe it
// must not be able to disagree about it:
//
//   * the GLOBAL row always counts;
//   * a PROGRAM row counts while a process whose name matches it is running (the SAME matching rule as every
//     other list in the program: fold, strip the path, anchored `*`/`?` -- common/match.h);
//   * the answer is the STRONGEST of them: the only two things a row can say are 1 and 2, so the maximum is the
//     whole of the combination rule.
int LevelOf(bool awake, bool display) { return display ? 2 : (awake ? 1 : 0); }

// Is any PROGRAM row switched on? If none is, the process walk is not worth its few milliseconds.
bool AnyProgramRowOn(const Settings &s)
{
  for (int i = 0; i < s.n; ++i)
    if (s.e[i].awake || s.e[i].display)
      return true;
  return false;
}

// THE STRONGEST LEVEL ANY RUNNING PROGRAM ROW ASKS FOR, and which program decided it.
//
// ⚠️ THE WALK IS THE EXPENSIVE PART (~3.6 ms on this machine, measured for the host's own process lookups), so
// it is done ONCE per cycle and only when a program row is switched on -- that test is `AnyProgramRowOn`.
//
// ⚠️ THE W VARIANT, and the name is converted before it is compared. The A variants of these structures are not
// declared by this toolchain's headers at all (the build stopped on PROCESSENTRY32A), and the wider one is the
// correct one anyway: a process whose name is not ASCII would come back as garbage through the ANSI path, and
// "garbage did not match my entry" is a failure the user cannot see.
int ListedLevel(const Settings &s, char *who, int whoSize)
{
  if (who && whoSize > 0)
    who[0] = 0;
  if (s.n <= 0)
    return 0;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;
  PROCESSENTRY32W pe;
  memset(&pe, 0, sizeof(pe));
  pe.dwSize = sizeof(pe);
  int best = 0;
  if (Process32FirstW(snap, &pe))
  {
    do
    {
      char exe[260] = {0};
      if (WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exe, (int)sizeof(exe) - 1, nullptr, nullptr) <= 0)
        continue;
      char name[kMaxName] = {0};
      apex::match::NormaliseName(exe, name, (int)sizeof(name));
      if (!name[0])
        continue;
      // ⚠️ EVERY ROW IS TESTED, not just the first that matches: two patterns can match one process, and the
      // decision is the strongest of them -- stopping at the first match would make the answer depend on the
      // order the rows happen to be in, which is exactly what "the strongest wins" rules out.
      for (int i = 0; i < s.n; ++i)
      {
        if (!s.e[i].awake && !s.e[i].display)
          continue;
        if (!apex::match::Pattern(s.e[i].name, name))
          continue;
        const int l = LevelOf(s.e[i].awake, s.e[i].display);
        if (l > best)
        {
          best = l;
          if (who && whoSize > 0)
            _snprintf(who, whoSize, "%s", name);
        }
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return best;
}

// ---- the worker ----------------------------------------------------------------------------------
//
// ONE PASS, ONCE A SECOND, FOREVER. Each pass: take a copy of the settings, ask the host whether this feature
// is switched on at all, decide, and only touch the OS when the answer changed (or when the re-assert is due).
DWORD WINAPI Worker(LPVOID)
{
  int applied = -1;
  DWORD lastAssert = 0;

  for (;;)
  {
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
      break;

    Settings s;
    EnterCriticalSection(&g_lock);
    s = g_set;
    LeaveCriticalSection(&g_lock);

    // ⚠️ THE PLUGIN LIST'S SWITCH IS PART OF THE DECISION, not just of whether the host asks us about wheels.
    // This feature does its work on its own thread, so nothing else would notice it being switched off -- and
    // "off" must mean "the screen may go to sleep again", which is the one thing a user would check.
    const bool enabled = !g_host || !g_host->featureEnabled || g_host->featureEnabled(kId) != 0;

    int level = 0;
    char who[kMaxName] = {0};
    bool walked = false;

    if (enabled)
    {
      // 系统全局 first: it always applies, so it is the floor the program rows are compared against.
      if (LevelOf(s.globalAwake, s.globalDisplay) > level)
        level = LevelOf(s.globalAwake, s.globalDisplay);
      if (AnyProgramRowOn(s))
      {
        walked = true;
        char listed[kMaxName] = {0};
        const int l = ListedLevel(s, listed, (int)sizeof(listed));
        if (l > level)
        {
          level = l;
          _snprintf(who, sizeof(who), "%s", listed);
        }
        // (`who` stays empty when the GLOBAL row is what decides, and the log below falls back to its own name
        // rather than naming a program that merely happens to be running.)
      }
    }

    const int wantLevel = level;
    // ⚠️ WHAT THE HOST IS TOLD IS PUBLISHED ON EVERY PASS, NOT ONLY WHEN THE REQUEST CHANGES. The first
    // version of this feature published its flag inside the "the request changed" branch below -- so a change
    // in WHY it was holding (which is a different bit, not a different request) left the stale bits standing
    // and the tray said something the feature was no longer doing. These describe the DECISION; the branch
    // below is about the API CALL.
    InterlockedExchange(&g_level, wantLevel);

    const DWORD now = GetTickCount();
    if (applied < 0 || wantLevel != applied || now - lastAssert >= kReassertMs)
    {
      DWORD flags = ES_CONTINUOUS;
      if (wantLevel >= 1)
        flags |= ES_SYSTEM_REQUIRED;
      if (wantLevel >= 2)
        flags |= ES_DISPLAY_REQUIRED;
      const DWORD prev = SetThreadExecutionState(flags);
      const bool changed = (applied < 0) || wantLevel != applied;
      if (changed)
      {
        char why[200] = {0};
        if (!enabled)
          _snprintf(why, sizeof(why), "the feature is switched off in the plugin list");
        else if (wantLevel == 0)
          _snprintf(why, sizeof(why), "no row is asking: %s",
                    walked ? "no listed program is running" : "every row is switched off");
        else
          _snprintf(why, sizeof(why), "asking: %s wants %s",
                    (who[0] ? who : kGlobalTitleEn), // the log is English, so the row is named in English
                    wantLevel >= 2 ? "awake + the screen on" : "awake");
        LogTransition(wantLevel, flags, prev, why);
        Log("keepawake: level=%d (%s)", wantLevel, why);
      }
      applied = wantLevel;
      lastAssert = now;
    }

    WaitForSingleObject(g_wake, kCycleMs);
    // The event is only ever used to say "look again now"; the next pass re-decides regardless.
  }

  // ⚠️ CLEARING IS NOT OPTIONAL. ES_CONTINUOUS means "until told otherwise", so a feature that goes away
  // without this call leaves the machine unable to sleep for as long as the request lasts -- a power setting
  // changed by a program the user then closed, which is the worst kind of leftover.
  SetThreadExecutionState(ES_CONTINUOUS);

  // ⚠️ AND IT IS SAID OUT LOUD IN THE LOG, because "the screen still does not sleep after I switched it off"
  // has exactly one place to look: whether this line is the last one.
  {
    char ts[32] = {0};
    Stamp(ts, (int)sizeof(ts));
    if (g_log)
    {
      fprintf(g_log, "%s  cleared (ES_CONTINUOUS) -- nothing is blocking sleep or the screen any more\n", ts);
      fflush(g_log);
    }
  }
  Log("keepawake: cleared the power request and stopped");
  return 0;
}

// ---- the panel's data ----------------------------------------------------------------------------
//
// JSON by hand, for the same reason the wheel feature does it by hand: a library for four controls would be
// the largest thing in this DLL. `AppendJsonString` is used for every string, including the ones that "look
// safe" -- a program name comes from the user and a quote in it would make the document unparseable.

void AppendText(char *out, int outSize, int &off, const char *fmt, ...)
{
  if (off >= outSize - 1)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + off, outSize - off, fmt, ap);
  va_end(ap);
  if (n > 0)
    off += n;
  if (off > outSize - 1)
    off = outSize - 1;
}

void AppendJsonString(char *out, int outSize, int &off, const char *s)
{
  AppendText(out, outSize, off, "\"");
  for (const char *p = s ? s : ""; *p; ++p)
  {
    if (*p == '"' || *p == '\\')
      AppendText(out, outSize, off, "\\%c", *p);
    else if ((unsigned char)*p < 0x20)
      AppendText(out, outSize, off, "\\u%04x", (unsigned char)*p);
    else
      AppendText(out, outSize, off, "%c", *p);
  }
  AppendText(out, outSize, off, "\"");
}

// ONE ROW OF THE LIST. `locked` is the 系统全局 row: the page draws no remove button for it (see abi.h), and
// this feature refuses to remove it as well -- a page is not a gatekeeper.
void AddItem(char *out, int outSize, int &off, bool &first, const char *title, bool awake, bool display,
             bool locked)
{
  AppendText(out, outSize, off, "%s{\"title\":", first ? "" : ",");
  first = false;
  AppendJsonString(out, outSize, off, title);
  AppendText(out, outSize, off, "%s\"values\":{\"awake\":%d,\"display\":%d}}", locked ? ",\"locked\":true," : ",",
             awake ? 1 : 0, display ? 1 : 0);
}

// THE 系统全局 ROW, which is the one item whose title is the FEATURE's own words rather than the user's data --
// so it travels in both languages and the page draws the reader's one (`titleZh`/`titleEn` in abi.h). The user
// found the need for this: "系统全局，这几个字，要随 中/英文切换".
void AddGlobalItem(char *out, int outSize, int &off, bool &first, const Settings &s)
{
  AppendText(out, outSize, off, "%s{\"titleZh\":", first ? "" : ",");
  first = false;
  AppendJsonString(out, outSize, off, kGlobalTitleZh);
  AppendText(out, outSize, off, ",\"titleEn\":");
  AppendJsonString(out, outSize, off, kGlobalTitleEn);
  AppendText(out, outSize, off, ",\"locked\":true,\"values\":{\"awake\":%d,\"display\":%d}}",
             s.globalAwake ? 1 : 0, s.globalDisplay ? 1 : 0);
}

// THE SWITCH THAT MAPS THIS WHOLE LIST INTO THE QUICK PANEL (`group.quick` in apex/abi.h, ABI 16 -> 17). It is
// written INSIDE the group it belongs to, because that is what it is about -- the page draws it at the right-hand
// end of the line the list's own controls are on (the add row, for this feature), and it travels through the same
// `setControl` every other control uses.
//
// ⚠️ THE LABEL IS JUST 快速面板, WITH NO ": THE LIST" AFTER IT, AND THAT IS THE POINT OF THE CHANGE. The two
// switches this replaced had to repeat the control they governed ("快速面板：保持唤醒") because they sat among the
// controls and "快速面板" alone said nothing about which one was meant. A switch on the list's own line IS about the
// list -- the row it is on answers the question the label would have had to spell out.
void AddGroupQuickSwitch(char *out, int outSize, int &off, const char *id, bool value)
{
  AppendText(out, outSize, off, ",\"quick\":{\"id\":");
  AppendJsonString(out, outSize, off, id);
  AppendText(out, outSize, off, ",\"labelZh\":");
  AppendJsonString(out, outSize, off, "\xe5\xbf\xab\xe9\x80\x9f\xe9\x9d\xa2\xe6\x9d\xbf"); // 快速面板
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Quick panel");
  AppendText(out, outSize, off, ",\"value\":%d}", value ? 1 : 0);
}

int SettingsJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  int off = 0;
  off += _snprintf(out + off, outSize - off, "{\"params\":[");
  if (off < 0 || off >= outSize)
    return 0;
  bool first = true;
  AppendText(out, outSize, off, "{\"id\":\"rules\",\"type\":\"group\",\"labelZh\":");
  AppendJsonString(out, outSize, off, "程序名单");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Programs");
  // ⚠️ `layout:"rows"` -- one line per item, every field live (see abi.h). A name and two switches have no
  // draft worth having, and this page's whole point is that the switches are one click away.
  AppendText(out, outSize, off, ",\"layout\":\"rows\"");
  // ⚠️ THE ORDER OF THE TWO SWITCHES: the weaker first. The pair is read left to right, and 阻止熄屏 implies
  // 保持唤醒 (see the coupling note above), so the one that implies the other is drawn second.
  AppendText(out, outSize, off, ",\"rowToggle\":[\"awake\",\"display\"]");
  // ⚠️ AND THE ONE SWITCH THAT MAPS THE LIST INTO THE QUICK PANEL (see AddGroupQuickSwitch). It goes here -- with
  // the rest of the group's own description, before its fields -- so that the page can draw it on the group's own
  // line rather than among the rows.
  AddGroupQuickSwitch(out, outSize, off, "quick_panel", s.quickPanel);
  AppendText(out, outSize, off, ",\"addHintZh\":");
  AppendJsonString(out, outSize, off, "例如 chrome.exe");
  AppendText(out, outSize, off, ",\"addHintEn\":");
  AppendJsonString(out, outSize, off, "e.g. chrome.exe");
  AppendText(out, outSize, off, ",\"fields\":[");
  AppendText(out, outSize, off, "{\"id\":\"awake\",\"type\":\"bool\",\"labelZh\":");
  AppendJsonString(out, outSize, off, "保持唤醒");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Keep awake");
  AppendText(out, outSize, off, "},{\"id\":\"display\",\"type\":\"bool\",\"labelZh\":");
  AppendJsonString(out, outSize, off, "阻止熄屏");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Keep the screen on");
  AppendText(out, outSize, off, "}],\"items\":[");
  bool firstItem = true;
  AddGlobalItem(out, outSize, off, firstItem, s);
  for (int i = 0; i < s.n; ++i)
    AddItem(out, outSize, off, firstItem, s.e[i].name, s.e[i].awake, s.e[i].display, false);
  AppendText(out, outSize, off, "]}");
  // ⚠️ THE TWO PER-CONTROL QUICK-PANEL SWITCHES USED TO BE EMITTED HERE, AS PARAMETERS. They are gone: the one
  // switch that replaced them lives INSIDE the group above (`group.quick`), because that is where the user put it
  // -- "放在「添加」按钮右侧" -- and because what it decides is about the list as a whole. Nothing else changed:
  // `quick_panel` travels through the same `setControl` they did.
  //
  // That closed `items` and `rules`; the params array is still open, which is where this feature's summary goes.

  AppendText(out, outSize, off, "],\"settingsFile\":\"KeepAwake.ini\",\"summaryZh\":");
  AppendJsonString(out, outSize, off, "阻止系统睡眠和屏幕关闭；可以只对指定程序生效");
  AppendText(out, outSize, off, ",\"summaryEn\":");
  AppendJsonString(out, outSize, off,
                   "Stops the system sleeping and the screen turning off -- always, or only while a chosen "
                   "program is running");
  AppendText(out, outSize, off, "}");
  if (off >= outSize)
    return 0; // a truncated document must be an error, not a document the page tries to parse
  return off;
}

// ---- the controls ------------------------------------------------------------------------------
//
// ⚠️ THE VALUE ARRIVES AS TEXT (ABI 6) and is parsed HERE, by the feature that knows what its own controls
// are. An unparseable value is REFUSED (return 0) rather than coerced, so a bad value cannot silently become
// "off" -- and the switch on the page is re-read from this feature's own answer afterwards.
int ApplyControl(const char *path, const char *value)
{
  if (!path || !value)
    return 0;
  bool v = false;
  if (!ParseBool(value, &v))
    return 0;
  // ⚠️ THE QUICK-PANEL SWITCH COMES FIRST, AND IT IS NOT A ROW FIELD. ApplyRowSwitch understands exactly one path
  // shape -- "rules[<i>].<field>" -- and this is this feature's own answer to "may the quick panel reach this
  // list" (see the field in Settings). Routing it through the row parser would have meant teaching it a second
  // grammar for one boolean.
  if (strcmp(path, "quick_panel") == 0)
  {
    EnterCriticalSection(&g_lock);
    g_set.quickPanel = v;
    LeaveCriticalSection(&g_lock);
    // NO SetEvent: the machine's behaviour has not changed, only where the switches can be reached from. Waking
    // the worker here would make it re-ask Windows for nothing.
    return 1;
  }
  bool changed = false;
  EnterCriticalSection(&g_lock);
  const bool ok = ApplyRowSwitch(path, v, &changed);
  LeaveCriticalSection(&g_lock);
  if (!ok)
    return 0;
  if (changed && g_wake)
    SetEvent(g_wake); // the worker re-decides now: a switch must not take a second to mean something
  return 1;
}

// ---- THE QUICK PANEL ----------------------------------------------------------------------------
//
// THE WHOLE LIST -- the 系统全局 row and every program row -- ONE LINE EACH, WITH BOTH OF ITS SWITCHES. The user's
// words: "保持唤醒插件的快速面板给一个开关…快速面板按列表显示系统和各应用的两个功能开关".
//
// ⚠️ THIS REPLACED "THE TWO MASTER SWITCHES AND NOTHING ELSE", AND THE REASON IT COULD IS THE SHAPE OF A ROW. The
// old version mapped the 系统全局 row's two switches, one per switch, because a flyout row held one control; the
// program rows were left out on the grounds that "a name has to be typed to add one, and a flyout has no room for
// a text box" -- which was true and beside the point: the rows are ALREADY in the list, and what the flyout is
// for is reaching the ones you have. So the mapping is the list, and the row that means "always" is simply its
// first line (which is also the order the settings page shows them in).
//
// ⚠️ THE ROW'S OWN SWITCH IS 保持唤醒 AND THE COMPANION BUTTON IS 阻止熄屏 (see `toggleId`/`toggleIcon` in abi.h;
// the icon is what tells the second switch apart from the first). The coupling between them is NOT re-implemented
// here: switching the screen button on writes `rules[i].display`, `ApplyRowSwitch` brings `awake` up with it, and
// the host redraws the row from what this function reports on the very next frame -- which is the whole reason a
// feature answers with a document instead of the panel assuming its own click worked.
//
// ⚠️ AND ONLY WHEN `quick_panel` IS ON. The switch on this feature's page is the permission (see the field in
// Settings and the user's rule quoted there): with it off nothing is even SENT, because a panel can only draw a
// row somebody described.
static void QuickCopy(char *dst, int cap, const char *src)
{
  int n = 0;
  if (src)
    for (; src[n] && n < cap - 1; ++n)
      dst[n] = src[n];
  dst[n] = 0;
}

static void QuickToggle(ApexQuickItem *q, const char *id, const char *zh, const char *en, bool on)
{
  QuickCopy(q->id, (int)sizeof(q->id), id);
  QuickCopy(q->labelZh, (int)sizeof(q->labelZh), zh);
  QuickCopy(q->labelEn, (int)sizeof(q->labelEn), en);
  q->type = APEX_QUICK_TOGGLE;
  q->min = 0.0;
  q->max = 1.0;
  q->step = 1.0;
  q->value = on ? 1.0 : 0.0;
  // The pane the whole list shares, in both languages (the host compares both to decide what belongs together).
  QuickCopy(q->groupZh, (int)sizeof(q->groupZh), kQuickGroupZh);
  QuickCopy(q->groupEn, (int)sizeof(q->groupEn), kQuickGroupEn);
}

// The second switch of a row: 阻止熄屏. ⚠️ IT CARRIES NO ICON, AND THAT IS NOT AN OMISSION: the panel draws the
// companion of a SWITCH row as a second switch -- the same sliding control as the 保持唤醒 one beside it, which is
// what the user asked for after seeing the compact button there ("保持唤醒的快速面板…是要用一样的滑动开关"). An icon
// is the picture of a BUTTON (`toggleIcon`, and the panel only consults it for a fader row's companion), so sending
// one here would be a field nothing reads.
static void QuickScreenSwitch(ApexQuickItem *q, int row, bool on)
{
  _snprintf(q->toggleId, sizeof(q->toggleId), "rules[%d].display", row);
  q->toggleOn = on ? 1 : 0;
}

static int KaQuickItems(ApexQuickItem *out, int max)
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  // One row for 系统全局 plus one per program row -- and it is reported even with no room for it, because that is
  // the two-part answer the ABI asks for.
  const int total = s.quickPanel ? (1 + s.n) : 0;
  if (!out || max <= 0)
    return total;
  ZeroMemory(out, sizeof(ApexQuickItem) * (size_t)max);
  if (!s.quickPanel)
    return total;

  int n = 0;
  // "rules[0]" is the GLOBAL row -- the page's numbering, which is the numbering this feature publishes (see
  // ApplyRowSwitch). The ids therefore go through the SAME setter the settings page uses, coupling and all.
  if (n < max)
  {
    QuickToggle(&out[n], "rules[0].awake", kGlobalTitleZh, kGlobalTitleEn, s.globalAwake);
    QuickScreenSwitch(&out[n], 0, s.globalDisplay);
    ++n;
  }
  // The program rows, in the order the user's file lists them -- which is the order the settings page shows, so
  // the two can be read against each other. The name is the USER's data and has no translation (the same string
  // in both languages, exactly as the settings page draws it).
  for (int i = 0; i < s.n && n < max; ++i)
  {
    char id[64] = {0};
    _snprintf(id, sizeof(id), "rules[%d].awake", i + 1);
    QuickToggle(&out[n], id, s.e[i].name, s.e[i].name, s.e[i].awake);
    QuickScreenSwitch(&out[n], i + 1, s.e[i].display);
    ++n;
  }
  return total;
}

// The list. `add` carries the typed text in `value` (see `addHintZh` in abi.h: a row is identified by its
// name, so there is nothing to type it into afterwards), `remove` names the row in `index`.
int ListOp(const char *id, const char *op, const char *value, int index)
{
  if (!id || strcmp(id, "rules") != 0 || !op)
    return 0;
  bool ok = false;
  EnterCriticalSection(&g_lock);
  if (strcmp(op, "add") == 0)
  {
    // ⚠️ A NEW ROW STARTS AS "keep awake", WITH THE SCREEN SWITCH OFF. Both switches off would be a row that
    // does nothing at all -- the user typed a program name because they wanted something to happen, and the
    // weaker of the two things this feature can do is the honest default. (The GLOBAL row starts off, and that
    // is the user's own decision about the program as a whole: nothing happens until they ask for it.)
    ok = AddEntry(g_set, value, true, false);
  }
  else if (strcmp(op, "remove") == 0)
  {
    // ⚠️ INDEX 0 IS 系统全局 AND IT IS NOT REMOVABLE. The page draws no button for it, and this is the other
    // half: the feature refuses the op it would send if the page were wrong, or older, or edited by hand.
    if (index >= 1 && index <= g_set.n)
    {
      for (int i = index - 1; i < g_set.n - 1; ++i)
        g_set.e[i] = g_set.e[i + 1];
      --g_set.n;
      ok = true;
    }
  }
  // No "move": the order of this list is not meaning (see the layout note in abi.h) and the page sends none.
  LeaveCriticalSection(&g_lock);
  if (ok && g_wake)
    SetEvent(g_wake);
  return ok ? 1 : 0;
}

// ---- the ABI surface ---------------------------------------------------------------------------

// ⚠️⚠️ `init` RETURNS **0 FOR "I AM READY"** AND NON-ZERO TO REFUSE -- and the direction is worth spelling out
// because `abi.h` said the opposite and that single inverted line cost an afternoon: returning 1 here made the
// host treat a perfectly working feature as refused, unload the DLL, and leave this feature's OWN WORKER
// THREAD running inside an unmapped module. The symptom was the whole host dying with SIGSEGV a second after
// startup, with a thread whose entry address was no longer memory -- and nothing in any log saying why, because
// the refusal branch writes to the panel's reason field and not to the log (that is fixed too).
//
// Both other features agree: SmoothWheel's SwsInit returns 0, AutoIME's fe_init returns 1 only to refuse.
int KeepInit(const ApexHost *host)
{
  g_host = host;
  if (host && host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));
  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  OpenLog();
  LoadSettings();
  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (!g_thread)
  {
    // No thread means no power request ever gets made, so this is a REFUSAL rather than a quiet failure: the
    // host unloads the feature and the panel shows why, which is the honest answer -- a feature that looked
    // switched on and did nothing would be the worst of both.
    Log("keepawake: could not start the worker thread (error %lu) -- refusing to load",
        (unsigned long)GetLastError());
    return 1; // ⚠️ NON-ZERO MEANS REFUSE (see the note on KeepInit itself)
  }
  Log("keepawake: started (global %d/%d, %d program row(s))", g_set.globalAwake ? 1 : 0,
      g_set.globalDisplay ? 1 : 0, g_set.n);
  return 0; // READY (not "refused" -- see the note above)
}

void KeepShutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_wake)
    SetEvent(g_wake);
  if (g_thread)
  {
    // The worker's cycle waits on the event, so this is at most a few milliseconds, not a second.
    if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0)
      Log("keepawake: the worker did not stop in time; the power request will be cleared by the OS when this "
          "process ends");
    CloseHandle(g_thread);
    g_thread = nullptr;
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
    DeleteCriticalSection(&g_lock);
}

int KeepReload()
{
  LoadSettings();
  return 1;
}

int KeepSetControl(const char *path, const char *value) { return ApplyControl(path, value); }
int KeepListOp(const char *id, const char *op, const char *value, int index)
{
  return ListOp(id, op, value, index);
}
int KeepSave() { return SaveSettings() ? 1 : 0; }

unsigned KeepFlags()
{
  // ⚠️ ALWAYS ENABLED, and the host's own switch is honoured by the WORKER instead (see the note in it): this
  // bit says "this feature is loaded and usable", not "its master switch is on" -- that is the host's, and a
  // feature that reported 0 here while its worker was holding a power request would be lying to the panel.
  unsigned f = APEX_FEATURE_ENABLED;
  // ⚠️ THIS IS WHY THE HOST CAN SAY ANYTHING AT ALL (abi.h): while a power request is held, these bits make the
  // tray mark say WHICH of the two things is being held -- the bar goes green for an ordinary hold and red for
  // the one that also keeps the screen on. (There used to be a third bit asking for a one-line notification;
  // the user removed it, so the mark is the whole signal, and that is why the mark had to be made to say the
  // difference at all.)
  const int level = (int)InterlockedCompareExchange(&g_level, 0, 0);
  if (level >= 1)
    f |= APEX_FEATURE_USER_VISIBLE;
  if (level >= 2)
    f |= APEX_FEATURE_HOLD_HARD;
  return f;
}

} // namespace

// ---------------------------------------------------------------------------
// THE EXPORTED STRUCTURE. `structSize` is checked by the host BEFORE anything below is read.
//
// NO onWheel AND NO tick: this feature has nothing to do with the wheel, and a feature that is not in the
// input path cannot slow the input path down. (`flags` still reports ENABLED, which is what makes the host
// list it as a working feature.)
// ---------------------------------------------------------------------------
static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    kId,
    "保持唤醒",
    "Keep Awake",
    kVersion,
    KeepInit,
    KeepShutdown,
    KeepReload,
    SettingsJson,
    KeepSetControl,
    KeepListOp,
    KaQuickItems, // the two master switches in the quick panel (see the note above it)
    KeepSave,
    nullptr, // onWheel
    nullptr, // tick
    KeepFlags,
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
