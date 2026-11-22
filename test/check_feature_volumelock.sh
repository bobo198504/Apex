#!/usr/bin/env bash
# Gate: VolumeLock -- ONE feature that guards a CHOSEN SET OF DEVICES, each at its own level.
#
# ⚠️⚠️ THE HARDEST PART OF GATING THIS FEATURE IS THAT ITS JOB IS TO WRITE REAL DEVICE VOLUMES, AND A GATE MUST
# NOT CHANGE THE USER'S MACHINE. Everything here is built around that one constraint:
#
#   * by default the gate runs the feature against NO device at all -- the list starts empty and nothing is
#     added -- so its whole worker runs, enumerates every endpoint, decides, and writes nothing. That proves
#     the plumbing (the DLL loads, the contract is met, the settings file is its own, the document parses)
#     WITHOUT touching a single device;
#
#   * the checks that need a REAL device (does the fader move it, is everything else really left alone) are
#     OPT-IN, with APEX_VOLUMELOCK_LIVE=1 and an explicit device name. They matter most and they must never run
#     by accident, so they are not in the default path.
#
# ⚠️ AND THE MEASUREMENT THAT MADE THIS SHAPE NECESSARY, RECORDED BECAUSE IT COST SOMETHING: an earlier
# validation ran the end-to-end probe with the matcher DELIBERATELY BROKEN to select every endpoint -- the only
# way to prove that "其它设备不动" is really being checked -- and that run set FIVE OTHER REAL DEVICES to 50%.
# Their values were recovered from the probe's own before-table and put back, but the lesson stands: A CHECK
# THAT CAN ONLY BE MEANINGFUL BY MOVING REAL DEVICES BELONGS BEHIND AN EXPLICIT OPT-IN.
#
# usage:
#   test/check_feature_volumelock.sh                          # safe: writes nothing anywhere
#   APEX_VOLUMELOCK_LIVE=1 test/check_feature_volumelock.sh     # also moves the named device's volume
#   APEX_VOLUMELOCK_DEVICE="PLATANE UP2 SE"                     # which device the live check may use
set -uo pipefail

# The toolchain lives beside the project, not in the system (the same line every gate that compiles has).
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
SRC="$ROOT/features/VolumeLock/feature_volumelock.cpp"
DLL="$ROOT/build/apex/Plugins/VolumeLock/VolumeLock.dll"
PROBE_SRC="$ROOT/_diag/feature_volumelock_probe.cpp"
# ⚠️ EVERYTHING THIS GATE BUILDS OR WRITES LIVES UNDER `build/temp/` AND IS REMOVED WHEN IT ENDS. Nothing a
# run creates belongs on the system drive (measured: a process started from this toolchain CANNOT create a
# directory under %LOCALAPPDATA%\Temp), and `build/apex/` is the PRODUCT, which a gate must not change.
TEMP_ROOT="$ROOT/build/temp"
PROBE="$TEMP_ROOT/_volumelock_probe.exe"
DIR="$TEMP_ROOT/volumelock_gate"
CleanUp() { rm -rf "$DIR" "$PROBE"; }
trap CleanUp EXIT

LIVE_DEVICE="${APEX_VOLUMELOCK_DEVICE:-PLATANE UP2 SE}"

fail=0
say() { printf '  %-72s %s\n' "$1" "$2"; [ "$2" = "ok" ] || fail=1; }

if [ ! -f "$SRC" ]; then
  echo "FAIL: $SRC is missing"
  exit 1
fi
if [ ! -f "$DLL" ]; then
  echo "FAIL: the feature is not built: $DLL"
  exit 1
fi
if [ ! -f "$PROBE_SRC" ]; then
  echo "FAIL: $PROBE_SRC is missing -- the feature would not be checked at all"
  exit 1
fi

# ---------------------------------------------------------------------------------------------
# 1. THE RULES A RUN CANNOT SHOW
# ---------------------------------------------------------------------------------------------
#
# ⚠️ EACH OF THESE IS A RULE THAT, IF BROKEN, WOULD FAIL SILENTLY AND ONLY ON SOMEONE ELSE'S MACHINE. They are
# read out of the source because the behaviour they prevent cannot be exercised here without changing the
# user's audio -- which is exactly the kind of rule that needs a machine to hold it.
echo "== the rules that a run here cannot show =="

