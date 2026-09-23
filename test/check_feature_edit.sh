#!/usr/bin/env bash
# Gate: THE EDIT / SAVE MODEL, DRIVEN THROUGH THE REAL PROGRAM.
#
# ⚠️ THIS GATE EXISTS BECAUSE TWO PROBES PASSED WHILE THE FEATURE DID NOT WORK.
#
# `_diag/apex_panel_probe.js` and `_diag/apex_edit_probe.js` both run the page against a DOM stub, and both
# passed every check while the user reported -- twice -- that edit/save did nothing. A stub has no host, no
# feature and no messages between them, and the fault was in exactly that gap: the host stamped every reply to
# the panel with the wrong message code (`SendDocument` hardcoded kIpcAnswer instead of `what`), so the live
# readout arrived looking like a control document, the page's `JSON.parse` threw, and the page was never told
# that anything had happened.
#
# Nothing in the page could have caught that, and nothing in the feature could either. This gate closes it: it
# speaks the REAL protocol to a REAL host with the REAL feature loaded, and reads what comes back.
#
# ⚠️ IT RUNS FROM A SCRATCH COPY, WITH ITS OWN CONFIG. The probe writes -- editing a rule and committing is the
# only way to prove the save reaches the FILE -- and the first version of it was pointed at the user's live
# folder, where it left a mangled rule behind and had its restore overwritten by the running host's own copy of
# the settings. Same rule as everywhere else in this project: A DIAGNOSTIC RUNS FROM A COPY.
#
# ⚠️ AND THE CONFIG IT EDITS IS ITS OWN FIXTURE, not a copy of the user's rules: this gate checks the mechanism,
# and the mechanism does not depend on which rules are in the file. (The feature's own tests are what use the
# user's real rules as a fixture -- see check_feature_ime.)
#
# usage: test/check_feature_edit.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # the field-taking helpers, and why the identity is a window class

BUILD="$ROOT/build"
RUN="$BUILD/_abi_run"
PROBE="$BUILD/_editabi.exe"
fail=0

echo "== building the probe and taking the field =="
g++ -std=c++17 -O2 -mconsole -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$PROBE" \
    "$ROOT/_diag/apex_edit_abi_probe.cpp" -luser32 || { echo "   FAIL: the probe did not build"; exit 1; }
echo "   ok"

# ⚠️ THE MACHINE ALLOWS ONE APEX (a product rule -- see main.cpp), so this gate takes the field like every other
# gate that starts a host, and gives it back at the end. The probe itself never starts anything: it talks to the
# host this script started.
cleanup() {
  apex_kill_own '_abi_run'
  apex_restore_the_field
}
trap cleanup EXIT
apex_take_the_field

echo
echo "== a private copy with its own rules =="
rm -rf "$RUN"
cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "   FAIL: the built folder could not be copied"; exit 1; }
printf '# written by check_feature_edit.sh\n' > "$RUN/apex.ini"
# A config with TWO rules, in the shape the feature reads. ASCII on purpose: this is a fixture, and a fixture
# that needed care with encodings would be testing the shell rather than the program.
#
# ⚠️ THE SECOND RULE IS AT PRIORITY 200, AND IT IS THERE TO BE IN THE WAY. A new rule is created at priority
# 100 (the feature's own default), and the rules are shown sorted by priority then name -- so with a 200 in the
# list, a brand-new rule does NOT land last. That is the arrangement in which the page's "the new row is the
# last one" guess is wrong, and the guess is what decides which rule the user then edits. A fixture where the
# new rule happens to sort last would pass while the bug was live.
cat > "$RUN/Plugins/AutoIME/config.json" <<'JSONEOF'
{
  "ime_toggle_hotkey": "Ctrl+Space",
  "switch_method": "ime",
  "rules": [
    {
      "id": "fixture1",
      "name": "fixture.exe / One",
      "enabled": true,
      "priority": 100,
      "match_target": "active_control",
      "match_mode": "wildcard",
      "use_process": true,
      "use_window_title": false,
      "use_window_class": false,
      "use_control_text": false,
      "use_control_class": false,
      "use_control_type": false,
      "use_automation_id": false,
      "use_container_text": false,
      "use_ancestor_class": false,
      "process_pattern": "fixture.exe",
      "window_title_pattern": "",
      "window_class_pattern": "",
      "control_text_pattern": "",
      "control_class_pattern": "",
      "control_type_pattern": "",
      "automation_id_pattern": "",
      "container_text_pattern": "",
      "ancestor_class_pattern": "",
      "action": "chinese"
    },
    {
      "id": "fixture2",
      "name": "fixture.exe / Two",
      "enabled": true,
      "priority": 200,
      "match_target": "active_control",
      "match_mode": "wildcard",
      "use_process": true,
      "use_window_title": false,
      "use_window_class": false,
      "use_control_text": false,
      "use_control_class": false,
      "use_control_type": false,
      "use_automation_id": false,
      "use_container_text": false,
      "use_ancestor_class": false,
      "process_pattern": "fixture.exe",
      "window_title_pattern": "",
      "window_class_pattern": "",
      "control_text_pattern": "",
      "control_class_pattern": "",
      "control_type_pattern": "",
      "automation_id_pattern": "",
      "container_text_pattern": "",
      "ancestor_class_pattern": "",
      "action": "english"
    }
  ]
}
JSONEOF
rm -f "$RUN"/apex*.log
echo "   $RUN (two rules: One at 100, Two at 200 -- so a new rule does NOT sort last)"

