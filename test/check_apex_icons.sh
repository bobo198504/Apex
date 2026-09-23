#!/usr/bin/env bash
# Gate: THE PRODUCT MARK -- that both versions exist, that they are actually different, and that each
# appearance gets the one whose PLATE matches it.
#
# WHY THIS GATE EXISTS. The tray icon did not follow the theme, and NOTHING in this repository could have
# noticed: check_apex_language.sh asserted the language rules and its header claimed the icon variant was
# checked too, but no probe ever loaded an icon. The failure was in the resource table -- the .rc registered
# the artwork under string NAMES while the code asked for NUMBERS -- so every layer reported success:
#
#   * LoadImage returned NULL and the shell kept showing the mark it already had;
#   * the host's log line printed the mark that was CHOSEN, not the one that LOADED, so it read as proof;
#   * the compiler and the resource compiler both said nothing, because a .rc name that no one has #defined
#     is not an error -- it is a resource with that name.
#
# So this gate starts from the ARTEFACT, not the source: it loads the ids out of the built exes the way the
# product does, converts them to pixels, and measures. Then it runs the host and reads what it decided.
#
# ⚠️ AND IT CHECKED THE WRONG DIRECTION FOR A WHILE. It asserted "a light theme wants the DARK mark",
# reasoning about contrast -- which is right for a bare glyph and WRONG for this artwork, where each mark is
# a PLATE that matches the background with the glyph carrying the contrast (see apex/icons.h). The gate then
# PASSED while the icons were visibly swapped on screen. The assertion below is the direct mapping the file
# names were always meant to say: a light appearance gets IDI_APEX_LIGHT. What that check can and cannot do is
# written out where it is made.
#
# THE THREE THINGS IT ASSERTS, and the failure each one catches:
#
#   1. both ids load in both exes        -- the resource ids and the .rc files have drifted apart again
#   2. the two marks differ in luma      -- one file has been copied over the other; a swap would be invisible
#   3. the host, given an appearance, asks for the matching mark
#                                        -- the decision is disconnected from the setting (the pinned-theme
#                                           path was silently dead: the panel said it affected the tray,
#                                           and the tray never read it)
#
# THE RUN IS FROM A COPY, in build/_icons_gate, so this never touches the settings or the log of the copy the
# user runs. (One Apex runs at a time -- this gate takes the field for the couple of seconds it needs.)
#
# usage: test/check_apex_icons.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

SCRATCH="$ROOT/build/_icons_gate"
fail=0

# ---------------------------------------------------------------------------
# ⚠️ STOP ONLY THE HOST THIS GATE STARTED -- NEVER BY IMAGE NAME.
#
# `taskkill /IM apex.exe` kills every apex.exe on the machine, and this program is one the USER RUNS: a test
# run would shoot down their live instance, wheel smoothing and all. It did, repeatedly, and it is half the
# reason settings were disappearing -- a killed process never reaches the exit path that writes them (the
# other half is why the host now writes them on a debounce; see SettingsTouch in main.cpp).
#
# Killing by PATH is what makes this safe, and it works because the gate runs its host out of a scratch COPY
# (build/_icons_gate/apex/...), never out of the built folder the user might be running.
#
# It is also silent about finding nothing: a run that failed to start its host must not turn a cleanup into a
# second failure. The filter itself is in test/lib_procs.sh.
# ---------------------------------------------------------------------------
HostPidFromScratch() { apex_own_pids '_icons_gate' | head -1; }
KillScratchHost() {
  local pid
  pid=$(HostPidFromScratch)
  [ -n "$pid" ] && taskkill //F //PID "$pid" >/dev/null 2>&1
}
# ⚠️ ONE APEX PER MACHINE -- see the note in check_apex_flash.sh. This gate starts a host out of a scratch
# copy, which cannot start while the user's is running, so the field is taken first and given back at exit.
# The note above about "two hosts coexisting" described the old folder-scoped rule and is kept only as the
# reason these processes are killed by path: that part is still true -- THIS gate kills only its own copy.
trap 'KillScratchHost; apex_restore_the_field' EXIT
apex_take_the_field

echo "== building the probes =="
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
gcc -O2 -o "$SCRATCH/icon_probe.exe" "$ROOT/_diag/apex_icon_probe.c" -luser32 -lgdi32 || exit 1
g++ -std=c++17 -O2 -I"$ROOT/apex" -DAPEX_BUILDING_HOST -o "$SCRATCH/theme_probe.exe" "$ROOT/_diag/apex_theme_probe.cpp" || exit 1
g++ -std=c++17 -O2 -mconsole -o "$SCRATCH/window_icon_probe.exe" "$ROOT/_diag/apex_window_icon_probe.cpp" \
  -luser32 -lgdi32 || exit 1
