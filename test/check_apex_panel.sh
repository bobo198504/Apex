#!/usr/bin/env bash
# Gate: THE PANEL'S OWN SCRIPT, run against a DOM stub -- with no window, no process, and no input device.
#
# ⚠️ THIS GATE EXISTS TO RUN _diag/apex_panel_probe.js, WHICH UNTIL NOW RAN NOWHERE.
#
# That probe found three of the panel's real bugs (the re-render loop that made the settings walk
# zh->auto->en->dark->light by themselves; the "host not running" banner that was deleted by the first
# render; the input handler wired into a render function). It was written as a diagnostic and the notes in
# AGENTS.md say to run it -- but no gate did, so nothing would have failed if it had stopped working. A check
# that is only mentioned is a check that rots.
#
# ⚠️ IT BELONGS IN THE BUILD LAYER, NOT THE ASSEMBLY LAYER. It opens no window and starts nothing: it loads
# apex/ui/panel.html into a hand-built DOM and calls into it. The page's half of every contract can therefore
# be verified in a few hundred milliseconds, on every edit, without touching the user's machine -- which is
# exactly the split the user asked for ("数据模型验证可以由你在构建时就要做好").
#
# ⚠️ AND IT IS NOT A SUBSTITUTE FOR THE USER'S OWN TESTING. A DOM stub has no layout and no hit-testing, so it
# cannot catch the class of bug the user found by hand ("开关不能用" -- a decoration div drawn over the
# checkbox, so the click never reached it). That one needed a real click, which is the user's job by
# agreement. This gate covers what a stub CAN see; the notes at the top of the probe say the same thing.
#
# usage: test/check_apex_panel.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
PROBE="$ROOT/_diag/apex_panel_probe.js"
EDIT_PROBE="$ROOT/_diag/apex_edit_probe.js"

if [ ! -f "$PROBE" ]; then
  echo "FAIL: $PROBE is missing -- the panel's script is not being checked at all"
  exit 1
fi
if [ ! -f "$EDIT_PROBE" ]; then
  echo "FAIL: $EDIT_PROBE is missing -- the group editor's edit/save/cancel flow is not being checked"
  exit 1
fi
if ! command -v node >/dev/null 2>&1; then
  # An ERROR rather than a skip, for the same reason check_feature_ime.sh does it: a missing toolchain must say
  # so, because the alternative is a gate that passes on the machine that has it and proves nothing here.
  echo "FAIL: node is not available, so the panel's script cannot be run (python is a Store stub on this machine)"
  exit 1
fi

out=$(node "$PROBE" 2>&1)
rc=$?
printf '%s\n' "$out" | tail -20 | sed 's/^/   /'

echo
if [ $rc -ne 0 ]; then
  echo "FAILED: the panel's script does not pass its own probe"
  exit 1
fi
# The probe's own verdict line, so a probe that quietly stopped making assertions cannot pass by printing
# nothing: "OK:" is the only thing it prints when every check ran.
if printf '%s' "$out" | grep -q "OK:"; then
  echo "OK: the panel boots, connects, lists features, and renders every control type"
else
  echo "FAILED: the probe ran but never reached its own verdict (did it stop making checks?)"
  exit 1
fi

# ---------------------------------------------------------------------------
echo
echo "== the REAL message sequence: what the host sends back after each command =="
#
# ⚠️ A THIRD PROBE, AND IT IS THE ONE THAT WOULD HAVE FOUND THE BUG. The two above drive the page one step at a
# time and both passed while the user reported edit/save doing nothing -- because the fault was in the SEQUENCE:
# the host answers every command with a snapshot, that snapshot re-reads the page, and the re-read was cancelling
# the edit the user had just started. This probe sends that snapshot, and it also checks that a re-read leaves the
# scroll position alone ("点‘编辑’时，参数界面会焦点会跳到顶端，这点规避掉，不要跳").
SEQ_PROBE="$ROOT/_diag/apex_edit_sequence_probe.js"
if [ -f "$SEQ_PROBE" ]; then
  seq_out=$(node "$SEQ_PROBE" 2>&1)
  seq_rc=$?
  printf '%s
' "$seq_out" | sed 's/^/   /'
  if [ $seq_rc -ne 0 ]; then
    echo "FAILED: the page does not survive the message sequence the host really sends"
    exit 1
  fi
else
  echo "   FAIL: $SEQ_PROBE is missing -- the sequence is not being checked"
  exit 1
fi

echo
echo "== the group editor: edit / save / cancel, and the draft behind them =="
#
# ⚠️ A SECOND PROBE, AND IT IS A SEPARATE FILE FOR A REASON. The checks above drive the whole page -- list,
# banner, theme, chart -- and each one resets the page state it needs. The edit flow is the opposite: it is a
# SEQUENCE (press Edit, type, redraw, press Save), so it needs a page that nobody else is resetting in the
# middle. Keeping it here as more checks in the same file is what produced two rounds of confusion: an
# assertion failing because a check three sections ABOVE had cleared the state it depended on.
#
# It comes from the standalone program, which the user calls mature: "列表需要有个编辑保存功能，用户使用逻辑见
# Auto-ime独立版". The properties it pins are the ones a user would notice -- fields are read-only until Edit,
# typing sends nothing, Save sends exactly what changed, Cancel throws the edit away.
edit_out=$(node "$EDIT_PROBE" 2>&1)
edit_rc=$?
printf '%s\n' "$edit_out" | sed 's/^/   /'
if [ $edit_rc -ne 0 ]; then
  echo "FAILED: the group editor's edit/save/cancel flow is broken"
  exit 1
