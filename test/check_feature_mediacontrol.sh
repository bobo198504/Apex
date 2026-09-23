#!/usr/bin/env bash
# Gate: MediaControl -- the per-monitor brightness rows, the screen-off WINDOW, the per-application volume rows,
# and the two NAMED groups it hands the quick panel.
#
# ⚠️ FOUR THINGS THIS GATE IS REALLY FOR, and none of them can be read off the source:
#
#   1. THAT A DARK SCREEN IS A WINDOW RATHER THAN A DISPLAY CHANGE. That is the user's whole requirement
#      ("不是断开，也不要锁屏"): the screen stays connected, the desktop does not lock, and the way back is a
#      window being destroyed. The probe asks the WINDOW MANAGER -- class, rectangle, visibility, topmost -- and
#      the same check is what proves it lands on a real monitor rather than on a remembered coordinate.
#   2. THAT THE GLOBAL SHORTCUT IS REALLY TAKEN. `RegisterHotKey` is exclusive, so the probe registers the same
#      combination itself: it must FAIL while the feature holds it and SUCCEED once it is cleared. A shortcut
#      that silently never registered looks exactly like one nobody pressed.
#   3. THAT A SCREEN'S ON/OFF STATE IS NOT WRITTEN TO DISK. A gamma ramp and a black window belong to the
#      running program; a program that restored "screen 2 was off" at the next start would boot to a black
#      screen. The check is the exact SHAPE of a `display=` line -- device, level, shortcut, and no third field.
#   4. THAT THE TWO GROUPS REACH THE FLYOUT NAMED, AND THAT SCREEN-OFF DOES NOT. "分组名称为「亮度」" and
#      "熄屏不用" are the user's own instructions, and the host can only draw what an item says it belongs to
#      (apex/abi.h: `groupZh`/`groupEn`).
#
# ⚠️ IT DARKENS A SCREEN FOR A MOMENT, DELIBERATELY, AND IT IS THE ONLY PART WITH A WAY OUT: `APEX_MC_NO_DARK=1`
# skips it. Everything else here starts no program other than the probe and touches no input device.
#
# usage: test/check_feature_mediacontrol.sh
set -uo pipefail
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
DLL="$ROOT/build/apex/Plugins/MediaControl/MediaControl.dll"
PROBE_SRC="$ROOT/_diag/feature_mediacontrol_probe.cpp"
PROBE="$ROOT/build/_mediacontrol_probe.exe"
DIR="$ROOT/build/_mediacontrol_probe"
SRC="$ROOT/features/MediaControl/feature_mediacontrol.cpp"

if [ ! -f "$DLL" ]; then
  echo "FAIL: the feature is not built: $DLL"
  exit 1
fi
if [ ! -f "$PROBE_SRC" ]; then
  echo "FAIL: $PROBE_SRC is missing -- the feature would not be checked at all"
  exit 1
fi

# THE OTHER FEATURES' FILES, HASHED BEFORE ANYTHING RUNS. Each feature owns its own settings (the user's rule:
# "每个插件的进程名字都是独立的，不要混用"), so the proof is that this run does not touch theirs.
SW_INI="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.ini"
KA_INI="$ROOT/build/apex/Plugins/KeepAwake/KeepAwake.ini"
before_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo "absent" )
before_ka=$( [ -f "$KA_INI" ] && md5sum "$KA_INI" | cut -d' ' -f1 || echo "absent" )

echo "== building the probe =="
# -lole32/-luuid for the silent render stream the probe opens (see OwnSound in it: without one, a quiet machine
# has no application rows at all and the volume half would be checked against an empty list).
# -lgdi32 for the two screen checks: the gamma ramp is read and written through a display DC.
g++ -std=c++17 -O2 -I"$ROOT/apex" -I"$ROOT/common" -o "$PROBE" "$PROBE_SRC" -lole32 -luuid -luser32 -lgdi32 || exit 1

# A CLEAN FOLDER EVERY RUN: "the defaults are empty and nothing is mapped" would pass for the wrong reason if a
# previous run's settings file were still there.
rm -rf "$DIR"
mkdir -p "$DIR"