# THE ONLY VOLUME API THIS FEATURE MAY USE. `SetMasterVolumeLevelScalar` is the mixer slider; a feature that
# reached for `SetMute`, a session volume or a hardware node would change something the user did not ask for.
if grep -q 'SetMasterVolumeLevelScalar' "$SRC"; then
  say "it sets the endpoint master volume (the mixer slider)" ok
else
  say "it sets the endpoint master volume (the mixer slider)" FAIL
fi
for forbidden in SetMute SetChannelVolumeLevelScalar ISimpleAudioVolume SetVolumeLevel; do
  if grep -q "$forbidden" "$SRC"; then
    say "it does not touch $forbidden" FAIL
  else
    say "it does not touch $forbidden" ok
  fi
done

# ⚠️ THE MATCH IS ON THE DEVICE DESCRIPTION AND IS EXACT -- MEASURED, NOT PREFERRED. The endpoint guid is unique
# but PER USB PORT (the same box elsewhere is a different id), and on this machine every endpoint's instance
# tail is port-derived, i.e. no serial number exists to identify it by. The user chose the description, and the
# wildcard language the first version had is gone with the typing it existed for.
if grep -q 'strrchr(endpointId, '"'"'\.'"'"')' "$SRC"; then
  say "it reads the device description from the registry by the endpoint's own guid" ok
else
  say "it reads the device description from the registry by the endpoint's own guid" FAIL
fi
if grep -q '_stricmp(s.rows\[i\].desc, desc)' "$SRC"; then
  say "a device is matched by its row, exactly (no wildcards to get wrong)" ok
else
  say "a device is matched by its row, exactly (no wildcards to get wrong)" FAIL
fi

# ⚠️⚠️ THE LIST STARTS EMPTY AND A DEVICE IS ONLY TOUCHED WHEN ITS ROW SAYS SO. The user: "这个设备列表默认应该
# 是空的……用户没添加到列表的设备不对进行保护（锁定）". The write must therefore be unreachable unless a row was
# found AND its protect flag is set -- a check that has to be visible in the source because the default path
# never reaches it.
if grep -q 'const DeviceRow \*row = FindRow(s, desc);' "$SRC" && grep -q 'if (row && row->protect)' "$SRC"; then
  say "a device is written to only when its own row asks for it" ok
else
  say "a device is written to only when its own row asks for it" FAIL
fi
if grep -q 'row.protect = true;' "$SRC" && grep -q 'bool protect = false;' "$SRC"; then
  say "the row's switch defaults off, and adding a device is what turns it on" ok
else
  say "the row's switch defaults off, and adding a device is what turns it on" FAIL
fi

# ⚠️ THE USER RULED OUT TYPING A DEVICE NAME: "不要让用户自己填". The add control is a dropdown of what the
# machine reports, and `listOp("add")` must REFUSE free text -- an older page sends it.
if grep -q 'if (_stricmp(op, "add") == 0)' "$SRC"; then
  say "a typed device name is refused (the user picks from a list, they do not type)" ok
else
  say "a typed device name is refused (the user picks from a list, they do not type)" FAIL
fi
if grep -q 'AddDeviceByDescription' "$SRC" && grep -q '"addDevice"' "$SRC"; then
  say "a device is added through a dropdown of what the machine reports" ok
else
  say "a device is added through a dropdown of what the machine reports" FAIL
fi

# ⚠️ EVENT DRIVEN, NOT POLLED. A poll wakes 30 times a minute to discover nothing; the registry notification is
# what makes this feature cost one sleeping thread while idle. Read from the source because "how often does it
# wake up" is invisible from outside.
if grep -q 'RegNotifyChangeKeyValue' "$SRC" && grep -q 'INFINITE' "$SRC"; then
  say "it waits on the registry's change notification, not on a timer" ok
else
  say "it waits on the registry's change notification, not on a timer" FAIL
