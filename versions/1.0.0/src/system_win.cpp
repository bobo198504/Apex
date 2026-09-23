// ---------------------------------------------------------------------------
// WHAT THE SYSTEM SAYS -- the two questions both Apex processes ask.
//
// WHY THIS IS ITS OWN FILE, LINKED INTO BOTH EXES: apex.exe needs the answer to pick the tray icon, and
// apex-settings.exe needs it for its window icon and its caption. The alternative shapes were both worse:
//
//   * the panel linking host_win.o -- which would drag the wheel hook, the injection thread and the target
//     lookup into the panel's process, exactly what the process split exists to prevent;
//   * two copies of the same fifteen registry lines -- which is how two answers to one question start to
//     differ, and the caption and the page would then disagree about which theme is in force.
//
// NOTHING HERE TOUCHES THE HOST'S STATE, which is why sharing it costs nothing: no hook, no timer, no
// globals. Two functions and a string comparison.
// ---------------------------------------------------------------------------

#include "host.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace apex {
namespace host {

namespace {
// A registry string under HKCU. false when it is not there, which callers treat as "use the default"
// rather than as an error.
bool RegString(const char *keyPath, const char *valueName, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;
  HKEY k = nullptr;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &k) != ERROR_SUCCESS)
    return false;
  DWORD sz = (DWORD)outSize, type = 0;
  const bool ok = RegQueryValueExA(k, valueName, nullptr, &type, (LPBYTE)out, &sz) == ERROR_SUCCESS;
  RegCloseKey(k);
  if (ok)
    out[outSize - 1] = 0;
  return ok && out[0];
}
} // namespace

// Is the system using its light theme for apps? AppsUseLightTheme = 1 means light; a missing value or
// anything else means dark.
//
// DARK IS THE DEFAULT ON PURPOSE, and it is not a coin toss: a dark caption above a dark page is
// unremarkable, while a bright one flashes every time the panel opens. The same reasoning picks the tray
// icon.
bool SystemIsLightTheme()
{
  HKEY k = nullptr;
  static const char *kPath = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
  if (RegOpenKeyExA(HKEY_CURRENT_USER, kPath, 0, KEY_READ, &k) != ERROR_SUCCESS)
    return false;
  DWORD v = 0, sz = sizeof(v), type = 0;
  const bool ok =
      RegQueryValueExA(k, "AppsUseLightTheme", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS;
  RegCloseKey(k);
  return ok && v != 0;
}

// The user's preferred UI language as a BCP-47 tag ("zh-CN", "en-US"). This is what resolves `auto`, and
// it is read from the USER's own preference rather than the system locale, so someone who set their
// display language explicitly gets that.
bool PreferredUiLanguage(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;

  // The documented place, and the one that reflects the user's own choice. On some profiles it holds a
  // multi-string (several languages); the first entry is the one that matters, which is what reading it as
  // a plain string gives us because the entries are NUL-separated.
  if (RegString("Control Panel\\Desktop\\PreferredUILanguages", nullptr, out, outSize))
    return true;
  // The legacy value, for older systems and for profiles where the above is absent.
  char mui[16] = {0};
  if (RegString("Control Panel\\Desktop\\MuiCached", "MachinePreferredUILanguages", mui, sizeof(mui)))
  {
    _snprintf(out, outSize, "%s", mui);
    return true;
  }
  // Last resort: the OS language, which is at least the right family.
  LANGID lang = GetUserDefaultUILanguage();
  _snprintf(out, outSize, "%04x", (unsigned)lang);
  return true;
}

// True when a language tag names Chinese. Matches the PRIMARY subtag, so "zh-CN", "zh-Hans" and a bare
// "zh" all answer the same, and so does a legacy LCID in hex (the fallback above).
bool LanguageTagIsChinese(const char *tag)
{
  if (!tag || !*tag)
    return false;
  if (_strnicmp(tag, "zh", 2) == 0 && (tag[2] == 0 || tag[2] == '-' || tag[2] == '_'))
    return true;
  // A hex LCID: the Chinese primaries are 0x04 (Chinese) and 0x7C04 (Chinese, traditional).
  if (strlen(tag) == 4)
  {
    const unsigned v = (unsigned)strtoul(tag, nullptr, 16);
    const unsigned primary = v & 0x3FF;
    if (primary == 0x04 || primary == 0x7C04)
      return true;
  }
  return false;
}

} // namespace host
} // namespace apex
