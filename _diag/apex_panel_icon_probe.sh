#!/usr/bin/env bash
# A REAL THEME SWITCH, OBSERVED ON THE PANEL'S OWN WINDOW.
#
# The companion to apex_theme_switch_probe.sh, which watches the TRAY (through the host's log). This one
# watches the thing that has a window -- the settings panel's window icon, which the page cannot draw. It is
# read from the WINDOW rather than from the exe, because a window's icon is a value that was SET on it: a
# theme change has to trigger another WM_SETICON, and the class icon is cached at registration.
#
# BOTH DIRECTIONS ARE CHECKED, and that is the point: the icon has to agree with the page on screen, so a
# dark appearance must not end up beside a mark whose plate is cream. The mark's PLATE matches the
# background it belongs on and the glyph carries the contrast -- see apex/icons.h, which also records how
# this mapping was got backwards once.
#
# The system theme is restored on every exit path (trap).
#
# usage: bash _diag/apex_panel_icon_probe.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

KEY='HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize'
SCRATCH="$ROOT/build/_panel_icon_probe"
fail=0

ORIG=$(reg query "$KEY" //v AppsUseLightTheme 2>/dev/null |
  sed -n 's/.*REG_DWORD[ \t]*0x\([0-9a-fA-F]*\).*/\1/p' | head -1)
ORIG=${ORIG:-0}

Restore() {
  reg add "$KEY" //v AppsUseLightTheme //t REG_DWORD //d "$((16#$ORIG))" //f >/dev/null 2>&1
  "$ROOT/build/apex_broadcast.exe" >/dev/null 2>&1
  # ⚠️ BY NAME **AND** PATH -- `/IM apex.exe` kills the user's own copy. Filter in test/lib_procs.sh.
  apex_kill_own '_panel_icon'
  rm -rf "$SCRATCH"
}
trap Restore EXIT

SetTheme() {
  reg add "$KEY" //v AppsUseLightTheme //t REG_DWORD //d "$1" //f >/dev/null 2>&1
  "$ROOT/build/apex_broadcast.exe" >/dev/null 2>&1
}

echo "== building =="
gcc -O2 -o "$ROOT/build/apex_broadcast.exe" "$ROOT/_diag/apex_broadcast.c" -luser32 || exit 1
g++ -std=c++17 -O2 -mconsole -o "$ROOT/build/apex_window_icon_probe.exe" \
  "$ROOT/_diag/apex_window_icon_probe.cpp" -luser32 -lgdi32 || exit 1
echo "   ok"

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
cp -r "$ROOT/build/apex" "$SCRATCH/apex" || { echo "   FAIL: build/apex could not be copied"; exit 1; }
RUN="$SCRATCH/apex"

# The window icon the panel currently holds, as a luma. Prints the probe's own lines.
PanelIconLuma() {
  "$ROOT/build/apex_window_icon_probe.exe" apex-settings.exe ApexSettingsWnd 2>&1 |
    sed -n 's/.*luma=\([0-9.]*\).*/\1/p' | head -1
}

echo
echo "== the panel, started on the theme the system is using now =="
( cd "$RUN" && cmd //c start "" apex-settings.exe >/dev/null 2>&1 )
sleep 4
"$ROOT/build/apex_window_icon_probe.exe" apex-settings.exe ApexSettingsWnd 2>&1 | sed 's/^/   /'
FIRST_LUMA=$(PanelIconLuma)
if [ -z "$FIRST_LUMA" ]; then
  echo "   FAIL: the panel's window holds no measurable icon"
  exit 1
fi

# A light appearance gets the CREAM-plated mark (luma high), a dark one gets the DARK plate (luma low).
# Each mark's PLATE matches the background it belongs on, and the glyph carries the contrast -- see
# apex/icons.h. (This was written the other way round once, from "a light taskbar needs a dark icon": a
# correct panel then got reported as a failure.)
if [ "$ORIG" = "1" ]; then OTHER_VALUE=0; OTHER_THEME=dark; WANT="low"
else OTHER_VALUE=1; OTHER_THEME=light; WANT="high"
fi

echo
echo "== switching the system theme to $OTHER_THEME, with the panel OPEN =="
SetTheme "$OTHER_VALUE"
sleep 3
"$ROOT/build/apex_window_icon_probe.exe" apex-settings.exe ApexSettingsWnd 2>&1 | sed 's/^/   /'
SECOND_LUMA=$(PanelIconLuma)

if [ -z "$SECOND_LUMA" ]; then
  echo "   FAIL: the panel's window holds no measurable icon after the switch"
  fail=1
else
  # The mark must have MOVED to the other one -- not merely still be loadable.
  if awk -v a="$FIRST_LUMA" -v b="$SECOND_LUMA" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d > 32) }'; then
    echo "   the window icon changed with the theme: luma $FIRST_LUMA -> $SECOND_LUMA"
    # ...and in the right DIRECTION: a light appearance needs the cream plate.
    if [ "$WANT" = "high" ]; then
      if awk -v b="$SECOND_LUMA" 'BEGIN { exit !(b > 128) }'; then
        echo "   and it is the cream plate, which is what a light appearance needs"
      else
        echo "   FAIL: the system went light but the window kept the dark plate (luma $SECOND_LUMA)"
        fail=1
      fi
    else
      if awk -v b="$SECOND_LUMA" 'BEGIN { exit !(b < 128) }'; then
        echo "   and it is the dark plate, which is what a dark appearance needs"
      else
        echo "   FAIL: the system went dark but the window kept the cream plate (luma $SECOND_LUMA)"
        fail=1
      fi
    fi
  else
    echo "   FAIL: the window icon did not change (luma $FIRST_LUMA -> $SECOND_LUMA)"
    fail=1
  fi
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "OK: the panel's window icon follows a real theme change, to the plate that matches the appearance"
else
  echo "FAILED"
fi
exit $fail
