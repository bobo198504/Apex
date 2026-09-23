#!/usr/bin/env bash
# WHO MAY THIS SCRIPT KILL? -- the one implementation of that answer.
#
# WHY THIS FILE EXISTS: the same four lines were copied into nine gates and four probes, and a copied rule is a
# rule that gets fixed in one place and left broken in eight. It has already been wrong twice, in both
# directions:
#
#   * `taskkill /F /IM apex.exe` -- kills EVERY apex.exe on the machine. Apex is a program the USER RUNS, so
#     running the gates shut down their live instance. The name alone is not an identity.
#
#   * `Get-Process | Where-Object { $_.Path -like '*App protable*' }` -- no name filter at all, so this returned
#     every process on the machine and the path fragment matched a whole DIRECTORY of the user's portable
#     tools. The user lost their tray applications to a test run. Nothing had granted that permission; the
#     filter was simply too loose. The path alone is not an identity either.
#
# So: a process belongs to us only if BOTH hold --
#
#   1. its image NAME is one this project builds, and
#   2. its image PATH lies inside a scratch folder the calling script owns.
#
# Either test alone is a bug. Neither is a judgement call, so both live here and nowhere else.
#
# ---------------------------------------------------------------------------
# usage (from a gate or a probe):
#
#   . "$ROOT/test/lib_procs.sh"
#   apex_kill_own '_delivery_run'                       # apex, apex-settings under that folder
#   apex_kill_own 'apex_receiver.exe' apex_receiver     # one of OUR OWN test binaries
#
# The path fragment is matched against the image path, so it must name something that belongs to this run --
# normally the private copy's folder (`_delivery_run`), never a shared directory such as build/.
# ---------------------------------------------------------------------------

# Prints the pids. $1 = path fragment, $2 = comma-separated image names (default: the two host binaries).
apex_own_pids() {
  local frag="$1" names="${2:-apex,apex-settings}"
  [ -n "$names" ] || names="apex,apex-settings"   # an empty -Name list would error, not match everything
  # ⚠️ AN EMPTY FRAGMENT IS NOT "NO FILTER", IT IS "EVERY PROCESS". The match is `-like '*$frag*'`, so with
  # nothing in the middle every image path matches -- i.e. the exact accident this file exists to prevent, and
  # one that would look like a working call at the place it is written. Refuse it instead.
  if [ -z "$frag" ]; then
    echo "lib_procs: refusing an empty path filter (that would select every process on the machine)" >&2
    return 0
  fi
  # And the fragment has to be specific: either this project's scratch-folder convention (`_flash_run`,
  # `_delivery_run`, ...), or the exact file name of a test binary this project builds (`apex_receiver.exe`).
  # Anything else -- `build`, `protable`, a drive letter -- is a filter that happens to work today and would
  # widen on its own tomorrow.
  case "$frag" in
    _*|*.exe) ;;
    *) echo "lib_procs: refusing the path filter '$frag' -- it is neither a scratch folder (leading _) nor one of our own binaries" >&2
       return 0 ;;
  esac
  powershell -NoProfile -Command \
    "Get-Process -Name $names -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '*$frag*' } | Select-Object -ExpandProperty Id" \
    2>/dev/null | tr -d '\r'
}

# Same filter, force-stopped. Silent when there is nothing to stop: a run that never started its host must not
# turn a cleanup into a second failure.
apex_kill_own() {
  local frag="$1" names="${2:-apex,apex-settings}" p
  for p in $(apex_own_pids "$frag" "$names"); do
    taskkill //F //PID "$p" >/dev/null 2>&1
  done
}

# Pids of Apex processes that are NOT ours -- the copy the user is running, or another gate's.
#
# ⚠️ THIS IS A READER, NEVER A KILLER, AND IT EXISTS BECAUSE APEX TAKES OVER THE WHOLE MACHINE'S WHEEL. A gate
# that measures "what arrives when Apex is not running" is measuring a premise that is FALSE if the user is
# running their own copy: the wheels it is watching for may belong to them. That is not hypothetical -- a suite
# run reported a baseline of 790 messages for 3 injected notches, all of them the user scrolling, and the gate
# went on to print a conclusion about the code from that number.
#
# So a measurement gate asks this first and says so. Nothing here is ever killed: the user's Apex is the user's.
apex_foreign_pids() {
  local frag="$1" names="${2:-apex,apex-settings}"
  [ -n "$frag" ] || { echo "lib_procs: apex_foreign_pids needs the caller's own path fragment" >&2; return 0; }
  case "$frag" in
    _*|*.exe) ;;
    *) echo "lib_procs: refusing the path filter '$frag' (see apex_own_pids)" >&2; return 0 ;;
  esac
  powershell -NoProfile -Command \
    "Get-Process -Name $names -ErrorAction SilentlyContinue | Where-Object { \$_.Path -notlike '*$frag*' } | Select-Object -ExpandProperty Id" \
    2>/dev/null | tr -d '\r'
}