echo
# ⚠️ THE PROBE'S OUTPUT IS KEPT, NOT JUST PRINTED, AND THAT IS BECAUSE OF WHAT A SUITE RUN DOES TO IT.
# `run_all.sh` shows the last six lines of a failed gate, and this gate's last lines are its calmest ones
# (section 10, "nothing left behind"); a failure in section 5 or 6 is scrolled off the top and the reader gets a
# red gate with no statement about what was wrong. The file is written on every run, so a red one can be read
# afterwards -- the same reason the quick-panel gate keeps its PNGs.
PROBE_LOG="$ROOT/build/_mediacontrol_probe.log"
"$PROBE" "$DLL" "$DIR/" 2>&1 | tee "$PROBE_LOG"
probe_rc=${PIPESTATUS[0]}
if [ "$probe_rc" -ne 0 ]; then
  echo
  echo "   (the probe's whole output is kept in $PROBE_LOG)"
  exit 1
fi

echo
echo "== the pieces that must not drift =="
fail=0
say() { printf '  %-64s %s\n' "$1" "$2"; [ "$2" = "ok" ] || fail=1; }

# ⚠️ THE WINDOW CLASS IS ONE STRING IN TWO FILES, AND THE PROBE CANNOT FIND THE WINDOW WITHOUT IT. The probe
# checks the feature by looking at a real window; if the name changed on one side only, the probe would look
# for a window that no longer exists and report a missing screen -- a confusing failure for a one-word rename.
# This is the cheap half of that: the two literals have to be the same string.
kf=$(grep -o '"ApexMediaControlOff"' "$SRC" | head -1)
kp=$(grep -o '"ApexMediaControlOff"' "$PROBE_SRC" | head -1)
if [ -n "$kf" ] && [ "$kf" = "$kp" ]; then
  say "the screen-off window class is the same string in the feature and in the probe" ok
else
  say "the screen-off window class is the same string in the feature and in the probe" FAIL
fi

# The gamma ramp is put back on the way out (a ramp outlives the process that set it), and WHAT GOES BACK IS THE
# ORIGINAL -- not the normalised baseline this feature works from (see ApplyGamma): putting the working value back
# would leave the screen brighter than the user had it. Read from the source as well as observed, because "it
# happened in this one run" is not the same claim as "it always happens".
if grep -q 'SetDeviceGammaRamp(d.gammaDc, d.gammaOriginal)' "$SRC"; then
  say "the gamma baseline is restored on the way out, not left scaled" ok
else
  say "the gamma baseline is restored on the way out, not left scaled" FAIL
fi

# ⚠️⚠️ "SCREEN OFF" IS A BLACK WINDOW, AND THERE IS NO LONGER A REAL POWER-OFF BEHIND A SWITCH. There was one, and
# the user took it out after living with it: "熄屏用显示器电源这个方案也去掉，这样会变成断开显示器。统一采用现在的方案."
# The reason is what the switch had been documented to do all along -- DDC/CI standby drops the display link far
# enough for Windows to see a hot-plug ("会熄屏，但有点像断开了，然后马上又连回来"), which is exactly what the
# requirement rules out. So this is now a check for the ABSENCE of the path: a future edit that reaches for VCP 0xD6
# again -- however reasonable it looks -- has to come back through the user first.
#
# ⚠️ AND LIKE THE SCAN BELOW, IT MUST TELL A CALL FROM PROSE: the feature's own comments still DISCUSS 0xD6 (that is
# where the reason for its removal is written down), so the pattern is the call's own shape -- a handle argument
# followed by the control number -- and it has its own two fixtures.
POWER_OFF_RE='phys[[:space:]]*,[[:space:]]*0xD6'
PWRFIX=$(mktemp)
printf '  const BOOL ok = SetVCPFeature(phys, 0xD6, v);\n' > "$PWRFIX"
pn_hit=$(grep -cE "$POWER_OFF_RE" "$PWRFIX")
printf '// VCP 0xD6 would turn the backlight off, and that path was removed on purpose\n' > "$PWRFIX"
pn_miss=$(grep -cE "$POWER_OFF_RE" "$PWRFIX")
rm -f "$PWRFIX"
if [ "$pn_hit" != "1" ] || [ "$pn_miss" != "0" ]; then
  echo "  FAIL: this scan cannot tell a real call from the same name in prose (call=$pn_hit prose=$pn_miss)"
  fail=1
