#!/usr/bin/env bash
# _diag/mica_probe.sh -- build the window-material probe, run it over the combinations, print the numbers.
#
# ⚠️⚠️ RUN THIS FROM A NORMAL SHELL, NOT FROM INSIDE THE DSH SANDBOX. Inside the confined harness WebView2's
# browser process crashes (0x80000003) and Windows puts an error dialog on the user's screen -- which is the
# disturbance this project's rules forbid, and it is not the probe's fault: the product's own apex-settings.exe
# fails there too ("WebView2 controller failed (0x8000ffff)"), because the confined modes block the named pipes
# WebView2's IPC is built on.
#
# ⚠️ THIS IS NOT A GATE AND IT IS NOT IN test/run_all.sh. It puts a window on screen (topmost, foreground) for
# about two seconds per run, because the only way to see what DWM draws BEHIND a window is to photograph the
# composited screen. It never touches the keyboard or the mouse, it uses its own exe name and its own folder, and
# it does not start, stop or disturb the user's Apex.
#
# WHAT IT ANSWERS:
#   1. does a DWM material reach the screen through this window at all -- and under which recipe?
#   2. is the material's light/dark variant steered by this WINDOW, or only by the system?
#      (The panel can PIN a theme against the system, so this is the question that could make Mica a bad idea.)
#   3. what does the "no material" case look like?
#   ... and, once the page is up, whether a TRANSPARENT WebView2 lets the material through to the page's area.
#
# usage: bash _diag/mica_probe.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
. "$ROOT/test/lib_procs.sh"   # who this script may kill (name AND path) -- see the file

RUN="$ROOT/build/_mica_run"
SRC="$ROOT/_diag/mica_probe.cpp"
EXE="$RUN/mica_probe.exe"
WV2="$ROOT/third_party/webview2"

if [ ! -f "$WV2/include/WebView2.h" ]; then
  echo "SKIPPED: third_party/webview2 is missing (nothing to host a page in)"
  exit 1
fi

mkdir -p "$RUN"

# ---------------------------------------------------------------------------
# BUILD. The flags are the panel's own (apex/build.sh), minus the parts this probe does not need: it is a
# standalone diagnostic, it includes no project header, and it links the same WebView2Loader import library --
# the loader DLL itself has to be beside the exe, exactly as apex-settings.exe needs it.
# ---------------------------------------------------------------------------
if [ ! -f "$EXE" ] || [ "$SRC" -nt "$EXE" ]; then
  echo "== building mica_probe.exe =="
  g++ -std=c++17 -O2 -mwindows -I"$WV2/include" "$SRC" -o "$EXE" \
      "$WV2/x64/WebView2Loader.dll.lib" \
      -luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32 -lcomctl32 || exit 1
else
  echo "== mica_probe.exe (up to date) =="
fi
cp -f "$WV2/x64/WebView2Loader.dll" "$RUN/" 2>/dev/null

echo
echo "⚠️  A test window will appear in the middle of the screen, on top, about 5 times, ~2 s each."
echo "    It closes itself. Do not click anything; you do not need to."
echo

# ---------------------------------------------------------------------------
# RUN ONE COMBINATION, and WAIT FOR THE CONDITION rather than for a fixed time: the probe writes `probe done`
# when it has taken its measurement. A sleep would either be slower than needed or read a half-written log.
# ---------------------------------------------------------------------------
SUMMARY="$RUN/summary.txt"
fail=0

