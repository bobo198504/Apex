#!/usr/bin/env bash
# PACKAGE THE PORTABLE BUILD -- the folder you can copy anywhere, as one zip.
#
# ⚠️⚠️ THE FILE LIST IS EXPLICIT, AND THAT IS THE WHOLE POINT. `build/apex` is not only an output folder: it is a
# folder this program has been RUN in, so it accumulates things that must never ship --
#   * the user's own settings: `apex.ini`, `Plugins/*/*.ini`, AutoIME's `config.json`
#   * logs and diagnostics: `apex.log`, `autoime_debug.log`, `observed.json`
#   * WebView2's profile data, the compiler's `obj/`
# "Zip up the folder" ships the author's configuration, which is exactly what a portable package must not contain.
# It became urgent on 2026-10-07, when this repository was about to be made public and an old package turned out to
# have been published the same way.
#
# ⚠️ THE VERSION COMES FROM apex/host.h (`APEX_HOST_VERSION`), the one place it is defined -- the file name, the
# sidebar and the release notes must not be able to disagree about which build this is.
#
# ⚠️⚠️ AND THIS IS THE ONE PLACE THAT GUARDS THE *RELEASE*, WHICH IS NOT THE SAME AS GUARDING THE REPOSITORY.
# `VolumeLock` is private (see features/VolumeLock/NOT_FOR_RELEASE.md): it does not go into the repository and is not
# open source. Three things said so -- the feature's own file header, that document, and `.gitignore` -- and **all
# three only cover `git`**. This script does not consult `git` at all: it zips `build/apex/Plugins/*/`, which is the
# folder the feature is BUILT into. So on 2026-10-12 the v1.5.0 package was about to publish VolumeLock.dll while every
# existing guard stayed silent, and v1.4.0's package happened to be clean only because that DLL was not built at the
# time. **A rule that is only about the repository does not protect the thing users download.**
#
# ⚠️ IT IS AN ALLOW-LIST, NOT A DENY-LIST, and that is the deliberate half. A deny-list ("skip VolumeLock") has to be
# updated by whoever adds the next private feature, and will be forgotten exactly once; an allow-list ships what is
# named here and nothing else, so a feature that is not mentioned is private BY DEFAULT. The cost is that adding a
# feature to the release means editing this list -- which is a one-line change that FAILS LOUDLY (the package simply
# will not contain it, and the read-back below lists what shipped).
#
# ⚠️ AND IT IS CHECKED AFTER IT IS BUILT (see the read-back), not trusted -- the same way the settings-file rule is.
PUBLIC_FEATURES="AutoIME KeepAwake MediaControl SmoothWheel"
# ⚠️ THE PRIVATE ONES ARE STILL BUILT AND STILL DEPLOYED to the author's own copy -- they are simply not shipped.
# Build-time exclusion would be a second, invisible difference between "what I run" and "what I test"; this is one
# decision in one place, at the moment of packaging.
PRIVATE_FEATURES="VolumeLock"

# ⚠️ AND THE PACKAGE IS CHECKED AFTER IT IS BUILT, not trusted: the entry list is read back out of the zip and any
# settings/log/profile file in it fails the script. A packaging rule that is only a comment is a rule that rots.
#
# ⚠️⚠️ THE ZIP TOOL IS WINDOWS' OWN `bsdtar`, NAMED IN FULL, AND THAT IS NOT PEDANTRY. The `tar` on a Git Bash PATH
# is GNU tar: it cannot read a zip (it says "Failed to open archive") and it cannot write one. Using it made this
# script condemn a good package, and then -- after PowerShell's Compress-Archive failed silently behind a
# `>/dev/null` -- made it report nothing at all. bsdtar both writes and reads zip, so one tool does both halves.
#
# usage: apex/package.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
SRC="$ROOT/build/apex"
TAR=/c/Windows/System32/tar.exe

if [ ! -x "$TAR" ]; then
  echo "FAIL: $TAR is missing, so the package could be neither built nor checked"
  exit 1
fi

VERSION=$(sed -n 's/.*APEX_HOST_VERSION "\([^"]*\)".*/\1/p' "$ROOT/apex/host.h" | head -1)
if [ -z "$VERSION" ]; then
  echo "FAIL: cannot read APEX_HOST_VERSION out of apex/host.h"
  exit 1
fi
OUT="$ROOT/build/Apex-$VERSION-portable.zip"

