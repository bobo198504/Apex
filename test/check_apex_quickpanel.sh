#!/usr/bin/env bash
# Gate: THE QUICK PANEL -- the arithmetic it is drawn from, and what the features put in it.
#
# WHY THIS GATE EXISTS. The flyout is a window the user opens with one click, and it can be wrong in ways that
# look like nothing at all:
#
#   * A LAYOUT AND A HIT TEST THAT DISAGREE. The painter and the click handler both ask "where is this row",
#     and if they each worked it out for themselves a switch would be drawn one place and clickable in another.
#     That exact bug has already happened once in this project, on a settings page, and it presents as "the
#     switch does nothing" (see docs/rules/panel.md, the pointer-events note). So the geometry is checked as
#     arithmetic: every row inside its block, no two overlapping, and the centre AND both corners of a row
#     answering that row.
#
#   * A VALUE THAT IS NOT WHAT IT LOOKS LIKE. A fader whose ends are not the range, a knob that jumps on the
#     first pixel of a drag, a step that does not land on the item's own grid -- all of them are silent.
#
#   * A FADE OUTSIDE THE RANGE THE USER ASKED FOR (0.2-0.5 s).
#
#   * A FEATURE WHOSE quickItems ANSWER WOULD DRAW A CONTROL THAT LIES: an empty id, an unknown shape, a range
#     with min >= max, or a value outside its own bounds. The panel cannot tell any of those from a good answer.
#
#   * AND THE COLOURS DRIFTING FROM THE SETTINGS PAGE'S. The flyout is drawn in C++ (apex/quickpalette.h) and
#     the page is a stylesheet (apex/ui/panel.css); the user sees them minutes apart, so the probe reads BOTH
#     and fails when a value stops matching. "Keep these in step" is a comment, not a mechanism -- and this
#     project has already had one mapping error that every self-consistent assertion passed (see
#     docs/rules/ui-look.md, the icon light/dark note).
#
# IT RUNS THE REAL ARTEFACTS: the built DLLs are loaded exactly as the host loads them (entry point by name,
# ABI version and struct size compared BEFORE anything is read), and each one is given a SCRATCH FOLDER of its
# own -- never the user's settings, which is the rule every gate here follows.
#
# usage: test/check_apex_quickpanel.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

PLUGINS="$ROOT/build/apex/Plugins"
OUT="$ROOT/build/_quickpanel_probe.exe"

for need in SmoothWheel KeepAwake AutoIME; do
  if [ ! -f "$PLUGINS/$need/$need.dll" ]; then
    echo "the features are not built: $PLUGINS/$need/$need.dll"
    echo "(run apex/build.sh first -- this gate checks the ARTEFACT, not the source)"
    exit 1
  fi
done
# ⚠️ AuditStub IS NOT REQUIRED, AND THAT IS NOT SLOPPINESS. It carries the `.dev-only` marker, so a RELEASE
# build deliberately removes it from the artefact folder (see apex/build.sh) -- which `check_apex_deploy`
# exercises, and which is why requiring it here made this gate red only when the suite ran as a whole. It is
# the one feature that answers `quickItems` with null ON PURPOSE, so it is worth checking when it is there;
# AutoIME (required above) covers the same case and is in every build.

echo "== building the probe =="
# ⚠️ -DAPEX_BUILDING_HOST: the quick panel's headers refuse to compile without it, which is what makes "a
# feature may not include the host's private headers" a contract rather than a convention. A probe is the
# host's own tool, so it passes the same define the host does (see apex/build.sh).
# ⚠️ AND -lole32, because the probe initialises COM before it loads the features: since ABI 13 one of them
# enumerates WASAPI sessions on the calling thread, and without COM it answers with zero application rows --
# which would make "the flyout offers as many faders as the page has rows" pass by having nothing on both sides.
g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I"$ROOT/apex" -I"$ROOT/common" \
    -o "$OUT" "$ROOT/_diag/quickpanel_probe.cpp" -lole32 -luser32 || exit 1
echo "   ok"
echo

"$OUT" "$ROOT" "$PLUGINS"
rc=$?
rm -f "$OUT"
rm -rf "$ROOT/build/_quickpanel_probe_dir"
if [ $rc -ne 0 ]; then
  exit $rc
fi

