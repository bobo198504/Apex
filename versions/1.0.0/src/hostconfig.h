#ifndef APEX_HOSTCONFIG_H
#define APEX_HOSTCONFIG_H

// ---------------------------------------------------------------------------
// THE HOST'S SETTINGS -- the things that are true for the whole program rather than for one feature.
//
// WHAT BELONGS HERE: the language, the theme, and which features the user has switched off -- decisions about
// APEX THE PROGRAM. A feature's own numbers (how far a notch travels, how long a glide lasts) belong to that
// feature and live in its own file, in its own folder.
//
// ⚠️ TWO THINGS THAT USED TO BE LISTED HERE ARE NOT, and the distinction is the whole point of this file:
// there is no master switch and no blacklist. Both were decisions about one BEHAVIOUR (smoothing a wheel)
// living in the program's settings, so a future feature with nothing to do with wheels would have inherited
// them. Enabling is per feature (the `off` list below), and a blacklist belongs to whichever feature wants
// one (see ApexFeature::listOp in abi.h).
//
// WHAT DOES NOT BELONG HERE: anything a feature could reasonably want to differ on. A global default
// that a feature then overrides is two places to look for one value, and the second place is the one
// that gets missed. Each feature's file is the only source for its own behaviour.
//
// NO WINDOWS IN THIS FILE. The file access and the settings-file location live in paths_win.cpp beside
// it, so the parsing and the rules here can be exercised without a platform (_diag/apex_hostconfig_probe).
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace apex {

inline double ClampD(double v, double lo, double hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static const int kMaxList = 32; // entries in a list this file keeps (only the disabled features use one now)
static const int kMaxName = 64; // one entry: a feature id here, lower-case, no path

// ---- the language ------------------------------------------------------------------------------
//
// AUTO IS THE DEFAULT AND IT IS A REAL ANSWER, not a placeholder: the panel is bilingual, so the one
// thing Apex can be sure of is that either language is readable. Auto resolves by asking Windows for the
// user's preferred UI language and choosing Chinese when it is Chinese, English otherwise -- so a
// German user reads English rather than getting a half-translated panel.
enum class Lang
{
  kAuto = 0,
  kZh = 1,
  kEn = 2
};

// ---- the theme ---------------------------------------------------------------------------------
//
// AUTO follows the system's app light/dark setting. The two explicit values exist because "follow the
// system" is wrong for the cases people actually hit: a dark desktop where one wants this panel light,
// or a light one used at night. When the theme is overridden, the CAPTION is overridden with it -- a dark
// panel under a light caption is the thing that looked broken in the first place.
enum class Theme
{
  kAuto = 0,
  kLight = 1,
  kDark = 2
};

// WHICH WAY THE THEME ACTUALLY RESOLVED -- a pure function, so it can be checked exhaustively.
//
// It exists because "which theme is in force" is asked in more than one place (the tray's mark and the
// window's icon in the host; the page's `data-theme`, the caption and the window icon in the panel), and
// each of those used to answer it for itself. The tray's answer was `SystemIsLightTheme()` alone, which
// IGNORES a pinned theme: with the system light and the theme pinned to dark, the page and the caption went
// dark while the tray kept the mark meant for a light background. The setting even said it affected the tray
// (`settings_host.cpp`), so the intent was there and the wiring was not.
//
// A pinned value wins over the system; `auto` is the system. Nothing here touches Windows, which is what
// makes the whole table testable.
inline bool ThemeResolvesLight(Theme t, bool systemIsLight)
{
  if (t == Theme::kLight)
    return true;
  if (t == Theme::kDark)
    return false;
  return systemIsLight;
}

struct HostConfig
{
  Lang lang = Lang::kAuto;
  Theme theme = Theme::kAuto;
  // ⚠️ THERE IS NO `enabled` FIELD. There was: a master switch, saved in apex.ini and toggled from the tray,
  // that made the host stop asking any feature about any wheel. It was removed at the user's request --
  // "以后只有插件各自有自己的启用" -- and its absence is not a gap to fill. A feature owns its own switch
  // (the `off` list below, which the panel exposes on each feature's page), and `decision.h` ends with
  // "no feature able to deliver -> pass", so a user who turns their features off still gets an untouched
  // wheel rather than a swallowed one. See apex/decision.h for the invariant that outlived the switch.

  // ⚠️ THERE IS NO BLACKLIST HERE ANY MORE. `skip[]` and SkipHas/SkipAdd/SkipRemove lived in this struct,
  // and it was the wrong home for them: "do not smooth in this program" is a statement about SMOOTHING,
  // which is a feature's business, not Apex's. A feature that has nothing to do with wheels has no reason
  // to inherit a wheel-shaped list -- and the setting existed in two files at once (apex.ini `skip=`, and
  // SmoothWheel.ini's own `skip=`, which the feature parsed and saved and never read).
  //
  // The list is now the FEATURE's own, edited through the generic `list` control (see ApexFeature::listOp
  // in abi.h) and stored in the feature's own file. What did not change is the guarantee a user relies on:
  // a listed program gets its wheel untouched, because a feature that declines leaves the message alone.

  // FEATURES TURNED OFF BY THE USER, by id. The host still loads them (so the panel can list them and
  // the user can turn one back on without a restart), but never asks them about a wheel. THIS IS THE ONLY
  // "off" THERE IS -- see the note above.
  char off[kMaxList][kMaxName] = {{0}};
  int offN = 0;

  // ---- the disabled features ----
  bool FeatureOff(const char *id) const
  {
    if (!id || !*id)
      return false;
    for (int i = 0; i < offN; ++i)
      if (NameEquali(off[i], id))
        return true;
    return false;
  }

  bool FeatureSetOff(const char *id, bool isOff)
  {
    if (!id || !*id)
      return false;
    const int at = FeatureFind(id);
    if (isOff)
    {
      if (at >= 0 || offN >= kMaxList)
        return false;
      NormaliseExe(id, off[offN], kMaxName); // same lower-casing, different meaning
      if (!off[offN][0])
        return false;
      ++offN;
      return true;
    }
    if (at < 0)
      return false;
    RemoveAt(off, offN, at);
    return true;
  }

  // A feature id (or a path to one) normalised the same way the exe names used to be: lower-case, and only
  // the last path component, so an id written as a path still matches. The name is kept because it describes
  // what it does -- it is no longer exe-specific, and the blacklist entries it was written for are gone.
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

private:
  static bool NameEquali(const char *a, const char *b)
  {
    if (!a || !b)
      return false;
    while (*a && *b)
    {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb)
        return false;
      ++a;
      ++b;
    }
    return !*a && !*b;
  }

  int FeatureFind(const char *id) const
  {
    for (int i = 0; i < offN; ++i)
      if (NameEquali(off[i], id))
        return i;
    return -1;
  }

  static void RemoveAt(char (*list)[kMaxName], int &n, int i)
  {
    if (i < 0 || i >= n)
      return;
    for (int j = i; j + 1 < n; ++j)
      memcpy(list[j], list[j + 1], kMaxName);
    list[n - 1][0] = 0;
    --n;
  }
};

// ---------------------------------------------------------------------------
// STRICT VALUE PARSING. Shared with the feature settings (the same reasoning -- see app/config.h for
// the bug this prevents): atof("abc") is 0.0, and 0.0 put through a clamp is the parameter's MINIMUM, so
// reading a typo as a number silently sends a setting to the end of its range. Digits are required and
// the whole token must be consumed.
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
  if (*p == 'e' || *p == 'E')
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
    return false;
  *out = atof(s);
  return true;
}

