#ifndef APEX_FEATURE_CONFIG_H
#define APEX_FEATURE_CONFIG_H

// ---------------------------------------------------------------------------
// A FEATURE'S SETTINGS: pure data plus pure text handling.
//
// NO WINDOWS, NO REAPER in here -- the parse and the format are pure, so they can be exercised without a
// platform (_diag/app_config_probe.cpp). Same split the plugin uses for its own testable logic.
//
// WHO READS AND WRITES THE FILE, now that this is Apex: THE FEATURE ITSELF, through the host's featureDir()
// (see apex/abi.h). There used to be a config_win.cpp beside this file holding the path and the file calls;
// it was the STANDALONE app's, and it was deleted at the split because nothing in Apex called it -- the host
// keeps its own settings through apex/paths_win.cpp and a feature keeps its own beside its own dll.
//
// THE FOUR PARAMETERS KEEP THE PLUGIN'S DEFAULTS AND RANGES, so the two products feel the same out of the
// box and a value saved by one is meaningful to the other. The ranges are stated HERE and nowhere
// else: every value that arrives (from the file or from the panel) goes through Clamp(), so a bad
// number in a settings file cannot reach the model.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace app {

inline double ClampD(double v, double lo, double hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static const int kMaxSkip = 16;  // exe names we never touch
static const int kMaxName = 64;  // an exe name, lower-case, without a path

struct Config
{
  // ---- the four model parameters ----
  //
  // THESE ARE THE APP'S TUNING SURFACE, and deliberately the ONLY one: no private scaling layer sits
  // between the model and what is delivered. A gain applied to the output was tried and removed -- it
  // scales the whole line, so the ceiling moves with it (a fast roll then delivers 2 x cap x topSpeed =
  // 3 x the wheel's own notch, past anything the model was tuned for), and it is a second place where
  // "how far a notch goes" is decided. `rampUp` below is the model's OWN knob for reaching full speed
  // sooner, and it cannot push past the ceiling.
  //
  // The values differ from the plugin's defaults on purpose -- see rampUp -- because the two apps are
  // different hosts for the same model, and each needs its own defaults. The model itself is untouched;
  // src/ is shared byte for byte.
  double glideMs = 200.0;   // -> model::Params::windowMs; the shortest window (a slow notch)
  double slowStep = 5.0;    // -> Travel(startDeltas)
  // ⚠️ 1000, THE PLUGIN'S OWN VALUE. This has been both ways and the history is worth keeping, because the
  // reasons were opposite and only one of them was right:
  //
  //   * it was halved to 500 on a feel argument -- the budget settles at roughly rate x 300 ms, so at 1000 a
  //     normal roll sits far down the ramp and moves a fraction of a notch;
  //   * the user then said the SLIDER'S DEFAULT should be the original one ("拉杆的默认值不是原来的"),
  //     and this file went back to 500 while claiming that was their specific request;
  //   * and now they have said it plainly: "加速基准，默认应该是1000". The plugin ships 1000 and a default
  //     that disagrees with the sibling product is the worse surprise -- the model is shared byte for byte,
  //     so the two should also start from the same numbers.
  //
  // The feel argument above was mine, not theirs, and it is kept only as an explanation of what the value
  // DOES. Nothing stops a user from dialling it back: it is a slider, and its range (60..2000) is unchanged.
  double rampUp = 1000.0;   // -> Travel(budgetDeltas)
  double topSpeed = 1.5;    // -> Travel(speedMul)
  // ⚠️ NO `enabled` HERE ANY MORE. The standalone app had one and this project inherited it, but Apex's
  // enabling is generic: the host keeps the "off" list and the panel draws a switch on every feature's page
  // (see ApexFeature::flags and hostconfig.h). That left this field read by nothing while `enabled=1` was
  // still WRITTEN into SmoothWheel.ini -- a line in a settings file that says something no code consults is
  // worse than no line: the next reader (or the next version of the feature) has to work out whether it
  // means something. If a feature ever needs its own switch it describes one as a `bool` control, which
  // travels through applySetting like any other parameter.

  // ⚠️ THE TAIL IS NOT A SETTING HERE, AND THAT IS THE POINT -- it is a MODEL now, and it lives in
  // common/release.h. Read that header before touching anything about how long a window lasts.
  //
  // What happened, because the file has been through both versions: there WAS a `tail` field here
  // (windowMs = glide * (1 + tail * u)), taken from the intermediate model (the 2.0 / C# line, which calls the
  // same idea Release). The user had it removed -- "拖尾这个参数哪来的，原模型没有这个" -- because the four
  // parameters above are the plugin 1.7.x model and 1.7.x has no such knob, so it was a fifth thing to reason
  // about that the sibling product does not have.
  //
  // It came back, differently, because the problem it solved is real: ONE number was being asked to serve both
  // a lone scroll (wants a short window, or it reads as lag) and a roll (wants a long one -- the user's own
  // words: "让滚动的时候，收尾拉长的"). The new version is NOT a slider and NOT scaled by speed: it is a fixed
  // 200 ms added to the window of a message that is part of a roll. Being fixed and being a model rather than a
  // parameter is what keeps it from becoming the thing that was rejected -- there is nothing to tune, nothing to
  // disagree with the plugin about, and no speed readout.
  //
  // AN OLD SETTINGS FILE MAY STILL CONTAIN tail=... AND STILL LOADS: unknown keys are ignored by design (see
  // ParseConfigText), which is exactly what makes removing a parameter safe. Nothing needs to delete the line.

  // ---- exe names we never touch, lower-case, no path ----
  char skip[kMaxSkip][kMaxName] = {{0}};
  int skipN = 0;

  // ---- ranges: THE single source (see the note above) ----
  static double GlideLo() { return 100.0; }
  static double GlideHi() { return 300.0; }
  static double SlowLo() { return 1.0; }
  static double SlowHi() { return 10.0; }
  static double RampLo() { return 60.0; }
  static double RampHi() { return 2000.0; }
  static double TopLo() { return 1.0; }
  static double TopHi() { return 2.0; }

  // THE WINDOW'S ENVELOPE, from the model's own header: 100..400 ms. Applied in WindowMsFor below.
  //
  // ⚠️ IT IS A DIFFERENT NUMBER FROM THE GLIDE SLIDER'S 100..300, and conflating the two cost a revision here.
  // The slider describes the BASE window (the plugin's own constant: "400 was uncomfortable, so the top is
  // 300"); the envelope describes what the MODEL accepts. They are two different questions, and the release
  // model's addition (release.h) sits ON TOP of the clamped base -- see `WindowMsFor` below for why the
  // envelope is the base's and not the result's.
  static double WindowLoMs() { return 100.0; }
  static double WindowHiMs() { return 400.0; }

  void Clamp()
  {
    glideMs = ClampD(glideMs, GlideLo(), GlideHi());
    slowStep = ClampD(slowStep, SlowLo(), SlowHi());
    rampUp = ClampD(rampUp, RampLo(), RampHi());
    topSpeed = ClampD(topSpeed, TopLo(), TopHi());
  }

  // THE BASE WINDOW: the Glide value, clamped to the model's envelope.
  //
  // ⚠️ THIS IS NOT THE WINDOW THAT GETS USED, AND THE NAME SAYS SO. It is the window a message gets when it
  // is NOT part of a roll; a message inside one gets this PLUS the release model's fixed addition (see
  // release.h, which is where that decision is made and the only place it is made). Calling this and handing
  // the result to the model directly would silently drop the tail.
  //
  // ⚠️ IT USED TO TAKE A SPEED ARGUMENT (`u`, the budget over the ramp) so the tail could stretch the window
  // at speed. The window does not depend on speed any more -- the tail it has now is fixed -- and the argument
  // went with it: an argument that is accepted and ignored is how a caller comes to believe it still does
  // something.
  //
  // WHY THE ENVELOPE APPLIES TO THE BASE AND NOT TO THE RESULT: the envelope is the MODEL's statement about
  // how long one amount may be spread, and the slider the user moves is the base. The release addition is the
  // app's own decision about a roll and the model has no opinion about it -- at Glide 300 a roll's window is
  // 500 ms, which is still ~63 windows in flight against the model's 2048 cap. Clamping the RESULT instead
  // would mean the addition silently disappeared at the top of the Glide range, which is the one place a user
  // would notice the tail going missing.
  double WindowMsFor() const { return ClampD(glideMs, WindowLoMs(), WindowHiMs()); }

  // ---- the skip list ----
  //
  // ENTRIES ARE PATTERNS, NOT EXACT NAMES -- the user asked for it ("黑名单支持进程名模糊匹配") and the
  // reason is practical: a program's window belongs to the launcher, not to the program (measured in this
  // very project: the settings page's window is owned by msedgewebview2.exe), and a family of executables
  // that share a prefix is a nuisance to enumerate. `*` matches any run of characters, `?` exactly one, and
  // an entry with neither still has to match the WHOLE name -- so nothing that used to work changes meaning.
  //
  // ⚠️ AND THE MATCH IS ANCHORED AT BOTH ENDS, WHICH IS THE PART WORTH ARGUING. A SUBSTRING match -- the
  // obvious way to make something feel "fuzzy" -- would let an entry of `e` or `a` silently claim half the
  // programs on the machine, and the failure is invisible: the wheel just stops smoothing somewhere the user
  // never listed. Anchoring means `game` still misses `game.exe`, but `game*` matches it, and the user can
  // always see the difference between what they typed and what they meant. (An entry that matches NOTHING is
  // the safe direction and is not an error; the panel lists entries verbatim.)
  //
  // IT IS A SMALL GLOB, NOT A REGEX, deliberately: no allocation (this is reachable from the wheel path),
  // no syntax to learn, and `*`/`?` cover "a family of names" and "one character varies" which is all a
  // process-name list needs.
  //
  // Matching is recursive, which is fine at this size and is bounded by the lengths involved: an entry is at
  // most kMaxName and a name is a bare file name. Worst case is a pattern of all `*`s against a long name --
  // 64 x name-length steps, on a path that already does OpenProcess.
  // ⚠️ IT LOWER-CASES THE NAME AS IT GOES, AND THAT IS NOT REDUNDANT. `skip[]` is stored lower-cased, but this
  // is called with a name that came from `BareExeName` -- which lower-cases too, TODAY. The previous
  // exact-match loop folded case on BOTH sides at the point of comparison, so it was correct whatever the
  // caller did; dropping that here made the matcher depend on an upstream invariant that nothing states.
  // (A test caught it: a query in capitals stopped matching.) Folding here costs a comparison per character
  // and removes the dependency.
  static char FoldAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

  static bool PatternMatch(const char *pat, const char *name)
  {
    while (*pat)
    {
      if (*pat == '*')
      {
        // Collapse runs of '*', then try every split. "**" would otherwise recurse without consuming.
        while (*pat == '*')
          ++pat;
        if (!*pat)
          return true; // trailing '*': everything from here matches
        for (const char *t = name; ; ++t)
        {
          if (PatternMatch(pat, t))
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

  // The pattern a user typed, lower-cased and trimmed to its file name (or to a pattern). Normalisation is
  // the same one the exact match always used, so a pasted path still becomes a bare name.
  static void NormalisePattern(const char *in, char *out, int outSize)
  {
    NormaliseExe(in, out, outSize);
  }

  bool SkipHas(const char *exe) const
  {
    if (!exe || !*exe)
      return false;
    for (int i = 0; i < skipN; ++i)
      if (PatternMatch(skip[i], exe))
        return true;
    return false;
  }

  bool SkipAdd(const char *pattern)
  {
    if (!pattern || !*pattern || skipN >= kMaxSkip)
      return false;
    char norm[kMaxName] = {0};
    NormalisePattern(pattern, norm, kMaxName);
    if (!norm[0])
      return false;
    // DUPLICATES ARE REJECTED BY THE PATTERN, not by a literal comparison: "add an entry that is already
    // covered" is the mistake a user makes, and the old check (SkipHas) only caught it when the new entry
    // matched an existing one as a NAME. Comparing the normalised strings catches the other direction too --
    // typing the same entry twice -- without claiming that `game*` and `game.exe` are the same thing, which
    // they are not: the first also covers game-launcher.exe.
    for (int i = 0; i < skipN; ++i)
      if (strcmp(skip[i], norm) == 0)
        return false;
    memcpy(skip[skipN], norm, kMaxName);
    ++skipN;
    return true;
  }

  void SkipRemove(int i)
  {
    if (i < 0 || i >= skipN)
      return;
    for (int j = i; j + 1 < skipN; ++j)
      memcpy(skip[j], skip[j + 1], kMaxName);
    skip[skipN - 1][0] = 0;
    --skipN;
  }

  // A path or a name or a name with spaces -> the bare lower-case file name. Returns "" when the input
  // has no usable name, so a bad entry is dropped rather than silently matching nothing.
  static void NormaliseExe(const char *in, char *out, int outSize)
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
};

// ---------------------------------------------------------------------------
// The settings file: one `key=value` per line, `#` starts a comment. A hand-editable text file, so a
// user can fix it without the panel and a bug report can paste it.
//
// An unknown key or an unparseable number is IGNORED, leaving that setting at its default -- so a
// half-written or hand-mangled file can never produce a nonsense configuration, and adding a key in a
// later version does not break an older file.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// STRICT VALUE PARSING, because a hand-edited file will contain typos.
//
// atof("abc") is 0.0, and 0.0 put through Clamp() is the parameter's MINIMUM -- so a single mistyped
// character would silently move a setting to the far end of its range rather than being ignored, which
// is the opposite of what a text file is for. Digits are therefore required and the whole token must be
// consumed, so "150ms" is rejected rather than read as 150.
// ---------------------------------------------------------------------------
inline bool ParseNumber(const char *s, double *out)
{
  if (!s || !out)
    return false;
  while (*s == ' ' || *s == '\t')
    ++s;
  const char *p = s;
  bool digits = false;
  if (*p == '+' || *p == '-')
    ++p;
  while (*p >= '0' && *p <= '9')
  {
    digits = true;
    ++p;
  }
  if (*p == '.')
  {
    ++p;
    while (*p >= '0' && *p <= '9')
    {
      digits = true;
      ++p;
    }
  }
  if (!digits)
    return false;
  if (*p == 'e' || *p == 'E') // an exponent, but only a well-formed one
  {
    const char *e = p + 1;
    if (*e == '+' || *e == '-')
      ++e;
    bool expDigits = false;
    while (*e >= '0' && *e <= '9')
    {
      expDigits = true;
      ++e;
    }
    if (!expDigits)
      return false;
    p = e;
  }
  while (*p == ' ' || *p == '\t' || *p == '\r')
    ++p;
  if (*p)
    return false; // trailing junk
  *out = atof(s);
  return true;
}

// A switch a person might write as 1/0, on/off, true/false, yes/no. Anything else keeps the current
// value rather than guessing, so a typo cannot flip a setting.
inline bool ParseBool(const char *s, bool *out)
{
  if (!s || !out)
    return false;
  while (*s == ' ' || *s == '\t')
    ++s;
  const char *yes[] = {"1", "on", "true", "yes"};
  const char *no[] = {"0", "off", "false", "no"};
  for (int i = 0; i < 4; ++i)
  {
    if (_stricmp(s, yes[i]) == 0)
    {
      *out = true;
      return true;
    }
    if (_stricmp(s, no[i]) == 0)
    {
      *out = false;
      return true;
    }
  }
  return false;
}

inline bool ParseConfigText(const char *text, Config &c)
{
  if (!text)
    return false;
  const char *p = text;
  while (*p)
  {
    const char *eol = p;
    while (*eol && *eol != '\n')
      ++eol;
    char line[512];
    int n = (int)(eol - p);
    if (n > (int)sizeof(line) - 1)
      n = (int)sizeof(line) - 1;
    memcpy(line, p, (size_t)n);
    line[n] = 0;
    if (!*eol)
      p = eol;
    else
      p = eol + 1;

    char *s = line;
    while (*s == ' ' || *s == '\t' || *s == '\r')
      ++s;
    if (!*s || *s == '#' || *s == ';')
      continue;
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *key = s;
    char *val = eq + 1;
    char *ke = key + strlen(key);
    while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r'))
      *--ke = 0;
    while (*val == ' ' || *val == '\t' || *val == '\r')
      ++val;
    char *ve = val + strlen(val);
    while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r'))
      *--ve = 0;

    // A NUMBER KEY: accepted only when it parses cleanly (see ParseNumber); otherwise the setting
    // keeps whatever it had, which is the documented behaviour for a mangled file.
    double *num = nullptr;
    if (strcmp(key, "glide") == 0)
      num = &c.glideMs;
    else if (strcmp(key, "slow") == 0)
      num = &c.slowStep;
    else if (strcmp(key, "ramp") == 0)
      num = &c.rampUp;
    else if (strcmp(key, "top") == 0)
      num = &c.topSpeed;
    // ⚠️ `tail=` IS NOT PARSED ANY MORE -- the parameter is gone (see the note by the fields). An old file
    // still carries the line and still loads: it lands in the "unknown key" case below, which is ignored by
    // design. Do not add a branch here "for compatibility": there is nothing for it to set.
    if (num)
    {
      double v = 0.0;
      if (ParseNumber(val, &v))
        *num = v;
    }
    // ⚠️ `enabled=` IS DELIBERATELY NOT PARSED, and this is where its removal is most visible: an OLD
    // settings file still contains the line (the app wrote it for years), and quietly ignoring it is what
    // keeps that file loadable. The alternative -- treating it as an unknown key -- is the same thing here,
    // because unknown keys are ignored by design; the note exists so that nobody "restores" the field
    // thinking the line was overlooked.
    else if (strcmp(key, "skip") == 0)
      c.SkipAdd(val);
  }
  c.Clamp();
  return true;
}

inline void FormatConfigText(const Config &c, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  int off = 0;
  off += _snprintf(out + off, outSize - off,
                   "# Smooth Wheel Scroll -- settings, kept beside the feature's own dll\n"
                   "#\n"
                   "# glide / slow / ramp / top are the model's four parameters, with the same meaning and\n"
                   "# ranges as the plugin's panel. This app ships a different DEFAULT for ramp (500 rather\n"
                   "# than 1000, so a normal roll is not a fraction of a notch); the model itself is the\n"
                   "# plugin's, unmodified.\n"
                   "# skip is a program whose wheel is passed through untouched; repeat the line for more.\n"
                   "glide=%.0f\nslow=%.1f\nramp=%.0f\ntop=%.2f\n",
                   c.glideMs, c.slowStep, c.rampUp, c.topSpeed);
  for (int i = 0; i < c.skipN && off < outSize - (kMaxName + 8); ++i)
    off += _snprintf(out + off, outSize - off, "skip=%s\n", c.skip[i]);
}

// ---------------------------------------------------------------------------
// THE PLATFORM SEAM -- REMOVED AT THE SPLIT.
//
// Three functions used to be declared here and implemented in config_win.cpp:
//
//     bool ConfigFilePath(char *out, int outSize);
//     bool LoadConfig(Config &c);
//     bool SaveConfig(const Config &c);
//
// They were how the STANDALONE app found and read its one settings file. Apex does not work that way: the
// feature asks the host for its own folder (ApexHost::featureDir, apex/abi.h) and opens its file there, so
// nothing calls them any more and the file they lived in was deleted rather than left behind as something
// that looks live. The portability requirement they protected is checked against the code that DOES decide
// it -- see test/check_apex_paths.sh and _diag/apex_paths_probe.cpp.
// ---------------------------------------------------------------------------

} // namespace app

#endif // APEX_FEATURE_CONFIG_H