One() { # <tag> <material> <dark> <colorkey> [extra flags...]
  local tag="$1" material="$2" dark="$3" ck="$4"
  shift 4
  local log="$RUN/$tag.log" bmp="$RUN/$tag.bmp"
  # Absolute Windows spellings: these are handed to a native exe, and MSYS rewrites an argument that looks like a
  # unix path (the project has been bitten by that in the other direction before).
  local wlog wbmp
  wlog="$(apex_win_path "$log")"
  wbmp="$(apex_win_path "$bmp")"

  apex_kill_own '_mica_run' 'mica_probe'   # a leftover from a previous run is not a measurement
  rm -f "$log" "$bmp"

  # --gdi=1 in EVERY run: the magenta block is the capture's own control, and a capture that cannot see what this
  # process drew cannot be trusted about anything else either.
  ( cd "$RUN" && ./mica_probe.exe "--backdrop=$material" "--dark=$dark" "--colorkey=$ck" "--gdi=1" \
      "--out=$wbmp" "--log=$wlog" "$@" >/dev/null 2>&1 )

  if ! apex_wait_line "$log" '^probe done' 25; then
    echo "   FAIL: $tag never reported \`probe done\` (25 s)"
    apex_kill_own '_mica_run' 'mica_probe'
    fail=1
    return
  fi

  # ---- the hard facts of this run ----
  local line
  if ! grep -q 'gdi mark: #FF00FF' "$log"; then
    echo "   FAIL: $tag -- the capture did not see the block this process drew (nothing else it says counts)"
    grep -m1 '^gdi mark:' "$log"
    fail=1
  fi
  # ⚠️⚠️ THE ACTIVE WINDOW IS PART OF THE MEASUREMENT. Documented (Microsoft's Mica page): Mica "falls back to a
  # solid color ... when an app window on desktop deactivates". A run that measures the FALLBACK looks like a
  # working run with a weak effect -- which is exactly how this probe first reported "#F3F3F3, 1 colour" and the
  # material was read as "not obvious". It must be a FAILURE, not a footnote.
  if ! grep -q 'capture:.*foreground=1' "$log"; then
    echo "   FAIL: $tag -- the window was NOT the active one, so this measured Mica's solid FALLBACK colour"
    grep -m1 '^capture:' "$log"
    fail=1
  fi
  line=$(grep -m1 '^transparent background:' "$log")
  case "$line" in
    *0x00000000*) ;;
    *) echo "   FAIL: $tag -- the runtime did not accept a transparent background: $line"; fail=1 ;;
  esac
  if ! grep -q 'as drawn' "$log"; then
    echo "   FAIL: $tag -- the page's control card did not read back, so the page is not on top of the material"
    grep -m1 '^card:' "$log"
    fail=1
  fi
  line=$(grep -m1 '^band:' "$log")
  if [ -z "$line" ]; then
    echo "   FAIL: $tag -- no sampled band in the log"; fail=1; return
  fi

  # ---- and the numbers, into one machine-readable table ----
  # `band: n=.. mean=#.. rgb=R,G,B luma=.. min=.. max=.. distinct=..`
  local rgb luma distinct
  rgb=$(printf '%s' "$line" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^rgb=/) {sub(/^rgb=/,"",$i); print $i}}')
  luma=$(printf '%s' "$line" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^luma=/) {sub(/^luma=/,"",$i); print $i}}')
  distinct=$(printf '%s' "$line" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^distinct=/) {sub(/^distinct=/,"",$i); print $i}}')
  if [ -z "$rgb" ] || [ -z "$luma" ]; then
    echo "   FAIL: $tag -- the band line could not be read: $line"; fail=1; return
  fi
  printf '%s %s %s %s %s %s\n' "$tag" "$material" "$dark" "$rgb" "$luma" "$distinct" |
    tr ',' ' ' | awk -v ck="$ck" '{print $1, $2, $3, ck, $4, $5, $6, $7, $8}' >> "$SUMMARY"
}

# The combinations, and why each one is here. All of them are run with the window FORCED ACTIVE, because the
# material is only drawn then (see the check in One above).
#   * mica vs none        -- is the material on the screen at all? `none` is the control: with the colour key and
#                            no material the DESKTOP shows through, which is what proves the hole is real.
#   * light vs dark       -- does this window steer the material's variant? (The pinned-theme question.)
#   * mica vs micaalt vs acrylic -- WHICH ONE IS EVEN VISIBLE. Mica is subtle by design and the fallback is a
#                            flat colour; these three are what the user actually has to choose between.
#   * ck=0                -- the "we simply did not paint" case. It is kept because it is the tempting wrong
#                            answer: it produces a WHITE window that looks like a working one.
echo "== running the combinations =="
# `--report-only` re-reads the numbers from the last run: no windows appear, which is what you want when the
# question is about the REPORT and not about the screen.
if [ "${1:-}" = "--report-only" ]; then
  if [ ! -s "$SUMMARY" ]; then
    echo "   FAIL: $SUMMARY is empty -- run it for real first"
    exit 1
  fi
  echo "   (--report-only: re-reading the last run, nothing will appear on screen)"
else
# ⚠️ THE TABLE IS CLEARED HERE, NOT NEXT TO ITS DECLARATION. It was cleared at the top first, which meant
# `--report-only` wiped the very numbers it had been asked to re-read -- and then reported them as missing. (The
# file was left at 0 bytes, which is how this was found.)
: > "$SUMMARY"
One mica-dark-ck     mica     1 1
One micaalt-dark-ck  micaalt  1 1
One acrylic-dark-ck  acrylic  1 1
One mica-light-ck    mica     0 1
One none-dark-ck     none     1 1
One mica-plain       mica     1 0
fi
echo
echo "   (the GPU side of the cost is a separate instrument: powershell -File _diag/mica_gpu.ps1)"