echo
echo "== 7. what it LOOKS like: rendered, and checked by pixels =="
#
# ⚠️ WHY A PICTURE IS PART OF A GATE. "There is no outer panel" and "the gap between two panes is air" are
# statements about the WHOLE WINDOW, and the arithmetic in quickpanel.h cannot see either of them: the layout is
# identical whether or not something paints a background behind it. The user reported both, twice ("现在看上去是
# 大面板装着小面板", then "视觉上只要小面板就够"), and both times the only instrument was their eye one deploy
# later -- which is the wrong shape for a loop. So the panel is rendered TWICE, once over a bare backdrop and
# once with the panel on top, and the two are compared pixel by pixel (see _diag/quickpanel_preview.cpp for the
# three things that fall out of it).
#
# ⚠️ AND THE PNGs IT WRITES ARE NOT A SIDE EFFECT TO BE CLEANED UP. When the answer is "that looks wrong", the
# next question is "what does it look like", and the picture IS the answer -- they land in build/ beside
# everything else this suite produces.
#
# ⚠️ IT CALLS THE SAME `PaintPanel` THE WINDOW CALLS (apex/quickpaint.h), so what is checked and what is looked
# at cannot be two different panels.
echo "== building the preview =="
PREVIEW="$ROOT/build/_quickpanel_preview.exe"
g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I"$ROOT/apex" -I"$ROOT/common" \
    -o "$PREVIEW" "$ROOT/_diag/quickpanel_preview.cpp" -lgdiplus -lgdi32 || exit 1
echo "   ok"
echo
"$PREVIEW" "$ROOT/build"
rc=$?
rm -f "$PREVIEW"
if [ $rc -ne 0 ]; then
  exit $rc
fi

echo
echo "== 8. and nothing asks the WINDOW MANAGER to paint behind it =="
#
# ⚠️ THIS ONE-LINE GREP COVERS A WHOLE ROUND TRIP, AND IT IS THE MOST VALUABLE CHECK IN THIS FILE.
#
# The panel once asked the DWM for `ACCENT_ENABLE_ACRYLICBLURBEHIND` -- for more glass -- and the accent is
# painted over THE WHOLE WINDOW RECTANGLE. A window is a rectangle (it has to be), so the blur and its tint
# covered the gaps between the panes and the margin around them too, and every pane ended up lying on one flat
# sheet. The user's report was exactly that, twice: "外层大板去掉" and then "还是有大面板".
#
# ⚠️ AND THE PIXEL GATE ABOVE COULD NOT SEE IT, because the accent is applied by the window manager and not by
# `PaintPanel` -- the preview renders `PaintPanel`, so its picture was correct while the screen was not, and
# every assertion passed. That is the boundary of what a rendered preview can answer, and the reason this check
# takes a different form: it reads the SOURCE.
#
# ⚠️ THE TWO ARE MUTUALLY EXCLUSIVE BY CONSTRUCTION -- a filled window rectangle cannot also be "the gaps
# between the panes are the desktop" -- so no future edit is free to add this back "for the glass".
#
# ⚠️⚠️ AND THE FIRST VERSION OF THIS CHECK COULD NEVER FIRE. It was written as
#     GetProcAddress[^)]*SetWindowCompositionAttribute
# and the real call reads
#     ...GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowCompositionAttribute");
# where `[^)]*` stops dead at the `)` of `GetModuleHandleA(` -- so the pattern could not match the very line it
# was written for, and the gate printed `ok` for a file that had the call in it. That is the failure this
# project has a rule about ("一扇不会红的门比没有门更糟"), and the cure is the same one check_apex_modular.sh
# uses: FEED THE SCAN A FIXTURE THAT MUST HIT before trusting what it says about the real file.
FRAME="$ROOT/apex/quickpanel_win.cpp"
DWM_RE='"SetWindowCompositionAttribute"|"DwmSetWindowAttribute"|SetWindowCompositionAttribute[[:space:]]*\(|DwmSetWindowAttribute[[:space:]]*\('

SELFTEST=$(mktemp)
printf '  F f = (F)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowCompositionAttribute");\n' > "$SELFTEST"
dwm_hit=$(grep -cE "$DWM_RE" "$SELFTEST")
printf '// there is no `SetWindowCompositionAttribute` anywhere in this file any more\n' > "$SELFTEST"
dwm_miss=$(grep -cE "$DWM_RE" "$SELFTEST")
rm -f "$SELFTEST"
if [ "$dwm_hit" != "1" ] || [ "$dwm_miss" != "0" ]; then
  echo "   FAIL: this scan cannot tell a real call from the same name in prose (call=$dwm_hit prose=$dwm_miss)"
  exit 1
fi
echo "   ok  the scan finds a real call, and ignores the same name in a comment"

if grep -qE "$DWM_RE" "$FRAME"; then
  echo "   FAIL: the panel asks the window manager to paint behind it. That fills the WINDOW's rectangle --"
  echo "         gaps and margin included -- and every pane ends up on one flat sheet, which is the '大面板'"
  echo "         the user asked to be rid of (see the note where that call used to be, in quickpanel_win.cpp)."
  exit 1
fi
echo "   ok  the panel paints itself and asks the window manager for nothing"
echo
echo "OK: the quick panel is placed by arithmetic, filled by the features that were MAPPED, drawn as separate"
echo "    panes over a real desktop, and painted by nobody else"
exit 0
