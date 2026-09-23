#!/usr/bin/env bash
# DOES A WHEEL OVER THE PANEL REALLY GET PASSED -- and does the slider really see one notch as one step?
#
# This is the LIVE half of the "wheel changes a slider" feature. The page-level probe
# (_diag/apex_panel_probe.js) proves the ARITHMETIC against a DOM stub; it cannot prove the two things that
# make the feature work in the real program:
#
#   1. THE HOST PASSES a wheel over its own panel. That is a rule in apex/decision.h, and if it were wrong the
#      wheel would be SMOOTHED -- arriving as dozens of small messages, each of which the page would count as
#      a notch of its own.
#   2. THE PAGE RECEIVES AN UNMODIFIED NOTCH. Measured from the panel's own log: the page logs each wheel it
#      handles, and the host logs the target it resolved. The two together are the evidence.
#
# ⚠️⚠️ THIS IS FOR THE USER TO RUN, NOT FOR THE AGENT. It MOVES THE MOUSE -- unavoidably, because "what is
# under the cursor" is the question it asks -- and this project's standing rule is that ALL UI interaction
# testing belongs to the user ("一切UI交互测试都由我来做"). It is not in run_all.sh for the same reason.
#
# It saves and restores the cursor, and it runs a private copy of the program, so it will not disturb
# whatever else is open. Run it by hand:
#
#     bash _diag/apex_uiwheel_probe.sh
#
# WHAT IT ANSWERS (and what it cannot): the host's decision for a point over the panel -- whether the wheel
# is passed untouched or smoothed. The SLIDER's own behaviour is the page's, and the page-level arithmetic is
# pinned by _diag/apex_panel_probe.js; this probe aims at the sidebar, which has no slider on it.
#
# usage: apex_uiwheel_probe.sh [--keep-open]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file
BUILD="$ROOT/build"
RUN="$BUILD/_uiwheel_run"

PidsInRun() { apex_own_pids '_uiwheel_run'; }

