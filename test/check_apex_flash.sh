#!/usr/bin/env bash
# Gate: THE PANEL'S SURFACE IS ONE COLOUR, IN BOTH THEMES, AND IT AGREES WITH THE PAGE.
#
# The user's report was "主界面的打开的时候，会先白一下" -- the panel flashes white as it opens. The flash comes
# from a layer the PAGE CANNOT REACH: a WebView2 control paints white from the moment it is visible until the
# document has painted, and CSS has nothing to apply to a surface that is not showing the document yet. The
# fix is to tell the CONTROLLER what to paint (put_DefaultBackgroundColor), plus this window's own erase.
#
# ⚠️ WHAT THIS GATE CAN AND CANNOT DO, stated plainly because the distinction matters:
#
#   * IT CANNOT judge the flash. It lasts a few frames; whether it is gone is a question for the eye, and the
#     user does the UI testing in this project.
#   * IT CAN check the three things that must all hold for the fix to be real, and each of them is a way it
#     could silently not work:
#       1. the call was made and the runtime ACCEPTED it (a failed HRESULT is otherwise invisible);
#       2. the colour passed is the PAGE'S OWN background -- read out of panel.html here, not written down
#          in two places, because a mismatch is exactly the bright line / wrong-colour flash this fixes;
#       3. the same holds for BOTH appearances, since the panel follows the system and the value is chosen
#          from it (a fix that only worked in dark mode would be half a fix).
#
# (3) is done with the product's own theme rule: the host is started with a pinned theme and writes which
# mark it chose; this gate reads the colour it then told the browser to paint and requires it to be the one
# panel.html declares for that appearance.
#
# usage: test/check_apex_flash.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

BUILD="$ROOT/build"
RUN="$BUILD/_flash_run"
fail=0

PidsInRun() { apex_own_pids '_flash_run'; }
cleanup() {
  local p
  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
}
# ⚠️ ONE APEX PER MACHINE, SO THIS GATE CANNOT SHARE. It starts a host to watch the panel paint, and a host
# cannot start while the user's is running -- so the field is taken before the first launch and given back on
# the way out. apex_restore_the_field is in the EXIT trap only: cleanup() is called mid-run by the retry loop,
# and restoring there would put the user's host back in the middle of this gate's own run, blocking the next
# launch. See apex_take_the_field in test/lib_procs.sh.
trap 'cleanup; apex_restore_the_field' EXIT
apex_take_the_field

# THE PAGE'S OWN DECLARATION, so this gate compares the two implementations instead of restating one of them.
# It reads the `--bg` custom property out of the selector asked for: `:root` for the light theme (which is
# also what an `auto` light appearance falls through to) and the pinned dark block.
# THE TWO DECLARATIONS ARE FOUND BY THEIR OWN LINES rather than by parsing CSS: each is the first `--bg:` line
# inside its own block. A tiny parser would be a second implementation of CSS; recognising the lines the file
# actually has is not.
#
# ⚠️ INSIDE THE BLOCK, NOT "THE LINE AFTER `:root {`". That is what this read first, and it broke the moment a
# comment was added above the values in `:root` -- the gate then reported "could not read --bg" while the page
# declared it perfectly, i.e. it accused the product of the gate's own assumption. Both functions now scan the
# block and stop at its closing brace, which is the same shape and the same cost.
PageBgLight() {
  awk '/^:root \{/ { while ((getline line) > 0) { if (match(line, /--bg:[ \t]*#[0-9a-fA-F]{6}/)) { s=substr(line,RSTART,RLENGTH); sub(/.*#/,"",s); print tolower(s); exit } if (line ~ /\}/) exit } }' \
    "$ROOT/build/panel.built.html"
}
PageBgDark() {
  awk '/data-theme="dark"/ { while ((getline line) > 0) { if (match(line, /--bg:[ \t]*#[0-9a-fA-F]{6}/)) { s=substr(line,RSTART,RLENGTH); sub(/.*#/,"",s); print tolower(s); exit } if (line ~ /\}/) exit } }' \
    "$ROOT/build/panel.built.html"
}