fi
# ⚠️ THE NOTIFICATION IS ONE-SHOT AND MUST BE RE-ARMED. A version that forgot makes the feature work EXACTLY
# ONCE -- the failure that looks like success until the second time it is needed.
if grep -q 'RegCloseKey' "$SRC" && grep -q 'REG_NOTIFY_CHANGE_LAST_SET' "$SRC"; then
  say "the registry watch is re-armed after every notification" ok
else
  say "the registry watch is re-armed after every notification" FAIL
fi

# ⚠️ NO AUTOMATIC RE-ASSERT. The rule the user chose is "once, when it appears", because a feature that keeps
# pushing its value back fights the user -- so an appearance is applied on a TRANSITION, not on every pass.
if grep -q 'wasActive' "$SRC"; then
  say "an appearing device is applied on a transition, not on every pass" ok
else
  say "an appearing device is applied on a transition, not on every pass" FAIL
fi
# ⚠️⚠️ AND THE ONE EXCEPTION IS THE USER'S OWN HAND: the fader writes to the device immediately ("推子拉的时候，
# 可以直接再次更改对应设备的音量"). The distinction is WHO moved the value, and it has to be visible here --
# without it, either the fader does nothing or the feature re-asserts by itself.
if grep -q 'g_pendingValid' "$SRC" && grep -q 'pendingPercent' "$SRC"; then
  say "the fader writes to the device immediately, and only because the user moved it" ok
else
  say "the fader writes to the device immediately, and only because the user moved it" FAIL
fi

# ⚠️ THE READ-BACK. A volume write that is accepted and then ignored is indistinguishable from one that worked
# -- the same lesson MediaControl's brightness learned, where a DDC write returns before the screen moves.
if grep -q 'GetMasterVolumeLevelScalar' "$SRC" && grep -q 'read back' "$SRC"; then
  say "it reads the value back and logs what the DEVICE reports" ok
else
  say "it reads the value back and logs what the DEVICE reports" FAIL
fi

# ⚠️ THE FEATURE MUST NOT SET UP COM ON THE HOST'S MAIN THREAD. The host owns the input hook; entering an
# apartment on its thread would change the threading model of the one process that must not change.
if grep -q 'CoInitializeEx' "$SRC" && grep -q 'WorkerWithCom' "$SRC"; then
  say "COM is entered on its own worker, not on the host's thread" ok
else
  say "COM is entered on its own worker, not on the host's thread" FAIL
fi

# ⚠️⚠️ THIS CHECK WAS "this feature must not need a new ABI", AND THAT CLAIM IS NOW FALSE -- WHICH IS WHY IT IS
# WORTH KEEPING IN THIS SHAPE.
#
# The original wording was: every control this feature uses (bool, range, group/rows, select) is already in the
# contract, so a version bump would mean a copy of the ABI had been touched for no reason. That was true until
# the user asked for a dropdown WITH A CONFIRM BUTTON beside it (「列表选中设备后，点一下"添加"才生效」), which the
# contract did not have -- so ABI 22 -> 23 added `select.applyZh/applyEn`, and the Rust feature's own copy had
# to move with it.
#
# ⚠️ WHAT IS STILL WORTH ASSERTING is the part that was never about the version number: the feature must
# DECLARE the version it was built against, because the host requires an exact match and a missing constant is
# a feature that refuses to load with one line in the log.
if grep -q 'APEX_ABI_VERSION' "$SRC"; then
  say "it declares the ABI it was built against" ok
else
  say "it declares the ABI it was built against" FAIL
fi

# ---------------------------------------------------------------------------------------------
# 1b. ★ THE "RATHER WRONG THAN MISSING" RULES (用户的原则：「宁可做错，也不要放过」)
# ---------------------------------------------------------------------------------------------
echo
echo "== 宁可做错，也不要放过 (fail towards acting) =="

# ⚠️⚠️ A FAILED WRITE MUST BE RETRIED. A device that has just appeared in the registry has usually not finished
# bringing its audio stack up, so the first activation attempt can legitimately fail -- and a feature that gave
# up there would leave the box at 100% for the exact scenario it exists to prevent.
if grep -q 'kApplyAttempts' "$SRC" && grep -q 'kApplyRetryMs' "$SRC"; then
  say "a failed volume write is retried, not accepted" ok
