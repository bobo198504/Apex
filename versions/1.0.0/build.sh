#!/usr/bin/env bash
# Build Apex: the host, the settings panel, and every feature.
#
# OUTPUT LAYOUT -- this is the user's requirement, and it is what makes the program portable:
#
#   build/apex/apex.exe                                the host; its settings sit BESIDE it (apex.ini)
#   build/apex/apex-settings.exe                       the settings panel, a separate process
#   build/apex/Plugins/SmoothWheel/SmoothWheel.dll     one folder per feature
#   build/apex/Plugins/SmoothWheel/SmoothWheel.ini     and that feature's settings beside its own dll
#
# Nothing is installed and nothing is written outside that folder. Copying build/apex/ somewhere else moves
# the whole program, settings included.
#
# WHY NOT CMAKE: the sibling plugin project uses a plain script against the same shared w64devkit toolchain,
# and two build systems is one more thing to keep in step than it is worth at this size.
#
# WHAT THIS DOES NOT NEED: the REAPER SDK. Apex links against the model headers in shared/ (which have no
# REAPER and no Windows dependency) and nothing else from that project.
set -euo pipefail

# The shared toolchain, per the workspace convention: portable tools live in _tools, never on PATH.
TOOLS="${TOOLS:-/d/Projects/Code/_tools}"
export PATH="$TOOLS/w64devkit/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
OUT="$ROOT/build/apex"
OBJ="$OUT/obj"

# THE WEBVIEW2 SDK SHIPS WITH THIS PROJECT (third_party/webview2), rather than being looked up in a NuGet
# cache. It used to be found under ~/.nuget, which made the build depend on what the machine happened to have
# installed at the right version -- and the whole point of keeping the SDK in the tree is that a clean clone
# builds. Only the header and the x64 import library are carried; the 10 MB static loader is not, because it
# is an MSVC binary that MinGW cannot link (it needs __security_cookie and _Init_thread_epoch).
WV2="$ROOT/third_party/webview2"

