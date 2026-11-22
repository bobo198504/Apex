#ifndef APEX_COMMON_TASKBAR_LAYOUT_H
#define APEX_COMMON_TASKBAR_LAYOUT_H

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// Geometry shared by the host's settings snapshot and a feature that draws into the taskbar. This header has no
// Windows dependency: the callers supply rectangles and the one rule that says how much room a line of text needs.

namespace apex {
namespace taskbar {

struct Rect
{
  long left;
  long top;
  long right;
  long bottom;
};

enum class Edge
{
  Top,
  Bottom,
  Left,
  Right,
};

struct Display
{
  Edge edge;
  bool vertical;
  bool singleLine;
  int lineCount;
};

struct TwoLineRows
{
  int lineHeight;
  int titleTop;
  int balanceTop;
};

struct Span
{
  long start;
  long end;
};

inline long Width(const Rect &r) { return r.right - r.left; }
inline long Height(const Rect &r) { return r.bottom - r.top; }

inline long Distance(long a, long b) { return a > b ? a - b : b - a; }

inline Edge ClassifyEdge(const Rect &taskbar, const Rect &monitor)
{
  if (Width(taskbar) >= Height(taskbar))
    return Distance(taskbar.top, monitor.top) <= Distance(taskbar.bottom, monitor.bottom) ? Edge::Top
                                                                                           : Edge::Bottom;
  return Distance(taskbar.left, monitor.left) <= Distance(taskbar.right, monitor.right) ? Edge::Left
                                                                                         : Edge::Right;
}

inline bool IsVertical(Edge edge)
{
  return edge == Edge::Left || edge == Edge::Right;
}

inline const char *EdgeName(Edge edge)
{
  switch (edge)
  {
  case Edge::Top:
    return "top";
  case Edge::Left:
    return "left";
  case Edge::Right:
    return "right";
  case Edge::Bottom:
  default:
    return "bottom";
  }
}

inline int Thickness(const Rect &taskbar, Edge edge)
{
  return (int)(IsVertical(edge) ? Width(taskbar) : Height(taskbar));
}

// A vertical taskbar always gets one rotated line. A horizontal taskbar keeps the title above the balance while
// there is room for two comfortably spaced rows; the Windows compact taskbar no longer qualifies, so drawing two
// chopped rows would satisfy "fits" while making both unreadable.
inline bool PreferSingleLine(int taskbarThickness, int fontPixels)
{
  const int readableFont = fontPixels < 12 ? 12 : fontPixels;
  return taskbarThickness <= readableFont * 2 + 16;
}

inline Display ChooseDisplay(const Rect &taskbar, const Rect &monitor, int fontPixels)
{
  Display d = {};
  d.edge = ClassifyEdge(taskbar, monitor);
  d.vertical = IsVertical(d.edge);
  d.singleLine = d.vertical || PreferSingleLine(Thickness(taskbar, d.edge), fontPixels);
  d.lineCount = d.singleLine ? 1 : 2;
  return d;
}

// A feature declares a preference, not a promise: one-line blocks stay one line, and a two-line block becomes one
// line whenever the taskbar itself cannot comfortably show two.
inline int LinesForItem(int declaredLines, const Display &display)
{
  return declaredLines >= 2 && !display.singleLine ? 2 : 1;
}

// On a normal-height taskbar, adjacent one-line slots are stacked into the two rows. An odd count leaves one
// unused half-row; that last slot spans the full height and is vertically centred rather than being forced into
// either row.
inline bool PairSingleLineSlots(const Display &display)
{
  return display.lineCount == 2;
}

inline bool LastSingleLineSlotIsCentred(int singleLineSlots)
{
  return singleLineSlots > 0 && (singleLineSlots & 1) != 0;
}

// The gap between separate contributions on the taskbar's long axis. Vertical taskbars get more air: rotated
// text is read a block at a time, and a compact horizontal gap turns into a very tight break there.
inline int ItemGap(bool vertical, int fontPixels)
{
  const int font = fontPixels > 0 ? fontPixels : 12;
  return vertical ? std::max(6, (font * 3) / 4) : std::max(3, font / 2);
}

inline int EffectiveTaskbarSide(int storedPosition, bool iconsLeftAligned)
{
  return iconsLeftAligned ? 1 : (storedPosition ? 1 : 0);
}

inline bool UseNearEnd(int storedPosition, bool, bool iconsLeftAligned)
{
  return EffectiveTaskbarSide(storedPosition, iconsLeftAligned) != 0;
}

// The two words are one reading unit, not two equal halves of the taskbar. Centring each word in height/2 made
// the gap grow with the taskbar and pushed the lower line against its edge. Keep each row one font-height plus a
// little breathing room, then centre that small pair in the available box.
inline TwoLineRows CompactTwoLineRows(int height, int fontPixels)
{
  TwoLineRows rows = {};
  rows.lineHeight = fontPixels + 4;
  if (rows.lineHeight < 12)
    rows.lineHeight = 12;
  const int total = rows.lineHeight * 2;
  rows.titleTop = total < height ? (height - total) / 2 : 0;
  rows.balanceTop = rows.titleTop + rows.lineHeight;
  if (rows.balanceTop + rows.lineHeight > height)
  {
    rows.lineHeight = height > 1 ? height / 2 : 1;
    rows.titleTop = 0;
    rows.balanceTop = rows.lineHeight;
  }
  return rows;
}

inline int FontCap(int taskbarThickness, int lineCount)
{
  const int lines = lineCount < 1 ? 1 : lineCount;
  const int cap = lines == 1 ? taskbarThickness - 8 : (taskbarThickness - 6) / lines - 2;
  return cap > 0 ? cap : 0;
}

inline bool ParseFontSize(const char *text, int *out)
{
  if (!text || !out)
    return false;
  while (*text == ' ' || *text == '\t')
    ++text;
  char *end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text)
    return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    ++end;
  if (*end || value < 9 || value > 16)
    return false;
  *out = (int)value;
  return true;
}

inline bool NormaliseColor(const char *text, char *out, int outSize)
{
  if (!text || !out || outSize < 8)
    return false;
  while (*text == ' ' || *text == '\t')
    ++text;
  const size_t length = std::strlen(text);
  size_t end = length;
  while (end && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                 text[end - 1] == '\n'))
    --end;
  if (end == 4 && std::tolower((unsigned char)text[0]) == 'a' &&
      std::tolower((unsigned char)text[1]) == 'u' &&
      std::tolower((unsigned char)text[2]) == 't' &&
      std::tolower((unsigned char)text[3]) == 'o')
  {
    std::snprintf(out, outSize, "auto");
    return true;
  }
  if (end != 7 || text[0] != '#')
    return false;
  for (size_t i = 1; i < end; ++i)
    if (!std::isxdigit((unsigned char)text[i]))
      return false;
  std::snprintf(out, outSize, "#%c%c%c%c%c%c",
                (char)std::toupper((unsigned char)text[1]),
                (char)std::toupper((unsigned char)text[2]),
                (char)std::toupper((unsigned char)text[3]),
                (char)std::toupper((unsigned char)text[4]),
                (char)std::toupper((unsigned char)text[5]),
                (char)std::toupper((unsigned char)text[6]));
  return true;
}

