#!/usr/bin/env bash
# Gate: WHICH DEVICE SENT THE WHEEL -- the classifier, and what the feature does with its verdict.
#
# WHY THIS GATE EXISTS. A touchpad is a continuous surface: the OS already reports it smoothly, so a second,
# feature-side easing on top of it is easing something that was never stepped. The app had NO rule about this --
# its only input filter was the injected flag (see docs/rules/wheel.md) -- so a touchpad's wheel was smoothed
# exactly like a notched mouse's. The rule now lives in common/device.h, moved there from the plugin project at
# the user's instruction.
#
# IT IS TWO CHECKS, AND BOTH ARE NEEDED, because "the classifier separates the senders" and "SmoothWheel leaves
# a touchpad alone" are different claims that can disagree:
#
#   1. _diag/app_device_probe.cpp -- the classifier alone, over the streams the three senders produce, including
#      the cases that could fool it (a touch-tagged whole notch; a creeping touchpad that goes steady). No DLL.
#   2. _diag/feature_device_probe.cpp -- the BUILT DLL, loaded and called exactly as the host calls it, with
#      `onWheel`'s return value as the verdict (1 = it takes the wheel, 0 = it leaves the message alone). This
#      is what catches "the feature forgot to ask", "it asks after another rule already took it", and "it asks
#      but the verdict is not wired to the decision".
#
# ⚠️ THE FAILURE THIS PREVENTS IS SILENT: a touchpad smoothed as if it were a mouse does not error anywhere --
# it just feels wrong, and the only person who can notice is the one holding the touchpad. That is why the rule
# is a pure function with a probe instead of a comment.
#
# usage: test/check_feature_device.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

fail=0

echo "== the classifier itself (common/device.h) =="
OUT="$ROOT/build/_app_device_probe.exe"
g++ -std=c++17 -O2 -I"$ROOT/common" -o "$OUT" "$ROOT/_diag/app_device_probe.cpp" || exit 1
"$OUT" || fail=1
rm -f "$OUT"

# ⚠️ AN ERROR, NOT A SKIP, when the feature is not built -- the same rule the other artefact gates follow: a
# check that quietly stops checking is worse than one that fails ("一扇不会红的门比没有门更糟").
DLL="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.dll"
if [ ! -f "$DLL" ]; then
  echo
  echo "FAIL: $DLL is not built, so the half of this gate that runs the real feature cannot run"
  echo "      (run apex/build.sh first -- this gate checks the ARTEFACT, not the source)"
  exit 1
fi

echo
echo "== what SmoothWheel.dll does with each device =="
OUT2="$ROOT/build/_feature_device_probe.exe"
# ⚠️ A SCRATCH FOLDER UNDER build/, NEVER THE USER'S: `init` reads the feature's settings file, and a probe
# must not be able to touch a deployed installation.
SCRATCH="$ROOT/build/_device_probe_dir"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
g++ -std=c++17 -O2 -I"$ROOT/apex" -o "$OUT2" "$ROOT/_diag/feature_device_probe.cpp" || exit 1
"$OUT2" "$DLL" "$SCRATCH" || fail=1
rm -f "$OUT2"
rm -rf "$SCRATCH"

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: the device rule does not hold -- a touchpad would be smoothed, or a wheel would not be"
  exit 1
fi
echo "OK: the three senders separate, and SmoothWheel takes a mouse (or a free-spinning wheel) and leaves a"
echo "    touchpad -- and a wheel it cannot identify yet -- entirely alone"
