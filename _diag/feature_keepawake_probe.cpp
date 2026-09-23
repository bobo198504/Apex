// ---------------------------------------------------------------------------
// KeepAwake, driven through the REAL DLL: the one list, its two coupled switches per row, the undeletable
// 系统全局 row, and WHAT IT ASKS WINDOWS FOR.
//
// WHY A PROBE RATHER THAN TRUSTING THE CODE READ. This feature's output is invisible: nothing on screen changes
// when it works, and the only other evidence is whether the user's screen stayed on overnight. Every failure
// mode is therefore silent -- a worker that never starts, a request made with the wrong bits, a row that is
// ignored, a power request that is never CLEARED when the feature is switched off. The last one is the worst,
// and it is why this exists.
//
// HOW IT OBSERVES THE OS CALL: `SetThreadExecutionState` cannot be read back from outside the thread that made
// the call, so the feature writes each TRANSITION to its own log with the exact flags it passed. This probe
// reads that log and checks the bit pattern -- ES_CONTINUOUS|ES_SYSTEM_REQUIRED is 0x80000001, with the display
// added 0x80000003, and a cleared request is 0x80000000. That is the closest thing to "Windows was actually
// asked" that a gate without administrator rights can have (`powercfg /requests` needs elevation, measured).
//
// ⚠️ THE ROWS ARE THE MODEL NOW, SO THE CHECKS ARE ABOUT ROWS: the two switches of one row are COUPLED
// (阻止熄屏 implies 保持唤醒 -- Windows cannot keep a screen on while letting the machine sleep), 系统全局
// cannot be removed, and the answer across all rows is the STRONGEST one. Every one of those is a rule a user
// would only ever discover by leaving the machine running for an hour, which is exactly what a gate is for.
//
// ⚠️ `init` IS CALLED FIRST, unlike the chart probe (which avoids it because it only reads a document). This
// feature's controls are guarded by a lock that init creates, and its whole job happens on a thread init
// starts -- a probe that called setControl before init would be testing an uninitialised feature, not this one.
//
// IT DOES NOT TOUCH ANY OTHER FEATURE. The host stub's `featureDir` points at a directory the caller gives it
// (the gate hands it a fresh one under build/), so the settings file it writes is its own and no deployed or
// sibling feature's file is opened by this run.
//
// Build: g++ -std=c++17 -O2 -I apex -I common -o build/_keepawake_probe.exe _diag/feature_keepawake_probe.cpp
// Run:   build/_keepawake_probe.exe build/apex/Plugins/KeepAwake/KeepAwake.dll build/_keepawake_probe/
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi.h"
#include "match.h"

typedef const ApexFeature *(*EntryFn)(void);

static int failures = 0;
static void Check(bool ok, const char *what, const char *detail = "")
{
  printf("  %-64s %s%s%s\n", what, ok ? "ok" : "FAIL", detail[0] ? "  " : "", detail);
  if (!ok)
    ++failures;
}

static char g_dir[512] = {0};      // the directory this probe tells the feature it lives in
static int g_enabled = 1;          // what the host stub answers for "is this feature switched on"
static char g_dump[600] = {0};     // OPTIONAL third argument: write the controls document HERE

// ⚠️ WHY A PROBE DUMPS ITS DOCUMENT. The panel's layout cannot be judged from a DOM stub (a stub has no layout
// and no hit testing -- see the note in apex_panel_probe.js), so the only way to see a page as it will really
// look is to render it in a browser. That needs the EXACT document the feature sends, and a hand-written
// "document that looks about right" is the mistake this project has already paid for twice: a page fed a
// format the feature no longer sends reports success while the real page is empty.
static void DumpDoc(ApexFeature *f, const char *why)
{
  if (!g_dump[0])
    return;
  static char text[64 * 1024];
  const int n = f->settingsJson(text, (int)sizeof(text));
  FILE *fp = fopen(g_dump, "wb");
  if (!fp || n <= 0)
  {
    printf("      (could not write %s)\n", g_dump);
    if (fp)
      fclose(fp);
    return;
  }
  fwrite(text, 1, (size_t)n, fp);
  fclose(fp);
  printf("      [document with %s] -> %s\n", why, g_dump);
}