# ---------------------------------------------------------------------------
# ⚠️ THE USER'S RULE, AND THE REASON THE OLD FILTERS ARE NOT ENOUGH FOR IT:
#
#     "这个项目开发过程，部署时，必须清掉当前项目在运行的进程，不管是不是我在运行；进程只能有一个Apex.exe"
#
# A gate that starts a private host, and the deploy script, must therefore CLEAR THE FIELD first. Neither of
# the two filters above can express that:
#
#   * apex_own_pids matches a path fragment -- it lists only OUR copy, which is the opposite of clearing.
#   * apex_foreign_pids lists the others, but it matches on `apex.exe` BY NAME, and kills would then hit any
#     unrelated program that happens to be called apex.exe.
#
# So the field is cleared by WINDOW CLASS: `ApexHostWnd` / `ApexSettingsWnd` are registered by this program
# and nothing else (see _diag/apex_owners.cpp). A process owning one of them IS Apex, wherever its exe lives.
# That is the same identity the product itself uses for single-instance and for the two halves finding each
# other, so "clear the field" and "refuse a second instance" cannot disagree.
#
# ⚠️ THESE ARE THE ONLY KILLS IN THIS PROJECT THAT ARE ALLOWED TO TOUCH A PROCESS THE CALLER DID NOT START.
# The user authorised exactly that: "不管是不是我在运行". Everything else still obeys name AND path.
# ---------------------------------------------------------------------------

# Pids of every running Apex, by window class. Empty output when none is running.
apex_all_pids() {
  local owners
  owners="$(dirname "${BASH_SOURCE[0]}")/../build/apex_owners.exe"
  if [ ! -x "$owners" ]; then
    echo "lib_procs: apex_owners.exe is not built (run apex/build.sh) -- cannot identify a running Apex" >&2
    return 0
  fi
  "$owners" 2>/dev/null | tr -d '\r'
}

# Stop every running Apex. Silent when none is running.
#
# ⚠️ THE PANEL IS STOPPED BY THE HOST, NOT SEPARATELY. The host is what installed the global hook, so it is the
# one that must go first; and it shuts its own panel down on the way out (that is the shutdown gate's subject).
# Killing panels first would leave a host that is about to be killed anyway, and killing both at once races the
# host's own teardown. The second pass is for a panel that outlived a host which was killed rather than closed.
apex_clear_the_field() {
  local p n=0
  for p in $(apex_all_pids); do
    n=$((n + 1))
    taskkill //F //PID "$p" >/dev/null 2>&1
  done
  if [ "$n" -gt 0 ]; then
    # Give the host a moment to finish: it is the process holding the wheel hook.
    sleep 0.7
  fi

  # A panel whose host was killed rather than closed survives, so ask again -- this time by the panel's class,
  # still by window, never by name.
  for p in $(apex_all_pids); do
    taskkill //F //PID "$p" >/dev/null 2>&1
  done
  return 0
}

