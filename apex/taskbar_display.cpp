// THE HOST-OWNED TASKBAR STRIP.
//
// Features contribute ONE text block per area. This file owns every pixel decision: order, side, font, colour,
// spacing, vertical rotation, and the compact-taskbar rule that turns a two-line contribution into one line.

#include "host.h"

#include "../common/system_theme.h"
#include "../common/taskbar_layout.h"

#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace apex {
namespace host {
namespace {

constexpr int kTimerId = 1;
constexpr int kRefreshMessage = WM_APP + 0x731;
constexpr int kTextCap = 1024;
constexpr int kLineCap = 512;
constexpr int kPad = 4;

struct Item
{
  int slot = -1;
  int side = 1;
  char key[128] = {};
  char line1[kLineCap] = {};
  char line2[kLineCap] = {};
  int declaredLines = 1;
  int lineCount = 1;
  int width = 0;
};

struct Column
{
  const Item *first = nullptr;
  const Item *second = nullptr; // non-null only when two one-line slots are stacked
  int width = 0;
  bool centred = false;
};

struct Geometry
{
  HWND taskbar = nullptr;
  RECT bar = {};
  RECT monitor = {};
  taskbar::Display display = {};
};

struct Occupants
{
  HWND taskbar = nullptr;
  HWND self = nullptr;
  DWORD taskbarPid = 0;
  DWORD selfPid = 0;
  RECT band = {};
  RECT items[32] = {};
  int count = 0;
};

HWND g_windows[2] = {nullptr, nullptr};
HFONT g_font = nullptr;
LOGFONTW g_logFont = {};
int g_fontPixels = 0;
ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplus = false;
bool g_visible[2] = {false, false};

struct WidthMemory
{
  char key[128] = {};
  int width = 0;
  ULONGLONG narrowSince = 0;
};

WidthMemory g_widths[kMaxTaskbarAreas];

void ResetWidthMemory()
{
  ZeroMemory(g_widths, sizeof(g_widths));
}

void Copy(char *dst, int cap, const char *src)
{
  if (!dst || cap <= 0)
    return;
  if (!src)
  {
    dst[0] = 0;
    return;
  }
  size_t n = 0;
  while (src[n] && src[n] != '\r' && src[n] != '\n' && n + 1 < (size_t)cap)
    ++n;
  std::memcpy(dst, src, n);
  dst[n] = 0;
}

void Trim(char *text)
{
  if (!text)
    return;
  char *begin = text;
  while (*begin == ' ')
    ++begin;
  if (begin != text)
    std::memmove(text, begin, std::strlen(begin) + 1);
  size_t n = std::strlen(text);
  while (n && text[n - 1] == ' ')
    text[--n] = 0;
}

void SplitLines(const char *text, char *first, int firstCap, char *second, int secondCap)
{
  first[0] = 0;
  second[0] = 0;
  if (!text)
    return;
  const char *newline = std::strchr(text, '\n');
  if (!newline)
  {
    Copy(first, firstCap, text);
    Trim(first);
    return;
  }
  const int n = (int)std::min<size_t>((size_t)firstCap - 1, (size_t)(newline - text));
  std::memcpy(first, text, (size_t)n);
  first[n] = 0;
  Copy(second, secondCap, newline + 1);
  Trim(first);
  Trim(second);
}

void MergeLines(Item *item)
{
  if (!item || !item->line2[0])
    return;
  for (char *p = item->line1; *p; ++p)
    if (*p == '\t')
      *p = ' ';
  for (char *p = item->line2; *p; ++p)
    if (*p == '\t')
      *p = ' ';
  const size_t used = std::strlen(item->line1);
  if (used + 2 < sizeof(item->line1))
    std::snprintf(item->line1 + used, sizeof(item->line1) - used, "  %s", item->line2);
  item->line2[0] = 0;
}

int ClampFont(int size)
{
  return std::max(9, std::min(16, size));
}

bool ParseHex(const char *text, unsigned *rgb)
{
  if (!text || std::strlen(text) != 7 || text[0] != '#')
    return false;
  unsigned value = 0;
  for (int i = 1; i < 7; ++i)
  {
    const char c = text[i];
    unsigned digit = 0;
    if (c >= '0' && c <= '9')
      digit = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      digit = (unsigned)(c - 'A' + 10);
    else
      return false;
    value = value * 16u + digit;
  }
  *rgb = value;
  return true;
}

COLORREF TextColor()
{
  HostConfig *config = SettingsConfig();
  unsigned rgb = 0;
  const char *configured = config ? config->taskbarTextColor : "auto";
  if (ParseHex(configured, &rgb))
    return RGB((rgb >> 16) & 0xffu, (rgb >> 8) & 0xffu, rgb & 0xffu);
  return common::SystemIsLightTheme() ? RGB(76, 80, 86) : RGB(188, 191, 196);
}

bool TaskbarIconsAreLeftAligned()
{
  DWORD value = 1;
  DWORD size = sizeof(value);
  const LONG result = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
      L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return result == ERROR_SUCCESS && size == sizeof(value) && value == 0;
}

int EffectiveFontSize(const Geometry &geometry)
{
  HostConfig *config = SettingsConfig();
  int size = ClampFont(config ? config->taskbarFontSize : 12);
  const int thickness = taskbar::Thickness(
      {geometry.bar.left, geometry.bar.top, geometry.bar.right, geometry.bar.bottom},
      geometry.display.edge);
  const int cap = taskbar::FontCap(thickness, geometry.display.lineCount);
  if (cap >= 9)
    size = std::min(size, cap);
  return size;
}

void EnsureFont(const Geometry &geometry)
{
  const int pixels = EffectiveFontSize(geometry);
  if (g_font && g_fontPixels == pixels)
    return;
  if (g_font)
    DeleteObject(g_font);
  // Widths are measured in the current font. Keeping a 16 px slot after the user changes the shared font to 9 px
  // leaves every item surrounded by the old font's blank space, even though the gap formula itself shrank.
  ResetWidthMemory();
  LOGFONTW font = {};
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    font = metrics.lfMessageFont;
  if (!font.lfFaceName[0])
    std::wcsncpy(font.lfFaceName, L"Segoe UI",
                 (sizeof(font.lfFaceName) / sizeof(font.lfFaceName[0])) - 1);
  font.lfHeight = -pixels;
  font.lfWeight = FW_NORMAL;
  font.lfQuality = ANTIALIASED_QUALITY;
  font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
  g_logFont = font;
  g_font = CreateFontIndirectW(&font);
  g_fontPixels = g_font ? pixels : 12;
}

bool CurrentGeometry(int requestedFont, Geometry *out)
{
  if (!out)
    return false;
  HWND taskbarWindow = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (!taskbarWindow)
    return false;
  RECT bar = {};
  if (!GetWindowRect(taskbarWindow, &bar))
    return false;
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromWindow(taskbarWindow, MONITOR_DEFAULTTONEAREST), &monitor))
    return false;
  out->taskbar = taskbarWindow;
  out->bar = bar;
  out->monitor = monitor.rcMonitor;
  out->display = taskbar::ChooseDisplay(
      {bar.left, bar.top, bar.right, bar.bottom},
      {monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right,
       monitor.rcMonitor.bottom},
      requestedFont);
  return true;
}

