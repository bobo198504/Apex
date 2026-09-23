#!/usr/bin/env bash
# Gate: the standalone app's settings handling.
#
# It compiles _diag/app_config_probe.cpp, which includes the REAL app/config.h, and runs it: defaults,
# clamping at both ends of every range, the hand-editable file's parse/format round trip, an unknown
# key being ignored, and the skip list matching on the exe NAME however it was written.
#
# These are the things a user or a bug report can get wrong in the settings file, and getting them
# wrong means a nonsense number reaching the model -- the app has no panel to guard the values yet, so
# this gate is the guard.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

OUT="$ROOT/build/_app_config_probe.exe"
g++ -std=c++17 -O2 -I"$ROOT/common" -I"$ROOT/shared" -o "$OUT" "$ROOT/_diag/app_config_probe.cpp"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