# How many Apex processes are running, by window class. Used by the gates that need to say "the field is not
# clear" rather than silently measuring the wrong one.
apex_count() {
  apex_all_pids | grep -c . 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# WAIT FOR A CONDITION, WITH A BUDGET -- never `sleep N` and hope.
#
# The project's own rule: "门的等待从固定 sleep 改成等条件" (a fixed sleep is either slower than it needs to be or
# too fast on a busy machine, and the failure it produces looks like a code bug). Measured 2026-09-20: the
# assembly layer held 57 s of fixed sleeps, 19 s of them in one gate.
#
# ⚠️ A CONDITION THAT CANNOT BE OBSERVED IS STILL A SLEEP, AND THAT IS FINE -- but say so at the call site.
# "5 seconds of nothing happening" is a MEASUREMENT (the chattering check in check_reaper_live), not a wait, and
# it must stay a sleep. What must not stay a sleep is "the panel has probably started by now".
#
# 0.1 s steps: a 6 s budget is 60 spawned `sleep`s (a few ms each) -- still an order of magnitude cheaper than
# the 6 s it replaces, and it returns the moment the condition holds.
# ---------------------------------------------------------------------------

# apex_wait_for <seconds> <command...>  -- 0 as soon as the command succeeds, 1 when the budget runs out.
apex_wait_for() {
  local budget="$1"; shift
  local tries=$(( ${budget%.*} * 10 )) i=0
  [ "$tries" -gt 0 ] || tries=1
  while [ "$i" -lt "$tries" ]; do
    if "$@" >/dev/null 2>&1; then return 0; fi
    i=$((i + 1))
    sleep 0.1
  done
  return 1
}

# apex_wait_line <file> <pattern> <seconds>  -- 0 as soon as the file contains a matching line.
apex_wait_line() {
  apex_wait_for "$3" grep -q "$2" "$1"
}

# How many lines match. ⚠️ NOT `grep -c ... || echo 0`: `grep -c` prints 0 AND exits 1 when nothing matches, so
# that idiom prints "0" TWICE and `[ "0\n0" -gt 0 ]` is a bash ERROR -- the condition then never holds and a wait
# silently burns its whole budget. (Cost of that, measured: 14 s in one gate, 32 s in another.)
apex_count_line() {
  local n
  n=$(grep -c "$2" "$1" 2>/dev/null)
  echo "${n:-0}"
}

# Wait until the file contains MORE matching lines than it did at `base`. This is the shape a gate needs when the
# same sentence is written more than once in a run: "wait for the line" is satisfied by a line from an earlier
# step, "wait for the count to grow" is satisfied only by this step's. (Measured: the first version of the REAPER
# wait matched the host's startup line and returned instantly, so the real announcement landed in the next
# section's quiet window and was reported as chattering.)
apex_wait_line_gt() {
  local file="$1" pat="$2" base="$3" budget="$4" i=0 tries
  tries=$(( ${budget%.*} * 10 )); [ "$tries" -gt 0 ] || tries=1
  while [ "$i" -lt "$tries" ]; do
    [ "$(apex_count_line "$file" "$pat")" -gt "$base" ] && return 0
    i=$((i + 1))
    sleep 0.1
  done
  return 1
}

# /d/App protable/Apex/apex.exe -> D:\App protable\Apex\apex.exe
apex_win_path() {
  local p d
  p="$(printf '%s' "$1" | sed -e 's|^/\([a-z]\)/|\1:/|' -e 's|/|\\|g')"
  d="$(printf '%s' "$p" | cut -c1 | tr 'a-z' 'A-Z')"
  printf '%s%s' "$d" "$(printf '%s' "$p" | cut -c2-)"
}

# Start an exe through WMI (the WMI service does the create). The path may be given either way round -- a bash path
# (`/d/App protable/Apex/apex.exe`) or a Windows one (`D:\App protable\Apex\apex.exe`), because the two callers
# have different things in hand: the deploy knows a bash path, the field-state file holds what PowerShell printed.
#
# Returns 0 when the WMI service reported a process id, 1 when it refused -- in which case a plain start is used
# and SAID OUT LOUD.
apex_start_detached() {
  local exe="${1:?apex_start_detached needs a path to the exe}"
  local wexe wdir out
  case "$exe" in
    [A-Za-z]:[\\/]*) wexe="$exe" ;;                      # already Windows-spelled
    *)               wexe="$(apex_win_path "$exe")" ;;
  esac
  case "$wexe" in
    *\\*) wdir="${wexe%\\*}" ;;                          # the directory, in the same spelling
    *)    wdir="$(dirname "$wexe")" ;;
  esac
  out=$(powershell -NoProfile -Command \
    "\$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = '\"$wexe\"'; CurrentDirectory = '$wdir' }; '{0} {1}' -f \$r.ReturnValue, \$r.ProcessId" \
    2>/dev/null | tr -d '\r')
  case "$out" in
    0\ *) return 0 ;;
  esac
  echo "   ⚠️  could not start $wexe through WMI (answer: '${out:-none}') -- starting it directly instead." >&2
  powershell -NoProfile -Command "Start-Process -FilePath '$wexe' -WorkingDirectory '$wdir'" >/dev/null 2>&1
  return 1
}

# ---------------------------------------------------------------------------
# TAKE THE FIELD / PUT IT BACK.
#
# A gate cannot start its own host while the user's is running any more (one Apex per machine -- see the rule
# above). So it TAKES the field: it records what was running, stops it, and runs its own copy.
#
# ⚠️ AND IT GIVES IT BACK, BECAUSE THE PROJECT'S OWN RULE SAYS SO: "测试不许打扰用户 ... 不许改系统设置而不还原".
# Killing the user's Apex and walking away leaves them without the program they had open -- the same
# disturbance the rule forbids, just with a process instead of a setting. A gate that took the field restores
# it on the way out, including on the failure paths (call it from the trap).
#
# ⚠️ THE RESTORE IS A PLAIN RELAUNCH OF THE SAME EXE PATH, and it is deliberately not cleverer than that: it
# does not restore window positions, it does not wait for the program to be useful, and it does not try to
# reconstruct which folder the user "meant". It starts the executable that was running.
#
# The state lives in a file rather than in a variable, because the gates call these from a subshell (`$(...)`)
# in some paths and from the trap in others, and a variable would be a different variable in each.
# ---------------------------------------------------------------------------
apex_field_state_path() {
  echo "${TMPDIR:-/tmp}/apex_field_state.$$"
}

