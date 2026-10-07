// THE WHEEL RULE FOR PROGRAMS THAT BRING THEIR OWN SMOOTHING -- checked on the real table, not on a restatement.
//
//   g++ -std=c++17 -O2 -Icommon -Iapex -o build/_engine_exclude_probe.exe _diag/engine_exclude_probe.cpp
//
// WHY THIS EXISTS. The user's rule (2026-10-07): "这些有独立引擎的，不管它们有没有打开，都是在排除名单内的，不接管".
// Two ways to get it wrong, and both are silent:
//
//   * a program in the table is NOT excluded -- Apex smooths its wheels on top of its own engine, which is the
//     "two handlers driving one view" that decision.h exists to avoid, and it feels like the program stuttering;
//   * a program NOT in the table IS excluded -- its wheels are never smoothed and nothing says why (the same
//     complaint in the other direction).
//
// So both halves are pinned here, plus the SHAPE of the table: bare lower-case file names (the spelling the host
// already has for the window under the cursor), and distinct kinds (a copy-paste that gave two rows one kind would
// make two engines look like one).
//
// ⚠️ THE PLATFORM LAYER'S HALF IS A STATIC CHECK IN THE GATE (that host_win.cpp actually asks this function) -- what
// can be executed without a real REAPER and a real Lertaro on the machine is executed here.

#include "engines.h"
#include <cstdio>
#include <cstring>

using app::kKnownEngineCount;
using app::kKnownEngines;
using app::ProgramBringsOwnEngine;

static int failures = 0;

static void Check(bool ok, const char *what, const char *detail = "")
{
  printf("  %-62s %s%s%s\n", what, ok ? "ok" : "WRONG", detail[0] ? "  " : "", detail);
  if (!ok)
    ++failures;
}

int main()
{
  printf("The programs that bring their own smoothing (%d):\n", kKnownEngineCount);
  for (int i = 0; i < kKnownEngineCount; ++i)
    printf("  kind=%d  %-10s %s\n", kKnownEngines[i].kind, kKnownEngines[i].name, kKnownEngines[i].exe);
  printf("\n");

  printf("== 1. every one of them is left alone, whatever spelling arrives ==\n");
  for (int i = 0; i < kKnownEngineCount; ++i)
  {
    char upper[64] = {0};
    _snprintf(upper, sizeof(upper), "%s", kKnownEngines[i].exe);
    for (char *c = upper; *c; ++c)
      if (*c >= 'a' && *c <= 'z')
        *c = (char)(*c - 'a' + 'A');
    char what[128] = {0};
    _snprintf(what, sizeof(what), "\"%s\" is excluded", kKnownEngines[i].exe);
    Check(ProgramBringsOwnEngine(kKnownEngines[i].exe), what);
    // The host lower-cases the name it reads (ApexTarget::exe), but the rule must not DEPEND on that: a case
    // difference turning into "Apex took the wheel after all" is exactly the silent failure this pins.
    _snprintf(what, sizeof(what), "  and so is \"%s\"", upper);
    Check(ProgramBringsOwnEngine(upper), what);
  }

  printf("\n== 2. everybody else is NOT -- an over-eager table is the other failure ==\n");
  Check(!ProgramBringsOwnEngine("chrome.exe"), "an ordinary program is not excluded");
  Check(!ProgramBringsOwnEngine("explorer.exe"), "and neither is the shell");
  // ⚠️ THE SERVICE IS NOT THE APP. Lertaro runs a background service with no windows to scroll; excluding it would
  // be a rule about a program nobody scrolls, and it would hide a real mistake in the table (a wildcard `lertaro*`).
  Check(!ProgramBringsOwnEngine("lertaro.service.exe"), "Lertaro's SERVICE is not the program the user scrolls");
  Check(!ProgramBringsOwnEngine("reaper.exe.bak"), "a near-miss name is not matched");
  Check(!ProgramBringsOwnEngine(""), "an empty name is not a program");
  Check(!ProgramBringsOwnEngine(nullptr), "and neither is a null one");

  printf("\n== 3. the table's own shape ==\n");
  bool bare = true, lower = true, suffixed = true;
  for (int i = 0; i < kKnownEngineCount; ++i)
  {
    const char *e = kKnownEngines[i].exe;
    if (strchr(e, '\\') || strchr(e, '/') || strchr(e, ':'))
      bare = false;
    for (const char *c = e; *c; ++c)
      if (*c >= 'A' && *c <= 'Z')
        lower = false;
    if (strlen(e) < 4 || strcmp(e + strlen(e) - 4, ".exe") != 0)
      suffixed = false;
  }
  Check(bare, "every entry is a bare file name, with no path in it");
  Check(lower, "and lower-case, the way the host spells the program under the cursor");
  Check(suffixed, "and ends in .exe");
  bool distinct = true;
  for (int i = 0; i < kKnownEngineCount && distinct; ++i)
    for (int j = i + 1; j < kKnownEngineCount; ++j)
      if (kKnownEngines[i].kind == kKnownEngines[j].kind || strcmp(kKnownEngines[i].exe, kKnownEngines[j].exe) == 0)
        distinct = false;
  Check(distinct, "the kinds and the names are distinct (two rows are two programs)");
  bool named = true;
  for (int i = 0; i < kKnownEngineCount; ++i)
    if (!kKnownEngines[i].name[0])
      named = false;
  Check(named, "and every entry carries the name the note will show");

  printf("\n%s\n", failures ? "FAILED: the own-engine rule does not hold" : "OK: the programs with their own smoothing are excluded, and nothing else is");
  return failures ? 1 : 0;
}
