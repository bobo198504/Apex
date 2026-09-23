#!/usr/bin/env bash
# Gate: THE BOUNDARIES -- is "a feature is a folder" true, or does it only look true?
#
# The user asked for this one in as many words: "确定所有功能都符合「模块化」「接口化」的开发，确保后面的功能
# 迭代和修改做到最小的修改". That is a claim about the SHAPE of the code, and a claim about shape is exactly
# what rots quietly -- every individual change looks reasonable, and the coupling is only visible from above.
#
# So this gate tests the properties that make the claim true, each one a way it could stop being true:
#
#   1. THE HOST DOES NOT KNOW ANY FEATURE BY NAME. Not its id, not its parameters, not its settings keys --
#      checked with comments stripped, because a mention in a comment is fine and a mention in code is not.
#   2. THE PANEL DOES NOT KNOW ANY FEATURE BY NAME EITHER. The page is driven by the features' own documents;
#      a hard-coded id there is how a second feature's page silently draws wrong.
#   3. A FEATURE CANNOT REACH INTO THE HOST. The host's private headers refuse to compile without
#      APEX_BUILDING_HOST, which the build defines for the host's sources and NOT for a feature's. Proven by
#      actually compiling a violating feature and requiring it to FAIL -- a guard that never fires is a
#      comment.
#   4. THE VERSIONED CONTRACT IS CHECKED. The loader must compare the ABI version and the struct size before
#      reading anything, because a stale DLL is otherwise an out-of-bounds read.
#
# ⚠️ WHAT THIS GATE CANNOT DO: prove that adding a feature is EASY, only that nothing in the way is hard-coded.
# The companion evidence is features/AuditStub, a throwaway second feature kept in the tree for exactly this:
# it shares nothing with SmoothWheel (different ids, a bool, no curve, Ctrl+wheel instead of plain), and the
# fact that it builds and loads untouched is the strongest statement available here.
#
# usage: test/check_apex_modular.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
fail=0

# ---------------------------------------------------------------------------
echo "== 1. does the HOST know any feature by name? =="
# Comments are stripped with the preprocessor rather than by a regex, so a name in a comment cannot make this
# pass or fail by accident. (-fpreprocessed -dD -E keeps comments out and line structure in.)
#
# ⚠️ ONE PREPROCESSOR RUN FOR THE WHOLE SET, NOT ONE PER FILE. Spawning `g++ -E` seventeen times costs 1.1 s
# of this gate's wall clock (measured) and produces the same text; one run over all of them costs 0.46 s. The
# per-file attribution is kept by printing the file name as a marker between files -- which is what the old
# per-file loop was really buying, and it is available for free.
HOSTDIRS=("$ROOT/apex")
found=0
host_sources=()
for f in "${HOSTDIRS[@]}"/*.cpp "${HOSTDIRS[@]}"/*.h; do
  [ -f "$f" ] || continue
  case "${f##*/}" in
    abi.h|icons.h) continue ;; # abi.h IS the contract; icons.h documents the example ids it ships with
  esac
  host_sources+=("$f")
