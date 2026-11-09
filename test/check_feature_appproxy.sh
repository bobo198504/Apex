#!/usr/bin/env bash
# Gate: AppProxy -- the page and settings file, plus a REAL intercepted TCP connection through both
# supported proxy protocols.
#
# The second half is the important one. A settings-only gate would pass for a feature that never patches
# anything. The probe therefore proves both directions:
#   * before loading the payload, the connection reaches a direct test server;
#   * after loading it, the same program and call route to a fake proxy and complete authentication.
#
# Everything is loopback, no process outside this probe is started, no input device is touched, and no packet
# leaves the machine.
set -uo pipefail
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
DLL="$ROOT/build/apex/Plugins/AppProxy/AppProxy.dll"
PAYLOAD="$ROOT/build/apex/Plugins/AppProxy/AppProxyHook.payload"
SERVICE="$ROOT/build/apex/Plugins/AppProxy/AppProxyService.exe"
SRC="$ROOT/_diag/feature_appproxy_probe.cpp"
PROBE="$ROOT/build/_feature_appproxy_probe.exe"
DIR="$ROOT/build/_appproxy_probe"

if [ ! -f "$DLL" ] || [ ! -f "$PAYLOAD" ] || [ ! -f "$SERVICE" ]; then
  echo "FAIL: AppProxy is not built (missing DLL, payload or service)"
  exit 1
fi
if [ ! -f "$SRC" ]; then
  echo "FAIL: $SRC is missing -- the feature would not be checked"
  exit 1
fi
if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is missing, so the feature document cannot be parsed"
  exit 1
fi

echo "== building the probe =="
g++ -std=c++17 -O2 -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/features/AppProxy" \
    -o "$PROBE" "$SRC" -lws2_32 -lmswsock || exit 1
echo "   ok"

rm -rf "$DIR"
mkdir -p "$DIR"

# The other feature's user data must not move. This is the same per-feature ownership rule every list-bearing
# feature is checked against.
SW_INI="$ROOT/build/apex/Plugins/SmoothWheel/SmoothWheel.ini"
before_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo absent )

echo
echo "== settings page and file =="
"$PROBE" doc "$DLL" "$DIR/" || exit 1

node -e '
  const fs = require("fs");
  const d = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
  const ids = (d.params || []).map(p => p.id);
  const want = ["serviceStatus", "bypassLocal", "windivertDir", "serviceAction", "blockQuic", "proxies", "apps"];
  for (const id of want)
    if (!ids.includes(id)) throw new Error("missing control " + id);
  if (ids.includes("addApp"))
    throw new Error("the running-process dropdown is back; programs must be typed by name");
  const status = d.params.find(p => p.id === "serviceStatus");
  if (status.type !== "status" || !status.textZh || !status.textEn ||
      ["ok", "warn", "error"].indexOf(status.level) < 0)
    throw new Error("the service status card is missing or malformed");
  const proxies = d.params.find(p => p.id === "proxies");
  if (proxies.type !== "group" || proxies.layout !== "rows" || proxies.items.length !== 2)
    throw new Error("proxy profiles are not a two-row group");
  if (proxies.fieldsWrap !== true)
    throw new Error("proxy fields are still crammed onto one line");
  if (proxies.items[1].title !== "Second" || proxies.items[1].values.port !== "18081")
    throw new Error("the second proxy profile was not preserved");
  const apps = d.params.find(p => p.id === "apps");
  if (apps.type !== "group" || apps.layout !== "rows")
    throw new Error("apps is not a rows group");
  if (!apps.addHintZh || !apps.addHintEn || apps.noAdd === true)
    throw new Error("apps does not offer a manual program-name field");
  if (apps.items.length !== 2) throw new Error("expected two application rows");
  if (apps.items[0].title !== "example.exe") throw new Error("the first row was not normalised");
  if (apps.items[0].values.proxy !== "Second") throw new Error("the per-app proxy choice was lost");
  if (apps.items[1].values.enabled !== 0) throw new Error("the row switch was not preserved");
  console.log("  parsed JSON with " + d.params.length + " controls, two proxies and two application rows");
' "$ROOT/build/_appproxy_doc.json" || exit 1

tr -d '\r' < "$DIR/AppProxy.ini" > "$DIR/AppProxy.normalised.ini"
for line in \
  'bypass_local=0' \
  'block_quic=1' \
  'windivert_dir=C:\Program Files\ProxyBridge' \
  'profile=Default|http|127.0.0.42|18080|alice|secret' \
  'profile=Second|socks5|127.0.0.43|18081||' \
  'app=example.exe|1|Second' \
  'app=*.game.exe|0|Default'
do
  if ! grep -qxF "$line" "$DIR/AppProxy.normalised.ini"; then
    echo "FAIL: settings file is missing '$line'"
    exit 1
  fi
done
echo "  settings file round-trip: ok"

after_sw=$( [ -f "$SW_INI" ] && md5sum "$SW_INI" | cut -d' ' -f1 || echo absent )
if [ "$before_sw" != "$after_sw" ]; then
  echo "FAIL: AppProxy changed SmoothWheel's exclude list"
  exit 1
fi

echo
echo "== real connect interception: HTTP CONNECT (connect) =="
"$PROBE" hook http "$PAYLOAD" "$DIR/" connect || exit 1

echo
echo "== real connect interception: HTTP CONNECT (ConnectEx) =="
"$PROBE" hook http "$PAYLOAD" "$DIR/" connectex || exit 1

echo
echo "== real connect interception: SOCKS5 (connect) =="
"$PROBE" hook socks5 "$PAYLOAD" "$DIR/" connect || exit 1

echo
echo "== real connect interception: SOCKS5 (ConnectEx) =="
"$PROBE" hook socks5 "$PAYLOAD" "$DIR/" connectex || exit 1

echo
echo "== the injected binary stays system-only =="
imports=$(objdump -p "$PAYLOAD" | grep 'DLL Name:' | sed 's/^[[:space:]]*//' | sort -u)
printf '%s\n' "$imports" | sed 's/^/   /'
if printf '%s\n' "$imports" | grep -vqE 'DLL Name: (KERNEL32|WS2_32)\.dll$'; then
  echo "FAIL: the payload acquired a runtime or GUI dependency"
  exit 1
fi
if [ ! -f "$ROOT/features/AppProxy/.crtless" ]; then
  echo "FAIL: the crtless build marker disappeared"
  exit 1
fi
service_imports=$(objdump -p "$SERVICE" | grep 'DLL Name:' | sed 's/^[[:space:]]*//' | sort -u)
if printf '%s\n' "$service_imports" | grep -qi 'WinDivert'; then
  echo "FAIL: the service statically links WinDivert; it must load the user's installed copy dynamically"
  exit 1
fi
echo "   service loads WinDivert dynamically: ok"
if ! grep -q 'advfirewall firewall add rule' "$ROOT/features/AppProxy/service/AppProxyService.cpp" ||
   ! grep -q 'advfirewall firewall delete rule' "$ROOT/features/AppProxy/service/AppProxyService.cpp"; then
  echo "FAIL: the service install/uninstall no longer manages the relay's Windows firewall rule"
  exit 1
fi
echo "   service install/uninstall manages its inbound firewall rule: ok"

rm -f "$PROBE" "$ROOT/build/_appproxy_doc.json"
rm -rf "$DIR"
echo
echo "OK: the page, file, process matcher and both TCP APIs through both proxy handshakes work"