// Find a gap for one overlay along the taskbar's long axis. `blocked` is an untrusted, unsorted list of spans
// belonging to other taskbar tools. The answer stays inside [start+margin, end-margin]; when no gap is large
// enough it returns -1 and the caller keeps its current position instead of jumping to the opposite side.
inline long PlaceSpan(long start, long end, long spanSize, const Span *blocked, int blockedCount, bool nearEnd,
                      long margin, long clearance)
{
  if (spanSize <= 0 || start + margin * 2 + spanSize > end)
    return -1;

  const long first = start + margin;
  const long last = end - margin - spanSize;
  long candidate = nearEnd ? last : first;

  // Each pass moves past at least one obstacle. The bounded loop prevents malformed/overlapping input from
  // making this geometry helper spin.
  for (int pass = 0; pass <= blockedCount; ++pass)
  {
    bool moved = false;
    for (int i = 0; i < blockedCount; ++i)
    {
      const long bs = blocked[i].start < start ? start : blocked[i].start;
      const long be = blocked[i].end > end ? end : blocked[i].end;
      if (be <= bs || candidate + spanSize + clearance <= bs || candidate - clearance >= be)
        continue;
      candidate = nearEnd ? bs - clearance - spanSize : be + clearance;
      moved = true;
    }
    if (!moved)
      return candidate >= first && candidate <= last ? candidate : -1;
  }
  return -1;
}

} // namespace taskbar
} // namespace apex

#endif
