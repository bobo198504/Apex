# Gate: the model in shared/ is still the same model the plugin has.
#
# THIS IS THE ONE GATE THAT WATCHES THE PROJECT SEAM, and it is here because of a decision with a real cost:
# until 2026-09-18 this project lived inside the plugin's repository (as app/), so "the same model" meant the
# same BYTES and drift was impossible. Two separate projects cannot do that, so shared/ holds a COPY, and a
# change on either side is now something a person has to remember.
#
# The model is what makes the plugin and Apex feel identical, which is the one promise the pair makes. If the
# two ever disagree, that promise is silently broken: both still build, both still run, and only the feel
# changes -- which is the hardest kind of difference to notice and the most annoying to debug.
#
# WHAT IT DOES: compares this project's copies against the plugin's originals, by md5. The plugin project is
# found at a KNOWN DEFAULT PATH (the two live side by side under D:\Projects\Code) and can be pointed
# elsewhere with PLUGIN_ROOT.
#
# WHAT IT DOES WHEN THE PLUGIN IS NOT THERE: reports it as SKIPPED and passes. That is deliberate -- someone
# who clones only this project should be able to run the gates -- but the line is printed loudly, because a
# skip that looks like a pass is how a check quietly stops happening.
#
# usage: test/check_model_sync.sh
#        PLUGIN_ROOT=/path/to/plugin test/check_model_sync.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"

PLUGIN_ROOT="${PLUGIN_ROOT:-/d/Projects/Code/SmoothWheelScroll for reaper}"

# The three headers this project copies. device.h is NOT among them: it is the plugin's own touchpad/device
# tracking and no Apex code uses it (the host reads modifier state from the OS hook instead).
FILES=(model.h anim3_core.h anim161_core.h)

echo "model sync"
echo "  this project : $ROOT/shared"
echo "  plugin       : $PLUGIN_ROOT/src"
echo

if [ ! -d "$PLUGIN_ROOT/src" ]; then
  echo "  SKIPPED: the plugin project was not found at that path."
  echo "  This is not a failure -- but it does mean the model was NOT compared. To check it:"
  echo "    PLUGIN_ROOT=/path/to/plugin test/check_model_sync.sh"
  exit 0
fi

fail=0
for f in "${FILES[@]}"; do
  mine="$ROOT/shared/$f"
  theirs="$PLUGIN_ROOT/src/$f"
  if [ ! -f "$mine" ]; then
    printf "  %-18s %s\n" "$f" "MISSING here (the copy was deleted?)"
    fail=1
    continue
  fi
  if [ ! -f "$theirs" ]; then
    printf "  %-18s %s\n" "$f" "MISSING in the plugin (was it renamed?)"
    fail=1
    continue
  fi
  a=$(md5sum "$mine" | cut -d' ' -f1)
  b=$(md5sum "$theirs" | cut -d' ' -f1)
  if [ "$a" = "$b" ]; then
    printf "  %-18s %s  %s\n" "$f" "same" "$a"
  else
    printf "  %-18s %s\n" "$f" "DIFFERENT"
    printf "      here:   %s\n" "$a"
    printf "      plugin: %s\n" "$b"
    fail=1
  fi
done

echo
if [ "$fail" -eq 0 ]; then
  echo "OK: the model is identical in both projects"
else
  cat <<'EOF'
FAILED: the two projects have different models, so the plugin and Apex will not feel alike.

  A model change belongs in the PLUGIN first -- that is the side with the gates that verify the model's
  behaviour (check_anim3 / check_conservation / check_travel). Then:

    1. copy the changed header(s) into shared/
    2. update the table in shared/SOURCE.md (bytes, md5) -- a stale fingerprint is worse than none,
       because it says "checked" when nobody did
    3. run test/run_all.sh here, including check_app_core, which asserts conservation

  If the difference is INTENTIONAL and belongs to this side only, that is a bigger decision than it looks:
  the shared model is what keeps the two products feeling the same, and forking it is how the sibling WPF
  project ended up stuck several versions behind. Do it deliberately, and write down why in SOURCE.md.
EOF
fi
exit $fail
