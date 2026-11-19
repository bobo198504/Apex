#!/usr/bin/env bash
# Gate: Wallpaper's real DLL, schedule rules, directory ownership and read-only system interfaces.
#
# The probe never changes the user's wallpaper. It starts the feature with APEX_WALLPAPER_NO_APPLY=1, reads the
# current system state before and after, and proves the wallpaper is unchanged. A rendered display image is
# generated only inside build/_wallpaper_probe/.
set -uo pipefail
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
DLL="$ROOT/build/apex/Plugins/Wallpaper/Wallpaper.dll"
PROBE_SRC="$ROOT/_diag/feature_wallpaper_probe.cpp"
PROBE="$ROOT/build/_wallpaper_probe.exe"
RUN="$ROOT/build/_wallpaper_probe"
DOCUMENT="$RUN/document.json"

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

SW_INI="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.ini"
KA_INI="$ROOT/build/apex/Plugins/KeepAwake/KeepAwake.ini"
MC_INI="$ROOT/build/apex/Plugins/MediaControl/MediaControl.ini"
before_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo absent )
before_ka=$( [ -f "$KA_INI" ] && md5sum "$KA_INI" | cut -d' ' -f1 || echo absent )
before_mc=$( [ -f "$MC_INI" ] && md5sum "$MC_INI" | cut -d' ' -f1 || echo absent )

echo "== syntax check with warnings promoted to errors =="
g++ -std=c++17 -O2 -mwindows -Wall -Wextra -Werror \
    -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/shared" -I"$ROOT/features/Wallpaper" \
    -fsyntax-only "$ROOT/features/Wallpaper/feature_wallpaper.cpp" || exit 1

echo "== building the real-DLL probe =="
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
    -I"$ROOT/apex" -I"$ROOT/features/Wallpaper" \
    -o "$PROBE" "$PROBE_SRC" \
    -luser32 -lole32 -luuid -lgdiplus || exit 1

rm -rf "$RUN"
mkdir -p "$RUN"

echo
APEX_WALLPAPER_NO_APPLY=1 "$PROBE" "$DLL" "$DOCUMENT" "$RUN/" || exit 1

echo
echo "== the document is real JSON and the generated image is a PNG =="
node -e '
  const fs = require("fs");
  const doc = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
  const params = doc.params || [];
  const lock = params.find((p) => p.type === "group" && p.id === "lock");
  const monitors = params.find((p) => p.type === "group" && p.id === "monitors");
  if (params.some((p) => p.id === "wallpaper_status" || p.type === "status")) process.exit(2);
  if (!monitors || monitors.fieldsWrap || monitors.live !== true ||
      monitors.fields.length !== 3) process.exit(3);
  if (!lock || lock.fields.length !== 1) process.exit(4);
  if (monitors.fields[0].id !== "style" || monitors.fields[1].id !== "locked" ||
      monitors.fields[2].id !== "wallpaper" ||
      monitors.fields[2].labelZh !== "" || monitors.fields[2].labelEn !== "" ||
      lock.fields[0].labelZh !== "" || lock.fields[0].labelEn !== "") process.exit(17);
  if (monitors.fields[1].type !== "bool" ||
      Number(monitors.items[0].values.locked) !== 1) process.exit(18);
  if (monitors.items.some((item) =>
        !String((item.values || {}).preview || "").startsWith("data:image/jpeg;base64,"))) process.exit(19);
  if (params.some((p) => p.id === "switching" || p.id === "schedule" ||
      p.id === "day_start" || p.id === "night_start" ||
      p.id === "interval_minutes")) process.exit(9);
  if (params.some((p) => p.id === "library" || p.id === "wallpaper_dir")) process.exit(10);
  if (!monitors || !Array.isArray(monitors.items) || monitors.items.length < 1) process.exit(5);
  const text = JSON.stringify(doc);
  if (text.includes("sunset") || text.includes("日出日落") ||
      text.includes("switching") || text.includes("自动切换")) process.exit(6);
  if (!text.includes("\"type\":\"file\"") || !text.includes("\"hidePath\":true")) process.exit(11);
  if (text.includes("alternate_file") || text.includes("\"type\":\"pan\"") ||
      text.includes("theme_follow") || text.includes("lock_follow")) process.exit(13);
  if (!text.includes("data:image/jpeg;base64,")) process.exit(14);
  if (text.length >= 64 * 1024) process.exit(20);
  const lockRow = lock.items && lock.items[0];
  if (lockRow && lockRow.values && lockRow.values.lock_file &&
      !String(lockRow.values.preview || "").startsWith("data:image/jpeg;base64,")) process.exit(16);
  if (!monitors.items[0].values.wallpaper) process.exit(12);
  const title = monitors.items[0].titleEn || "";
  if (title.includes("\\\\.\\DISPLAY") || title.includes("DISPLAY1")) process.exit(15);
  if (text.includes("notes.txt")) process.exit(7);
  console.log(`  parsed ${params.length} control(s), ${monitors.items.length} monitor row(s), ` +
              `lock ${lockRow && lockRow.values.lock_file ? "resolved" : "not exposed"}`);
