#!/usr/bin/env bash
# Gate: UnifiedUI -- the contract, the exclude list, and the three promises the design is built on.
#
# ⚠️ WHAT THIS GATE CANNOT CHECK, SAID FIRST SO ITS PASS IS NOT READ AS MORE THAN IT IS: it cannot check what
# the feature DOES to a window. That needs a window belonging to ANOTHER process, and the feature deliberately
# refuses to touch a window in its own process -- so the only instrument that can see the effect is
# _diag/ui_probe.cpp, which is not a gate (it puts a window on screen for a few seconds). The numbers it
# produced are written up in docs/rules/features.md, and they are what every claim in the feature's header
# rests on. THIS GATE COVERS THE CONTRACT AND THE SETTINGS.
#
# It runs the REAL DLL through a stub host (see _diag/feature_unifiedui_probe.cpp). No window of its own, no
# process started, no input device touched, no admin.
#
# WHY THE SOURCE SCANS ARE HERE AT ALL. Three of this feature's promises are about what it DOES NOT do, and a
# promise about an absence cannot be seen in a test result -- it has to be read out of the code:
#
#   * IT NEVER INJECTS. The whole approach is "ask DWM to restyle a window another process owns", and there is
#     no DLL pushed anywhere and no hook inside anybody else. An injected version of this feature would be a
#     different product with a different risk profile (anti-cheat, antivirus, a crash in one program taking
#     another down), so the absence is worth a machine check.
#   * IT NEVER SENDS WM_NCCALCSIZE INTO SOMEBODY ELSE'S PROGRAM, which is what SetWindowPos(SWP_FRAMECHANGED)
#     does. Measured (_diag/ui_probe.cpp): the dark flag needs no forced redraw at all -- the picture after
#     SWP_FRAMECHANGED was byte-for-byte the picture without it -- so the blunt call buys nothing and costs a
#     message a strange program did not ask for.
#   * IT WATCHES WINDOWS FROM ITS OWN THREAD, with an OUTOFCONTEXT event hook. A hook inside the input path, or
#     one installed in another process, would put this feature in the way of the thing this program exists for.
#
# ⚠️ AND EACH SCAN ALREADY KNOWS THE TWO CASES APART, because the feature's own comments DISCUSS all three
# things by name -- "SWP_FRAMECHANGED sends WM_NCCALCSIZE into the target program" is a sentence in the file.
# A scan that matched prose would fail on the very comments that record the decision, which is how the
# MediaControl gate's 0xD6 check first went wrong (see docs/rules/gates.md). So comments are stripped first,
# and two fixtures prove the strip works: one line that MUST match and one that MUST NOT.
#
# usage: test/check_feature_unifiedui.sh
set -uo pipefail
# The toolchain lives beside the project, not in the system (the same line every gate that compiles has).
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
DLL="$ROOT/build/apex/Plugins/UnifiedUI/UnifiedUI.dll"
SRC="$ROOT/features/UnifiedUI/feature_unifiedui.cpp"
PROBE_SRC="$ROOT/_diag/feature_unifiedui_probe.cpp"
PROBE="$ROOT/build/_unifiedui_probe.exe"
DIR="$ROOT/build/_unifiedui_probe"

if [ ! -f "$DLL" ]; then
  echo "FAIL: the feature is not built: $DLL"
  echo "      (a feature that was never built must not look like a feature that passed)"
  exit 1
fi
if [ ! -f "$PROBE_SRC" ]; then
  echo "FAIL: $PROBE_SRC is missing -- the feature would not be checked at all"
  exit 1
fi

# THE OTHER FEATURES' FILES, HASHED BEFORE ANYTHING RUNS. "每个插件的进程名字都是独立的，不要混用" is the user's
# rule, and it is only proven by showing the other lists are untouched.
SW_INI="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.ini"
KW_INI="$ROOT/build/apex/Plugins/KeepAwake/KeepAwake.ini"
IME_JSON="$ROOT/build/apex/Plugins/AutoIME/config.json"
hash_of() { [ -f "$1" ] && md5sum "$1" | cut -d' ' -f1 || echo "absent"; }
before_sw=$(hash_of "$SW_INI")
before_kw=$(hash_of "$KW_INI")
before_ime=$(hash_of "$IME_JSON")

echo "== building the probe =="
g++ -std=c++17 -O2 -I"$ROOT/apex" -I"$ROOT/common" -o "$PROBE" "$PROBE_SRC" || exit 1

# A CLEAN FOLDER EVERY RUN: the "the list starts empty" checks would pass for the wrong reason if a previous
# run's settings file were still sitting there.
rm -rf "$DIR"
mkdir -p "$DIR"

echo
"$PROBE" "$DLL" "$DIR/" || exit 1

echo
echo "== the list is this feature's own (the user's rule) =="
fail=0
say() { printf '  %-66s %s\n' "$1" "$2"; [ "$2" = "ok" ] || fail=1; }

if [ -f "$DIR/unifiedui.ini" ]; then
  say "its settings file is in its own folder" ok
