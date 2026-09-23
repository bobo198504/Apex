#!/usr/bin/env bash
# Gate: the standalone app's smoothing core.
#
# It compiles _diag/app_core_probe.cpp, which includes the REAL app/core.h, and checks the two things
# that are easy to confuse and were in fact confused once (reading the app's raw log, where 75 notches
# in and ~1900 deltas out "looked like" lost travel):
#
#   1. CONSERVATION is exact -- every amount the core is fed is handed over in full, at every speed.
#      A shortfall here is a real bug.
#   2. ATTENUATION is by design -- a slow roll moves less than the wheel reported, because the model's
#      Travel() ramps from Slow step up to the message's own size. That is the feature, not a loss.
#
# It also checks that each of the four parameters moves the number it claims to, with Glide length
# changing only the timing and Top speed never lowering anything.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

OUT="$ROOT/build/_app_core_probe.exe"
g++ -std=c++17 -O2 -I"$ROOT/common" -I"$ROOT/shared" -o "$OUT" "$ROOT/_diag/app_core_probe.cpp"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
