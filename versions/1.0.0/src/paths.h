#ifndef APEX_PATHS_H
#define APEX_PATHS_H

// ---------------------------------------------------------------------------
// WHERE EVERYTHING LIVES. One place, so the layout is stated once and can be tested without Windows.
//
// THE LAYOUT (the user's requirement). Paths below are drawn with a trailing separator shown as (sep),
// because a line in a C++ comment that ends in a backslash CONTINUES THE COMMENT onto the next line --
// which silently ate three function definitions the first time this file was written.
//
//     Apex(sep)                     <- the folder the user copies around; nothing outside it is touched
//       apex.exe
//       apex.ini                    <- the HOST's settings, beside apex.exe
//       Plugins(sep)
//         SmoothWheel(sep)
//           SmoothWheel.dll         <- the feature
//           SmoothWheel.ini         <- the FEATURE's settings, beside its own dll
//         (future features, one folder each)
//
// WHY THE FEATURE FOLDER AND NOT ONE SHARED SETTINGS FILE: a feature can be added, removed, or handed to
// someone else by moving its folder. Its settings travel with it, and two features cannot write over each
// other's keys -- which is the failure a single shared file invites as soon as there are three features.
//
// WHY NOTHING IS STORED ELSEWHERE: this is a portable program. No %APPDATA%, no registry, no installer.
// The exe's own folder is found from the MODULE PATH rather than the current directory, because a
// shortcut or a launch from a terminal can set a working directory that is somewhere else entirely and
// the settings would be written there.
//
// THE HELPERS BELOW ARE PURE: they join and trim strings. Only ApexModuleDir() needs the platform, and
// it is the single hook the whole layout hangs from.
// ---------------------------------------------------------------------------

#include <string.h>
#include <stdio.h>

namespace apex {

// Path separator. Kept as a named constant so no call site has to remember which way it goes.
static const char kSep = '\\';

// Append `name` to `dir`, which must already end with a separator. Returns false if it will not fit.
inline bool JoinPath(const char *dir, const char *name, char *out, int outSize)
{
  if (!dir || !name || !out || outSize <= 0)
    return false;
  out[0] = 0;
  const int n = _snprintf(out, outSize, "%s%s", dir, name);
  return n > 0 && n < outSize;
}

// The directory part of a full path, WITH a trailing separator ("" where there is none).
inline void DirOf(const char *path, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  if (!path)
    return;
  const char *last = nullptr;
  for (const char *p = path; *p; ++p)
    if (*p == '\\' || *p == '/')
      last = p;
  if (!last)
    return;
  const int n = (int)(last - path) + 1;
  if (n >= outSize)
    return;
  memcpy(out, path, (size_t)n);
  out[n] = 0;
}

// The last component of a path, without its extension ("C:\a\b.exe" -> "b").
inline void StemOf(const char *path, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  if (!path)
    return;
  const char *name = path;
  for (const char *p = path; *p; ++p)
    if (*p == '\\' || *p == '/')
      name = p + 1;
  int n = 0;
  for (; *name && *name != '.' && n < outSize - 1; ++name)
    out[n++] = *name;
  out[n] = 0;
}

// ---- the platform hook -------------------------------------------------------------------------
//
// The folder apex.exe lives in, WITH a trailing separator. Implemented in paths_win.cpp.
bool ApexModuleDir(char *out, int outSize);

// ---- the derived paths -------------------------------------------------------------------------

// <exe dir>apex.ini
inline bool HostConfigPath(char *out, int outSize)
{
  char dir[512] = {0};
  if (!ApexModuleDir(dir, (int)sizeof(dir)))
    return false;
  return JoinPath(dir, "apex.ini", out, outSize);
}

// <exe dir>Plugins (with a trailing separator)
inline bool PluginsDir(char *out, int outSize)
{
  char dir[512] = {0};
  if (!ApexModuleDir(dir, (int)sizeof(dir)))
    return false;
  return JoinPath(dir, "Plugins\\", out, outSize);
}

// <exe dir>Plugins\<id> (with a trailing separator)
inline bool FeatureDir(const char *id, char *out, int outSize)
{
  if (!id || !*id)
    return false;
  char p[512] = {0};
  if (!PluginsDir(p, (int)sizeof(p)))
    return false;
  const int n = _snprintf(out, outSize, "%s%s\\", p, id);
  return n > 0 && n < outSize;
}

// <exe dir>Plugins\<id> (with a trailing separator)<id>.dll -- the convention a feature folder follows. The loader also accepts
// any single .dll in the folder, so a differently-named binary still loads; this is just the expected
// name, and what the settings window offers to open.
inline bool FeatureDllPath(const char *id, char *out, int outSize)
{
  char d[512] = {0};
  if (!FeatureDir(id, d, (int)sizeof(d)))
    return false;
  const int n = _snprintf(out, outSize, "%s%s.dll", d, id);
  return n > 0 && n < outSize;
}

// <exe dir>Plugins\<id> (with a trailing separator)<id>.ini -- the feature's own settings, in its own folder.
inline bool FeatureConfigPath(const char *id, char *out, int outSize)
{
  char d[512] = {0};
  if (!FeatureDir(id, d, (int)sizeof(d)))
    return false;
  const int n = _snprintf(out, outSize, "%s%s.ini", d, id);
  return n > 0 && n < outSize;
}

} // namespace apex

#endif // APEX_PATHS_H
