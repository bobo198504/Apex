#!/usr/bin/env bash
# Gate: SystemStatus's real DLL, adaptive formatting, per-item taskbar mappings, live status cards,
# taskbar display-only rule, and private-file ownership.
#
# The real DLL runs with a fake host and a scratch folder under build/. It creates no visible window and
# does not read or write the deployed settings. WMI/NVML/PDH reads are passive observations of this machine;
# no process, network request, input device, or taskbar setting is changed by the probe.
set -uo pipefail
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
FEATURE="$ROOT/features/SystemStatus/feature_systemstatus.cpp"
DLL="$ROOT/build/apex/Plugins/SystemStatus/SystemStatus.dll"
PROBE_SRC="$ROOT/_diag/feature_systemstatus_probe.cpp"
TAG="$$"
PROBE="$ROOT/build/_systemstatus_probe_$TAG.exe"
RUN="$ROOT/build/_systemstatus_probe_$TAG"
DOCUMENT="$RUN/document.json"
cleanup() {
  rm -f "$PROBE" "$DOCUMENT"
  rm -rf "$RUN"
}
trap cleanup EXIT

if [ ! -f "$FEATURE" ]; then
  echo "FAIL: the feature source is missing: $FEATURE"
  exit 1
fi
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

echo "== syntax check with warnings promoted to errors =="
g++ -std=c++17 -O2 -mwindows -Wall -Wextra -Werror \
    -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/shared" -I"$ROOT/features/SystemStatus" \
    -fsyntax-only "$FEATURE" || exit 1

echo "== the host-owned taskbar copy cannot steal clicks =="
HOST_TASKBAR="$ROOT/apex/taskbar_display.cpp"
if grep -q 'HTTRANSPARENT' "$HOST_TASKBAR" &&
   grep -q 'WS_EX_TRANSPARENT' "$HOST_TASKBAR"; then
  echo "  the host draws the taskbar and the feature supplies text only"
else
  echo "FAIL: taskbar drawing is not host-owned or can intercept input"
  exit 1
fi

echo "== building the real-DLL probe =="
g++ -std=c++17 -O2 -mconsole -Wall -Wextra -Werror \
    -I"$ROOT/apex" -I"$ROOT/features/SystemStatus" \
    -o "$PROBE" "$PROBE_SRC" || exit 1

rm -rf "$RUN"
mkdir -p "$RUN"
APEX_SYSTEMSTATUS_NO_TASKBAR=1 "$PROBE" "$DLL" "$DOCUMENT" "$RUN/" || exit 1
mkdir -p "$RUN/static"
APEX_SYSTEMSTATUS_NO_TASKBAR=1 APEX_SYSTEMSTATUS_STATIC=1 \
  "$PROBE" "$DLL" "$RUN/static/document.json" "$RUN/static/" >/dev/null || exit 1
mkdir -p "$RUN/migrate"
cat > "$RUN/migrate/SystemStatus.ini" <<'EOF'
gpu_00000000_00000001_usage=1
gpu_00000000_00000001_usage.unit=gb
gpu_00000000_00000001_temp=0
gpu_00000000_00000002_usage=0
gpu_00000000_00000002_usage.unit=percent
gpu_00000000_00000002_temp=1
EOF
APEX_SYSTEMSTATUS_NO_TASKBAR=1 APEX_SYSTEMSTATUS_STATIC=1 \
  APEX_SYSTEMSTATUS_MIGRATE_ONLY=1 \
  "$PROBE" "$DLL" "$RUN/migrate/document.json" "$RUN/migrate/" >/dev/null || exit 1

echo
echo "== the document is real JSON with every independent item =="
node -e '
  const fs = require("fs");
  const doc = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
  const params = doc.params || [];
  if (params.length < 12 || params.length > 64) process.exit(2);
  if (!params.every((p) => p.type === "status")) process.exit(3);
  if (!params.every((p) => p.live === true)) process.exit(4);
  const ids = new Set(params.map((p) => p.toggleId));
  const fixed = [
    "net_down", "net_up", "cpu_usage", "cpu_temp", "ram_usage", "ram_temp",
    "clock", "date_calendar", "lunar_date"
  ];
  if (!fixed.every((id) => ids.has(id))) process.exit(5);
  const gpuUsage = [...ids].filter((id) => /^gpu_.+_usage$/.test(id));
  const gpuTemp = [...ids].filter((id) => /^gpu_.+_temp$/.test(id));
  const diskUsage = [...ids].filter((id) => /^disk_.+_usage$/.test(id));
  const diskTemp = [...ids].filter((id) => /^disk_.+_temp$/.test(id));
  const diskRead = [...ids].filter((id) => /^disk_.+_read$/.test(id));
  const diskWrite = [...ids].filter((id) => /^disk_.+_write$/.test(id));
  const ipAddresses = [...ids].filter((id) => /^ip_\d+_/.test(id));
  if (gpuUsage.length < 1 || gpuTemp.length !== gpuUsage.length) process.exit(11);
  if (diskUsage.length < 1 || diskTemp.length !== diskUsage.length) process.exit(12);
  if (diskRead.length !== diskUsage.length || diskWrite.length !== diskUsage.length) process.exit(14);
  if (ipAddresses.length < 1) process.exit(15);
  if (params.some((p) => /^show_(cpu|memory|storage|gpu|clock|date|lunar)$/.test(p.toggleId))) process.exit(13);
  if (params.some((p) => !p.textZh || !p.textEn)) process.exit(6);
  if (params.some((p) => Array.isArray(p.lines) && p.lines.length)) process.exit(16);
  const usageChoices = params.filter((p) => p.choiceId);
  if (usageChoices.length < 3 || usageChoices.some((p) => p.choiceValue !== "percent" ||
      p.choiceSwitch !== true || p.choiceOffValue !== "percent" ||
      (p.choiceOnValue !== "ghz" && p.choiceOnValue !== "gb"))) process.exit(17);
  if (!doc.summaryZh || !doc.summaryEn || doc.settingsFile !== "SystemStatus.ini") process.exit(7);
  const text = JSON.stringify(doc);
  if (/taskbar_font_size|taskbar_text_color/.test(text)) process.exit(8);
  const lunar = params.find((p) => p.toggleId === "lunar_date");
  if (!lunar || lunar.textZh.includes("农历") || lunar.textEn.includes("Lunar")) process.exit(9);
  if (text.includes("农历 · 系统不支持")) process.exit(10);
  console.log(`  parsed ${params.length} one-line cards: ${gpuUsage.length} GPUs, ${diskUsage.length} partitions, ${ipAddresses.length} IP addresses`);