int TextWidth(const char *text)
{
  if (!text || !*text || !g_gdiplus)
    return 0;
  wchar_t wide[1024] = {};
  if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide,
                          (int)(sizeof(wide) / sizeof(wide[0]))) <= 0)
    return 0;
  Gdiplus::FontFamily primary(g_logFont.lfFaceName);
  Gdiplus::FontFamily chinese(L"Microsoft YaHei UI");
  Gdiplus::FontFamily latin(L"Segoe UI");
  Gdiplus::FontFamily *family = &primary;
  // Chinese text used to inherit Segoe UI's line metrics and only the glyphs were substituted. That makes the
  // visible Chinese ink sit high inside the row even when the rectangle itself is centred. Use the font that
  // actually owns the glyphs, so measurement, line height and drawing all agree.
  if (UiIsChinese() && chinese.GetLastStatus() == Gdiplus::Ok && chinese.IsAvailable())
    family = &chinese;
  else if (primary.GetLastStatus() != Gdiplus::Ok || !primary.IsAvailable())
    family = &chinese;
  if (family->GetLastStatus() != Gdiplus::Ok || !family->IsAvailable())
    family = &latin;
  Gdiplus::Font font(family, (Gdiplus::REAL)g_fontPixels, Gdiplus::FontStyleRegular,
                     Gdiplus::UnitPixel);
  Gdiplus::FontFamily resolved;
  if (font.GetFamily(&resolved) != Gdiplus::Ok)
    return 0;
  Gdiplus::StringFormat format;
  format.SetFormatFlags(static_cast<Gdiplus::StringFormatFlags>(
      Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces));
  format.SetAlignment(Gdiplus::StringAlignmentNear);
  format.SetLineAlignment(Gdiplus::StringAlignmentNear);
  Gdiplus::RectF layout(0, 0, 4096, 256);
  Gdiplus::GraphicsPath path;
  Gdiplus::RectF bounds;
  if (path.AddString(wide, -1, &resolved, font.GetStyle(), font.GetSize(),
                     layout, &format) != Gdiplus::Ok ||
      path.GetBounds(&bounds) != Gdiplus::Ok)
    return 0;
  return std::max(1, (int)(bounds.Width + 0.999f));
}

int SpaceWidth()
{
  static int cachedPixels = 0;
  static int cachedWidth = 0;
  if (cachedPixels == g_fontPixels && cachedWidth > 0)
    return cachedWidth;
  cachedPixels = g_fontPixels;
  cachedWidth = std::max(1, TextWidth("n n") - TextWidth("nn"));
  return cachedWidth;
}

