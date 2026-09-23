#!/usr/bin/env bash
# Deploy Apex to the folder it is run from: D:\App protable\Apex by default.
#
# ⚠️ THIS IS A BATCH OPERATION, NOT PART OF THE EDIT LOOP. The user's rule: "一次性开发多个功能，不用每个小功能都
# 部署一遍 ... 太浪费资源时间". So this is run ONCE after a batch of work is finished and the gates pass -- not
# after each change. Working in build/apex/ and testing there is the fast loop; this is the delivery step.
#
# WHAT IT DOES, IN THIS ORDER, AND THE ORDER IS THE POINT:
#
#   1. BUILDS A RELEASE BUILD (APEX_RELEASE=1). Development-only features (.dev-only, e.g. AuditStub) are
#      skipped, and the previous release's copies of them are REMOVED from the output -- see the note in
#      apex/build.sh: skipping alone is not enough, because the folder is what gets copied.
#   2. TAKES THE FIELD. Every running Apex is stopped, including the user's -- authorised explicitly:
#      "部署时，必须清掉当前项目在运行的进程，不管是不是我在运行；进程只能有一个Apex.exe".
#      The identity is the WINDOW CLASS, not the image name (see _diag/apex_owners.cpp for why the name is
#      not good enough to kill by).
#   3. COPIES THE PROGRAM. Only the things that ARE the program: the two exes, WebView2, and Plugins/.
#      ⚠️ NOT the logs, and not apex.ini -- these are the deployed copy's own state, and a deploy that
#      overwrites the user's settings is how a fix arrives as a surprise. Nothing else in the target is
#      touched, so anything else living there stays.
#   4. STARTS THE HOST, so the deployed copy is the one running rather than nothing at all.
#
# ⚠️ THERE IS NO ROLLBACK, AND THERE IS NO "PREVIOUS VERSION" FOLDER. The gates are the safety net: a deploy
# is expected to follow a green run (test/run_all.sh), which is where "this build works" was established.
# 
# ⚠️ --gated IS THE ONE-CYCLE DELIVERY, and it exists because the user asked for exactly that: "部署直接停掉
# 正在运行的实例，一次成型". Without it, a delivery stops their Apex TWICE -- once for the assembly gates
# (which must have the field to themselves, because they inject real wheel input) and once for the copy (Windows
# will not let a running image be overwritten). With it: build, run the gates with APEX_KEEP_FIELD=1 (the field
# goes down and STAYS down when they pass), then copy and start -- one stop, one start.
#
# ⚠️ AND A RED GATE PUTS THINGS BACK. test/run_all.sh gives the field back on every failing path, so a delivery
# that cannot proceed leaves the user with the program they already had rather than with nothing.
#
# usage: bash apex/deploy.sh [--gated] [--full] [target-dir]
#        (default: /d/App protable/Apex)
#        --gated  run the gates first, in one stop/start cycle
#        --full   run the WHOLE assembly layer even when the change looks local (see step 1a)
set -uo pipefail

TOOLS="${TOOLS:-/d/Projects/Code/_tools}"
export PATH="$TOOLS/w64devkit/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
. "$ROOT/test/lib_procs.sh"   # apex_all_pids / apex_take_the_field -- and the note on why by window class

OUT="$ROOT/build/apex"
GATED=0
FULL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --gated) GATED=1; shift ;;
    --full)  FULL=1;  shift ;;
    *) break ;;
  esac
done
TARGET="${1:-/d/App protable/Apex}"

