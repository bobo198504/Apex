#!/usr/bin/env bash
# Every gate in this project, in TWO LAYERS.
#
# ⚠️ THE SPLIT IS THE USER'S RULE, AND IT EXISTS BECAUSE THE OLD SUITE WAS BOTH SLOW AND RUDE.
#
# The user's words: "缩减没必要的反复测试流程，一切UI交互测试交给我，但不是所有，只限部署后要交给用户交互的部分，
# 数据模型验证可以由你在构建时就要做好，不要拿到部署后来验证，太浪费时间资源" -- and, about the shape of the
# whole loop: "一次性开发多个功能，不用每个小功能都部署一遍 ... 太浪费资源时间".
#
# So the suite is two layers with different jobs and very different costs:
#
#   THE BUILD LAYER (default, a few seconds). Pure logic and compiled artefacts. No window opens, no process
#   is started, nothing is written outside build/, and no input device is touched. This is what runs after
#   every edit, and it is where the model, the settings, the rules and the boundaries are verified --
#   "数据模型验证由你在构建时做好", not after a deploy.
#
#   THE ASSEMBLY LAYER (--assembly). The gates that start the real program. They hold the machine's only Apex
#   for the duration, and they are the only way to answer "does the assembled program work" -- a DELIVERY
#   question, not an editing-loop question. Run them once when a batch of work is finished, before a deploy.
#
# ⚠️ THE ASSEMBLY LAYER TAKES THE FIELD ONCE, FOR ALL OF ITS GATES, AND THE REASON WAS MEASURED:
#
#   * BEFORE (each gate took and gave it back on its own): the user's own Apex was stopped and restarted
#     EIGHT times inside one suite run -- eight tray icons appearing and vanishing while they were working.
#   * NOW: one stop, all the gates, one restore at the end, through a trap so it happens on every exit path,
#     including a failure or a Ctrl-C.
#
#   It also cannot be otherwise any more: one Apex per machine is a PRODUCT rule (main.cpp), so a gate that
#   wanted its own host while another was up would be refused rather than merely rude.
#
# WHAT IS NOT HERE: the plugin project's gates. They test ITS code; the only thing shared is the model in
# shared/, and check_model_sync is how that is watched.
#
# usage: test/run_all.sh              the build layer (the edit loop; safe to run at any time)
#        test/run_all.sh --assembly   the build layer, then the assembly layer (a delivery run)
#        test/run_all.sh --fast       accepted as an alias for the build layer
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # apex_take_the_field / apex_restore_the_field -- see the file

ASSEMBLY=0
GATE_SET="all"
while [ $# -gt 0 ]; do
  case "$1" in
    --assembly) ASSEMBLY=1; shift ;;
    --set)      ASSEMBLY=1; GATE_SET="${2:-all}"; shift 2 ;;
    --fast)     break ;;
    *) echo "usage: test/run_all.sh [--assembly] [--set none|panel|all] [--fast]"; exit 2 ;;
  esac
done

pass=0
fail=0
failed_names=()

