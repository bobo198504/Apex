#!/usr/bin/env bash
# THE DELIVERED STREAM, TIMED -- the end-to-end measurement of "does it jump".
#
# WHY THIS EXISTS AND WHY THE EARLIER PROBES WERE NOT ENOUGH: everything before this measured a MODEL REPLICA
# (a C++ file replicating common/core.h in a synthetic loop). That answers "is the arithmetic smooth", which is
# not the same question as "is what arrives smooth", because between the model's output and the receiving
# window sit the host's engine period, its per-frame carry and rounding, its injection queue, and the OS's
# dispatch. The user reports a jump at a fast scroll; the model measures clean; so the measurement has to move
# to the only place that sees the whole path -- the messages themselves, timed as they arrive.
#
# It runs the REAL program (a private copy), drives a REAL wheel through it, and records the arrival time of
# every message the receiver gets. Then the rate is computed from those times and the biggest single-frame step
# is reported, per configuration.
#
# ⚠️ IT INJECTS SYNTHETIC WHEELS AND MOVES NOTHING: the injector sends MOUSEEVENTF_WHEEL to the receiver's own
# point, the receiver's window is built AROUND the cursor and the cursor is never moved. The copy runs with
# APEX_ACCEPT_INJECTED=1, which is the existing test-only mode.
#
# usage: bash _diag/apex_delivery_probe.sh [ramp] [gapMs] [notches]
#        (defaults: 1000, 8, 60 -- the user's settings and a fast flick)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file
BUILD="$ROOT/build"
RUN="$BUILD/_delivery_run"

PidsInRun() { apex_own_pids '_delivery_run'; }
cleanup() {
  local p
  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  apex_kill_own 'apex_receiver.exe' apex_receiver # ours alone; nobody else builds that name
}
trap cleanup EXIT

echo "== building =="
g++ -std=c++17 -O2 -mwindows "$ROOT/_diag/apex_receiver.cpp" -o "$BUILD/apex_receiver.exe" -lgdi32 || exit 1
g++ -std=c++17 -O2 -mconsole "$ROOT/_diag/apex_inject.cpp" -o "$BUILD/apex_inject.exe" || exit 1
echo "   ok"

# The injector's own gap control: it takes <x> <y> <notches> <gapMs>. Checked here rather than assumed.
"$BUILD/apex_inject.exe" 2>&1 | head -3 | sed 's/^/   injector usage: /'