# ⚠️ THE TARGET MUST BE THE APEX FOLDER, NOT A PARENT OF IT. This project was once given "D:\App protable"
# and a loose filter there killed 36 of the user's portable tools; the same mistake in a copy is how a deploy
# would scatter files across every tool the user owns.
#
# An empty folder, or one that does not exist, is created and used. A folder that already has files is accepted
# only if it looks like an Apex installation -- and "looks like" cannot be just `apex.exe`, because the whole
# point of the deploy being idempotent is that it can run against a folder whose program is not there yet. The
# markers are the things only this program has: its window-side WebView2 folder, or its own Plugins folder.
# A directory of somebody else's tools has neither.
if [ -d "$TARGET" ] && [ -n "$(ls -A "$TARGET" 2>/dev/null)" ]; then
  if [ ! -f "$TARGET/apex.exe" ] && [ ! -d "$TARGET/Plugins" ] && [ ! -d "$TARGET/WebView2" ]; then
    echo "REFUSING: $TARGET exists and does not look like an Apex folder."
    echo "          (no apex.exe, no Plugins/, no WebView2/ -- so it is somebody else's directory)"
    echo "          Pass the Apex folder itself, e.g. bash apex/deploy.sh \"/d/App protable/Apex\""
    exit 1
  fi
fi

# The build output must be a RELEASE build. Checked rather than assumed: a .dev-only feature sitting in the
# output means the flags were wrong, and copying it would ship it.
echo "== 1. release build =="
APEX_RELEASE=1 bash "$ROOT/apex/build.sh" >/dev/null || { echo "   FAIL: the build failed"; exit 1; }

