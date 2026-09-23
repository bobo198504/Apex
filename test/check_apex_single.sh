#!/usr/bin/env bash
# Gate: ONE APEX PER MACHINE -- and a repeat launch, from anywhere, does nothing.
#
# THE USER'S RULE: "进程只能有一个Apex.exe" -- one Apex, whichever folder it came from.
#
# ⚠️ THIS GATE USED TO ASSERT THE OPPOSITE, AND THE CHANGE IS THE POINT. It previously required that two
# copies in two folders COEXIST ("two different installations still coexist"), because Apex is portable and the
# folder used to be the installation. The user has since made the rule machine-wide, and their reasoning is
# in the product: two hosts are not two windows, they are two GLOBAL WHEEL HOOKS. Each swallows what it sees
# and injects its own output, and the second one sees the first one's injected events -- the "two handlers
# driving one view" that decision.h exists to prevent. The old folder rule allowed exactly that whenever a
# second copy was launched from anywhere else.
#
# THREE PROPERTIES:
#
#   1. A SECOND LAUNCH DOES NOTHING -- not "asks the first to quit", not "shows a box" -- it exits and the
#      first instance is untouched. Checked from the SAME folder and then from a DIFFERENT one, because those
#      are the two ways it can be attempted and only the second one distinguishes this rule from the old one.
#
#   2. THE RUNNING INSTANCE'S LOG SURVIVES. The log is opened with "w" (truncate), so a second copy that got
#      as far as opening it would wipe the evidence of what the first one was doing -- then exit, leaving an
#      empty file and no way to tell anything had happened. The ordering in WinMain is what prevents it, and
#      this is the check that keeps that ordering honest.
#
#   3. THE FOLDER IS STILL WHAT RUNS. The copy that is up is the one that was launched first, and the second
#      folder must not have displaced it -- "ignored" has to mean the first one keeps working, not that the
#      two take turns.
#
# ⚠️ IT TAKES THE FIELD AND GIVES IT BACK. It cannot start a host while the user's is running (that is the
# rule), so it stops what is running first -- authorised by the user: "必须清掉当前项目在运行的进程，不管是不是
# 我在运行" -- and relaunches it on the way out, on every exit path through the trap. See apex_take_the_field in
# test/lib_procs.sh.
#
# usage: test/check_apex_single.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill, and how the field is taken -- see the file
BUILD="$ROOT/build"
A="$BUILD/_single_a"
B="$BUILD/_single_b"
fail=0

