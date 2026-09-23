#!/usr/bin/env bash
# Gate: A DEPLOY REPLACES THE PROGRAM AND NOTHING ELSE.
#
# ⚠️ THIS GATE EXISTS BECAUSE apex/deploy.sh DELETED THE USER'S DATA ON ITS FIRST REAL RUN.
#
# The step that copies Plugins/ began with `rm -rf "$TARGET/Plugins"` -- correct about a retired feature (one
# removed from the tree must disappear from the deployed folder, or a merged copy keeps running while the source
# says it is gone) and catastrophic about everything else living in there. It deleted:
#
#   * Plugins/AutoIME/config.json -- the user's nine hand-written IME rules,
#   * Plugins/SmoothWheel/SmoothWheel.ini -- their SmoothWheel settings,
#   * and anything else a feature keeps beside itself.
#
# The script's own text two paragraphs below said "config.json belongs to the USER and is never written by a
# deploy". A general cleanup is not a safe way to remove a specific thing, and a promise in a comment is not a
# check: it was the comment that the `rm -rf` silently contradicted.
#
# So this gate RUNS THE DEPLOY INTO A THROWAWAY TARGET and looks at what survived. It needs no real deploy, no
# process, and no user data beyond a file it creates itself -- so it belongs in the build layer, where the
# mistake would have been caught before it touched anything.
#
# usage: test/check_apex_deploy.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # the field-taking helpers the script under test uses

# ⚠️ IT MUST NOT TOUCH THE USER'S INSTALLATION. The script under test defaults to D:\App protable\Apex; this
# gate passes a target of its own under build/, which is the only reason it is safe to run at any time.
SCRATCH="$ROOT/build/_deploy_gate"
TARGET="$SCRATCH/Apex"
FOREIGN="$SCRATCH/another-apex"

fail=0
echo "== a throwaway target that looks like a real installation =="
rm -rf "$SCRATCH"
mkdir -p "$TARGET/Plugins/AutoIME" "$TARGET/Plugins/SmoothWheel" "$TARGET/Plugins/RetiredFeature"
mkdir -p "$FOREIGN"

# What a real deployed folder carries, in the two categories this gate is about.
printf '{"ime_toggle_hotkey":"Ctrl+Space","rules":[{"id":"mine","name":"my rule"}]}\n' > "$TARGET/Plugins/AutoIME/config.json"
printf '# the user dialled these in by hand\nslow=9.5\n' > "$TARGET/Plugins/SmoothWheel/SmoothWheel.ini"
printf '{"observed":"a window I pointed at"}\n' > "$TARGET/Plugins/AutoIME/observed.json"
printf 'theme=dark\nlang=zh\n' > "$TARGET/apex.ini"
printf 'old dll\n' > "$TARGET/Plugins/RetiredFeature/RetiredFeature.dll"
# And a marker that must survive untouched just outside the folders being replaced.
printf '# not the program\n' > "$TARGET/NOTES.txt"
echo "   $TARGET"

# A checksum of everything that belongs to the user, taken before the deploy.
before=$(cd "$TARGET" && md5sum Plugins/AutoIME/config.json Plugins/SmoothWheel/SmoothWheel.ini \
                               Plugins/AutoIME/observed.json apex.ini NOTES.txt 2>/dev/null | sort)

