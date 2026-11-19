#!/usr/bin/env bash
# Gate: ComputeManager's real DLL, period arithmetic, document size and settings ownership.
#
# This feature is intentionally generic: it does not know any provider's JSON shape. That makes its
# own edges important: midnight-crossing peak/valley ranges, a cumulative usage counter that resets,
# an API key that must not leak into the page, and a full endpoint list that must still fit Apex's
# 64 KiB control-document transport.
#
# The real DLL runs with a fake host and a scratch folder under build/. No deployed setting is read or
# written, no window opens, no network request is made (the probe switches auto polling off first), and
# no input device is touched.
set -uo pipefail
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
DLL="$ROOT/build/apex/Plugins/ComputeManager/ComputeManager.dll"
PROBE_SRC="$ROOT/_diag/feature_computemanager_probe.cpp"
TAG="$$"
PROBE="$ROOT/build/_computemanager_probe_$TAG.exe"
RUN="$ROOT/build/_computemanager_probe_$TAG"
DOCUMENT="$RUN/document.json"
trap 'rm -f "$PROBE"; rm -rf "$RUN"' EXIT

if [ ! -f "$DLL" ]; then
  echo "FAIL: the feature is not built: $DLL"
  exit 1
fi
if [ ! -f "$PROBE_SRC" ]; then
  echo "FAIL: the probe is missing: $PROBE_SRC"
  exit 1
fi
if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is unavailable, so the control document cannot be parsed"
  exit 1
fi

# The three old features own their files. ComputeManager must not be a second writer to any of them.
SW_INI="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.ini"
KA_INI="$ROOT/build/apex/Plugins/KeepAwake/KeepAwake.ini"
IME_JSON="$ROOT/build/apex/Plugins/AutoIME/config.json"
before_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo absent )
before_ka=$( [ -f "$KA_INI" ] && md5sum "$KA_INI" | cut -d' ' -f1 || echo absent )
before_ime=$( [ -f "$IME_JSON" ] && md5sum "$IME_JSON" | cut -d' ' -f1 || echo absent )

echo "== syntax check with warnings promoted to errors =="
g++ -std=c++17 -O2 -mwindows -Wall -Wextra -Werror -I"$ROOT/apex" -I"$ROOT/common" \
    -I"$ROOT/shared" -I"$ROOT/features/ComputeManager" \
    -fsyntax-only "$ROOT/features/ComputeManager/feature_computemanager.cpp" || exit 1

echo "== taskbar copy is display-only =="
if grep -q 'g_isTaskbar ? HTTRANSPARENT : HTCAPTION' "$ROOT/features/ComputeManager/feature_computemanager.cpp" &&
   grep -q 'g_isTaskbar ? WS_EX_TRANSPARENT : 0' "$ROOT/features/ComputeManager/feature_computemanager.cpp"; then
  echo "  the taskbar overlay passes hit-testing through to the taskbar; the desktop widget stays draggable"
else
  echo "FAIL: the taskbar overlay can intercept mouse input"
  exit 1
fi

echo "== building the real-DLL probe =="
g++ -std=c++17 -O2 -mconsole -Wall -Wextra -Werror \
    -I"$ROOT/apex" -I"$ROOT/features/ComputeManager" \
    -o "$PROBE" "$PROBE_SRC" || exit 1

# An explicit path under build/, never the deployed Plugins folder.
rm -rf "$RUN"
mkdir -p "$RUN"

echo
APEX_CM_NO_TASKBAR=1 "$PROBE" "$DLL" "$DOCUMENT" "$RUN/" || exit 1

echo
echo "== the document is real JSON with all status cards =="
node -e '
  const fs = require("fs");
  const doc = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
  const params = doc.params || [];
  const statuses = params.filter((p) => p.type === "status");
  const group = params.find((p) => p.type === "group" && p.id === "accounts");
  const processes = params.find((p) => p.type === "group" && p.id === "processes");
  if (!group || !Array.isArray(group.items) || group.items.length !== 12) process.exit(2);
  const fieldIds = new Set((group.fields || []).map((f) => f.id));
  for (const id of ["preset", "models", "models_url", "api_key"]) {
    if (!fieldIds.has(id)) process.exit(11);
  }
  if (fieldIds.has("model")) process.exit(12);
  { const ids = (group.fields || []).map((f) => f.id);
    if (ids.indexOf("models") < 0 || ids.indexOf("models") + 1 !== ids.indexOf("api_key")) process.exit(15); }
  const modelsField = (group.fields || []).find((f) => f.id === "models");
  if (!modelsField || !modelsField.separated || modelsField.removeLabelZh !== "移除") process.exit(16);
  if (group.hideCount !== true || processes.hideCount !== true) process.exit(20);
  if (!params.some((p) => p.type === "row" && p.id === "refresh_row" &&
      p.controls.some((c) => c.id === "auto_poll") &&
      p.controls.some((c) => c.id === "poll_seconds"))) process.exit(13);
  if (params.some((p) => p.id === "taskbar_row" || p.id === "taskbar_font_size" ||
      p.id === "taskbar_text_color")) process.exit(14);
  if (!processes || !Array.isArray(processes.items) || processes.items.length !== 1) process.exit(6);
  if (statuses.length !== 12) process.exit(3); // one compact row per endpoint, no aggregate
  if (!statuses.some((p) => p.id === "endpoint_11")) process.exit(4);
  if (statuses.some((p) => p.id === "overall")) process.exit(17);
  if (!statuses.every((p) => !p.lines || p.lines.length === 0)) process.exit(18);
  if (!statuses.every((p) => p.toggleId && p.toggleId.endsWith(".taskbar_visible"))) process.exit(19);
  if (JSON.stringify(doc).includes("secret-token-01")) process.exit(5);
  if (!JSON.stringify(doc).includes("manually-added-model")) process.exit(7);
  if (group.items[0].values.schedule_enabled !== 0) process.exit(8);
  if (group.items[1].values.schedule_enabled !== 0) process.exit(10);
  console.log(`  parsed ${params.length} control(s), ${statuses.length} live status cards, ${group.items.length} endpoints, ${processes.items.length} process trigger`);
