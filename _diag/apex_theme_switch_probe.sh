#!/usr/bin/env bash
# A REAL LIGHT/DARK SWITCH, OBSERVED FROM OUTSIDE.
#
# THE ONE THING THE OTHER TESTS CANNOT DO. test/check_apex_icons.sh proves the marks exist, that they differ,
# and that the theme rule picks the right one; it starts the host once per setting and reads the log. What it
# does not do is CHANGE the system theme while the host is running -- which is the user's actual complaint
# ("the icon does not switch when the theme changes"), and the path where the whole thing was broken.
#
# So this flips the real setting, broadcasts the real message, and reads what the host did about it. The
# theme is restored on every exit path (trap), because leaving someone's desktop inverted would be a worse
# bug than the one being tested.
#
# NOT IN run_all.sh, deliberately: it inverts the user's desktop for a few seconds, and a gate that does that
# on every run would be a gate people stop running.
#
# usage: bash _diag/apex_theme_switch_probe.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

KEY='HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize'
RUN="$ROOT/build/_switch_probe/apex"
fail=0

# The value that was there before this script touched anything.
ORIG=$(reg query "$KEY" //v AppsUseLightTheme 2>/dev/null |
  sed -n 's/.*REG_DWORD[ \t]*0x\([0-9a-fA-F]*\).*/\1/p' | head -1)
ORIG=${ORIG:-0}

Restore() {
  reg add "$KEY" //v AppsUseLightTheme //t REG_DWORD //d "$((16#$ORIG))" //f >/dev/null 2>&1
  "$ROOT/build/apex_broadcast.exe" >/dev/null 2>&1
  # ⚠️ BY NAME **AND** PATH. This used to be `taskkill /F /IM apex.exe`, which kills EVERY apex.exe on
  # the machine -- including the one the user is running. Filter in test/lib_procs.sh.
  apex_kill_own '_switch_probe'
  rm -rf "$ROOT/build/_switch_probe"
}
trap Restore EXIT

SetTheme() { # 1 = light, 0 = dark
  reg add "$KEY" //v AppsUseLightTheme //t REG_DWORD //d "$1" //f >/dev/null 2>&1
  "$ROOT/build/apex_broadcast.exe" >/dev/null
}

# The last tray line the host wrote, as-is (it is the only evidence there is -- the tray belongs to the shell).
LastTrayLine() { grep '^tray: ' "$RUN/apex.log" 2>/dev/null | tail -1; }

# Wait until the LAST tray line matches a pattern (or say so).
WaitForLine() {
  local pat="$1" i=0
  while [ $i -lt 24 ]; do
    if LastTrayLine | grep -q "$pat"; then return 0; fi
    sleep 0.25
    i=$((i + 1))
  done
  return 1
}

echo "== preparing =="
gcc -O2 -o "$ROOT/build/apex_broadcast.exe" "$ROOT/_diag/apex_broadcast.c" -luser32 || exit 1
echo "   broadcast helper built"
echo "   the theme starts at AppsUseLightTheme=$ORIG (it will be put back exactly)"

rm -rf "$ROOT/build/_switch_probe"
mkdir -p "$ROOT/build/_switch_probe"
cp -r "$ROOT/build/apex" "$RUN" || { echo "   FAIL: build/apex could not be copied -- build first"; exit 1; }
printf 'theme=auto\nenabled=1\n' > "$RUN/apex.ini"
rm -f "$RUN/apex.log"

echo
echo "== starting the host with the theme as it is now =="
( cd "$RUN" && cmd //c start "" apex.exe >/dev/null 2>&1 )
if ! WaitForLine 'load='; then
  echo "   FAIL: no tray line after 6s -- the host did not start or did not reach the tray"
  exit 1
fi
echo "   $(LastTrayLine)"
FIRST=$(LastTrayLine)

# A theme switch is only interesting if the answer CHANGES, so the test moves to the OPPOSITE of what is in
# force -- derived from the line the host actually wrote, not from the registry (if the two ever disagreed,
# the disagreement itself is the finding, and this way it shows up instead of being papered over).
#
# ⚠️ THE MAPPING IS THE DIRECT ONE -- a light appearance gets the cream-plated mark (apex/icons.h). These two
# lines said the opposite for a while, from the "a light taskbar needs a dark icon" reasoning: the host was
# behaving correctly and this probe reported it as a failure.
CURRENT_THEME=$(printf '%s' "$FIRST" | sed -n 's/.*(\(dark\|light\) theme.*/\1/p')
if [ "$CURRENT_THEME" = "dark" ]; then
  OTHER_THEME=light; OTHER_VALUE=1; OTHER_MARK='icon=light-mark'
else
  OTHER_THEME=dark; OTHER_VALUE=0; OTHER_MARK='icon=dark-mark'
fi
if [ -z "$CURRENT_THEME" ]; then
  echo "   FAIL: the host's line does not say which theme it used: $FIRST"
  exit 1
fi

echo
echo "== switching the system theme to $OTHER_THEME, with the host RUNNING =="
SetTheme "$OTHER_VALUE"
if WaitForLine "$OTHER_MARK"; then
  echo "   $(LastTrayLine)"
  echo "   the mark changed to the one $OTHER_THEME needs"
else
  echo "   FAIL: the mark did not change to $OTHER_MARK within 6s"
  echo "   last line: $(LastTrayLine)"
  fail=1
fi

echo
echo "== switching back =="
SetTheme "$((16#$ORIG))"
if WaitForLine "load=ok"; then
  echo "   $(LastTrayLine)"
else
  echo "   (the host wrote no further line; it may already have been at that value)"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "OK: a real system theme change, with the host running, moves the tray mark"
else
  echo "FAILED"
fi
exit $fail