fi
if grep -qE "$POWER_OFF_RE" "$SRC"; then
  say "screen-off does NOT touch the monitor's own power mode (a real power-off disconnects the screen)" FAIL
else
  say "screen-off does NOT touch the monitor's own power mode (a real power-off disconnects the screen)" ok
fi

# ... and the black window is the only way a screen goes off, which is what the settings-file checks below and the
# whole of section 5 of the probe are about.

# ⚠️ AND A DEVICE'S OWN NAME BOX SITS WHERE ITS LABEL WOULD, WITH NO LABEL OF ITS OWN -- the user's arrangement:
# "把它放在每个设备的「亮度」「音量」位置就很合适，那个「名字」提示的也可以去掉". The page skips an empty label
# (textRow), so the two halves have to agree: a feature that stopped sending the empty string would get a bare
# "名字" back in the row it asked to be quiet.
if grep -q 'AddTextField(out, outSize, off, "alias", "", ""' "$SRC"; then
  say "the name box carries no label of its own (it sits where the label was)" ok
else
  say "the name box carries no label of its own (it sits where the label was)" FAIL
fi

# ⚠️ AND NEITHER DOES THE SHORTCUT BOX, for the same reason and at the user's request: "每个设备后面有个「快捷键」的
# 文字去掉". That one is checked by the PROBE, on the document the feature really sends
# (section 2: "and the shortcut field asks for no label of its own") -- a source scan cannot see it here, because
# the empty string is emitted by a separate call rather than written as a literal next to the field's name.

# ⚠️⚠️ AND THE RAMP IT FINDS IS NORMALISED TO FULL SCALE BEFORE IT IS USED -- that is what makes the slider an
# ABSOLUTE brightness rather than a percentage of whatever another dimming tool left behind, which is the user's
# "能做成统一控制?" A run of this gate cannot prove it on a machine where nothing else is dimming the screen (the
# normalisation only shows up against a dimmed ramp), so it is read from the source and named exactly.
if grep -q 'normalising it to full scale' "$SRC" && grep -q 'd.gammaOriginal' "$SRC"; then
  say "the ramp it finds is normalised to full scale (the slider is an absolute brightness)" ok
else
  say "the ramp it finds is normalised to full scale (the slider is an absolute brightness)" FAIL
fi

# ⚠️⚠️ THE WMI CALL MUST BE ADDRESSED WITH AN OBJECT PATH, NOT WITH THE INSTANCE'S KEY. This is the bug that took
# a second deploy to find, and it is the kind that a gate can hold down: the key value ("DISPLAY\SDC4190\5&...")
# looks exactly like an address, and `ExecMethod` answers `WBEM_E_INVALID_PARAMETER` -- a parameter error for an
# addressing mistake, which sends the reader looking at the arguments. `_diag/wmi_set_probe.cpp` is how it was
# found (four spellings, one of which works). So: the path has to come from WMI's own `__RELPATH`/`__PATH`.
wmi_path_hit=$(grep -c '__RELPATH\|__PATH' "$SRC")
wmi_call_hit=$(grep -c 'ExecMethod(objPath' "$SRC")
if [ "$wmi_path_hit" -ge 1 ] && [ "$wmi_call_hit" -ge 1 ]; then
  say "the WMI instance is addressed by __RELPATH/__PATH, not by its key value" ok
else
  say "the WMI instance is addressed by __RELPATH/__PATH, not by its key value" FAIL
fi