echo "   ok"

# THE IDS COME FROM icons.h, which is the file the product and both .rc scripts share. Reading them here
# rather than hard-coding 1 and 2 is what makes the gate able to fail: if someone renumbers the marks and
# forgets a .rc file, the probe is handed the NEW numbers and finds nothing.
LIGHT_ID=$(awk '/^#[ \t]*define[ \t]+IDI_APEX_LIGHT/ {print $3; exit}' "$ROOT/apex/icons.h")
DARK_ID=$(awk '/^#[ \t]*define[ \t]+IDI_APEX_DARK/ {print $3; exit}' "$ROOT/apex/icons.h")
echo
echo "== the shared ids (apex/icons.h) =="
echo "   IDI_APEX_LIGHT=$LIGHT_ID   IDI_APEX_DARK=$DARK_ID"
if [ -z "$LIGHT_ID" ] || [ -z "$DARK_ID" ]; then
  echo "   FAIL: icons.h does not define both ids -- the .rc files include it, so nothing is registered"
  exit 1
fi
if [ "$LIGHT_ID" = "$DARK_ID" ]; then
  echo "   FAIL: both names resolve to the same id, so one artwork can never be reached"
  exit 1
fi

# The luma the probe measured for a given id, out of its own output.
LumaOf() {
  printf '%s\n' "$1" | sed -n "s/.*id=$2 .*luma=\([0-9.]*\).*/\1/p" | head -1
}

echo
echo "== the marks inside the built exes =="
for exe in apex.exe apex-settings.exe; do
  path="$ROOT/build/apex/$exe"
  if [ ! -f "$path" ]; then
    echo "   $exe: MISSING -- build first (apex/build.sh)"
    fail=1
    continue
  fi
  out=$("$SCRATCH/icon_probe.exe" "$path" "$LIGHT_ID" "$DARK_ID" 2>&1)
  rc=$?
  printf '%s\n' "$out" | sed -n 's/^  id=/     id=/p;s/^  RT_GROUP_ICON/     /p;s/^  FAILED/     /p'
  if [ $rc -ne 0 ]; then
    echo "   $exe: FAIL -- an id the code asks for is not in this exe (that is the bug this gate was built for)"
    fail=1
    continue
  fi
  # The NAMES have to match the ARTWORK, not just exist: IDI_APEX_LIGHT is the cream-plated one. If the two
  # were swapped in the .rc files, the tray would still swap on a theme change -- while showing the wrong
  # mark on both appearances.
  #
  # ⚠️ THIS MEASURES THE PLATE, and the plate is deliberately the same colour as the background the mark
  # belongs on. So "the light one is brighter" is a check that the two FILES are the right way round -- it is
  # NOT a rule for choosing between them (see apex/icons.h; reading the mean luma as "the bright one must be
  # for a dark desktop" is exactly the mistake that swapped these two on screen once).
  light_luma=$(LumaOf "$out" "$LIGHT_ID")
  dark_luma=$(LumaOf "$out" "$DARK_ID")
  if [ -z "$light_luma" ] || [ -z "$dark_luma" ]; then
    echo "   $exe: FAIL -- the probe reported no luma for one of the ids"
    fail=1
    continue
  fi
  if awk -v a="$light_luma" -v b="$dark_luma" 'BEGIN { exit !(a > b + 32) }'; then
    echo "   $exe: ok (IDI_APEX_LIGHT holds the cream-plated artwork: luma $light_luma vs $dark_luma)"
  else
    echo "   $exe: FAIL -- the artwork behind the two names is not what the names say (luma $light_luma vs $dark_luma)"
    fail=1
  fi
done

echo
echo "== the rule that decides which one is wanted =="
"$SCRATCH/theme_probe.exe" | sed 's/^/   /'
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
  fail=1
fi

# ---------------------------------------------------------------------------
# AND WHAT THE HOST ACTUALLY DOES WITH IT.
#
# The probes above prove the marks exist and the rule is right. Neither proves the host ASKS. That wiring is
# the other half of the reported bug -- a pinned theme reached the settings, the panel said it affected the
# tray, and the tray's own icon choice read the system instead. The evidence is the host's own log line,
# which is the one channel that exists precisely because the tray belongs to the shell (see AGENTS.md).
# ---------------------------------------------------------------------------echo
echo "== the host's own decision, read from its log =="

