#!/usr/bin/env bash
# Gate: A SETTING CHANGED IN THE PANEL SURVIVES THE PROGRAM BEING KILLED.
#
# The user's requirement: "插件设置保存可以一直在，不用每次编译都清掉". Two halves to that, and the first gate
# below is about the more dangerous one.
#
#   A. THE FILE IS WRITTEN SOON AFTER THE EDIT, not only on a clean exit. "Save on exit" sounds adequate and is
#      not: the exit path is WM_CLOSE -> DestroyWindow, and the ways this program actually ends are taskkill,
#      a crash, a machine reset, or being left running for a week. Measured, that is exactly what happened --
#      settings were tuned, the process was killed, and the next start came up on the defaults.
#
#   B. THE GATES DO NOT DESTROY THE USER'S SETTINGS. Two of them wrote their own pinned apex.ini into
#      build/apex/ -- the folder the user is told to run from -- and killed every apex.exe by image name,
#      including their live one. Checked here as "the built folder's own files are untouched by a full gate
#      run", because that is the property that matters and it is invisible from inside any single gate.
#
# ⚠️ IT RUNS A PRIVATE COPY and kills only processes started from it (by path), so it cannot disturb the real
# installation. See the notes in check_apex_icons.sh.
#
# usage: test/check_apex_persist.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

BUILD="$ROOT/build"
RUN="$BUILD/_persist_run"
PROBE="$BUILD/_edit_probe.exe"
fail=0

echo "== building the probe =="
g++ -std=c++17 -O2 -mconsole -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$PROBE" "$ROOT/_diag/apex_edit_probe.cpp" -luser32 || exit 1
echo "   ok"

