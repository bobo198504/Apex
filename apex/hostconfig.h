#ifndef APEX_HOSTCONFIG_H
#define APEX_HOSTCONFIG_H

// ⚠️ A FEATURE MAY NOT INCLUDE THIS FILE -- the guard below is what makes that a CONTRACT rather than a
// convention. A feature is compiled with the same -I paths as the host (see apex/build.sh) and exports a C
// ABI, so nothing else stops it reaching in here and calling host internals the ABI never promised to keep
// stable. Everything a feature is entitled to is in abi.h; this file is the HOST's own settings (language, theme, the off list).
#ifndef APEX_BUILDING_HOST
#error "hostconfig.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif

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
  // ⚠️ START WITH WINDOWS: OFF UNLESS THE USER ASKS FOR IT ("通用设置增加选项'开机启动'，默认不启用").
  //
  // ⚠️⚠️ AND IT IS THE ONE SETTING THAT PUTS SOMETHING OUTSIDE THIS FOLDER -- the whole program is portable
  // otherwise (no registry, no %APPDATA%), which is a promise this file has kept since it was written. Windows
  // has no way to start a program at logon from inside that program's own directory: it is the Run key under
  // HKCU, or a shortcut in the Startup folder, and both are outside. The user asked for the feature, so it is
  // implemented -- with the least invasive mechanism there is (per-user, no administrator, one value, deleted
  // the moment the switch goes off) -- and the promise is narrowed rather than broken: NOTHING ELSE in Apex
  // writes outside its folder, and this one writes exactly one value and only while it is switched on.
  bool autostart = false;

  // ---- WHAT THE QUICK PANEL SHOWS --------------------------------------------------------------
  //
  // The flyout above the tray icon (quickpanel_win.cpp) has exactly two halves, and the user asked for a
  // switch for each: "快速面板开关在设置'通用'里，有两个开关，一个以紧凑视图排列的所有插件开关，一个是插件
  // 自主开关模板".
  //
  //   quickCompact -- the grid of EVERY feature's on/off switch. This list is the host's own (`off`, below),
  //                   so this half works with no feature implementing anything at all.
  //   quickOwn     -- the controls the FEATURES asked for, each in a block of its own (`quickItems` in abi.h).
  //
  // ⚠️⚠️ BOTH DEFAULT TO OFF, AND THAT REVERSED AN EARLIER DECISION ON THE USER'S INSTRUCTION (2026-10-07):
  // "所有插件的开关默认值改成关，还有快速面板默认值也是关，就是用户全新用上时，什么功能也不开，让用户按需打开。已经有
  // 配置过的用户不影响。" So a fresh install shows nothing in the flyout until the user maps something, and the
  // panel says in words where the switches are rather than showing an empty box (see RowKind::kNote in
  // quickpanel.h) -- "nothing happened" and "there is nothing to show" must not look the same.
  //
  // ⚠️ AN EXISTING USER IS UNAFFECTED, AND NOT BY LUCK: FormatHostConfig ALWAYS writes both keys, so any apex.ini
  // that exists carries the values that user chose (or the defaults of the version that wrote it). A default only
  // ever reaches a machine that has no file yet -- the same rule the feature `off` list follows (see main.cpp).
  bool quickCompact = false;
  bool quickOwn = false;

  // ---- THE ORDER THE FEATURES' OWN CONTROLS APPEAR IN, IN THE FLYOUT -----------------------------
  //
  // The user's request: "设置通用里，把快速面板功能单独给一个小面板，在两个总开关下面实时给出当前有映射在快速面板
  // 的功能，并能实现上下排序". So this is the order the BLOCKS come out in, and it is a decision about the whole
  // program (which feature's controls are worth reaching first) rather than about any feature -- which is why it
  // lives here, in apex.ini, and not in a feature's own file.
  //
  // ⚠️ IT IS AN ORDER OF PANES, NOT OF PLUGINS, AND THE USER SAID SO: "快速面板的局部功能分组是按单个开关算的，不是
  // 按插件算，所以这边排序要注意". A KEY IS `<feature id>|<group name>` -- the group being the name the feature gives
  // the controls that belong together, which is what the flyout prints as that pane's heading -- or
  // `<feature id>|<item id>` for a control that names no group (it floats in a pane of its own, see
  // `QuickBlockKey` in host.h). One pane, one key, one row in the General page's list.
  //
  // ⚠️ A KEY IS AN OPAQUE STRING HERE, KEPT EXACTLY AS THE HOST BUILT IT: it can carry a feature's own words
  // (「亮度」), so nothing may case-fold it or strip a path out of it on the way in or out.
  //
  // ⚠️ AND NOTHING HERE IS A GUARANTEE ABOUT WHAT IS SHOWN: `quickOrder` says in what order, `quickItems` (per
  // feature) says whether, `off` (below) can hide a whole feature's panes, and `quickOwn` says whether that half
  // of the flyout exists at all.
  char quickOrder[kMaxList][kMaxName * 2] = {{0}};
  int quickOrderN = 0;

  // ONE KEY FROM THE FILE (`quickorder=<key>`, repeated -- see ParseHostConfig). A repeated key is ignored: the
  // first mention is the order, and a second one has nothing to say.
  //
  // ⚠️ THE KEY IS STORED VERBATIM. It carries a feature's own words for a pane (「亮度」), so case-folding it or
  // stripping a path out of it -- which is exactly what `NormaliseExe` does, and what an earlier version of this
  // did by mistake -- would quietly stop the key from matching the pane it names.
  void QuickOrderAppend(const char *key)
  {
    if (!key || !*key || quickOrderN >= kMaxList)
      return;
    for (int i = 0; i < quickOrderN; ++i)
      if (_stricmp(quickOrder[i], key) == 0)
        return;
    _snprintf(quickOrder[quickOrderN], (int)sizeof(quickOrder[quickOrderN]), "%s", key);
    if (quickOrder[quickOrderN][0])
      ++quickOrderN;
  }

  // REPLACE THE SAVED ORDER WITH the keys in `text` (comma- or newline-separated), KEEPING ANY KEY IT DOES NOT
  // MENTION AFTER THE ONES IT DOES. That last half is what makes a one-row move non-destructive: the page sends the
  // keys it is SHOWING (the panes that exist right now), and a pane that is temporarily absent -- its feature
  // switched off, or its mapping switched off -- must not lose the place it had (the user's rule is that switching
  // a feature back on restores its rows "按各局部功能快速面板功能开关状态还原").
  void QuickOrderSetFromKeys(const char *text)
  {
    char kept[kMaxList][kMaxName * 2];
    int keptN = 0;
    if (text)
    {
      const char *p = text;
      while (*p && keptN < kMaxList)
      {
        const char *end = p;
        // ⚠️ THE SEPARATORS ARE THE COMMA AND THE NEWLINE, AND BOTH ARE SAFE BECAUSE OF WHERE THE KEY IS BUILT:
        // `QuickBlockKey` (apex/quickpanel.h) replaces either character with a space, so a key can contain neither.
        // It has to: this value travels as ONE LINE of the request protocol (`key=value`, see ParseFields in
        // settings_host.cpp), and a newline in it truncates the list to its first key -- which is exactly the bug
        // the user reported as "快速面板分组不能调顺序". The comma is what the PANEL sends; the newline is what a
        // hand-edited file naturally has (one `quickorder=` line per key).
        while (*end && *end != ',' && *end != '\n' && *end != '\r')
          ++end;
        char one[kMaxName * 2] = {0};
        int n = 0;
        for (const char *q = p; q < end && n < (int)sizeof(one) - 1; ++q)
          one[n++] = *q;
        one[n] = 0;
        char *s = one;
        while (*s == ' ' || *s == '\t')
          ++s;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
          *--e = 0;
        if (*s)
        {
          bool dup = false;
          for (int i = 0; i < keptN; ++i)
            if (_stricmp(kept[i], s) == 0)
              dup = true;
          if (!dup)
          {
            _snprintf(kept[keptN], sizeof(kept[keptN]), "%s", s);
            ++keptN;
          }
        }
        p = *end ? end + 1 : end;
      }
    }
    // ... then everything the list did not mention, in the order it was already in.
    for (int i = 0; i < quickOrderN && keptN < kMaxList; ++i)
    {
      bool named = false;
      for (int k = 0; k < keptN; ++k)
        if (_stricmp(kept[k], quickOrder[i]) == 0)
          named = true;
      if (!named)
      {
        _snprintf(kept[keptN], sizeof(kept[keptN]), "%s", quickOrder[i]);
        ++keptN;
      }
    }
    for (int i = 0; i < kMaxList; ++i)
      quickOrder[i][0] = 0;
    quickOrderN = keptN;
    for (int i = 0; i < keptN; ++i)
      _snprintf(quickOrder[i], (int)sizeof(quickOrder[i]), "%s", kept[i]);
  }

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
    else if (strcmp(key, "autostart") == 0)
    {
      bool b;
      if (ParseBool(val, &b))
        c.autostart = b;
    }
    else if (strcmp(key, "quickcompact") == 0)
    {
      bool b;
      if (ParseBool(val, &b))
        c.quickCompact = b;
    }
    else if (strcmp(key, "quickown") == 0)
    {
      bool b;
      if (ParseBool(val, &b))
        c.quickOwn = b;
    }
    // ONE LINE PER ID, IN ORDER -- the same shape as `off=`. ⚠️ THE FILE SPELLS IT ONE ID PER LINE and the PANEL
    // sends the whole order as one comma-separated value (see ApplySetHost), so the two callers use the two
    // methods below rather than one method guessing which shape it was handed.
    else if (strcmp(key, "quickorder") == 0)
      c.QuickOrderAppend(val);
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
                   "# autostart : 1 to start with Windows (default 0)\n"
                   "# quickcompact : 1 to show every feature's on/off switch in the quick panel (default 0)\n"
                   "# quickown     : 1 to show the controls the features asked for there (default 0)\n"
                   "# quickorder   : a feature id, in the order its controls should appear in the quick panel;\n"
                   "#                repeat the line for more. Anything not named comes after, in load order.\n"
                   "lang=%s\n"
                   "theme=%s\n"
                   "autostart=%d\n"
                   "quickcompact=%d\n"
                   "quickown=%d\n",
                   LangName(c.lang), ThemeName(c.theme), c.autostart ? 1 : 0, c.quickCompact ? 1 : 0,
                   c.quickOwn ? 1 : 0);
  for (int i = 0; i < c.quickOrderN && off < outSize - (kMaxName + 16); ++i)
    off += _snprintf(out + off, outSize - off, "quickorder=%s\n", c.quickOrder[i]);
  for (int i = 0; i < c.offN && off < outSize - (kMaxName + 8); ++i)
    off += _snprintf(out + off, outSize - off, "off=%s\n", c.off[i]);
}

} // namespace apex

#endif // APEX_HOSTCONFIG_H