' "$DOCUMENT" || {
  code=$?
  echo "FAIL: the control document check exited with code $code"
  exit 1
}

echo
echo "== settings stayed in this feature's folder =="
fail=0
say() { printf '  %-64s %s\n' "$1" "$2"; [ "$2" = "ok" ] || fail=1; }

if [ -f "$RUN/ComputeManager.ini" ]; then
  say "its configuration is in its own scratch folder" ok
else
  say "its configuration is in its own scratch folder" FAIL
fi
if grep -q '^endpoint\.0\.schedule_enabled=0$' "$RUN/ComputeManager.ini" 2>/dev/null; then
  say "the preset does not invent a vendor peak/valley schedule" ok
else
  say "the preset does not invent a vendor peak/valley schedule" FAIL
fi
if grep -q '^endpoint\.0\.api_key=secret-token-01$' "$RUN/ComputeManager.ini" 2>/dev/null &&
   grep -q '^endpoint\.0\.model=vendor-model-alpha$' "$RUN/ComputeManager.ini" 2>/dev/null &&
   grep -q '^endpoint\.11\.name=' "$RUN/ComputeManager.ini" 2>/dev/null; then
  say "the real key is stored only in the private configuration file" ok
else
  say "the real key is stored only in the private configuration file" FAIL
fi
if grep -q '^process\.0\.name=code\.exe$' "$RUN/ComputeManager.ini" 2>/dev/null; then
  say "the process trigger is stored in the same private feature file" ok
else
  say "the process trigger is stored in the same private feature file" FAIL
fi
if grep -q '^poll_seconds=1$' "$RUN/ComputeManager.ini" 2>/dev/null &&
   ! grep -q '^taskbar_font_size=' "$RUN/ComputeManager.ini" 2>/dev/null &&
   ! grep -q '^taskbar_text_color=' "$RUN/ComputeManager.ini" 2>/dev/null; then
  say "taskbar text appearance is not duplicated in the private feature file" ok
else
  say "taskbar text appearance is not duplicated in the private feature file" FAIL
fi
if grep -q '^endpoint\.0\.taskbar_visible=1$' "$RUN/ComputeManager.ini" 2>/dev/null; then
  say "per-endpoint taskbar visibility is stored in the private file" ok
else
  say "per-endpoint taskbar visibility is stored in the private file" FAIL
fi
if grep -q '^endpoint\.0\.models=manually-added-model$' "$RUN/ComputeManager.ini" 2>/dev/null; then
  say "manually added sub-models are stored per endpoint" ok
else
  say "manually added sub-models are stored per endpoint" FAIL
fi

after_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo absent )
after_ka=$( [ -f "$KA_INI" ] && md5sum "$KA_INI" | cut -d' ' -f1 || echo absent )
after_ime=$( [ -f "$IME_JSON" ] && md5sum "$IME_JSON" | cut -d' ' -f1 || echo absent )
[ "$before_sw" = "$after_sw" ] && say "SmoothWheel's settings untouched" ok || say "SmoothWheel's settings untouched" FAIL
[ "$before_ka" = "$after_ka" ] && say "KeepAwake's settings untouched" ok || say "KeepAwake's settings untouched" FAIL
[ "$before_ime" = "$after_ime" ] && say "AutoIME's settings untouched" ok || say "AutoIME's settings untouched" FAIL

if find "$RUN" -maxdepth 1 -name '*.tmp' -print -quit 2>/dev/null | grep -q .; then
  say "the atomic save leaves no temporary file behind" FAIL
else
  say "the atomic save leaves no temporary file behind" ok
fi

rm -f "$PROBE" "$DOCUMENT"
rm -rf "$RUN"

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: ComputeManager's settings or document contract is broken"
  exit 1
fi
echo "OK: schedule, counter reset, JSON paths, masking, 12-endpoint document, and file ownership"
