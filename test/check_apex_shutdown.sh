#!/usr/bin/env bash
# Gate: WHEN THE HOST EXITS, EVERYTHING IT STARTED GOES WITH IT.
#
# The user's requirement, verbatim: "主界面在托盘进程退出，也要跟着关闭，插件进程什么的要退干净".
#
# WHY IT NEEDS A GATE RATHER THAN A CODE REVIEW: the failure is invisible from inside the program. A panel
# left behind still draws, still responds, and simply fails to do anything -- so it reads as "the panel is
# broken" rather than "the host died". And the browser processes behind it are shared infrastructure
# (msedgewebview2.exe is used by Edge, by several audio plugins and by Windows itself), so counting the
# process NAME proves nothing: this gate counts the ones belonging to THIS run's user-data-dir, which is the
# only sound way to identify them (AGENTS.md, "msedgewebview2.exe 是共享设施").
#
# ⚠️ IT CLOSES THE HOST THE WAY THE USER DOES, not with a kill. `taskkill /F` skips every teardown step --
# which is precisely the thing being tested -- so the probe posts WM_CLOSE, the same message the tray's Quit
# and the panel's Quit produce.
#
# ⚠️ AND IT RUNS FROM A COPY, so it cannot touch the user's own settings or instance. (The gates used to kill
# by image name and write into build/apex/; see the notes in check_apex_icons.sh and check_apex_endtoend.sh.)
#
# usage: test/check_apex_shutdown.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

BUILD="$ROOT/build"
RUN="$BUILD/_shutdown_run"
PROBE="$BUILD/_close_probe.exe"
fail=0

echo "== building the probe =="
g++ -std=c++17 -O2 -mconsole -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$PROBE" "$ROOT/_diag/apex_close_probe.cpp" -luser32 || exit 1
echo "   ok"

# Pids of the processes launched from THIS run's folder. Anything else on the machine is somebody else's (the
# user's, or another gate's) and must not be counted, let alone touched. Filter in test/lib_procs.sh.
PidsInRun() { apex_own_pids '_shutdown_run'; }
CountInRun() { PidsInRun | grep -c . ; }

cleanup() {
  local p
  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. Take the field, give it back at exit.
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

echo
echo "== starting a private copy of the whole program =="
rm -rf "$RUN"
cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "   FAIL: the built folder could not be copied"; exit 1; }
printf '# written by check_apex_shutdown.sh -- the feature must be LIVE for this gate\n' > "$RUN/apex.ini"
rm -f "$RUN/apex.log" "$RUN/apex-settings.log"
( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
# ⚠️ CONDITIONS, NOT SLEEPS: "the host is up" and "the panel is up" are both observable, and each one is waited
# for with a budget instead of guessed with a `sleep`. (The old pair was 3 s + 4 s of pure waiting.)
HostUp()  { [ -n "$(apex_own_pids '_shutdown_run' | head -1)" ]; }
PanelUp() { [ -n "$(apex_own_pids '_shutdown_run' 'apex-settings' | head -1)" ]; }
apex_wait_for 10 HostUp

HOSTPID=$(apex_own_pids '_shutdown_run' | head -1)
if [ -z "$HOSTPID" ]; then
  echo "   FAIL: the host did not start from $RUN"
  exit 1
fi
echo "   host pid $HOSTPID is up"

# THE PANEL IS LAUNCHED BY THE HOST, which is the arrangement that matters (launched by a double-click it
# would have no host to watch and would correctly report "not running" instead of closing).
#
# ⚠️ AND IT IS NOT SHOWN (APEX_NO_WINDOW): this gate asserts about PROCESSES -- that the host's exit closes the
# panel, and that the browser children it started are gone (counted by user-data-dir). A window on screen is a
# side effect nobody here reads, and the user reported a delivery as "中间有弹了三次窗口". Same switch the
# reaper gate uses; see the note on it in docs/rules/gates.md.
APEX_NO_WINDOW=1 "$RUN/apex-settings.exe" &
apex_wait_for 15 PanelUp
PANELPID=$(apex_own_pids '_shutdown_run' 'apex-settings' | head -1)
if [ -z "$PANELPID" ]; then
  echo "   FAIL: the panel did not start"
  exit 1
fi
echo "   panel pid $PANELPID is up"

# The browser processes this run owns. Identified by the user-data-dir the panel passes to WebView2 (it lives
# beside the exe -- see RunPanel), which is the ONLY sound discriminator: the process name is shared.
BrowsersInRun() {
  powershell -NoProfile -Command \
    "Get-CimInstance Win32_Process -Filter \"Name='msedgewebview2.exe'\" -ErrorAction SilentlyContinue |
       Where-Object { \$_.CommandLine -like '*_shutdown_run*' } | Select-Object -ExpandProperty ProcessId" \
    2>/dev/null | tr -d '\r'
}
NB=$(BrowsersInRun | grep -c .)
echo "   the panel owns $NB browser process(es)"
if [ "$NB" -lt 1 ]; then
  echo "        (none found -- the browser may not have started, so the check below is weaker than it looks)"
fi

echo
echo "== closing the host the way the user does =="
"$PROBE" "$HOSTPID" || { echo "   FAIL: could not ask the host to close"; exit 1; }

# Poll rather than sleeping a flat time: the whole point is how quickly things go, and a fixed wait would
# hide a slow teardown as easily as a fast one.
wait_gone() {
  local label="$1" tries=0
  while [ $tries -lt 100 ]; do
    if [ -z "$(PidsInRun)" ]; then return 0; fi
    sleep 0.1
    tries=$((tries + 1))
  done
  echo "   FAIL: $label still running after 10s:"
  PidsInRun | sed 's/^/        pid /'
  return 1
}
if ! wait_gone "the host/panel"; then fail=1; else echo "   the host and the panel are both gone"; fi

if [ $fail -eq 0 ]; then
  # THE BROWSER PROCESSES ARE THE SLOW PART, so they get their own wait. A WebView2 controller closes its
  # children during WM_DESTROY; the host asks the panel to close and waits up to 2s before killing it (see
  # main.cpp), so the budget here has to cover "asked, closed, children noticed".
  tries=0
  while [ $tries -lt 100 ]; do
    [ "$(BrowsersInRun | grep -c .)" = "0" ] && break
    sleep 0.1
    tries=$((tries + 1))
  done
  left=$(BrowsersInRun | grep -c .)
  if [ "$left" != "0" ]; then
    echo "   FAIL: $left browser process(es) from this run outlived the program"
    BrowsersInRun | sed 's/^/        pid /'
    fail=1
  else
    echo "   and its $NB browser process(es) went with it"
  fi
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: closing the host closed the panel and every process the panel had started"
else
  echo "FAILED: something outlived the host"
fi
exit $fail
