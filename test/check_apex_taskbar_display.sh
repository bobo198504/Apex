#!/usr/bin/env bash
# Gate: taskbar layout is host-owned, one contribution is one text slot, and compact/side taskbars merge two lines.
set -uo pipefail
export PATH="/d/Projects/Code/_tools/w64devkit/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
HOST="$ROOT/apex/taskbar_display.cpp"
PROBE="$ROOT/build/_taskbar_display_probe.cpp"
EXE="$ROOT/build/_taskbar_display_probe.exe"
cleanup() { rm -f "$PROBE" "$EXE"; }
trap cleanup EXIT

echo "== syntax check with warnings promoted to errors =="
g++ -std=c++17 -O2 -mwindows -Wall -Wextra -Werror -DAPEX_BUILDING_HOST \
    -I"$ROOT/apex" -I"$ROOT/common" -fsyntax-only "$HOST" || exit 1

echo "== ownership =="
if grep -q 'ApexTaskbarDisplayWnd' "$HOST" &&
   grep -q 'feature.api->taskbarText' "$HOST" &&
   grep -q 'HTTRANSPARENT' "$HOST" &&
   grep -q 'WS_EX_TRANSPARENT' "$HOST" &&
   grep -q 'column.second' "$HOST" &&
   grep -q 'column.centred' "$HOST" &&
   grep -q 'StableWidth' "$HOST" &&
   grep -q 'narrowSince' "$HOST" &&
   grep -q 'ResetWidthMemory' "$HOST" &&
   grep -q 'g_windows\[side\]' "$HOST" &&
   grep -q 'EffectiveTaskbarSide(allItems\[i\].side, iconsLeftAligned) == side' "$HOST" &&
   grep -q 'rightAligned ?' "$HOST" &&
   grep -q 'ParseMarkedText' "$HOST" &&
   grep -q 'FillEllipse' "$HOST" &&
   grep -q 'BuildTextPath' "$HOST" &&
   grep -q 'graphics.FillPath(&brush, &path)' "$HOST" &&
   grep -q 'partAdvances' "$HOST" &&
   grep -q 'partInkWidths' "$HOST" &&
   grep -q 'TextAdvance' "$HOST" &&
   grep -q 'SpaceWidth' "$HOST" &&
   grep -q 'StringFormatFlagsMeasureTrailingSpaces' "$HOST" &&
   grep -q 'DotTextGap' "$HOST" &&
   grep -q 'dotBetweenText' "$HOST" &&
   grep -q 'dotCentreY' "$HOST" &&
   grep -q 'ColumnGap' "$HOST" &&
   grep -q '(naturalWidth + 1) & ~1' "$HOST" &&
   grep -q 'groupTop + (groupHeight - inkHeight)' "$HOST" &&
   grep -q 'SplitColumns' "$HOST" &&
   grep -q 'DrawTaskbarLine' "$HOST" &&
   grep -q 'TaskbarLineWidth' "$HOST" &&
   grep -q 'const int leftPad = side == 1 ? 0 : kPad' "$HOST" &&
   grep -q 'const int rightPad = side == 1 ? 0 : kPad' "$HOST" &&
   grep -q 'placementMargin = nearEnd ? 0 : 4' "$HOST" &&
   grep -q 'UiIsChinese() && chinese.GetLastStatus()' "$HOST" &&
   grep -q 'g_topmostInitialized\[side\] ? SWP_NOZORDER : 0' "$HOST" &&
   grep -q 'TaskbarIconsAreLeftAligned' "$HOST" &&
   grep -q 'EffectiveTaskbarSide' "$HOST" &&
   grep -q 'ForegroundIsFullscreen' "$HOST" &&
   grep -q 'QUNS_RUNNING_D3D_FULL_SCREEN' "$HOST" &&
   grep -q 'kFullscreenTimerId' "$HOST" &&
   grep -q 'taskbarHideFullscreen' "$HOST" &&
   grep -q 'geometry.display.vertical && !nearEnd' "$HOST" &&
   grep -q 'placed = axisStart' "$HOST"; then
  echo "  the host draws all feature text, stays click-through, and pins the vertical top choice to the top"
else
  echo "FAIL: the host-owned taskbar strip is incomplete"
  exit 1
fi

if [ "$(grep -c 'ResetWidthMemory();' "$HOST")" -lt 3 ]; then
  echo "FAIL: the font-change path does not invalidate widths measured with the old font"
  exit 1
fi

if grep -q 'else if (bucket < memory.width)' "$HOST"; then
  echo "FAIL: taskbar widths still shrink while values fluctuate, moving every later item"
  exit 1
fi

cat > "$PROBE" <<'EOF'
#include "common/taskbar_layout.h"
#include <cstdio>

int main()
{
  using namespace apex::taskbar;
  const Rect monitor = {0, 0, 1920, 1080};
  const Display compact = ChooseDisplay(Rect{0, 1040, 1920, 1080}, monitor, 12);
  const Display roomy = ChooseDisplay(Rect{0, 1032, 1920, 1080}, monitor, 12);
  const Display vertical = ChooseDisplay(Rect{0, 0, 48, 1080}, monitor, 12);
  if (LinesForItem(1, roomy) != 1) return 2;
  if (LinesForItem(2, compact) != 1) return 3;
  if (LinesForItem(2, roomy) != 2) return 4;
  if (LinesForItem(2, vertical) != 1) return 5;
  if (!PairSingleLineSlots(roomy) || PairSingleLineSlots(compact)) return 6;
  if (!LastSingleLineSlotIsCentred(5) || LastSingleLineSlotIsCentred(4) ||
      LastSingleLineSlotIsCentred(0)) return 7;
  if (EffectiveTaskbarSide(0, true) != 1 || EffectiveTaskbarSide(0, false) != 0 ||
      EffectiveTaskbarSide(1, false) != 1) return 9;
  const int horizontalGap = ItemGap(false, 12);
  const int verticalGap = ItemGap(true, 12);
  if (verticalGap <= horizontalGap) return 8;
  if (ItemGap(false, 9) >= ItemGap(false, 12) ||
      ItemGap(true, 9) >= ItemGap(true, 12)) return 10;
  std::printf("  single-line stays single; two-line merges when compact or vertical\n");
  std::printf("  adjacent one-line slots pair into two rows; an odd leftover is centred\n");
  std::printf("  vertical spacing %d px is wider than horizontal spacing %d px\n",
              verticalGap, horizontalGap);
  std::printf("  both long-axis gaps shrink when the shared font gets smaller\n");
  std::printf("  left-aligned system icons map every left/top slot to the near end\n");
  return 0;
}
EOF

echo "== text-line and spacing rules =="
g++ -std=c++17 -O2 -mconsole -Wall -Wextra -Werror \
    -I"$ROOT" -o "$EXE" "$PROBE" || exit 1
"$EXE" || exit 1

echo
echo "OK: host-owned taskbar layout, automatic compact merging, and wider side-taskbar spacing"
