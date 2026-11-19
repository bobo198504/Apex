#ifndef APEX_WALLPAPER_LOGIC_H
#define APEX_WALLPAPER_LOGIC_H

// Pure rules for Wallpaper. This header deliberately has no Windows dependency so the gate can prove the
// schedule, time parsing and virtual-desktop mapping without touching the user's wallpaper.

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace apex {
namespace wallpaper {

constexpr double kPi = 3.14159265358979323846;

enum class Schedule
{
  Clock,
  Solar,
  Interval
};

enum class Variant
{
  Day,
  Night,
  Interval
};

struct Rect
{
  long left;
  long top;
  long right;
  long bottom;
};

inline bool ParseClock(const char *text, int *out)
{
  if (!text || !out)
    return false;
  while (*text == ' ' || *text == '\t')
    ++text;
  if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9' || text[2] != ':' ||
      text[3] < '0' || text[3] > '9' || text[4] < '0' || text[4] > '9')
    return false;
  const int hour = (text[0] - '0') * 10 + (text[1] - '0');
  const int minute = (text[3] - '0') * 10 + (text[4] - '0');
  const char *tail = text + 5;
  while (*tail == ' ' || *tail == '\t' || *tail == '\r' || *tail == '\n')
    ++tail;
  if (*tail || hour > 23 || minute > 59)
    return false;
  *out = hour * 60 + minute;
  return true;
}

inline bool InClockRange(int minute, int start, int end)
{
  if (start == end)
    return false;
  if (start < end)
    return minute >= start && minute < end;
  return minute >= start || minute < end;
}

inline bool ParseDouble(const char *text, double *out)
{
  if (!text || !out)
    return false;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    ++text;
  if (!*text)
    return false;
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(text, &end);
  if (end == text || errno == ERANGE || !std::isfinite(value))
    return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    ++end;
  if (*end)
    return false;
  *out = value;
  return true;
}

inline bool ParseInt(const char *text, int *out)
{
  if (!text || !out)
    return false;
  while (*text == ' ' || *text == '\t')
    ++text;
  const char *begin = text;
  if (*text == '+' || *text == '-')
    ++text;
  if (!*text)
    return false;
  for (const char *p = text; *p; ++p)
  {
    if (*p < '0' || *p > '9')
    {
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
      if (*p)
        return false;
      break;
    }
  }
  *out = std::atoi(begin);
  return true;
}

inline bool JsonCoordinateAt(const char *json, const char *key, double *out)
{
  if (!json || !key || !out)
    return false;
  const char *at = std::strstr(json, key);
  if (!at)
    return false;
  at += std::strlen(key);
  while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n' || *at == ':')
    ++at;
  if (*at != '-' && (*at < '0' || *at > '9'))
    return false;
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(at, &end);
  if (end == at || errno == ERANGE || !std::isfinite(value))
    return false;
  *out = value;
  return true;
}

// Parse the small, stable part of public IP-location JSON that Wallpaper needs. It intentionally accepts both
// common spellings (`latitude`/`longitude` and `lat`/`lon`) and ignores every other field.
inline bool ParseGeoJson(const char *json, double *latitude, double *longitude)
{
  if (!json || !latitude || !longitude)
    return false;
  double lat = 0.0, lon = 0.0;
  if (!JsonCoordinateAt(json, "\"latitude\"", &lat) &&
      !JsonCoordinateAt(json, "\"lat\"", &lat))
    return false;
  if (!JsonCoordinateAt(json, "\"longitude\"", &lon) &&
      !JsonCoordinateAt(json, "\"lon\"", &lon))
    return false;
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
    return false;
  *latitude = lat;
  *longitude = lon;
  return true;
}

inline bool ParsePan(const char *text, double *x, double *y)
{
  if (!text || !x || !y)
    return false;
  const char *comma = std::strchr(text, ',');
  if (!comma)
    return false;
  char first[64] = {0};
  const size_t n = static_cast<size_t>(comma - text);
  if (n == 0 || n >= sizeof(first))
    return false;
  std::memcpy(first, text, n);
  first[n] = 0;
  double px = 0, py = 0;
  if (!ParseDouble(first, &px) || !ParseDouble(comma + 1, &py))
    return false;
  if (px < 0.0 || px > 100.0 || py < 0.0 || py > 100.0)
    return false;
  *x = px;
  *y = py;
  return true;
}

inline Variant SelectVariant(Schedule schedule, int minute, int dayStart, int nightStart,
                             bool solarDay, long long epochSeconds, int intervalMinutes,
                             int imageCount, int *intervalIndex)
{
  if (intervalIndex)
    *intervalIndex = 0;
  switch (schedule)
  {
  case Schedule::Clock:
    if (dayStart == nightStart)
      return Variant::Day;
    return InClockRange(minute, dayStart, nightStart) ? Variant::Day : Variant::Night;
  case Schedule::Solar:
    return solarDay ? Variant::Day : Variant::Night;
  case Schedule::Interval:
  {
    if (intervalMinutes < 1 || imageCount < 1 || epochSeconds < 0)
      return Variant::Day;
    const long long bucket = epochSeconds / (static_cast<long long>(intervalMinutes) * 60);
    if (intervalIndex)
      *intervalIndex = static_cast<int>(bucket % imageCount);
    return Variant::Interval;
  }
  default:
    return Variant::Day;
  }
}

inline double Radians(double degrees)
{
  return degrees * kPi / 180.0;
}

inline double Degrees(double radians)
{
  return radians * 180.0 / kPi;
}

inline double NormaliseDegrees(double value)
{
  value = std::fmod(value, 360.0);
  return value < 0.0 ? value + 360.0 : value;
}

inline int DayOfYear(int year, int month, int day)
{
  static const int kDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  if (month < 1 || month > 12 || day < 1 || day > 31)
    return 0;
  int value = kDays[month - 1] + day;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (leap && month > 2)
    ++value;
  return value;
}

// NOAA's standard sunrise equation. The result is in local minutes from midnight, rounded to the nearest
// minute. The polar-day/night cases are explicit instead of being represented by an impossible clock time.
inline bool SolarTimes(double latitude, double longitude, int year, int month, int day,
                       int timezoneMinutes, int *sunriseMinute, int *sunsetMinute, bool *alwaysDay)
{
  if (!sunriseMinute || !sunsetMinute || !alwaysDay)
    return false;
  if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0 ||
      timezoneMinutes < -900 || timezoneMinutes > 900)
    return false;
  const int doy = DayOfYear(year, month, day);
  if (doy == 0)
    return false;

  const double gamma = 2.0 * kPi / 365.0 * (doy - 1);
  const double eqtime = 229.18 *
                        (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) -
                         0.014615 * std::cos(2.0 * gamma) - 0.040849 * std::sin(2.0 * gamma));
  const double decl =
      0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) -
      0.006758 * std::cos(2.0 * gamma) + 0.000907 * std::sin(2.0 * gamma) -
      0.002697 * std::cos(3.0 * gamma) + 0.00148 * std::sin(3.0 * gamma);

  const double lat = Radians(latitude);
  const double zenith = Radians(90.833);
  const double cosHa = (std::cos(zenith) - std::sin(lat) * std::sin(decl)) /
                       (std::cos(lat) * std::cos(decl));
  if (cosHa < -1.0)
  {
    *alwaysDay = true;
    *sunriseMinute = 0;
    *sunsetMinute = 1439;
    return true;
  }
  if (cosHa > 1.0)
  {
    *alwaysDay = false;
    *sunriseMinute = 720;
    *sunsetMinute = 720;
    return true;
  }

  const double ha = Degrees(std::acos(cosHa));
  const double sunriseUtc = 720.0 - 4.0 * (longitude + ha) - eqtime;
  const double sunsetUtc = 720.0 - 4.0 * (longitude - ha) - eqtime;
  int rise = static_cast<int>(std::floor(sunriseUtc + 0.5)) + timezoneMinutes;
  int set = static_cast<int>(std::floor(sunsetUtc + 0.5)) + timezoneMinutes;
  while (rise < 0)
    rise += 1440;
  while (rise >= 1440)
    rise -= 1440;
  while (set < 0)
    set += 1440;
  while (set >= 1440)
    set -= 1440;
  *sunriseMinute = rise;
  *sunsetMinute = set;
  *alwaysDay = false;
  return true;
}

