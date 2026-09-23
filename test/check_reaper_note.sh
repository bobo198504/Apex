#!/usr/bin/env bash
# Gate: THE REAPER NOTE -- absent when it should be, and PRESENT when it should be.
#
# The user asked for this, beside the Exclude list: "检测到Reaper那个Smooth插件（就是上游）在运行，在「排除」右边
# 跟一行颜色灰淡一点的字：REAPER专用插件已运行".
#
# ⚠️ THE POSITIVE CASE IS THE ONE WORTH TESTING. "The note is not there" is also exactly what a detector that
# never says yes prints -- and this machine has no REAPER, so the negative case alone would pass with the
# whole feature deleted. So this gate BUILDS A STAND-IN that satisfies both halves of the host's check: a
# process really named reaper.exe, with a really-loaded module named reaper_smoothwheelscroll*.
#
# ⚠️ AND IT READS THE HOST'S OWN LOG RATHER THAN THE PANEL'S. The host logs the answer as it computes it
# (`reaper check:`), which is the only place that distinguishes "the check ran and said no" from "the check
# never ran". A gate that looked at the note itself could not tell those apart.
#
# ⚠️ NOTHING REAL IS TOUCHED: the stand-in runs from build/ and is killed BY PID -- never by name, because it
# is called reaper.exe and this machine may one day have a real one (see the note in check_apex_icons.sh).
#
# usage: test/check_reaper_note.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file
BUILD="$ROOT/build"
FAKE="$BUILD/_reaper_fake"
RUN="$BUILD/_reaper_run"
fail=0