else
  say "a failed volume write is retried, not accepted" FAIL
fi
# ⚠️ AND A FAILURE RE-ARMS: the row is left out of the seen table so the NEXT pass treats it as newly appeared.
if grep -q 'now\[nowCount - 1\].active = false' "$SRC"; then
  say "a device it could not set is re-armed for the next pass" ok
else
  say "a device it could not set is re-armed for the next pass" FAIL
fi

# ⚠️⚠️ A FAILED PASS MUST NOT BE RECORDED AS "NOTHING IS PLUGGED IN". The seen table is rebuilt each pass, so a
# pass that enumerated nothing would make every device look BRAND NEW on the next pass and re-apply the volume
# to hardware the user is listening to.
if grep -q 'bool complete = true' "$SRC" && grep -q 'if (!complete)' "$SRC"; then
  say "an incomplete enumeration is discarded, not recorded as an empty machine" ok
else
  say "an incomplete enumeration is discarded, not recorded as an empty machine" FAIL
fi

# ⚠️ AN UNARMED WATCH MUST NOT BECOME AN ETERNAL WAIT. A version that left the event allocated but unarmed and
# then waited on it with INFINITE ran exactly one pass and slept for the rest of the session.
if grep -q 'bool armed = false' "$SRC" && grep -q 'if (!armed && g_regEvent)' "$SRC"; then
  say "a watch that could not be armed falls back to polling, never to a wait for ever" ok
else
  say "a watch that could not be armed falls back to polling, never to a wait for ever" FAIL
fi
if grep -q 'timeout = g_regEvent ? INFINITE : 500' "$SRC"; then
  say "the fallback poll is 500 ms, not seconds" ok
else
  say "the fallback poll is 500 ms, not seconds" FAIL
fi
if grep -q 'could not open the MMDevices Render key for notification' "$SRC"; then
  say "it says why the watch could not be armed (a silent downgrade would be invisible)" ok
else
  say "it says why the watch could not be armed (a silent downgrade would be invisible)" FAIL
fi

# ---------------------------------------------------------------------------------------------
# 2. THE REAL DLL, AGAINST A STUB HOST, WITH AN EMPTY LIST
# ---------------------------------------------------------------------------------------------
echo
echo "== the real DLL, a stub host, and an empty list =="
echo "   (its whole worker runs -- enumeration, matching, the decision -- and writes to no device)"

echo "== building the probe =="
# ⚠️ -lole32 AND FRIENDS ARE REQUIRED, and leaving them out fails at LINK time with a wall of
# `undefined reference to __imp_CoCreateInstance`, which reads like a broken probe rather than a missing flag.
mkdir -p "$TEMP_ROOT"
g++ -std=c++17 -O2 -I"$ROOT/apex" -I"$ROOT/common" -o "$PROBE" "$PROBE_SRC" -lole32 -loleaut32 -luuid || exit 1

# A CLEAN FOLDER EVERY RUN: the "the list starts empty" check would pass for the wrong reason if a previous
# run's settings file were still there.
rm -rf "$DIR"
mkdir -p "$DIR"

PROBE_MODE="safe"
if [ "${APEX_VOLUMELOCK_LIVE:-0}" = "1" ]; then
  PROBE_MODE="live"
  echo
  echo "⚠️  LIVE MODE: this will really set the volume of \"$LIVE_DEVICE\" (the probe puts it back)."
fi

if [ "$PROBE_MODE" = "safe" ]; then
  # ⚠️⚠️ `--readonly` IS WHAT MAKES THE DEFAULT RUN HARMLESS, AND IT IS NOT COSMETIC. The probe's value is
  # that it walks the REAL user path -- add a device, move its fader, watch the endpoint follow -- and that
  # path writes a real device's volume. The gate runs on every commit, so it must not touch the user's audio;
  # what still runs here is everything that only READS: the DLL loads, the contract is checked, the settings
  # document is produced and parsed, and this machine's own device list is read (which is how the dropdown is
  # checked at all).
  out=$("$PROBE" "$DLL" "$LIVE_DEVICE" 50 "$DIR/" --readonly 2>&1)
  rc=$?
