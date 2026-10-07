// ---------------------------------------------------------------------------
// feature_unifiedui_probe -- A STUB HOST, SO THE FEATURE'S SETTINGS HALF CAN BE CHECKED WITHOUT ITS WATCHER.
//
// WHY A STUB RATHER THAN THE REAL HOST: the real host would have to be started, which means taking the
// machine's only Apex away from the user (see test/lib_procs.sh), and none of the questions below need it.
// What they need is a featureDir() and a logLine(), which is a dozen lines.
//
// ⚠️⚠️ AND THE PROBE SETS APEX_UNIFIEDUI_DRY BEFORE CALLING init(), WHICH IS THE POINT RATHER THAN A DETAIL.
// init() installs a window-event hook and sweeps every window on the machine -- so a probe that loaded this
// DLL without that switch would be RESTYLING THE USER'S DESKTOP as a side effect of a test run. That is
// forbidden outright by this project's gate rules ("测试不许打扰用户"), and the switch is the feature's own,
// documented in its init().
//
// WHAT IT DOES NOT COVER, SAID PLAINLY SO THE GATE'S PASS IS NOT READ AS MORE THAN IT IS: it cannot check what
// the feature DOES to a window. That needs a window belonging to another process, and the feature deliberately
// refuses to touch a window in its own process (see ShouldTouch) -- so the one thing that can verify the
// effect is the measurement instrument, _diag/ui_probe.cpp, which is not a gate (it puts a window on screen).
// The numbers from it are written up in docs/rules/features.md. THIS GATE COVERS THE CONTRACT AND THE
// SETTINGS: the DLL loads, the ABI matches, the exclude list normalises and round-trips through the file, and
// the JSON the panel would draw is well formed.
//
// Build + run: test/check_feature_unifiedui.sh
// ---------------------------------------------------------------------------

#include "abi.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace {

char g_dir[512] = {0};
int g_fail = 0;

const ApexHost *g_host = nullptr;

void Say(const char *what, bool ok)
{
  printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    g_fail = 1;
}

void HostLog(const char *text) { printf("      [feature log] %s\n", text ? text : ""); }

int HostDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  snprintf(out, (size_t)outSize, "%s", g_dir);
  return (int)strlen(out);
}

int HostEnabled(const char *) { return 1; }

// Is `needle` in `hay`? Used for the JSON checks below -- not a parser, and it does not pretend to be one.
bool Has(const char *hay, const char *needle) { return hay && needle && strstr(hay, needle) != nullptr; }

// Brace/bracket balance, which is what actually caught a real bug in this project once: a missing ']' in the
// panel's snapshot document. A string comparison cannot see it.
bool Balanced(const char *s)
{
  int braces = 0, brackets = 0;
  bool inString = false;
  for (const char *p = s; p && *p; ++p)
  {
    if (inString)
    {
      if (*p == '\\' && p[1])
        ++p;
      else if (*p == '"')
        inString = false;
      continue;
    }
    if (*p == '"')
      inString = true;
    else if (*p == '{')
      ++braces;
    else if (*p == '}')
      --braces;
    else if (*p == '[')
      ++brackets;
    else if (*p == ']')
      --brackets;
    if (braces < 0 || brackets < 0)
      return false;
  }
  return braces == 0 && brackets == 0 && !inString;
}

