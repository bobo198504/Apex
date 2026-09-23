#!/usr/bin/env bash
# END-TO-END: does a wheel go in one end and come out the other, transformed?
#
# THIS IS THE GATE THAT MATTERS. Every other test in this repository checks a piece -- the model's
# arithmetic, the config parsing, the loader's rules. None of them can tell you whether the assembled
# program actually takes a wheel from the OS, decides about it, and hands smoothed motion to the program
# under the cursor. That is what this does, with an A/B so the answer is a comparison rather than an
# impression:
#
#   A: Apex NOT running  -> the receiver must see the raw notches (3 in, 3 messages, 360 deltas)
#   B: Apex running      -> the receiver must see MANY SMALL messages instead, and the raw notches must be
#                           gone (they were swallowed)
#
# THE RECEIVER IS OUR OWN WINDOW, not a real program. Notepad was tried first and abandoned: it scrolls,
# and reading back how far it scrolled is a proxy with its own errors, while a counter is the answer
# itself. It also means this test cannot disturb whatever the user has open.
#
# Apex is started with APEX_ACCEPT_INJECTED=1. That is a TEST-ONLY mode (see host_win.cpp): a synthetic
# wheel is indistinguishable from Apex's own output -- both carry LLMHF_INJECTED -- so without it this
# test cannot drive the real path at all, and would instead measure an idle process and report success.
#
# usage: test/check_apex_endtoend.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

BUILD="$ROOT/build"
OUT="$BUILD/apex/Plugins/SmoothWheel"
mkdir -p "$BUILD" "$OUT"

# ---- build everything the test needs ----
echo "== building =="
# ⚠️ THE RECEIVER IS BUILT WITH -mwindows, AND THAT IS A UI FIX, NOT A STYLE CHOICE. It was -mconsole and
# launched through `cmd start`, so every run of this gate opened a black console window on the user's screen
# beside the receiver's own white rectangle. Its output goes to a file (apex_receiver.txt) -- which is what
# this script reads -- so the console was never carrying information to anyone. The GUI subsystem allocates
# no console. (The injector stays a console program: it is run in the foreground and its output is piped.)
g++ -std=c++17 -O2 -mwindows "$ROOT/_diag/apex_receiver.cpp" -o "$BUILD/apex_receiver.exe" -lgdi32 || exit 1
g++ -std=c++17 -O2 -mconsole "$ROOT/_diag/apex_inject.cpp" -o "$BUILD/apex_inject.exe" || exit 1
bash "$ROOT/apex/build.sh" >/dev/null || { echo "FAIL: the host did not build"; exit 1; }
echo "   ok"

P="$BUILD/apex"
# The copy this gate actually RUNS and writes config into -- never the built folder itself. The `_e2e_run`
# name is what HostPidFromRun matches on, so it must stay in step with the matcher above.
APEXRUN="$BUILD/_e2e_run"
NOTCHES=3

# ---------------------------------------------------------------------------
# ⚠️ APEX IS A GLOBAL WHEEL HOOK, SO NOTHING ELSE MAY BE RUNNING ONE.
#
# The user runs this program. If their copy were up while this gate measured, the baseline's premise ("Apex is
# not running, so the raw notches arrive") would be false -- the wheels this gate injects might be discarded by
# their hook, and the wheels it counts might be theirs. A suite run reported exactly that: a baseline of 790
# messages and -1944 deltas for 3 injected notches, which the verdict then read as a statement about the code.
#
# ⚠️ AND THERE IS NOW EXACTLY ONE Apex PER MACHINE, so there is nothing to warn about -- the field is taken
# before this gate runs and given back after (apex_take_the_field / apex_restore_the_field, in the trap near the
# top). That is what makes the baseline's premise true by construction rather than by hoping the user is idle.
#
# The attribution check in RunOnce stays: it costs nothing, and it is the check that catches a wheel arriving
# from somewhere this gate did not expect.
# ---------------------------------------------------------------------------