# ---------------------------------------------------------------------------
# WHAT ACTUALLY CHANGED SINCE THE LAST DELIVERY -- and therefore what has to be verified and copied.
#
# ⚠️ WHY THIS EXISTS (the user's report): "我实测跑了另一个项目的部署，整个过程2分钟不到，过程没有多余的运行测试
# 干扰用户 … 而本项目才3个插件…部署却花了5分钟". Measured here: one `--gated` delivery took 257 s and started
# 20 hosts, because the ASSEMBLY LAYER ran in full whatever had changed -- nine gates that each start a whole
# private copy of the program. Two of those gates are about the wheel, two about REAPER, one about the panel's
# first frame; a change to `panel.html` cannot affect any of the wheel ones.
#
# ⚠️ THE DELTA IS COMPUTED FROM THE ARTEFACTS, NOT FROM SOURCE TIMESTAMPS, and that is the honest source: it is
# the same comparison the copy step wants, it needs no state file to drift, and "the bytes in build/ are not the
# bytes in the install" is exactly what a delivery is. It also gives the Lertaro rule for free -- only the files
# that differ are copied (see step 3).
#
# ⚠️ WHEN IN DOUBT, EVERYTHING RUNS. `apex.exe` contains the host -- the hook, the tray, the IPC server, the
# config writing -- and a feature DLL changes what the host decides about the wheel, so either of those means
# the full assembly layer. Only ONE case is narrow: nothing but the panel changed.
# ---------------------------------------------------------------------------
ChangedArtefacts() {
  local rel f d id
  for rel in apex.exe apex-settings.exe WebView2Loader.dll; do
    if [ ! -f "$TARGET/$rel" ] || ! cmp -s "$OUT/$rel" "$TARGET/$rel"; then echo "$rel"; fi
  done
  for d in "$OUT"/Plugins/*/; do
    [ -d "$d" ] || continue
    id="$(basename "$d")"
    for f in "$d"*.dll; do
      [ -f "$f" ] || continue
      rel="Plugins/$id/$(basename "$f")"
      if [ ! -f "$TARGET/$rel" ] || ! cmp -s "$f" "$TARGET/$rel"; then echo "$rel"; fi
    done
  done
}

CHANGED="$(ChangedArtefacts)"
# `--full` forces the whole suite (a release-shaped delivery, or when a change is not obviously local).
if [ "$FULL" -eq 1 ]; then
  GATE_SET=all
elif [ -z "$CHANGED" ]; then
  GATE_SET=none
elif [ "$CHANGED" = "apex-settings.exe" ]; then
  GATE_SET=panel
else
  GATE_SET=all
fi

echo
echo "== 1a. what changed since the last delivery =="
if [ -z "$CHANGED" ]; then
  echo "   nothing -- the built program is byte-for-byte what is already deployed"
else
  echo "$CHANGED" | sed 's/^/   /'
fi
case "$GATE_SET" in
  none)  echo "   -> nothing to verify or copy" ;;
  panel) echo "   -> only the panel changed: the panel gates${GATED:+, and only those, run}" ;;
  *)     echo "   -> the host or a feature changed: the whole assembly layer runs" ;;
esac
if [ "$GATED" -eq 0 ]; then
  echo "   (no --gated: this run only copies; whatever the gates did was decided by the caller)"
fi

# ⚠️ THE GATES COME BEFORE THE COPY AND TAKE THE FIELD WITH THEM (see --gated above). Their output is shown as it
# runs -- a delivery that hides the one thing that can stop it is a delivery nobody can trust.
if [ "$GATED" -eq 1 ]; then
  echo "== 1b. the gates, with the field taken for the whole delivery =="
  if ! APEX_KEEP_FIELD=1 bash "$ROOT/test/run_all.sh" --assembly --set "$GATE_SET"; then
    echo "   FAIL: the gates did not pass -- nothing was copied, and the running Apex was put back"
    exit 1
  fi
  # ⚠️ THE RELEASE BUILD IS REPEATED HERE, AND THAT IS NOT PARANOIA. Several gates build the program to test it
  # (`check_apex_modular` compiles a feature against the host headers, the chart and IME gates build their own
  # probes), and a plain `apex/build.sh` puts the DEVELOPMENT-ONLY feature back into the output. The check below
  # caught exactly that on the first run of this mode: the gates were green, the copy then refused to ship an
  # AuditStub.dll, and -- because the field had already been taken by the gates -- the user's Apex was left down.
  # So: the release build is the LAST thing before the copy, always, and it is incremental (about a second).
  echo "== 1c. release build again (the gates rebuild non-release artefacts) =="
  if ! APEX_RELEASE=1 bash "$ROOT/apex/build.sh" >/dev/null; then
    echo "   FAIL: the release rebuild failed -- restarting the deployed copy unchanged"
    # start it through WMI (apex_start_detached in test/lib_procs.sh)
    apex_start_detached "$TARGET/apex.exe"
    exit 1
  fi
fi
if [ -d "$OUT/Plugins/AuditStub" ]; then
  echo "   FAIL: a development-only feature is still in the output after a release build"
  exit 1
fi
echo "   ok  $(ls "$OUT"/Plugins | tr '\n' ' ')"

# Nothing changed: there is no copy to make and no process to restart. Said out loud rather than quietly
# re-copying identical files and starting a second instance the single-instance rule would only ignore.
# ⚠️ `${FILES_ONLY:-0}`: this check sits BEFORE the line that defines it, and `set -u` makes reading it early a
# hard error -- which it did, on the first run of this path (the deploy died with "unbound variable" instead of
# saying there was nothing to do, and left a confusing exit code behind).
if [ "$GATE_SET" = "none" ] && [ "${FILES_ONLY:-0}" != "1" ]; then
  echo
  echo "NOTHING TO DELIVER: the built program is identical to $TARGET"
  exit 0
fi

# ⚠️⚠️ `APEX_DEPLOY_FILES_ONLY=1` SKIPS THE TWO STAGES THAT TOUCH RUNNING PROCESSES (2 and 4).
#
# Why it has to exist: `test/check_apex_deploy.sh` exercises this script's file semantics -- what is copied, what
# is replaced, what is left alone -- and those are pure filesystem facts that need no running program. Without a
# switch, running the gate meant running the WHOLE script: it took the field (stopping the user's live Apex) and
# then started ITS OWN copy from a throwaway folder. That is exactly what happened, and the user's deployment
# went down because a BUILD-LAYER gate -- one that promises "no window, no process, no input device" -- ran the
# real thing.
#
# The gate is the caller that sets this. A real deploy never does.
FILES_ONLY="${APEX_DEPLOY_FILES_ONLY:-0}"

if [ "$FILES_ONLY" != "1" ]; then
  echo
  if [ "$GATED" -eq 1 ]; then
    # The gates took it and (because they passed) kept it down. Taking it again would be the second blink this
    # whole mode exists to remove.
    echo "== 2. taking the field -- already down (the gates took it and kept it)"
  else
    echo "== 2. taking the field =="
    # apex_take_the_field records what was running so it can be given back; a DEPLOY does not give it back -- the
    # deployed copy is what should be up at the end. The recording is discarded deliberately.
    apex_take_the_field
    REST=$(apex_all_pids)
    if [ -n "$REST" ]; then
      echo "   FAIL: something is still running Apex: [$(echo $REST | tr '\n' ' ')]"
      exit 1
    fi
    echo "   ok  nothing is running"
  fi
else
  echo
  echo "== 2. taking the field -- SKIPPED (APEX_DEPLOY_FILES_ONLY=1: a caller checking the file semantics) =="
fi

echo
echo "== 3. copying the program to $TARGET =="
mkdir -p "$TARGET"

# ⚠️ ONLY THE FILES THAT DIFFER ARE COPIED -- the Lertaro rule, from the same profile that made the point:
# "只覆盖真正变化的产物 … 覆盖面越窄，需要解锁的文件越少，失败面越小". The list was computed in step 1a from the
# same byte comparison, so it is not a second opinion: it IS what "changed" means for this delivery.
#
# The two executables first, one at a time, so a failure names which one.
for exe in apex.exe apex-settings.exe; do
  if [ ! -f "$OUT/$exe" ]; then
    echo "   FAIL: $OUT/$exe is missing (the build did not produce it)"
    exit 1
  fi
  case "$CHANGED" in
    *"$exe"*)
      # ⚠️ A RUNNING EXE CANNOT BE OVERWRITTEN ON WINDOWS, and the field is clear, so a failure here means
      # something else holds the file -- worth naming rather than leaving as a bare "cp failed".
      if ! cp -f "$OUT/$exe" "$TARGET/$exe"; then
        echo "   FAIL: could not replace $TARGET/$exe (is something holding it open?)"
        exit 1
      fi
      echo "   $exe" ;;
    *) echo "   $exe  (unchanged, left alone)" ;;
  esac
done

# WebView2Loader.dll, and the WebView2 folder beside it. Required by the panel; without it the panel cannot
# start at all, and the failure looks like "the panel does not open" with no explanation.
case "$CHANGED" in
  *WebView2Loader.dll*)
    cp -f "$OUT/WebView2Loader.dll" "$TARGET/" 2>/dev/null && echo "   WebView2Loader.dll" ;;
  *) echo "   WebView2Loader.dll  (unchanged, left alone)" ;;
esac
if [ -d "$OUT/WebView2" ] && [ ! -d "$TARGET/WebView2" ]; then
  cp -r "$OUT/WebView2" "$TARGET/WebView2" && echo "   WebView2/ (was not there)"
fi

# ⚠️⚠️ THE PROGRAM IS REPLACED; THE USER'S DATA IS NOT. THIS DISTINCTION WAS LEARNED THE HARD WAY.
#
# The first version of this step was `rm -rf "$TARGET/Plugins"` followed by a copy of the new DLLs -- which is
# correct about RETIRED FEATURES (a feature removed from the tree must disappear from the deployed folder, or a
# merged copy keeps running while the source says it is gone) and catastrophically wrong about everything else
# that lives in there. It deleted the user's nine AutoIME rules (Plugins/AutoIME/config.json), their dark-theme
# choice for SmoothWheel (SmoothWheel.ini) and the capture history, in one silent step, on a deploy whose whole
# message was "nothing of yours is touched". The rule below had already been written down two paragraphs further
# on -- "config.json belongs to the USER and is never written by a deploy" -- and the `rm -rf` above it ignored
# it. A GENERAL CLEANUP IS NOT A SAFE WAY TO REMOVE A SPECIFIC THING.
#
# So the removal is now per-file and per-folder, and it knows what a program file is:
#
#   * a FEATURE FOLDER that the release no longer builds is removed entirely -- that is the retired-feature
#     case, and there is no user data in a folder whose feature is gone;
#   * inside a folder that IS built, only the program's own files are replaced: the `.dll` and the packed
#     `.ini` the build ships next to it. Everything else -- config.json, observed.json, anything a feature or
#     the user keeps there -- is left exactly as it was.
mkdir -p "$TARGET/Plugins"
for d in "$OUT"/Plugins/*/; do
  [ -d "$d" ] || continue
  id="$(basename "$d")"
  mkdir -p "$TARGET/Plugins/$id"
  # Same rule one level down: a feature whose DLL is byte-identical to the deployed one is left alone (its
  # folder may hold the user's config.json right next to it, and not touching it at all is the safest form of
  # "not touching the user's data").
  for f in "$d"*.dll; do
    [ -f "$f" ] || continue
    if [ ! -f "$TARGET/Plugins/$id/$(basename "$f")" ] || ! cmp -s "$f" "$TARGET/Plugins/$id/$(basename "$f")"; then
      cp -f "$f" "$TARGET/Plugins/$id/"
      echo "   Plugins/$id/$(basename "$f")"
    else
      echo "   Plugins/$id/$(basename "$f")  (unchanged, left alone)"
    fi
  done
done
# ... and now the other direction: a folder in the target that this release does not build is a retired feature.
for t in "$TARGET"/Plugins/*/; do
  [ -d "$t" ] || continue
  id="$(basename "$t")"
  if [ ! -d "$OUT/Plugins/$id" ]; then
    rm -rf "$t"
    echo "   removed Plugins/$id/ (not in this release)"
  fi
done

# ⚠️ SETTINGS ARE SEEDED ONLY IF ABSENT, AND A FEATURE'S OWN FILES ARE IN THE SAME CATEGORY. apex.ini is the
# deployed copy's own state (theme, language) and a deploy must not overwrite the user's choice; the same goes
# for a feature's ini, and for anything else a feature keeps beside itself -- see the note above about what the
# first version of this script deleted.
if [ ! -f "$TARGET/apex.ini" ]; then
  if [ -f "$OUT/apex.ini" ]; then
    cp -f "$OUT/apex.ini" "$TARGET/apex.ini"
    echo "   apex.ini (seeded -- none was there)"
  fi
else
  echo "   apex.ini kept (the deployed copy's own settings)"
fi
for d in "$OUT"/Plugins/*/; do
  [ -d "$d" ] || continue
  id="$(basename "$d")"
  src="$d$id.ini"
  dst="$TARGET/Plugins/$id/$id.ini"
  if [ -f "$src" ] && [ ! -f "$dst" ]; then
    cp -f "$src" "$dst"
    echo "   Plugins/$id/$id.ini (seeded)"
  fi
done

if [ "$FILES_ONLY" != "1" ]; then
  echo
  echo "== 4. starting the deployed copy =="
  # start it through WMI (apex_start_detached in test/lib_procs.sh)
  apex_start_detached "$TARGET/apex.exe"
  tries=0
  while [ $tries -lt 30 ]; do
    [ -n "$(apex_all_pids)" ] && break
    sleep 0.2
    tries=$((tries + 1))
  done
  RUNNING=$(apex_all_pids | head -1)
  if [ -z "$RUNNING" ]; then
    echo "   FAIL: the deployed host did not come up"
    exit 1
  fi
  echo "   up (pid $RUNNING) from $TARGET"
else
  echo
  echo "== 4. starting the deployed copy -- SKIPPED (APEX_DEPLOY_FILES_ONLY=1) =="
fi

echo
echo "DEPLOYED: $TARGET"
echo "  features: $(ls "$TARGET"/Plugins 2>/dev/null | tr '\n' ' ')"
echo "  (the tray icon is where its window is -- the host window itself is hidden by design)"