if [ ! -f "$SRC/apex.exe" ] || [ ! -f "$SRC/apex-settings.exe" ]; then
  echo "FAIL: $SRC is not built yet -- run apex/build.sh first"
  exit 1
fi

STAGE=$(mktemp -d) || exit 1
trap 'rm -rf "$STAGE"' EXIT
DEST="$STAGE/Apex-$VERSION"
mkdir -p "$DEST/Plugins"

echo "== staging Apex $VERSION =="
cp "$SRC/apex.exe" "$SRC/apex-settings.exe" "$SRC/WebView2Loader.dll" "$DEST/" || exit 1
echo "   apex.exe  apex-settings.exe  WebView2Loader.dll"
for d in "$SRC"/Plugins/*/; do
  [ -d "$d" ] || continue
  id=$(basename "$d")
  # ⚠️ A FEATURE SHIPS ONLY IF `PUBLIC_FEATURES` NAMES IT -- see the note at the top. This is the line that keeps a
  # private feature out of a release, so a read-only `git status` cannot.
  case " $PUBLIC_FEATURES " in
    *" $id "*) ;;
    *)
      echo "   (NOT SHIPPING Plugins/$id -- it is not in PUBLIC_FEATURES, so it is private by default)"
      continue
      ;;
  esac
  # A feature is `<id>.dll` in its own folder; anything else in there (its .ini, its config.json, its debug log)
  # belongs to whoever ran the program and stays behind.
  if [ ! -f "$d$id.dll" ]; then
    echo "   (skipping Plugins/$id -- no $id.dll)"
    continue
  fi
  mkdir -p "$DEST/Plugins/$id"
  cp "$d$id.dll" "$DEST/Plugins/$id/" || exit 1
  echo "   Plugins/$id/$id.dll"
done

rm -f "$OUT"
"$TAR" -a -c -f "$(cygpath -w "$OUT")" -C "$(cygpath -w "$STAGE")" "Apex-$VERSION" || {
  echo "FAIL: could not write $OUT"
  exit 1
}

echo
echo "== reading the package back =="
entries=$("$TAR" -tf "$(cygpath -w "$OUT")" 2>/dev/null) || {
  echo "FAIL: the zip could not be read back"
  exit 1
}
echo "$entries" | sed 's/^/   /'
if echo "$entries" | grep -qiE '\.(ini|json|log)$'; then
  echo
  echo "FAIL: the package carries a settings or log file -- it must ship the PROGRAM, not anybody's configuration:"
  echo "$entries" | grep -iE '\.(ini|json|log)$' | sed 's/^/   /'
  exit 1
fi

# ⚠️⚠️ AND THE PRIVATE FEATURES ARE CHECKED FOR BY NAME, IN THE FINISHED ZIP. The allow-list above is the mechanism;
# this is the proof, and the two are not the same thing -- a loop that filters wrongly (a typo in `PUBLIC_FEATURES`, a
# path that does not match the `case`) still produces a zip, and only reading the zip back can tell you what is in it.
# ⚠️ THIS IS THE CHECK THAT WOULD HAVE CAUGHT v1.5.0: it shipped VolumeLock.dll while every repository-level guard
# said nothing, because none of them look at a zip.
for id in $PRIVATE_FEATURES; do
  if echo "$entries" | grep -qi "Plugins/$id/"; then
    echo
    echo "FAIL: the package contains the PRIVATE feature '$id' -- it must not be published:"
    echo "$entries" | grep -i "Plugins/$id/" | sed 's/^/   /'
    echo "   (see features/$id/NOT_FOR_RELEASE.md; the allow-list is PUBLIC_FEATURES in this script)"
    exit 1
  fi
done
# ... and the other way round, because an allow-list that matches nothing ships an empty program: every name in
# PUBLIC_FEATURES that is BUILT must actually be in the zip.
for id in $PUBLIC_FEATURES; do
  [ -f "$SRC/Plugins/$id/$id.dll" ] || continue
  if ! echo "$entries" | grep -qi "Plugins/$id/$id.dll"; then
    echo
    echo "FAIL: '$id' is named in PUBLIC_FEATURES and is built, but is NOT in the package:"
    echo "   the allow-list pattern does not match what the staging loop produced"
    exit 1
  fi
done

size=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
hash=$(sha256sum "$OUT" | cut -c1-16)
echo
echo "OK: $OUT"
echo "    $((size / 1024)) KB   sha256 $hash..."