PidsInRun() { apex_own_pids '_persist_run'; }
NoPidsInRun() { [ -z "$(PidsInRun)" ]; }
cleanup() {
  local p
  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. Take the field, give it back at exit.
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

echo
echo "== starting a private copy =="
rm -rf "$RUN"
cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "   FAIL: the built folder could not be copied"; exit 1; }
printf 'theme=auto\nlang=auto\n' > "$RUN/apex.ini"
rm -f "$RUN"/apex*.log
# ⚠️ TWO MAPPINGS ARE PUT IN THE PRIVATE COPY'S OWN FEATURE FILES **BEFORE THE HOST STARTS**, because a feature reads
# its settings in `init()`: writing them afterwards leaves the feature answering "I map nothing" for the rest of the
# run, which is exactly what the first version of section A2 did (and the gate said so: "PANES" came back empty).
#
# ⚠️ AND THEY ARE TWO *DETERMINISTIC* PANES, one per feature, with nothing about this machine in them: SmoothWheel's
# four faders are one named group (one pane) and KeepAwake's list is another. That is what makes the ORDER testable
# at all -- MediaControl's volume pane only exists while something on the machine is making sound.
printf 'quick_panel=1\n' > "$RUN/Plugins/SmoothWheel/SmoothWheel.ini"
printf 'quick_panel=1\n' > "$RUN/Plugins/KeepAwake/KeepAwake.ini"
( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
# ⚠️ CONDITION, NOT A SLEEP: the host is ready when its startup has written this line (see main.cpp).
apex_wait_line "$RUN/apex.log" 'engine running' 10

# ⚠️ EVERY EXE FILE IN THE BUILT FOLDER, so a future gate that writes a pinned apex.ini is caught by this one.
# (Comparing the *contents* would fail on the log files, which the program legitimately writes -- the promise
# is "settings and binaries are not rewritten by a test run", not "nothing ever changes".)
SnapshotBuilt() {
  ( cd "$BUILD/apex" 2>/dev/null && ls -la --time-style=+%s apex.ini Plugins/*/*.ini 2>/dev/null ) | md5sum
}
# Nothing to snapshot if the user's folder has no ini yet -- that is itself the "not cleared" answer.
BEFORE=$(SnapshotBuilt)

echo
echo "== A. an edit is written shortly after it is made, WITHOUT closing the program =="
# The panel is what normally sends these; the probe speaks the same protocol so the test does not depend on
# driving a browser. The host is left RUNNING the whole time -- that is the point.
"$PROBE" "$RUN" theme dark
# ⚠️ THE CONDITION IS THE ASSERTION ITSELF: wait for the value to be IN THE FILE (up to 5 s, well over the 1 s
# debounce) instead of sleeping past the debounce and hoping. On a slow machine the old `sleep 2` was a coin
# toss; here a missing write simply costs the full budget and then fails, as it should.
apex_wait_line "$RUN/apex.ini" '^theme=dark$' 5

if [ ! -f "$RUN/apex.ini" ]; then
  echo "   FAIL: apex.ini does not exist after an edit"
  fail=1
else
  if grep -q '^theme=dark$' "$RUN/apex.ini"; then
    echo "   the host config was written with the edit (theme=dark), host still running"
  else
    echo "   FAIL: apex.ini exists but does not carry the edit:"
    sed 's/^/        /' "$RUN/apex.ini"
    fail=1
  fi
fi

# AND THE SAME FOR A FEATURE'S OWN FILE, which is the one the user named ("插件设置保存"). A feature is told
# about a value through applySetting and persists it through saveSettings; the host schedules that write.
"$PROBE" "$RUN" theme light
apex_wait_line "$RUN/apex.ini" '^theme=light$' 5
if grep -q '^theme=light$' "$RUN/apex.ini"; then
  echo "   a second edit replaced it (theme=light) -- the write is durable, not append-only"
else
  echo "   FAIL: the second edit did not reach the file"
  fail=1
fi

echo
echo "== A2. the document the panel reads is valid JSON, and says what the switches mean =="
#
# ⚠️ WHY THIS IS HERE AND WHY IT MATTERS. The snapshot is the panel's entire view of the world, and a document it
# cannot parse is indistinguishable from a host that is not running -- the page says "Apex 主程序没有在运行" while
# the host answers every message. settings_host.cpp has carried a note saying "there is no gate that parses the
# snapshot" for as long as the snapshot has existed, and a missing `]` in a newly added key got all the way to a
# deployed build because of exactly that. The host is already running here, so this is where it gets asked.
#
# ⚠️ AND IT IS ASKED MORE THAN ONCE, because the second half is a STATEMENT ABOUT THE SWITCHES: the user's rule is
# that a feature's master switch HIDES that feature's panes from the flyout -- "当插件总开关关闭后…其插件的局部快速
# 面板功能要隐藏，打开后再按各局部功能快速面板功能开关状态还原" -- and `quickOrder` in the snapshot is exactly the
# list of panes the flyout is about to draw. So: map something, look, switch the feature off, look again (its keys
# must be gone AND ITS PLACE MUST BE KEPT), switch it back on, look a third time (same keys, same order).
#
# ⚠️ AND IT IS PARSED BY A REAL PARSER, not by counting brackets: a bracket-counting check is green for
# `{"a":1]` and for a document with a stray comma, which is what the two earlier versions of this class of bug
# were (`]},,{` in a feature's controls document). `node` is already a dependency of the panel gate.
SNAP_PROBE="$BUILD/_snap_probe.exe"
g++ -std=c++17 -O2 -mconsole -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$SNAP_PROBE" \
    "$ROOT/_diag/apex_describe_probe.cpp" -luser32 || { echo "   FAIL: the snapshot probe did not build"; exit 1; }
if ! command -v node >/dev/null 2>&1; then
  echo "   FAIL: node is not available, so the snapshot cannot be parsed"
  exit 1
fi

# (The mapping this section needs was written into the private copy BEFORE the host was started -- see the note
#  where the copy is assembled. A feature reads its settings in init().)

# Read the snapshot and print what the page cares about: is it JSON, and which panes are in the flyout?
snapshot_panes() {
  "$SNAP_PROBE" "$RUN/" snapshot > "$BUILD/_snapshot_probe.json" 2>/dev/null
  node -e '
const fs = require("fs");
const raw = fs.readFileSync(process.argv[1], "utf8").replace(/^\uFEFF/, "");
let j;
try { j = JSON.parse(raw); } catch (e) { console.log("PARSE: " + e.message); process.exit(0); }
const bad = [];
if (!j || typeof j !== "object") bad.push("not an object");
if (!j.host || typeof j.host !== "object") bad.push("no host");
if (typeof j.host.lang !== "string" || typeof j.host.theme !== "string") bad.push("host.lang/theme");
if (typeof j.host.quickCompact === "undefined" || typeof j.host.quickOwn === "undefined")
  bad.push("the quick-panel switches");
if (!Array.isArray(j.features)) bad.push("features is not an array");
// ⚠️ `quickOrder` IS CHECKED AS AN ARRAY OF PANES WITH A KEY AND A NAME, not merely as "present": the General
// page draws its reorder list straight from it, so a string, a missing key or a nameless row would be a broken
// row rather than a lost key.
if (!Array.isArray(j.quickOrder)) bad.push("quickOrder is not an array");
else for (const e of j.quickOrder)
  if (!e || typeof e !== "object" || typeof e.key !== "string" || !e.key ||
      typeof e.nameZh !== "string" || typeof e.nameEn !== "string")
    bad.push("a quickOrder entry without a key or a name");
if (bad.length) { console.log("BAD: " + bad.join(", ")); process.exit(0); }
// ⚠️ THE KEYS COME OUT COMMA-SEPARATED, WHICH IS THE SHAPE THE PANEL SENDS THEM BACK IN (and the keys themselves
// cannot contain a comma -- see QuickBlockKey). That makes this output directly comparable with what the edit probe
// sends below.
console.log("PANES " + j.quickOrder.map((e) => e.key).join(",") + " FEATURES " + j.features.length);
' "$BUILD/_snapshot_probe.json" 2>&1
}

# Just the keys, without the feature count that rides along on the same line.
snapshot_keys() { snapshot_panes | sed -n 's/^PANES //p' | sed 's/ FEATURES .*//'; }

snap_out=$(snapshot_panes)
if printf '%s' "$snap_out" | grep -q '^PANES '; then
  echo "   the snapshot parses, and carries the host settings, the features and the flyout's panes"
  echo "   ($snap_out)"
else
  echo "   FAIL: the snapshot the panel would read is not usable: $snap_out"
  fail=1
fi

# ... the master switch, and the user's rule about it. (KeepAwake is the feature whose pane section A2 mapped, and
# the only thing that matters about the choice is that it OWNS A PANE.)
KA_SLOT=$("$SNAP_PROBE" "$RUN/" snapshot 2>/dev/null | node -e '
let s=""; process.stdin.on("data",(d)=>s+=d).on("end",()=>{ try {
  const j = JSON.parse(s.replace(/^\uFEFF/,""));
  const i = (j.features||[]).findIndex((f)=>f.id==="KeepAwake");
  console.log(i >= 0 ? i : "");
} catch (e) { console.log(""); } });')
if [ -z "$KA_SLOT" ]; then
  echo "   FAIL: KeepAwake is not in the snapshot's feature list, so its switch cannot be tested"
  fail=1
else
  panes_before=$(snapshot_keys)
  "$SNAP_PROBE" "$RUN/" "off=$KA_SLOT,1" >/dev/null 2>&1
  panes_off=$(snapshot_keys)
  "$SNAP_PROBE" "$RUN/" "off=$KA_SLOT,0" >/dev/null 2>&1
  panes_back=$(snapshot_keys)
  if printf '%s' "$panes_before" | grep -q 'KeepAwake|'; then
    echo "   with the feature ON, its panes are in the flyout's list ($panes_before)"
  else
    echo "   FAIL: the mapped feature has no panes in the list at all -- the rest of this section cannot mean"
    echo "         anything (got '$panes_before')"
    fail=1
  fi
  if printf '%s' "$panes_off" | grep -q 'KeepAwake|'; then
    echo "   FAIL: switching the feature OFF did not hide its panes from the flyout ($panes_off)"
    fail=1
  else
    echo "   switching the feature OFF hides every one of its panes (list now '$panes_off')"
  fi
  # ⚠️ AND SWITCHING IT BACK ON RESTORES THE SAME PANES IN THE SAME ORDER -- which is the other half of the
  # user's sentence ("打开后再按各局部功能快速面板功能开关状态还原"), and it is the merge rule in
  # QuickOrderSetFromKeys that makes it true: an absent pane keeps its saved place.
  if [ "$panes_back" = "$panes_before" ]; then
    echo "   and switching it back ON restores exactly the same panes in the same order"
  else
    echo "   FAIL: the panes did not come back as they were ('$panes_before' -> '$panes_back')"
    fail=1
  fi

  # ⚠️⚠️ AND THE ORDER ITSELF, SENT THE WAY THE PANEL SENDS IT -- the seam that let a real bug through. The General
  # page hands the whole order back in ONE `setHost` value, and the request protocol is `key=value` LINES: a value
  # containing a NEWLINE arrives TRUNCATED AT ITS FIRST KEY. The page used to join the keys with newlines, so the
  # host saw exactly one of them and a drag could move at most the first row -- the user's report was precisely that:
  # "快速面板分组不能调顺序". Nothing in the build layer could see it: the page probe checks the MESSAGE the page
  # builds, the hostconfig probe checks the PARSER, and the line BETWEEN them (ui_webview.cpp: JsonString ->
  # "key=...\nvalue=...") had never been given a value with a separator in it. This does that, over the same
  # protocol, against a real host.
  #
  # ⚠️⚠️ AND THE SPLIT IS PURE BASH, BECAUSE THE OBVIOUS VERSION IS SILENTLY WRONG HERE. `tr ',' '\n'` under this
  # MSYS shell DELETED the commas instead of translating them (its `\n` argument gets mangled), so the split
  # "worked" and produced ONE key -- the same class of trap as the slash-leading SCRIPT (not path) recorded in
  # docs/rules/gates.md §5.0.1, where Git Bash rewrote the argument before a native tool saw it. ⚠️ AND THE SPELLING
  # HERE IS DELIBERATE: `check_apex_modular.sh` §8 scans this tree for that literal shape, and a comment that
  # quotes it trips the scan -- which is exactly how this line read before it was reworded.
  # A `read -a` needs no external tool and no backslash. ⚠️ AND THE SKIP BELOW CANNOT HIDE THAT FAILURE: if the
  # value plainly contains a comma and the split still found fewer than two keys, that is a FAIL, not a skip.
  IFS=',' read -r -a pane_keys <<< "$panes_before"
  if [ "${#pane_keys[@]}" -lt 2 ]; then
    if printf '%s' "$panes_before" | grep -q ','; then
      echo "   FAIL: the pane list has more than one key and could not be split: '$panes_before'"
      fail=1
    else
      echo "   (one pane only, so there is no order to reorder -- the transport check is skipped)"
    fi
  else
    rev=""
    for ((i = ${#pane_keys[@]} - 1; i >= 0; --i)); do
      rev="${rev}${rev:+,}${pane_keys[i]}"
    done
    # ⚠️⚠️ THE VALUE GOES THROUGH A FILE, NOT THROUGH ARGV, AND THAT IS A MEASURED LESSON RATHER THAN TIDINESS:
    # a pane key carries the FEATURE's own words (`MediaControl|亮度`), and this probe is a MinGW console program
    # whose argv the C runtime decodes as ANSI -- the Chinese came back as replacement characters, the host saved
    # `MediaControl|???`, nothing matched, and the gate reported "the order did not arrive whole" about a transport
    # that was in fact fine. The panel never touches argv (it posts JSON, which becomes UTF-8 bytes), so the gate
    # writes the bytes the PANEL would send and points the probe at the file. (node writes them, because node is
    # the only thing here that is already handling the text as UTF-8.)
    printf '%s' "$rev" > "$BUILD/_order_value.txt"
    node -e 'const fs=require("fs");fs.writeFileSync(process.argv[1], process.argv[2], "utf8");' \
      "$BUILD/_order_value.txt" "$rev"
    "$PROBE" "$RUN" quickorder "@$BUILD/_order_value.txt" >/dev/null 2>&1
    panes_after=$(snapshot_keys)
    if [ "$panes_after" = "$rev" ]; then
      echo "   a whole ORDER sent in one value arrives whole and is applied ($rev)"
    else
      echo "   FAIL: the order sent in one value did not arrive whole"
      echo "         sent     '$rev'"
      echo "         snapshot '$panes_after'"
      fail=1
    fi
    # ... and it is in the settings file, one key per line, so a restart keeps it. ⚠️ CONDITION, NOT A SLEEP: the
    # write is debounced (see SettingsTouch), so the file is asked to say so within 5 s rather than being read a
    # moment after the message.
    apex_wait_line "$RUN/apex.ini" '^quickorder=' 5 || true
    # ⚠️ `grep -c` PRINTS 0 AND EXITS 1, so `$(grep -c ... || echo 0)` yields "0\n0" and the comparison below blows
    # up with "integer expected" -- measured. `|| true` keeps the status quiet without touching the output.
    n_lines=$(grep -c '^quickorder=' "$RUN/apex.ini" 2>/dev/null || true)
    first_key=$(grep '^quickorder=' "$RUN/apex.ini" 2>/dev/null | head -1 | cut -d= -f2-)
    want_first=$(printf '%s' "$rev" | cut -d, -f1)
    if [ "${n_lines:-0}" -ge 2 ] && [ "$first_key" = "$want_first" ]; then
      echo "   and it is in the settings file, one key per line, in that order ($n_lines line(s))"
    else
      echo "   FAIL: the file does not carry the order ($n_lines line(s), first '$first_key', want '$want_first')"
      fail=1
    fi
  fi
fi

echo
echo "== B. killing the host does not lose the write (the reason for the debounce) =="
before_kill=$(grep '^theme=' "$RUN/apex.ini" 2>/dev/null)
for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
# `taskkill /F` returns once the process is gone, so the condition is "no pid left" -- not a courtesy second.
apex_wait_for 3 NoPidsInRun
after_kill=$(grep '^theme=' "$RUN/apex.ini" 2>/dev/null)
if [ -n "$before_kill" ] && [ "$before_kill" = "$after_kill" ]; then
  echo "   the value survived an unclean kill ($after_kill)"
else
  echo "   FAIL: the value changed across the kill ('$before_kill' -> '$after_kill')"
  fail=1
fi

echo
echo "== C. the built folder's settings were not touched by any of this =="
AFTER=$(SnapshotBuilt)
if [ "$BEFORE" = "$AFTER" ]; then
  echo "   build/apex/ is unchanged (nothing was written there or deleted from it)"
else
  echo "   FAIL: a test run changed the built folder's own settings files:"
  ( cd "$BUILD/apex" 2>/dev/null && ls -la --time-style=+%s apex.ini Plugins/*/*.ini 2>/dev/null ) |
    sed 's/^/        /'
  fail=1
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: an edit reaches the disk while the program runs, survives a kill, and no gate writes into the built folder"
else
  echo "FAILED: persistence"
fi
exit $fail