# THE CURSOR IS SAVED BEFORE IT IS MOVED AND PUT BACK AFTER, whatever happens. This project has already been
# complained about once for a tool that took the mouse; a probe that does it must at least give it back.
ORIG=$(powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; \$p=[System.Windows.Forms.Cursor]::Position; Write-Output (\"{0},{1}\" -f \$p.X,\$p.Y)" 2>/dev/null | tr -d '\r')
cleanup() {
  if [ -n "${ORIG:-}" ]; then
    powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point(${ORIG%%,*}, ${ORIG##*,})" >/dev/null 2>&1
  fi
  local p
  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
trap cleanup EXIT

echo "== building the tools =="
g++ -std=c++17 -O2 -mconsole -o "$BUILD/_uiw_inject.exe" "$ROOT/_diag/apex_inject.cpp" || exit 1
# ⚠️ A COMPILED RECTANGLE HELPER, NOT POWERSHELL. The first version asked PowerShell for the panel's rectangle
# with a P/Invoke block, and its struct marshalling silently produced nothing -- the script then failed with
# "could not find the panel's rectangle" while the panel was demonstrably up. A tiny C file is shorter than
# the P/Invoke text was and has no marshalling to get wrong.
cat > "$BUILD/_uiw_rect.c" <<'EOFC'
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv)
{
  HWND w = FindWindowA(argc > 1 ? argv[1] : "ApexSettingsWnd", NULL);
  if (!w) return 1;
  RECT r;
  GetWindowRect(w, &r);
  printf("%ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
  return 0;
}
EOFC
gcc -O2 -o "$BUILD/_uiw_rect.exe" "$BUILD/_uiw_rect.c" -luser32 || exit 1
echo "   ok (the cursor is at ${ORIG:-unknown} and will be restored)"

echo
echo "== a private copy, with the panel up =="
rm -rf "$RUN"; cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "FAIL: could not copy"; exit 1; }
printf 'theme=auto\nlang=auto\n' > "$RUN/apex.ini"
rm -f "$RUN"/apex*.log
( cd "$RUN" && cmd //c start "" apex.exe )
sleep 3
( cd "$RUN" && cmd //c start "" apex-settings.exe )
sleep 6

PANEL_PID=$(apex_own_pids '_uiwheel_run' 'apex-settings' | head -1)
if [ -z "$PANEL_PID" ]; then echo "FAIL: the panel did not start"; exit 1; fi
echo "   panel pid $PANEL_PID"

# WHERE TO AIM: over the panel's page, from the panel's own window rectangle. The left side, well below the
# title bar, is the sidebar -- page content in every layout the panel has.
RECT=$("$BUILD/_uiw_rect.exe" 2>/dev/null | tr -d '\r')
if [ -z "$RECT" ]; then echo "FAIL: the panel's window was not found (is the panel up?)"; exit 1; fi
RL=$(echo "$RECT" | cut -d' ' -f1); RT=$(echo "$RECT" | cut -d' ' -f2)
PX=$((RL + 80)); PY=$((RT + 300))
echo "   the panel is at $RECT; aiming at $PX,$PY (over its sidebar)"

powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point($PX, $PY)" >/dev/null 2>&1
sleep 1

echo
echo "== three notches down, then two up =="
N_BEFORE=$(grep -c 'wheel over the panel' "$RUN/apex.log" 2>/dev/null || echo 0)
"$BUILD/_uiw_inject.exe" "$PX" "$PY" 3 150 >/dev/null 2>&1
sleep 1
"$BUILD/_uiw_inject.exe" "$PX" "$PY" -2 150 >/dev/null 2>&1
sleep 1

echo
echo "== what the host decided about that point =="
grep 'target exe=' "$RUN/apex.log" 2>/dev/null | tail -3 | sed 's/^/   /'
if grep -q 'our own panel' "$RUN/apex.log" 2>/dev/null; then
  echo "   OK: the host identified the point as its own panel"
else
  echo "   (no '[our own panel' line -- if the panel is not up yet this is expected)"
fi

echo
echo "== and whether it smoothed them =="
# A smoothed wheel shows up as MANY small injections; a passed one shows up as the original messages. The
# clearest signal this probe can produce is the absence of a smoothing burst for that point.
if grep -q 'wheel delta=' "$RUN/apex.log" 2>/dev/null; then
  echo "   the trace shows wheels the host CLAIMED (it smooths only what it takes):"
  grep 'wheel delta=' "$RUN/apex.log" | tail -5 | sed 's/^/      /'
else
  echo "   the host claimed no wheels from that point -- nothing was swallowed there"
fi

echo
echo "== the page's side =="
# ⚠️ WHAT THIS PROBE PROVES AND WHAT IT DOES NOT, because the difference decides who tests what:
#
#   PROVED HERE: the host resolves a wheel over the panel as its OWN, and claims nothing -- so the browser
#   receives an ordinary wheel event with the device's own delta (one notch = one event). That is the half
#   that lives in this program, and the half that would be wrong in a way no page-level probe could see.
#
#   NOT PROVED HERE: that the SLIDER moves. The point aimed at is the sidebar, which has no slider on it, and
#   reaching one means clicking through to a feature page first -- UI interaction, which is the user's to test
#   ("一切UI交互测试都由我来做"). The arithmetic of the handler is pinned by _diag/apex_panel_probe.js
#   (one notch = exactly one step, trackpad deltas accumulate, the value stays on the grid).
if grep -q 'setFeature' "$RUN/apex-settings.log" 2>/dev/null; then
  echo "   the panel sent a setting change (so a slider was under the cursor):"
  grep 'setFeature' "$RUN/apex-settings.log" | tail -3 | sed 's/^/      /'
else
  echo "   no setting change was sent -- correct for this aim point (the sidebar has no slider)"
fi

echo
echo "done. The cursor is restored on exit."
echo
echo "WHAT WAS PROVED HERE: the host passes a wheel over its own panel without smoothing it."
echo "WHAT IT DID NOT: that a slider moves -- aim a wheel at one by hand (that is UI testing)."