echo
echo "== running the real deploy script against it =="
# ⚠️ THE BUILD IS RUN BY THE SCRIPT ITSELF (it does an APEX_RELEASE=1 build first), which is what makes this a
# test of the script as written rather than of a copy of its logic. It is a no-op when the tree is already
# built, which is the case inside a suite run.
# ⚠️⚠️ FILES_ONLY, AND IT IS NOT OPTIONAL. This gate checks what the deploy does to FILES -- what is copied,
# what is replaced, what is left alone -- and those are facts about a directory, needing no running program.
#
# Without the switch it ran the WHOLE script, which takes the field (stopping the user's live Apex) and then
# starts its OWN copy from the throwaway folder. That is not a hypothetical: the first version of this gate did
# exactly that, and the user's deployment went down because a gate whose whole premise is "no window, no
# process, no input device" quietly ran the real thing. A build-layer gate that starts a program is a gate that
# violates the layer it lives in.
out=$(APEX_DEPLOY_FILES_ONLY=1 bash "$ROOT/apex/deploy.sh" "$TARGET" 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
  echo "   FAIL: the deploy script returned $rc"
  printf '%s\n' "$out" | tail -8 | sed 's/^/        /'
  rm -rf "$SCRATCH"
  exit 1
fi
printf '%s\n' "$out" | grep -E "^   " | head -12 | sed 's/^/   /'
# And the switch must actually be honoured -- if the script ever ignored it, this gate would be starting
# processes again without saying so.
if ! printf '%s' "$out" | grep -q "SKIPPED (APEX_DEPLOY_FILES_ONLY=1"; then
  echo "   FAIL: the deploy ignored APEX_DEPLOY_FILES_ONLY -- this gate is starting real processes"
  fail=1
fi

echo
echo "== 1. the user's data is all still there, unchanged =="
after=$(cd "$TARGET" && md5sum Plugins/AutoIME/config.json Plugins/SmoothWheel/SmoothWheel.ini \
                            Plugins/AutoIME/observed.json apex.ini NOTES.txt 2>/dev/null | sort)
if [ "$before" = "$after" ]; then
  echo "   ok  every user file survived byte-for-byte"
  printf '%s\n' "$after" | sed 's/^/        /'
else
  echo "   FAIL: the deploy changed or deleted something that belongs to the user"
  echo "        before:                    after:"
  diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | sed 's/^/        /'
  fail=1
fi

echo
echo "== 2. the program itself was replaced =="
if [ -f "$TARGET/apex.exe" ] && [ -f "$TARGET/apex-settings.exe" ]; then
  # Not just present: the same bytes as the build output, or the "replace" half did nothing.
  if cmp -s "$TARGET/apex.exe" "$ROOT/build/apex/apex.exe"; then
    echo "   ok  apex.exe is the freshly built one"
  else
    echo "   FAIL: apex.exe in the target is not the built one"
    fail=1
  fi
  if [ -f "$TARGET/Plugins/AutoIME/AutoIME.dll" ] && cmp -s "$TARGET/Plugins/AutoIME/AutoIME.dll" "$ROOT/build/apex/Plugins/AutoIME/AutoIME.dll"; then
    echo "   ok  and the feature DLLs are too"
  else
    echo "   FAIL: the feature DLLs were not copied"
    fail=1
  fi
else
  echo "   FAIL: the deployed folder has no program in it"
  fail=1
fi

echo
echo "== 3. a feature this release no longer builds is REMOVED =="
if [ -d "$TARGET/Plugins/RetiredFeature" ]; then
  echo "   FAIL: Plugins/RetiredFeature survived -- a retired feature keeps running on the user's machine"
  fail=1
else
  echo "   ok  Plugins/RetiredFeature is gone (the release does not build it)"
fi
# ⚠️ AND THE OTHER HALF, WHICH IS THE ONE THAT WAS BROKEN: only THAT folder went. If the fix had been to stop
# removing anything at all, the check above would still pass and this one would fail.
if [ -f "$TARGET/Plugins/AutoIME/config.json" ] && [ -f "$TARGET/Plugins/SmoothWheel/SmoothWheel.ini" ]; then
  echo "   ok  and the folders that ARE built kept their user files"
else
  echo "   FAIL: a folder that is still built lost its user files -- the removal is too broad"
  fail=1
fi

echo
echo "== 4. it refuses a target that is not an Apex folder =="
# The guard that keeps a mistyped path from scattering the program across a directory of the user's tools --
# the same class of mistake that killed their portable apps once (AGENTS.md, the four gate rules).
mkdir -p "$FOREIGN/SomeOtherTool"
printf 'not apex\n' > "$FOREIGN/SomeOtherTool/tool.exe"
refuse_out=$(bash "$ROOT/apex/deploy.sh" "$FOREIGN" 2>&1)
if printf '%s' "$refuse_out" | grep -q "REFUSING"; then
  echo "   ok  a non-Apex folder is refused before anything is written"
else
  echo "   FAIL: the deploy accepted a directory that is not an Apex installation"
  printf '%s\n' "$refuse_out" | head -3 | sed 's/^/        /'
  fail=1
fi

rm -rf "$SCRATCH"
echo
if [ $fail -eq 0 ]; then
  echo "OK: a deploy replaces the program, removes retired features, and leaves the user's data alone"
else
  echo "FAILED: the deploy does not keep its promise"
fi
exit $fail
