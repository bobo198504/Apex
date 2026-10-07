#!/usr/bin/env bash
# Gate: THE REAPER NOTE FOLLOWS REAPER WHILE THE PANEL IS OPEN, with nobody touching the UI.
#
# The user's question was the reason this exists: "做不到动态加载，还要拖动切换插件标签才能显示吗？" The first
# version could only refresh when a feature was selected, so the honest answer was yes.
#
# ⚠️ IT PROVES THE WHOLE CHAIN IN ONE RUN: the host notices REAPER starting and stopping, tells the panel, and
# the panel runs the page callback -- without anyone clicking anything. The page's own half (re-read the
# controls when told, and do nothing when there is nothing to re-read) is pinned in _diag/apex_panel_probe.js
# against the DOM stub, because driving a real click is UI interaction and that belongs to the user.
#
# ⚠️ NOTHING IS DISTURBED: it runs a private copy of the program, moves no pointer, and kills only its own
# processes -- by PATH, never by name, because its stand-in is called reaper.exe.
#
# usage: test/check_reaper_live.sh
## DOES THE NOTE APPEAR AND DISAPPEAR ON ITS OWN, with the panel open and nobody touching it?
#
# This is the user's question, verbatim: "「排除」标签右边那行灰字，做不到动态加载，还要拖动切换插件标签才能显示吗？"
# The first version could only refresh when a feature was selected, so the honest answer was yes -- and this
# probe is what makes it no.
#
# ⚠️ IT WATCHES THE HOST'S LOG, NOT THE SCREEN, and that is the right level for a probe on this side: the log
# carries the host's own "the answer changed -> telling the panel" line, so this proves the CHANGE was noticed
# and the panel was told. What the page then draws is the page's business, and its handling of the notification
# is pinned separately (the panel probe drives __apexStateChanged against the DOM stub).
#
# ⚠️ IT OPENS A REAL PANEL, so it is a UI-adjacent probe: it runs a private copy of the program, moves nothing,
# and kills only its own processes by path. It is NOT in run_all.sh.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file
BUILD="$ROOT/build"
FAKE="$BUILD/_reaper_fake"
RUN="$BUILD/_live_run"
fail=0