inline bool ParseBool(const char *s, bool *out)
{
  if (!s || !out)
    return false;
  while (*s == ' ' || *s == '\t')
    ++s;
  static const char *yes[] = {"1", "on", "true", "yes"};
  static const char *no[] = {"0", "off", "false", "no"};
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

inline bool ParseLang(const char *s, Lang *out)
{
  if (!s || !out)
    return false;
  if (_stricmp(s, "auto") == 0) { *out = Lang::kAuto; return true; }
  if (_stricmp(s, "zh") == 0 || _stricmp(s, "cn") == 0 || _stricmp(s, "chinese") == 0)
  {
    *out = Lang::kZh;
    return true;
  }
  if (_stricmp(s, "en") == 0 || _stricmp(s, "english") == 0)
  {
    *out = Lang::kEn;
    return true;
  }
  return false;
}

inline bool ParseTheme(const char *s, Theme *out)
{
  if (!s || !out)
    return false;
  if (_stricmp(s, "auto") == 0) { *out = Theme::kAuto; return true; }
  if (_stricmp(s, "light") == 0) { *out = Theme::kLight; return true; }
  if (_stricmp(s, "dark") == 0) { *out = Theme::kDark; return true; }
  return false;
}

inline const char *LangName(Lang l)
{
  switch (l)
  {
  case Lang::kZh: return "zh";
  case Lang::kEn: return "en";
  default: return "auto";
  }
}

inline const char *ThemeName(Theme t)
{
  switch (t)
  {
  case Theme::kLight: return "light";
  case Theme::kDark: return "dark";
  default: return "auto";
  }
}

// `key=value`, `#` or `;` starts a comment, an unknown key is ignored. Same shape as the feature files,
// because the two are edited by the same person in the same folder.
inline bool ParseHostConfig(const char *text, HostConfig &c)
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
    p = *eol ? eol + 1 : eol;

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

    // EVERY VALUE KEEPS WHAT IT HAD WHEN IT CANNOT BE UNDERSTOOD -- see the note on ParseNumber. A key
    // that is recognised but whose value is gibberish leaves the setting alone rather than zeroing it.
    if (strcmp(key, "lang") == 0)
    {
      Lang l;
      if (ParseLang(val, &l))
        c.lang = l;
    }
    else if (strcmp(key, "theme") == 0)
    {
      Theme t;
      if (ParseTheme(val, &t))
        c.theme = t;
    }
    else if (strcmp(key, "off") == 0)
      c.FeatureSetOff(val, true);
  }
  return true;
}

inline void FormatHostConfig(const HostConfig &c, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  int off = 0;
  off += _snprintf(out + off, outSize - off,
                   "# Apex -- host settings\n"
                   "#\n"
                   "# lang   : auto (follow the system) | zh | en\n"
                   "# theme  : auto (follow the system) | light | dark\n"
                   "# off    : a feature id to leave loaded but inactive; repeat the line for more\n"
                   "lang=%s\n"
                   "theme=%s\n",
                   LangName(c.lang), ThemeName(c.theme));
  for (int i = 0; i < c.offN && off < outSize - (kMaxName + 8); ++i)
    off += _snprintf(out + off, outSize - off, "off=%s\n", c.off[i]);
}

} // namespace apex

#endif // APEX_HOSTCONFIG_H
