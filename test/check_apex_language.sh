#!/usr/bin/env bash
# Gate: the APEX HOST's own surfaces follow the user's language and the system's theme.
#
# WHAT CAN AND CANNOT BE CHECKED HERE, stated plainly because the distinction matters:
#
#   CAN:  the rules. Which language a given configuration resolves to; that the Chinese strings the tray
#         shows survive the UTF-8 -> UTF-16 conversion the wide Windows calls require; that the icon variant
#         follows the theme.
#   CANNOT: what the tray actually LOOKS like. The tray is the shell's own window -- reading it back was
#         attempted (_diag/apex_tray_state.cpp) and abandoned: its internal structure differs between Windows
#         builds, and this machine's Shell_TrayWnd is not where the documented layout says it is. A diagnostic
#         that cannot be run is worse than none, so the rule is tested and the pixels are looked at by hand.
#
# THE MOJIBAKE CHECK IS THE POINT OF THIS GATE. The Chinese strings are UTF-8 in the source; if any of them
# reaches a Windows call through an ANSI (non-W) entry point, the user sees 乱码 and nothing in the build or
# the logs says a word about it. That failure is invisible to review and obvious to a user, so it is asserted.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

echo "== building the probes =="
g++ -std=c++17 -O2 -mconsole "$ROOT/_diag/apex_tray_lang_probe.cpp" -o "$ROOT/build/_apex_traylang.exe" \
  -luser32 || exit 1
echo "   ok"

echo
"$ROOT/build/_apex_traylang.exe"
rc=$?
rm -f "$ROOT/build/_apex_traylang.exe"
exit $rc