else
  say "its settings file is in its own folder" FAIL
fi

# ⚠️ THE STATE CHECKED IS THE PROBE'S LAST ONE, ON PURPOSE: it leaves exactly one entry and saves before it
# exits, so this gate can assert an exact line instead of "something like it". A gate that reads whatever the
# last experiment happened to leave is a gate that only passes in one order.
if grep -q '^exclude=game\*$' "$DIR/unifiedui.ini" 2>/dev/null &&
   [ "$(grep -c '^exclude=' "$DIR/unifiedui.ini")" = "1" ]; then
  say "it holds exactly the one entry the probe left" ok
else
  say "it holds exactly the one entry the probe left" FAIL
fi

after_sw=$(hash_of "$SW_INI")
after_kw=$(hash_of "$KW_INI")
after_ime=$(hash_of "$IME_JSON")
if [ "$before_sw" = "$after_sw" ] && [ "$before_kw" = "$after_kw" ] && [ "$before_ime" = "$after_ime" ]; then
  say "no other feature's settings were touched" ok
else
  say "no other feature's settings were touched" FAIL
  echo "      wheel: $before_sw -> $after_sw"
  echo "      keepawake: $before_kw -> $after_kw"
  echo "      autoime: $before_ime -> $after_ime"
fi

# ⚠️ AND ITS OWN LIST MUST NOT HAVE ARRIVED FROM ANOTHER FEATURE'S. A fresh install starts empty; the only
# entry here is the one the probe typed. (`doom.exe` is the wheel feature's example name and appears in no
# file this gate writes -- the probe removes it again before the checks above.)
if [ "$(grep -c '^exclude=' "$DIR/unifiedui.ini")" = "1" ]; then
  say "no name arrived from another feature's list" ok
else
  say "no name arrived from another feature's list" FAIL
fi

# ---------------------------------------------------------------------------
echo
echo "== the three promises, read out of the source =="
#
# Comments are stripped first, because this file DISCUSSES every one of these by name (see the header). The
# two fixtures below the strip are the proof that it works -- one must match, one must not. Without them the
# scan would be a regex with a comment about how it is right.
CodeOnly() { awk '{ sub(/\/\/.*/, ""); print }' "$1"; }
CODE="$(CodeOnly "$SRC")"

# ---- the strip's own self-test ------------------------------------------------------------------------
#
# ⚠️⚠️ THE FIXTURES GO THROUGH THE SAME STRIP, NOT AROUND IT -- and the first version of this did not, which
# is why this gate failed on its own fixture the first time it ran. `FixtureMatches` piped the line straight
# into grep, so the "must NOT match" fixture -- a COMMENT that names the call, which is exactly what the
# feature's own header is full of -- matched, and the self-test declared itself broken. A self-test that does
# not exercise the real path proves nothing about the real path; it only proves the pipeline runs.
FixtureMatches() { printf '%s\n' "$1" | awk '{ sub(/\/\/.*/, ""); print }' | grep -qE "$2"; }
must_hit='SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_FRAMECHANGED);'
must_miss='// it never calls SetWindowPos(SWP_FRAMECHANGED), and that is the measurement'
if FixtureMatches "$must_hit" 'SetWindowPos' && ! FixtureMatches "$must_miss" 'SetWindowPos'; then
  say "the source scan can tell a call from a comment (its own self-test)" ok
else
  say "the source scan can tell a call from a comment (its own self-test)" FAIL
fi

ScanAbsent() { # <description> <extended regex>
  if printf '%s\n' "$CODE" | grep -qE "$2"; then
    say "$1" FAIL
    printf '%s\n' "$CODE" | grep -nE "$2" | head -3 | sed 's/^/        /'
  else
    say "$1" ok
  fi
}

# 1. NOTHING IS PUSHED INTO ANOTHER PROCESS. SetWindowsHookEx is on this list and SetWinEventHook is not: the
#    second is a machine-wide NOTIFICATION delivered to this feature's own thread, the first is a hook that
#    runs inside whatever process the events come from. The difference is the whole design.
ScanAbsent "nothing is injected into another process" \
  'WriteProcessMemory|CreateRemoteThread|VirtualAllocEx|NtCreateThreadEx|SetWindowsHookEx|QueueUserAPC'

# 2. NOTHING IS SENT INTO ANOTHER PROGRAM'S LAYOUT.
ScanAbsent "no WM_NCCALCSIZE is forced on a stranger (no SWP_FRAMECHANGED)" 'SWP_FRAMECHANGED'

# 3. IT USES ITS OWN THREAD AND AN OUT-OF-CONTEXT EVENT HOOK.
if printf '%s\n' "$CODE" | grep -q 'SetWinEventHook'; then
  say "it watches windows with SetWinEventHook" ok
else
  say "it watches windows with SetWinEventHook" FAIL
fi
if printf '%s\n' "$CODE" | grep -q 'WINEVENT_OUTOFCONTEXT'; then
  say "the hook is OUTOFCONTEXT (the callback arrives on this feature's own thread)" ok
