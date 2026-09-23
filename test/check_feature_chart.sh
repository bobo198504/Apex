#!/usr/bin/env bash
# Gate: EVERY FEATURE'S OWN CONTROLS DOCUMENT -- is it valid JSON, and does every control mean something?
#
# WHY THIS GATE EXISTS, and why it is not the same as the panel's probe: the panel probe runs the PAGE against
# a DOM stub built by hand, so it proves the page can draw a well-formed document. This one checks the
# document the FEATURE ACTUALLY BUILDS, by loading the built DLL and calling its settingsJson -- the same call
# the host makes. The failures that motivated it were both on that side:
#
#   * the opening `{"params":[` was written with a bare _snprintf whose return value was not counted, so every
#     later write landed at offset 0 and sliced the head off the document;
#   * the motion chart was appended INSIDE the params array (a missing `]`).
#
# Both produced a document the page rejected outright -- "Unexpected non-whitespace character after JSON" --
# and the user-visible symptom was the same as "the feature sent nothing": an empty card, no error anywhere
# except the panel's log. The page cannot tell a malformed document from a missing one, so the check belongs
# where the document is made.
#
# ⚠️⚠️ AND IT NOW RUNS FOR EVERY FEATURE, BECAUSE IT USED TO RUN FOR ONE. The probe was written for SmoothWheel
# and this script loaded exactly `Plugins/SmoothWheel/SmoothWheel.dll`. MediaControl then shipped a document
# with a doubled comma in it (`]},,{`) and the user's report was "媒体控制页面是空的" -- the exact symptom above,
# with the gate green, because it had never looked at that feature. A gate that covers one of the four features
# is a gate for one of the four features.
#
# ⚠️⚠️ AND "THE BRACKETS BALANCE" IS NOT "IT IS JSON". The probe's own `JsonBalanced` counts brackets, and
# `]},,{` balances exactly; a comma where an element should be is invisible to it. So the document is ALSO
# handed to a real parser here (`node`), which is the same tool the project already uses for everything else
# that has to read JSON (check_apex_panel.sh, check_reaper_note.sh).
#
# IT ALSO CHECKS THE CHART, for a feature that sends one: a chart that ignores a slider teaches the wrong
# lesson confidently, so every control such a feature describes must move the picture it draws. (That chart is
# a PORT of the REAPER plugin's, which was left behind when Apex was split out of it -- see the note in the
# feature.) A feature with no chart is not asked for one.
#
# usage: test/check_feature_chart.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

PLUGINS="$ROOT/build/apex/Plugins"
OUT="$ROOT/build/_chart_probe.exe"
DUMP="$ROOT/build/_chart_doc.json"

if [ ! -d "$PLUGINS" ]; then
  echo "the features are not built: $PLUGINS"
  echo "(run apex/build.sh first -- this gate checks the ARTEFACT, not the source)"
  exit 1
fi

# ⚠️ AN ERROR RATHER THAN A SKIP when the parser is missing, for the same reason check_apex_panel.sh does it: a
# check that quietly stops checking is worse than one that fails, and this project has paid for that lesson
# already ("一扇不会红的门比没有门更糟").
if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is not available, so no document can actually be parsed"
  exit 1
fi

echo "== building the probe =="
g++ -std=c++17 -O2 -I"$ROOT/apex" -I"$ROOT/common" -o "$OUT" "$ROOT/_diag/feature_chart_probe.cpp" || exit 1
echo "   ok"
echo

fail=0
checked=0
SCRATCH="$ROOT/build/_chart_probe_dir"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
for dll in "$PLUGINS"/*/*.dll; do
  [ -f "$dll" ] || continue
  id="$(basename "$(dirname "$dll")")"
  checked=$((checked + 1))
  echo "== $id =="
  # ⚠️ THE FEATURE IS GIVEN A SCRATCH FOLDER OF ITS OWN, because `init` runs now (see the probe): a feature is
  # allowed to read its settings file there, and that file must never be a deployed or hand-made one.
  mkdir -p "$SCRATCH/$id"
  "$OUT" "$dll" "$DUMP" "$SCRATCH/$id/"
  rc=$?
  if [ $rc -ne 0 ]; then
    fail=1
    continue
  fi
  # ⚠️ THE REAL PARSER, AND IT SAYS WHERE THE TROUBLE IS. A bare "FAIL" for a document assembled by hand is a
  # search through a one-line string; the position plus a window of the text is the answer.
  node -e '
    const fs = require("fs");
    const t = fs.readFileSync(process.argv[1], "utf8");
    try {
      const d = JSON.parse(t);
      const ids = (d.params || []).map((p) => p.id + ":" + p.type).join(", ");
      if (!d.params || !Array.isArray(d.params)) { console.log("  FAIL: no params array"); process.exit(1); }
      console.log("  parsed as JSON -- " + (d.params.length) + " control(s): " + ids);
    } catch (e) {
      console.log("  FAIL: the document is not JSON -- " + e.message);
      const m = /position (\d+)/.exec(e.message);
      if (m) {
        const p = Number(m[1]);
        console.log("        around byte " + p + ": " + JSON.stringify(t.slice(Math.max(0, p - 120), p + 60)));
      }
      process.exit(1);
    }
  ' "$DUMP"
  if [ $? -ne 0 ]; then
    fail=1
  fi
  echo
done
rm -f "$OUT" "$DUMP"
rm -rf "$SCRATCH"

if [ "$checked" -eq 0 ]; then
  echo "FAIL: no feature DLL was found -- nothing was checked"
  exit 1
fi

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: a feature's controls document is not something the page can draw"
  exit 1
fi
echo "OK: $checked feature document(s), each one valid JSON, and every slider of the one that sends a chart"
echo "    moves it"