CC_FLAGS=(-std=c++17 -O2 -mwindows -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/shared")
LINK_LIBS=(-luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32 -lcomctl32)

mkdir -p "$OUT" "$OBJ"

echo "== toolchain =="
g++ --version | head -1

# ---------------------------------------------------------------------------
# INCREMENTAL COMPILATION, because the edit-build-test loop is where the time goes.
#
# A full build is about eight seconds, almost all of it recompiling translation units that did not change --
# and that runs after EVERY edit. The rule below is the poor man's make: a source is recompiled when it is newer
# than its object, or when any header it might have included is. The headers are checked as one set (the newest
# mtime under the include directories) rather than file by file, which over-compiles slightly when an unrelated
# header changes -- and that is the right way to be wrong: a MISSED rebuild is a stale binary that looks green,
# while an extra one costs a second.
#
# ⚠️ THE OBJECT DIRECTORY IS KEPT BETWEEN RUNS. That is the entire point, so nothing here clears it. (If a build
# ever looks wrong, `rm -rf build/apex/obj` is the way to force everything.)
#
# ⚠️ AND THE TEST IS `find -newer`, NOT A TIMESTAMP COMPARISON, because of what `find` IS here. This script puts
# the toolchain on PATH so it can call g++, and w64devkit ships its own **BusyBox `find`** -- which has no
# `-printf`. A version of this that asked find for the newest mtime printed nothing, the stamp file came out
# empty, and every object was then older than it and got recompiled: the incremental build silently did
# nothing at all, while LOOKING like it was working. `-newer FILE` and `-quit` exist in both finds and need no
# timestamp arithmetic, so "is anything newer than this object" is asked directly.
AnyHeaderNewerThan() {
  local ref="$1"
  shift
  [ -f "$ref" ] || return 0
  local found
  found=$(find "$@" -name '*.h' -newer "$ref" -print -quit 2>/dev/null)
  [ -n "$found" ]
}

# The include directories a translation unit under apex/ can reach: the project's own headers and the model.
HDR_DIRS=("$ROOT/apex" "$ROOT/common" "$ROOT/shared")

# Compile $1 -> $OBJ/$1.o unless it is up to date. Says which it did, so a build with nothing to do is visibly
# doing nothing rather than silently skipping.
CompileUnit() {
  local name="$1"
  local src="$ROOT/apex/$name.cpp"
  local obj="$OBJ/$name.o"
  if [ -f "$obj" ] && [ "$obj" -nt "$src" ] && ! AnyHeaderNewerThan "$obj" "${HDR_DIRS[@]}"; then
    echo "   $name.cpp  (up to date)"
    return 0
  fi
  echo "   $name.cpp"
  g++ "${CC_FLAGS[@]}" -c "$src" -o "$obj"
}

# ---------------------------------------------------------------------------
# THE HOST
# ---------------------------------------------------------------------------
echo "== resources =="
# windres resolves a .rc file's own relative paths from the DIRECTORY IT IS RUN IN, so the resource scripts
# refer to the artwork as ../../icon/*.ico and to the page as ui/panel.html (see apex.rc and panel.rc).
#
# Rebuilt only when something they pull in is newer: the two .ico files are 374 KB each and the page is 45 KB.
# (A .rc cannot be asked what it includes, so the condition is "any of these files changed".)
RES_SRC=("$ROOT/apex/apex.rc" "$ROOT/apex/panel.rc" "$ROOT/apex/apex.manifest"
         "$ROOT/apex/settings.manifest" "$ROOT/apex/icons.h"
         "$ROOT/icon/apex-light.ico" "$ROOT/icon/apex-dark.ico" "$ROOT/apex/ui/panel.html")
ResNeeded() {
  local obj="$1"
  shift
  [ ! -f "$obj" ] && return 0
  local f
  for f in "$@"; do
    [ "$f" -nt "$obj" ] && return 0
  done
  return 1
}
if ResNeeded "$OBJ/apex_res.o" "${RES_SRC[@]}"; then
  ( cd "$ROOT/apex" && windres apex.rc -o "$OBJ/apex_res.o" )
else
  echo "   apex.rc  (up to date)"
fi
if ResNeeded "$OBJ/panel_res.o" "${RES_SRC[@]}"; then
  ( cd "$ROOT/apex" && windres panel.rc -o "$OBJ/panel_res.o" )
else
  echo "   panel.rc  (up to date)"
fi

echo "== host =="
HOST_OBJS=()
for src in main host_win loader_win paths_win settings_host system_win; do
  CompileUnit "$src"
  HOST_OBJS+=("$OBJ/$src.o")
done

echo "== linking apex.exe =="
g++ "${CC_FLAGS[@]}" "${HOST_OBJS[@]}" "$OBJ/apex_res.o" -o "$OUT/apex.exe" "${LINK_LIBS[@]}"

# ---------------------------------------------------------------------------
# THE SETTINGS PANEL -- a SEPARATE EXE, not a window in the host.
#
# The hook lives in the OS input path, so a panel that renders a browser and waits on a user must not be able
# to affect it. Two processes mean the panel can be hung, killed, or restarted while smoothing keeps running
# unbroken -- see apex/settings_ipc.h for the protocol between them.
# ---------------------------------------------------------------------------
echo "== settings panel =="
if [ ! -f "$WV2/include/WebView2.h" ]; then
  echo "   SKIPPED: third_party/webview2 is missing."
  echo "   apex.exe still builds and runs; Settings... will report that the panel is unavailable."
else
  # ui_webview.cpp includes the WebView2 SDK header, so its condition also watches that tree -- it is a much
  # bigger header than anything in this project and it changes when the SDK is updated.
  PanelUnit() {
    local obj="$OBJ/ui_webview.o"
    local src="$ROOT/apex/ui_webview.cpp"
    if [ -f "$obj" ] && [ "$obj" -nt "$src" ] && ! AnyHeaderNewerThan "$obj" "${HDR_DIRS[@]}" &&
       [ "$obj" -nt "$WV2/include/WebView2.h" ]; then
      echo "   ui_webview.cpp  (up to date)"
      return 0
    fi
    echo "   ui_webview.cpp"
    g++ "${CC_FLAGS[@]}" -I"$WV2/include" -c "$src" -o "$obj"
  }
  PanelUnit
  CompileUnit settings_main
  echo "   linking apex-settings.exe"
  # system_win.o is the two registry reads both processes ask (the theme and the UI language). It is linked
  # into the panel deliberately INSTEAD OF host_win.o: the panel must not carry the hook, the injection
  # thread or the target lookup -- that is the whole point of it being a separate process.
  g++ "${CC_FLAGS[@]}" "$OBJ/settings_main.o" "$OBJ/ui_webview.o" "$OBJ/system_win.o" "$OBJ/panel_res.o" \
      -o "$OUT/apex-settings.exe" "$WV2/x64/WebView2Loader.dll.lib" "${LINK_LIBS[@]}"
  [ -f "$OUT/WebView2Loader.dll" ] && cmp -s "$WV2/x64/WebView2Loader.dll" "$OUT/WebView2Loader.dll" ||
    cp -f "$WV2/x64/WebView2Loader.dll" "$OUT/"
fi

# ---------------------------------------------------------------------------
# THE FEATURES
# ---------------------------------------------------------------------------
echo "== features =="
shopt -s nullglob
for FEAT in "$ROOT"/features/*/; do
  ID="$(basename "$FEAT")"
  SRC=$(ls "$FEAT"*.cpp 2>/dev/null | head -1)
  if [ -z "$SRC" ]; then
    echo "   $ID: no .cpp, skipped"
    continue
  fi
  DEST="$OUT/Plugins/$ID"
  mkdir -p "$DEST"
  # A feature rebuilds when its own source, its own headers, or anything under the include directories changed.
  # It does not depend on all of apex/*.h (it only includes abi.h), but checking the whole set is what makes this
  # one test rather than a dependency walk -- and over-compiling one small file is cheaper than a missed one.
  if [ -f "$DEST/$ID.dll" ] && [ "$DEST/$ID.dll" -nt "$SRC" ] && ! AnyHeaderNewerThan "$DEST/$ID.dll" "$FEAT" "${HDR_DIRS[@]}"; then
    echo "   $ID -> Plugins/$ID/$ID.dll  (up to date)"
    continue
  fi
  echo "   $ID -> Plugins/$ID/$ID.dll"
  # -shared: a feature is a dll the host loads. -I"$FEAT": headers kept beside the feature itself.
  g++ "${CC_FLAGS[@]}" -shared -I"$FEAT" "$SRC" -o "$DEST/$ID.dll" \
      -luser32 -lgdi32 -lole32 -ladvapi32
done

echo
echo "== built =="
ls -l "$OUT/apex.exe"
[ -f "$OUT/apex-settings.exe" ] && ls -l "$OUT/apex-settings.exe"
for d in "$OUT"/Plugins/*/; do ls -l "$d"*.dll 2>/dev/null; done
echo
echo "the folder to run or copy is: $OUT"