done
# `#line` directives make the preprocessor name the file it is reading, so a hit can still be attributed.
host_clean=$(g++ -fpreprocessed -dD -E -x c++ "${host_sources[@]}" 2>/dev/null)
hits=$(printf '%s\n' "$host_clean" |
  awk '/^# [0-9]+ "/ { f = $3; gsub(/"/, "", f); sub(/.*[\/\\]/, "", f); next }
       /SmoothWheel|AuditStub|glideMs|slowStep|rampUp|topSpeed|SmoothWheel\.ini/ {
         if (!(f in said)) { said[f] = 1; printf "%s: ", f }
         print; n++
         if (n > 2) exit
       }')
if [ -n "$hits" ]; then
  echo "   FAIL: a host source mentions a feature or its parameters in CODE:"
  printf '%s\n' "$hits" | head -3 | sed 's/^/        /'
  found=1
fi
if [ $found -eq 0 ]; then
  echo "   ok  no host source names a feature, a parameter, or a settings key"
else
  fail=1
fi

# ---------------------------------------------------------------------------
echo
echo "== 2. does the PANEL know any feature by name? =="
# The page is JS inside HTML, so comments are stripped as // and /* */ -- and the check is for the literal
# identifiers a hard-coded feature would need.
UI="$ROOT/build/panel.built.html"
ui_hits=$(sed 's|//.*||; s|/\*.*\*/||' "$UI" \
  | grep -n "SmoothWheel\|glide\|slowStep\|rampUp\|topSpeed" | head -5)
if [ -n "$ui_hits" ]; then
  echo "   FAIL: the panel names a feature or its parameters:"
  printf '%s\n' "$ui_hits" | sed 's/^/        /'
  fail=1
else
  echo "   ok  the page is driven entirely by the documents features send it"
fi
# ... and the mechanism that replaced the hard-coded segment list must be present on BOTH sides.
# (`seg` is the field that replaced the hard-coded list of one feature's parameter names -- without it the
# panel has a number on the curve and a colour on the slider and no way to join them. Backticks are avoided
# in these strings: inside double quotes the shell runs them as commands, which is what this line did first.)
# ⚠️ MATCHED WITH ITS ESCAPES, because the feature builds JSON by hand: the field is written as
# `\"seg\":%d` inside a C string, so a search for the plain `"seg":` finds nothing and reports a missing field
# that is right there. (The gate caught itself doing exactly that, which is the useful kind of failure.)
SEG_COUNT=$(grep -c '\\"seg\\":' "$ROOT/features/SmoothWheel/feature_smoothwheel.cpp" || true)
if [ "${SEG_COUNT:-0}" -lt 1 ]; then
  echo "   FAIL: the feature does not send seg, so the panel cannot colour the chart without guessing"
  fail=1
else
  echo "   ok  and each range declares the chart segment it colours (the panel reads, it does not guess)"
fi

# ---------------------------------------------------------------------------
echo
echo "== 3. can a feature reach into the host? (the guard must FIRE) =="
PROBE_SRC="$ROOT/build/_modular_probe.cpp"
mkdir -p "$ROOT/build"
cat > "$PROBE_SRC" <<'EOF'
#include "host.h"   // must be refused: this is the host's own header
int main(void) { return 0; }
EOF
out=$(g++ -std=c++17 -fsyntax-only -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/shared" "$PROBE_SRC" 2>&1)
if printf '%s' "$out" | grep -q "is the HOST's"; then
  echo "   ok  a feature including host.h is refused by the compiler"
else
  echo "   FAIL: a feature CAN include the host's private headers -- the boundary is only a convention"
  printf '%s\n' "$out" | head -3 | sed 's/^/        /'
  fail=1
fi
# ... and the same file MUST compile once it says it is the host, or the guard would block the host too.
cat > "$PROBE_SRC" <<'EOF'
#define APEX_BUILDING_HOST
#include "host.h"
int main(void) { return 0; }
EOF
if g++ -std=c++17 -fsyntax-only -I"$ROOT/apex" -I"$ROOT/common" -I"$ROOT/shared" "$PROBE_SRC" 2>/dev/null; then
  echo "   ok  and the host itself still compiles with it"
else
  echo "   FAIL: the host cannot include its own headers"
  fail=1
fi
rm -f "$PROBE_SRC"

# ... and every host-private header must actually carry the guard.
echo
echo "   -- every private header carries it --"
for h in host.h loader.h paths.h settings_ipc.h hostconfig.h decision.h icons.h; do
  if grep -q "APEX_BUILDING_HOST" "$ROOT/apex/$h"; then
    printf '      %-18s guarded\n' "$h"
  else
    printf '      %-18s NOT GUARDED\n' "$h"
    fail=1
  fi
done

# ---------------------------------------------------------------------------
echo
echo "== 4. is the ABI checked before anything is read? =="
if grep -q "abiVersion != APEX_ABI_VERSION" "$ROOT/apex/loader_win.cpp" &&
   grep -q "structSize" "$ROOT/apex/loader_win.cpp"; then
  echo "   ok  the loader refuses an old DLL before touching its fields"
else
  echo "   FAIL: the loader does not check the version and size first"
  fail=1
fi

# ---------------------------------------------------------------------------
echo
echo "== 5. the second feature is still in the tree (the live evidence) =="
if [ -f "$ROOT/features/AuditStub/feature_audit_stub.cpp" ]; then
  echo "   ok  features/AuditStub exists -- a second feature that shares nothing with the first"
  echo "      (it builds with the normal script and the host loads it; that is what proves the seam)"
else
  echo "   FAIL: the second feature was deleted, so nothing here is exercised against a foreign feature"
  fail=1
fi

# ---------------------------------------------------------------------------
echo
echo "== 6. the ABI is described TWICE, and the two must agree =="
#
# ⚠️ THIS IS THE ONE PLACE THE COMPILER CANNOT HELP. `apex/abi.h` is C; `features/AutoIME/src/abi.rs` is the
# same structs written out again in Rust. Neither compiler sees the other's copy, and a mismatch is not a
# subtle bug -- the host REFUSES the DLL, so the feature simply does not load:
#
#     feature: FAILED -- ABI 6, this host speaks 7 -- rebuild the feature
#
# which is correct and safe, and also exactly what happened when the version was bumped on the C side and not
# on the Rust side. The Rust DLL exports a self-check (`ApexFeatureSizes`) for this; it is compared here against
# what the C compiler computes from the real header.
DLL="$ROOT/build/apex/Plugins/AutoIME/AutoIME.dll"
if [ ! -f "$DLL" ]; then
  echo "   (the AutoIME feature is not built -- nothing to compare; run apex/build.sh)"
else
  cat > "$ROOT/build/_abi_size.cpp" <<'EOF'
#include "abi.h"
#include <stdio.h>
int main(void)
{
  printf("%u %u %u\n", (unsigned)sizeof(ApexHost), (unsigned)sizeof(ApexFeature),
         (unsigned)APEX_ABI_VERSION);
  return 0;
}
EOF
  # (stderr is NOT discarded here, unlike the other probe builds in this project: a silent empty answer from
  # this block is indistinguishable from "the sizes disagree", and it was: the gate reported a mismatch while
  # the same two commands worked by hand.)
  C_SIZES=$(g++ -std=c++17 -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$ROOT/build/_abi_size.exe" \
            "$ROOT/build/_abi_size.cpp" && "$ROOT/build/_abi_size.exe")
  # The Rust side's own numbers, through the exported self-check.
  cat > "$ROOT/build/_abi_size2.cpp" <<'EOF'
#include <windows.h>
#include <stdio.h>
typedef void (*SizesFn)(unsigned *, unsigned *);
int main(int argc, char **argv)
{
  HMODULE h = LoadLibraryA(argv[1]);
  if (!h) { printf("load failed\n"); return 1; }
  SizesFn f = (SizesFn)GetProcAddress(h, "ApexFeatureSizes");
  if (!f) { printf("no ApexFeatureSizes export\n"); return 2; }
  unsigned hs = 0, fs = 0;
  f(&hs, &fs);
  printf("%u %u\n", hs, fs);
  return 0;
}
EOF
  R_SIZES=$(g++ -std=c++17 -o "$ROOT/build/_abi_size2.exe" "$ROOT/build/_abi_size2.cpp" -luser32 2>/dev/null &&
            "$ROOT/build/_abi_size2.exe" "$DLL")
  rm -f "$ROOT/build/_abi_size.cpp" "$ROOT/build/_abi_size.exe" \
        "$ROOT/build/_abi_size2.cpp" "$ROOT/build/_abi_size2.exe"

  c_host=$(echo "$C_SIZES" | cut -d' ' -f1)
  c_feat=$(echo "$C_SIZES" | cut -d' ' -f2)
  c_ver=$(echo "$C_SIZES" | cut -d' ' -f3)
  r_host=$(echo "$R_SIZES" | cut -d' ' -f1)
  r_feat=$(echo "$R_SIZES" | cut -d' ' -f2)

  if [ -z "$c_feat" ] || [ -z "$r_feat" ]; then
    echo "   FAIL: could not read the sizes from one side (C='$C_SIZES' Rust='$R_SIZES')"
    fail=1
  elif [ "$c_host" = "$r_host" ] && [ "$c_feat" = "$r_feat" ]; then
    echo "   ok  ApexHost $c_host both sides, ApexFeature $c_feat both sides (ABI $c_ver)"
  else
    echo "   FAIL: the two descriptions of the ABI disagree -- the host would refuse this DLL:"
    echo "        C    : ApexHost=$c_host ApexFeature=$c_feat"
    echo "        Rust : ApexHost=$r_host ApexFeature=$r_feat"
    echo "        (fix features/AutoIME/src/abi.rs to match apex/abi.h, and bump BOTH versions)"
    fail=1
  fi
fi

# ---------------------------------------------------------------------------
echo
echo "== 7. every gate that starts a process can also stop it =="
#
# ⚠️ THE SAME BUG HAS BEEN MADE THREE TIMES, SO IT IS A CHECK RATHER THAN A NOTE.
#
# A gate's cleanup may kill ONLY its own processes, identified by NAME *and* PATH:
#
#   * `taskkill /F /IM apex.exe` killed every apex.exe on the machine -- including the user's live copy.
#     Apex is a program the user runs; running the gates shot it down, over and over.
#   * `Get-Process | Where-Object { $_.Path -like '*...*' }` had NO name filter, so it returned every process
#     on the machine and the path fragment matched a whole folder of the user's portable tools. Their tray
#     applications went down with a test run. Nothing had granted that.
#   * and narrowing the filter then silently stopped killing things: check_apex_flash.sh started apex.exe AND
#     apex-settings.exe and killed only apex, so its panel survived each run and the NEXT run failed with
#     "the previous run's folder could not be removed" -- which reads as a disk problem, not a leftover process.
#
# The filter now lives in exactly ONE place (test/lib_procs.sh), because it was copied into nine scripts and a
# copied rule is a rule that gets fixed in one place and left broken in eight. So this section checks two
# things, both mechanical:
#
#   A. no script carries its own copy of the filter -- it must call the library;
#   B. it must be able to stop every executable it starts.
#
# It cannot judge whether a filter is narrow enough. "You started it and never kill it" and "you wrote the
# filter a tenth time" are not judgement calls.
LIB="$ROOT/test/lib_procs.sh"
if [ ! -f "$LIB" ]; then
  echo "   FAIL: $LIB is missing -- every gate's cleanup depends on it"
  fail=1
fi

# ---------------------------------------------------------------------------
# THE RULES, IN ONE PASS OVER EVERY SCRIPT.
#
# ⚠️ THEY LIVE IN test/proc_rules.awk, AND THEY LIVE THERE FOR A MEASURED REASON. The first version of this
# section was a bash loop that ran six to eight PROGRAMS per script (grep, awk, basename, and an `echo | grep`
# for every executable name it checked). A program spawn on this machine costs ~0.15 s -- measured: 29
# `basename` calls took 1.3 s -- so over twenty scripts the section spent ~11 seconds of wall clock doing
# arithmetic on text. The user asked for exactly this to stop: "缩减没必要的反复测试流程 ... 太浪费资源时间".
# The same rules as one awk program: 127 ms for every script in the tree.
#
# ⚠️ AND A REWRITE OF A WORKING CHECK IS WHERE A CHECK QUIETLY STOPS CHECKING. So the gate keeps its
# self-tests below, feeding the program fixtures whose answers are known, and reports the result as ok only
# when they all come back right. That is also what buys the two exemptions in the program (this file, and
# lib_procs.sh): an exemption plus a self-test is a decision, an exemption alone is a hole.
RULES="$ROOT/test/proc_rules.awk"
if [ ! -f "$RULES" ]; then
  echo "   FAIL: $RULES is missing -- the process rules cannot be checked"
  fail=1
else
  # -- the self-tests -------------------------------------------------------
  #
  # Three fixtures. Each is the smallest file that exercises one rule, and each has a known answer:
  #
  #   1. a real call site      -> OWNQUERY (and the file must be named in the report)
  #   2. the same words in a comment, and as a regex literal -> NO finding at all
  #   3. a launch with no way to stop it -> NOCOVER, with the executable named
  #
  # ⚠️ THE FIRST VERSION OF THE AWK PROGRAM LABELLED EVERY FINDING WITH THE NEXT FILE'S NAME (it read FILENAME
  # inside the per-file finish, which by then pointed at the file whose FIRST LINE had just arrived). It
  # reported check_apex_palette as executing a process query -- a file that does not, named after one that
  # does. Fixture 1 checks the name, so that mistake cannot come back unnoticed.
  FIXTURES=$(mktemp -d)
  cat > "$FIXTURES/zz_hit.sh" <<'FIXEOF'
HOSTPID=$(powershell -NoProfile -Command \
  "Get-Process apex -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '*_x*' }" \
  2>/dev/null | tr -d '\r' | head -1)
FIXEOF
  cat > "$FIXTURES/zz_miss.sh" <<'FIXEOF'
# This gate does NOT call Get-Process, and the pattern /Get-Process/ below is only a pattern.
ui_hits=$(grep -n Get-Process "$UI")
FIXEOF
  cat > "$FIXTURES/zz_cover.sh" <<'FIXEOF'
. "$ROOT/test/lib_procs.sh"
( cd "$RUN" && cmd //c start "" apex.exe >/dev/null 2>&1 )
taskkill //F //PID 1 >/dev/null 2>&1
FIXEOF

  rules_out=$(awk -f "$RULES" "$FIXTURES/zz_hit.sh" "$FIXTURES/zz_miss.sh" "$FIXTURES/zz_cover.sh" 2>&1)
  rm -rf "$FIXTURES"

  case "$rules_out" in
    *"OWNQUERY zz_hit.sh"*) echo "   ok  the rules see a real process query" ;;
    *) echo "   FAIL: the rules missed a real process query (got: $rules_out)"; fail=1 ;;
  esac
  case "$rules_out" in
    *"zz_miss.sh"*) echo "   FAIL: the rules fired on a comment or a regex literal (got: $rules_out)"; fail=1 ;;
    *) echo "   ok  the rules ignore a comment and a regex literal" ;;
  esac
  case "$rules_out" in
    *"NOCOVER zz_cover.sh apex"*) echo "   ok  the rules see a launch with nothing to stop it" ;;
    *) echo "   FAIL: the rules missed a launch with no cleanup (got: $rules_out)"; fail=1 ;;
  esac

  # -- and now the real tree -------------------------------------------------
  #
  # The program prints one line per file it has something to say about. Everything it reports is a failure
  # here: it only speaks when a rule is broken (or when a file passed, which is reported as ok below).
  findings=$(awk -f "$RULES" "$ROOT"/test/*.sh "$ROOT"/_diag/*.sh 2>&1)
  offenders=0
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    set -- $line
    case "$1" in
      COVEROK) printf '   ok  %-32s starts and can stop the same set\n' "$2" ;;
      OK)      : ;;
      OWNQUERY)
        echo "   FAIL: $2 runs its own process query"
        echo "        (that filter lives in test/lib_procs.sh -- call apex_own_pids / apex_kill_own instead)"
        offenders=$((offenders + 1)) ;;
      NOCOVER)
        shift
        echo "   FAIL: $1 starts $* but nothing in it can stop them"
        echo "        (name them in a lib_procs call in that script, or the process outlives the run)"
        offenders=$((offenders + 1)) ;;
      NOLIB)
        echo "   FAIL: $2 kills processes but does not source test/lib_procs.sh"
        offenders=$((offenders + 1)) ;;
      *)
        echo "   FAIL: the process rules produced a line this gate does not understand: $line"
        offenders=$((offenders + 1)) ;;
    esac
  done <<< "$findings"
  [ "$offenders" -eq 0 ] || fail=1
fi

# ---------------------------------------------------------------------------
echo
echo "== 8. the tools the gate calls actually do what the gate thinks =="
#
# ⚠️ A GATE CAN BE GUTTED BY ITS TOOLCHAIN AND STILL REPORT ok.
#
# This section exists because one was. Section 7 drops empty lines from a pipeline with `sed '/^$/d'`. Every
# gate does `export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"`, so `sed` resolves to w64devkit's -- a
# native Windows binary, not an MSYS one -- and Git Bash rewrites any argument that STARTS WITH A SLASH into a
# Windows path before a native program sees it. So sed was handed the path form of `/^$/d` and answered
# "unsupported command C". In a pipeline the exit status belongs to the LAST stage, so the error vanished and
# the stage simply emitted nothing: the coverage half of section 7 was a no-op, printing ok for 25 scripts.
#
# It is not a sed rule and not a quirk of that one expression. It is the path rewrite, and it lands on any
# native tool handed a slash-leading argument -- `awk '/x/ {print}'` survives only because the braces make
# MSYS skip the rewrite. So the gate is checked where it actually is: the pattern must not appear in the tree,
# and the probe below shows why, live, through the same PATH a gate runs with.

# 8a. the shape, anywhere in the tree.
#
# Matching `sed '/'` is deliberately blunt. A sed script that begins with a slash is the thing that breaks,
# and there is no case in this tree where one is needed -- `grep -v '^$'` covers the only use, and every
# substitution starts with `s/`. Blunt is right here: the failure it prevents is silent, and the cost of a
# false positive is one rewritten line.
risky=""
for f in "$ROOT"/test/*.sh "$ROOT"/_diag/*.sh; do
  [ -f "$f" ] || continue
  base="${f##*/}"
  [ "$base" = "check_apex_modular.sh" ] && continue   # this file has to spell the pattern it forbids
  if grep -qE "sed +(-e +)?'/" "$f" 2>/dev/null; then risky="$risky $base"; fi