# ONE RUN: start the host with a pinned config, start the receiver, inject, read the trace.
# $1 = ramp   $2 = gapMs   $3 = notches   $4 = "off" to disable the feature (the raw-passthrough control)
OneRun() {
  local ramp="$1" gap="$2" notches="$3" off="${4:-}"
  cleanup
  sleep 0.5
  rm -rf "$RUN"
  cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "   copy failed"; return 1; }
  if [ "$off" = "off" ]; then
    printf '# pinned by apex_delivery_probe.sh -- THE FEATURE IS OFF (the raw-passthrough control)\noff=SmoothWheel\n' > "$RUN/apex.ini"
  else
    printf '# pinned by apex_delivery_probe.sh\n' > "$RUN/apex.ini"
  fi
  rm -f "$RUN/apex.log"
  # THE FEATURE'S OWN SETTINGS: ramp is the parameter under test, the rest are the shipped defaults.
  printf 'glide=200\nslow=5.0\nramp=%s\ntop=1.50\n' "$ramp" > "$RUN/Plugins/SmoothWheel/SmoothWheel.ini"

  ( cd "$RUN" && APEX_ACCEPT_INJECTED=1 cmd //c start "" apex.exe )
  sleep 3

  rm -f "$BUILD/apex_receiver.txt" "$BUILD/trace.txt"
  ( cd "$BUILD" && APEX_RECV_TRACE="$BUILD/trace.txt" cmd //c start "" apex_receiver.exe 6 )

  # Wait for the receiver's window to be up and hit-tested.
  local tries=0
  while [ $tries -lt 60 ]; do
    grep -q 'under cursor' "$BUILD/apex_receiver.txt" 2>/dev/null && break
    sleep 0.2
    tries=$((tries + 1))
  done
  local pt px py
  pt=$(grep 'target point' "$BUILD/apex_receiver.txt" | tail -1 |
    sed -n 's/.*target point \([0-9-]*\),\([0-9-]*\).*/\1 \2/p')
  px=$(echo "$pt" | cut -d' ' -f1); py=$(echo "$pt" | cut -d' ' -f2)
  if [ -z "$px" ]; then echo "   no target point"; return 1; fi

  "$BUILD/apex_inject.exe" "$px" "$py" "$notches" "$gap" >/dev/null 2>&1

  # Wait for the receiver to finish on its own (it ends 1.2 s after the last message).
  tries=0
  while [ $tries -lt 100 ]; do
    tasklist //FI "IMAGENAME eq apex_receiver.exe" //FO CSV //NH 2>/dev/null | grep -qi apex_receiver || break
    sleep 0.1
    tries=$((tries + 1))
  done
  sleep 0.3
  cleanup
}

# THE ANALYSIS: from the arrival times, the rate between messages, and the biggest single step in it.
# A "jump" here is what actually arrives: |rate[i] - rate[i-1]| where rate = delta / gap-to-previous.
Analyze() {
  local label="$1" trace="$2"
  if [ ! -f "$trace" ]; then echo "   $label: NO TRACE"; return; fi
  node -e '
    const fs=require("fs");
    const lines=fs.readFileSync(process.argv[1],"utf8").split("\n").filter(l=>l && l[0]!=="#");
    if (lines.length < 4) { console.log("   " + process.argv[2] + ": only " + lines.length + " messages"); process.exit(0); }
    const t=[], d=[];
    for (const l of lines) { const p=l.trim().split(/\s+/); t.push(+p[0]/1000); d.push(+p[1]); } // ms, deltas
    // ⚠️ THE RAW LEAKS ARE COUNTED SEPARATELY, because they are a DIFFERENT FAULT from a smooth-stream ripple and
    // the two are easy to confuse in one number. A raw notch is a message the host did not swallow; it arrives
    // at the wheel own size (120 deltas, the injector sends nothing else) and it is followed by a smoothed
    // message at a nearly-zero interval, which is what makes the rate spike. Counting them here is what says
    // which fault this is.
    let raw=0; for (const x of d) if (Math.abs(x) >= 100) raw++;
    // The delivered rate at message i: deltas per millisecond since the previous message.
    const r=[];
    for (let i=1;i<t.length;i++) { const dt=t[i]-t[i-1]; r.push(dt>0 ? d[i]/dt : 0); }
    // The biggest single step between consecutive rates, as a fraction of the peak rate.
    const peak=Math.max(...r.map(Math.abs));
    let worst=0, at=-1;
    for (let i=1;i<r.length;i++) { const s=Math.abs(r[i]-r[i-1]); if (s>worst) { worst=s; at=i; } }
    const gaps=[];
    for (let i=1;i<t.length;i++) gaps.push(t[i]-t[i-1]);
    gaps.sort((a,b)=>a-b);
    const sum=t[t.length-1]-t[0];
    console.log("   " + process.argv[2].padEnd(22) +
      " msgs " + String(t.length).padStart(4) +
      "  raw notches leaked " + String(raw).padStart(3) +
      "  span " + sum.toFixed(0).padStart(5) + " ms" +
      "  median gap " + gaps[Math.floor(gaps.length/2)].toFixed(2).padStart(6) + " ms" +
      "  peak " + peak.toFixed(3).padStart(8) + " d/ms" +
      "  jump " + (100*worst/peak).toFixed(1).padStart(5) + "%");
    // The three biggest steps, with the rates around them, so a number can be checked against its shape.
    const steps=[];
    for (let i=1;i<r.length;i++) steps.push([Math.abs(r[i]-r[i-1]), i]);
    steps.sort((a,b)=>b[0]-a[0]);
    if (steps.length && steps[0][0]/peak > 0.08) {
      const i=steps[0][1];
      console.log("       biggest step at message " + i + ": rates " +
        r.slice(Math.max(0,i-3),i).map(x=>x.toFixed(2)).join(" ") + " [" + r[i-1].toFixed(2) + " -> " + r[i].toFixed(2) + "] " +
        r.slice(i+1,i+3).map(x=>x.toFixed(2)).join(" "));
      console.log("       arrival gaps there: " + gaps.slice(0,0).join("") +
        (()=>{ const g=[]; for(let k=Math.max(1,i-3);k<=Math.min(t.length-1,i+3);k++) g.push((t[k]-t[k-1]).toFixed(1)); return g.join(" "); })() + " ms");
    }
  ' "$trace" "$label"
}

RAMP="${1:-1000}"
GAP="${2:-8}"
NOTCHES="${3:-60}"

echo
echo "== a real fast roll through the real program: ramp=$RAMP gap=${GAP}ms notches=$NOTCHES =="

echo "   -- CONTROL: the feature is OFF, so nothing is swallowed and the raw stream arrives --"
OneRun "$RAMP" "$GAP" "$NOTCHES" off
Analyze "control (feature off)" "$BUILD/trace.txt"
cp "$BUILD/trace.txt" "$BUILD/trace_off.txt"

echo "   -- SHIPPING: the feature on --"
OneRun "$RAMP" "$GAP" "$NOTCHES"
Analyze "SHIPPING (2 axes)" "$BUILD/trace.txt"
cp "$BUILD/trace.txt" "$BUILD/trace_shipping.txt"

echo
echo "== what the receiver saw (its own verdict) =="
grep -E 'wheel messages|valid:|under cursor' "$BUILD/apex_receiver.txt" 2>/dev/null | sed 's/^/   /'
echo
echo "traces kept: $BUILD/trace_shipping.txt"