LIGHT_BG=$(PageBgLight)
DARK_BG=$(PageBgDark)
echo "== the page's own colours =="
echo "   light --bg #$LIGHT_BG   dark --bg #$DARK_BG"
if [ -z "$LIGHT_BG" ] || [ -z "$DARK_BG" ]; then
  echo "   FAIL: could not read --bg out of apex/ui/panel.html (this gate compares against it)"
  exit 1
fi
if [ "$LIGHT_BG" = "$DARK_BG" ]; then
  echo "   FAIL: both appearances declare the same background, so one of the reads is wrong"
  exit 1
fi

# What the PANEL PROCESS told the browser to paint, and whether the runtime took it.
TryTheme() {
  local theme="$1" want="$2"
  # ⚠️ THE PREVIOUS RUN'S PROCESSES MUST BE GONE BEFORE THE FOLDER IS RECREATED, and finding that out is why
  # this loop exists: `rm -rf` on a folder a live process is holding fails PART WAY, leaving a half-deleted
  # copy -- and the next start then fails with "no such file: apex.exe" or, worse, runs from a stale folder
  # and reads the PREVIOUS theme's log. Both were observed, and both look like the code being broken.
  local p
  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  local gone=0
  while [ $gone -lt 60 ]; do
    [ -z "$(PidsInRun)" ] && break
    sleep 0.1
    gone=$((gone + 1))
  done
  rm -rf "$RUN"
  if [ -d "$RUN" ]; then
    echo "   FAIL: the previous run's folder could not be removed (something still holds it)"
    return 1
  fi
  cp -r "$BUILD/apex" "$RUN" 2>/dev/null || { echo "   FAIL: could not copy the built folder"; return 1; }
  printf 'theme=%s\n' "$theme" > "$RUN/apex.ini"
  rm -f "$RUN"/apex*.log

  ( cd "$RUN" && APEX_NO_TRAY=1 cmd //c start "" apex.exe >/dev/null 2>&1 )
  local tries=0
  while [ $tries -lt 40 ]; do
    [ -n "$(PidsInRun)" ] && break
    sleep 0.25
    tries=$((tries + 1))
  done
  # ⚠️ NOT SHOWN (APEX_NO_WINDOW), AND THE ASSERTION BELOW IS WHY THAT IS ALLOWED: what this gate reads is the
  # `webview background:` LINE -- the colour the panel handed the WebView2 controller, written as the controller
  # is created, before anything is on screen. Whether the window is visible is not part of what is being tested
  # (the gate's own header says the flash itself "needs eyes"), and showing it twice per delivery was two of the
  # three windows the user reported.
  ( cd "$RUN" && APEX_NO_WINDOW=1 cmd //c start "" apex-settings.exe >/dev/null 2>&1 )

  # The line is written as the controller is created; poll for it rather than sleeping a fixed time.
  local line="" i=0
  while [ $i -lt 100 ]; do
    line=$(grep -m1 '^webview background: ' "$RUN/apex-settings.log" 2>/dev/null)
    [ -n "$line" ] && break
    sleep 0.1
    i=$((i + 1))
  done

  local rc=0
  if [ -z "$line" ]; then
    echo "   FAIL: theme=$theme -- the panel never reported a background colour (took too long, or it crashed)"
    rc=1
  else
    printf '   theme=%-5s %s\n' "$theme" "$line"
    # 1. the runtime accepted it
    if ! printf '%s\n' "$line" | grep -q '0x00000000'; then
      echo "        FAIL: put_DefaultBackgroundColor did not succeed (the flash would come back)"
      rc=1
    fi
    # 2. and it is the page's own colour for this appearance
    if ! printf '%s\n' "$line" | grep -qi "#$want "; then
      echo "        FAIL: the panel paints #$want for $theme, but panel.html declares #$want -- mismatch"
      rc=1
    fi
  fi

  for p in $(PidsInRun); do taskkill //F //PID "$p" >/dev/null 2>&1; done
  return $rc
}

echo
echo "== what the panel tells the browser to paint =="
TryTheme dark "$DARK_BG" || fail=1
TryTheme light "$LIGHT_BG" || fail=1

echo
if [ $fail -eq 0 ]; then
  echo "OK: both appearances paint the page's own background before it loads, and the runtime accepts it"
  echo "    (whether the flash is GONE is a question for the eye -- this gate cannot see it)"
else
  echo "FAILED: the pre-paint background is not what the page declares"
fi
exit $fail