done
if [ -n "$risky" ]; then
  echo "   FAIL: a sed script starts with a slash in:$risky"
  echo "        Git Bash turns that argument into a Windows path before a native sed sees it,"
  echo "        so the stage produces an error and no output -- silent inside a pipeline."
  echo "        Use grep -v '^$' (or another slash-free spelling) instead."
  fail=1
else
  echo "   ok  no script hands sed a slash-leading script"
fi

# 8b. and the live proof, so the next person does not have to take the paragraph above on faith: run the
# same call through the same PATH and report what happens. This is the shape the fix must keep working.
PROBE_IN=$(mktemp)
printf 'first

second
' > "$PROBE_IN"
probe_lines=$(sed 's/^/x/' "$PROBE_IN" 2>&1 | grep -c . )
probe_slash=$(sed '/^$/d' "$PROBE_IN" 2>&1 | head -1)
rm -f "$PROBE_IN"
if [ "$probe_lines" != "3" ]; then
  echo "   FAIL: the substitution form failed too (got $probe_lines lines) -- the toolchain is not usable"
  fail=1
else
  echo "   ok  sed works through the gate PATH (3 lines in, 3 out)"
fi
# Reported, never enforced: the slash form is expected to break on this toolchain. Recording it here is what
# makes the paragraph above checkable by whoever reads the output -- if it ever starts working, that line says
# so, and the 8a rule above can be retired on evidence.
case "$probe_slash" in
  *"unsupported command"*|*.exe*) echo "   note: sed '/^$/d' is mangled by the path rewrite here: \"$probe_slash\"" ;;
  *) echo "   note: sed '/^$/d' returned \"$probe_slash\" (the rewrite did not bite -- 8a can be revisited)" ;;