' "$DOCUMENT" || exit 1
PNG=$(find "$RUN" -maxdepth 1 -name 'Wallpaper.render-monitor-1-*.png' -print -quit)
[ -n "$PNG" ] || { echo "FAIL: no uniquely named display render was produced"; exit 1; }
node -e '
  const fs = require("fs");
  const png = fs.readFileSync(process.argv[1]);
  if (png.subarray(0, 8).toString("hex") !== "89504e470d0a1a0a" || png.length < 1000) process.exit(8);
  console.log(`  rendered PNG ${png.length} bytes`);
' "$PNG" || exit 1

echo
echo "== settings stayed in this feature's folder =="
fail=0
say() { printf '  %-68s %s\n' "$1" "$2"; [ "$2" = "ok" ] || fail=1; }

if grep -q '^schedule=clock$' "$RUN/Wallpaper.ini" 2>/dev/null &&
   grep -Eq '^monitor\.[0-9a-f]+\.wallpaper=.*Wallpaper\.display-.*\.bmp$' "$RUN/Wallpaper.ini" 2>/dev/null &&
   grep -Eq '^monitor\.[0-9a-f]+\.style=fit$' "$RUN/Wallpaper.ini" 2>/dev/null &&
   grep -Eq '^lock_file=.*Wallpaper\.lock-.*\.bmp$' "$RUN/Wallpaper.ini" 2>/dev/null; then
  say "its complete settings file is in its own scratch folder" ok
else
  say "its complete settings file is in its own scratch folder" FAIL
fi

after_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo absent )
after_ka=$( [ -f "$KA_INI" ] && md5sum "$KA_INI" | cut -d' ' -f1 || echo absent )
after_mc=$( [ -f "$MC_INI" ] && md5sum "$MC_INI" | cut -d' ' -f1 || echo absent )
[ "$before_sw" = "$after_sw" ] && say "SmoothWheel's settings untouched" ok || say "SmoothWheel's settings untouched" FAIL
[ "$before_ka" = "$after_ka" ] && say "KeepAwake's settings untouched" ok || say "KeepAwake's settings untouched" FAIL
[ "$before_mc" = "$after_mc" ] && say "MediaControl's settings untouched" ok || say "MediaControl's settings untouched" FAIL

if find "$RUN" -maxdepth 1 -name '*.tmp' -print -quit 2>/dev/null | grep -q .; then
  say "the atomic save leaves no temporary file behind" FAIL
else
  say "the atomic save leaves no temporary file behind" ok
fi

rm -f "$PROBE" "$DOCUMENT"
rm -rf "$RUN"

echo
if [ $fail -ne 0 ]; then
  echo "FAILED: Wallpaper's system capability, rules, or settings contract is broken"
  exit 1
fi
echo "OK: display identities, hidden path pickers, plugin-owned copies, thumbnails, modes, lock screen, and hidden switching"
