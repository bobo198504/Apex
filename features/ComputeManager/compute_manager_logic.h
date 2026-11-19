#ifndef APEX_COMPUTE_MANAGER_LOGIC_H
#define APEX_COMPUTE_MANAGER_LOGIC_H

// Pure schedule and JSON helpers. This header has no Windows or ABI dependency so the gate can test it
// without loading the feature DLL.

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace apex {
namespace compute_manager {

enum class Period { Normal, Peak, Valley };

inline bool ParseHhMm(const char *text, int *out)
{
  if (!text || !out)
    return false;
  while (*text == ' ' || *text == '\t')
    ++text;
  if (!std::isdigit((unsigned char)text[0]) || !std::isdigit((unsigned char)text[1]) ||
      text[2] != ':' || !std::isdigit((unsigned char)text[3]) ||
      !std::isdigit((unsigned char)text[4]))
    return false;
  const int hour = (text[0] - '0') * 10 + (text[1] - '0');
  const int minute = (text[3] - '0') * 10 + (text[4] - '0');
  if (hour > 23 || minute > 59)
    return false;
  const char *tail = text + 5;
  while (*tail == ' ' || *tail == '\t' || *tail == '\r' || *tail == '\n')
    ++tail;
  if (*tail)
    return false;
  *out = hour * 60 + minute;
  return true;
}

inline bool ParseHexColor(const char *text, unsigned *rgb)
{
  if (!text || !rgb)
    return false;
  while (*text == ' ' || *text == '\t')
    ++text;
  if (*text == '#')
    ++text;
  unsigned value = 0;
  int digits = 0;
  for (; *text && *text != ' ' && *text != '\t' && *text != '\r' && *text != '\n'; ++text, ++digits)
  {
    if (digits >= 6)
      return false;
    unsigned part = 0;
    if (*text >= '0' && *text <= '9')
      part = (unsigned)(*text - '0');
    else if (*text >= 'a' && *text <= 'f')
      part = (unsigned)(*text - 'a' + 10);
    else if (*text >= 'A' && *text <= 'F')
      part = (unsigned)(*text - 'A' + 10);
    else
      return false;
    value = (value << 4) | part;
  }
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    ++text;
  if (digits != 6 || *text)
    return false;
  *rgb = value;
  return true;
}

inline bool InRange(int minute, int start, int end)
{
  if (start == end)
    return false;
  if (start < end)
    return minute >= start && minute < end;
  return minute >= start || minute < end;
}

inline Period PeriodAt(int minute, int peakStart, int peakEnd, int valleyStart, int valleyEnd)
{
  // Peak wins if a hand-edited schedule overlaps; deterministic beats silently ambiguous.
  if (InRange(minute, peakStart, peakEnd))
    return Period::Peak;
  if (InRange(minute, valleyStart, valleyEnd))
    return Period::Valley;
  return Period::Normal;
}

inline int MinutesUntilPeriodChange(int minute, int peakStart, int peakEnd, int valleyStart, int valleyEnd)
{
  const Period current = PeriodAt(minute, peakStart, peakEnd, valleyStart, valleyEnd);
  for (int ahead = 1; ahead <= 7 * 24 * 60; ++ahead)
  {
    if (PeriodAt((minute + ahead) % (24 * 60), peakStart, peakEnd, valleyStart, valleyEnd) != current)
      return ahead;
  }
  return -1;
}

inline Period PeriodAtWindows(int minute, int weekday, const int *starts, const int *ends, int count,
                              unsigned weekdayMask)
{
  if (!starts || !ends || count <= 0 || weekday < 0 || weekday > 6 ||
      (weekdayMask & (1u << weekday)) == 0)
    return Period::Valley;
  for (int i = 0; i < count; ++i)
    if (InRange(minute, starts[i], ends[i]))
      return Period::Peak;
  return Period::Valley;
}

inline int MinutesUntilWindowsChange(int minute, int weekday, const int *starts, const int *ends, int count,
                                     unsigned weekdayMask)
{
  const Period current = PeriodAtWindows(minute, weekday, starts, ends, count, weekdayMask);
  for (int ahead = 1; ahead <= 7 * 24 * 60; ++ahead)
  {
    const int total = minute + ahead;
    const int nextWeekday = (weekday + total / (24 * 60)) % 7;
    const int nextMinute = total % (24 * 60);
    if (PeriodAtWindows(nextMinute, nextWeekday, starts, ends, count, weekdayMask) != current)
      return ahead;
  }
  return -1;
}

inline const char *PeriodId(Period period)
{
  if (period == Period::Peak)
    return "peak";
  if (period == Period::Valley)
    return "valley";
  return "normal";
}

inline double UsageDelta(double previous, double current, bool *reset)
{
  if (reset)
    *reset = false;
  if (current >= previous)
    return current - previous;
  if (reset)
    *reset = true;
  return current;
}

inline void ExpandModel(const char *src, const char *model, std::string *out)
{
  if (!out)
    return;
  out->clear();
  if (!src)
    return;
  const char *token = "{model}";
  const size_t tokenLen = std::strlen(token);
  for (const char *p = src; *p;)
  {
    if (model && std::strncmp(p, token, tokenLen) == 0)
    {
      *out += model;
      p += tokenLen;
    }
    else
      *out += *p++;
  }
}

inline bool ParseNumberText(const char *text, double *out)
{
  if (!text || !out)
    return false;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n' || *text == ',')
    ++text;
  if (!*text)
    return false;
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(text, &end);
  if (end == text || errno == ERANGE)
    return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' || *end == ',')
    ++end;
  if (*end)
    return false;
  *out = value;
  return true;
}

enum class JsonType { Null, Bool, Number, String, Object, Array };

struct JsonView
{
  const char *begin = nullptr;
  const char *end = nullptr;
  JsonType type = JsonType::Null;
};

inline void SkipSpace(const char **p, const char *end)
{
  while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n'))
    ++*p;
  if (end - *p >= 3 && (unsigned char)(*p)[0] == 0xEF && (unsigned char)(*p)[1] == 0xBB &&
      (unsigned char)(*p)[2] == 0xBF)
    *p += 3;
}

inline bool ScanString(const char *p, const char *end, const char **after)
{
  if (p >= end || *p != '"')
    return false;
  ++p;
  while (p < end)
  {
    if (*p == '"')
    {
      *after = p + 1;
      return true;
    }
    if (*p == '\\')
    {
      ++p;
      if (p >= end)
        return false;
    }
    ++p;
  }
  return false;
}

inline bool ScanValue(const char *begin, const char *end, JsonView *out, int depth = 0);

inline bool ScanContainer(const char *begin, const char *end, char open, char close, JsonView *out,
                          JsonType type, int depth)
{
  if (depth > 64 || begin >= end || *begin != open)
    return false;
  const char *p = begin + 1;
  bool needValue = true;
  while (true)
  {
    SkipSpace(&p, end);
    if (p >= end)
      return false;
    if (*p == close)
    {
      out->begin = begin;
      out->end = p + 1;
      out->type = type;
      return true;
    }
    if (!needValue)
    {
      if (*p != ',')
        return false;
      ++p;
      needValue = true;
      continue;
    }
    if (type == JsonType::Object)
    {
      JsonView key;
      if (!ScanValue(p, end, &key, depth + 1) || key.type != JsonType::String)
        return false;
      p = key.end;
      SkipSpace(&p, end);
      if (p >= end || *p != ':')
        return false;
      ++p;
    }
    JsonView value;
    if (!ScanValue(p, end, &value, depth + 1))
      return false;
    p = value.end;
    needValue = false;
  }
}

inline bool ScanValue(const char *begin, const char *end, JsonView *out, int depth)
{
  if (!out || depth > 64)
    return false;
  SkipSpace(&begin, end);
  if (begin >= end)
    return false;
  const char *after = nullptr;
  if (*begin == '{')
    return ScanContainer(begin, end, '{', '}', out, JsonType::Object, depth);
  if (*begin == '[')
    return ScanContainer(begin, end, '[', ']', out, JsonType::Array, depth);
  if (*begin == '"')
  {
    if (!ScanString(begin, end, &after))
      return false;
    out->begin = begin;
    out->end = after;
    out->type = JsonType::String;
    return true;
  }
  if (end - begin >= 4 && std::memcmp(begin, "true", 4) == 0)
  {
    out->begin = begin;
    out->end = begin + 4;
    out->type = JsonType::Bool;
    return true;
  }
  if (end - begin >= 5 && std::memcmp(begin, "false", 5) == 0)
  {
    out->begin = begin;
    out->end = begin + 5;
    out->type = JsonType::Bool;
    return true;
  }
  if (end - begin >= 4 && std::memcmp(begin, "null", 4) == 0)
  {
    out->begin = begin;
    out->end = begin + 4;
    out->type = JsonType::Null;
    return true;
  }
  const char *p = begin;
  if (*p == '-')
    ++p;
  if (p >= end || !std::isdigit((unsigned char)*p))
    return false;
  while (p < end && std::isdigit((unsigned char)*p))
    ++p;
  if (p < end && *p == '.')
  {
    ++p;
    if (p >= end || !std::isdigit((unsigned char)*p))
      return false;
    while (p < end && std::isdigit((unsigned char)*p))
      ++p;
  }
  if (p < end && (*p == 'e' || *p == 'E'))
  {
    ++p;
    if (p < end && (*p == '+' || *p == '-'))
      ++p;
    if (p >= end || !std::isdigit((unsigned char)*p))
      return false;
    while (p < end && std::isdigit((unsigned char)*p))
      ++p;
  }
  out->begin = begin;
  out->end = p;
  out->type = JsonType::Number;
  return true;
}

inline bool DecodeString(const JsonView &view, char *out, int outSize)
{
  if (!out || outSize <= 0 || view.type != JsonType::String || view.begin + 1 >= view.end)
    return false;
  const char *p = view.begin + 1;
  const char *last = view.end - 1;
  int n = 0;
  while (p < last && n < outSize - 1)
  {
    unsigned char c = (unsigned char)*p++;
    if (c != '\\')
    {
      out[n++] = (char)c;
      continue;
    }
    if (p >= last)
      return false;
    const char esc = *p++;
    switch (esc)
    {
    case '"': out[n++] = '"'; break;
    case '\\': out[n++] = '\\'; break;
    case '/': out[n++] = '/'; break;
    case 'b': out[n++] = '\b'; break;
    case 'f': out[n++] = '\f'; break;
    case 'n': out[n++] = '\n'; break;
    case 'r': out[n++] = '\r'; break;
    case 't': out[n++] = '\t'; break;
    case 'u':
      if (last - p < 4)
        return false;
      p += 4;
      out[n++] = '?';
      break;
    default:
      return false;
    }
  }
  out[n] = 0;
  return p == last;
}

inline bool ObjectMember(const JsonView &object, const char *key, JsonView *out)
{
  if (!out || object.type != JsonType::Object || !key || object.begin + 2 > object.end)
    return false;
  const char *p = object.begin + 1;
  const char *end = object.end - 1;
  while (true)
  {
    SkipSpace(&p, end);
    if (p >= end || *p == '}')
      return false;
    JsonView rawKey;
    if (!ScanValue(p, end, &rawKey) || rawKey.type != JsonType::String)
      return false;
    char decoded[256] = {0};
    if (!DecodeString(rawKey, decoded, (int)sizeof(decoded)))
      return false;
    p = rawKey.end;
    SkipSpace(&p, end);
    if (p >= end || *p != ':')
      return false;
    ++p;
    JsonView value;
    if (!ScanValue(p, end, &value))
      return false;
    if (std::strcmp(decoded, key) == 0)
    {
      *out = value;
      return true;
    }
    p = value.end;
    SkipSpace(&p, end);
    if (p >= end)
      return false;
    if (*p != ',')
      return false;
    ++p;
  }
}

inline bool ArrayItem(const JsonView &array, int index, JsonView *out)
{
  if (!out || array.type != JsonType::Array || index < 0 || array.begin + 2 > array.end)
    return false;
  const char *p = array.begin + 1;
  const char *end = array.end - 1;
  int current = 0;
  while (true)
  {
    SkipSpace(&p, end);
    if (p >= end || *p == ']')
      return false;
    JsonView value;
    if (!ScanValue(p, end, &value))
      return false;
    if (current == index)
    {
      *out = value;
      return true;
    }
    ++current;
    p = value.end;
    SkipSpace(&p, end);
    if (p >= end)
      return false;
    if (*p != ',')
      return false;
    ++p;
  }
}

inline bool JsonPath(const char *json, const char *path, JsonView *out)
{
  if (!json || !path || !out)
    return false;
  const char *end = json + std::strlen(json);
  JsonView current;
  if (!ScanValue(json, end, &current))
    return false;
  const char *p = path;
  while (*p == ' ' || *p == '\t')
    ++p;
  if (*p == '$')
    ++p;
  if (*p == '.')
    ++p;
  if (!*p)
  {
    *out = current;
    return true;
  }
  while (*p)
  {
    if (*p == '[')
    {
      ++p;
      char *numberEnd = nullptr;
      const long index = std::strtol(p, &numberEnd, 10);
      if (numberEnd == p || index < 0 || index > 1000000 || *numberEnd != ']')
        return false;
      p = numberEnd + 1;
      if (!ArrayItem(current, (int)index, &current))
        return false;
    }
    else
    {
      char key[256] = {0};
      int n = 0;
      while (*p && *p != '.' && *p != '[' && n < (int)sizeof(key) - 1)
        key[n++] = *p++;
      key[n] = 0;
      if (!n || !ObjectMember(current, key, &current))
        return false;
    }
    while (*p == ' ' || *p == '\t')
      ++p;
    if (*p == '.')
      ++p;
  }
  *out = current;
  return true;
}

inline bool ExtractJsonNumber(const char *json, const char *path, double *out)
{
  if (!out)
    return false;
  JsonView value;
  if (!JsonPath(json, path, &value))
    return false;
  if (value.type == JsonType::Number)
  {
    char text[128] = {0};
    const int n = (int)(value.end - value.begin);
    if (n <= 0 || n >= (int)sizeof(text))
      return false;
    std::memcpy(text, value.begin, (size_t)n);
    text[n] = 0;
    return ParseNumberText(text, out);
  }
  if (value.type == JsonType::String)
  {
    char text[256] = {0};
    return DecodeString(value, text, (int)sizeof(text)) && ParseNumberText(text, out);
  }
  return false;
}

inline bool ExtractModelIds(const char *json, std::vector<std::string> *out)
{
  if (!json || !out)
    return false;
  out->clear();
  JsonView array;
  bool found = JsonPath(json, "data", &array) && array.type == JsonType::Array;
  if (!found)
    found = JsonPath(json, "models", &array) && array.type == JsonType::Array;
  if (!found)
  {
    const char *end = json + std::strlen(json);
    found = ScanValue(json, end, &array) && array.type == JsonType::Array;
  }
  if (!found)
    return false;

  for (int i = 0; i < 256; ++i)
  {
    JsonView item;
    if (!ArrayItem(array, i, &item))
      break;
    if (item.type == JsonType::String)
    {
      char value[256] = {0};
      if (DecodeString(item, value, (int)sizeof(value)) && value[0])
        out->push_back(value);
      continue;
    }
    if (item.type != JsonType::Object)
      continue;
    JsonView name;
    if (!ObjectMember(item, "id", &name) && !ObjectMember(item, "name", &name))
      continue;
    if (name.type != JsonType::String)
      continue;
    char value[256] = {0};
    if (!DecodeString(name, value, (int)sizeof(value)) || !value[0])
      continue;
    const char *id = std::strncmp(value, "models/", 7) == 0 ? value + 7 : value;
    out->push_back(id);
  }
  return !out->empty();
}

inline bool JsonTime(const char *json, const char *path, int *minute)
{
  JsonView value;
  if (!minute || !JsonPath(json, path, &value))
    return false;
  char text[16] = {0};
  if (value.type == JsonType::String)
  {
    if (!DecodeString(value, text, (int)sizeof(text)))
      return false;
  }
  else if (value.type == JsonType::Number)
  {
    const int n = (int)(value.end - value.begin);
    if (n <= 0 || n >= (int)sizeof(text))
      return false;
    std::memcpy(text, value.begin, (size_t)n);
    text[n] = 0;
  }
  else
    return false;
  return ParseHhMm(text, minute);
}

inline bool ExtractVendorSchedule(const char *json, int *peakStart, int *peakEnd, int *valleyStart,
                                  int *valleyEnd)
{
  if (!json || !peakStart || !peakEnd || !valleyStart || !valleyEnd)
    return false;
  const char *prefixes[] = {"schedule.", "pricing.", "data.schedule."};
  for (const char *prefix : prefixes)
  {
    const std::string ps = std::string(prefix) + "peak_start";
    const std::string pe = std::string(prefix) + "peak_end";
    const std::string vs = std::string(prefix) + "valley_start";
    const std::string ve = std::string(prefix) + "valley_end";
    int a = 0, b = 0, c = 0, d = 0;
    if (JsonTime(json, ps.c_str(), &a) && JsonTime(json, pe.c_str(), &b) &&
        JsonTime(json, vs.c_str(), &c) && JsonTime(json, ve.c_str(), &d))
    {
      *peakStart = a;
      *peakEnd = b;
      *valleyStart = c;
      *valleyEnd = d;
      return true;
    }
  }
  int a = 0, b = 0, c = 0, d = 0;
  if (JsonTime(json, "peak_start", &a) && JsonTime(json, "peak_end", &b) &&
      JsonTime(json, "valley_start", &c) && JsonTime(json, "valley_end", &d))
  {
    *peakStart = a;
    *peakEnd = b;
    *valleyStart = c;
    *valleyEnd = d;
    return true;
  }
  return false;
}

inline bool ParseDeepSeekPeakWindows(const char *text, int *starts, int *ends, int *count,
                                     unsigned *weekdayMask)
{
  if (!text || !starts || !ends || !count || !weekdayMask)
    return false;
  const char *line = std::strstr(text, "Peak hours are");
  if (!line)
    return false;
  const char *endLine = std::strstr(line, "All other hours");
  if (!endLine)
    endLine = line + std::strlen(line);
  int times[8] = {};
  int timeCount = 0;
  for (const char *p = line; p + 5 <= endLine && timeCount < 8; ++p)
  {
    if (std::isdigit((unsigned char)p[0]) && std::isdigit((unsigned char)p[1]) && p[2] == ':' &&
        std::isdigit((unsigned char)p[3]) && std::isdigit((unsigned char)p[4]))
    {
      const int hour = (p[0] - '0') * 10 + (p[1] - '0');
      const int minute = (p[3] - '0') * 10 + (p[4] - '0');
      if (hour <= 23 && minute <= 59)
        times[timeCount++] = hour * 60 + minute;
      p += 4;
    }
  }
  if (timeCount < 2 || (timeCount & 1))
    return false;
  *count = timeCount / 2;
  for (int i = 0; i < *count; ++i)
  {
    starts[i] = times[i * 2];
    ends[i] = times[i * 2 + 1];
  }
  *weekdayMask = std::strstr(line, "Monday through Friday") ? 0x3eu : 0x7fu;
  return true;
}

} // namespace compute_manager
} // namespace apex

#endif // APEX_COMPUTE_MANAGER_LOGIC_H
