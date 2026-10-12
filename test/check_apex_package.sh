#!/usr/bin/env bash
# Gate: THE PORTABLE PACKAGE SHIPS THE PUBLIC PROGRAM AND NOTHING ELSE.
#
# ⚠️⚠️ THIS GATE EXISTS BECAUSE v1.5.0 WAS ABOUT TO PUBLISH A PRIVATE FEATURE.
#
# `VolumeLock` is private: it does not go into the repository and is not open source
# (features/VolumeLock/NOT_FOR_RELEASE.md, the user's own words of 2026-10-08). THREE things already said so --
# the feature's file header, that document, and `.gitignore` -- and **all three only cover `git`**. `apex/package.sh`
# never consults `git`: it zips `build/apex/Plugins/*/`, which is the folder the feature is BUILT into. On 2026-10-12
# the v1.5.0 package contained VolumeLock.dll and every existing guard stayed silent; v1.4.0's package was clean only
# by luck (that DLL happened not to be built at the time).
#
# ⚠️ SO THE LESSON IS NOT "ADD VOLUMELOCK TO A LIST". It is that a rule about the REPOSITORY does not protect the
# artifact users DOWNLOAD, and the two need separate proofs. The script now carries an allow-list and a read-back
# check; this gate is the third party that runs the real thing and reads the real zip.
#
# ⚠️ IT RUNS THE REAL SCRIPT, NOT A COPY OF ITS LOGIC. Asserting "the script contains PUBLICAL_FEATURES" would be a
# gate about text; what matters is what ends up in the file users download. So it packages for real and lists the zip.
#
# usage: test/check_apex_package.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"

TAR=/c/Windows/System32/tar.exe
SRC="$ROOT/build/apex"
fail=0

# The private features, named here as well as in the script -- deliberately duplicated, because this gate must be able
# to disagree with the script. If both read one variable, a change to that variable would move both halves together and
# the gate would agree with any mistake.
PRIVATE="VolumeLock"
PUBLIC="AutoIME KeepAwake MediaControl SmoothWheel"

echo "== 1. the package can be built, and says what it did =="
if [ ! -f "$SRC/apex.exe" ]; then
  echo "   SKIP: $SRC is not built (run apex/build.sh first)"
  echo
  echo "OK: nothing to check"
  exit 0
fi

out=$(bash "$ROOT/apex/package.sh" 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
  echo "   FAIL: apex/package.sh exited $rc"
  printf '%s\n' "$out" | tail -5 | sed 's/^/        /'
  fail=1
else
  echo "   ok  apex/package.sh succeeded"
fi

# ⚠️ THE ZIP IS FOUND THE WAY THE USER WOULD FIND IT -- by the name the version produces -- rather than by trusting a
# path echoed in the output. If the two disagree, that is itself the bug.
VERSION=$(sed -n 's/.*APEX_HOST_VERSION "\([^"]*\)".*/\1/p' "$ROOT/apex/host.h" | head -1)
ZIP="$ROOT/build/Apex-$VERSION-portable.zip"
if [ ! -f "$ZIP" ]; then
  echo "   FAIL: no package at build/Apex-$VERSION-portable.zip"
  exit 1
fi
echo "   ok  $ZIP"

echo
echo "== 2. what is actually inside it =="
entries=$("$TAR" -tf "$(cygpath -w "$ZIP")" 2>/dev/null)
if [ -z "$entries" ]; then
  echo "   FAIL: the zip could not be listed (or is empty) -- a package nobody can read is not a package"
  exit 1
fi
printf '%s\n' "$entries" | sed 's/^/   /'

echo
echo "== 3. NO private feature is in it =="
# ⚠️ THIS IS THE ASSERTION THE RELEASE NEEDS. It is deliberately independent of the script's own read-back: that check
# protects the author's next packaging run, this one protects the artifact.
for id in $PRIVATE; do
  if printf '%s\n' "$entries" | grep -qi "Plugins/$id/"; then
    echo "   FAIL: the package contains the PRIVATE feature '$id' and must not be published"
    fail=1
  else
    echo "   ok  '$id' is absent"
  fi
done

echo
echo "== 4. every PUBLIC feature that is built IS in it =="
# The other half of an allow-list: one that matches nothing ships an empty program and every "is X absent" check
# above still passes.
for id in $PUBLIC; do
  [ -f "$SRC/Plugins/$id/$id.dll" ] || continue
  if printf '%s\n' "$entries" | grep -qi "Plugins/$id/$id.dll"; then
    echo "   ok  '$id' shipped"
  else
    echo "   FAIL: '$id' is built and public but is MISSING from the package"
    fail=1
  fi
done

echo
echo "== 5. it carries the program and no configuration =="
# ⚠️⚠️ `case` RATHER THAN `grep`, AND THAT IS A FINDING RATHER THAN A STYLE CHOICE.
#
# The first version of this check piped `$entries` into `grep -qiF "/$f"` and reported all three files MISSING while
# the very same listing -- printed a few lines above -- plainly contained them. Inside this script `grep` returned 1
# for a pattern that `case` matched, on the same variable, in the same shell, with flags that work in a two-line
# script. I did not find the cause (the data was byte-identical, `GREP_OPTIONS` was unset, and `grep` on this PATH
# matched when tested alone), and I stopped looking once the honest conclusion was clear:
#
#   **A CHECK WHOSE FAILURE I CANNOT EXPLAIN IS NOT A CHECK.** It would report a good package as unpublishable, and the
#   next person would learn to ignore it -- which is worse than not having it.
#
# `case` does the same job with no pipeline, no subprocess and no flags, it is what steps 3 and 4 above already use,
# and it demonstrably agrees with the listing.
#
# ⚠️ AND IT IS MATCHED AGAINST THE WHOLE `$entries` STRING, which contains every path -- so `*/apex.exe` cannot be
# satisfied by `apex-settings.exe`: the `/` before the name and the `.exe` after it are both part of the pattern.
for f in apex.exe apex-settings.exe WebView2Loader.dll; do
  case "$entries" in
    *"/$f"*)
      echo "   ok  $f"
      ;;
    *)
      echo "   FAIL: $f is not in the package"
      fail=1
      ;;
  esac
done
# ⚠️ AND THE MATCHER IS PROVED, NOT ASSUMED -- this project's rule for any gate that judges mechanically: "a step that
# makes a decision must have a way to be shown doing its job". These two cases are the fixture: one that must match and
# one that must not, so a matcher that silently stopped working cannot leave the checks above passing by accident.
case "$entries" in
  *"/apex-settings.exe"*) echo "   ok  the matcher finds a longer name (self-test: must match)" ;;
  *) echo "   FAIL: the matcher cannot find apex-settings.exe, so the checks above prove nothing"; fail=1 ;;
esac
case "$entries" in
  *"/not-a-real-file.exe"*) echo "   FAIL: the matcher matches a name that is not in the package"; fail=1 ;;
  *) echo "   ok  the matcher rejects a name that is absent (self-test: must not match)" ;;
esac
if printf '%s\n' "$entries" | grep -qiE '\.(ini|json|log)$'; then
  echo "   FAIL: the package carries a settings/log file -- it must ship the program, not a configuration"
  printf '%s\n' "$entries" | grep -iE '\.(ini|json|log)$' | sed 's/^/        /'
  fail=1
else
  echo "   ok  no .ini / .json / .log in it"
fi

echo
if [ $fail -eq 0 ]; then
  echo "OK: the package ships the public program, exactly once, configured by nobody"
else
  echo "FAILED: the package is not fit to publish"
fi
exit $fail
