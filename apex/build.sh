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

# ⚠️ TWO FLAG SETS, AND THE DIFFERENCE IS THE FEATURE BOUNDARY.
#
# The host and the panel may include anything: they ARE the host. A FEATURE may include only its contract
# (`abi.h`), its own helpers (`common/`) and the shared model (`shared/`) -- and until this split that was
# only a convention, because a feature is compiled with the same -I paths and nothing stopped it reaching
# into host.h and calling internals the ABI never promised to keep stable. The host's private headers now
# refuse to compile without APEX_BUILDING_HOST, which is defined HERE for the host's own sources and
# deliberately NOT for a feature's. (Probes that legitimately read host headers pass the same define -- see
# the gates; they are the host's own tools, not features.)
CC_FLAGS=(-std=c++17 -O2 -mwindows -DAPEX_BUILDING_HOST
          -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/shared")
FEATURE_FLAGS=(-std=c++17 -O2 -mwindows
               -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/shared")
LINK_LIBS=(-luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32 -lcomctl32 -lgdiplus)

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

# ⚠️ LINK ONLY WHEN SOMETHING IT IS MADE OF IS NEWER -- the same rule the objects already follow, applied to the
# step that had none. Both executables were relinked on EVERY run: a no-change build cost 3.2 s, and the link was
# 2.2 s of it (measured). The inputs are the objects plus the resource object, and the resource object is itself
# rebuilt when the .rc, the .ico or panel.html changes -- so "any input newer" covers the icons and the page too.
NeedsLink() {
  local out="$1"
  shift
  [ -f "$out" ] || return 0        # never built: link
  local f
  for f in "$@"; do
    [ -f "$f" ] || return 0        # a missing input is not a reason to skip
    [ "$f" -nt "$out" ] && return 0
  done
  return 1
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
# refer to the artwork as ../../icon/*.ico and to the page as ../build/panel.built.html (see apex.rc and panel.rc).
#
# ⚠️⚠️ THE PAGE IS ASSEMBLED FIRST, AND IT IS THREE FILES NOW. `apex/ui/panel.html` is the SHELL (the markup plus
# two markers); its `<style>` block lives in `apex/ui/panel.css` and its script in `apex/ui/panel.js`. This step
# inlines them into `build/panel.built.html`, which is what the .rc embeds -- so what the panel carries is
# byte-for-byte the same shape it always was (one file, one RCDATA), while a UI edit no longer means scrolling
# through a 3,200-line file to find the 40 lines it concerns.
#
# ⚠️ THE PARTS ARE THE SOURCE, THE ASSEMBLED FILE IS THE ARTEFACT. Editing `build/panel.built.html` is editing
# something the next build overwrites -- the same rule the rest of build/ follows.
PANEL_SHELL="$ROOT/apex/ui/panel.html"
PANEL_CSS="$ROOT/apex/ui/panel.css"
PANEL_JS="$ROOT/apex/ui/panel.js"
PANEL_BUILT="$ROOT/build/panel.built.html"
mkdir -p "$ROOT/build"
for part in "$PANEL_SHELL" "$PANEL_CSS" "$PANEL_JS"; do
  [ -f "$part" ] || { echo "   FAIL: $part is missing (the panel is assembled from three files)"; exit 1; }
done
# ⚠️ THE MARKER IS REPLACED BY THE FILE CONTENTS AND NOTHING ELSE. The shell already carries the style/script
# tags around it, and the first version of this added them here as well -- producing nested tags. The CSS still
# applied (an HTML style element runs to the FIRST closing tag), but the page also rendered a stray closing tag
# as visible text. Caught by reading the assembled file, not by looking at the panel.
#
# ⚠️ AND THE AWK PROGRAM CARRIES NO COMMENT WITH AN APOSTROPHE: it is inside a single-quoted shell string, so
# "the file's contents" ended the string and bash tried to run `<style>` as a command. The note lives out here.
awk -v css="$PANEL_CSS" -v js="$PANEL_JS" '
  /__APEX_PANEL_CSS__/ { while ((getline line < css) > 0) print line; next }
  /__APEX_PANEL_JS__/  { while ((getline line < js)  > 0) print line; next }
  { print }
' "$PANEL_SHELL" > "$PANEL_BUILT" || { echo "   FAIL: the panel could not be assembled"; exit 1; }
echo "   panel: $(( $(wc -c < "$PANEL_SHELL") )) + $(( $(wc -c < "$PANEL_CSS") )) + $(( $(wc -c < "$PANEL_JS") )) -> $(( $(wc -c < "$PANEL_BUILT") )) bytes"
#
# Rebuilt only when something they pull in is newer: the two .ico files are 374 KB each and the page is 45 KB.
# (A .rc cannot be asked what it includes, so the condition is "any of these files changed".)
RES_SRC=("$ROOT/apex/apex.rc" "$ROOT/apex/panel.rc" "$ROOT/apex/apex.manifest"
         "$ROOT/apex/settings.manifest" "$ROOT/apex/icons.h"
         "$ROOT/icon/apex-light.ico" "$ROOT/icon/apex-dark.ico" "$PANEL_BUILT")
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
for src in main host_win loader_win paths_win settings_host system_win quickpanel_win; do
  CompileUnit "$src"
  HOST_OBJS+=("$OBJ/$src.o")
done

if NeedsLink "$OUT/apex.exe" "${HOST_OBJS[@]}" "$OBJ/apex_res.o"; then
  echo "== linking apex.exe =="
  g++ "${CC_FLAGS[@]}" "${HOST_OBJS[@]}" "$OBJ/apex_res.o" -o "$OUT/apex.exe" "${LINK_LIBS[@]}"
else
  echo "== linking apex.exe  (up to date) =="
fi

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
  if NeedsLink "$OUT/apex-settings.exe" "$OBJ/settings_main.o" "$OBJ/ui_webview.o" "$OBJ/system_win.o" "$OBJ/panel_res.o"; then
    echo "   linking apex-settings.exe"
    # system_win.o is the two registry reads both processes ask (the theme and the UI language). It is linked
    # into the panel deliberately INSTEAD OF host_win.o: the panel must not carry the hook, the injection
    # thread or the target lookup -- that is the whole point of it being a separate process.
    g++ "${CC_FLAGS[@]}" "$OBJ/settings_main.o" "$OBJ/ui_webview.o" "$OBJ/system_win.o" "$OBJ/panel_res.o" \
        -o "$OUT/apex-settings.exe" "$WV2/x64/WebView2Loader.dll.lib" "${LINK_LIBS[@]}"
  else
    echo "   linking apex-settings.exe  (up to date)"
  fi
  [ -f "$OUT/WebView2Loader.dll" ] && cmp -s "$WV2/x64/WebView2Loader.dll" "$OUT/WebView2Loader.dll" ||
    cp -f "$WV2/x64/WebView2Loader.dll" "$OUT/"
fi

# ---------------------------------------------------------------------------
# THE DIAGNOSTIC TOOLS
# ---------------------------------------------------------------------------
echo "== tools =="
#
# ⚠️ apex_owners.exe ANSWERS "IS APEX RUNNING?" AND BOTH THE GATES AND THE DEPLOY SCRIPT DEPEND ON IT.
#
# It is built here, into build/, because everything that needs it is a shell script. It identifies an Apex
# process by its WINDOW CLASS (ApexHostWnd / ApexSettingsWnd) rather than by image name -- the name `apex.exe`
# is generic, and killing by name is the mistake this project has already made twice (see test/lib_procs.sh).
# A missing executable would make "the field is clear" mean "the question could not be asked", so a failure
# here is fatal rather than a warning.
# ⚠️ AND ONLY WHEN IT CHANGED, like everything else here: this was an unconditional compile, so every build --
# including the one where nothing changed -- paid for a g++ invocation. (Measured on a quiet machine: one such
# compile of this small tool is ~0.4 s, and it was the single largest item in a no-change build.)
if NeedsLink "$ROOT/build/apex_owners.exe" "$ROOT/_diag/apex_owners.cpp"; then
  g++ -std=c++17 -O2 -mconsole -o "$ROOT/build/apex_owners.exe" "$ROOT/_diag/apex_owners.cpp" -luser32 || exit 1
  echo "   apex_owners.exe -> build/apex_owners.exe"
else
  echo "   apex_owners.exe  (up to date)"
fi

# ---------------------------------------------------------------------------
# THE FEATURES
# ---------------------------------------------------------------------------
echo "== features =="
shopt -s nullglob
for FEAT in "$ROOT"/features/*/; do
  ID="$(basename "$FEAT")"
  DEST="$OUT/Plugins/$ID"

  # -------------------------------------------------------------------------
  # A DEVELOPMENT FEATURE IS NOT A PRODUCT FEATURE.
  #
  # ⚠️ `APEX_RELEASE=1` SKIPS ANY FOLDER WITH A `.dev-only` MARKER. Those are tools that exist to answer a
  # question about the SEAM rather than to do something for the user (see features/AuditStub/.dev-only), and
  # one of them shipping inside the product is a surprise nobody notices -- a panel with a feature the user
  # never installed is how that was discovered.
  #
  # ⚠️ IT IS A MARKER FILE RATHER THAN A LIST IN THIS SCRIPT on purpose: a list is a second place that has to
  # be kept in step with the tree, and the failure mode of getting it wrong is "it ships after all". A marker
  # travels with the thing it marks, and the developer who adds a stub is the one who knows to mark it.
  #
  # ⚠️ AND THE DEFAULT IS TO BUILD IT. The gates need the stub (check_apex_modular.sh asserts it exists and
  # that the host loads a foreign feature), and a development build is where that matters. Release is the
  # deliberate act.
  #
  # ⚠️ A PREVIOUS RELEASE'S PLUGIN FOLDER IS REMOVED TOO, not just left unbuilt: build/apex/ is a folder the
  # user copies wholesale, so a feature that was there from an earlier release build has to disappear from it,
  # or "hidden" would only mean "not rebuilt" and the old DLL would ship anyway.
  # -------------------------------------------------------------------------
  if [ "${APEX_RELEASE:-0}" != "0" ] && [ -f "$FEAT/.dev-only" ]; then
    rm -rf "$DEST"
    echo "   $ID: DEV-ONLY, skipped for release"
    continue
  fi

  # -------------------------------------------------------------------------
  # A FEATURE IS A DLL, AND IT DOES NOT HAVE TO BE C++.
  #
  # ⚠️ A DIRECTORY WITH NO .cpp IS NOT "SKIPPED" -- IT IS A RUST FEATURE OR A MISTAKE, and those two must not
  # look the same. The first version of this loop printed "no .cpp, skipped" and carried on, which is how a
  # feature that simply failed to be recognised would ship as a working build with a plugin that is not there.
  # Now: a Cargo.toml means cargo builds it, and anything else is an ERROR.
  #
  # ⚠️ CARGO IS OPTIONAL AND ITS ABSENCE IS AN ERROR, NOT A SKIP. The Rust feature is the only one that needs
  # it, so a machine without cargo can still build Apex -- but it must SAY so rather than quietly produce a
  # program missing a plugin the user can see in the source tree.
  # -------------------------------------------------------------------------
  # ⚠️ THE `for` LOOP IS THE TEST, NOT `ls`. With `nullglob` on, an unmatched `"$FEAT"*.cpp` expands to
  # NOTHING -- but wrapping that in `$(ls ... | head -1)` reintroduces the problem it was meant to avoid: when
  # the glob expands to nothing, `ls` is called with only the directory name and LISTS THE DIRECTORY, so the
  # substitution is non-empty (here it returned "AGENTS.md") and the check reads every non-C++ feature as a C++
  # one. The build then handed `AGENTS.md` to g++ as source. Collecting with `for` cannot do that: an
  # unmatched glob contributes zero iterations, and `$CPPS` really is empty.
  CPPS=""
  for f in "$FEAT"*.cpp; do
    [ -e "$f" ] && CPPS="$f"
  done
  if [ -z "$CPPS" ]; then
    if [ -f "$FEAT/Cargo.toml" ]; then
      if ! command -v cargo >/dev/null 2>&1 && [ ! -x "$HOME/.cargo/bin/cargo.exe" ]; then
        echo "   $ID: RUST FEATURE BUT NO CARGO -- cannot build it (see AGENTS.md)"
        exit 1
      fi
      # -------------------------------------------------------------------------
      # ⚠️⚠️ RUST MUST NOT SEE THIS SCRIPT'S MinGW, AND THE FAILURE IS BEWILDERING UNTIL YOU KNOW WHY.
      #
      # This script puts w64devkit FIRST on PATH (see the top) because every C++ compile needs it. Rust's
      # windows-gnu target ships its own linker driver and expects its OWN MinGW runtime -- and w64devkit's
      # has no libgcc_eh, so a clean `cargo build` under this PATH dies with:
      #
      #     ld.exe: cannot find -lgcc_eh
      #     error: could not compile `serde_json` (build script) due to 1 previous error
      #
      # which names a DEPENDENCY and a LIBRARY, not the real cause (two toolchains fighting over PATH).
      # MEASURED: with rustup's toolchain first the same crate builds in a second; with w64devkit first it
      # fails outright. That would have shipped as "the AutoIME feature does not build".
      #
      # SO PATH IS REBUILT FOR CARGO: cargo's own directory first, w64devkit REMOVED. The C++ compiles are
      # unaffected -- they have already run by now, and they never read PATH again.
      # -------------------------------------------------------------------------
      CLEAN_PATH=""
      OLD_IFS="$IFS"
      IFS=':'
      for p in $PATH; do
        case "$p" in
          *w64devkit*) ;; # dropped: Rust's own linker must win
          *) CLEAN_PATH="${CLEAN_PATH:+$CLEAN_PATH:}$p" ;;
        esac
      done
      IFS="$OLD_IFS"
      NEW_PATH="$HOME/.cargo/bin:$CLEAN_PATH"
      mkdir -p "$DEST"
      # The crate is named in Cargo.toml (`[lib] name`), and the file it produces is that name + .dll -- not
      # the folder name. Read it rather than assuming the two match.
      LIBNAME=$(awk -F'"' '/^\[lib\]/{l=1} l && /^name/{print $2; exit}' "$FEAT/Cargo.toml")
      [ -n "$LIBNAME" ] || LIBNAME="$ID"
      echo "   $ID -> Plugins/$ID/$LIBNAME.dll   (cargo)"
      # ⚠️ THE PATH CHANGE IS SCOPED TO THIS ONE COMMAND, IN A SUBSHELL. An earlier version exported it for
      # the rest of the script, and the C++ feature compiled after this one then failed with "g++: command not
      # found" -- the exact inverse of the bug above, caused by the fix for it. A feature's build must not
      # change how the next feature builds.
      # ⚠️ `--quiet` IS NOT COSMETIC: it keeps a CLEAN build from writing anything to stderr, so that a
      # non-zero exit code means "something is wrong" again. Cargo logs its progress ("Compiling", "Finished")
      # to stderr by design, and a caller that merges stderr into stdout (PowerShell does -- measured: a
      # command that only writes one line to stderr and exits 0 comes back as exit 1) then reports a green
      # deploy as a failure. rustc's own warnings and errors are NOT cargo log messages and still come
      # through at this level -- measured by putting a warning back into AutoIME and rebuilding.
      ( cd "$FEAT" && PATH="$NEW_PATH" cargo build --release --quiet ) || { echo "   $ID: cargo build FAILED"; exit 1; }
      cp -f "$FEAT/target/release/$LIBNAME.dll" "$DEST/$LIBNAME.dll" || exit 1
      continue
    fi
    echo "   $ID: NOT A FEATURE -- no .cpp and no Cargo.toml"
    exit 1
  fi

  SRC="$CPPS"
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
  # FEATURE_FLAGS, not CC_FLAGS -- no APEX_BUILDING_HOST, so the host's private headers refuse to compile
  # here. That is the boundary being enforced by the compiler instead of by everyone remembering.
  #
  # ⚠️ THE LINK LINE IS SHARED BY EVERY FEATURE, AND THAT IS ON PURPOSE. A feature that needs a library the line
  # does not carry would otherwise have to be special-cased here -- one more place the build script knows a
  # feature by name, which is exactly what "adding a feature is adding a folder" rules out. The four below were
  # added for MediaControl and cost the other features nothing (an unread import is not linked in):
  #   -ldxva2      DDC/CI: GetPhysicalMonitorsFromHMONITOR, GetVCPFeatureAndVCPFeatureReply, SetVCPFeature
  #   -lwbemuuid   WMI: CLSID_WbemLocator / IID_IWbemLocator, the internal-panel brightness path
  #   -loleaut32   VARIANT and BSTR, which every WMI answer arrives in
  #   -luuid       the IIDs and CLSIDs themselves (MMDeviceEnumerator, IAudioSessionManager2, PROPERTYKEYs)
  #   -lshell32    already linked into the host; a feature that draws a window may want it too
  g++ "${FEATURE_FLAGS[@]}" -shared -I"$FEAT" "$SRC" -o "$DEST/$ID.dll" \
      -luser32 -lgdi32 -lole32 -ladvapi32 -ldxva2 -lwbemuuid -loleaut32 -luuid -lshell32
done

echo
echo "== built =="
ls -l "$OUT/apex.exe"
[ -f "$OUT/apex-settings.exe" ] && ls -l "$OUT/apex-settings.exe"
for d in "$OUT"/Plugins/*/; do ls -l "$d"*.dll 2>/dev/null; done
echo
echo "the folder to run or copy is: $OUT"