// The file's whole text, or "" when it is not there.
bool ReadFile(const char *path, char *out, int outSize)
{
  out[0] = 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  size_t n = fread(out, 1, (size_t)outSize - 1, f);
  out[n] = 0;
  fclose(f);
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    printf("usage: feature_unifiedui_probe <dll> <dir with trailing separator>\n");
    return 2;
  }
  const char *dll = argv[1];
  snprintf(g_dir, sizeof(g_dir), "%s", argv[2]);

  printf("== loading the real DLL ==\n");
  HMODULE mod = LoadLibraryA(dll);
  if (!mod)
  {
    printf("  FAIL: LoadLibraryA(%s) error %lu\n", dll, (unsigned long)GetLastError());
    return 1;
  }

  ApexFeatureEntryFn entry = (ApexFeatureEntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  Say("the DLL exports ApexFeatureEntry", entry != nullptr);
  if (!entry)
    return 1;

  const ApexFeature *f = entry();
  Say("the entry returns a structure", f != nullptr);
  if (!f)
    return 1;

  // ⚠️ THE TWO CHECKS THE HOST MAKES BEFORE IT READS ANYTHING, MADE HERE IN THE SAME ORDER: a stale DLL must
  // be refused by the host rather than read past its own end, so a gate that skipped this would be testing a
  // DLL the host would never load.
  Say("the ABI version matches this host", f->abiVersion == APEX_ABI_VERSION);
  Say("the structure size matches this host", f->structSize == sizeof(ApexFeature));
  Say("the id is UnifiedUI", f->id && strcmp(f->id, "UnifiedUI") == 0);
  Say("it has a Chinese name and an English name", f->nameZh && f->nameZh[0] && f->nameEn && f->nameEn[0]);
  Say("it has a version", f->version && f->version[0]);

  // ---- init, in dry-run mode -------------------------------------------------------------------------
  printf("\n== init (APEX_UNIFIEDUI_DRY) ==\n");
  SetEnvironmentVariableA("APEX_UNIFIEDUI_DRY", "1");

  ApexHost host = {0};
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(ApexHost);
  host.logLine = HostLog;
  host.featureDir = HostDir;
  host.featureEnabled = HostEnabled;
  g_host = &host;

  if (!f->init)
  {
    printf("  FAIL: no init\n");
    return 1;
  }
  // ⚠️ 0 MEANS READY. The direction was once written the wrong way round in abi.h and it cost an afternoon of
  // debugging (a working feature was unloaded and its live thread took the host down), so it is asserted here
  // rather than assumed.
  const int rc = f->init(&host);
  Say("init() returns 0 (\"I am ready\")", rc == 0);

  // ---- the document the panel draws ----------------------------------------------------------------
  printf("\n== settingsJson ==\n");
  char json[8192] = {0};
  const int jn = f->settingsJson ? f->settingsJson(json, (int)sizeof(json)) : 0;
  Say("it produces a document", jn > 0 && json[0] == '{');
  Say("the document is balanced (braces and brackets)", Balanced(json));
  Say("it describes the exclude list", Has(json, "\"id\":\"exclude\"") && Has(json, "\"type\":\"list\""));
  Say("the list is the FIRST parameter", Has(json, "{\"params\":[{\"id\":\"exclude\""));
  // ⚠️ THE MENU-CORNER SWITCH, and it is not a decoration: it is the ONE piece of the system's own look that
  // an old program's popup menu can be given from outside this process (_diag/dark_probe.cpp measured that a
  // material and an accent both change exactly zero pixels, and a corner changes the picture).
  Say("it describes the menu-corner switch",
      Has(json, "\"id\":\"round_menus\"") && Has(json, "\"type\":\"bool\""));
  // ⚠️ AND NOTHING ABOUT TRANSLUCENCY -- ASSERTED HERE RATHER THAN LEFT IMPLICIT. A translucent menu was built,
  // shipped, and removed after it did nothing on a real program: a real menu window uses UpdateLayeredWindow,
  // where the alpha is baked into the bitmap it submits, so `GetLayeredWindowAttributes` fails on it
  // (measured: layered=1 readable=0) and SetLayeredWindowAttributes is a mutually exclusive mode. The full
  // account is in docs/rules/features.md §3.14.8. A removed control is a promise that was withdrawn, so the
  // page is checked for its ABSENCE too -- otherwise it could come back without anyone noticing.
  Say("it does NOT offer a translucency control", !Has(json, "fade_menus") && !Has(json, "menu_alpha"));
  Say("it carries a one-line summary in both languages",
      Has(json, "\"summaryZh\"") && Has(json, "\"summaryEn\""));

  // ---- the exclude list, which is this feature's own (the user's rule) ------------------------------
  printf("\n== the exclude list ==\n");
  // A PATH WITH CAPITALS AND A DIRECTORY: the feature must store the bare lower-case file name. The rule it
  // uses is common/match.h, which is the one shared implementation (see that header).
  Say("a full path is accepted", f->listOp && f->listOp("exclude", "add", "C:\\Games\\DOOM.EXE", 0) == 1);
  Say("the same name again is refused as a duplicate", f->listOp("exclude", "add", "doom.exe", 0) == 0);

  char json2[8192] = {0};
  f->settingsJson(json2, (int)sizeof(json2));
  Say("it is stored under its bare lower-case name", Has(json2, "\"doom.exe\""));
  Say("the path's directory did not come with it", !Has(json2, "Games"));

  Say("a wildcard entry is accepted", f->listOp("exclude", "add", "game*", 0) == 1);
  Say("a nonsense op is refused", f->listOp("exclude", "shuffle", "x", 0) == 0);
  Say("an unknown list id is refused", f->listOp("somethingelse", "add", "x", 0) == 0);
  // ⚠️ REORDERING IS REFUSED ON PURPOSE: "is this name on the list" does not depend on the order, so
  // move-up/move-down would be a control that does nothing.
  Say("reordering is refused (order is not meaning here)",
      f->listOp("exclude", "move-up", "", 0) == 0 && f->listOp("exclude", "move-down", "", 0) == 0);

  // ---- the menu-corner switch ------------------------------------------------------------------------
  printf("\n== the menu-corner switch ==\n");
  Say("it can be switched off", f->setControl && f->setControl("round_menus", "0") == 1);
  Say("and back on", f->setControl("round_menus", "1") == 1);
  // ⚠️ AND THE REMOVED TRANSLUCENCY CONTROLS ARE REFUSED LIKE ANY OTHER UNKNOWN ID -- which is what they are
  // now. Asserting it keeps the removal honest in the other direction as well: if somebody adds the controls
  // back, this line goes red instead of the page quietly growing an option that does nothing.
  Say("the removed translucency controls are refused",
      f->setControl("fade_menus", "1") == 0 && f->setControl("menu_alpha", "95") == 0);
  Say("an unknown control is still refused", f->setControl("nonsense", "1") == 0);

  // ---- the settings file ---------------------------------------------------------------------------
  printf("\n== the settings file ==\n");
  char path[600] = {0};
  snprintf(path, sizeof(path), "%sunifiedui.ini", g_dir);
  Say("saveSettings() reports success", f->saveSettings && f->saveSettings() == 1);

  char text[8192] = {0};
  Say("the file is written", ReadFile(path, text, (int)sizeof(text)));
  Say("it holds the two entries, one per line",
      Has(text, "exclude=doom.exe") && Has(text, "exclude=game*"));
  Say("it holds exactly two exclude lines", [&] {
    int n = 0;
    for (const char *p = text; (p = strstr(p, "exclude=")) != nullptr; ++p)
      ++n;
    return n == 2;
  }());

  // A round trip: remove the first entry, save, and read it back. The gate checks the file itself as well, so
  // this is about the feature's own answer rather than about the file.
  Say("removing the first entry is accepted", f->listOp("exclude", "remove", "", 0) == 1);
  Say("removing a non-existent index is refused", f->listOp("exclude", "remove", "", 99) == 0);
  f->saveSettings();
  memset(text, 0, sizeof(text));
  ReadFile(path, text, (int)sizeof(text));
  Say("the removal reached the file", !Has(text, "doom.exe") && Has(text, "exclude=game*"));

  // ---- reload, and shutdown ------------------------------------------------------------------------
  printf("\n== reload and shutdown ==\n");
  Say("reloadSettings() reports success", !f->reloadSettings || f->reloadSettings() == 1);

  // ⚠️ NO onWheel AND NO tick: this feature is not in the input path, and a feature that is not there cannot
  // slow it down. Asserted because "it does not hook the wheel" is one of the things this design promises.
  Say("it does not take the wheel", f->onWheel == nullptr);
  Say("it does not ask for a frame tick", f->tick == nullptr);
  Say("it puts nothing in the quick panel", f->quickItems == nullptr);
  Say("it reports itself as enabled", f->flags && (f->flags() & APEX_FEATURE_ENABLED) != 0);

  if (f->shutdown)
    f->shutdown();
  Say("shutdown() returned without crashing", true);

  printf("\n");
  if (g_fail)
  {
    printf("PROBE FAILED\n");
    return 1;
  }
  printf("PROBE OK\n");
  return 0;
}
