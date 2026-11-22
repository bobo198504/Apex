#ifndef APEX_SYSTEM_STATUS_LOGIC_H
#define APEX_SYSTEM_STATUS_LOGIC_H

// Pure rules for SystemStatus. No Windows types live here, so the gate can exercise the same
// visibility and number-formatting code that the DLL uses.

#include <cmath>
#include <cstdio>
#include <cstring>

namespace apex {
namespace system_status {

enum Metric : unsigned
{
  kDownload = 1u << 0,
  kUpload = 1u << 1,
  kCpu = 1u << 2,
  kMemory = 1u << 3,
  kStorage = 1u << 4,
  kGpu = 1u << 5,
  kIp = 1u << 6,
  kClock = 1u << 7,
  kDate = 1u << 8,
  kLunar = 1u << 9,
};

constexpr unsigned kAllMetrics = (1u << 10) - 1u;

// The first-run row is intentionally small. Every other item is one switch away, and a fresh install
// should show the status people look at most without turning the taskbar into a wall of telemetry.
constexpr unsigned kDefaultVisible = kDownload | kUpload | kCpu | kMemory | kClock;

inline bool ParseBool(const char *text, bool *out)
{
  if (!text || !out)
    return false;
  if (!std::strcmp(text, "1") || !_stricmp(text, "true") || !_stricmp(text, "on") ||
      !_stricmp(text, "yes"))
  {
    *out = true;
    return true;
  }
  if (!std::strcmp(text, "0") || !_stricmp(text, "false") || !_stricmp(text, "off") ||
      !_stricmp(text, "no"))
  {
    *out = false;
    return true;
  }
  return false;
}

inline unsigned SetVisible(unsigned bits, unsigned metric, bool visible)
{
  return visible ? (bits | metric) : (bits & ~metric);
}

inline bool IsVisible(unsigned bits, unsigned metric)
{
  return (bits & metric) != 0;
}

inline unsigned VisibleCount(unsigned bits)
{
  unsigned count = 0;
  while (bits)
  {
    bits &= bits - 1u;
    ++count;
  }
  return count;
}

// Rates are shown in the unit that keeps the number readable. 1024 is the boundary because these
// are byte counters from the network stack; the suffix makes the base explicit to the reader.
inline void FormatRate(double bytesPerSecond, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  if (!std::isfinite(bytesPerSecond) || bytesPerSecond < 0.0)
    bytesPerSecond = 0.0;
  static const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
  double value = bytesPerSecond;
  int unit = 0;
  while (value >= 1024.0 && unit < 4)
  {
    value /= 1024.0;
    ++unit;
  }
  if (unit == 0)
    _snprintf(out, outSize, "%.0f %s", value, units[unit]);
  else if (value < 10.0)
    _snprintf(out, outSize, "%.1f %s", value, units[unit]);
  else
    _snprintf(out, outSize, "%.0f %s", value, units[unit]);
}

inline void FormatPercent(double percent, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  if (!std::isfinite(percent) || percent < 0.0)
    percent = 0.0;
  if (percent > 100.0)
    percent = 100.0;
  _snprintf(out, outSize, "%.0f%%", percent);
}

inline void FormatTemperature(bool available, double celsius, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  if (!available || !std::isfinite(celsius))
  {
    _snprintf(out, outSize, "--");
    return;
  }
  _snprintf(out, outSize, "%.0f C", celsius);
}

inline const char *WeekdayZh(int weekday)
{
  static const char *names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  return weekday >= 0 && weekday < 7 ? names[weekday] : "";
}

inline const char *WeekdayEn(int weekday)
{
  static const char *names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return weekday >= 0 && weekday < 7 ? names[weekday] : "";
}

} // namespace system_status
} // namespace apex

#endif // APEX_SYSTEM_STATUS_LOGIC_H
