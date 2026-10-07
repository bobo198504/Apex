#ifndef APEX_COMMON_SYSTEM_THEME_H
#define APEX_COMMON_SYSTEM_THEME_H

// ---------------------------------------------------------------------------
// IS THE SYSTEM USING ITS LIGHT THEME FOR APPS -- ONE IMPLEMENTATION, FOR THE HOST *AND* FOR ITS FEATURES.
//
// WHY IT MOVED HERE. It was written in apex/system_win.cpp, for the two things the host itself needs it for
// (the tray icon, and the panel's caption). A feature that restyles OTHER programs' window chrome needs the
// same number for a blunter reason: it is the value it writes into every window it touches.
//
// And a feature cannot reach it. `apex/host.h` refuses to compile without APEX_BUILDING_HOST -- deliberately,
// so that a feature reaching into host internals is a build failure rather than a convention -- and the ABI
// has no channel for this question. The three ways out were:
//
//   * COPY THE TWELVE LINES into the feature. This project's own rule forbids it, and the rule exists because
//     of three separate incidents: "if changing a rule means changing two places, the rule is broken", and a
//     rule copied once is a rule fixed in one place and left wrong in the other.
//   * ADD A CHANNEL TO THE ABI. Correct in the long run, and the wrong size of change for one question: a
//     version bump carried in two languages (apex/abi.h and features/AutoIME/src/abi.rs), plus the gate that
//     compares the two struct sizes, plus every feature rebuilt -- to pass back an answer that is a registry
//     read.
//   * MOVE THE ONE IMPLEMENTATION somewhere both sides already share. That is this: `common/` is exactly the
//     directory the host and its features are both allowed to include (see apex/build.sh's two flag sets).
//
// ⚠️ AND IT IS THE ONE HEADER IN common/ THAT TOUCHES WINDOWS. The directory is described as "pure logic,
// no Windows" (AGENTS.md, the tree), and everything else in it is. This is not logic -- it is a question about
// the machine, and the machine is what has to answer it. A caller that needs it links advapi32, which the
// host already does and every feature's build line already carries.
// ---------------------------------------------------------------------------

#include <windows.h>

namespace apex {
namespace common {

// AppsUseLightTheme = 1 means light. A MISSING VALUE OR ANYTHING ELSE MEANS DARK, and that is not a coin
// toss -- it is the host's own reasoning, kept because the two callers must not disagree about the default
// either: a dark caption above a dark page is unremarkable, while a bright one flashes every time a window
// opens.
inline bool SystemIsLightTheme()
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

} // namespace common
} // namespace apex

#endif // APEX_COMMON_SYSTEM_THEME_H