int TextAdvance(const char *text)
{
  if (!text || !*text)
    return 0;
  size_t length = std::strlen(text);
  size_t trailing = 0;
  while (length > 0 && text[length - 1] == ' ')
  {
    --length;
    ++trailing;
  }
  if (!trailing)
    return TextWidth(text);
  char trimmed[kLineCap] = {};
  const size_t copy = std::min(length, sizeof(trimmed) - 1);
  std::memcpy(trimmed, text, copy);
  trimmed[copy] = 0;
  return TextWidth(trimmed) + (int)trailing * SpaceWidth();
}

struct MarkedTextPart
{
  std::string text;
  bool dot = false;
  unsigned rgb = 0;
};

std::vector<MarkedTextPart> ParseMarkedText(const char *text)
{
  std::vector<MarkedTextPart> parts;
  const char *cursor = text ? text : "";
  while (*cursor)
  {
    const char *open = std::strstr(cursor, "[[dot:#");
    const char *close = open ? std::strstr(open + 7, "]]") : nullptr;
    if (!open || !close || close - (open + 7) != 6)
    {
      MarkedTextPart part;
      part.text = cursor;
      if (!part.text.empty())
        parts.push_back(part);
      break;
    }
    if (open > cursor)
    {
      MarkedTextPart part;
      part.text.assign(cursor, (size_t)(open - cursor));
      parts.push_back(part);
    }
    char color[8] = {};
    std::snprintf(color, sizeof(color), "#%.*s", 6, open + 7);
    unsigned rgb = 0;
    if (!ParseHex(color, &rgb))
    {
      MarkedTextPart part;
      part.text.assign(cursor, (size_t)(close + 2 - cursor));
      parts.push_back(part);
      cursor = close + 2;
      continue;
    }
    MarkedTextPart dot;
    dot.dot = true;
    dot.rgb = rgb;
    parts.push_back(dot);
    cursor = close + 2;
  }
  return parts;
}

int DotPixels()
{
  return std::max(8, g_fontPixels - 2);
}

int DotTextGap()
{
  return std::max(2, g_fontPixels / 4);
}

int MarkedTextWidth(const char *text)
{
  int width = 0;
  const std::vector<MarkedTextPart> parts = ParseMarkedText(text);
  for (size_t i = 0; i < parts.size(); ++i)
  {
    const MarkedTextPart &part = parts[i];
    width += part.dot ? DotPixels() : TextAdvance(part.text.c_str());
    if (part.dot && i + 1 < parts.size())
      width += DotTextGap();
  }
  return width;
}

bool BuildTextPath(const wchar_t *wide, Gdiplus::Font *font,
                   const Gdiplus::StringFormat &format, Gdiplus::GraphicsPath *path,
                   Gdiplus::RectF *out)
{
  if (!wide || !font || !path || !out)
    return false;
  Gdiplus::FontFamily family;
  if (font->GetFamily(&family) != Gdiplus::Ok)
    return false;
  path->Reset();
  const Gdiplus::RectF layout(0.0f, 0.0f, 4096.0f, 256.0f);
  if (path->AddString(wide, -1, &family, font->GetStyle(), font->GetSize(),
                      layout, &format) != Gdiplus::Ok ||
      path->GetBounds(out) != Gdiplus::Ok)
    return false;
  return out->Height > 0.0f;
}