fi

# ---------------------------------------------------------------------------
echo
echo "== the window has a floor under its width (a page is a layout, not a document) =="
#
# ⚠️ WHY A SOURCE CHECK, AND WHAT IT CANNOT SEE. What has to be prevented is "the panel can be dragged so narrow
# that a plugin's row is cut off" -- the user's report: "设置面板宽度要有一个最小值限定，往小了拉，插件有些控件会超出
# 面板". The fix is a MINIMUM TRACK SIZE, and the window manager only consults it while somebody is dragging the
# border: the honest test is a person dragging it, and "any automation must not touch the user's mouse" is a rule
# here. So this checks the mechanism is still THERE -- and that it is scaled by the monitor's DPI, because a floor
# measured in device pixels is a third of a floor at 300%, which is the same bug wearing a different hat.
#
# ⚠️ AND THE SCAN IS FED A FIXTURE THAT MUST HIT before it is believed. The DWM scan in check_apex_quickpanel.sh
# was written as `GetProcAddress[^)]*SetWindowCompositionAttribute` and could never match the line it was written
# for, while printing `ok` for a file that had the call in it (see docs/rules/quickpanel.md). Each of the three
# patterns below therefore has to find its own line in a fixture AND find nothing in a comment that names the same
# things -- which is also what keeps "a handler that does nothing" (the message, no assignment) from passing.
PANEL_WIN="$ROOT/apex/ui_webview.cpp"
# ⚠️ THE CASE PATTERN IS ANCHORED AT BOTH ENDS, AND THAT IS NOT PEDANTRY: the first version stopped at the
# message's name, so `case WM_GETMINMAXINFO_REMOVED_FOR_A_MOMENT:` -- a handler for a message that does not exist
# -- satisfied it. (Found by breaking it on purpose, which is the only reason the anchor is there.)
W_CASE='case[[:space:]]+WM_GETMINMAXINFO[[:space:]]*:'
W_SIZE='ptMinTrackSize[[:space:]]*\.[xy][[:space:]]*=[^=]'
W_SCALE='PanelScalePercent[[:space:]]*\('

SELFTEST=$(mktemp)
printf '  case WM_GETMINMAXINFO:\n    mmi->ptMinTrackSize.x = kPanelMinWidthCss * PanelScalePercent(h) / 100;\n' >"$SELFTEST"
h1=$(grep -cE "$W_CASE" "$SELFTEST")
h2=$(grep -cE "$W_SIZE" "$SELFTEST")
h3=$(grep -cE "$W_SCALE" "$SELFTEST")
printf '// WM_GETMINMAXINFO is not handled; ptMinTrackSize and PanelScalePercent were removed with it\n' >"$SELFTEST"
m1=$(grep -cE "$W_CASE" "$SELFTEST")
m2=$(grep -cE "$W_SIZE" "$SELFTEST")
m3=$(grep -cE "$W_SCALE" "$SELFTEST")
rm -f "$SELFTEST"
if [ "$h1" = "0" ] || [ "$h2" = "0" ] || [ "$h3" = "0" ] ||
   [ "$m1" != "0" ] || [ "$m2" != "0" ] || [ "$m3" != "0" ]; then
  echo "   FAIL: this scan cannot tell the handler from prose about it"
  echo "         (case=$h1/$m1 size=$h2/$m2 scale=$h3/$m3 -- want three hits, then three misses)"
  exit 1
fi
echo "   ok  the scan finds the handler, and ignores its own names in a comment"

if [ ! -f "$PANEL_WIN" ]; then
  echo "   FAIL: $PANEL_WIN is missing -- there is no window to put a floor under"
  exit 1
fi
if ! grep -qE "$W_CASE" "$PANEL_WIN"; then
  echo "   FAIL: the panel window no longer answers WM_GETMINMAXINFO, so it can be dragged narrower than the"
  echo "         layout it is showing (see docs/rules/panel.md)."
  exit 1
fi
if ! grep -qE "$W_SIZE" "$PANEL_WIN"; then
  echo "   FAIL: nothing sets ptMinTrackSize -- the handler is there and does nothing, which is worse than"
  echo "         having none: it reads as if the floor were in place."
  exit 1
fi
if ! grep -qE "$W_SCALE" "$PANEL_WIN"; then
  echo "   FAIL: the floor is not scaled by the monitor's DPI, so at 150% it is two thirds of the size it was"
  echo "         measured to be (see the note on kPanelMinWidthCss)."
  exit 1
fi
echo "   ok  a minimum size is answered, and it is scaled by the window's own monitor"

