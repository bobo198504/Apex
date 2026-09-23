#!/usr/bin/env bash
# Gate: THE PANEL'S COLOURS -- ONE PALETTE PER APPEARANCE, AND THE MEASUREMENTS THAT SAY SO.
#
# WHY THIS EXISTS. The user's report was "主界面浅色UI，颜色饱和度改低些，让它看起来再白淡些，现在太黄了", and the
# cause was not a wrong colour: it was TWO light themes. The light theme's two surfaces were swapped at an
# earlier request, and the swap was written into `:root[data-theme="light"]` ONLY -- while plain `:root`, which
# an AUTO light appearance falls through to, kept the old saturated cream. So a pinned light theme and an auto
# light theme rendered differently, and only one of them had been corrected.
#
# A PALETTE CANNOT BE TESTED BY EYE HERE (this project's UI testing belongs to the user), but every property
# that made it wrong CAN be measured, and each one is a way it could break again:
#
#   1. THE TWO PATHS AGREE. `:root` and `:root[data-theme="light"]` must declare the same light values. This is
#      the bug above, stated as a rule.
#   2. THE SURFACES ARE ORDERED. A card is a recessed area of the light page: panel darker than bg. (In dark
#      mode it is the other way round and must stay that way -- there, lifting a card is what makes it
#      visible.) This is the property that a careless swap destroys.
#   3. THE TEXT STILL READS. Contrast is computed (WCAG ratio) rather than eyeballed: lowering saturation is
#      exactly the change that can make grey-on-cream unreadable, and it is the failure a palette change is
#      most likely to introduce.
#   4. THE SATURATION ACTUALLY WENT DOWN, compared with the values the user rejected -- otherwise the request
#      was not carried out, however nice the new numbers look.
#
# It reads apex/ui/panel.html and computes. No process is started; this is a sub-second gate.
#
# ⚠️ THE COMPUTATION LIVES IN _diag/apex_palette_probe.js, NOT IN A `node -e '...'` HERE. It was inline first,
# and an apostrophe in an ordinary comment closed the shell's single-quoted string and made bash try to run the
# rest of it as commands. A file has no such interaction with the shell at all.
#
# usage: test/check_apex_palette.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
PAGE="$ROOT/build/panel.built.html"
PROBE="$ROOT/_diag/apex_palette_probe.js"

[ -f "$PAGE" ]  || { echo "FAIL: $PAGE is missing"; exit 1; }
[ -f "$PROBE" ] || { echo "FAIL: $PROBE is missing"; exit 1; }

node "$PROBE" "$PAGE" "$ROOT/apex/icons.h"
