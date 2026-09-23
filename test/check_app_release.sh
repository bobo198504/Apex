#!/usr/bin/env bash
# Gate: THE RELEASE MODEL -- how long a window lasts, and therefore how long an ending lasts.
#
# This is the app's own small model, sitting between the main model and the delivery layer (common/release.h).
# It exists because ONE number was being asked to do two opposite jobs: the window is the RAMP for a lone
# scroll (wants to be short, or it reads as lag) and the TAIL for a roll (wants to be long -- the user's note
# is "让滚动的时候，收尾拉长的"). So a message inside a roll gets the Glide setting plus a FIXED 200 ms.
#
# WHAT THIS GATE PINS, and each one is a way the rule could go wrong rather than a restatement of it:
#
#   * a rolling ending really is 200 ms longer                (the change does what was asked);
#   * a LONE scroll is untouched, to the last fraction        (the Glide slider goes on meaning what it meant);
#   * the addition is the same at every rolling speed         (it is fixed, not scaled by speed);
#   * travel is conserved across the boundary where the window steps by 200 ms
#                                                            (a shrinking window would drop what the windows
#                                                             in flight still held -- check_app_core does the
#                                                             exhaustive version; this is the same promise
#                                                             measured through the release model's own probe).
#
# usage: test/check_app_release.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

OUT="$ROOT/build/_app_release_probe.exe"
g++ -std=c++17 -O2 -I"$ROOT/common" -I"$ROOT/shared" -o "$OUT" "$ROOT/_diag/app_release_probe.cpp"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