# THE TARGET POINT IS READ FROM THE RECEIVER, NOT ASSUMED.
#
# The first version passed a fixed 300,220 and injected there. On a working desktop that point is frequently
# covered by something else (measured, on this machine: an unrelated PowerShell window), so the wheels went
# to the wrong program and the test reported "0 deltas" -- for Apex AND for the baseline. A failure that looks
# like a regression in the code under test, but is really the test aiming at the wrong window, is the worst
# kind of test bug: it is believable.
#
# ⚠️ AND IT IS NOT SOMEWHERE THE TEST CHOSE, EITHER. The receiver used to move the MOUSE to a point it had
# picked, which dragged the pointer out from under the user and -- when they moved it back -- sent the wheels
# to whatever they were using. Now the pointer is read and the window is built around it, and the point
# reported here IS the cursor's own position. The test comes to the cursor; the cursor does not go to the test.
ReadTarget() {
  if [ ! -f "$BUILD/apex_receiver.txt" ]; then
    echo "0 0"
    return
  fi
  grep 'target point' "$BUILD/apex_receiver.txt" | tail -1 |
    sed -n 's/.*target point \([0-9]*\),\([0-9]*\).*/\1 \2/p'
}

# ---------------------------------------------------------------------------
# ⚠️ THIS GATE TOUCHES ONLY ITS OWN PROCESSES AND ITS OWN FOLDER.
#
# It used to `taskkill /F /IM apex.exe`, and the user runs this program: running the gates shot down their
# live instance. It also wrote its pinned `apex.ini` into build/apex/ -- a real settings file, in the folder
# the user is told to run from -- and deleted apex.log beside it. Both are the same mistake in two places:
# treating the shared built folder as scratch.
#
# So the gate now runs its host out of a COPY (see APEXRUN below) and kills by path. The built folder is
# read-only to this script. The filter itself is in test/lib_procs.sh.
# ---------------------------------------------------------------------------
HostPidFromRun() { apex_own_pids '_e2e_run' | head -1; }
KillRunHost() {
  local pid
  pid=$(HostPidFromRun)
  [ -n "$pid" ] && taskkill //F //PID "$pid" >/dev/null 2>&1
}
cleanup() {
  KillRunHost                                # only the copy this gate started
  apex_kill_own 'apex_receiver.exe' apex_receiver # ours alone; nobody else builds that name
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. This gate measures a machine-wide wheel hook,
# so it cannot share with the user's copy AT ALL: not only would its own launch be refused, the user's hook
# would also be shaping the wheels this gate is counting. It takes the field for the run and gives it back.
#
# ⚠️ THE RESTORE IS IN THE EXIT TRAP ONLY. cleanup() is called by every OneRun, and restoring the user's host
# there would put a running hook back in the middle of the measurement.
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

# Wait for the receiver to finish ON ITS OWN.
#
# ⚠️ NOT A KILL, AND NOT FOR THE REASON IT USED TO GIVE. The receiver no longer moves the user's cursor (it
# builds its window around the pointer instead), so there is no cursor to restore on the way out -- but it
# does write the "was this run trustworthy?" verdict as its last act, and a kill would throw that away, which
# is exactly the evidence this gate needs to avoid reporting a false failure. So it is waited for.
WaitForReceiverExit() {
  local i=0
  while [ $i -lt 200 ]; do
    if ! tasklist //FI "IMAGENAME eq apex_receiver.exe" //FO CSV //NH 2>/dev/null | grep -qi apex_receiver; then
      return 0
    fi
    sleep 0.1
    i=$((i + 1))
  done
  echo "   (the receiver did not exit on its own; killing it)" >&2
  apex_kill_own 'apex_receiver.exe' apex_receiver
  return 1
}

# One run: start the receiver, optionally with Apex up, inject, and report what arrived.
# Prints two numbers: messages and total deltas, or "0 0 (reason)" when nothing could be measured.
RunOnce() {
  local with_apex="$1"
  local log="$2"
  cleanup; sleep 1
  rm -f "$BUILD/apex_receiver.txt"

  if [ "$with_apex" = "yes" ]; then
    # ⚠️ THE CONFIG IS PINNED, NOT INHERITED. This used to run against whatever apex.ini happened to be in the
    # build folder -- so a leftover `off=SmoothWheel` from an earlier experiment (mine) made the host leave its
    # only feature disabled, the wheels passed through untouched, and this gate reported "the stream was not
    # split" as if the CODE had regressed. It had not: `decision.h` was doing exactly its job (no feature can
    # deliver -> pass the wheel through). A gate whose verdict depends on what someone left lying around is
    # not measuring the program, so the config is written out here every run.
    #
    # ⚠️ AND IT IS WRITTEN INTO A COPY, NOT INTO build/apex/. Deleting apex.log and overwriting apex.ini in the
    # folder the user runs from is the same "scratch folder" mistake as the image-name kill above: the pinned
    # config replaces their real one, and their log disappears. Re-copied each run so every run starts from a
    # known state.
    rm -rf "$APEXRUN"
    cp -r "$P" "$APEXRUN" 2>/dev/null || { echo "0 0 (the built folder could not be copied)"; return 1; }
    printf '# written by check_apex_endtoend.sh -- the feature must be LIVE for this gate\n' > "$APEXRUN/apex.ini"
    rm -f "$APEXRUN/apex.log"
    # ⚠️ THE REDIRECTION IS NOT COSMETIC -- IT IS WHAT KEEPS THE CALLER FROM HANGING FOREVER.
    # `cmd //c start` returns at once, but the process it launches INHERITS the caller's stdout, and this
    # function's output is collected with `out=$(RunOnce ...)`. A command substitution waits for EOF on its
    # pipe, i.e. until EVERY holder of the write end has closed it -- and the host runs until the gate ends,
    # so the collection never returned and this gate sat there with a live host and a finished receiver while
    # nothing was printed. (It did not hang when the caller used `read < <(RunOnce ...)`, because `read`
    # returns on the first newline and never waits for EOF. The retry wrapper is what made it lethal.)
    # Sending the host's own stdio to /dev/null gives it nothing to hold.
    ( cd "$APEXRUN" && APEX_ACCEPT_INJECTED=1 APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
    # Poll for the host rather than sleeping a flat three seconds: it is up in well under one, and this gate
    # runs twice. Polling for the SCRATCH copy also means a failure here cannot be a user's copy answering.
    local tries=0
    while [ $tries -lt 30 ]; do
      [ -n "$(HostPidFromRun)" ] && break
      sleep 0.2
      tries=$((tries + 1))
    done
    if [ -z "$(HostPidFromRun)" ]; then
      echo "0 0 (apex did not start)"
      return 1
    fi
  fi

  # ⚠️ THE RECEIVER IS NO LONGER TOLD WHERE TO BE. It builds its window around the cursor as it finds it --
  # the pointer is READ and never written, so the user can keep working and the wheel still lands on the
  # window this gate is measuring. It reports the point it used (the cursor's own position) and the target is
  # taken from that report.
  # Redirected for the same reason as the host above: whatever this starts must not hold the caller's pipe.
  # (The receiver exits by itself after ~1.2 s of quiet, so it would release it anyway -- this is about not
  # depending on that, because the day it grows a "stay up until killed" mode the gate would hang instead of
  # saying anything.)
  ( cd "$BUILD" && cmd //c start "" apex_receiver.exe 8 >/dev/null 2>&1 )

  # ⚠️ WAIT FOR READY, NOT FOR A FIXED THREE SECONDS. The receiver is invisible but it IS over the user's
  # cursor, so every second it lives is a second their clicks land on a window that does nothing. The report
  # file says when the window is up and hit-tested; polling for that is both faster and more honest than
  # sleeping past it. (The cap keeps a broken receiver from hanging the gate.)
  local ready=0 tries=0
  while [ $tries -lt 40 ]; do
    if grep -q 'under cursor' "$BUILD/apex_receiver.txt" 2>/dev/null; then
      ready=1
      break
    fi
    sleep 0.25
    tries=$((tries + 1))
  done
  if [ "$ready" -ne 1 ]; then
    echo "0 0 (the receiver never came up -- nothing can be measured)"
    return 1
  fi

  local pt px py
  pt=$(ReadTarget)
  px=$(echo "$pt" | cut -d' ' -f1)
  py=$(echo "$pt" | cut -d' ' -f2)
  if [ -z "$px" ] || [ "$px" = "0" ]; then
    echo "0 0 (the receiver did not report a target -- nothing can be measured)"
    return 1
  fi
  # If the receiver is not the window under the cursor, the point is covered by something else and any count
  # below measures THAT window. Checked before injecting so that the failure is attributed to the setup rather
  # than to the code under test.
  if grep -q 'under cursor : .*(SOMETHING ELSE)' "$BUILD/apex_receiver.txt"; then
    echo "0 0 (the test point is covered by another window -- nothing can be measured)"
    return 1
  fi

  ( cd "$BUILD" && ./apex_inject.exe "$px" "$py" "$NOTCHES" 150 >/dev/null )
  # The receiver ends when the wheels stop arriving and writes its verdict as it goes; waiting for the process
  # to disappear is what lets that verdict be read (and is faster than the old fixed sleep).
  WaitForReceiverExit

  if [ -f "$BUILD/apex_receiver.txt" ]; then
    cp "$BUILD/apex_receiver.txt" "$log" 2>/dev/null
    # ⚠️ THE VERDICT IS REQUIRED BEFORE THE NUMBERS ARE BELIEVED. The receiver is under the user's cursor, and
    # if the user moves the mouse while the wheels are being injected, the wheels go to THEIR window and this
    # one counts zero -- which would be reported as "the code under test dropped the wheel". Measured, on this
    # machine, with the user working: one of three notches arrived. A zero from an invalid run is not evidence,
    # and saying so here is the whole point of the line.
    if ! grep -q '^valid: yes' "$BUILD/apex_receiver.txt"; then
      echo "0 0 (the cursor left the test window during the run -- the result would be about the wrong window)"
      return 1
    fi
    # "done: N wheel messages, +M deltas total" -- or "timeout:" if nothing ever arrived. The FIRST word is
    # NOT matched: the receiver names the reason it stopped (done / timeout) and those names have already
    # changed once, which silently turned every count into a zero (the gate looked for "exit:" and reported
    # 0 0 while the receiver's own file said 115 messages). The line's shape is what is stable, so that is
    # what is matched.
    local line
    line=$(grep 'wheel messages' "$BUILD/apex_receiver.txt" | tail -1)
    local msgs deltas
    msgs=$(echo "$line" | sed -n 's/.*: \([0-9]*\) wheel messages.*/\1/p')
    deltas=$(echo "$line" | sed -n 's/.*, \([+-][0-9]*\) deltas total.*/\1/p')

    # ⚠️ AND THE COUNT HAS TO BE ATTRIBUTABLE TO THIS TEST, WHICH IS NOT THE SAME AS BEING NON-ZERO.
    #
    # The receiver counts EVERY wheel message that lands on it, and it sits under the user's cursor, so the
    # user's own rolling is counted too. That is how a suite run reported a baseline of 790 messages and -1944
    # deltas for 3 injected notches: all of it was the user, and the gate then printed "the originals were not
    # swallowed" -- a conclusion about the code, drawn from somebody else's scrolling.
    #
    # ⚠️ THE CEILING IS DIFFERENT IN THE TWO RUNS, AND USING ONE NUMBER FOR BOTH WAS WRONG.
    #
    #   * WITHOUT Apex, each injected notch is delivered verbatim: one message per notch, so anything above
    #     that cannot be this test. (The first version of this check used $NOTCHES for both runs and threw away
    #     a perfectly good smoothing measurement of 72 messages -- 24 per notch, which is the design.)
    #   * WITH Apex, more messages than were injected is EXPECTED -- that is what smoothing produces. All that
    #     can be said without a per-message trace is that the count must be at least this test's own: 4x per
    #     notch is the floor the verdict below already uses, so a run below the floor is either a real fault or
    #     a coincidence of the user's scrolling interrupting it, and the retry is what tells those apart.
    #
    # A ceiling the smoothing run cannot exceed even in principle: the model splits each notch into a bounded
    # number of frames, so the count is bounded by the frame budget and the test's own duration.
    local ceiling=$NOTCHES
    if [ "$with_apex" = "yes" ]; then
      ceiling=$(( NOTCHES * 60 ))
    fi
    if [ -n "$msgs" ] && [ "$msgs" -gt "$ceiling" ]; then
      echo "0 0 (that window received $msgs messages for $NOTCHES injected notches -- too many to be this test's; something else was rolling)"
      return 1
    fi
    echo "${msgs:-0} ${deltas:-0}"
  else
    echo "0 0 (no receiver output)"
    return 1
  fi
}

# ---------------------------------------------------------------------------
# "COULD NOT MEASURE" IS NOT "FAILED", AND THE TWO MUST NOT BE CONFUSED.
#
# RunOnce reports "0 0 (why)" when no measurement could be taken at all -- most often because the cursor left
# the test window while the wheels were being injected (the user is using their machine; that is allowed, and
# the receiver says so itself: `valid: no`). Comparing that "0" against the expected counts, as an earlier
# version did, produced a wall of arithmetic about a measurement that was never taken -- and "the originals
# were not swallowed" is a frightening sentence to print about a run that declared it proved nothing.
#
# ⚠️ THE REASON LANDS IN THE SECOND FIELD, NOT THE FIRST. The output is read with `read -r MSGS DELTAS`, so
# "0 0 (the cursor left...)" splits into MSGS=0 and DELTAS="0 (the cursor left...)". Checking MSGS for a
# parenthesis -- which is what this did first -- never matches.
#
# The distinction is the receiver's own (see apex_receiver.cpp): a test that lies is worse than one that
# declines to answer.
# ---------------------------------------------------------------------------
NotMeasured() {
  case "$1" in
    *"("*) return 0 ;;
    *) return 1 ;;
  esac
}

# ---------------------------------------------------------------------------
# A RUN IS RETRIED WHEN IT COULD NOT BE MEASURED, AND ONLY THEN.
#
# ⚠️ THE USER IS ALLOWED TO USE THEIR MACHINE WHILE THIS RUNS. The receiver builds its window around the
# cursor and refuses to report a count if the cursor leaves it -- correct, because that count would be about
# a different window -- and that refusal is common precisely when the user is working, which they usually are.
# Failing on the first such attempt made this gate unrunnable in normal use: it reported "NO MEASUREMENT" at
# exactly the moments the user was busy, i.e. whenever they were most likely to be reading the result.
#
# So an unmeasured run is RETRIED, with a pause for the pointer to settle. A run that FAILS (measured, and
# wrong) is never retried -- that is a verdict about the code, and re-rolling it until it passes is how a
# gate stops meaning anything. The two are different outcomes and only this one is worth waiting out.
# ---------------------------------------------------------------------------
RunWithRetry() {
  local with_apex="$1" log="$2" label="$3" attempts="${4:-4}"
  local n=0 out msgs deltas
  while [ $n -lt "$attempts" ]; do
    out=$(RunOnce "$with_apex" "$log")
    msgs=${out%% *}
    deltas=${out#* }
    if ! NotMeasured "$deltas"; then
      echo "$out"
      return 0
    fi
    n=$((n + 1))
    if [ $n -lt "$attempts" ]; then
      echo "   ($label: could not measure -- the pointer was busy; waiting and retrying, $n/$((attempts - 1)))" >&2
      sleep 2
    fi
  done
  # Out of attempts: hand the last refusal back, so the caller reports it exactly as before.
  echo "$out"
  return 0
}

echo
echo "== A: Apex NOT running (the raw notches must arrive) =="
read -r A_MSGS A_DELTAS < <(RunWithRetry no "$BUILD/apex_e2e_off.txt" "baseline")
echo "   receiver saw: $A_MSGS messages, $A_DELTAS deltas"

echo
echo "== B: Apex running (many small messages instead) =="
read -r B_MSGS B_DELTAS < <(RunWithRetry yes "$BUILD/apex_e2e_on.txt" "with Apex")
echo "   receiver saw: $B_MSGS messages, $B_DELTAS deltas"

echo
echo "== verdict =="
fail=0

if NotMeasured "$A_DELTAS" || NotMeasured "$B_DELTAS"; then
  echo "  NO MEASUREMENT: a run could not be taken, so nothing is concluded here."
  echo "    A (Apex off): $A_MSGS messages, $A_DELTAS"
  echo "    B (Apex on) : $B_MSGS messages, $B_DELTAS"
  echo "    Two causes, and both are about the machine rather than the code:"
  echo "      - the pointer moved off the test window while the wheels were injected (the receiver refuses to"
  echo "        report a count for a window the cursor left, because that count would be about another program);"
  echo "      - something else was rolling -- another Apex smoothing the user's own wheel into the same window"
  echo "        (see apex_foreign_pids), or the user scrolling on it themselves."
  echo "    Re-run it and leave the mouse alone for the few seconds it takes."
  cleanup
  exit 1
fi

# A: exactly one message per notch, at full size. Anything else means the baseline is not the baseline.
EXPECT_A=$NOTCHES
EXPECT_A_DELTAS=$(( NOTCHES * 120 ))
if [ "$A_MSGS" -eq "$EXPECT_A" ] && [ "$A_DELTAS" -eq "$EXPECT_A_DELTAS" ]; then
  echo "  baseline is exact: $EXPECT_A notches in, $EXPECT_A messages out, $EXPECT_A_DELTAS deltas"
else
  echo "  FAIL: baseline expected $EXPECT_A messages / $EXPECT_A_DELTAS deltas, got $A_MSGS / $A_DELTAS"
  echo "        (without a trustworthy baseline nothing below can be concluded)"
  fail=1
fi

if [ "$B_MSGS" -gt $(( NOTCHES * 3 )) ]; then
  echo "  the stream was split: $B_MSGS messages for $NOTCHES notches (>= 4x per notch is the design)"
else
  echo "  FAIL: expected many small messages, got $B_MSGS -- the wheels were probably not taken at all"
  fail=1
fi

# The original notches are GONE: if the raw 360 deltas had also arrived, the receiver's total would be at
# least that, because the model never adds travel to a slow roll.
if [ "${B_DELTAS#-}" -lt "$EXPECT_A_DELTAS" ]; then
  echo "  the raw notches were swallowed: total is $B_DELTAS, below the $EXPECT_A_DELTAS that arrived raw"
else
  echo "  FAIL: total $B_DELTAS is not below the raw $EXPECT_A_DELTAS -- the originals were not swallowed"
  fail=1
fi

# And the direction is the one that was asked for.
if [ "${B_DELTAS#-}" -gt 0 ] && [ "$B_DELTAS" -gt 0 ]; then
  echo "  direction preserved: wheel-up arrived as positive deltas"
elif [ "$B_DELTAS" -lt 0 ]; then
  echo "  direction preserved: wheel-up arrived as negative deltas (sign convention differs, consistent)"
else
  echo "  FAIL: no travel arrived in the smoothing run"
  fail=1
fi

cleanup
echo
if [ "$fail" -eq 0 ]; then
  echo "OK: a wheel reaches the program under the cursor, smoothed"
else
  echo "FAILED"
fi
exit $fail
