#!/usr/bin/env bash
# Gate: the HOST's own settings file (apex.ini) -- what it means, and that it survives a round trip.
#
# It compiles _diag/apex_hostconfig_probe.cpp, which includes the REAL apex/hostconfig.h (with APEX_BUILDING_HOST,
# so the "a feature may not include this" guard is satisfied the same way a host source file satisfies it) and
# runs it. No Windows, no window, no process.
#
# ⚠️ WHY IT EXISTS NOW, AND WHAT IT IS REALLY FOR. The file was already read and written before this gate, and the
# part that made a check worth writing is the QUICK PANEL'S ORDER (`quickorder` in hostconfig.h): the General page
# lets the user drag the features that are mapped into the flyout, and what it sends back is the list it is
# SHOWING -- which is not the same as the list that has to be stored, because a feature that maps nothing right
# now has no row on that page and must not lose the place it had. "The order I am looking at" and "the order that
# gets saved" are two different lists, and that difference is the rule this gate pins down.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

SRC="$ROOT/_diag/apex_hostconfig_probe.cpp"
if [ ! -f "$SRC" ]; then
  echo "FAIL: $SRC is missing -- the host's own settings file is not being checked at all"
  exit 1
fi

OUT="$ROOT/build/_hostconfig_probe.exe"
# -DAPEX_BUILDING_HOST, like the host's own translation units and like check_apex_quickpanel.sh: without it the
# header refuses to compile, which is the boundary working rather than an obstacle.
g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I"$ROOT/apex" -o "$OUT" "$SRC"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
