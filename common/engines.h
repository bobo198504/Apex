#ifndef APEX_ENGINES_H
#define APEX_ENGINES_H

// ---------------------------------------------------------------------------
// THE PROGRAMS THAT BRING THEIR OWN WHEEL SMOOTHING -- ONE TABLE, TWO USES.
//
// USE 1, THE WHEEL RULE: a program in this table is ALWAYS LEFT ALONE -- whether or not its own engine is switched
// on. The user's rule (2026-10-07): "这些有独立引擎的，不管它们有没有打开，都是在排除名单内的，不接管".
//
// USE 2, THE SETTINGS NOTE: "which engines are running", answered by the host and written into a sentence by the
// feature (see ApexHost::activeEngines in abi.h).
//
// ⚠️ ONE TABLE, BECAUSE THESE ARE ONE FACT. "This program has its own smoothing" is why the note mentions it and
// why Apex keeps its hands off it; two lists would drift, and the drift is exactly the confusion the note exists to
// remove -- a sentence naming a program whose wheels are being smoothed anyway.
//
// ⚠️ AND IT IS WHAT MAKES ADDING A PROGRAM NEARLY ONE PLACE. A new row here is excluded by the wheel rule at once
// and shows up in the sentence at once; the one thing left to write is its PROBE in host_win.cpp, which cannot be
// avoided -- finding a program's marker is OS work, not data -- plus nothing at all in any feature.
//
// ⚠️ `exe` IS THE BARE, LOWER-CASE FILE NAME, the same spelling a feature's exclude list uses (see
// common/match.h): a program is matched the way the user would type it. It is the name the host already has for the
// window under the cursor (ApexTarget::exe), so the rule costs one comparison on a cold path.
//
// NOTHING HERE TOUCHES THE OS: a table and one string comparison, so the rule can be compiled and tested outside
// the program itself (see _diag/engine_exclude_probe.cpp, and the gate that runs it).
// ---------------------------------------------------------------------------

#include <string.h>

#include "abi.h"

namespace app {

struct KnownEngine
{
  int kind;         // APEX_ENGINE_*
  const char *name; // the product's own spelling -- the same in every language (see ApexEngine in abi.h)
  const char *exe;  // bare lower-case file name
};

static const KnownEngine kKnownEngines[] = {
    {APEX_ENGINE_REAPER, "REAPER", "reaper.exe"},
    // ⚠️ THE APP, NOT THE SERVICE: Lertaro.Service.exe is a background service and has no windows to scroll. The
    // App is the WPF program the user actually scrolls in -- and the same process whose start time orders it.
    {APEX_ENGINE_LERTARO, "Lertaro", "lertaro.app.exe"},
};
static const int kKnownEngineCount = (int)(sizeof(kKnownEngines) / sizeof(kKnownEngines[0]));

// Does this program bring its own smoothing engine? (If so it is always left alone -- see the header note.)
inline bool ProgramBringsOwnEngine(const char *exe)
{
  if (!exe || !exe[0])
    return false;
  for (int i = 0; i < kKnownEngineCount; ++i)
    if (_stricmp(exe, kKnownEngines[i].exe) == 0)
      return true;
  return false;
}

} // namespace app

#endif // APEX_ENGINES_H