echo
echo "== starting that copy =="
( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
tries=0
while [ $tries -lt 40 ]; do
  [ -n "$(apex_own_pids '_abi_run')" ] && break
  sleep 0.2
  tries=$((tries + 1))
done
if [ -z "$(apex_own_pids '_abi_run')" ]; then
  echo "   FAIL: the copy did not start"
  exit 1
fi
echo "   up (pid $(apex_own_pids '_abi_run' | head -1))"

# The host loads features after its window is up; give it a moment to report the rule count in its log.
sleep 2
echo "   its log says: $(grep -o 'AutoIME: [0-9]* rule(s) loaded' "$RUN/apex.log" 2>/dev/null | tail -1)"

echo
echo "== driving begin-edit / setControl / commit-edit through the real IPC =="
out=$("$PROBE" "$(cygpath -w "$RUN" 2>/dev/null || echo "$RUN")" 0 2>&1)
rc=$?
printf '%s\n' "$out" | sed 's/^/   /'

if [ $rc -ne 0 ]; then
  echo
  echo "   FAIL: the probe could not drive the program (exit $rc)"
  fail=1
fi

# The two properties that matter, read from the probe's own output rather than re-derived here.
if printf '%s' "$out" | grep -q "contains PROBE-EDITED: YES"; then
  echo
  echo "   ok  the edit reached the rule, and the rule reached the FILE"
else
  echo
  echo "   FAIL: the save did not reach the file"
  echo "        (the edit is committed in memory but never written -- the user's rule would be lost on exit)"
  fail=1
fi
if printf '%s' "$out" | grep -q "PROBE-EDITED"; then
  echo "   ok  and the document shows it while it is being edited"
else
  echo "   FAIL: the document never showed the edit (the page would draw the old value)"
  fail=1
fi
# ⚠️⚠️ AND THE DOCUMENT SAYS WHETHER AN ACTION IS STILL WAITING (apex/abi.h, `waiting`; ABI 10 -> 11).
#
# This field is the page's ONLY way to know that a capture has finished: it polls while the feature says it is
# waiting and stops when it stops saying so. The user's report -- "捕获事件进行时，鼠标点击后结果要马上给到参数页，
# 目前没有，要等到点击新建的规则条才会出现" -- was the page having guessed the same thing from "the document
# changed", which ends the wait at arming time. If the field ever disappears from the document, that comes back.
if printf '%s' "$out" | grep -q "THE WAIT FLAG: in the document: YES"; then
  echo "   ok  and it reports whether an action is still waiting (the page's poll turns on this)"
else
  echo "   FAIL: the document has no 'waiting' field -- a capture's result could not reach the page by itself"
  fail=1
fi

# ⚠️⚠️ ADD AND REMOVE, WHICH IS WHERE THE USER SAID IT WAS BROKEN. The report was "规则添加、编辑、保存功能都不完善",
# and "add does nothing" is true in three different places: the row never appears, it appears but is not saved, or
# it is saved on the wrong row. The probe measures all three (see its section 8+), so the gate can be specific.
echo
echo "== add and remove: the row, and the file =="
if printf '%s' "$out" | grep -q "ADD: in memory: YES"; then
  echo "   ok  a new rule appears in the document"
else
  echo "   FAIL: add produced no row at all"
  fail=1
fi
# ⚠️⚠️ THE UNSAVED NEW RULE MUST NOT BE IN THE FILE, AND THIS CHECK USED TO SAY THE OPPOSITE.
#
# The user's rule: "新建规则后，没点保存，切到其它规则后不保留" -- answered with "没点保存就切走 = 等于没建（不保留）".
# So a rule that was just created lives in memory until Save: walking away from it leaves nothing behind, and no
# empty 新规则 can appear in config.json.
#
# ⚠️ THE OLD CHECK DEMANDED THE ROW REACH config.json WITHIN THE DEBOUNCE, and it was not wrong when it was
# written -- it caught a new rule that vanished on restart because nothing ever wrote it. What changed is the
# MODEL, not that bug: the row is still created the moment the user asks for it; it is simply not PUBLISHED until
# Save. The next check is the other half, so the pair still pins "a new rule can be made, and can be kept".
if printf '%s' "$out" | grep -q "ADD: reached the FILE: NO"; then
  echo "   ok  and it is NOT in config.json while it is unsaved (not saved = not created)"
else
  echo "   FAIL: the unsaved new rule was written to config.json -- abandoning it would leave an empty row behind"
  fail=1
fi
if printf '%s' "$out" | grep -q "SAVE: reached the FILE: YES"; then
  echo "   ok  and Save is what writes it"
else
  echo "   FAIL: saving a new rule does not persist it"
  printf '%s\n' "$out" | grep -E "the file now has|SAVE: reached" | sed 's/^ */        /'
  fail=1
fi
# ⚠️⚠️ AND THE EDITOR MUST BE ON THE ROW THAT WAS CREATED. This is the defect the user reported as
# "规则添加、编辑、保存功能都不完善": the page selected the LAST row and asked the feature to edit it, while the
# feature had put the new rule wherever it sorts (priority, then name). The user then typed into a rule they
# never chose, and Save wrote their fields onto it. The fixture has a rule at priority 200 precisely so that the
# new rule (priority 100) does NOT sort last -- without that, a wrong guess and a right answer look the same.
if printf '%s' "$out" | grep -q "ADD: the editor is on the new row: YES"; then
  echo "   ok  and the editor opens on the row the new rule really is"
  printf '%s\n' "$out" | grep "the page's old guess" | sed 's/^ */        /'
else
  echo "   FAIL: the editor is not on the new rule's row -- the user would be editing a different rule"
  printf '%s\n' "$out" | grep -E "ADD: the editor|the added row is at|THE EDITOR IS ON THE WRONG ROW" | sed 's/^ */        /'
  fail=1
fi
if printf '%s' "$out" | grep -q "REMOVE: reached the FILE: YES"; then
  echo "   ok  and removing one is written too"
else
  echo "   FAIL: a deleted rule comes back on the next start"
  fail=1
fi
# ⚠️ REORDERING IS BY DRAGGING NOW ("规则顺序可以直接拖动调整顺序，上下按钮去掉"), so the op a drag sends has to
# work through the real host and feature -- and it carries TWO numbers (where the row was, where it was dropped),
# which is exactly the kind of pair that gets swapped without anybody noticing.
if printf '%s' "$out" | grep -q "MOVE: the row went where it was dropped: YES"; then
  echo "   ok  and a dragged row lands where it was dropped"
else
  echo "   FAIL: dragging a row does not move it to the drop position"
  printf '%s\n' "$out" | grep -E "MOVE:|asked to move" | sed 's/^ */        /'
  fail=1
fi
if printf '%s' "$out" | grep -q "MOVE: the editor followed the row: YES"; then
  echo "   ok  and an open edit follows the row it belongs to"
else
  echo "   FAIL: the editor stayed behind -- typing would go into a different rule"
  fail=1
fi
if printf '%s' "$out" | grep -q "REMOVE: nothing is left open for editing: YES"; then
  echo "   ok  and deleting the row leaves no editor open on it"
else
  echo "   FAIL: a draft survived the row it belonged to"
  fail=1
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: begin / edit / commit work end to end, and the change is on disk"
else
  echo "FAILED: the edit model does not survive the round trip"
fi
exit $fail