esac

# ---------------------------------------------------------------------------
echo
echo "== 9. nothing automated touches the user's mouse or keyboard =="
#
# ⚠️ THE USER'S RULE: "部署时，不要抢鼠标，以及其它相关输入，除非是功能必须的" --
#    and: "一切UI交互测试都由我来做，我会反馈给你。能直接跑的算法数据可以由你来测."
#
# This is a CHECK rather than a note because the project has already broken it once, and the break was not a
# decision anyone made -- it was a line of code left over from an older design. `apex_inject` used to begin with
# `SetCursorPos(x, y)`, from back when the receiver dragged the pointer to a test point; the receiver was later
# rebuilt to come to the CURSOR instead, and the SetCursorPos stayed. Every run of the end-to-end gate then
# teleported the user's pointer for a moment. The user's report was "该项目每次部署都在抢用户鼠标" and they were
# right. No reading of the intent would have found it; only opening the tool would have.
#
# TWO PARTS, because there are two ways to be wrong:
#
#   A. NO SCRIPT IN run_all.sh MOVES THE CURSOR OR TYPES. The gates run unattended, often while the user is
#      working, so anything they call must be passive with respect to input devices.
#   B. ANY TOOL THAT DOES MOVE THE CURSOR MUST SAY SO IN ITS OWN HEADER (`MANUAL-ONLY`). A tool kept for the
#      user to run deliberately is fine; one that looks like every other _diag tool is a trap for whoever
#      wires it into a suite next. The marker is what makes the difference visible at the place the decision
#      is made.
#
# ⚠️ IT SEARCHES FOR THE API CALLS, NOT FOR TEXT, AND THAT MEANS COMMENTS MUST BE STRIPPED FIRST.
#
# The first version of this check scanned the raw file and reported five tools -- of which three mention
# `SetCursorPos` ONLY in a comment explaining that they do not use it (apex_hit_probe: "It is READ-ONLY: no
# SetCursorPos, no clicks"; apex_receiver: "the old version called SetCursorPos..."), and one was this file
# itself, whose pattern text contains the words it forbids. A check that cannot tell a rule from the statement
# it is checking is the same mistake as the process-query rule in section 7, and it fails in the direction that
# costs the most: it teaches the reader to ignore it.
#
# So: strip comments (// and /* */ for C, # for shell), THEN scan. The exemption this file needs is bought
# with a self-test that runs the same scan over a fixture containing a real call and a commented-out one.
StripComments() {
  awk '
    {
      line = $0
      # /* ... */ on one line, then anything after // or #.
      gsub(/\/\*[^*]*\*\//, "", line)
      sub(/\/\/.*/, "", line)
      sub(/#.*/, "", line)
      print line
    }
  ' "$1" 2>/dev/null
}

RISK_RE='SetCursorPos|mouse_event|keybd_event|SendInput|MOUSEEVENTF_LEFT|MOUSEEVENTF_RIGHT'
input_offenders=""
manual_missing=""

# THE SELF-TEST, and it is not ceremony: this check reads source text, so it is exactly the kind of check that
# passes by matching nothing. One fixture has a real call (must be found), the other mentions it in a comment
# (must not be), and a third is the marker itself.
SELFTEST=$(mktemp)
printf 'SetCursorPos(1, 2);\n' > "$SELFTEST"
fixture_hit=$(StripComments "$SELFTEST" | grep -cE "$RISK_RE")
printf '// this tool does not call SetCursorPos\nint x;\n' > "$SELFTEST"
fixture_miss=$(StripComments "$SELFTEST" | grep -cE "$RISK_RE")
rm -f "$SELFTEST"
if [ "$fixture_hit" != "1" ] || [ "$fixture_miss" != "0" ]; then
  echo "   FAIL: the input-safety scan does not tell a call from a comment (call=$fixture_hit comment=$fixture_miss)"
  fail=1
else
  echo "   ok  the input-safety scan sees calls and ignores comments"
fi

# A. the gates. These run unattended, so anything they call must be passive about input devices.
#
# ⚠️ THIS FILE IS EXEMPT, AND THE EXEMPTION IS BOUGHT RATHER THAN CLAIMED. It has to hold the pattern string in
# order to search for it, so it will always match itself. That is only acceptable because the self-test above
# runs the very same scan over a real call and a commented-out mention FIRST: without it, this loop would be
# "everything is safe" whether or not the scan could tell -- the failure mode that makes exemptions dangerous.
for f in "$ROOT"/test/*.sh; do
  [ -f "$f" ] || continue
  base="${f##*/}"
  [ "$base" = "check_apex_modular.sh" ] && continue
  if StripComments "$f" | grep -qE "$RISK_RE"; then
    input_offenders="$input_offenders $base"
  fi
done

# B. the tools: a script or source that moves input must say so in its header, so the next person to wire it
# into a suite can see what it costs. `MANUAL-ONLY` is for a tool that moves the pointer or clicks;
# `WHEEL-ONLY` is for the deliberate wheel injectors (SendInput of MOUSEEVENTF_WHEEL and nothing else), which
# the gates are SUPPOSED to use -- measuring the wheel is the point of the end-to-end gate.
for f in "$ROOT"/_diag/*.sh "$ROOT"/_diag/*.c "$ROOT"/_diag/*.cpp; do
  [ -f "$f" ] || continue
  base="${f##*/}"
  StripComments "$f" | grep -qE "$RISK_RE" || continue
  if ! grep -qE "MANUAL-ONLY|WHEEL-ONLY" "$f" 2>/dev/null; then
    manual_missing="$manual_missing $base"
  fi
