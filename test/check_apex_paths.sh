#!/usr/bin/env bash
# Gate: WHERE Apex's files land -- the host's settings, and each feature's dll and settings.
#
# Portability is a promise about RUNTIME PATHS, so it cannot be judged by reading the source: it is what the
# OS resolves that matters. The probe is therefore built into a scratch directory, run FROM A DIFFERENT
# WORKING DIRECTORY, and every path must still come back beside the probe's own exe.
#
# ⚠️ THIS REPLACES check_app_configfile.sh, and the reason is worth keeping: that gate tested
# `config_win.cpp`, the STANDALONE app's settings-file code. After the split into Apex, that file is dead --
# the host uses apex/paths_win.cpp and a feature gets its own folder from the host's featureDir() -- so the
# old gate would have been a green light over code nothing runs. The REQUIREMENT was kept and re-pointed at
# the code that actually decides it.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

SCRATCH="$ROOT/build/_paths_gate"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"

echo "== building the probe =="
g++ -std=c++17 -O2 -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$SCRATCH/paths_probe.exe" \
  "$ROOT/_diag/apex_paths_probe.cpp" "$ROOT/apex/paths_win.cpp" || exit 1
echo "   ok"

echo
# Run from somewhere else entirely: a path built from the working directory would land THERE, and the probe
# would then report a path that is not beside its own exe. That is the check.
( cd /tmp && "$SCRATCH/paths_probe.exe" )
rc=$?

rm -rf "$SCRATCH"
exit $rc