# `reaper` is in the name list because the stand-in IS called reaper.exe -- the host looks for that name. It is
# safe here only because the path must be inside this gate's scratch folder too: this machine may one day have
# a real REAPER, and that one must never be touched. The filter itself is in test/lib_procs.sh.
PidsIn() { apex_own_pids "$1" 'apex,apex-settings,reaper'; }
# ⚠️ CONDITIONS, NOT SLEEPS (see apex_wait_for in test/lib_procs.sh): "the stand-in is up" and "the host has
# finished starting" are both observable, and waiting for them is what makes this gate fast on an idle machine
# and correct on a busy one. (Measured: the assembly layer used to hold 57 s of fixed sleeps.)
StandInUp() { [ -n "$(PidsIn '_reaper_fake')" ]; }
HostUp()    { [ -n "$(PidsIn '_reaper_run')" ]; }
cleanup() {
  local p
  for p in $(PidsIn '_reaper_fake'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  for p in $(PidsIn '_reaper_run'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. Take the field, give it back at exit.
# (The stand-in is called reaper.exe and is NOT an Apex process, so the field does not touch it.)
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

# Starts a private copy of the host, waits for it to settle, and prints its log.
RunHostAndReadLog() {
  local tag="$1"
  for p in $(PidsIn '_reaper_run'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  sleep 0.5
  rm -rf "$RUN" 2>/dev/null
  cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "COPYFAIL"; return 1; }
  printf '# reaper-note gate\n' > "$RUN/apex.ini"
  rm -f "$RUN"/apex*.log
  ( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
  local t=0
  while [ $t -lt 60 ]; do
    [ -n "$(PidsIn '_reaper_run')" ] && break
    sleep 0.2
    t=$((t + 1))
  done
  # ⚠️ THE QUERY IS MADE BY ASKING THE HOST FOR THE FEATURE'S CONTROLS, through the real IPC -- NOT by
  # opening the panel and clicking into a feature page. Clicking is UI interaction, which in this project
  # belongs to the user. This calls the same `describe` the page calls, from a probe with its own window.
  #
  # The feature builds its settings document when it is ASKED, and that is the moment it queries the host --
  # so this call is what makes the check run at all.
  #
  # ⚠️ THE SLOT IS FOUND, NOT ASSUMED. Features are numbered by FOLDER NAME, so the slot depends on what else
  # is in the tree -- SmoothWheel was slot 1 here because the audit's stand-in feature (features/AuditStub)
  # sorts first. Asking a fixed slot silently measured the wrong feature: it returned a valid document, so
  # nothing looked broken, and the check simply never ran. The slot is read out of the snapshot by id.
  # The slot is derived the way the LOADER does it: the folders under Plugins/, sorted, one slot each. (The
  # snapshot carries the same map, but reading it would mean parsing a document in shell for one number.)
  local slot="" i=0
  for d in "$RUN"/Plugins/*/; do
    if [ "$(basename "$d")" = "SmoothWheel" ]; then slot=$i; break; fi
    i=$((i + 1))
  done
  "$BUILD/_describe_probe.exe" "$RUN" "$slot" >/dev/null 2>&1
  # The host answers a describe by writing its `reaper check:` line -- wait for the LINE, not for a second.
  apex_wait_line "$RUN/apex.log" 'reaper check:' 8
  grep 'reaper check:' "$RUN/apex.log" 2>/dev/null | tail -2
  for p in $(PidsIn '_reaper_run'); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  sleep 0.5
}

echo "== building the probes =="
mkdir -p "$FAKE"
g++ -std=c++17 -O2 -mwindows -o "$FAKE/_fake.exe" "$ROOT/_diag/apex_reaper_fake.cpp" -luser32 || exit 1
g++ -std=c++17 -O2 -mconsole -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$BUILD/_describe_probe.exe" \
    "$ROOT/_diag/apex_describe_probe.cpp" -luser32 || exit 1
g++ -std=c++17 -O2 -shared -o "$FAKE/reaper_smoothwheelscroll_fake.dll" -x c++ - <<'EOF' || exit 1
extern "C" __declspec(dllexport) int marker(void) { return 1; }
EOF
cp "$FAKE/_fake.exe" "$FAKE/reaper.exe"
echo "   ok  ($FAKE/reaper.exe + reaper_smoothwheelscroll_fake.dll)"

echo
echo "== 1. with NOTHING running, the host must say so =="
out=$(RunHostAndReadLog "nothing")
if printf '%s' "$out" | grep -q "not running"; then
  echo "   ok  $(printf '%s' "$out" | tail -1)"
else
  echo "   FAIL: the host did not report 'reaper.exe is not running'. It said:"
  printf '%s\n' "$out" | sed 's/^/        /'
  fail=1
fi

echo
echo "== 2. with the stand-in running, the host must find the PLUGIN =="
( cd "$FAKE" && cmd //c start "" "$FAKE/reaper.exe" >/dev/null 2>&1 )
apex_wait_for 5 StandInUp
if [ -z "$(PidsIn '_reaper_fake')" ]; then
  echo "   FAIL: the stand-in did not start"
  exit 1
fi
echo "   stand-in pid $(PidsIn '_reaper_fake' | head -1), module reaper_smoothwheelscroll_fake.dll"
out=$(RunHostAndReadLog "stand-in")
if printf '%s' "$out" | grep -q "the plugin is running"; then
  echo "   ok  $(printf '%s' "$out" | tail -1)"
else
  echo "   FAIL: the host did not find the plugin in the stand-in. It said:"
  printf '%s\n' "$out" | sed 's/^/        /'
  fail=1
fi

echo
echo "== 3. the three ends exist (static) =="
if grep -q "reaperPluginRunning" "$ROOT/apex/abi.h" &&
   grep -q "ReaperPluginIsRunning" "$ROOT/apex/main.cpp"; then
  echo "   ok  the ABI carries the question and the host answers it"
else
  echo "   FAIL: the ABI call or its host implementation is missing"
  fail=1
fi
if grep -q "noteZh" "$ROOT/features/SmoothWheel/feature_smoothwheel.cpp" &&
   grep -q "reaperPluginRunning" "$ROOT/features/SmoothWheel/feature_smoothwheel.cpp"; then
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

echo
# THE SENTENCE THE USER ASKED FOR: REAPER专用插件已运行, as UTF-8 BYTES.
#
# ⚠️ THE COMPARISON IS DONE IN HEX, AND THAT IS NOT FUSSINESS. Two earlier versions of this line wrote the
# characters (or their printf escapes) directly, and both compared the feature's correct note against
# mojibake produced by this script's own encoding -- so the gate failed a note that was right. Hex is the one
# representation that survives being edited, copied and run under any codepage.
NOTE_WANT_HEX="524541504552e4b893e794a8e68f92e4bbb6e5b7b2e8bf90e8a18c"

# Starts the stand-in, runs a private host, asks it for SmoothWheel's controls, and prints the note the
# feature sent. This is the whole chain in one call: process walk -> module scan -> ABI -> settings document.
ReadNoteWithStandIn() {
  ( cd "$FAKE" && cmd //c start "" "$FAKE/reaper.exe" >/dev/null 2>&1 )
  apex_wait_for 5 StandInUp
  for q in $(PidsIn '_reaper_run'); do taskkill //F //PID "$q" >/dev/null 2>&1; done
  sleep 0.5
  rm -rf "$RUN" 2>/dev/null
  cp -r "$BUILD/apex" "$RUN" 2>/dev/null || return 1
  printf '# reaper-note gate
' > "$RUN/apex.ini"
  rm -f "$RUN"/apex*.log
  ( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
  local t=0
  while [ $t -lt 60 ]; do
    [ -n "$(PidsIn '_reaper_run')" ] && break
    sleep 0.2; t=$((t + 1))
  done
  # The process being up is not the same as the host being ready to answer: its startup ends with this line.
  apex_wait_line "$RUN/apex.log" 'engine running' 10
  # The slot, the way the loader assigns it: Plugins/ subfolders, sorted, one each.
  local slot="" i=0
  for d in "$RUN"/Plugins/*/; do
    if [ "$(basename "$d")" = "SmoothWheel" ]; then slot=$i; break; fi
    i=$((i + 1))
  done
  [ -n "$slot" ] || slot=0
  "$BUILD/_describe_probe.exe" "$RUN" "$slot" 2>/dev/null |
    node -e 'let s="";process.stdin.on("data",(d)=>(s+=d)).on("end",()=>{try{const D=JSON.parse(s);const l=(D.params||[]).find((q)=>q.type==="list");process.stdout.write(l&&l.noteZh?Buffer.from(l.noteZh,"utf8").toString("hex"):"")}catch(e){process.stdout.write("")}});'
  for q in $(PidsIn '_reaper_run'); do taskkill //F //PID "$q" >/dev/null 2>&1; done
  for q in $(PidsIn '_reaper_fake'); do taskkill //F //PID "$q" >/dev/null 2>&1; done
}

echo "== 4. the note is the user's own sentence, read from what the feature SENDS =="
# ⚠️ READ OUT OF THE LIVE ANSWER, NOT GREPPED OUT OF THE SOURCE. The source writes the sentence as hex escapes
# ("REA..." spells REAPER), so a search for the literal text finds nothing and reports a missing
# note that is right there -- a gate failing on its own quoting rather than on the product.
sent_hex=$(ReadNoteWithStandIn)
sent_text=$(printf %s "$sent_hex" | node -e 'let s="";process.stdin.on("data",(d)=>(s+=d)).on("end",()=>process.stdout.write(Buffer.from(s.trim(),"hex").toString("utf8")))' 2>/dev/null)
if [ "$sent_hex" = "$NOTE_WANT_HEX" ]; then
  echo "   ok  the feature sends the user's sentence, verbatim: $sent_text"
else
  echo "   FAIL: the note sent is not the requested sentence"
  echo "        got (hex):      $sent_hex"
  echo "        expected (hex): $NOTE_WANT_HEX"
  echo "        got (text):     $sent_text"
  fail=1
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: the check runs, answers both ways correctly, and the note is wired through all three places"
else
  echo "FAILED: the REAPER note"
fi
exit $fail
