#!/usr/bin/env bash
# Gate: the swallow rule -- an event may be eaten ONLY when a replacement will be delivered.
#
# This is the rule that broke: with the runtime (tray) switch off, the hook kept swallowing while the
# injector stayed silent, so the original notch was eaten and nothing replaced it -- scrolling stopped
# working entirely. The probe sweeps every combination of the two switches, the skip list, the
# other-handler verdict and target presence, and asserts the invariant "swallowing implies delivering".
#
# The app has no panel yet, so this gate is the only thing standing between that bug and a user.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

OUT="$ROOT/build/_app_swallow_probe.exe"
g++ -std=c++17 -O2 -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$OUT" "$ROOT/_diag/app_swallow_probe.cpp"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