# What the system says, so the `auto` expectation is derived rather than assumed. The value is a hex
# DWORD: `0x1` is light. (An earlier version of this line used `0x0*` to absorb leading zeros, which
# swallowed the single 0 of `0x0` as well and made a DARK system read as "cannot be read".)
syslight=$(reg query 'HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize' //v AppsUseLightTheme 2>/dev/null |
  sed -n 's/.*REG_DWORD[ \t]*0x\([0-9a-fA-F]*\).*/\1/p' | head -1)
if [ -z "$syslight" ]; then
  syslight=0 # the same default the code takes: a missing value means dark
  echo "   (the system theme could not be read; assuming dark, which is the code's own default)"
fi
if [ "$syslight" = "1" ]; then sys_theme="light"; else sys_theme="dark"; fi

cp -r "$ROOT/build/apex" "$SCRATCH/apex" 2>/dev/null || { echo "   FAIL: the built folder could not be copied"; exit 1; }
RUN="$SCRATCH/apex"

# One run per setting; the expectation is written out so a failure says what was wanted and why.
#
# ⚠️ THE FEATURE IS TURNED OFF IN THE CONFIG, AND THAT IS WHAT MAKES THIS GATE HARMLESS. Apex is a real host
# with a real wheel hook; if a feature were live it would smooth the user's wheel for the second or two each
# run lasts. This used to be done with the host's master switch (`enabled=0`), which no longer exists -- the
# replacement is `off=<feature>`, which is the same thing expressed the only way left: with no feature able
# to deliver, `decision.h` passes every wheel through untouched. The icon choice does not depend on feature
# state at all, so the gate still gets its answer.
TryConfig() {
  local theme="$1" want_mark="$2" why="$3"
  printf 'theme=%s\noff=SmoothWheel\n' "$theme" > "$RUN/apex.ini"
  rm -f "$RUN/apex.log"
  ( cd "$RUN" && cmd //c start "" apex.exe >/dev/null 2>&1 )
  # The tray line is written as the icon is added, within the first second of startup; poll rather than
  # sleeping for a fixed time, so a slow machine does not turn into a false failure.
  local i=0 line=""
  while [ $i -lt 30 ]; do
    line=$(grep -m1 '^tray: ' "$RUN/apex.log" 2>/dev/null)
    [ -n "$line" ] && break
    sleep 0.1
    i=$((i + 1))
  done
  # ⚠️ THE HOST IS DELIBERATELY STILL RUNNING HERE. The window-icon check below has to read a LIVE window,
  # so it is killed at the single exit at the bottom of this function rather than at this point.
  #
  # Everything from here on sets `rc` instead of returning, so that one exit killed the process on every path
  # -- an early return that skipped the kill would leave a host running with its wheel hook installed.
  local rc=0 luma plate
  if [ -z "$line" ]; then
    echo "   theme=$theme: FAIL -- the host wrote no tray line at all ($why)"
    rc=1
  else
    printf '   theme=%-5s %s\n' "$theme" "$line"
    if ! printf '%s\n' "$line" | grep -q "icon=$want_mark "; then
      echo "        FAIL: wanted $want_mark ($why)"
      rc=1
    fi
    # A mark that was chosen but could not be loaded is the original bug; the line now reports the load.
    if ! printf '%s\n' "$line" | grep -q "load=ok"; then
      echo "        FAIL: the wanted mark could not be loaded from the exe"
      rc=1
    fi
  fi

  # -------------------------------------------------------------------------
  # ⚠️ THE LINK THAT WAS MISSING, and the icons were visibly swapped on screen while everything above was
  # green. "The host asked for light-mark" plus "IDI_APEX_LIGHT holds the cream plate" does not add up to
  # "a light appearance gets the cream plate" while the log text is built by comparing against the same
  # constant the choice was made with: a swapped mapping stays perfectly self-consistent.
  #
  # This reads the pixels the HOST'S OWN WINDOW is holding, which ties the three together -- the appearance,
  # the mark the log named, and the artwork behind it. The host sets its window icon from the same id as the
  # tray, so it is the same decision; and unlike the tray (which belongs to the shell and cannot be read
  # back), this one can be measured.
  # -------------------------------------------------------------------------
  if [ $rc -eq 0 ]; then
    luma=$("$SCRATCH/window_icon_probe.exe" apex.exe ApexHostWnd 2>/dev/null |
      sed -n 's/.*luma=\([0-9.]*\).*/\1/p' | head -1)
    if [ -z "$luma" ]; then
      echo "        FAIL: the host's window icon could not be read, so the artwork behind it is unverified"
      rc=1
    else
      if [ "$want_mark" = "light-mark" ]; then plate="cream"; else plate="dark"; fi
      if awk -v l="$luma" -v want="$plate" \
           'BEGIN { if (want == "cream") exit !(l > 128); else exit !(l < 128) }'; then
        echo "        and its window holds the $plate plate (luma $luma), which is what $theme needs"
      else
        echo "        FAIL: $want_mark should hold the $plate plate, but the window holds luma $luma"
        rc=1
      fi
    fi
  fi

  KillScratchHost # this run's host only -- see the note at the top of this script
  # ⚠️ WAIT FOR THE PROCESS TO ACTUALLY GO rather than sleeping a flat second. Each run starts a real host and
  # there are three of them, so a fixed second is three seconds spent waiting for something that normally takes
  # milliseconds. The cap still matters: if the kill failed, the next run's ini would be read by a host that is
  # already up -- so after the cap it falls back to the old flat wait rather than pressing on.
  #
  # ⚠️ AND IT ASKS ABOUT THE SCRATCH HOST, NOT ABOUT "apex.exe". Asking by image name would see the user's own
  # running copy, never come back false, and spend the full cap here followed by a second of sleep -- every
  # run -- while telling itself it was waiting for something that had already gone.
  local gone=0
  while [ $gone -lt 60 ]; do
    [ -z "$(HostPidFromScratch)" ] && break
    sleep 0.05
    gone=$((gone + 1))
  done
  if [ $gone -ge 60 ]; then
    sleep 1 # it really did not die: give it the old flat wait and carry on
  fi
  return $rc
}

# auto -> the system's answer, and the mapping is the DIRECT one: a light appearance gets the light mark,
# whose plate matches a light background. (This said the opposite while the icons were swapped on screen --
# see the header, and apex/icons.h for why "contrast" is the wrong intuition for this artwork.)
if [ "$sys_theme" = "light" ]; then AUTO_MARK="light-mark"; else AUTO_MARK="dark-mark"; fi
TryConfig auto "$AUTO_MARK" "auto follows the $sys_theme system" || fail=1
TryConfig light light-mark "a light appearance gets the cream-plated mark" || fail=1
TryConfig dark dark-mark "a dark appearance gets the dark-plated mark" || fail=1

# ---------------------------------------------------------------------------
echo
echo "== the mark while a feature is working: the middle bar turns orange =="
#
# ⚠️ THIS IS THE HALF THAT WAS MISSING, and it cost the user a round trip. The host logged `load=ok` for the
# composite mark while the shell was being handed an icon it could not draw (no mask / no alpha), so "the mark
# never changed" and "the composition silently failed" looked identical from outside. And there are THREE states
# to check now, not two -- the user asked for the tray to say WHICH of the two things KeepAwake is holding
# ("保持唤醒（绿色），保持唤醒+防止熄屏（红色）"), so a second ink that composed to the same pixels as the first
# would leave the distinction invisible while every layer reported success. This runs the same composition
# (_diag/apex_badge_probe.cpp, the host's own apex/traymark.h) against the built exe and checks: the marks load,
# both working versions compose, and each state's bar really is ITS OWN ink -- and carries none of the other --
# for BOTH appearances, because the plate decides which colour is legible.
BADGE_PROBE="$ROOT/_diag/apex_badge_probe.cpp"
if [ ! -f "$BADGE_PROBE" ]; then
  echo "   FAIL: $BADGE_PROBE is missing -- the working mark would not be checked at all"
  fail=1
else
  export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
  if g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I"$ROOT/apex" -o "$ROOT/build/_badge_probe.exe" "$BADGE_PROBE" \
       -lgdi32 -luser32 2>&1 | sed 's/^/   /'; then
    bmp="$ROOT/build/_badge.bmp"
    if "$ROOT/build/_badge_probe.exe" "$ROOT/build/apex/apex.exe" "$bmp" 2>&1 | sed 's/^/   /'; then
      : # the probe prints its own verdict
    else
      fail=1
    fi
    # ⚠️ AND THE IMAGE IS KEPT, not thrown away: the numbers say the bar is the right colour, and the picture is
    # the only thing that says the mark still LOOKS like the mark. (Look at build/_badge.bmp after a run: plain,
    # green and red, light and dark, side by side.)
    [ -f "$bmp" ] && echo "   (the picture is at build/_badge.bmp -- six marks: plain/hold/hold-hard, light/dark)"
  else
    echo "   FAIL: the probe did not compile"
    fail=1
  fi
fi

rm -rf "$SCRATCH"
echo
if [ "$fail" -eq 0 ]; then
  echo "OK: both marks load in both exes, they differ, and each appearance gets the plate that matches it"
else
  echo "FAILED"
fi
exit $fail
