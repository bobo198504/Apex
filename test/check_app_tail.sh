#!/usr/bin/env bash
# Gate: the delivered SHAPE -- a lone notch settles, a roll is left alone, and the distance is unchanged.
#
# The reported symptom was "the tail stops suddenly". Measured, the cause was a shape: the model's own
# rule gives a NON-OVERLAPPING window a constant rate (correct for a continuous roll, where tiling
# windows are already flat and easing only adds ripple), and for a LONE notch -- which is what a slow
# roll produces -- that means running at full speed right up to the frame it stops on.
#
# The app therefore asks for the model's own eased shape when a window will not touch its neighbour, and
# leaves the model's rule untouched when it will. This gate pins all three consequences:
#   * a lone notch no longer stops dead (last frame well below the peak rate);
#   * a roll still gets the model's rule, and a tiling gap is treated as a roll (the model's ripple
#     warning still applies there);
#   * the TOTAL handed over is identical either way, which is what makes the change safe -- the ease
#     redistributes within a window, so no model parameter's meaning changes.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

OUT="$ROOT/build/_app_tail_probe.exe"
g++ -std=c++17 -O2 -I"$ROOT/common" -I"$ROOT/shared" -o "$OUT" "$ROOT/_diag/app_tail_probe.cpp"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
