#!/usr/bin/env bash
# _diag/ui_probe.sh -- build and run the "can this process restyle a stranger's window" probe, then turn its
# pictures into PNG so they can be looked at.
#
# WHAT IT ANSWERS, and why each question blocks the feature:
#   1. CAN A DIFFERENT PROCESS SET DwmSetWindowAttribute? The feature runs inside apex.exe, so every window it
#      wants to touch belongs to another process. If this fails, "no injection" is dead and the answer to the
#      user is "it needs injection after all".
#   2. WHERE DOES THE MENU BAR LIVE? The user asked for menus by name; a window's menu bar is not a window, so
#      SetWindowTheme is the only cross-process lever that might reach it.
#   3. DOES AN ALREADY-VISIBLE WINDOW NEED A FORCED FRAME CHANGE, and which of the two ways is safe? One of
#      them (SWP_FRAMECHANGED) sends WM_NCCALCSIZE into the target program -- fine on a window this probe owns,
#      a risk on a stranger's.
#
# ⚠️ THIS IS NOT A GATE AND IT IS NOT IN test/run_all.sh. It puts ONE window of its own on screen, topmost and
# in the foreground, for a few seconds -- a DWM material is only drawn while its window is active (measured:
# _diag/mica_probe.cpp). It never touches the keyboard or the mouse, it uses its own exe name and its own
# folder, it does not start, stop or disturb the user's Apex, and it touches NO window that belongs to anybody
# else: the only windows it changes are the two it created itself.
#
# usage: bash _diag/ui_probe.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

RUN="$ROOT/build/_ui_run"
SRC="$ROOT/_diag/ui_probe.cpp"
EXE="$RUN/ui_probe.exe"
LOG="$RUN/ui_probe.log"

mkdir -p "$RUN"

# ---------------------------------------------------------------------------
# BUILD. No project header is included, so the flags are just "a small windows exe": -mwindows (no console
# flashes when the probe re-runs itself as the applier process), and the two libraries the calls need.
# ⚠️ dwmapi and uxtheme are NOT linked: every call is reached through LoadLibrary + GetProcAddress, which is
# also how the feature will have to do it (MinGW's dwmapi.h on this toolchain predates DWMWA_SYSTEMBACKDROP_TYPE
# and friends -- see _diag/mica_probe.cpp).
# ---------------------------------------------------------------------------
if [ ! -f "$EXE" ] || [ "$SRC" -nt "$EXE" ]; then
  echo "== building ui_probe.exe =="
  g++ -std=c++17 -O2 -mwindows "$SRC" -o "$EXE" -luser32 -lgdi32 || exit 1
else
  echo "== ui_probe.exe (up to date) =="
fi

apex_kill_own '_ui_run' 'ui_probe'
rm -f "$RUN"/*.log "$RUN"/*.bmp "$RUN"/*.png

echo
echo "⚠️  ONE test window will appear in the middle of the screen and take the foreground for ~5 s."
echo "    It closes itself. Do not click anything; you do not need to."
echo

( cd "$RUN" && ./ui_probe.exe "--dir=$(apex_win_path "$RUN")" "--log=$(apex_win_path "$LOG")" >/dev/null 2>&1 )

if ! apex_wait_line "$LOG" '^probe done' 30; then
  echo "FAIL: the probe never reported \`probe done\` (30 s)"
  [ -f "$LOG" ] && cat "$LOG"
  apex_kill_own '_ui_run' 'ui_probe'
  exit 1
fi

# ---------------------------------------------------------------------------
# BMP -> PNG, because the pictures are the half of this that has to be LOOKED at and the viewer here reads PNG.
#
# ⚠️ NOT WRITTEN BY THE PROBE ITSELF: Gdiplus::Bitmap::Save segfaults on this toolchain after a successful
# capture (recorded in _diag/mica_probe.cpp), which turns a working capture into an apparent failure. The BMP
# writer above has never done that, so the conversion is somebody else's job.
# ---------------------------------------------------------------------------
echo "== converting the pictures =="
powershell -NoProfile -Command '
  Add-Type -AssemblyName System.Drawing
  $dir = $args[0]
  Get-ChildItem -LiteralPath $dir -Filter *.bmp | ForEach-Object {
    $png = [System.IO.Path]::ChangeExtension($_.FullName, ".png")
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    try { $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $img.Dispose() }
    Write-Output ("   " + $_.Name + " -> " + [System.IO.Path]::GetFileName($png))
  }
' "$(apex_win_path "$RUN")" 2>&1

# ---------------------------------------------------------------------------
# THE LOG, and the two things in it that are not decoration:
#
#   * `mark=as drawn` on every step is the capture's own control. If the magenta block this process painted
#     does not read back, the capture was looking at something else and no other line here means anything.
#   * `foreground=` is part of the measurement rather than a note: a DWM material falls back to a solid colour
#     while its window is inactive (documented, and measured in _diag/mica_probe.cpp), so the Mica step is only
#     a statement about Mica if it says 1.
# ---------------------------------------------------------------------------
echo
echo "== the log =="
cat "$LOG"
echo
if [ -f "$RUN/applier.log" ]; then
  echo "== what the OTHER process said =="
  cat "$RUN/applier.log"
  echo
fi

echo "== the questions, as the log answers them =="
miss=$(apex_count_line "$LOG" 'mark=MISSING')
if [ "${miss:-0}" -gt 0 ]; then
  printf '   !! %s step(s) did not read back this process s own control block -- those pictures are not of this window\n' "$miss"
else
  printf '   ok  every step read back the control block: the captures are of this window\n'
fi
grep -q 'SYSTEMBACKDROP_TYPE, 2) hr=0x00000000' "$LOG" &&
  printf '   ok  a DWM material was accepted on a window with a painted client area\n' ||
  printf '   note the material call did not return S_OK -- read the hr above\n'

printf '\n   the pictures, in order:\n'
for f in "$RUN"/*.png; do [ -f "$f" ] && printf '      %s\n' "$f"; done
echo
echo "   Look at 00-baseline vs 01-dark-no-refresh vs 02-dark-redraw-frame vs 03-dark-setpos-framechanged:"
echo "   that is the title bar changing, and which of the two forced-redraw calls it actually needed."
echo "   Then 07-menu-theme-darkmode-explorer: that is whether SetWindowTheme reaches the MENU BAR."
echo "   And 09/10-crosseproc-*: the same dark flag set by a DIFFERENT process -- the measurement the whole"
echo "   no-injection plan rests on."