PidsIn() { apex_own_pids "$1"; }
# ⚠️ TWO JOBS, AND THEY ARE DELIBERATELY SEPARATE. Killing this gate's own leftover copies is what the START of the
# gate needs; restoring the user's installation is what the EXIT needs. Calling both at the start was a race this
# gate lost once: the suite (in the delivery mode) is holding the field down on purpose, and restoring it there
# STARTED the user's copy -- or, worse, a scratch copy that an earlier take had recorded (see
# `apex_is_scratch_path` in lib_procs.sh) -- while this gate was starting its own. Two hosts came up together and
# the gate reported "2 Apex processes are running (the rule is one)": a gate failing the very rule it exists to
# check, because of its own cleanup.
kill_my_copies() {
  local p
  for p in $(PidsIn '_single_a'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  for p in $(PidsIn '_single_b'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
cleanup() {
  kill_my_copies
  apex_restore_the_field   # whatever the user had running before this gate started
}
trap cleanup EXIT

echo "== preparing two private installations =="
kill_my_copies 2>/dev/null || true   # ⚠️ NOT `cleanup`: see the note above
apex_take_the_field
sleep 0.5
rm -rf "$A" "$B"
cp -r "$BUILD/apex" "$A" 2>/dev/null || { echo "   FAIL: could not copy to $A"; exit 1; }
cp -r "$BUILD/apex" "$B" 2>/dev/null || { echo "   FAIL: could not copy to $B"; exit 1; }
printf '# single-instance gate\n' > "$A/apex.ini"
printf '# single-instance gate\n' > "$B/apex.ini"
rm -f "$A"/apex*.log "$B"/apex*.log
echo "   $A and $B"

echo
echo "== 1. the first launch comes up =="
( cd "$A" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
tries=0
while [ $tries -lt 60 ]; do
  [ -n "$(PidsIn '_single_a')" ] && break
  sleep 0.2
  tries=$((tries + 1))
done
if [ -z "$(PidsIn '_single_a')" ]; then
  echo "   FAIL: the first instance did not start"
  exit 1
fi
FIRST_PID=$(PidsIn '_single_a' | head -1)
echo "   up (pid $FIRST_PID)"

# The identity the user's rule is about, checked through the SAME mechanism the product uses: the window
# class, not the image name. If this ever disagrees with PidsIn above, one of the two identities is wrong.
OWNERS=$(apex_all_pids)
echo "   running Apex, by window class: [$(echo $OWNERS | tr '\n' ' ')]"

# A distinctive line the first instance wrote, to prove the log was not truncated by the second launch.
# ⚠️ CONDITION, NOT A SLEEP: wait until the first instance has finished starting (that is the last line WinMain
# writes before it settles), then take the count. A fixed 2 s was a guess about a machine we do not control.
apex_wait_line "$A/apex.log" 'engine running' 10
LINES_BEFORE=$(wc -l < "$A/apex.log" 2>/dev/null || echo 0)
echo "   its log has $LINES_BEFORE lines"

echo
echo "== 2. a SECOND launch of the SAME installation is ignored =="
( cd "$A" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
sleep 3
COUNT=$(PidsIn '_single_a' | grep -c .)
if [ "$COUNT" = "1" ]; then
  echo "   still exactly one instance running"
else
  echo "   FAIL: $COUNT instances are running (the second launch was not ignored)"
  fail=1
fi
LINES_AFTER=$(wc -l < "$A/apex.log" 2>/dev/null || echo 0)
if [ "$LINES_AFTER" -ge "$LINES_BEFORE" ] && [ "$LINES_BEFORE" -gt 0 ]; then
  echo "   and the running instance's log survived ($LINES_BEFORE -> $LINES_AFTER lines)"
else
  echo "   FAIL: the log was truncated by the second launch ($LINES_BEFORE -> $LINES_AFTER lines)"
  fail=1
fi
# The second launch must not have left its own "starting" line behind either -- that would mean it ran far
# enough to open the log, which is exactly what the ordering in WinMain is meant to prevent.
if [ "$LINES_AFTER" -gt "$((LINES_BEFORE + 2))" ]; then
  echo "   (note: the log grew a lot -- the second launch may have started before exiting)"
fi

echo
echo "== 3. a launch from a DIFFERENT FOLDER is ignored too (the rule the user asked for) =="
( cd "$B" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
# ⚠️⚠️ THIS IS A BOUNDED QUIET WINDOW, NOT A POLL, AND THE DIFFERENCE IS 32 SECONDS.
#
# The check is that the second copy does NOT appear -- an expectation about something that must not happen, and
# a thing that does not happen produces nothing to wait for. The old version polled `PidsIn '_single_b'` up to 60
# times, and each poll spawns POWERSHELL (0.39 s, measured in the cost notes): 60 x 0.53 s = 32 s of a 43 s gate,
# burned in full every run because the loop can only exit by timing out. A process that is going to start has
# started within a second or two; after that, one check is worth as much as sixty. The count is checked again
# below (`apex_count`), so a late appearance is still caught.
#
# (This is the same shape as the "no announcements" window in check_reaper_live: a MEASUREMENT of absence is a
# sleep, and no condition can express it.)
sleep 2
if [ -n "$(PidsIn '_single_b')" ]; then
  echo "   FAIL: a copy in a DIFFERENT folder started as well -- two hosts, two wheel hooks"
  echo "         (that is the fault this rule exists to prevent: each hook sees the other's injected wheels)"
  fail=1
else
  echo "   the second folder's copy did not start"
fi

# ... and "ignored" must mean the FIRST one is still the one running, undisturbed.
if [ "$(PidsIn '_single_a' | grep -c .)" = "1" ] && [ "$(PidsIn '_single_a' | head -1)" = "$FIRST_PID" ]; then
  echo "   and the first instance (pid $FIRST_PID) is still the one running"
else
  echo "   FAIL: the first instance was displaced or lost (was $FIRST_PID, now [$(PidsIn '_single_a' | tr '\n' ' ')])"
  fail=1
fi

# And the machine-wide count agrees: exactly one Apex, by window class.
if [ "$(apex_count)" = "1" ]; then
  echo "   exactly one Apex on the machine, by window class"
else
  echo "   FAIL: $(apex_count) Apex processes are running (the rule is one)"
  fail=1
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: one Apex per machine, a repeat launch is ignored from any folder, and the first one keeps running"
else
  echo "FAILED: single-instance behaviour"
fi
exit $fail