void DrawMarkedText(Gdiplus::Graphics &graphics, const char *text, Gdiplus::Font *font,
                    const Gdiplus::SolidBrush &brush, const Gdiplus::RectF &box,
                    bool rightAligned)
{
  const std::vector<MarkedTextPart> parts = ParseMarkedText(text);
  std::vector<Gdiplus::REAL> partAdvances;
  std::vector<Gdiplus::REAL> partInkWidths;
  partAdvances.reserve(parts.size());
  partInkWidths.reserve(parts.size());
  bool hasDot = false;
  bool haveTextInk = false;
  Gdiplus::REAL inkTop = 0.0f;
  Gdiplus::REAL inkBottom = 0.0f;
  Gdiplus::StringFormat format;
  format.SetAlignment(Gdiplus::StringAlignmentNear);
  format.SetLineAlignment(Gdiplus::StringAlignmentNear);
  format.SetFormatFlags(static_cast<Gdiplus::StringFormatFlags>(
      Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces));
  format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  for (size_t i = 0; i < parts.size(); ++i)
  {
    const MarkedTextPart &part = parts[i];
    hasDot = hasDot || part.dot;
    if (part.dot)
    {
      const Gdiplus::REAL width = (Gdiplus::REAL)DotPixels();
      partAdvances.push_back(width + (i + 1 < parts.size() ? DotTextGap() : 0));
      partInkWidths.push_back(width);
    }
    else
    {
      const Gdiplus::REAL advance = (Gdiplus::REAL)TextAdvance(part.text.c_str());
      Gdiplus::REAL inkWidth = advance;
      wchar_t wide[1024] = {};
      if (MultiByteToWideChar(CP_UTF8, 0, part.text.c_str(), -1, wide,
                              (int)(sizeof(wide) / sizeof(wide[0]))) > 0)
      {
        Gdiplus::RectF ink;
        Gdiplus::GraphicsPath path;
        if (BuildTextPath(wide, font, format, &path, &ink))
        {
          inkWidth = ink.Width;
          if (!haveTextInk)
          {
            inkTop = ink.Y;
            inkBottom = ink.Y + ink.Height;
            haveTextInk = true;
          }
          else
          {
            inkTop = std::min(inkTop, ink.Y);
            inkBottom = std::max(inkBottom, ink.Y + ink.Height);
          }
        }
      }
      partAdvances.push_back(advance);
      partInkWidths.push_back(inkWidth);
    }
  }
  Gdiplus::REAL visualWidth = 0.0f;
  for (size_t i = 0; i < parts.size(); ++i)
    visualWidth += (i + 1 == parts.size() && !parts[i].dot)
                       ? partInkWidths[i]
                       : partAdvances[i];
  if (!haveTextInk)
  {
    const Gdiplus::REAL fontHeight = font->GetHeight(&graphics);
    inkTop = 0.0f;
    inkBottom = fontHeight;
  }
  // Centre the visible ink, not GDI+'s line box. Chinese glyphs have almost no descender, so line-box centring
  // leaves blank space below them and makes the words look high even when the rectangle is mathematically centred.
  // The same rule serves plain lines and dot-bearing lines, which is what keeps the two rows on one visual line.
  const Gdiplus::REAL inkHeight = inkBottom - inkTop;
  const Gdiplus::REAL dotHeight = hasDot ? (Gdiplus::REAL)DotPixels() : 0.0f;
  const Gdiplus::REAL groupHeight = std::max(inkHeight, dotHeight);
  const Gdiplus::REAL groupTop =
      box.Y + std::max(0.0f, (box.Height - groupHeight) / 2.0f);
  // A merged one-line Compute Manager item puts the coloured circle BETWEEN two text runs. GDI+'s ellipse
  // raster sits visually one pixel lower than the glyph ink centre; lift only that middle-circle case so the
  // ordinary two-line leading circle keeps the position already accepted by the user.
  const bool dotBetweenText =
      hasDot && parts.size() > 2 && !parts.front().dot;
  const Gdiplus::REAL dotCentreY =
      groupTop + groupHeight / 2.0f - (dotBetweenText ? 1.0f : 0.0f);
  const Gdiplus::REAL desiredInkTop = groupTop + (groupHeight - inkHeight) / 2.0f;
  const Gdiplus::REAL textTop = desiredInkTop - inkTop;
  const Gdiplus::REAL textHeight = box.Height;
  Gdiplus::REAL x =
      box.X + (rightAligned ? std::max(0.0f, box.Width - visualWidth) : 0.0f);
  for (size_t i = 0; i < parts.size(); ++i)
  {
    const MarkedTextPart &part = parts[i];
    if (part.dot)
    {
      const int diameter = DotPixels();
      Gdiplus::SolidBrush dot(
          Gdiplus::Color(255, (part.rgb >> 16) & 0xffu, (part.rgb >> 8) & 0xffu,
                         part.rgb & 0xffu));
      const Gdiplus::REAL left = x;
      const Gdiplus::REAL top = dotCentreY - diameter / 2.0f;
      graphics.FillEllipse(&dot, left, top, (Gdiplus::REAL)diameter,
                           (Gdiplus::REAL)diameter);
      x += partAdvances[i];
      continue;
    }
    wchar_t wide[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, part.text.c_str(), -1, wide,
                        (int)(sizeof(wide) / sizeof(wide[0])));
    Gdiplus::GraphicsPath path;
    Gdiplus::RectF ink;
    if (BuildTextPath(wide, font, format, &path, &ink))
    {
      const Gdiplus::REAL startX =
          x;
      Gdiplus::Matrix transform;
      transform.Translate(startX - ink.X, desiredInkTop - ink.Y);
      path.Transform(&transform);
      graphics.FillPath(&brush, &path);
    }
    else
    {
      const int width = TextWidth(part.text.c_str());
      Gdiplus::RectF rect(x, textTop, (Gdiplus::REAL)width,
                          textHeight);
      graphics.DrawString(wide, -1, font, rect, &format, &brush);
    }
    x += partAdvances[i];
  }
}

int ColumnGap()
{
  return std::max(4, g_fontPixels / 2);
}

bool SplitColumns(const char *text, char *left, int leftCap, char *right, int rightCap)
{
  if (!text || !left || !right)
    return false;
  const char *tab = std::strrchr(text, '\t');
  if (!tab)
    return false;
  const int n = (int)std::min<size_t>((size_t)leftCap - 1, (size_t)(tab - text));
  std::memcpy(left, text, (size_t)n);
  left[n] = 0;
  Copy(right, rightCap, tab + 1);
  Trim(left);
  Trim(right);
  return left[0] || right[0];
}

int TaskbarLineWidth(const char *text)
{
  char left[kLineCap] = {};
  char right[kLineCap] = {};
  if (!SplitColumns(text, left, sizeof(left), right, sizeof(right)))
    return MarkedTextWidth(text);
  int width = MarkedTextWidth(left) + MarkedTextWidth(right);
  if (left[0] && right[0])
    width += ColumnGap();
  return width;
}