else
  out=$("$PROBE" "$DLL" "$LIVE_DEVICE" 50 "$DIR/" 2>&1)
  rc=$?
fi
echo "$out" | sed 's/^/   /'

# ⚠️ `say` MARKS A NON-"ok" SECOND ARGUMENT AS A FAILURE, so the SKIP below is printed directly rather than
# through it -- "could not measure" is not a failure of the feature, and routing it through the failure
# helper is how it ended up as red output with the right words in it.
if [ $rc -eq 3 ]; then
  printf '  %-72s %s\n' "the probe ran the feature to completion" "SKIP (device not connected)"
  echo
  echo "== THE LIVE CHECK COULD NOT RUN =="
  echo "   \"$LIVE_DEVICE\" has no active endpoint right now, so nothing could be measured."
  echo "   Plug it in and run this again -- nothing above is a statement about the feature."
  echo
  # ⚠️ AND THE CHECKS THAT NEED NO DEVICE STILL DECIDE THE RESULT: if they failed, this is still a failure.
  if [ $fail -ne 0 ]; then
    echo "FAILED: the rules that need no device do not hold"
    exit 1
  fi
  echo "OK (partly): every rule that needs no device holds; the live check was skipped"
  exit 0
fi
if [ $fail -ne 0 ]; then
  echo "FAILED: VolumeLock does not hold the rules it exists to hold"
  exit 1
fi

# ⚠️ THE FEATURE MUST LOAD AND DESCRIBE ITSELF. If it refused, the host would unload it and the panel would
# show a feature that is not there -- the failure that has no other symptom.
if echo "$out" | grep -q 'init() returned 0 (READY)'; then
  say "it reports itself ready to the host" ok
else
  say "it reports itself ready to the host" FAIL
fi
# ⚠️⚠️ THE VERSION IS READ FROM THE CONTRACT, NOT WRITTEN INTO THIS FILE. It was hard-coded as `abi=22`, and
# that turned a PASSING feature into a FAILING one the moment the ABI moved to 23 (the `select` confirm
# button) -- a gate that has to be edited every time the contract version changes is a gate that will one day
# be edited WRONG, and the failure it produces ("VolumeLock does not hold the rules it exists to hold") points
# at the feature rather than at the stale number.
#
# ⚠️ WHAT THE CHECK IS FOR is that this DLL and this host agree, so the number is taken from `apex/abi.h` --
# the one place the contract lives -- and compared against what the loaded DLL reported.
ABI_NOW=$(grep -oE 'define APEX_ABI_VERSION +[0-9]+' "$ROOT/apex/abi.h" | grep -oE '[0-9]+' | head -1)
if [ -n "$ABI_NOW" ] && echo "$out" | grep -q "abi=$ABI_NOW"; then
  say "it declares ABI $ABI_NOW, the version in the contract" ok
else
  say "it declares ABI $ABI_NOW, the version in the contract" FAIL
fi

echo
echo "== the document the page has to draw =="

# ---------------------------------------------------------------------------------------------
# 2b. THE DOCUMENT THE PAGE HAS TO DRAW
# ---------------------------------------------------------------------------------------------
#
# ⚠️⚠️ THE DOCUMENT IS PARSED, NOT MERELY GREPPED, AND IT IS THE DOCUMENT THE FEATURE JUST PRODUCED.
#
# A grep for `"layout":"rows"` proves the feature INTENDED a rows group; it does not prove the page can read
# the result. That distinction is not academic here: this document ended `"items":[},` -- the group's closing
# `]` and `}` were in the wrong places -- which a bracket count calls balanced and a parser calls broken, and
# the page would have drawn no rows at all with nothing anywhere saying why.
#
# ⚠️ SO THE PROBE PRINTS THE DOCUMENT BETWEEN MARKERS AND THIS READS IT WITH A REAL PARSER (`node`, the same
# one the other gates use). The assertions below are about the DOCUMENT, not about a description of it.
DOC="$DIR/_doc.json"
printf '%s\n' "$out" | awk '/---- DOCUMENT ----/{f=1;next} /---- END DOCUMENT ----/{f=0} f' > "$DOC"