inline bool IsSolarDay(int minute, int sunrise, int sunset, bool alwaysDay)
{
  if (alwaysDay)
    return true;
  if (sunrise == sunset)
    return false;
  return InClockRange(minute, sunrise, sunset);
}

inline bool VirtualBounds(const Rect *rects, int count, Rect *out)
{
  if (!rects || !out || count <= 0)
    return false;
  Rect value = rects[0];
  if (value.right <= value.left || value.bottom <= value.top)
    return false;
  for (int i = 1; i < count; ++i)
  {
    if (rects[i].right <= rects[i].left || rects[i].bottom <= rects[i].top)
      return false;
    if (rects[i].left < value.left)
      value.left = rects[i].left;
    if (rects[i].top < value.top)
      value.top = rects[i].top;
    if (rects[i].right > value.right)
      value.right = rects[i].right;
    if (rects[i].bottom > value.bottom)
      value.bottom = rects[i].bottom;
  }
  *out = value;
  return true;
}

inline Rect PlaceInCanvas(const Rect &monitor, const Rect &canvas)
{
  Rect out;
  out.left = monitor.left - canvas.left;
  out.top = monitor.top - canvas.top;
  out.right = monitor.right - canvas.left;
  out.bottom = monitor.bottom - canvas.top;
  return out;
}

inline bool HasImageExtension(const char *name)
{
  if (!name)
    return false;
  const char *dot = std::strrchr(name, '.');
  if (!dot)
    return false;
  auto same = [](const char *a, const char *b) {
    while (*a && *b)
    {
      char ca = *a++, cb = *b++;
      if (ca >= 'A' && ca <= 'Z')
        ca = static_cast<char>(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z')
        cb = static_cast<char>(cb - 'A' + 'a');
      if (ca != cb)
        return false;
    }
    return *a == 0 && *b == 0;
  };
  return same(dot, ".jpg") || same(dot, ".jpeg") || same(dot, ".png") || same(dot, ".bmp");
}

} // namespace wallpaper
} // namespace apex

#endif // APEX_WALLPAPER_LOGIC_H