' "$DOCUMENT" || {
  code=$?
  echo "FAIL: control document check exited with code $code"
  exit 1
}

echo
echo "== integrated and discrete GPUs are separate readings =="
node -e '
  const fs = require("fs");
  const doc = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
  const gpuUsage = doc.params.filter((p) => /^gpu_.+_usage$/.test(p.toggleId));
  const gpuTemp = doc.params.filter((p) => /^gpu_.+_temp$/.test(p.toggleId));
  if (gpuUsage.length !== 2 || gpuTemp.length !== 2) process.exit(2);
  if (!gpuUsage.some((p) => p.textEn.includes("Iris")) ||
      !gpuUsage.some((p) => p.textEn.includes("RTX 4060"))) process.exit(3);
  if (!gpuUsage.every((p) => p.choiceValue === "gb" && p.textZh.includes("显存"))) process.exit(5);
  if (!gpuTemp.some((p) => p.textZh.includes("54 C")) ||
      !gpuTemp.some((p) => p.textZh.includes("--"))) process.exit(4);
  const cpu = doc.params.find((p) => p.toggleId === "cpu_usage");
  const ram = doc.params.find((p) => p.toggleId === "ram_usage");
  if (!cpu || cpu.choiceValue !== "ghz" || !cpu.textZh.includes("GHz")) process.exit(6);
  if (!ram || ram.choiceValue !== "gb" || !ram.textZh.includes("GB")) process.exit(7);
  console.log("  " + gpuUsage.map((p) => p.textZh).join(" | "));
' "$RUN/static/document.json" || exit 1

echo
echo "== mappings and configuration belong only to this feature =="
fail=0
say() { printf '  %-68s %s\n' "$1" "$2"; [ "$2" = "ok" ] || fail=1; }

if [ -f "$RUN/SystemStatus.ini" ]; then
  say "the settings file is in the probe's own folder" ok
else
  say "the settings file is in the probe's own folder" FAIL
fi
if grep -q '^cpu_usage=1$' "$RUN/SystemStatus.ini" 2>/dev/null &&
   grep -q '^net_down=0$' "$RUN/SystemStatus.ini" 2>/dev/null; then
  say "each item's taskbar mapping is stored independently" ok
else
  say "each item's taskbar mapping is stored independently" FAIL
fi
if ! grep -q '^taskbar_font_size=' "$RUN/SystemStatus.ini" 2>/dev/null &&
   ! grep -q '^taskbar_text_color=' "$RUN/SystemStatus.ini" 2>/dev/null; then
  say "shared taskbar appearance is not duplicated here" ok
else
  say "shared taskbar appearance is not duplicated here" FAIL
fi
if find "$RUN" -maxdepth 1 -name '*.tmp' -print -quit 2>/dev/null | grep -q .; then
  say "atomic save leaves no temporary file behind" FAIL
else
  say "atomic save leaves no temporary file behind" ok
fi
if grep -q '^gpu_v8086_.*_usage=1$' "$RUN/migrate/SystemStatus.ini" &&
   grep -q '^gpu_v8086_.*_usage.unit=gb$' "$RUN/migrate/SystemStatus.ini" &&
   grep -q '^gpu_v8086_.*_usage.legacy=gpu_00000000_00000001_usage$' \
     "$RUN/migrate/SystemStatus.ini" &&
   grep -q '^gpu_v10de_.*_temp=1$' "$RUN/migrate/SystemStatus.ini" &&
   ! grep -q '^gpu_00000000_' "$RUN/migrate/SystemStatus.ini"; then
  say "legacy GPU switches, units and positions migrate to stable hardware keys" ok
else
  say "legacy GPU switches, units and positions migrate to stable hardware keys" FAIL
fi

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: SystemStatus's mappings or document contract is broken"
  exit 1
fi
echo "OK: adaptive units, dynamic per-device mappings, split usage/temperature cards, and private settings"