if [ -s "$DOC" ] && node -e '
    const fs = require("fs");
    const t = fs.readFileSync(process.argv[1], "utf8");
    const d = JSON.parse(t);
    if (!d.params || !Array.isArray(d.params)) throw new Error("no params array");
    const g = d.params.find((p) => p.type === "group" && p.id === "devices");
    if (!g) throw new Error("no devices group");
    if (g.layout !== "rows") throw new Error("the devices group is not a rows layout");
    if (g.live !== true) throw new Error("the devices group is not live");
    if (!Array.isArray(g.items)) throw new Error("the devices group has no items array");
    if (g.items.length !== 0) throw new Error("a fresh install must start with an empty list");
    const fields = (g.fields || []).map((f) => f.id + ":" + f.type).join(",");
    if (fields !== "protect:bool,percent:range")
      throw new Error("the row fields are not what the page draws: " + fields);
    // ⚠️⚠️ NO SECOND ADD CONTROL ON THIS GROUP (the user, 2026-10-08): 「"从设备列表中选择"是可以填的，还有
    // 那个"添加"按钮，这两个控件没什么意思，可以去掉」.
    //
    // The leftovers were `addHint` -- the ABI text box plus Add button for a rows group -- sitting beside the
    // `select` that actually does the job. Worse than clutter: the text box INVITED a typed device name,
    // accepted the keystrokes, and then did nothing, because `listOp("add")` refuses free text by design.
    // A control that accepts input and silently discards it is not something a page should show.
    if (g.addHintZh || g.addHintEn)
      throw new Error("the group still carries a text box for adding devices");
    if (g.noAdd !== true)
      throw new Error("the group does not say noAdd, so the page will draw an append button");
    const sel = d.params.find((p) => p.type === "select");
    if (!sel) throw new Error("no add-device dropdown");
    if (!Array.isArray(sel.options) || sel.options.length === 0)
      throw new Error("the dropdown offers no device");
    // ⚠️⚠️ AND IT MUST CARRY THE CONFIRM BUTTON (apex/abi.h, ABI 22 -> 23). Without it the page sends the
    // highlighted option the moment it changes, and an option that is ALREADY highlighted can never be
    // chosen -- which is the bug the user found: 「默认在预选列表的第一个设备就加不进去」.
    if (!sel.applyZh && !sel.applyEn)
      throw new Error("the dropdown has no confirm button, so its first option cannot be chosen");
  ' "$DOC" 2>/dev/null; then
  say "the document parses: a rows list, live, empty, with devices to choose from" ok
else
  say "the document parses: a rows list, live, empty, with devices to choose from" FAIL
fi

# ⚠️ AND THE TWO SHAPES THE PAGE NEEDS ARE CONFIRMED IN THE PARSED DOCUMENT RATHER THAN IN THE SOURCE -- a
# feature can describe a control correctly and still fail to put it in the document.
if echo "$out" | grep -q 'the dropdown offers 0 candidate'; then
  say "the dropdown is populated from this machine's devices" FAIL
else
  say "the dropdown is populated from this machine's devices" ok
fi

# ---------------------------------------------------------------------------------------------
# 3. IT IS SELF-CONTAINED: ITS OWN FILE, ITS OWN LOG, AND NOBODY ELSE'S
# ---------------------------------------------------------------------------------------------
echo
echo "== the feature's own files, and nobody else's =="