void DrawTaskbarLine(Gdiplus::Graphics &graphics, const char *text, Gdiplus::Font *font,
                     const Gdiplus::SolidBrush &brush, const Gdiplus::RectF &box,
                     bool rightAligned)
{
  char left[kLineCap] = {};
  char right[kLineCap] = {};
  if (!SplitColumns(text, left, sizeof(left), right, sizeof(right)))
  {
    DrawMarkedText(graphics, text, font, brush, box, rightAligned);
    return;
  }
  const int rightWidth = MarkedTextWidth(right);
  const int gap = left[0] && right[0] ? ColumnGap() : 0;
  const int leftWidth = std::max(0, (int)box.Width - rightWidth - gap);
  if (left[0])
  {
    Gdiplus::RectF leftBox(box.X, box.Y, (Gdiplus::REAL)leftWidth, box.Height);
    DrawMarkedText(graphics, left, font, brush, leftBox, false);
  }
  if (right[0])
  {
    Gdiplus::RectF rightBox(box.X + box.Width - rightWidth, box.Y,
                            (Gdiplus::REAL)rightWidth, box.Height);
    DrawMarkedText(graphics, right, font, brush, rightBox, true);
  }
}

int StableWidth(const char *key, int naturalWidth)
{
  if (!key || !*key || naturalWidth <= 0)
    return std::max(1, naturalWidth);
  const int bucket = (naturalWidth + 1) & ~1;
  WidthMemory *freeEntry = nullptr;
  for (int i = 0; i < kMaxTaskbarAreas; ++i)
  {
    if (!g_widths[i].key[0])
    {
      if (!freeEntry)
        freeEntry = &g_widths[i];
      continue;
    }
    if (!std::strcmp(g_widths[i].key, key))
    {
      WidthMemory &memory = g_widths[i];
      if (bucket > memory.width)
      {
        memory.width = bucket;
        memory.narrowSince = 0;
      }
      else
        memory.narrowSince = 0;
      return memory.width;
    }
  }
  if (freeEntry)
  {
    Copy(freeEntry->key, sizeof(freeEntry->key), key);
    freeEntry->width = bucket;
    freeEntry->narrowSince = 0;
    return freeEntry->width;
  }
  return bucket;
}

bool GlyphLineWindow(HWND window, const Occupants &scan)
{
  if (!window || window == scan.taskbar || window == scan.self || !IsWindowVisible(window))
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (!pid || pid == scan.taskbarPid || pid == scan.selfPid)
    return false;
  RECT r = {};
  if (!GetWindowRect(window, &r) || r.right <= r.left || r.bottom <= r.top)
    return false;
  if (r.bottom - r.top > scan.band.bottom - scan.band.top + 2)
    return false;
  if (r.top < scan.band.top - 2 || r.bottom > scan.band.bottom + 2)
    return false;
  return r.right > scan.band.left && r.left < scan.band.right;
}

BOOL CALLBACK CollectChild(HWND window, LPARAM parameter)
{
  auto *scan = reinterpret_cast<Occupants *>(parameter);
  if (GlyphLineWindow(window, *scan) &&
      scan->count < (int)(sizeof(scan->items) / sizeof(scan->items[0])))
    GetWindowRect(window, &scan->items[scan->count++]);
  return TRUE;
}

BOOL CALLBACK CollectTopLevel(HWND window, LPARAM parameter)
{
  auto *scan = reinterpret_cast<Occupants *>(parameter);
  if (!GlyphLineWindow(window, *scan))
    return TRUE;
  const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if (!(ex & (WS_EX_TOOLWINDOW | WS_EX_LAYERED)))
    return TRUE;
  wchar_t cls[64] = {};
  GetClassNameW(window, cls, (int)(sizeof(cls) / sizeof(cls[0])));
  if (!std::wcscmp(cls, L"#32768") || !_wcsicmp(cls, L"tooltips_class32"))
    return TRUE;
  if (scan->count < (int)(sizeof(scan->items) / sizeof(scan->items[0])))
    GetWindowRect(window, &scan->items[scan->count++]);
  return TRUE;
}

void ScanOccupants(const Geometry &geometry, HWND self, Occupants *out)
{
  Occupants scan;
  scan.taskbar = geometry.taskbar;
  scan.self = self;
  scan.selfPid = GetCurrentProcessId();
  GetWindowThreadProcessId(geometry.taskbar, &scan.taskbarPid);
  scan.band = geometry.bar;
  EnumChildWindows(geometry.taskbar, CollectChild, (LPARAM)&scan);
  EnumWindows(CollectTopLevel, (LPARAM)&scan);
  *out = scan;
}

