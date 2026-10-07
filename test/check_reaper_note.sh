#!/usr/bin/env bash
# Gate: THE ENGINE NOTE BESIDE 「排除」 -- absent when it should be, and PRESENT, IN THE RIGHT ORDER, when it
# should be. (The file keeps its old name because REAPER is still the case it was written for; what it checks is
# the whole engine set now.)
#
# The user asked for this first, beside the Exclude list: "检测到Reaper那个Smooth插件（就是上游）在运行，在「排除」右边
# 跟一行颜色灰淡一点的字：REAPER专用插件已运行". It has since become a SET, because Lertaro also brings its own
# smooth scrolling and publishes a marker while it is on:
#
#   "改成「REAPER专用引擎已运行」，多个APP就写一块：「REAPER、Lertaro专用引擎已运行」。谁先运行谁显式在前面。"
#
# ⚠️ SO FOUR DIRECTIONS ARE CHECKED, and the last two are what a single-engine detector would pass by accident:
#   1. nothing running        -> no note at all;
#   2. REAPER's stand-in      -> "REAPER专用引擎已运行";
#   3. Lertaro's marker       -> "Lertaro专用引擎已运行";
#   4. BOTH, started in a KNOWN order -> the sentence names them IN THAT ORDER (both ways round).
#
# ⚠️ THE POSITIVE CASES ARE THE ONES WORTH TESTING. "The note is not there" is also exactly what a detector that
# never says yes prints -- and this machine has no REAPER -- so the negative case alone would pass with the whole
# feature deleted. So this gate builds STAND-INS: a process really named reaper.exe with a really-loaded module
# named reaper_smoothwheelscroll*, and a process really named Lertaro.App.exe holding the named event Lertaro
# publishes (`Local\Lertaro.SmoothScroll.Active`).
#
# ⚠️ THE ORDER IS READ FROM PROCESS START TIMES, so the stand-ins are started a second apart on purpose: closer
# than that and the answer would depend on the OS's ~15 ms clock rather than on the gate.
#
# ⚠️ AND IT READS THE HOST'S OWN LOG RATHER THAN THE PANEL'S. The host logs the answer as it computes it
# (`engines:`), which is the only place that distinguishes "the check ran and said no" from "the check never
# ran". A gate that looked at the note itself could not tell those apart.
#
# ⚠️ NOTHING REAL IS TOUCHED: the stand-ins run from build/ and are killed BY PID -- never by name, because one
# of them is called reaper.exe and this machine may one day have a real one (see check_apex_icons.sh).
#
# usage: test/check_reaper_note.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file
BUILD="$ROOT/build"
FAKE="$BUILD/_reaper_fake"
LFAKE="$BUILD/_lertaro_fake"
RUN="$BUILD/_reaper_run"
fail=0

# `reaper` and `Lertaro.App` are in the name list because the stand-ins ARE called those -- the host looks for
# exactly those names. It is safe only because the path must be inside this gate's scratch folder too: this
# machine may one day have a real REAPER (and a real Lertaro), and those must never be touched. The filter itself
# is in test/lib_procs.sh.
PidsIn() { apex_own_pids "$1" 'apex,apex-settings,reaper,Lertaro.App'; }
# ⚠️ CONDITIONS, NOT SLEEPS (see apex_wait_for in test/lib_procs.sh): "the stand-in is up" and "the host has
# finished starting" are both observable, and waiting for them is what makes this gate fast on an idle machine
# and correct on a busy one. (Measured: the assembly layer used to hold 57 s of fixed sleeps.)
StandInUp() { [ -n "$(PidsIn '_reaper_fake')" ]; }
MarkerUp()  { [ -n "$(PidsIn '_lertaro_fake')" ]; }