# `reaper` is in the name list because the stand-in IS called reaper.exe -- the host looks for that name. It is
# safe here only because the path must be inside this gate's scratch folder too: this machine may one day have
# a real REAPER, and that one must never be touched. The filter itself is in test/lib_procs.sh.
PidsIn() { apex_own_pids "$1" 'apex,apex-settings,reaper'; }
cleanup() {
  local p
  for p in $(PidsIn '_reaper_fake'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  for p in $(PidsIn '_live_run'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. Take the field, give it back at exit.
# (The stand-in is called reaper.exe and is NOT an Apex process, so the field does not touch it.)
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

echo "== building the stand-in =="
mkdir -p "$FAKE"
g++ -std=c++17 -O2 -mwindows -o "$FAKE/_fake.exe" "$ROOT/_diag/apex_reaper_fake.cpp" -luser32 || exit 1
g++ -std=c++17 -O2 -shared -o "$FAKE/reaper_smoothwheelscroll_fake.dll" -x c++ - <<'EOF' || exit 1
extern "C" __declspec(dllexport) int marker(void) { return 1; }
EOF
cp "$FAKE/_fake.exe" "$FAKE/reaper.exe"
LFAKE="$BUILD/_lertaro_fake"
mkdir -p "$LFAKE"
g++ -std=c++17 -O2 -mconsole -o "$LFAKE/_marker.exe" "$ROOT/_diag/lertaro_marker_fake.cpp" -luser32 || exit 1
echo "   ok"

# ⚠️ THE MARKER HAS TO BE ABSENT, OR THIS GATE MEASURES THE USER'S PROGRAM INSTEAD OF ITS FIXTURE. Lertaro holds
# the same named event, and the note names EVERY engine that is running -- so with a real Lertaro up, "REAPER
# alone" and "no engines" are both unreachable states, and every assertion below would be about whatever the user
# happened to have open. Said out loud rather than passing quietly (see the project's rule about a gate that
# cannot fail).
if "$LFAKE/_marker.exe" --check >/dev/null 2>&1; then
  echo
  echo "SKIPPED: the REAPER note, live -- a real Lertaro is running and holds the engine marker, so the engine"
  echo "         set this gate asserts on cannot be produced. Nothing was checked."
  exit 0
fi

echo
echo "== a private copy, WITH THE PANEL OPEN and nobody touching it =="
cleanup; sleep 0.5
rm -rf "$RUN" 2>/dev/null
cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "FAIL: copy"; exit 1; }
printf '# live-note probe\n' > "$RUN/apex.ini"
rm -f "$RUN"/apex*.log
( cd "$RUN" && APEX_NO_TRAY=1 APEX_NO_WINDOW=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
tries=0
while [ $tries -lt 60 ]; do
  [ -n "$(PidsIn '_live_run')" ] && break
  sleep 0.2; tries=$((tries + 1))
done
( cd "$RUN" && APEX_NO_TRAY=1 APEX_NO_WINDOW=1 cmd //c start "" apex-settings.exe >/dev/null 2>&1 )
# ⚠️ CONDITIONS, NOT SLEEPS: the panel process has to be up, and its PAGE has to have drawn -- the second is what
# `page is drawn` says, and it is the sentence that matters here, because the host's "telling the panel" call is
# only delivered to a page that is already running. (It used to be `sleep 5`.)
PanelUp() { [ "$(PidsIn '_live_run' | grep -c .)" -ge 2 ]; }   # the host AND the panel it started
apex_wait_for 15 PanelUp
apex_wait_line "$RUN/apex-settings.log" 'page is drawn' 20
echo "   host and panel are up; the panel is left alone from here"

base=$(apex_count_line "$RUN/apex.log" 'state: the engines are')
echo "   (the host has announced $base change(s) so far)"

echo
echo "== 1. start REAPER while the panel is open =="
# ⚠️⚠️ A CONDITION HAS TO BE ABOUT **THIS STEP**, NOT ABOUT THE FILE HAVING EVER SAID IT.
#
# Two versions of this wait were wrong, and both were caught by the gate itself:
#   1. asking for "is not running -- telling the panel" matched the line the host writes at STARTUP ("the first
#      report"), so the wait returned instantly and the real announcement landed inside section 4's quiet window,
#      which then failed as "the host is chattering";
#   2. counting ANY 'state: the engines are' line made the baseline 0 (the host had not written its first
#      report yet) and the wait again returned on the first line to appear.
# So each step counts ITS OWN sentence and waits for that count to grow (apex_wait_line_gt).
running_before=$(apex_count_line "$RUN/apex.log" 'are REAPER -- telling the panel')
( cd "$FAKE" && cmd //c start "" "$FAKE/reaper.exe" >/dev/null 2>&1 )
# The host polls about once a second; wait for the line it writes when it notices, rather than for two polls.
apex_wait_line_gt "$RUN/apex.log" 'are REAPER -- telling the panel' "$running_before" 15
if grep -q 'state: the engines are REAPER -- telling the panel' "$RUN/apex.log"; then
  echo "   ok  the host noticed and told the panel, with nobody touching the UI"
else
  echo "   FAIL: nothing was announced after REAPER started. Recent host lines:"
  tail -4 "$RUN/apex.log" | sed 's/^/        /'
  fail=1
fi

echo
echo "== 2. the notice reaches the panel PROCESS and its page =="
# ⚠️ THIS CHECK IS WHY THE HANDLER NOW LOGS. The first version of the panel-side handler called ExecuteScript
# directly and left no trace, and this probe then reported "the panel never ran the page callback" about a
# handler that was working. Every script the panel process runs gets one line, for exactly this reason.
if grep -q '__apexStateChanged' "$RUN/apex-settings.log" 2>/dev/null; then
  echo "   ok  the panel ran the page callback ($(grep -c '__apexStateChanged' "$RUN/apex-settings.log") time(s))"
else
  echo "   FAIL: the panel process never ran the page callback"
  tail -3 "$RUN/apex-settings.log" | sed 's/^/        /'
  fail=1
fi

echo "== 3. close REAPER: the note must go away by itself =="
stopping_before=$(apex_count_line "$RUN/apex.log" 'are none -- telling the panel')
for p in $(PidsIn '_reaper_fake'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
apex_wait_line_gt "$RUN/apex.log" 'are none -- telling the panel' "$stopping_before" 15
if grep -q 'state: the engines are none -- telling the panel' "$RUN/apex.log"; then
  echo "   ok  and it noticed the plugin going away"
else
  echo "   FAIL: nothing was announced after REAPER stopped"
  fail=1
fi

echo
echo "== 4. the host is not chattering (a steady state must be silent) =="
before=$(apex_count_line "$RUN/apex.log" 'state: the engines are')
# ⚠️ THIS SLEEP STAYS A SLEEP, AND IT IS THE ONE KIND THAT MUST: it is not waiting for something to happen, it
# is MEASURING that nothing happens. A condition cannot express "no announcements in this window" -- the window
# is the measurement. (Same reason the endtoend gate's retry pause stays a sleep: it lets the pointer settle.)
sleep 5
after=$(apex_count_line "$RUN/apex.log" 'state: the engines are')
if [ "$before" = "$after" ]; then
  echo "   ok  no announcements in 5 s of no change ($before total)"
else
  echo "   FAIL: $((after - before)) announcements with nothing changing -- the page would redraw forever"
  fail=1
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: the note follows REAPER while the panel is open, with nobody touching the UI"
else
  echo "FAILED: the note still needs the user to do something"
fi
exit $fail