# ⚠️ AND IT DOES NOT TOUCH THE LIVE SCREEN'S POWER STATE. The user asked for "不是断开，也不要锁屏", and the
# tempting shortcuts for a per-monitor off are all in these calls: a display-mode change (which would disconnect
# the monitor) and the system-wide monitor power command (which turns EVERY screen off and starts the system's
# own idle timer). Neither may appear in this feature at all.
#
# ⚠️⚠️ AND THE PATTERN HAS TO TELL A CALL FROM A COMMENT, BECAUSE THE FILE TALKS ABOUT BOTH. The feature's own
# header explains WHY these two are not used -- so both names appear in its prose, and the first version of this
# check (a plain grep for the words) failed on the documentation it was written next to. This project has made
# exactly this mistake before in check_apex_quickpanel.sh, and the cure is the same: a pattern that requires the
# call's own shape (an identifier followed by `(`, or the constant standing as an argument), and a FIXTURE that
# must hit and one that must not, checked before the real file is read.
POWER_RE='ChangeDisplaySettings[A-Za-z]*[[:space:]]*\(|SC_MONITORPOWER[[:space:]]*,|,[[:space:]]*SC_MONITORPOWER'
SELFTEST=$(mktemp)
printf '  ChangeDisplaySettingsExA(dev, &dm, NULL, CDS_UPDATEREGISTRY, NULL);\n' > "$SELFTEST"
power_hit=$(grep -cE "$POWER_RE" "$SELFTEST")
printf '// there is no ChangeDisplaySettings call here, and SC_MONITORPOWER is only a word in this sentence\n' > "$SELFTEST"
power_miss=$(grep -cE "$POWER_RE" "$SELFTEST")
rm -f "$SELFTEST"
if [ "$power_hit" != "1" ] || [ "$power_miss" != "0" ]; then
  echo "  FAIL: this scan cannot tell a real call from the same name in prose (call=$power_hit prose=$power_miss)"
  fail=1
fi
if grep -qE "$POWER_RE" "$SRC"; then
  say "no display-mode change and no system-wide monitor power in the feature" FAIL
else
  say "no display-mode change and no system-wide monitor power in the feature" ok
fi

# The settings file is this feature's own, and the on/off state is not in it (the probe checks the shape; this
# checks the file exists at all and that no other feature's file moved).
if [ -f "$DIR/MediaControl.ini" ]; then
  say "its settings file is in its own folder" ok
else
  say "its settings file is in its own folder" FAIL
fi
if grep -q '^display=' "$DIR/MediaControl.ini" 2>/dev/null; then
  say "it writes one line per monitor, keyed on the GDI device name" ok
else
  say "it writes one line per monitor, keyed on the GDI device name" FAIL
fi
if grep -qE '^off=' "$DIR/MediaControl.ini" 2>/dev/null; then
  say "and never the screen's on/off state (that belongs to the running program)" FAIL
else
  say "and never the screen's on/off state (that belongs to the running program)" ok
fi

after_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo "absent" )
after_ka=$( [ -f "$KA_INI" ] && md5sum "$KA_INI" | cut -d' ' -f1 || echo "absent" )
if [ "$before_sw" = "$after_sw" ]; then
  say "the wheel feature's exclude list untouched" ok
else
  say "the wheel feature's exclude list untouched" FAIL
fi
if [ "$before_ka" = "$after_ka" ]; then
  say "KeepAwake's own list untouched" ok
else
  say "KeepAwake's own list untouched" FAIL
fi

# A FEATURE THAT WROTE NOTHING ELSE BESIDE ITSELF.
extra=$(ls "$DIR" 2>/dev/null | grep -vcE '^(MediaControl\.ini|mediacontrol\.log)$' || true)
if [ "${extra:-0}" = "0" ]; then
  say "the folder holds only its own settings and log" ok
else
  say "the folder holds only its own settings and log" FAIL
fi

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: MediaControl's own files, class name or display handling drifted"
  exit 1
fi
echo "OK: a screen goes dark as a WINDOW (connected, unlocked, undoable), the shortcut is really taken, and the"
echo "    two named groups reach the quick panel without screen-off"
