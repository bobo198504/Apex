#!/usr/bin/env bash
# Gate: THE QUICK PANEL IS ACTUALLY DRAWN -- the model is built from live features, the panel is painted and
# blitted onto a real window, and the host is still running afterwards.
#
# WHY THIS GATE EXISTS, AND WHAT IT IS THE ONLY COVER FOR. Everything about the flyout's GEOMETRY is arithmetic
# and is checked without a screen (apex/quickpanel.h, by check_apex_quickpanel.sh); everything about the host's
# start-up, hook and shutdown is checked by the other assembly gates. The one seam nothing else touches is the
# path from "the host is running with its features loaded" through:
#
#     build the model from what each feature answers (quickItems)
#       -> lay it out and paint it with GDI+ into a 32-bit DIB
#         -> hand it to the window with UpdateLayeredWindow
#
# ...and it runs ON THE UI THREAD OF THE PROCESS THAT OWNS THE WHEEL HOOK. A mistake there does not take out a
# window, it takes out the program -- and the user finds out by clicking the tray icon, which is precisely the
# moment they are not expecting anything to go wrong. So it is worth one private copy of the host to find out.
#
# ⚠️ IT CLICKS NOTHING AND TAKES NO FOCUS. The trigger is a switch the product carries for this one purpose
# (APEX_QUICKPANEL_ONCE -- see QuickPanelSelfTest in main.cpp): the host shows the panel once at start-up and
# hides it again. Under that switch the flyout deliberately does NOT call SetForegroundWindow, because a gate
# that pulls the focus away from whatever somebody is typing into is not a test, it is an interruption.
#
# ⚠️ AND IT RUNS A PRIVATE COPY, killing only processes started from it (by name AND path) -- see lib_procs.sh.
#
# usage: test/check_apex_flyout.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

BUILD="$ROOT/build"
RUN="$BUILD/_flyout_run"
LOG="$RUN/apex.log"
fail=0

PidsInRun() { apex_own_pids '_flyout_run'; }
NoPidsInRun() { [ -z "$(PidsInRun)" ]; }
cleanup() {
  local p
  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. Take the field, give it back at exit.
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

echo "== starting a private copy with the flyout self test on =="
rm -rf "$RUN"
cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "   FAIL: the built folder could not be copied"; exit 1; }
printf 'theme=auto\nlang=auto\n' > "$RUN/apex.ini"
rm -f "$RUN"/apex*.log
# ⚠️ THE TRAY IS LEFT ON, AND THAT IS THE POINT RATHER THAN AN OVERSIGHT. The panel is placed above the tray
# icon by asking the shell where that icon is (Shell_NotifyIconGetRect), and there is no other way to exercise
# that call: with the icon suppressed the lookup always fails and the panel silently falls back to the cursor,
# so the gate would pass while "the panel appears in the wrong place" -- the most visible way this feature can
# be wrong -- was never once tested. So this gate keeps its icon, exactly like check_apex_icons (see the note in
# main.cpp about the two gates that must not suppress it); every other gate is silent.
( cd "$RUN" && APEX_QUICKPANEL_ONCE=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
# CONDITION, NOT A SLEEP: the host is up when its start-up has written this line (see main.cpp).
apex_wait_line "$LOG" 'engine running' 10
# The icon has to be IN the shell before it can be asked where it is (the line is written by TrayAdd).
apex_wait_line "$LOG" 'tray icon added' 10
if [ ! -f "$LOG" ]; then
  echo "   FAIL: the host wrote no log at all"
  exit 1
fi

echo
echo "== 1. the window exists before anything is shown =="
# The window is created at start-up on purpose (see QuickPanelInit): the first click must not pay for it.
if grep -q 'quickpanel: window created (GDI+ up)' "$LOG"; then
  echo "   the flyout window and GDI+ are up"
else
  echo "   FAIL: the host did not report a working flyout window:"
  grep -i 'quickpanel' "$LOG" | sed 's/^/        /'
  fail=1
fi

echo
echo "== 2. the panel is built, painted and blitted =="
apex_wait_line "$LOG" 'quickpanel: shown' 15
SHOWN=$(grep 'quickpanel: shown' "$LOG" | tail -1)
if [ -z "$SHOWN" ]; then
  echo "   FAIL: the panel was never shown (the self test did not run)"
  fail=1
else
  echo "   $SHOWN"
  # ⚠️ THE BLIT'S OWN ANSWER IS THE ASSERTION, and the row count is the other half of it: "0 rows in 0 blocks"
  # would mean the model came out empty on a host with features loaded, which is exactly the failure the
  # arithmetic gate cannot see (it feeds its own model).
  case "$SHOWN" in
    *'blit=ok'*) echo "   the layered blit succeeded" ;;
    *) echo "   FAIL: the blit did not succeed"; fail=1 ;;
  esac
  if echo "$SHOWN" | grep -Eq 'shown -- [1-9][0-9]* rows in [1-9][0-9]* blocks'; then
    echo "   the model was built from the live features (there is something in the panel)"
  else
    echo "   FAIL: the panel came up empty (0 rows or 0 blocks) -- the feature answers were not used"
    fail=1
  fi
  case "$SHOWN" in
    *'FOREGROUND REFUSED'*) echo "   FAIL: the self test took the focus, which it must not"; fail=1 ;;
    *) echo "   the focus was left alone (as a gate must)" ;;
  esac
  # ⚠️ AND IT ANCHORED ON THE ICON. The fallback to the cursor is written into the same line (see LogShown), so
  # "the shell would not say where the tray icon is" appearing here means the placement has never been tested --
  # which is the whole reason this gate leaves APEX_NO_TRAY unset.
  case "$SHOWN" in
    *'the shell would not say'*)
      echo "   FAIL: the panel fell back to the cursor -- the tray icon was not found, so \"above the icon\" is unverified"
      fail=1
      ;;
    *) echo "   it was placed above the tray icon (the shell answered where the icon is)" ;;
  esac
fi

echo
echo "== 3. it fades out and the host is still alive =="
apex_wait_line "$LOG" 'flyout selftest: hiding it again' 15
if grep -q 'quickpanel: hiding -- the selftest is over' "$LOG"; then
  echo "   the panel was put away"
else
  echo "   FAIL: the panel was never hidden again"
  fail=1
fi
# ⚠️ THE POINT OF THE WHOLE GATE. Every call above runs on the thread that owns the wheel hook; if any of it
# crashed, the process would be gone by now -- and this is the assertion that says so rather than assuming it.
if [ -n "$(PidsInRun)" ]; then
  echo "   the host survived showing and hiding the panel (same pid, still running)"
else
  echo "   FAIL: the host is gone -- it crashed while the panel was built or drawn"
  fail=1
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: the quick panel is built from the live features, drawn on a real window, and the host survives it"
else
  echo "FAILED: the quick panel"
fi
exit $fail
