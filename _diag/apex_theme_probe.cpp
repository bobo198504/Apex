// ---------------------------------------------------------------------------
// apex_theme_probe -- the rule that says WHICH WAY THE APPEARANCE RESOLVED, exhaustively.
//
// THE RULE IS `ThemeResolvesLight(theme, systemIsLight)` in apex/hostconfig.h, and it is a pure function for
// exactly this reason: the whole table is three settings values times two system values, so it can be
// enumerated rather than argued about.
//
// WHY IT NEEDS A TEST AT ALL. The tray's mark, the window's icon, the caption and the page all ask this
// question, and each one used to answer it for itself. The tray's answer was `SystemIsLightTheme()` alone,
// which ignores a PINNED theme -- so with the system light and the theme pinned to dark, the page and the
// caption went dark while the tray kept the mark meant for a light background. It was also reachable by a
// *setting*: the panel reported "the tray icon is chosen by the theme" when the theme was changed, and the
// choice was made somewhere else. This walks the table, so a change to the rule has to be deliberate.
//
// Build: g++ -std=c++17 -O2 -I apex -o build/apex_theme_probe.exe _diag/apex_theme_probe.cpp
// Exit:  0 = the table is as intended, 1 = a case disagrees.
// ---------------------------------------------------------------------------

#include "hostconfig.h"
#include <stdio.h>

using apex::Theme;
using apex::ThemeResolvesLight;

int main()
{
  struct Case
  {
    Theme theme;
    bool systemIsLight;
    bool wantLight;
    const char *why;
  };

  // THE WHOLE TABLE -- both system values for each of the three settings, so nothing is left unstated.
  const Case cases[] = {
      {Theme::kAuto, true, true, "auto follows a light system"},
      {Theme::kAuto, false, false, "auto follows a dark system"},
      {Theme::kLight, true, true, "pinned light, on a light system"},
      {Theme::kLight, false, true, "pinned light wins over a dark system"},
      {Theme::kDark, false, false, "pinned dark, on a dark system"},
      {Theme::kDark, true, false, "pinned dark wins over a light system"},
  };

  printf("the theme rule (ThemeResolvesLight)\n");
  int failures = 0;
  for (const Case &c : cases)
  {
    const bool got = ThemeResolvesLight(c.theme, c.systemIsLight);
    const bool ok = got == c.wantLight;
    if (!ok)
      ++failures;
    printf("  %-46s system=%-5s -> %-5s  %s\n", c.why, c.systemIsLight ? "light" : "dark",
           got ? "light" : "dark", ok ? "ok" : "FAIL");
  }

  printf("\n");
  if (failures == 0)
  {
    printf("OK: a pinned theme overrides the system, and auto follows it\n");
    return 0;
  }
  printf("FAILED: %d case(s)\n", failures);
  return 1;
}