done

if [ -n "$input_offenders" ]; then
  echo "   FAIL: a gate moves the cursor or sends input:$input_offenders"
  echo "        (gates run while the user is working -- see the rule at the top of this section)"
  fail=1
else
  echo "   ok  no gate moves the cursor or sends input"
fi

if [ -n "$manual_missing" ]; then
  echo "   FAIL: a probe moves input without saying MANUAL-ONLY in its header:$manual_missing"
  echo "        (mark it, so the next person to wire it into a suite can see what it costs)"
  fail=1
else
  echo "   ok  every input-moving tool declares itself"
fi

#
# ⚠️ AND THERE IS NO "LIVE HALF" HERE, DELIBERATELY. An earlier version of this section sampled the cursor
# position before and after a pause and warned when it had moved. It could not tell MY doing from the USER's:
# the user is allowed to move their mouse while a gate runs, so the check either passed for the wrong reason
# (nothing of ours touches the cursor TODAY) or printed a note that blamed them for using their machine. It
# also cost 0.8 s of PowerShell startup to say nothing -- two `powershell` launches at ~0.39 s each (measured).
#
# The source scan above IS the live check, in the only form that can be honest: it looks at what the tools call
# unconditionally, and a tool cannot move the cursor without calling one of those functions.
echo
if [ $fail -eq 0 ]; then
  echo "OK: no feature named in the host or the panel, the compiler keeps features out, and the ABI agrees"
else
  echo "FAILED: a boundary is only a convention"
fi
exit $fail