else
  say "the hook is OUTOFCONTEXT (the callback arrives on this feature's own thread)" FAIL
fi

# 4. IT ASKS THE WINDOW WHAT IT ALREADY IS. This is what "如果原生有，就不接管明暗" is implemented with: a window
#    that already follows the system reads back the system's own value and is never written to. Without the
#    read there is no comparison, and without the comparison the feature would overwrite every program's own
#    choice -- the one thing the user asked it not to do.
if printf '%s\n' "$CODE" | grep -q 'DwmGetWindowAttribute'; then
  say "it READS a window's current light/dark before deciding (the user's rule)" ok
else
  say "it READS a window's current light/dark before deciding (the user's rule)" FAIL
fi

# 5. THE ONE IMPLEMENTATION OF "is the system light" IS THE SHARED ONE. A second registry read in the feature
#    is the copy this project's rules forbid (see common/system_theme.h).
if printf '%s\n' "$CODE" | grep -q 'RegOpenKeyEx\|RegQueryValueEx'; then
  say "it asks the shared implementation instead of reading the registry itself" FAIL
else
  say "it asks the shared implementation instead of reading the registry itself" ok
fi
if printf '%s\n' "$CODE" | grep -q 'SystemIsLightTheme'; then
  say "and it really does call it" ok
else
  say "and it really does call it" FAIL
fi

# 6. THE HOST'S SWITCH IS ACTUALLY ASKED. A feature that owns a thread and a hook keeps running whatever the
#    plugin list says, so "off" only means something if the feature asks -- and the FIRST VERSION OF THIS ONE
#    DID NOT, while its own comment claimed it did. The user found it by asking for "开关切换...有实时效果",
#    which is exactly the kind of absence a source scan can hold down.
if printf '%s\n' "$CODE" | grep -q 'featureEnabled'; then
  say "it asks the host whether the plugin list's switch is on" ok
else
  say "it asks the host whether the plugin list's switch is on" FAIL
fi

# 7. AND IT CAN PUT BACK WHAT IT CHANGED. Both live behaviours the user asked for -- the switch, and adding a
#    name to the exclude list -- are "undo what was done to the windows that are ALREADY open", and neither can
#    be worked out from the windows themselves: a program that follows the system by itself reads exactly like
#    one this feature set. So there has to be a record, and there has to be a restore.
if printf '%s\n' "$CODE" | grep -q 'RememberTouched' && printf '%s\n' "$CODE" | grep -q 'RestoreAll'; then
  say "it records what it changed, so the switch and the list can undo it at once" ok
else
  say "it records what it changed, so the switch and the list can undo it at once" FAIL
fi

# 8. IT ROUNDS THE MENUS, WHICH IS THE ONE THING IT CAN DO TO SOMEBODY ELSE'S MENU FROM OUTSIDE. Measured in
#    _diag/dark_probe.cpp with pictures: DWMWA_WINDOW_CORNER_PREFERENCE changes a #32768 menu's pixels, while a
#    material and the user32 accent change exactly zero of them. A feature that quietly lost this call would
#    look like it was working -- titles still darken, the log still fills -- and the menus would just go back
#    to being square, which nobody would notice for weeks.
if printf '%s\n' "$CODE" | grep -q 'DWMWA_WINDOW_CORNER_PREFERENCE'; then
  say "it rounds the menus (the only part of a menu reachable from outside)" ok
else
  say "it rounds the menus (the only part of a menu reachable from outside)" FAIL
fi

# 9. AND IT DOES NOT REACH FOR A MENU'S TRANSPARENCY, WHICH WAS BUILT, SHIPPED, AND THEN REMOVED -- so this
#    assertion exists to stop it from being quietly added back by someone who has not read §3.14.8. It is a
#    backwards assertion on purpose, and the project has one already ("no injection APIs").
#
#    ⚠️ THE MEASUREMENT THAT REMOVED IT: a real menu window carries WS_EX_LAYERED *and* uses
#    UpdateLayeredWindow, where the alpha is baked into the bitmap the menu submits. `GetLayeredWindowAttributes`
#    FAILS on it -- measured on renamer.exe's own menu: `layered=1 readable=0` -- and SetLayeredWindowAttributes
#    is a mutually exclusive mode, so an alpha written from outside only lands in the sliver of time before the
#    menu manager takes the window over. A material and the user32 accent change exactly ZERO pixels for a
#    different reason: both are drawn BEHIND a window, and a menu paints its own background over its whole
#    rectangle. Corners work because DWM draws them AROUND the window.
if printf '%s\n' "$CODE" | grep -qE 'SetLayeredWindowAttributes|WS_EX_LAYERED|GWL_EXSTYLE'; then
  say "it does NOT reach for a menu's transparency (measured not to work: §3.14.8)" FAIL
else
  say "it does NOT reach for a menu's transparency (measured not to work: §3.14.8)" ok
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: the contract holds, the list is its own, and the three absences are still absences"
else
  echo "FAILED: see the lines above"
fi
exit $fail
