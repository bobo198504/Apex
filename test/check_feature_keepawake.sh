#!/usr/bin/env bash
# Gate: KeepAwake -- the one list, the two coupled switches of each row, and the exact power requests it makes.
#
# ⚠️ TWO THINGS THIS GATE IS REALLY FOR, and neither is visible by looking at the program:
#
#   1. THAT THE REQUEST IS NOT LEFT STANDING. A feature that blocks sleep and then goes away without clearing
#      it would leave the machine unable to sleep until the next reboot -- a power setting changed by a program
#      the user has closed. The probe checks the exact flags of every transition, including the cleared one.
#   2. THAT THE LIST IS PER-FEATURE. The user's rule, in their own words: "每个插件的进程名字都是独立的，不要混用".
#      So this gate also proves the list lives in KeepAwake's own file, that nothing in this run writes to
#      another feature's settings, and that a fresh install starts with ONLY the 系统全局 row rather than
#      inheriting names from the wheel feature's exclude list or from AutoIME's rules.
#
# It runs the REAL DLL (a stub host, a scratch folder under build/). No window, no input device, no admin.
#
# usage: test/check_feature_keepawake.sh
set -uo pipefail
# The toolchain lives beside the project, not in the system (the same line every gate that compiles has).
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
DLL="$ROOT/build/apex/Plugins/KeepAwake/KeepAwake.dll"
PROBE_SRC="$ROOT/_diag/feature_keepawake_probe.cpp"
PROBE="$ROOT/build/_keepawake_probe.exe"
DIR="$ROOT/build/_keepawake_probe"

if [ ! -f "$DLL" ]; then
  echo "FAIL: the feature is not built: $DLL"
  exit 1
fi
if [ ! -f "$PROBE_SRC" ]; then
  echo "FAIL: $PROBE_SRC is missing -- the feature would not be checked at all"
  exit 1
fi

# THE OTHER FEATURES' FILES, HASHED BEFORE ANYTHING RUNS. "Do not mix the lists" is only proven by showing that
# they are untouched, so the two files that hold names are compared at the end.
SW_INI="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.ini"
IME_JSON="$ROOT/build/apex/Plugins/AutoIME/config.json"
before_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo "absent" )
before_ime=$( [ -f "$IME_JSON" ] && md5sum "$IME_JSON" | cut -d' ' -f1 || echo "absent" )

echo "== building the probe =="
g++ -std=c++17 -O2 -I"$ROOT/apex" -I"$ROOT/common" -o "$PROBE" "$PROBE_SRC" || exit 1

# A CLEAN FOLDER EVERY RUN: the "defaults are empty and both switches are off" checks would pass for the wrong
# reason if a previous run's settings file were still there.
rm -rf "$DIR"
mkdir -p "$DIR"

echo
"$PROBE" "$DLL" "$DIR/" || exit 1

echo
echo "== the list is this feature's own (the user's rule) =="
fail=0
say() { printf '  %-64s %s\n' "$1" "$2"; [ "$2" = "ok" ] || fail=1; }

if [ -f "$DIR/KeepAwake.ini" ]; then
  say "its settings file is in its own folder" ok
else
  say "its settings file is in its own folder" FAIL
fi

# The names in it are the ones this run typed, and nothing else: no names copied from anywhere.
#
# ⚠️ THE STATE CHECKED HERE IS THE PROBE'S LAST ONE, ON PURPOSE -- it puts the file into a known state and saves
# it before it exits (see the end of its section 6), so this gate can assert exact lines instead of "something
# like them". A gate that reads whatever the last experiment happened to leave behind is a gate that only
# passes in one order.
if grep -q '^item=zoom\.exe|1|0$' "$DIR/KeepAwake.ini" 2>/dev/null &&
   [ "$(grep -c '^item=' "$DIR/KeepAwake.ini")" = "1" ]; then
  say "it holds exactly the one name the probe left, with its own two switches" ok
else
  say "it holds exactly the one name the probe left, with its own two switches" FAIL
fi
if grep -q '^global_awake=1$' "$DIR/KeepAwake.ini" 2>/dev/null &&
   grep -q '^global_display=0$' "$DIR/KeepAwake.ini" 2>/dev/null; then
  say "the 系统全局 row is two plain keys of its own" ok
else
  say "the 系统全局 row is two plain keys of its own" FAIL
fi
# ⚠️ AND THE QUICK-PANEL SWITCH IS ONE KEY, NOT TWO (ABI 16 -> 17). It used to be `quick_awake` and
# `quick_display`, one per master switch; the user replaced the pair with a single switch for the whole list
# ("保持唤醒插件的快速面板给一个开关"), so a file that carried the old spelling would be a second answer to a
# question that now has one. The probe leaves it ON, so both halves are checked: the key is there, and it is
# written from what was set rather than from a default.
if grep -q '^quick_panel=1$' "$DIR/KeepAwake.ini" 2>/dev/null &&
   ! grep -qE '^quick_(awake|display)=' "$DIR/KeepAwake.ini" 2>/dev/null; then
  say "the quick panel is mapped by ONE key, and the two it replaced are gone" ok
else
  say "the quick panel is mapped by ONE key, and the two it replaced are gone" FAIL
fi
# ⚠️ ONE FILE, ONE FORMAT. The first version's keys are READ (so an upgrade costs nothing) but must never be
# WRITTEN again: a file that carried both spellings would have two answers to the same question in it.
if grep -qE '^(process|listOnly|sleep|display)=' "$DIR/KeepAwake.ini" 2>/dev/null; then
  say "nothing from the older format is written back" FAIL
else
  say "nothing from the older format is written back" ok
fi
# The wheel feature's example name and AutoIME's fixture rule must not have leaked in.
if grep -qiE '^item=(doom|game)\.exe(\||$)' "$DIR/KeepAwake.ini" 2>/dev/null; then
  say "no name arrived from another feature's list" FAIL
else
  say "no name arrived from another feature's list" ok
fi

after_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo "absent" )
after_ime=$( [ -f "$IME_JSON" ] && md5sum "$IME_JSON" | cut -d' ' -f1 || echo "absent" )
if [ "$before_sw" = "$after_sw" ]; then
  say "the wheel feature's exclude list untouched" ok
else
  say "the wheel feature's exclude list untouched" FAIL
fi
if [ "$before_ime" = "$after_ime" ]; then
  say "AutoIME's rules untouched" ok
else
  say "AutoIME's rules untouched" FAIL
fi

# A FEATURE THAT WROTE NOTHING ELSE BESIDE ITSELF: the log it keeps is its own, and no file appeared in the
# wheel feature's folder during this run.
extra=$(ls "$DIR" 2>/dev/null | grep -vcE '^(KeepAwake\.ini|keepawake\.log)$' || true)
if [ "${extra:-0}" = "0" ]; then
  say "the folder holds only its own settings and log" ok
else
  say "the folder holds only its own settings and log" FAIL
fi

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: the list is not independent per feature"
  exit 1
fi
echo "OK: the strongest row wins, the switches of a row stay a legal pair, and no request is left standing"