run_gate() {
  local name="$1"
  local script="$ROOT/test/$name.sh"
  if [ ! -f "$script" ]; then
    printf "  %-26s %s\n" "$name" "SKIP (missing)"
    return
  fi
  out=$(bash "$script" 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then
    printf "  %-26s %s\n" "$name" "PASS"
    pass=$((pass + 1))
  else
    printf "  %-26s %s\n" "$name" "FAIL"
    echo "$out" | tail -6 | sed 's/^/        /'
    fail=$((fail + 1))
    failed_names+=("$name")
  fi
}

echo "== build layer =="
echo "   (pure logic and built artefacts: no window, no process, no input device)"
run_gate check_apex_paths      # where the files land (the portability promise)
run_gate check_apex_language   # the tray/panel language rule and the UTF-8 conversion
run_gate check_apex_hostconfig # the host's own settings file: what it means, and the quick panel's order
run_gate check_app_swallow     # swallowing implies delivering -- the rule that broke once
run_gate check_app_core        # the model: conservation and the travel rules
run_gate check_app_config      # the settings: ranges, clamping, a mangled file
run_gate check_app_tail        # the payout shape
run_gate check_app_release     # how long a window lasts (the small model before the delivery layer)
run_gate check_feature_chart   # the feature's own controls document: valid JSON, and every slider moves it
run_gate check_feature_ime     # AutoIME's matching engine: the rules, the order, the anchoring
run_gate check_feature_keepawake  # KeepAwake: two independent switches, a gated list, and the power requests it makes
# ⚠️ THE ONE BUILD-LAYER GATE THAT TOUCHES THE SCREEN, AND IT SAYS SO: it checks that a "screen off" is a WINDOW
# covering a real monitor (connected, unlocked, undoable) by looking at the window itself, which cannot be done
# without the window existing. It is put back in tens of milliseconds, and APEX_MC_NO_DARK=1 skips that part --
# see the gate's own header. Everything else it does starts no program and touches no input device.
run_gate check_feature_mediacontrol # MediaControl: brightness rows, the screen-off window, per-app volume, named panes
run_gate check_apex_palette    # the panel's colours, one palette per appearance, ordered, readable, paler
run_gate check_apex_quickpanel # the flyout: its geometry, its values, its colours, and what the features put in it
run_gate check_apex_panel      # the panel's SCRIPT against a DOM stub: every control type still renders
run_gate check_apex_deploy     # a deploy replaces the program, never the user's data, and drops retired features
run_gate check_apex_modular    # the boundaries, the process rules, the toolchain, and the input-safety rule
run_gate check_apex_docs       # AGENTS.md is still short enough to be read whole (see the gate's header)
run_gate check_model_sync      # shared/ vs the plugin's src/ -- the one thing that must not drift

if [ "$ASSEMBLY" != "1" ]; then
  echo
  if [ "$fail" -eq 0 ]; then
    echo "BUILD LAYER OK ($pass gates)"
    echo "  (the assembly layer starts the real program -- run it before a deploy: test/run_all.sh --assembly)"
  else
    echo "FAILED: $fail of $((pass + fail)) -- ${failed_names[*]}"
  fi
  exit $((fail > 0 ? 1 : 0))
fi

# ---------------------------------------------------------------------------
echo
echo "== assembly layer =="
echo "   (these start the real program; the machine's one Apex is taken for the run and given back at the end)"
#
# ⚠️⚠️ AND ONLY THE GATES THE CHANGE CAN AFFECT RUN (`--set`, set by apex/deploy.sh from the artefact delta).
#
# The user's report was "我实测跑了另一个项目的部署，整个过程2分钟不到 … 而本项目…部署却花了5分钟，中间还不断的弹窗
# 打扰用户". Measured here: one delivery took 257 s and started 20 hosts. The assembly layer is where that goes:
# nine gates that each start a whole private copy of the program. Two are about the wheel, two about REAPER, one
# about the panel's first frame -- and a change to `panel.html` cannot affect the wheel ones.
#
# ⚠️ THE SETS ARE NAMED HERE, IN ONE PLACE, AND THE DEFAULT IS `all`. A gate that is not in the chosen set is
# PRINTED as skipped, never silently dropped -- a suite that quietly runs less than it says is the failure mode
# this whole file is written against. `--set all` (the default, and what `--full` forces) runs everything.
#
#   none  -- the built program is byte-identical to what is deployed: nothing to verify (the build layer already
#            ran; a delivery with no artefacts to copy has nothing an assembly gate could find).
#   panel -- ONLY `apex-settings.exe` differs. What can a panel binary break? The panel's first paint
#            (`check_apex_flash`), the shutdown chain that closes it (`check_apex_shutdown`), and the panel
#            process actually running the page callback (`check_reaper_live`). The wheel, the REAPER note's
#            host side and the single-instance rule live in `apex.exe`, which did not change.
#   all   -- the host or a feature changed. Everything runs, and that is not caution for its own sake: a feature
#            DLL changes what the host decides about the wheel (see decision.h), and `apex.exe` contains the
#            hook, the tray, the IPC server and the config writer.
GATE_SET="${GATE_SET:-all}"
PANEL_GATES=" check_apex_flash check_apex_shutdown check_reaper_live "
in_set() {
  case "$GATE_SET" in
    all)   return 0 ;;
    none)  return 1 ;;
    panel) case "$PANEL_GATES" in *" $1 "*) return 0 ;; *) return 1 ;; esac ;;
    *)     return 0 ;;
  esac
}