static int HostFeatureDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  const int n = _snprintf(out, outSize, "%s", g_dir);
  return n > 0 ? n : 0;
}

static void HostLogLine(const char *text) { printf("      [feature] %s\n", text); }

static int HostFeatureEnabled(const char *) { return g_enabled; }

static int Doc(ApexFeature *f, char *out, int outSize)
{
  out[0] = 0;
  return f->settingsJson(out, outSize);
}

static bool Has(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

static int CountOf(const char *hay, const char *needle)
{
  int n = 0;
  for (const char *p = hay; (p = strstr(p, needle)) != nullptr; p += strlen(needle))
    ++n;
  return n;
}

// The feature's own log, as one string. Empty when it does not exist (which is itself a failure worth seeing).
static bool ReadLog(const char *dir, char *out, int outSize)
{
  char path[600] = {0};
  _snprintf(path, sizeof(path), "%skeepawake.log", dir);
  FILE *f = fopen(path, "rb");
  if (!f)
  {
    out[0] = 0;
    return false;
  }
  const size_t n = fread(out, 1, outSize - 1, f);
  fclose(f);
  out[n] = 0;
  return true;
}

// The LAST flags value in the log -- which is the request currently held.
static bool LastFlags(const char *log, unsigned long *out)
{
  unsigned long v = 0;
  bool found = false;
  const char *p = log;
  while ((p = strstr(p, "flags=0x")) != nullptr)
  {
    p += 8; // "flags=0x" is EIGHT characters: skipping seven parsed the 'x' and every value came back 0
    v = strtoul(p, nullptr, 16);
    found = true;
  }
  if (found)
    *out = v;
  return found;
}

static char g_detail[64] = {0};
static const char *Hex(unsigned long v)
{
  _snprintf(g_detail, sizeof(g_detail), "0x%lx", v);
  return g_detail;
}

// The worker wakes on its event as soon as a setting changes, so this is generous.
static void Settle(void) { Sleep(350); }

// How many rows the document carries -- the probe counts rows rather than assuming, because most of the checks
// below are about what happened to the LIST.
//
// ⚠️ IT COUNTS `"values":{`, NOT `"title"`. The 系统全局 row carries `titleZh`/`titleEn` instead of `title` (it is
// a label rather than the user's data -- see abi.h), so a count of `"title":` silently missed it, and the row
// INDEX derived from the same pattern was one too low: the probe then switched on a different row and reported
// failures that were about its own arithmetic. One field per item is what is being counted; the title's spelling
// is not the probe's business.
static int RowCount(const char *doc) { return CountOf(doc, "\"values\":{"); }

// ⚠️ WHICH ROW A NAME ENDED UP IN IS READ OUT OF THE DOCUMENT, NOT ASSUMED. `add` appends, but every earlier
// edit in this probe has been moving rows around -- and a probe that hard-codes "the row I just added is
// rules[1]" is a probe that silently starts testing a different row the moment any line above it changes.
//
// ⚠️ AND THE COUNT IS OF `"values":{`, WHICH IS WHAT AN ITEM HAS EXACTLY ONE OF. Counting titles instead is a
// trap that SPRANG: the 系统全局 row carries `titleZh` AND `titleEn`, and `"title` is a prefix of both, so the
// index came out one too high and the probe addressed the row after the one it meant. (The symptom was three
// unrelated-looking failures -- a switch that "did not take", a request that "was not cleared" -- all of them the
// probe talking about the wrong row.)
static int RowIndexOf(const char *doc, const char *title)
{
  char pat[400] = {0};
  _snprintf(pat, sizeof(pat), "\"title\":\"%s\"", title);
  const char *p = strstr(doc, pat);
  if (!p)
    return -1;
  int n = 0;
  for (const char *q = doc; (q = strstr(q, "\"values\":{")) != nullptr && q < p; q += 10)
    ++n;
  return n;
}

// Write a file the way the FIRST version of this feature would have, so the translation into the new format is
// exercised rather than taken on trust.
static void WriteLegacy(const char *dir, const char *text)
{
  char path[600] = {0};
  _snprintf(path, sizeof(path), "%sKeepAwake.ini", dir);
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}

int main(int argc, char **argv)
{
  const char *dll = (argc >= 2) ? argv[1] : "build/apex/Plugins/KeepAwake/KeepAwake.dll";
  const char *dir = (argc >= 3) ? argv[2] : "build/_keepawake_probe/";
  _snprintf(g_dir, sizeof(g_dir), "%s", dir);
  if (argc >= 4)
    _snprintf(g_dump, sizeof(g_dump), "%s", argv[3]);

  HMODULE mod = LoadLibraryA(dll);
  if (!mod)
  {
    printf("cannot load %s (err %lu)\n", dll, GetLastError());
    return 2;
  }
  EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  if (!entry)
  {
    printf("no ApexFeatureEntry in %s\n", dll);
    return 2;
  }
  ApexFeature *f = (ApexFeature *)entry();
  printf("KeepAwake -- driven through %s\n", dll);
  if (f->abiVersion != APEX_ABI_VERSION || f->structSize < sizeof(ApexFeature))
  {
    printf("  FAIL: the DLL does not match this host's ABI\n");
    return 1;
  }
  Check(f->id && strcmp(f->id, "KeepAwake") == 0, "the feature identifies itself as KeepAwake",
        f->id ? f->id : "");
  Check(f->nameZh && f->nameZh[0] && f->nameEn && f->nameEn[0], "  and carries both display names", "");
  Check(f->onWheel == nullptr && f->tick == nullptr,
        "  and stays out of the input path (no onWheel, no tick)", "");

  ApexHost host;
  memset(&host, 0, sizeof(host));
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLogLine;
  host.featureEnabled = HostFeatureEnabled;

  printf("\n1. the controls it describes (from a clean folder, so these are the defaults)\n");
  // ⚠️ ZERO MEANS READY. The ABI's own comment had this backwards, and this probe asserted `== 1` for a while
  // -- it PASSED, because the feature also returned the wrong value: two wrongs that agreed with each other,
  // while the host read 1 as a refusal, unloaded the DLL and left the feature's worker thread running in
  // unmapped memory (SIGSEGV a second after startup). A probe that asserts what the code does instead of what
  // the contract says is worse than no probe.
  Check(f->init(&host) == 0, "init reports READY (0) and starts the feature", "");
  static char doc[64 * 1024];
  int n = Doc(f, doc, (int)sizeof(doc));
  Check(n > 0, "it describes its controls", "");
  Check(Has(doc, "\"settingsFile\":\"KeepAwake.ini\""), "  and names its own settings file", "");
  Check(Has(doc, "\"summaryZh\":") && Has(doc, "\"summaryEn\":"),
        "  and says in one line what it is for", "");

  // THE SHAPE: ONE list (a group), drawn as rows, with two switches in each row and a text box to add a name.
  Check(Has(doc, "\"id\":\"rules\",\"type\":\"group\""), "the list is a single group", "");
  Check(Has(doc, "\"layout\":\"rows\""), "  drawn as rows (one line per entry, every field live)", "");
  Check(Has(doc, "\"rowToggle\":[\"awake\",\"display\"]"),
        "  with BOTH switches in the row, in the order the feature declares", "");
  Check(Has(doc, "\"id\":\"awake\",\"type\":\"bool\"") && Has(doc, "\"id\":\"display\",\"type\":\"bool\""),
        "  the two switches are fields of the row", "");
  Check(Has(doc, "\"addHintZh\":") && Has(doc, "\"addHintEn\":"),
        "  and it asks for a text box to add a program by name", "");
  Check(Has(doc, "\"settingsFile\""), "  and the page is told which file this feature keeps", "");
  // ⚠️ AND THE SWITCH THAT MAPS THE LIST INTO THE QUICK PANEL IS ON THE GROUP, NOT A PARAMETER OF ITS OWN (ABI 16
  // -> 17). It used to be TWO top-level `bool` params ("快速面板：保持唤醒" / "快速面板：阻止熄屏"), which is a stack of
  // switches at the bottom of the page for a decision about the list as a whole; the user's correction was
  // "保持唤醒插件的快速面板给一个开关，放在「添加」按钮右侧". The page draws it on the group's own line, which it can only
  // do if the group is what carries it (see `quick` in apex/abi.h) -- so this asserts the SHAPE, and the name of
  // the control path the page will write through.
  Check(Has(doc, "\"quick\":{\"id\":\"quick_panel\"") && Has(doc, "\"Quick panel\",\"value\":0}"),
        "the list carries ONE quick-panel switch, inside the group it maps, OFF by default", "");
  Check(!Has(doc, "\"id\":\"quick_awake\"") && !Has(doc, "\"id\":\"quick_display\""),
        "  and the two per-switch parameters it replaced are gone", "");

  // THE 系统全局 ROW: always there, in both languages, and not the user's to delete.
  Check(Has(doc, "\"titleZh\":\"系统全局\"") && Has(doc, "\"titleEn\":\"System-wide\"") &&
            Has(doc, "\"locked\":true"),
        "the first row is 系统全局 / System-wide, in both languages, and LOCKED (no remove button)", "");
  Check(RowCount(doc) == 1, "  and it is the only row to begin with -- no name arrives from anywhere else",
        "rows=1?");
  Check(Has(doc, "\"awake\":0,\"display\":0"),
        "  with both of its switches OFF (the user's default: the feature does nothing)", "");

  printf("\n2. the panel's edits, row by row\n");
  Check(f->setControl("rules[0].awake", "1") == 1, "switching the global row's 保持唤醒 on is accepted");
  Check(f->setControl("rules[0].display", "1") == 1, "  and 阻止熄屏 is accepted as well");
  Doc(f, doc, (int)sizeof(doc));
  Check(Has(doc, "\"awake\":1,\"display\":1"), "  both of them show as on", "");
  Check(f->setControl("rules[0].display", "off") == 1, "the words off/false are understood too");
  Doc(f, doc, (int)sizeof(doc));
  Check(Has(doc, "\"awake\":1,\"display\":0"), "  and the row keeps 保持唤醒 while the screen switch goes off", "");
  Check(f->setControl("rules[0].awake", "maybe") == 0,
        "a value that is not a switch is REFUSED, not guessed at", "");
  Check(f->setControl("rules[0].nosuch", "1") == 0, "  and so is a field this feature does not have", "");
  Check(f->setControl("rules[7].awake", "1") == 0, "  and a row that is not there", "");

  // ⚠️ THE COUPLING, IN BOTH DIRECTIONS. This is the rule the user described ("防止熄屏肯定不能睡眠") and it is
  // the one thing about this feature a page cannot enforce on its own -- the page re-reads the controls after
  // every edit, which is how the user SEES the other switch correct itself.
  Check(f->setControl("rules[0].display", "1") == 1, "switching 阻止熄屏 on");
  Doc(f, doc, (int)sizeof(doc));
  Check(Has(doc, "\"awake\":1,\"display\":1"), "  turns 保持唤醒 on with it (no screen-on without awake)", "");
  Check(f->setControl("rules[0].awake", "0") == 1, "switching 保持唤醒 off");
  Doc(f, doc, (int)sizeof(doc));
  Check(Has(doc, "\"awake\":0,\"display\":0"), "  takes 阻止熄屏 down with it", "");

  printf("\n3. adding and removing rows\n");
  Check(f->listOp("rules", "add", "C:\\Games\\VLC.EXE", 0) == 1,
        "adding a program accepts a pasted path");
  Doc(f, doc, (int)sizeof(doc));
  Check(Has(doc, "\"title\":\"vlc.exe\""), "  and stores it as a bare lower-case name", "");
  Check(Has(doc, "\"awake\":1,\"display\":0"), "  switched on for 保持唤醒, screen switch off", "");
  Check(RowCount(doc) == 2, "  the list grew by one row", "");
  Check(f->listOp("rules", "add", "vlc.exe", 0) == 0, "  a duplicate is refused", "");
  Check(f->listOp("rules", "add", "   ", 0) == 0, "  and an empty name is refused", "");
  Check(f->listOp("rules", "add", "zoom.exe", 0) == 1, "a second program is accepted", "");
  Check(f->listOp("swallow", "add", "vlc.exe", 0) == 0,
        "  and a list this feature does not own is refused", "");
  // ⚠️ INDEX 0 IS THE 系统全局 ROW AND IT IS NOT REMOVABLE. The page draws no button for it; this is the other
  // half, and it is the half that matters -- the page is not a gatekeeper.
  Check(f->listOp("rules", "remove", "", 0) == 0, "REMOVING 系统全局 is refused, even though the page has no button for it", "");
  Check(f->listOp("rules", "remove", "", 9) == 0, "  and a row that is not there cannot be removed", "");
  Check(f->listOp("rules", "move", "1", 1) == 0, "  (and this list has no order to change)", "");
  Check(f->listOp("rules", "remove", "", 1) == 1, "removing a program row by index works", "");
  Doc(f, doc, (int)sizeof(doc));
  Check(!Has(doc, "\"title\":\"vlc.exe\"") && Has(doc, "\"title\":\"zoom.exe\""),
        "  and takes the right row out", "");
  Check(Has(doc, "\"titleZh\":\"系统全局\""), "  with 系统全局 still there", "");  // A real document with several rows, for looking at the page in a browser (see DumpDoc).
  DumpDoc(f, "two program rows");

  printf("\n4. what it asks Windows for (read out of the feature's own log)\n");
  static char log[64 * 1024];
  unsigned long flags = 0;

  // (a) 系统全局 with 保持唤醒 only: the system request, and nothing about the screen.
  f->setControl("rules[0].awake", "1");
  f->setControl("rules[0].display", "0");
  Settle();
  Check(ReadLog(dir, log, (int)sizeof(log)), "the feature logs what it asked for", "");
  if (LastFlags(log, &flags))
    Check(flags == 0x80000001ul, "保持唤醒 alone -> ES_CONTINUOUS|ES_SYSTEM_REQUIRED (0x80000001)", Hex(flags));
  else
    Check(false, "  and the request is in the log", "");

  f->setControl("rules[0].display", "1");
  Settle();
  ReadLog(dir, log, (int)sizeof(log));
  if (LastFlags(log, &flags))
    Check(flags == 0x80000003ul, "  + 阻止熄屏 -> both bits (0x80000003)", Hex(flags));
  else
    Check(false, "  and the request is in the log", "");

  f->setControl("rules[0].awake", "0");
  Settle();
  ReadLog(dir, log, (int)sizeof(log));
  if (LastFlags(log, &flags))
    Check(flags == 0x80000000ul, "both switches off -> NOTHING is asked for (0x80000000)", Hex(flags));
  else
    Check(false, "  and the request is in the log", "");

  // (b) A PROGRAM ROW ALONE. The global row is off, so this is the list doing the work -- and the program has
  // to be one that is really running, which is why it is this probe's own name.
  char self[600] = {0};
  GetModuleFileNameA(nullptr, self, (int)sizeof(self));
  const char *base = strrchr(self, '\\');
  base = base ? base + 1 : self;
  char selfName[64] = {0};
  apex::match::NormaliseName(base, selfName, (int)sizeof(selfName)); // the name the list will hold
  f->listOp("rules", "add", base, 0);
  Doc(f, doc, (int)sizeof(doc));
  const int selfRow = RowIndexOf(doc, selfName);
  Check(selfRow >= 1, "this probe's own program was added as a row", selfName);
  char path[64] = {0};
  _snprintf(path, sizeof(path), "rules[%d].awake", selfRow);
  f->setControl(path, "1");
  _snprintf(path, sizeof(path), "rules[%d].display", selfRow);
  f->setControl(path, "0");
  Settle();
  ReadLog(dir, log, (int)sizeof(log));
  if (LastFlags(log, &flags))
    Check(flags == 0x80000001ul, "a listed program running -> its own row holds the machine awake", Hex(flags));
  else
    Check(false, "  and the request is in the log", "");
  Check(Has(log, selfName), "  and the log names the row that decided it", selfName);

  // (c) THE COMBINATION RULE: the strongest row wins, whichever row it is.
  f->setControl("rules[0].display", "1"); // the global row now asks for awake + the screen
  Settle();
  ReadLog(dir, log, (int)sizeof(log));
  if (LastFlags(log, &flags))
    Check(flags == 0x80000003ul, "a weaker row AND a stronger row -> the stronger one decides (0x80000003)",
          Hex(flags));
  else
    Check(false, "  and the request is in the log", "");
  // ⚠️ THE LOG IS ENGLISH, so the 系统全局 row is named in English there ("System-wide") while the PAGE draws
  // whichever language the reader has -- the row carries both titles for exactly that reason (see abi.h).
  Check(Has(log, "System-wide"), "  and the log says which row that was", "");

  // ... and the weaker one on its own, with the stronger one switched off again: the machine stays awake, and
  // the screen may go. That is the other half of the same rule, and it is the half a "sticky" bug would break.
  f->setControl("rules[0].awake", "0"); // takes the global row's display switch down with it
  Settle();
  ReadLog(dir, log, (int)sizeof(log));
  if (LastFlags(log, &flags))
    Check(flags == 0x80000001ul, "  with only the weaker row left, the screen is let go again (0x80000001)",
          Hex(flags));
  else
    Check(false, "  and the request is in the log", "");

  printf("\n5. what the host is told, which is now the ONLY way the user sees this feature\n");
  _snprintf(path, sizeof(path), "rules[%d].display", selfRow);
  f->setControl(path, "1");
  Settle();
  // ⚠️ THE READ-OUT IS GONE, SO THE TRAY BITS ARE THE WHOLE CHANNEL. This feature used to publish a one-line
  // sentence for the panel as well (`liveText`); the user removed it ("其实这个提示可以完全去掉。并不需要") and the
  // field went out of the ABI with it (9 -> 10). What is left -- and what these checks are now the only cover
  // for -- is the two bits that colour the tray mark.
  {
    const unsigned bits = f->flags ? f->flags() : 0;
    Check((bits & APEX_FEATURE_USER_VISIBLE) != 0 && (bits & APEX_FEATURE_HOLD_HARD) != 0,
          "the stronger hold reports USER_VISIBLE + HOLD_HARD (the tray bar goes RED)", "");
    // ... and the weaker one is told apart from it, which is the whole reason there are two bits.
    _snprintf(path, sizeof(path), "rules[%d].display", selfRow);
    f->setControl(path, "0");
    Settle();
    const unsigned weak = f->flags ? f->flags() : 0;
    Check((weak & APEX_FEATURE_USER_VISIBLE) != 0 && (weak & APEX_FEATURE_HOLD_HARD) == 0,
          "  the ordinary hold reports USER_VISIBLE alone (the tray bar goes GREEN)", "");
  }

  // (d) THE PLUGIN LIST'S SWITCH: off must CLEAR the request rather than leave it standing.
  g_enabled = 0;
  _snprintf(path, sizeof(path), "rules[%d].awake", selfRow);
  f->setControl(path, "0"); // a settings change, so the worker looks again at once
  f->setControl(path, "1");
  Settle();
  ReadLog(dir, log, (int)sizeof(log));
  if (LastFlags(log, &flags))
    Check(flags == 0x80000000ul, "switched off in the plugin list -> CLEARED (0x80000000)", Hex(flags));
  else
    Check(false, "  and the request is in the log", "");
  {
    const unsigned bits = f->flags ? f->flags() : 0;
    Check((bits & (APEX_FEATURE_USER_VISIBLE | APEX_FEATURE_HOLD_HARD)) == 0,
          "  and the mark is taken down with it (neither holding bit)", "");
  }
  g_enabled = 1;

  printf("\n6. what it wrote, and what it does with an older file\n");
  static char ini[600] = {0};
  _snprintf(ini, sizeof(ini), "%sKeepAwake.ini", dir);
  // ⚠️ SAVED FIRST, THEN LOOKED FOR. The file is written by saveSettings (the panel's edits arrive through
  // setControl/listOp and the host persists them on its own debounce), so checking for it before asking for it
  // can only ever fail -- which is what the first version of this probe did.
  Check(f->saveSettings() == 1, "it writes its settings into its own folder", "");
  {
    FILE *fp = fopen(ini, "rb");
    Check(fp != nullptr, "  and the file is there", ini);
    if (fp)
    {
      static char text[4096];
      const size_t got = fread(text, 1, sizeof(text) - 1, fp);
      fclose(fp);
      text[got] = 0;
      Check(Has(text, "global_awake=") && Has(text, "global_display="),
            "  the 系统全局 row is written as two plain keys", "");
      Check(Has(text, "quick_panel="),
            "  and which list the quick panel may show, as ONE key of its own", "");
      Check(!Has(text, "quick_awake=") && !Has(text, "quick_display="),
            "    not the two per-switch keys it replaced -- a file has one format", "");
      Check(Has(text, "item=") && Has(text, "|"),
            "  one `item=<name>|<awake>|<display>` line per program row", "");
      Check(!Has(text, "\nprocess=") && !Has(text, "\nlistOnly=") && !Has(text, "\nsleep="),
            "  and NOT the first version's keys -- a file has one format", "");
    }
  }

  // ⚠️ THE OLDER FORMAT IS TRANSLATED, and this is not politeness: the feature has already shipped in the
  // field, so a file written by it exists on machines that upgrade. Dropping those keys silently would lose
  // whatever the user set up -- and the two formats do not map onto each other obviously, which is why the
  // translation is tested rather than described.
  //
  // ⚠️ THE SECOND CASE IS THE INTERESTING ONE: `listOnly=0` meant the names did NOTHING. Translating them into
  // switched-on rows would start holding a power request that was not being held a moment before the upgrade,
  // so they are kept with both switches OFF: the user's typing survives and the behaviour does not change.
  WriteLegacy(dir, "# the first version's file\nsleep=1\ndisplay=1\nlistOnly=1\nprocess=chrome.exe\n");
  f->reloadSettings();
  Doc(f, doc, (int)sizeof(doc));
  Check(Has(doc, "\"awake\":0,\"display\":0"), "old file with listOnly=1: the global row stays off (the old switches belonged to the list)", "");
  Check(Has(doc, "\"title\":\"chrome.exe\""), "  and the listed program becomes a row", "");
  Check(Has(doc, "\"awake\":1,\"display\":1"), "  carrying the old sleep/display switches", "");

  WriteLegacy(dir, "sleep=1\ndisplay=0\nlistOnly=0\nprocess=zoom.exe\n");
  f->reloadSettings();
  Doc(f, doc, (int)sizeof(doc));
  Check(Has(doc, "\"awake\":1,\"display\":0"), "old file with listOnly=0: the global row takes the two switches", "");
  Check(Has(doc, "\"title\":\"zoom.exe\""), "  the listed program is kept", "");
  Check(!Has(doc, "\"title\":\"chrome.exe\""), "  and the file's own list replaces the old one", "");

  // ⚠️ AND THE STATE IS PUT BACK, so that what is on disk when this probe exits is exactly what the gate around
  // it checks. A probe that leaves the file in whatever state its last experiment produced makes the gate's
  // assertions depend on the probe's history -- which is how a gate becomes a thing that only passes in order.
  {
    Doc(f, doc, (int)sizeof(doc));
    const int rows = RowCount(doc);
    for (int i = rows - 1; i >= 1; --i)
      f->listOp("rules", "remove", "", i);
    f->listOp("rules", "add", "zoom.exe", 0);
    f->setControl("rules[0].awake", "1");
    f->setControl("rules[0].display", "0");
    // ... including the quick-panel switch, which is ON in the file the gate reads -- so the gate can assert the
    // ONE key that replaced two (`quick_awake` / `quick_display`; see the field in the feature).
    f->setControl("quick_panel", "1");
    Check(f->saveSettings() == 1, "  and a known state is left on disk for the gate to check", "");
  }

  printf("\n7. going away\n");
  f->shutdown();
  ReadLog(dir, log, (int)sizeof(log));
  Check(Has(log, "cleared (ES_CONTINUOUS)"),
        "shutdown clears the request and says so in the log", "");

  printf("\n");
  if (failures)
  {
    printf("FAILED: %d check(s)\n", failures);
    return 1;
  }
  printf("OK: one list, coupled switches per row, the strongest row wins, and the exact requests Windows gets\n");
  return 0;
}