void UsableSpan(const Geometry &geometry, long *start, long *end)
{
  const RECT &bar = geometry.bar;
  *start = geometry.display.vertical ? bar.top + 4 : bar.left + 4;
  *end = geometry.display.vertical ? bar.bottom - 4 : bar.right - 4;
  HWND notify = FindWindowExW(geometry.taskbar, nullptr, L"TrayNotifyWnd", nullptr);
  RECT notifyRect = {};
  if (!notify || !GetWindowRect(notify, &notifyRect))
    return;
  if (geometry.display.vertical)
  {
    const long middle = (notifyRect.top + notifyRect.bottom) / 2;
    if (middle >= (bar.top + bar.bottom) / 2)
      *end = std::min(*end, (long)notifyRect.top - 4);
    else
      *start = std::max(*start, (long)notifyRect.bottom + 4);
  }
  else
  {
    const long middle = (notifyRect.left + notifyRect.right) / 2;
    if (middle >= (bar.left + bar.right) / 2)
      *end = std::min(*end, (long)notifyRect.left - 4);
    else
      *start = std::max(*start, (long)notifyRect.right + 4);
  }
  if (*end - *start < 16)
  {
    *start = geometry.display.vertical ? bar.top + 4 : bar.left + 4;
    *end = geometry.display.vertical ? bar.bottom - 4 : bar.right - 4;
  }
}

int CollectItems(Item *items, int max)
{
  TaskbarArea areas[kMaxTaskbarAreas];
  const int areaCount = TaskbarAreas(areas, kMaxTaskbarAreas);
  Loader *loader = SettingsLoader();
  if (!loader)
    return 0;
  const int chinese = UiIsChinese() ? 1 : 0;
  int count = 0;
  for (int i = 0; i < areaCount && count < max; ++i)
  {
    LoadedFeature feature = loader->At(areas[i].slot);
    if (!feature.ok || !feature.api || !feature.api->taskbarText)
      continue;
    char text[kTextCap] = {};
    SetCurrentFeature(feature.api->id);
    const int ok = feature.api->taskbarText(areas[i].key, chinese, text, sizeof(text));
    SetCurrentFeature(nullptr);
    if (!ok)
      continue;
    Item item;
    item.slot = areas[i].slot;
    item.side = areas[i].side ? 1 : 0;
    Copy(item.key, sizeof(item.key), areas[i].key);
    item.declaredLines = areas[i].lineCount == 2 ? 2 : 1;
    SplitLines(text, item.line1, sizeof(item.line1), item.line2, sizeof(item.line2));
    if (!item.line1[0])
      continue;
    items[count++] = item;
  }
  return count;
}

int BuildColumns(const Item *items, int count, const taskbar::Display &display,
                 Column *columns, int max)
{
  int written = 0;
  for (int i = 0; i < count && written < max;)
  {
    const Item &item = items[i];
    Column &column = columns[written++];
    column.first = &item;
    column.width = item.width;
    if (taskbar::PairSingleLineSlots(display) && item.lineCount == 1)
    {
      if (i + 1 < count && items[i + 1].lineCount == 1)
      {
        column.second = &items[i + 1];
        column.width = std::max(column.width, items[i + 1].width);
        i += 2;
      }
      else
      {
        column.centred = true;
        ++i;
      }
    }
    else
      ++i;
  }
  return written;
}

void Render(HWND window, const Column *columns, int count, int width, int height,
            const Geometry &geometry, int side)
{
  HDC screen = GetDC(nullptr);
  if (!screen)
    return;
  HDC memory = CreateCompatibleDC(screen);
  if (!memory)
  {
    ReleaseDC(nullptr, screen);
    return;
  }
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap)
  {
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
  auto *pixels = static_cast<DWORD *>(bits);
  Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB,
                          reinterpret_cast<BYTE *>(pixels));
  Gdiplus::Graphics graphics(&surface);
  graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
  graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  int drawHeight = height;
  if (geometry.display.vertical)
  {
    graphics.TranslateTransform((Gdiplus::REAL)width, 0.0f);
    graphics.RotateTransform(90.0f);
    drawHeight = width;
  }
  Gdiplus::FontFamily primary(g_logFont.lfFaceName);
  Gdiplus::FontFamily chinese(L"Microsoft YaHei UI");
  Gdiplus::FontFamily latin(L"Segoe UI");
  Gdiplus::FontFamily *family = &primary;
  if (UiIsChinese() && chinese.GetLastStatus() == Gdiplus::Ok && chinese.IsAvailable())
    family = &chinese;
  else if (primary.GetLastStatus() != Gdiplus::Ok || !primary.IsAvailable())
    family = &chinese;
  if (family->GetLastStatus() != Gdiplus::Ok || !family->IsAvailable())
    family = &latin;
  Gdiplus::Font font(family, (Gdiplus::REAL)g_fontPixels, Gdiplus::FontStyleRegular,
                     Gdiplus::UnitPixel);
  const COLORREF color = TextColor();
  Gdiplus::SolidBrush brush(
      Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
  const taskbar::TwoLineRows rows = taskbar::CompactTwoLineRows(drawHeight, g_fontPixels);
  int x = side == 1 ? 0 : kPad;
  for (int i = 0; i < count; ++i)
  {
    const Column &column = columns[i];
    const Item &item = *column.first;
    if (column.second)
    {
      Gdiplus::RectF r1((Gdiplus::REAL)x, (Gdiplus::REAL)rows.titleTop,
                        (Gdiplus::REAL)column.width, (Gdiplus::REAL)rows.lineHeight);
      Gdiplus::RectF r2((Gdiplus::REAL)x, (Gdiplus::REAL)rows.balanceTop,
                        (Gdiplus::REAL)column.width, (Gdiplus::REAL)rows.lineHeight);
      DrawTaskbarLine(graphics, item.line1, &font, brush, r1, side == 1);
      DrawTaskbarLine(graphics, column.second->line1, &font, brush, r2, side == 1);
    }
    else if (item.lineCount == 2 && item.line2[0])
    {
      Gdiplus::RectF r1((Gdiplus::REAL)x, (Gdiplus::REAL)rows.titleTop,
                        (Gdiplus::REAL)column.width, (Gdiplus::REAL)rows.lineHeight);
      Gdiplus::RectF r2((Gdiplus::REAL)x, (Gdiplus::REAL)rows.balanceTop,
                        (Gdiplus::REAL)column.width, (Gdiplus::REAL)rows.lineHeight);
      DrawTaskbarLine(graphics, item.line1, &font, brush, r1, side == 1);
      DrawTaskbarLine(graphics, item.line2, &font, brush, r2, side == 1);
    }
    else
    {
      Gdiplus::RectF r((Gdiplus::REAL)x, 0.0f, (Gdiplus::REAL)column.width,
                       (Gdiplus::REAL)drawHeight);
      DrawTaskbarLine(graphics, item.line1, &font, brush, r, side == 1);
    }
    x += column.width;
    if (i + 1 < count)
      x += taskbar::ItemGap(geometry.display.vertical, g_fontPixels);
  }
  graphics.Flush(Gdiplus::FlushIntentionSync);
  POINT source = {0, 0};
  SIZE size = {width, height};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(window, screen, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA);
  SelectObject(memory, oldBitmap);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
}