# ⚠️⚠️ IN THE SAFE RUN THERE IS NO SETTINGS FILE, AND THAT IS THE CORRECT OUTCOME RATHER THAN A FAILURE.
#
# The file is written when something is CHANGED (`SaveSettings` is called from setControl and listOp and from
# nowhere else), and the read-only run changes nothing -- no device is added, no slider is moved. So the
# expectation has to follow the mode: reading must not create state, and a gate that demanded a file here
# would be demanding that a read leave something behind.
#
# ⚠️ THE LIVE RUN IS THE ONE THAT WRITES, and there the file must exist and hold exactly what was done.
if [ "$PROBE_MODE" = "safe" ]; then
  if [ -f "$DIR/VolumeLock.ini" ]; then
    say "reading the feature wrote no settings file (a read leaves nothing behind)" FAIL
  else
    say "reading the feature wrote no settings file (a read leaves nothing behind)" ok
  fi
  # ⚠️ THE LOG IS THE EXCEPTION AND IT IS MEANT TO BE: it is created by `init` itself (`OpenLog`), so it exists
  # after any successful load -- empty, because nothing has happened yet. An empty log here is not a missing
  # one; it is the accurate record of a run that did nothing.
  if [ -f "$DIR/volumelock.log" ]; then
    say "its log exists from the moment it loads (and is empty -- nothing happened)" ok
  else
    say "its log exists from the moment it loads (and is empty -- nothing happened)" FAIL
  fi
else
  if [ -f "$DIR/VolumeLock.ini" ]; then
    say "its settings file is in its own folder" ok
  else
    say "its settings file is in its own folder" FAIL
  fi
  if [ -f "$DIR/volumelock.log" ]; then
    say "its log is its own file, beside its own settings" ok
  else
    say "its log is its own file, beside its own settings" FAIL
  fi
fi

# ⚠️⚠️ AN EMPTY LIST IS THE SHIPPING STATE -- no rows at all, not "a row that happens to be switched off".
# The user: "这个设备列表默认应该是空的……用户没添加到列表的设备不对进行保护".
#
# ⚠️ AND THE CHECK READS THE FEATURE'S OWN DOCUMENT RATHER THAN A FILE, because in the safe run there is no
# file to read: whether the list is empty is a property of the FEATURE, and the document is where it says so.
if echo "$out" | grep -q '"id":"devices"' && echo "$out" | grep -q '"items":\[\]'; then
  say "the protected list starts empty, so nothing is guarded by default" ok
else
  say "the protected list starts empty, so nothing is guarded by default" FAIL
fi

# ⚠️ NOTHING IT WROTE CAME FROM ANOTHER FEATURE'S LIST. The wheel feature's exclude list is the other place a
# name lives, and this run must not have read or written it.
SW_INI="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.ini"
if [ -f "$SW_INI" ] && grep -qiE '^(PLATANE|VolumeLock)' "$SW_INI" 2>/dev/null; then
  say "no device name leaked into the wheel feature's list" FAIL
else
  say "no device name leaked into the wheel feature's list" ok
fi

# ---------------------------------------------------------------------------------------------
# 4. THE OPT-IN LIVE CHECK
# ---------------------------------------------------------------------------------------------
echo
if [ "$PROBE_MODE" = "live" ]; then
  echo "== LIVE: the fader moves the device, and nothing else does =="
  if echo "$out" | grep -q 'OK: the fader really moved the device'; then
    say "the fader really moved the device (read back off the endpoint)" ok
  else
    say "the fader really moved the device (read back off the endpoint)" FAIL
  fi
  if echo "$out" | grep -q 'other endpoint(s) compared: all unchanged'; then
    say "every OTHER endpoint was left exactly as it was" ok
  else
    say "every OTHER endpoint was left exactly as it was" FAIL
  fi
  if echo "$out" | grep -q 'is among them: yes'; then
    say "the named device is offered by the dropdown, so it can be added" ok
  else
    say "the named device is offered by the dropdown, so it can be added" FAIL
  fi
else
  echo "== LIVE CHECK SKIPPED =="
  echo "   The things that cannot be checked without moving a real device: that the fader really"
  echo "   changes the level, and that every other endpoint is really left alone."
  echo "   Run it deliberately, with the device named:"
  echo "     APEX_VOLUMELOCK_LIVE=1 APEX_VOLUMELOCK_DEVICE='PLATANE UP2 SE' test/check_feature_volumelock.sh"
  echo "   ⚠️ It moves that device's volume and puts it back afterwards."
fi

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: VolumeLock does not hold the rules it exists to hold"
  exit 1
fi
echo "OK: a chosen list of devices, each at its own level, moved only by the user's hand -- and nothing else touched"