# ⚠️ THE FIELD IS TAKEN HERE, ONCE, AND ONLY IF SOMETHING IS GOING TO RUN. With `--set none` the user's Apex is
# never touched at all -- no gate starts, so nothing needs the machine to itself.
if [ "$GATE_SET" != "none" ]; then
  apex_take_the_field
fi
#
# ⚠️ AND WITH APEX_KEEP_FIELD=1 IT IS NOT GIVEN BACK AT ALL -- the caller is about to deploy into the same
# folder and would otherwise stop the user's Apex a second time (see `apex/deploy.sh --gated`, which is the only
# thing that sets this). The rule is deliberately asymmetric:
#   * the field is KEPT only when every gate passed. A green run is the one case where "what is running" is
#     about to be replaced, so starting the old copy just to kill it again a second later is pure noise --
#     and for the user it is the difference between their Apex blinking twice per delivery and once.
#   * EVERY other exit path -- a failed gate, Ctrl-C, a crash -- still gives it back. Nothing about a broken run
#     should leave the user without the program they had.
if [ "${APEX_KEEP_FIELD:-0}" = "1" ]; then
  trap '[ "$fail" -eq 0 ] || apex_restore_the_field' EXIT
else
  trap 'apex_restore_the_field' EXIT
fi

# A gate outside the chosen set is PRINTED as skipped -- see the note above about a suite that quietly runs
# less than it says.
skipped=""
run_gate_maybe() {
  local name="$1" why="$2"
  if in_set "$name"; then
    run_gate "$name"
  else
    printf "  %-26s %s\n" "$name" "SKIP (not in the '$GATE_SET' set)"
    skipped="$skipped $name"
  fi
}

run_gate_maybe check_feature_edit "the edit/save model through the REAL host and feature"
run_gate_maybe check_apex_icons   "the two marks: loadable, different, one per appearance"
run_gate_maybe check_reaper_note  "the REAPER note: absent by default, present once a stand-in runs"
run_gate_maybe check_reaper_live  "the note FOLLOWS REAPER while the panel is open"
run_gate_maybe check_apex_endtoend "a real wheel through the real host"
run_gate_maybe check_apex_shutdown "closing the host closes the panel and every process it started"
run_gate_maybe check_apex_persist "an edit reaches the disk while running, and survives a kill"
run_gate_maybe check_apex_flash   "the panel paints the page's own colour before the page loads"
run_gate_maybe check_apex_single  "one Apex per machine; a repeat launch from any folder is ignored"
# ⚠️ NOT IN THE `panel` SET: this one is about apex.exe -- the flyout is the host's own window, drawn on its UI
# thread -- so a change to apex-settings.exe cannot affect it.
run_gate_maybe check_apex_flyout  "the quick panel is really built, painted and blitted, and the host survives it"

if [ -n "$skipped" ]; then
  echo
  echo "   skipped for the '$GATE_SET' set:$skipped"
  echo "   (a full run: test/run_all.sh --assembly with no --set, or apex/deploy.sh --full)"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL PASS ($pass gates: build layer + assembly layer)"
else
  echo "FAILED: $fail of $((pass + fail)) -- ${failed_names[*]}"
fi
exit $((fail > 0 ? 1 : 0))