void HideSide(int side)
{
  if (g_windows[side] && g_visible[side])
    ShowWindow(g_windows[side], SW_HIDE);
  g_visible[side] = false;
}

void RefreshSide(int side, bool iconsLeftAligned)
{
  side = side ? 1 : 0;
  if (!g_windows[side])
    return;
  HostConfig *config = SettingsConfig();
  const char *suppressed = std::getenv("APEX_NO_TRAY");
  if (!config || !config->taskbarEnabled || (suppressed && !std::strcmp(suppressed, "1")))
  {
    HideSide(side);
    return;
  }
  Geometry geometry;
  if (!CurrentGeometry(config->taskbarFontSize, &geometry))
  {
    HideSide(side);
    return;
  }
  EnsureFont(geometry);
  Item items[kMaxTaskbarAreas];
  Item allItems[kMaxTaskbarAreas];
  const int allCount = CollectItems(allItems, kMaxTaskbarAreas);
  int count = 0;
  for (int i = 0; i < allCount; ++i)
    if (taskbar::EffectiveTaskbarSide(allItems[i].side, iconsLeftAligned) == side)
      items[count++] = allItems[i];
  if (!count)
  {
    HideSide(side);
    return;
  }
  for (int i = 0; i < count; ++i)
  {
    items[i].lineCount = taskbar::LinesForItem(items[i].declaredLines, geometry.display);
    if (items[i].lineCount == 1)
      MergeLines(&items[i]);
    const int w1 = TaskbarLineWidth(items[i].line1);
    const int w2 = items[i].lineCount == 2 ? TaskbarLineWidth(items[i].line2) : 0;
    items[i].width =
        StableWidth(items[i].key, std::max(1, std::max(w1, w2) + 1));
  }
  Column columns[kMaxTaskbarAreas];
  const int columnCount =
      BuildColumns(items, count, geometry.display, columns, kMaxTaskbarAreas);
  long axisStart = 0, axisEnd = 0;
  UsableSpan(geometry, &axisStart, &axisEnd);
  int content = 0;
  for (int i = 0; i < columnCount; ++i)
  {
    content += columns[i].width;
    if (i + 1 < columnCount)
      content += taskbar::ItemGap(geometry.display.vertical, g_fontPixels);
  }
  // A right-side strip ends at the text itself. The left-side strip keeps the small breathing room on both ends;
  // the right-side one must not add a second blank after already being right-aligned within every column.
  const int leftPad = side == 1 ? 0 : kPad;
  const int rightPad = side == 1 ? 0 : kPad;
  int extent = content + leftPad + rightPad;
  extent = (int)std::max(1L, std::min((long)extent, axisEnd - axisStart - 8));
  const bool nearEnd = side == 1;
  long placed = axisStart;
  if (geometry.display.vertical && !nearEnd)
  {
    // The user's explicit top choice means the top edge. Do not let an unrelated vertical band push the strip
    // down into the middle and leave a large empty block above it.
    placed = axisStart;
  }
  else
  {
    Occupants occupants;
    ScanOccupants(geometry, g_windows[side], &occupants);
    taskbar::Span blocked[32] = {};
    int blockedCount = 0;
    for (int i = 0; i < occupants.count; ++i)
    {
      blocked[blockedCount].start =
          geometry.display.vertical ? occupants.items[i].top : occupants.items[i].left;
      blocked[blockedCount].end =
          geometry.display.vertical ? occupants.items[i].bottom : occupants.items[i].right;
      ++blockedCount;
    }
    const long placementMargin = nearEnd ? 0 : 4;
    const long answer = taskbar::PlaceSpan(
        axisStart, axisEnd, extent, blocked, blockedCount, nearEnd, placementMargin, 8);
    placed = answer >= 0 ? answer : (nearEnd ? axisEnd - extent : axisStart);
  }
  const int barWidth = geometry.bar.right - geometry.bar.left;
  const int barHeight = geometry.bar.bottom - geometry.bar.top;
  const int thickness =
      taskbar::Thickness({geometry.bar.left, geometry.bar.top, geometry.bar.right, geometry.bar.bottom},
                         geometry.display.edge);
  int width = 0, height = 0, x = 0, y = 0;
  if (geometry.display.vertical)
  {
    width = std::max(16, thickness - 4);
    height = extent;
    x = geometry.bar.left + (barWidth - width) / 2;
    y = (int)placed;
  }
  else
  {
    width = extent;
    height = std::max(16, thickness - 4);
    x = (int)placed;
    y = geometry.bar.top + (barHeight - height) / 2;
  }
  // First show goes into the topmost band. Once it is already visible, moving/resizing must not promote it above
  // a screen-off window (or any later topmost overlay) on every one-second refresh.
  const UINT positionFlags =
      SWP_NOACTIVATE | SWP_SHOWWINDOW | (g_visible[side] ? SWP_NOZORDER : 0);
  SetWindowPos(g_windows[side], HWND_TOPMOST, x, y, width, height, positionFlags);
  g_visible[side] = true;
  Render(g_windows[side], columns, columnCount, width, height, geometry, side);
}

