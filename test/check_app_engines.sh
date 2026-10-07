#!/usr/bin/env bash
# Gate: A PROGRAM THAT BRINGS ITS OWN SMOOTHING IS ALWAYS LEFT ALONE -- WHETHER OR NOT ITS ENGINE IS SWITCHED ON.
#
# The user's rule (2026-10-07): "这些有独立引擎的，不管它们有没有打开，都是在排除名单内的，不接管". Two programs qualify
# today (REAPER, through this project's plugin, and Lertaro, which ports the same model), and the list is
# common/engines.h -- the SAME table the settings note is built from, so the sentence and the behaviour cannot
# disagree.
#
# WHY IT IS A GATE AND NOT A COMMENT. Both ways of getting it wrong are silent:
#   * a listed program is NOT excluded -> Apex smooths on top of that program's own engine, and the user sees
#     stuttering with nothing anywhere to explain it (this is the state the rule was written to end: the exclusion
#     used to depend on REAPER having the plugin LOADED, so a REAPER with the plugin switched off was smoothed);
#   * an UNLISTED program IS excluded -> its wheels are never smoothed and nothing says why.
#
# WHAT THIS CANNOT REACH: the wheel rule's own half in host_win.cpp (does ExternalHandlerState actually ask the
# table?). That needs a real REAPER and a real Lertaro holding real markers on this machine, so it is pinned in the
# cheapest way that still fails when the wiring is removed -- a static check at the end, the same shape
# check_reaper_note.sh uses for its three ends.
#
# usage: test/check_app_engines.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

OUT="$ROOT/build/_engine_exclude_probe.exe"
g++ -std=c++17 -O2 -I"$ROOT/common" -I"$ROOT/apex" -o "$OUT" "$ROOT/_diag/engine_exclude_probe.cpp" || exit 1
rc=0
"$OUT" || rc=$?
rm -f "$OUT"

echo
echo "== the platform layer's half =="
# ⚠️ THE TABLE DECIDES NOTHING UNLESS THE WHEEL RULE ASKS IT. A gate that only ran the probe would stay green with
# ExternalHandlerState reverted to "REAPER, if the module scan finds the plugin" -- i.e. with the user's rule gone.
if grep -q 'ProgramBringsOwnEngine' "$ROOT/apex/host_win.cpp"; then
  echo "  ok  the wheel rule consults the table (ExternalHandlerState in host_win.cpp)"
else
  echo "  FAIL: nothing in the platform layer asks ProgramBringsOwnEngine, so the table would be decoration"
  rc=1
fi
# ... and the note must take the names from the host rather than keeping a table of its own, or a program added
# tomorrow would be dropped by a feature nobody remembered to update (see ApexEngine in abi.h).
if grep -q 'engines\[i\]\.name' "$ROOT/features/SmoothWheel/feature_smoothwheel.cpp"; then
  echo "  ok  and the note reads the names off the ABI's answer, with no kind table of its own"
else
  echo "  FAIL: the feature no longer takes the engine names from the host"
  rc=1
fi

echo
if [ $rc -eq 0 ]; then
  echo "OK: the programs with their own smoothing are excluded whether or not their engine is on, and the note gets"
  echo "    its names from the one table"
else
  echo "FAILED: the own-engine rule"
fi
exit $rc
