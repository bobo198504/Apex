#!/usr/bin/env bash
# Gate: THE AUTO-IME FEATURE'S MATCHING ENGINE -- its rules, its ordering, and its anchoring.
#
# ⚠️ WHY THIS IS A GATE AND NOT JUST `cargo test`. This feature is the only one whose behaviour cannot be seen
# by looking at the program: when a rule stops matching, nothing errors -- the input method simply does not
# switch in one program, and the report that reaches a human is "the IME behaved oddly in Explorer". The tests
# pin the semantics against the USER'S OWN RULES (their deployed config.json is a fixture in the crate), so a
# change that quietly alters what a pattern means fails here rather than in their typing.
#
# It runs `cargo test` in the feature's crate. That needs the Rust toolchain, which lives outside Apex's
# MinGW-only build -- so its absence is an ERROR rather than a skip: a missing toolchain must say so, because
# the alternative is a gate that passes on the machine that has the toolchain and silently proves nothing on
# the one that does not.
#
# usage: test/check_feature_ime.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
FEAT="$ROOT/features/AutoIME"

if [ ! -f "$FEAT/Cargo.toml" ]; then
  echo "the AutoIME feature is not in the tree: $FEAT/Cargo.toml"
  exit 1
fi

# Cargo is a user-scoped tool (rustup installs it under the profile). The build script does the same lookup;
# see how Apex/build.sh handles a Rust feature.
if ! command -v cargo >/dev/null 2>&1 && [ ! -x "$HOME/.cargo/bin/cargo.exe" ]; then
  echo "FAIL: cargo is not available, so the AutoIME feature cannot be tested (or built)."
  echo "      Install Rust, or remove features/AutoIME -- but do not let a gate pass by proving nothing."
  exit 1
fi

# ⚠️⚠️ w64devkit MUST BE OFF PATH FOR CARGO, AND run_all.sh PUTS IT ON (line 21, for the C++ builds).
# Inheriting it here made this gate fail inside the suite while passing on its own -- the worst kind of
# difference, because the failure looks like a flaky test rather than an environment collision. Rust's
# windows-gnu target uses its own linker driver with its own runtime, and w64devkit's copy has no libgcc_eh:
# a clean build then dies with `cannot find -lgcc_eh`, naming a library instead of the cause. Same fix, same
# reasoning and the same measurement as apex/build.sh, which is where the long version of this note lives.
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
export PATH="$HOME/.cargo/bin:$CLEAN_PATH"

echo "== the AutoIME matching engine =="
cd "$FEAT" || exit 1
out=$(cargo test --release 2>&1)
rc=$?
printf '%s\n' "$out" | grep -E '^test tests::' | sed 's/^test /   /'
printf '%s\n' "$out" | grep -E '^test result:' | sed 's/^/   /'

if [ $rc -ne 0 ]; then
  echo
  echo "   FAIL: the tests above did not all pass"
  printf '%s\n' "$out" | grep -A 6 "panicked at" | head -20 | sed 's/^/        /'
  exit 1
fi

# THE FIXTURE IS THE POINT: it proves the tests run against the USER'S rules and not a convenience copy
# someone edited to make them pass.
#
# ⚠️ IT ONLY EVER LOOKS INSIDE THIS PROJECT AND THE APEX DEPLOY FOLDER, and that is a rule rather than
# tidiness: `D:\App protable\` holds 36 of the user's applications and the agent's permission stops at
# `D:\App protable\Apex`. (An earlier version of this gate read the OLD standalone AutoIME's config out of
# `D:\App protable\AutoIME-portable\` as its ground truth. Reading was harmless, but "harmless" is how the
# wider mistake started -- see §零之二·五, where a loose path filter killed the user's programs.) The old
# program is on the way out anyway; once it is gone that path would be a gate that quietly proves less.
FIXTURE="$FEAT/tests/fixtures/user_config.json"
DEPLOYED="/d/App protable/Apex/Plugins/AutoIME/config.json"
if [ -f "$DEPLOYED" ]; then
  if diff -q "$DEPLOYED" "$FIXTURE" >/dev/null 2>&1; then
    echo "   the fixture is byte-identical to the config.json in the deployed Apex"
  else
    echo "   OK (the fixture and the deployed config.json differ -- expected once the rules are edited in the"
    echo "       panel; re-copy it to test against the current ones:"
    echo "         cp \"$DEPLOYED\" \"$FIXTURE\")"
  fi
else
  echo "   (nothing deployed yet at $DEPLOYED -- the fixture is the only copy, which is fine)"
fi

# AND THE FEATURE MUST ACTUALLY BE THE ARTEFACT THE HOST LOADS. A crate whose tests pass but which produces no
# DLL is a feature that is not there -- and `cargo test` does NOT build the cdylib, only the test binary, so
# this has to ask for it. (Checked by BUILDING rather than by looking for a file: an earlier version of this
# gate looked, found nothing after a `cargo clean`, and reported "the tests pass but nothing would load" --
# which was true and useless, because the gate is what should have built it.)
BUILD_OUT=$(cargo build --release 2>&1)
if [ $? -ne 0 ]; then
  echo "   FAIL: the feature does not build as a DLL"
  printf '%s\n' "$BUILD_OUT" | grep -E "^error" -A 4 | head -12 | sed 's/^/        /'
  exit 1
fi
DLL="$FEAT/target/release/AutoIME.dll"
if [ -f "$DLL" ]; then
  echo "   and it builds the DLL the host loads: $(wc -c < "$DLL") bytes"
else
  echo "   FAIL: cargo succeeded but produced no AutoIME.dll -- check [lib] name in Cargo.toml"
  exit 1
fi
echo
echo "OK: the rules match where they should, the order is priority-then-name, and the patterns are anchored"