# ---------------------------------------------------------------------------
# THE REPORT. Parsed by awk out of the table above rather than by reading the logs again: one reading, one
# implementation.
# ---------------------------------------------------------------------------
echo
awk '
  { t[NR]=$1; bd[NR]=$2; dk[NR]=$3; ck[NR]=$4; r[NR]=$5; g[NR]=$6; b[NR]=$7; l[NR]=$8; ds[NR]=$9; n=NR }
  function idx(tag,   i){ for(i=1;i<=n;i++) if(t[i]==tag) return i; return 0 }
  # The distance between two runs: the channel means plus the luma, added up. Crude on purpose -- the question is
  # "are these the same picture", not "by how much do they differ in a colour space".
  function dist(a,b,   i,j,d){
    i=idx(a); j=idx(b); if(!i||!j) return -1; d=0;
    d += (r[i]>r[j]? r[i]-r[j] : r[j]-r[i]);
    d += (g[i]>g[j]? g[i]-g[j] : g[j]-g[i]);
    d += (b[i]>b[j]? b[i]-b[j] : b[j]-b[i]);
    d += (l[i]>l[j]? l[i]-l[j] : l[j]-l[i]);
    return d
  }
  END {
    printf "== the sampled band (client area the page leaves transparent) ==\n"
    printf "   %-18s %-8s %-5s %-4s %10s %9s   %s\n", "run", "material", "dark", "key", "mean luma", "colours", "mean rgb"
    for (i=1; i<=n; i++)
      printf "   %-18s %-8s %-5s %-4s %10.1f %9d   %d,%d,%d\n", t[i], bd[i], dk[i], ck[i], l[i], ds[i], r[i], g[i], b[i]

    printf "\n== the questions ==\n"
    # ⚠️ WHAT IS **NOT** A VERDICT HERE, AND WHY. The obvious control -- "mica must differ from none, because with
    # no material you see the DESKTOP through the hole" -- is only as good as what happens to be behind the
    # window. Measured twice: with the wallpaper behind it `none` read 221 distinct colours, and with a dark
    # window behind it `none` read 30,30,32 -- within 8 of dark Mica, i.e. the control "failed" while the material
    # was being drawn perfectly. So the two HARD checks below are the ones that do not depend on the desktop:
    #
    #   (b) the key creates a hole at all: with the key the band is NOT the opaque white surface it is without it;
    #   (c) the thing in that hole is the MATERIAL and not just the desktop: the DESKTOP does not change when
    #       the dark flag of this window is flipped, and the material does. That is the fingerprint of the
    #       material, and it is the only check here that the desktop showing through cannot satisfy.
    p_plain = idx("mica-plain"); p_dark = idx("mica-dark-ck")
    holeOk = (p_plain && p_dark) ? (dist("mica-dark-ck","mica-plain") > 12) : 0
    printf "   b. does the colour key create a hole?  with key vs without (mica): distance %d -> %s\n",
      dist("mica-dark-ck","mica-plain"),
      (holeOk ? "YES: the band is no longer the opaque white surface of the window" \
              : "NO: the surface with the key is the same as without it")
    d2 = dist("mica-light-ck","mica-dark-ck")
    steerOk = (d2 > 12)
    printf "   c. is it MATERIAL and not just the desktop?  flipping only the dark flag of this window: distance %d -> %s\n",
      d2, (steerOk ? "MATERIAL: it follows this window, so it is the DWM material" \
                   : "NOT PROVEN: the material ignores this window (it follows the system)")
    d = dist("mica-dark-ck","none-dark-ck")
    printf "   (reported, NOT a verdict) mica vs none: distance %d -- this one depends on what is behind the window,\n", d
    printf "      and measured as low as 8 when a dark window happened to be there. Take the two hard checks\n"
    printf "      above, and read `none` only when that spot happens to show the wallpaper.\n"
    i = idx("mica-plain")
    if (i) printf "   d. without the colour key (\"we simply did not paint\"): luma %.1f, %d distinct colour(s) -> %s\n",
      l[i], ds[i],
      (ds[i] <= 1 && l[i] > 240 ? "an opaque WHITE surface -- the material is hidden, and the window looks fine" \
                                : "something else is on screen; measure again")
    # HOW VISIBLE IS IT, which is the question that decides whether any of this is worth doing. `colours` is the
    # tell: a flat surface is 1, wallpaper-tinted Mica is a few, and a material that actually shows texture is
    # dozens.
    printf "   e. how visible, and at what texture:\n"
    for (i = 1; i <= n; i++)
      if (bd[i] != "none")
        printf "        %-18s luma %6.1f   %3d distinct colour(s)   %d,%d,%d\n", t[i], l[i], ds[i], r[i], g[i], b[i]

    if (holeOk && steerOk) {
      print "\nOK: the key makes a real hole, and what fills it follows THIS window -- it is the DWM material"
      print "    (question e is the one that decides whether it is worth having: Mica is subtle BY DESIGN)"
    } else {
      print "\nFAILED: the material was not shown to be on the screen (see b and c above)"
      exit 1
    }
  }
' "$SUMMARY"
awkrc=$?
[ $awkrc -ne 0 ] && fail=1

echo
echo "== raw logs and pictures =="
for f in "$RUN"/*.log; do
  [ -f "$f" ] && printf '   %s\n' "$f"
done
for f in "$RUN"/*.bmp; do
  [ -f "$f" ] && printf '   %s   (open these to judge the LOOK -- the numbers only say "it is there")\n' "$f"
done
echo
if [ $fail -eq 0 ]; then
  echo "OK: every run reported, transparency was accepted, the page drew on top, and the material is visible"
else
  echo "FAILED: see the lines above"
fi
exit $fail