void Refresh()
{
  const bool iconsLeftAligned = TaskbarIconsAreLeftAligned();
  RefreshSide(0, iconsLeftAligned);
  RefreshSide(1, iconsLeftAligned);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message)
  {
  case WM_NCHITTEST:
    return HTTRANSPARENT;
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_TIMER:
    if (wParam == kTimerId)
      Refresh();
    return 0;
  case WM_SETTINGCHANGE:
  case WM_DISPLAYCHANGE:
    Refresh();
    return 0;
  case kRefreshMessage:
    Refresh();
    return 0;
  case WM_DESTROY:
    return 0;
  default:
    return DefWindowProcW(window, message, wParam, lParam);
  }
}

} // namespace

void TaskbarDisplayInit()
{
  if (g_windows[0] || g_windows[1])
    return;
  ResetWidthMemory();
  Gdiplus::GdiplusStartupInput input;
  g_gdiplus = Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok;
  if (!g_gdiplus)
  {
    SettingsLog("taskbar display: GDI+ could not start");
    return;
  }
  WNDCLASSW cls = {};
  cls.lpfnWndProc = WindowProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"ApexTaskbarDisplayWnd";
  if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    SettingsLog("taskbar display: class registration failed (%lu)", GetLastError());
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
    g_gdiplus = false;
    return;
  }
  for (int side = 0; side < 2; ++side)
  {
    g_windows[side] = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        cls.lpszClassName, side ? L"Apex taskbar right" : L"Apex taskbar left", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
    if (g_windows[side])
      SetWindowLongPtrW(g_windows[side], GWLP_USERDATA, side);
  }
  if (!g_windows[0] || !g_windows[1])
  {
    SettingsLog("taskbar display: window creation failed (%lu)", GetLastError());
    for (int side = 0; side < 2; ++side)
      if (g_windows[side])
      {
        DestroyWindow(g_windows[side]);
        g_windows[side] = nullptr;
      }
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
    g_gdiplus = false;
    return;
  }
  SetTimer(g_windows[0], kTimerId, 1000, nullptr);
  SettingsLog("taskbar display: host-owned strip created");
  Refresh();
}

void TaskbarDisplayShutdown()
{
  for (int side = 0; side < 2; ++side)
  {
    if (g_windows[side])
    {
      KillTimer(g_windows[side], kTimerId);
      DestroyWindow(g_windows[side]);
      g_windows[side] = nullptr;
    }
  }
  UnregisterClassW(L"ApexTaskbarDisplayWnd", GetModuleHandleW(nullptr));
  if (g_font)
    DeleteObject(g_font);
  g_font = nullptr;
  g_fontPixels = 0;
  ResetWidthMemory();
  g_visible[0] = false;
  g_visible[1] = false;
  ZeroMemory(g_widths, sizeof(g_widths));
  if (g_gdiplus)
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
  g_gdiplusToken = 0;
  g_gdiplus = false;
}

void TaskbarDisplayRefresh()
{
  for (int side = 0; side < 2; ++side)
    if (g_windows[side])
      PostMessageW(g_windows[side], kRefreshMessage, 0, 0);
}

} // namespace host
} // namespace apex