KillStandIns() {
  local p
  for p in $(PidsIn '_reaper_fake'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  for p in $(PidsIn '_lertaro_fake'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
cleanup() {
  KillStandIns
  local p
  for p in $(PidsIn '_reaper_run'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. Take the field, give it back at exit.
# (The stand-ins are called reaper.exe / Lertaro.App.exe and are NOT Apex processes, so the field does not touch
# them -- but the private host copies do need the machine to themselves.)
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

# The slot SmoothWheel occupies in a snapshot, derived the way the LOADER does it: the folders under Plugins/,
# sorted, one slot each. (The snapshot carries the same map, but reading it would mean parsing a document in
# shell for one number.) ⚠️ IT IS FOUND, NOT ASSUMED: SmoothWheel was slot 1 here because the audit's stand-in
# feature (features/AuditStub) sorts first, and asking a fixed slot silently measured the wrong feature -- it
# returned a valid document, so nothing looked broken and the check simply never ran.
SmoothWheelSlot() {
  local i=0 d
  for d in "$1"/Plugins/*/; do
    if [ "$(basename "$d")" = "SmoothWheel" ]; then echo "$i"; return; fi
    i=$((i + 1))
  done
  echo 0
}

# Starts a private copy of the host, asks it for SmoothWheel's controls through the REAL IPC, and prints the
# `engines:` log lines.
#
# ⚠️ THE QUERY IS MADE BY ASKING THE HOST FOR THE FEATURE'S CONTROLS, through the real IPC -- NOT by opening the
# panel and clicking into a feature page. Clicking is UI interaction, which in this project belongs to the user.
# This calls the same `describe` the page calls, from a probe with its own window. The feature builds its
# settings document when it is ASKED, and that is the moment it queries the host -- so this call is what makes
# the check run at all.
RunHostAndReadLog() {
  for p in $(PidsIn '_reaper_run'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  sleep 0.5
  rm -rf "$RUN" 2>/dev/null
  cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "COPYFAIL"; return 1; }
  printf '# engine-note gate\n' > "$RUN/apex.ini"
  rm -f "$RUN"/apex*.log
  ( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
  local t=0
  while [ $t -lt 60 ]; do
    [ -n "$(PidsIn '_reaper_run')" ] && break
    sleep 0.2
    t=$((t + 1))
  done
  "$BUILD/_describe_probe.exe" "$RUN" "$(SmoothWheelSlot "$RUN")" >/dev/null 2>&1
  # The host answers a describe by writing its `engines:` line -- wait for the LINE, not for a second.
  apex_wait_line "$RUN/apex.log" 'engines:' 8
  grep 'engines:' "$RUN/apex.log" 2>/dev/null | tail -2
  for p in $(PidsIn '_reaper_run'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  sleep 0.5
}

# Starts ONE stand-in: "reaper" (the fake with the module) or "lertaro" (the marker's owner).
StartStandIn() {
  if [ "$1" = "reaper" ]; then
    ( cd "$FAKE" && cmd //c start "" "$FAKE/reaper.exe" >/dev/null 2>&1 )
    apex_wait_for 5 StandInUp
  else
    ( cd "$LFAKE" && cmd //c start "" "$LFAKE/Lertaro.App.exe" 60 >/dev/null 2>&1 )
    apex_wait_for 5 MarkerUp
  fi
}

# Starts the given stand-ins IN THIS ORDER (a second apart -- see the header), runs a private host, asks it for
# SmoothWheel's controls, and prints the note the feature sent as HEX.
#
# ⚠️ THE COMPARISON IS DONE IN HEX, AND THAT IS NOT FUSSINESS. Two earlier versions of this check wrote the
# characters (or their printf escapes) directly, and both compared the feature's correct note against mojibake
# produced by this script's own encoding -- so the gate failed a note that was right. Hex is the one
# representation that survives being edited, copied and run under any codepage.
ReadNoteWith() {
  KillStandIns
  local s
  for s in "$@"; do
    StartStandIn "$s"
    sleep 1.2
  done
  for q in $(PidsIn '_reaper_run'); do taskkill //F //PID "$q" >/dev/null 2>&1; done
  rm -rf "$RUN" 2>/dev/null
  cp -r "$BUILD/apex" "$RUN" 2>/dev/null || return 1
  printf '# engine-note gate\n' > "$RUN/apex.ini"
  rm -f "$RUN"/apex*.log
  ( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
  local t=0
  while [ $t -lt 60 ]; do
    [ -n "$(PidsIn '_reaper_run')" ] && break
    sleep 0.2; t=$((t + 1))
  done
  # The process being up is not the same as the host being ready to answer: its startup ends with this line.
  apex_wait_line "$RUN/apex.log" 'engine running' 10
  "$BUILD/_describe_probe.exe" "$RUN" "$(SmoothWheelSlot "$RUN")" 2>/dev/null |
    node -e 'let s="";process.stdin.on("data",(d)=>(s+=d)).on("end",()=>{try{const D=JSON.parse(s);const l=(D.params||[]).find((q)=>q.type==="list");process.stdout.write(l&&l.noteZh?Buffer.from(l.noteZh,"utf8").toString("hex"):"")}catch(e){process.stdout.write("")}});'
  for q in $(PidsIn '_reaper_run'); do taskkill //F //PID "$q" >/dev/null 2>&1; done
  KillStandIns
}

# THE SENTENCE, one per direction, as UTF-8 BYTES. Same tail, the names in the order the engines started.
NOTE_REAPER_ONLY="524541504552e4b893e794a8e5bc95e6938ee5b7b2e8bf90e8a18c"                       # REAPER专用引擎已运行
NOTE_LERTARO_ONLY="4c65727461726fe4b893e794a8e5bc95e6938ee5b7b2e8bf90e8a18c"                     # Lertaro专用引擎已运行
NOTE_REAPER_FIRST="524541504552e380814c65727461726fe4b893e794a8e5bc95e6938ee5b7b2e8bf90e8a18c"   # REAPER、Lertaro…
NOTE_LERTARO_FIRST="4c65727461726fe38081524541504552e4b893e794a8e5bc95e6938ee5b7b2e8bf90e8a18c"   # Lertaro、REAPER…

HexText() {
  printf %s "$1" | node -e 'let s="";process.stdin.on("data",(d)=>(s+=d)).on("end",()=>process.stdout.write(Buffer.from(s.trim(),"hex").toString("utf8")))' 2>/dev/null
}

echo "== building the probes =="
mkdir -p "$FAKE" "$LFAKE"
g++ -std=c++17 -O2 -mwindows -o "$FAKE/_fake.exe" "$ROOT/_diag/apex_reaper_fake.cpp" -luser32 || exit 1
g++ -std=c++17 -O2 -mconsole -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$BUILD/_describe_probe.exe" \
    "$ROOT/_diag/apex_describe_probe.cpp" -luser32 || exit 1
g++ -std=c++17 -O2 -shared -o "$FAKE/reaper_smoothwheelscroll_fake.dll" -x c++ - <<'EOF' || exit 1
extern "C" __declspec(dllexport) int marker(void) { return 1; }
EOF
cp "$FAKE/_fake.exe" "$FAKE/reaper.exe"
# ⚠️ THE MARKER'S OWNER MUST BE NAMED Lertaro.App.exe: that name is how the host finds the PROCESS whose start
# time orders it (the marker itself is what says it is running). A differently named stand-in would still be
# REPORTED -- and would sort last, because its start time would be unknown.
g++ -std=c++17 -O2 -mconsole -o "$LFAKE/_marker.exe" "$ROOT/_diag/lertaro_marker_fake.cpp" -luser32 || exit 1
cp "$LFAKE/_marker.exe" "$LFAKE/Lertaro.App.exe"
echo "   ok  ($FAKE/reaper.exe + reaper_smoothwheelscroll_fake.dll, $LFAKE/Lertaro.App.exe + its marker)"

# ⚠️ THE MARKER HAS TO BE ABSENT, OR THIS GATE MEASURES THE USER'S PROGRAM INSTEAD OF ITS FIXTURE. Lertaro holds
# the same named event, and the note names EVERY engine that is running -- so with a real Lertaro up, "REAPER
# alone", "Lertaro alone" and "nothing running" are all unreachable states, and every assertion below would be
# about whatever the user happened to have open. Said out loud rather than passing quietly (see the project's rule
# about a gate that cannot fail).
if "$LFAKE/_marker.exe" --check >/dev/null 2>&1; then
  echo
  echo "SKIPPED: the engine note -- a real Lertaro is running and holds the engine marker, so the engine sets this"
  echo "         gate asserts on cannot be produced. Nothing was checked."
  exit 0
fi

echo
echo "== 1. with NOTHING running, the host must say so =="
KillStandIns
out=$(RunHostAndReadLog)
if printf '%s' "$out" | grep -q "^engines: none running"; then
  echo "   ok  $(printf '%s' "$out" | tail -1)"
else
  echo "   FAIL: the host did not report that nothing is running. It said:"
  printf '%s\n' "$out" | sed 's/^/        /'
  fail=1
fi

echo
echo "== 2. with the REAPER stand-in running, the host must find the PLUGIN =="
StartStandIn reaper
if [ -z "$(PidsIn '_reaper_fake')" ]; then
  echo "   FAIL: the REAPER stand-in did not start"
  exit 1
fi
echo "   stand-in pid $(PidsIn '_reaper_fake' | head -1), module reaper_smoothwheelscroll_fake.dll"
out=$(RunHostAndReadLog)
if printf '%s' "$out" | grep -q "^engines: REAPER$"; then
  echo "   ok  $(printf '%s' "$out" | tail -1)"
else
  echo "   FAIL: the host did not name REAPER alone. It said:"
  printf '%s\n' "$out" | sed 's/^/        /'
  fail=1
fi

echo
echo "== 3. with LERTARO's marker running, the host must find the marker =="
KillStandIns
StartStandIn lertaro
if [ -z "$(PidsIn '_lertaro_fake')" ]; then
  echo "   FAIL: the Lertaro stand-in did not start"
  exit 1
fi
echo "   marker pid $(PidsIn '_lertaro_fake' | head -1), event Local\\Lertaro.SmoothScroll.Active"
out=$(RunHostAndReadLog)
if printf '%s' "$out" | grep -q "^engines: Lertaro$"; then
  echo "   ok  $(printf '%s' "$out" | tail -1)"
else
  echo "   FAIL: the host did not name Lertaro alone. It said:"
  printf '%s\n' "$out" | sed 's/^/        /'
  fail=1
fi
KillStandIns

echo
echo "== 4. the three ends exist (static) =="
if grep -q "activeEngines" "$ROOT/apex/abi.h" &&
   grep -q "HostActiveEngines" "$ROOT/apex/main.cpp"; then
  echo "   ok  the ABI carries the engine list and the host answers it"
else
  echo "   FAIL: the ABI call or its host implementation is missing"
  fail=1
fi
if grep -q "noteZh" "$ROOT/features/SmoothWheel/feature_smoothwheel.cpp" &&
   grep -q "activeEngines" "$ROOT/features/SmoothWheel/feature_smoothwheel.cpp"; then
  echo "   ok  the feature asks, and sends the answer as the list control's note"
else
  echo "   FAIL: the feature does not send the note"
  fail=1
fi
if grep -q "rownote" "$ROOT/build/panel.built.html"; then
  echo "   ok  the panel draws a note beside a label when given one"
else
  echo "   FAIL: the panel has no way to draw the note"
  fail=1
fi

# One direction of the sentence: start the stand-ins in order, read what the feature actually SENT, compare hex.
CheckNote() {
  local want="$1" what="$2"
  shift 2
  local got
  got=$(ReadNoteWith "$@")
  if [ "$got" = "$want" ]; then
    echo "   ok  $what -> $(HexText "$got")"
  else
    echo "   FAIL: $what"
    echo "        got (hex):      $got"
    echo "        expected (hex): $want"
    echo "        got (text):     $(HexText "$got")"
    fail=1
  fi
}

echo
echo "== 5. the note is the user's sentence, read from what the feature SENDS =="
# ⚠️ READ OUT OF THE LIVE ANSWER, NOT GREPPED OUT OF THE SOURCE. The source writes the sentence as hex escapes,
# so a search for the literal text finds nothing and reports a missing note that is right there -- a gate failing
# on its own quoting rather than on the product.
CheckNote "$NOTE_REAPER_ONLY"   "REAPER alone"   reaper
CheckNote "$NOTE_LERTARO_ONLY" "Lertaro alone"  lertaro

echo
echo "== 6. BOTH engines, and the one that started FIRST is named first =="
CheckNote "$NOTE_REAPER_FIRST"   "REAPER started first"  reaper lertaro
CheckNote "$NOTE_LERTARO_FIRST" "Lertaro started first" lertaro reaper

echo
if [ $fail -eq 0 ]; then
  echo "OK: the check runs, names the right engines in both directions, and the sentence is wired through all"
  echo "    three places -- and it names them in START order"
else
  echo "FAILED: the engine note"
fi
exit $fail
