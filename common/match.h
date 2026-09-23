#ifndef APEX_COMMON_MATCH_H
#define APEX_COMMON_MATCH_H

// ---------------------------------------------------------------------------
// MATCHING A PROGRAM NAME AGAINST WHAT THE USER TYPED -- one rule, one place.
//
// WHO USES IT: the wheel feature's "exclude" list (through its own config.h, which forwards to here so that
// nothing that already called `Config::PatternMatch` had to change) and KeepAwake's program list. The rule
// was written for the first of those and the second wants exactly the same behaviour, so it moved here
// rather than being copied -- a second copy is a second thing to fix the day the rule is wrong.
//
// THE RULE, and both halves matter:
//
//   * `*` = any run of characters, `?` = exactly one. WITH NO WILDCARD IT IS AN EXACT FULL-NAME MATCH --
//     the old behaviour, unchanged.
//   * ⚠️ BOTH ENDS ARE ANCHORED. The tempting wrong version is a SUBSTRING match, and its failure is silent
//     and enormous: one entry of `e` would quietly match half the machine's programs, and all the user would
//     see is "smoothing stopped in some places". Anchored, `game` still does not match `game.exe` -- but
//     `game*` does, and the user can see that what they typed is not what they meant.
//
// ⚠️ THE CASE FOLDING LIVES INSIDE THE MATCH (it was once an unwritten convention that names arrived
// lower-cased, and a test caught the day that stopped being true). An entry in capitals matches.
//
// NO WINDOWS AND NO ALLOCATION here: it is a header of two pure functions, used on paths where neither is
// available.
// ---------------------------------------------------------------------------

#include <cstring>

namespace apex {
namespace match {

// Lower-cases an ASCII letter and leaves everything else alone. Program names are ASCII in practice; a
// non-ASCII byte compares equal to itself, which is the honest behaviour for a rule that does not know
// what the bytes mean.
inline char FoldAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// True when `name` matches `pat`. See the rule above, including the anchoring.
inline bool Pattern(const char *pat, const char *name)
{
  if (!pat || !name)
    return false;
  while (*pat)
  {
    if (*pat == '*')
    {
      // Collapse runs of '*', then try every split. "**" would otherwise recurse without consuming.
      while (*pat == '*')
        ++pat;
      if (!*pat)
        return true; // trailing '*': everything from here matches
      for (const char *t = name;; ++t)
      {
        if (Pattern(pat, t))
          return true;
        if (!*t)
          return false;
      }
    }
    if (!*name)
      return false;
    if (*pat != '?' && *pat != FoldAscii(*name))
      return false;
    ++pat;
    ++name;
  }
  return *name == 0; // BOTH ended: a pattern never matches a mere prefix of a name
}

// A path, a name, or a name with spaces -> the bare lower-case file name. Writes "" when the input has no
// usable name, so a bad entry is dropped rather than silently matching nothing.
//
// ⚠️ USED FOR BOTH SIDES OF EVERY COMPARISON: what the user typed (`C:\Games\DOOM.exe` becomes `doom.exe`)
// and what a process is called (`DOOM.EXE` becomes `doom.exe`), so the two cannot disagree about case or
// about whether a path was included.
inline void NormaliseName(const char *in, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  if (!in)
    return;
  const char *name = in;
  for (const char *p = in; *p; ++p)
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

} // namespace match
} // namespace apex

#endif // APEX_COMMON_MATCH_H