# IS THIS PROCESS PATH ONE OF OUR OWN SCRATCH COPIES (i.e. under this project's build/)?
#
# ⚠️⚠️ THE COMPARISON HAS TO SURVIVE TWO SPELLINGS OF THE SAME PATH, AND THIS FUNCTION EXISTS BECAUSE THE FIRST
# VERSION DID NOT. `$root` comes from bash (`/d/Projects/Code/Apex`) and the path comes from PowerShell
# (`D:\Projects\Code\Apex\build\_single_a\apex.exe`), so a `case "$path" in "$root"/build/*)` test can never match
# -- and it never did. The rule it was written to enforce ("a scratch copy is stopped and FORGOTTEN") therefore
# never fired: a copy left behind by a crashed gate was recorded and RELAUNCHED by the next gate's restore,
# which is the one outcome the rule exists to prevent.
#
# It showed up as a flake in check_apex_single: "2 Apex processes are running (the rule is one)", both of them
# under build/_single_a -- the gate's own copy and a restored leftover, racing each other into existence. A gate
# that fights itself is worse than a gate that fails, because the next run goes green and the fault is filed as
# noise. (Measured, not reasoned: build/_pathcheck.sh printed the two forms side by side and the mismatch.)
apex_is_scratch_path() {
  local p root unix win
  p="$(printf '%s' "${1:-}" | tr '\\' '/' | tr 'A-Z' 'a-z')"
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  unix="$(printf '%s' "$root" | tr 'A-Z' 'a-z')"
  # The Windows spelling of the same root: D:/projects/code/apex
  win="$(printf '%s' "$unix" | sed -E 's|^/([a-z])/|\1:/|')"
  case "$p" in
    "$unix"/build/*|"$win"/build/*) return 0 ;;
    *) return 1 ;;
  esac
}

# Records what is running and stops it. Prints one line per process it stopped, for the gate's report.
#
# ⚠️ A PROCESS INSIDE THIS PROJECT'S build/ IS NEVER RECORDED FOR RESTORE. Every gate runs its own copy out of
# build/_something, and a gate that leaves one running (a crash, a Ctrl-C) would otherwise be found by the
# NEXT take and relaunched at the end of the run -- so a suite could finish by starting some scratch copy
# instead of the user's installation. The user's rule is that the running instance is
# D:\App protable\Apex\apex.exe, so what gets restored is restricted to it by construction: anything under
# $ROOT/build is stopped and forgotten.
#
# ⚠️ AND THAT TEST IS DONE BY `apex_is_scratch_path`, NOT BY A `case` ON `$root`. The `case` version never matched
# -- see the note on that function: bash paths and PowerShell paths are spelled differently, so the rule above was
# a promise the code did not keep, and the symptom was two hosts racing in check_apex_single.
apex_take_the_field() {
  local state p path
  state="$(apex_field_state_path)"
  : > "$state"

  for p in $(apex_all_pids); do
    path=$(powershell -NoProfile -Command "(Get-Process -Id $p -ErrorAction SilentlyContinue).Path" 2>/dev/null | tr -d '\r')
    if [ -n "$path" ]; then
      if apex_is_scratch_path "$path"; then
        # This gate's own scratch copy (or the one a previous gate forgot). Stop it, do not remember it.
        echo "   (taking the field: stopped a scratch copy at $path -- not restored)"
      else
        echo "$path" >> "$state"
        if [ "${APEX_KEEP_FIELD:-0}" = "1" ]; then
          echo "   (taking the field: stopped $path (pid $p) -- KEPT DOWN for the delivery that follows)"
        else
          echo "   (taking the field: stopped $path (pid $p) -- will be restored at the end)"
        fi
      fi
    else
      echo "   (taking the field: stopped pid $p -- its path could not be read, so it is not restored)"
    fi
  done

  apex_clear_the_field
  return 0
}

# Relaunches whatever apex_take_the_field stopped. Silent when nothing was taken.
apex_restore_the_field() {
  local state path
  state="$(apex_field_state_path)"
  [ -f "$state" ] || return 0

  # Each line is one process that was running. Only the HOST is relaunched: the panel is started by the host
  # itself (that is the arrangement the shutdown gate tests), so relaunching it separately would produce two.
  #
  # ⚠️ APEX.EXE, NOT apex-settings.exe, AND NOT BY DIRECTORY TESTING THE PEER: `apex-settings.exe` ends with
  # "apex-settings.exe", so a suffix test on "apex.exe" already excludes it -- but the path may be either
  # separator-cased, so the test is done in bash on the basename.
  while IFS= read -r path; do
    [ -n "$path" ] || continue
    case "$(basename "$path" | tr 'A-Z' 'a-z')" in
      apex.exe)
        # start it through WMI (apex_start_detached)
        apex_start_detached "$path"
        ;;
    esac
  done < "$state"

  rm -f "$state"
  return 0
}
