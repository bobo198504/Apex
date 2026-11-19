// ComputeManager -- a portable settings-page feature for watching several usage/balance APIs.
//
// The feature owns its HTTP worker, its overlay UI thread, its INI file and every provider row. The host only sees
// the normal ApexFeature ABI; the panel receives generic groups plus live status cards.

#include "../../apex/abi.h"
#include "../../common/match.h"
#include "../../common/system_theme.h"
#include "../../common/taskbar_layout.h"
#include "compute_manager_logic.h"

#include <windows.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

using apex::compute_manager::ExtractJsonNumber;
using apex::compute_manager::ExpandModel;
using apex::compute_manager::ExtractVendorSchedule;
using apex::compute_manager::MinutesUntilPeriodChange;
using apex::compute_manager::MinutesUntilWindowsChange;
using apex::compute_manager::ParseHhMm;
using apex::compute_manager::ParseDeepSeekPeakWindows;
using apex::compute_manager::ParseHexColor;
using apex::compute_manager::ParseNumberText;
using apex::compute_manager::Period;
using apex::compute_manager::PeriodAt;
using apex::compute_manager::PeriodAtWindows;
using apex::compute_manager::UsageDelta;

const char *kId = "ComputeManager";
const char *kVersion = "1.8.2";

// The panel's control document is capped at 64 KiB (apex/settings_ipc.h). These limits are deliberately
// chosen so a full endpoint list still leaves room for the status cards and the group schema.
constexpr int kMaxEndpoints = 12;
constexpr int kMaxName = 64;
constexpr int kMaxModel = 64;
constexpr int kMaxUrl = 384;
constexpr int kMaxKey = 256;
constexpr int kMaxPath = 128;
constexpr int kMaxUnit = 24;
constexpr int kMaxHeader = 192;
constexpr int kMaxBody = 384;
constexpr int kMaxError = 256;
constexpr int kLogCap = 128 * 1024;
constexpr int kMaxProcesses = 16;
constexpr int kMaxProcessName = 96;
constexpr int kMaxPreset = 24;

struct ProviderPreset
{
  const char *id;
  const char *zh;
  const char *en;
  const char *baseUrl;
  const char *modelsUrl;
  const char *usageUrl;
  const char *usagePath;
  const char *balancePath;
  const char *auth;
  const char *authHeader;
  const char *extraHeaders;
};

static const ProviderPreset kPresets[] = {
    {"custom", "自定义接口", "Custom API", "", "", "", "", "", "bearer", "X-API-Key", ""},
    {"openai", "OpenAI", "OpenAI", "https://api.openai.com/v1", "https://api.openai.com/v1/models",
     "", "", "", "bearer", "X-API-Key", ""},
    {"deepseek", "DeepSeek", "DeepSeek", "https://api.deepseek.com",
     "https://api.deepseek.com/models", "https://api.deepseek.com/user/balance", "",
     "balance_infos[0].total_balance", "bearer", "X-API-Key", ""},
    {"moonshot", "Moonshot / Kimi", "Moonshot / Kimi", "https://api.moonshot.cn/v1",
     "https://api.moonshot.cn/v1/models", "https://api.moonshot.cn/v1/users/me/balance", "",
     "data.available_balance", "bearer", "X-API-Key", ""},
    {"zhipu", "智谱 GLM", "Zhipu GLM", "https://open.bigmodel.cn/api/paas/v4",
     "https://open.bigmodel.cn/api/paas/v4/models", "", "", "", "bearer", "X-API-Key", ""},
    {"dashscope", "阿里云百炼 / DashScope", "Alibaba DashScope",
     "https://dashscope.aliyuncs.com/compatible-mode/v1",
     "https://dashscope.aliyuncs.com/compatible-mode/v1/models", "", "", "", "bearer", "X-API-Key", ""},
    {"siliconflow", "硅基流动 SiliconFlow", "SiliconFlow", "https://api.siliconflow.cn/v1",
     "https://api.siliconflow.cn/v1/models", "", "", "", "bearer", "X-API-Key", ""},
    {"openrouter", "OpenRouter", "OpenRouter", "https://openrouter.ai/api/v1",
     "https://openrouter.ai/api/v1/models", "https://openrouter.ai/api/v1/credits", "",
     "data.total_credits", "bearer", "X-API-Key", "HTTP-Referer: Apex;X-Title: ComputeManager"},
    {"groq", "Groq", "Groq", "https://api.groq.com/openai/v1",
     "https://api.groq.com/openai/v1/models", "", "", "", "bearer", "X-API-Key", ""},
    {"together", "Together AI", "Together AI", "https://api.together.xyz/v1",
     "https://api.together.xyz/v1/models", "", "", "", "bearer", "X-API-Key", ""},
    {"anthropic", "Anthropic Claude", "Anthropic Claude", "https://api.anthropic.com/v1",
     "https://api.anthropic.com/v1/models", "", "", "", "header", "x-api-key",
     "anthropic-version: 2023-06-01"}};

struct Endpoint
{
  bool enabled = true;
  bool taskbarVisible = true;
  char name[kMaxName] = {0};
  char model[kMaxModel] = {0};
  char preset[kMaxPreset] = "custom";
  char url[kMaxUrl] = {0};
  char modelsUrl[kMaxUrl] = {0};
  char auth[16] = "bearer";
  char authHeader[64] = "X-API-Key";
  char apiKey[kMaxKey] = {0};
  char method[8] = "GET";
  char body[kMaxBody] = {0};
  char usagePath[kMaxPath] = {0};
  char balancePath[kMaxPath] = {0};
  char unit[kMaxUnit] = "units";
  char currency[8] = "USD";
  char extraHeaders[kMaxHeader] = {0};
  bool scheduleEnabled = true;
  int peakStart = 9 * 60;
  int peakEnd = 18 * 60;
  int peak2Start = -1;
  int peak2End = -1;
  int valleyStart = 0;
  int valleyEnd = 7 * 60;
  unsigned scheduleWeekdayMask = 0x7fu;
  char scheduleUrl[kMaxUrl] = {0};
  unsigned long long scheduleFetchDue = 0;
  char scheduleError[kMaxError] = {0};
  unsigned scheduleMask = 0;

  bool hasUsage = false;
  bool hasBalance = false;
  bool hasRawUsage = false;
  bool lastOk = false;
  double usage = 0.0;
  double balance = 0.0;
  double rawUsage = 0.0;
  double period[3] = {0.0, 0.0, 0.0}; // normal, peak, valley
  int lastStatus = 0;
  unsigned long long lastPollTick = 0;
  char lastError[kMaxError] = {0};
  unsigned long long revision = 0; // bumped by every configuration edit; stale HTTP replies are discarded
  unsigned long long modelFetchDue = 0;
  std::vector<std::string> models;
  char modelsError[kMaxError] = {0};
};

struct ProcessRule
{
  bool enabled = true;
  char name[kMaxProcessName] = {0};
};

struct Settings
{
  bool autoPoll = true;
  bool showAdvanced = false;
  int pollSeconds = 60;
  // Legacy file-level schedule. It is only a migration default for an endpoint that has no four time keys yet.
  int peakStart = 9 * 60;
  int peakEnd = 18 * 60;
  int valleyStart = 0;
  int valleyEnd = 7 * 60;
  int n = 0;
  Endpoint e[kMaxEndpoints] = {};
  int processN = 0;
  ProcessRule process[kMaxProcesses] = {};
  int widgetX = -1;
  int widgetY = -1;
};

Settings g_set;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
CRITICAL_SECTION g_ioLock;
bool g_ioLockReady = false;
const ApexHost *g_host = nullptr;
char g_dir[512] = {0};
HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;
volatile LONG g_stop = 0;
volatile LONG g_uiStop = 0;
HWND g_overlay = nullptr;
HWND g_owner = nullptr;
bool g_isTaskbar = false;
HANDLE g_uiThread = nullptr;
DWORD g_uiThreadId = 0;
HANDLE g_uiReady = nullptr;
unsigned long long g_nextPollTick = 0;
unsigned long long g_nextRevision = 1;
HINTERNET g_session = nullptr;
FILE *g_log = nullptr;
ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;

Period EndpointPeriodAt(const Endpoint &e, const SYSTEMTIME &time);
int EndpointMinutesUntilChange(const Endpoint &e, const SYSTEMTIME &time);
SYSTEMTIME AddMinutes(const SYSTEMTIME &time, int minutes);
COLORREF TaskbarTextColor();
int OverlayTextWidth(HDC dc, HFONT font, const char *text);

void CopyText(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0)
    return;
  size_t n = 0;
  if (src)
  {
    for (size_t i = 0; src[i] && src[i] != '\r' && src[i] != '\n' && n + 1 < cap; ++i)
    {
      const unsigned char c = (unsigned char)src[i];
      if (c < 0x20 && c != '\t')
        continue;
      dst[n++] = (char)c;
    }
  }
  dst[n] = 0;
}

void TrimCopy(char *dst, size_t cap, const char *src)
{
  if (!src)
  {
    if (dst && cap)
      dst[0] = 0;
    return;
  }
  while (*src == ' ' || *src == '\t')
    ++src;
  size_t len = std::strlen(src);
  while (len && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' ||
                 src[len - 1] == '\n'))
    --len;
  if (len >= cap)
    len = cap ? cap - 1 : 0;
  if (dst && cap)
  {
    if (len)
      std::memcpy(dst, src, len);
    dst[len] = 0;
  }
}

bool ParseBool(const char *v, bool *out)
{
  if (!v || !out)
    return false;
  if (!std::strcmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "on") || !_stricmp(v, "yes"))
  {
    *out = true;
    return true;
  }
  if (!std::strcmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no"))
  {
    *out = false;
    return true;
  }
  return false;
}

bool ParseInt(const char *v, int lo, int hi, int *out)
{
  if (!v || !out)
    return false;
  char *end = nullptr;
  const long n = std::strtol(v, &end, 10);
  if (end == v || *end || n < lo || n > hi)
    return false;
  *out = (int)n;
  return true;
}

bool ConfigPath(char *out, int outSize)
{
  if (!g_dir[0])
    return false;
  const int n = _snprintf(out, outSize, "%sComputeManager.ini", g_dir);
  return n > 0 && n < outSize;
}

void LogLine(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[512] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_host->logLine(buf);
}

void OpenLog()
{
  if (!g_dir[0])
    return;
  char path[560] = {0};
  if (_snprintf(path, sizeof(path), "%scomputemanager.log", g_dir) > 0)
    g_log = fopen(path, "w");
}

void WriteLog(const char *fmt, ...)
{
  if (!g_log)
    return;
  char buf[512] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(g_log, "%02d:%02d:%02d  %s\n", st.wHour, st.wMinute, st.wSecond, buf);
  if (ftell(g_log) > kLogCap)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  else
    fflush(g_log);
}

const ProviderPreset *FindPreset(const char *id)
{
  if (!id)
    return nullptr;
  for (const ProviderPreset &p : kPresets)
    if (!std::strcmp(p.id, id))
      return &p;
  return nullptr;
}

void ScheduleModelFetch(Endpoint &e)
{
  if (!e.modelsUrl[0] || !e.apiKey[0] || !std::strcmp(e.preset, "custom"))
  {
    e.modelFetchDue = 0;
    return;
  }
  e.modelFetchDue = GetTickCount64() + 700;
}

const char *PresetCurrency(const char *preset)
{
  if (!preset)
    return "USD";
  if (!std::strcmp(preset, "deepseek") || !std::strcmp(preset, "moonshot") ||
      !std::strcmp(preset, "zhipu") || !std::strcmp(preset, "dashscope") ||
      !std::strcmp(preset, "siliconflow"))
    return "CNY";
  return "USD";
}

const char *PresetScheduleUrl(const char *preset)
{
  if (preset && !std::strcmp(preset, "deepseek"))
    return "https://api-docs.deepseek.com/quick_start/pricing.md";
  return "";
}

bool LooksLikeDeepSeek(const Endpoint &e)
{
  return !_stricmp(e.preset, "deepseek") || !_strnicmp(e.name, "deepseek", 8) ||
         std::strstr(e.url, "api.deepseek.com") != nullptr;
}

bool ApplyPreset(Endpoint &e, const char *value)
{
  const ProviderPreset *p = FindPreset(value);
  if (!p)
    return false;
  CopyText(e.preset, sizeof(e.preset), p->id);
  if (!std::strcmp(p->id, "custom"))
  {
    CopyText(e.currency, sizeof(e.currency), "USD");
    e.scheduleEnabled = false;
    e.scheduleUrl[0] = 0;
    e.scheduleFetchDue = 0;
    e.peak2Start = -1;
    e.peak2End = -1;
    e.scheduleWeekdayMask = 0x7fu;
    e.modelFetchDue = 0;
  }
  else
  {
    CopyText(e.url, sizeof(e.url), p->usageUrl);
    CopyText(e.modelsUrl, sizeof(e.modelsUrl), p->modelsUrl);
    CopyText(e.auth, sizeof(e.auth), p->auth);
    CopyText(e.authHeader, sizeof(e.authHeader), p->authHeader);
    CopyText(e.usagePath, sizeof(e.usagePath), p->usagePath);
    CopyText(e.balancePath, sizeof(e.balancePath), p->balancePath);
    CopyText(e.extraHeaders, sizeof(e.extraHeaders), p->extraHeaders);
    CopyText(e.currency, sizeof(e.currency), PresetCurrency(p->id));
    e.scheduleEnabled = false;
    CopyText(e.scheduleUrl, sizeof(e.scheduleUrl), PresetScheduleUrl(p->id));
    e.scheduleFetchDue = e.scheduleUrl[0] ? GetTickCount64() : 0;
    e.peak2Start = -1;
    e.peak2End = -1;
    e.scheduleWeekdayMask = 0x7fu;
    e.models.clear();
    e.modelsError[0] = 0;
    ScheduleModelFetch(e);
  }
  return true;
}

bool SetEndpointField(Endpoint &e, const char *field, const char *value)
{
  if (!field || !value)
    return false;
  if (!std::strcmp(field, "enabled"))
  {
    bool v = false;
    if (!ParseBool(value, &v))
      return false;
    e.enabled = v;
    return true;
  }
  if (!std::strcmp(field, "taskbar_visible"))
  {
    bool v = false;
    if (!ParseBool(value, &v))
      return false;
    e.taskbarVisible = v;
    return true;
  }
  if (!std::strcmp(field, "preset"))
    return ApplyPreset(e, value);
  if (!std::strcmp(field, "schedule_enabled"))
  {
    bool enabled = false;
    if (!ParseBool(value, &enabled))
      return false;
    e.scheduleEnabled = enabled;
    return true;
  }
  if (std::strpbrk(value, "\r\n"))
    return false;
  if (!std::strcmp(field, "name"))
  {
    char name[kMaxName] = {0};
    TrimCopy(name, sizeof(name), value);
    if (!name[0])
      return false;
    CopyText(e.name, sizeof(e.name), name);
    return true;
  }
  if (!std::strcmp(field, "model"))
  {
    CopyText(e.model, sizeof(e.model), value);
    return true;
  }
  if (!std::strcmp(field, "url"))
  {
    CopyText(e.url, sizeof(e.url), value);
    ScheduleModelFetch(e);
    return true;
  }
  if (!std::strcmp(field, "models_url"))
  {
    CopyText(e.modelsUrl, sizeof(e.modelsUrl), value);
    ScheduleModelFetch(e);
    return true;
  }
  if (!std::strcmp(field, "schedule_url"))
  {
    CopyText(e.scheduleUrl, sizeof(e.scheduleUrl), value);
    e.scheduleFetchDue = e.scheduleUrl[0] ? GetTickCount64() : 0;
    return true;
  }
  if (!std::strcmp(field, "schedule_weekday_mask"))
  {
    int mask = 0;
    if (!ParseInt(value, 0, 127, &mask))
      return false;
    e.scheduleWeekdayMask = (unsigned)mask;
    return true;
  }
  if (!std::strcmp(field, "models"))
  {
    e.models.clear();
    const char *p = value;
    while (*p && e.models.size() < 64)
    {
      const char *end = std::strchr(p, '|');
      const size_t len = end ? (size_t)(end - p) : std::strlen(p);
      if (len && len < 256)
        e.models.emplace_back(p, len);
      p = end ? end + 1 : p + len;
    }
    return true;
  }
  if (!std::strcmp(field, "auth"))
  {
    if (std::strcmp(value, "none") && std::strcmp(value, "bearer") && std::strcmp(value, "header"))
      return false;
    CopyText(e.auth, sizeof(e.auth), value);
    return true;
  }
  if (!std::strcmp(field, "auth_header"))
  {
    char v[64] = {0};
    TrimCopy(v, sizeof(v), value);
    if (!v[0])
      return false;
    CopyText(e.authHeader, sizeof(e.authHeader), v);
    return true;
  }
  if (!std::strcmp(field, "api_key"))
  {
    if (!std::strcmp(value, "********"))
      return true; // the page's mask is not a new key
    CopyText(e.apiKey, sizeof(e.apiKey), value);
    ScheduleModelFetch(e);
    return true;
  }
  if (!std::strcmp(field, "method"))
  {
    if (_stricmp(value, "GET") && _stricmp(value, "POST"))
      return false;
    CopyText(e.method, sizeof(e.method), _stricmp(value, "POST") ? "GET" : "POST");
    return true;
  }
  if (!std::strcmp(field, "body"))
  {
    CopyText(e.body, sizeof(e.body), value);
    return true;
  }
  if (!std::strcmp(field, "usage_path"))
  {
    CopyText(e.usagePath, sizeof(e.usagePath), value);
    return true;
  }
  if (!std::strcmp(field, "balance_path"))
  {
    CopyText(e.balancePath, sizeof(e.balancePath), value);
    return true;
  }
  if (!std::strcmp(field, "unit"))
  {
    CopyText(e.unit, sizeof(e.unit), value);
    return true;
  }
  if (!std::strcmp(field, "currency"))
  {
    if (std::strcmp(value, "CNY") && std::strcmp(value, "USD"))
      return false;
    CopyText(e.currency, sizeof(e.currency), value);
    return true;
  }
  if (!std::strcmp(field, "extra_headers"))
  {
    CopyText(e.extraHeaders, sizeof(e.extraHeaders), value);
    return true;
  }
  if (!std::strcmp(field, "peak_start") || !std::strcmp(field, "peak_end") ||
      !std::strcmp(field, "valley_start") || !std::strcmp(field, "valley_end") ||
      !std::strcmp(field, "peak2_start") || !std::strcmp(field, "peak2_end"))
  {
    int minute = 0;
    if (!ParseHhMm(value, &minute))
      return false;
    if (!std::strcmp(field, "peak_start"))
    {
      e.peakStart = minute;
      e.scheduleMask |= 1u;
    }
    else if (!std::strcmp(field, "peak_end"))
    {
      e.peakEnd = minute;
      e.scheduleMask |= 2u;
    }
    else if (!std::strcmp(field, "valley_start"))
    {
      e.valleyStart = minute;
      e.scheduleMask |= 4u;
    }
    else if (!std::strcmp(field, "valley_end"))
    {
      e.valleyEnd = minute;
      e.scheduleMask |= 8u;
    }
    else if (!std::strcmp(field, "peak2_start"))
      e.peak2Start = minute;
    else
      e.peak2End = minute;
    return true;
  }
  return false;
}

bool SetProcessField(ProcessRule &p, const char *field, const char *value)
{
  if (!field || !value)
    return false;
  if (!std::strcmp(field, "enabled"))
  {
    bool v = false;
    if (!ParseBool(value, &v))
      return false;
    p.enabled = v;
    return true;
  }
  if (!std::strcmp(field, "name"))
  {
    char name[kMaxProcessName] = {0};
    apex::match::NormaliseName(value, name, (int)sizeof(name));
    if (!name[0])
      return false;
    CopyText(p.name, sizeof(p.name), name);
    return true;
  }
  return false;
}

bool SetRuntimeField(Endpoint &e, const char *field, const char *value)
{
  if (!field || !value)
    return false;
  if (!std::strcmp(field, "has_usage") || !std::strcmp(field, "has_balance") ||
      !std::strcmp(field, "has_raw_usage") || !std::strcmp(field, "last_ok"))
  {
    bool v = false;
    if (!ParseBool(value, &v))
      return false;
    if (!std::strcmp(field, "has_usage"))
      e.hasUsage = v;
    else if (!std::strcmp(field, "has_balance"))
      e.hasBalance = v;
    else if (!std::strcmp(field, "has_raw_usage"))
      e.hasRawUsage = v;
    else
      e.lastOk = v;
    return true;
  }
  if (!std::strcmp(field, "usage") || !std::strcmp(field, "balance") ||
      !std::strcmp(field, "raw_usage"))
  {
    double v = 0;
    if (!ParseNumberText(value, &v))
      return false;
    if (!std::strcmp(field, "usage"))
      e.usage = v;
    else if (!std::strcmp(field, "balance"))
      e.balance = v;
    else
      e.rawUsage = v;
    return true;
  }
  if (!std::strcmp(field, "period_normal") || !std::strcmp(field, "period_peak") ||
      !std::strcmp(field, "period_valley"))
  {
    double v = 0;
    if (!ParseNumberText(value, &v))
      return false;
    if (!std::strcmp(field, "period_normal"))
      e.period[0] = v;
    else if (!std::strcmp(field, "period_peak"))
      e.period[1] = v;
    else
      e.period[2] = v;
    return true;
  }
  if (!std::strcmp(field, "last_status"))
  {
    int v = 0;
    if (!ParseInt(value, 0, 999, &v))
      return false;
    e.lastStatus = v;
    return true;
  }
  if (!std::strcmp(field, "last_error"))
  {
    CopyText(e.lastError, sizeof(e.lastError), value);
    return true;
  }
  if (!std::strcmp(field, "schedule_error"))
  {
    CopyText(e.scheduleError, sizeof(e.scheduleError), value);
    return true;
  }
  return false;
}

void LoadSettings()
{
  if (g_ioLockReady)
    EnterCriticalSection(&g_ioLock);
  Settings fresh;
  char path[560] = {0};
  if (ConfigPath(path, (int)sizeof(path)))
  {
    FILE *f = fopen(path, "rb");
    if (f)
    {
      char line[2048] = {0};
      while (fgets(line, sizeof(line) - 1, f))
      {
        if (line[0] == '#')
          continue;
        char *eq = std::strchr(line, '=');
        if (!eq)
          continue;
        *eq = 0;
        char *key = line;
        char *value = eq + 1;
        while (*key == ' ' || *key == '\t')
          ++key;
        char *keyEnd = key + std::strlen(key);
        while (keyEnd > key && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t' || keyEnd[-1] == '\r'))
          *--keyEnd = 0;
        while (*value == ' ' || *value == '\t')
          ++value;
        char *valueEnd = value + std::strlen(value);
        while (valueEnd > value && (valueEnd[-1] == '\r' || valueEnd[-1] == '\n' ||
                                    valueEnd[-1] == ' ' || valueEnd[-1] == '\t'))
          *--valueEnd = 0;

        if (!std::strcmp(key, "auto_poll"))
          ParseBool(value, &fresh.autoPoll);
        else if (!std::strcmp(key, "show_advanced"))
          ParseBool(value, &fresh.showAdvanced);
        else if (!std::strcmp(key, "poll_seconds"))
          ParseInt(value, 1, 60, &fresh.pollSeconds);
        else if (!std::strcmp(key, "peak_start"))
          ParseHhMm(value, &fresh.peakStart);
        else if (!std::strcmp(key, "peak_end"))
          ParseHhMm(value, &fresh.peakEnd);
        else if (!std::strcmp(key, "valley_start"))
          ParseHhMm(value, &fresh.valleyStart);
        else if (!std::strcmp(key, "valley_end"))
          ParseHhMm(value, &fresh.valleyEnd);
        else if (!std::strncmp(key, "endpoint.", 9))
        {
          char *indexEnd = nullptr;
          const long index = std::strtol(key + 9, &indexEnd, 10);
          if (indexEnd == key + 9 || *indexEnd != '.' || index < 0 || index >= kMaxEndpoints)
            continue;
          const char *field = indexEnd + 1;
          if ((int)index + 1 > fresh.n)
            fresh.n = (int)index + 1;
          SetEndpointField(fresh.e[index], field, value);
          SetRuntimeField(fresh.e[index], field, value);
        }
        else if (!std::strncmp(key, "process.", 8))
        {
          char *indexEnd = nullptr;
          const long index = std::strtol(key + 8, &indexEnd, 10);
          if (indexEnd == key + 8 || *indexEnd != '.' || index < 0 || index >= kMaxProcesses)
            continue;
          const char *field = indexEnd + 1;
          if ((int)index + 1 > fresh.processN)
            fresh.processN = (int)index + 1;
          SetProcessField(fresh.process[index], field, value);
        }
        else if (!std::strcmp(key, "widget.x"))
          ParseInt(value, -100000, 100000, &fresh.widgetX);
        else if (!std::strcmp(key, "widget.y"))
          ParseInt(value, -100000, 100000, &fresh.widgetY);
      }
      fclose(f);
    }
  }
  for (int i = 0; i < fresh.n; ++i)
  {
    Endpoint &e = fresh.e[i];
    if (!(e.scheduleMask & 1u))
      e.peakStart = fresh.peakStart;
    if (!(e.scheduleMask & 2u))
      e.peakEnd = fresh.peakEnd;
    if (!(e.scheduleMask & 4u))
      e.valleyStart = fresh.valleyStart;
    if (!(e.scheduleMask & 8u))
      e.valleyEnd = fresh.valleyEnd;
    // Old builds wrote schedule_enabled=0 after applying a preset and then loaded that row with
    // schedule_fetch_due=0. FetchPendingSchedules ignores a zero deadline, so the official DeepSeek
    // pricing page was never read. A provider URL is itself the request to fetch it; do that once on
    // every start, and let the worker schedule a retry if the network or document format fails.
    if (LooksLikeDeepSeek(e) && !e.scheduleUrl[0])
      CopyText(e.scheduleUrl, sizeof(e.scheduleUrl), PresetScheduleUrl("deepseek"));
    if (e.scheduleUrl[0])
    {
      e.scheduleFetchDue = GetTickCount64();
    }
  }
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < fresh.n; ++i)
    fresh.e[i].revision = ++g_nextRevision;
  g_set = fresh;
  g_nextPollTick = 0;
  LeaveCriticalSection(&g_lock);
  if (g_ioLockReady)
    LeaveCriticalSection(&g_ioLock);
}

void WriteDouble(FILE *f, const char *key, double value)
{
  fprintf(f, "%s=%.17g\n", key, value);
}

bool SaveSettings()
{
  if (g_ioLockReady)
    EnterCriticalSection(&g_ioLock);
  char path[560] = {0};
  if (!ConfigPath(path, (int)sizeof(path)))
  {
    if (g_ioLockReady)
      LeaveCriticalSection(&g_ioLock);
    return false;
  }
  char tmp[600] = {0};
  if (_snprintf(tmp, sizeof(tmp), "%s.tmp", path) <= 0)
  {
    if (g_ioLockReady)
      LeaveCriticalSection(&g_ioLock);
    return false;
  }
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);
  FILE *f = fopen(tmp, "wb");
  if (!f)
  {
    if (g_ioLockReady)
      LeaveCriticalSection(&g_ioLock);
    return false;
  }
  fprintf(f, "auto_poll=%d\n", s.autoPoll ? 1 : 0);
  fprintf(f, "show_advanced=%d\n", s.showAdvanced ? 1 : 0);
  fprintf(f, "poll_seconds=%d\n", s.pollSeconds);
  fprintf(f, "peak_start=%02d:%02d\n", s.peakStart / 60, s.peakStart % 60);
  fprintf(f, "peak_end=%02d:%02d\n", s.peakEnd / 60, s.peakEnd % 60);
  fprintf(f, "valley_start=%02d:%02d\n", s.valleyStart / 60, s.valleyStart % 60);
  fprintf(f, "valley_end=%02d:%02d\n", s.valleyEnd / 60, s.valleyEnd % 60);
  fprintf(f, "widget.x=%d\n", s.widgetX);
  fprintf(f, "widget.y=%d\n", s.widgetY);
  for (int i = 0; i < s.n; ++i)
  {
    const Endpoint &e = s.e[i];
    char prefix[64] = {0};
    _snprintf(prefix, sizeof(prefix), "endpoint.%d.", i);
    fprintf(f, "%senabled=%d\n", prefix, e.enabled ? 1 : 0);
    fprintf(f, "%staskbar_visible=%d\n", prefix, e.taskbarVisible ? 1 : 0);
    fprintf(f, "%sname=%s\n", prefix, e.name);
    fprintf(f, "%smodel=%s\n", prefix, e.model);
    fprintf(f, "%spreset=%s\n", prefix, e.preset);
    fprintf(f, "%surl=%s\n", prefix, e.url);
    fprintf(f, "%smodels_url=%s\n", prefix, e.modelsUrl);
    fprintf(f, "%smodels=", prefix);
    for (size_t mi = 0; mi < e.models.size(); ++mi)
      fprintf(f, "%s%s", mi ? "|" : "", e.models[mi].c_str());
    fprintf(f, "\n");
    fprintf(f, "%sauth=%s\n", prefix, e.auth);
    fprintf(f, "%sauth_header=%s\n", prefix, e.authHeader);
    fprintf(f, "%sapi_key=%s\n", prefix, e.apiKey);
    fprintf(f, "%smethod=%s\n", prefix, e.method);
    fprintf(f, "%sbody=%s\n", prefix, e.body);
    fprintf(f, "%susage_path=%s\n", prefix, e.usagePath);
    fprintf(f, "%sbalance_path=%s\n", prefix, e.balancePath);
    fprintf(f, "%sunit=%s\n", prefix, e.unit);
    fprintf(f, "%scurrency=%s\n", prefix, e.currency);
    fprintf(f, "%sextra_headers=%s\n", prefix, e.extraHeaders);
    fprintf(f, "%sschedule_url=%s\n", prefix, e.scheduleUrl);
    fprintf(f, "%sschedule_enabled=%d\n", prefix, e.scheduleEnabled ? 1 : 0);
    fprintf(f, "%speak_start=%02d:%02d\n", prefix, e.peakStart / 60, e.peakStart % 60);
    fprintf(f, "%speak_end=%02d:%02d\n", prefix, e.peakEnd / 60, e.peakEnd % 60);
    fprintf(f, "%svalley_start=%02d:%02d\n", prefix, e.valleyStart / 60, e.valleyStart % 60);
    fprintf(f, "%svalley_end=%02d:%02d\n", prefix, e.valleyEnd / 60, e.valleyEnd % 60);
    if (e.peak2Start >= 0 && e.peak2End >= 0)
    {
      fprintf(f, "%speak2_start=%02d:%02d\n", prefix, e.peak2Start / 60, e.peak2Start % 60);
      fprintf(f, "%speak2_end=%02d:%02d\n", prefix, e.peak2End / 60, e.peak2End % 60);
    }
    fprintf(f, "%sschedule_weekday_mask=%u\n", prefix, e.scheduleWeekdayMask);
    fprintf(f, "%shas_usage=%d\n", prefix, e.hasUsage ? 1 : 0);
    fprintf(f, "%shas_balance=%d\n", prefix, e.hasBalance ? 1 : 0);
    fprintf(f, "%shas_raw_usage=%d\n", prefix, e.hasRawUsage ? 1 : 0);
    fprintf(f, "%slast_ok=%d\n", prefix, e.lastOk ? 1 : 0);
    {
      char key[96] = {0};
      _snprintf(key, sizeof(key), "%susage", prefix);
      WriteDouble(f, key, e.usage);
      _snprintf(key, sizeof(key), "%sbalance", prefix);
      WriteDouble(f, key, e.balance);
      _snprintf(key, sizeof(key), "%sraw_usage", prefix);
      WriteDouble(f, key, e.rawUsage);
      _snprintf(key, sizeof(key), "%speriod_normal", prefix);
      WriteDouble(f, key, e.period[0]);
      _snprintf(key, sizeof(key), "%speriod_peak", prefix);
      WriteDouble(f, key, e.period[1]);
      _snprintf(key, sizeof(key), "%speriod_valley", prefix);
      WriteDouble(f, key, e.period[2]);
    }
    fprintf(f, "%slast_status=%d\n", prefix, e.lastStatus);
    fprintf(f, "%slast_error=%s\n", prefix, e.lastError);
    fprintf(f, "%sschedule_error=%s\n", prefix, e.scheduleError);
  }
  for (int i = 0; i < s.processN; ++i)
  {
    char prefix[64] = {0};
    _snprintf(prefix, sizeof(prefix), "process.%d.", i);
    fprintf(f, "%senabled=%d\n", prefix, s.process[i].enabled ? 1 : 0);
    fprintf(f, "%sname=%s\n", prefix, s.process[i].name);
  }
  bool ok = ferror(f) == 0;
  if (fclose(f) != 0)
    ok = false;
  if (ok)
    ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
  if (!ok)
    DeleteFileA(tmp);
  if (g_ioLockReady)
    LeaveCriticalSection(&g_ioLock);
  return ok;
}

void AppendText(char *out, int outSize, int &off, const char *fmt, ...)
{
  if (!out || off < 0 || off >= outSize)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + off, outSize - off, fmt, ap);
  va_end(ap);
  if (n > 0)
    off += n;
  if (off >= outSize)
    off = outSize;
}

void AppendJsonString(char *out, int outSize, int &off, const char *s)
{
  AppendText(out, outSize, off, "\"");
  if (!s)
  {
    AppendText(out, outSize, off, "\"");
    return;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
  {
    switch (*p)
    {
    case '"': AppendText(out, outSize, off, "\\\""); break;
    case '\\': AppendText(out, outSize, off, "\\\\"); break;
    case '\b': AppendText(out, outSize, off, "\\b"); break;
    case '\f': AppendText(out, outSize, off, "\\f"); break;
    case '\n': AppendText(out, outSize, off, "\\n"); break;
    case '\r': AppendText(out, outSize, off, "\\r"); break;
    case '\t': AppendText(out, outSize, off, "\\t"); break;
    default:
      if (*p < 0x20)
        AppendText(out, outSize, off, "\\u%04x", *p);
      else
        AppendText(out, outSize, off, "%c", *p);
      break;
    }
  }
  AppendText(out, outSize, off, "\"");
}

void FormatNumber(char *out, int outSize, double value)
{
  if (value >= 1000000.0 || value <= -1000000.0)
    _snprintf(out, outSize, "%.0f", value);
  else
    _snprintf(out, outSize, "%.6g", value);
}

void FormatBalance(char *out, int outSize, const Endpoint &e)
{
  char value[64] = {0};
  FormatNumber(value, sizeof(value), e.hasBalance ? e.balance : 0);
  if (!std::strcmp(e.currency, "CNY"))
    _snprintf(out, outSize, "¥%s", e.hasBalance ? value : "--");
  else
    _snprintf(out, outSize, "$%s", e.hasBalance ? value : "--");
}

void FormatCountdown(char *out, int outSize, int minutes, Period next, bool chinese)
{
  const int days = minutes / (24 * 60);
  const int hours = (minutes / 60) % 24;
  const int mins = minutes % 60;
  const char *periodZh = next == Period::Peak ? "峰期" : (next == Period::Valley ? "谷期" : "平期");
  const char *periodEn = next == Period::Peak ? "peak" : (next == Period::Valley ? "valley" : "normal");
  if (chinese)
  {
    if (days)
      _snprintf(out, outSize, "距下一%s还有%d天%d时%02d分", periodZh, days, hours, mins);
    else if (hours)
      _snprintf(out, outSize, "距下一%s还有%d时%02d分", periodZh, hours, mins);
    else
      _snprintf(out, outSize, "距下一%s还有%d分", periodZh, mins);
  }
  else
  {
    if (days)
      _snprintf(out, outSize, "next %s in %dd %dh %02dm", periodEn, days, hours, mins);
    else if (hours)
      _snprintf(out, outSize, "next %s in %dh %02dm", periodEn, hours, mins);
    else
      _snprintf(out, outSize, "next %s in %dm", periodEn, mins);
  }
}

void AddTextField(char *out, int outSize, int &off, const char *id, const char *zh,
                  const char *en, const char *placeholderZh, const char *placeholderEn,
                  const char *value = nullptr)
{
  AppendText(out, outSize, off, "%s{\"id\":", off ? "" : "");
  AppendJsonString(out, outSize, off, id);
  AppendText(out, outSize, off, ",\"type\":\"text\",\"labelZh\":");
  AppendJsonString(out, outSize, off, zh);
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, en);
  if (placeholderZh)
  {
    AppendText(out, outSize, off, ",\"placeholderZh\":");
    AppendJsonString(out, outSize, off, placeholderZh);
  }
  if (placeholderEn)
  {
    AppendText(out, outSize, off, ",\"placeholderEn\":");
    AppendJsonString(out, outSize, off, placeholderEn);
  }
  if (value)
  {
    AppendText(out, outSize, off, ",\"value\":");
    AppendJsonString(out, outSize, off, value);
  }
  AppendText(out, outSize, off, "}");
}

void AddSelectField(char *out, int outSize, int &off, const char *id, const char *zh,
                    const char *en, const char *const *values, const char *const *zhLabels,
                    const char *const *enLabels, int count, bool deferRefresh = false)
{
  AppendText(out, outSize, off, "{\"id\":");
  AppendJsonString(out, outSize, off, id);
  AppendText(out, outSize, off, ",\"type\":\"select\",\"labelZh\":");
  AppendJsonString(out, outSize, off, zh);
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, en);
  AppendText(out, outSize, off, ",\"options\":[");
  for (int i = 0; i < count; ++i)
  {
    AppendText(out, outSize, off, "%s{\"value\":", i ? "," : "");
    AppendJsonString(out, outSize, off, values[i]);
    AppendText(out, outSize, off, ",\"labelZh\":");
    AppendJsonString(out, outSize, off, zhLabels[i]);
    AppendText(out, outSize, off, ",\"labelEn\":");
    AppendJsonString(out, outSize, off, enLabels[i]);
    AppendText(out, outSize, off, "}");
  }
  AppendText(out, outSize, off, "]");
  if (deferRefresh)
    AppendText(out, outSize, off, ",\"deferRefresh\":true");
  AppendText(out, outSize, off, "}");
}

const char *PeriodZh(Period p)
{
  if (p == Period::Peak)
    return "峰期";
  if (p == Period::Valley)
    return "谷期";
  return "平期";
}

const char *PeriodEn(Period p)
{
  if (p == Period::Peak)
    return "peak";
  if (p == Period::Valley)
    return "valley";
  return "off-peak";
}

int SettingsJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  SYSTEMTIME st;
  GetLocalTime(&st);

  int off = 0;
  AppendText(out, outSize, off, "{\"params\":[");

  bool firstParam = true;

  // ONE COMPACT STATUS LINE PER ENABLED ENDPOINT. There is no aggregate card and no details block: the
  // information the user needs is the name, balance, current/next peak state, and the taskbar switch.
  for (int i = 0; i < s.n; ++i)
  {
    const Endpoint &e = s.e[i];
    if (!e.enabled)
      continue;
    char zh[512] = {0};
    char en[512] = {0};
    const char *rowLevel = e.lastOk ? "ok" : (e.lastError[0] || e.lastStatus ? "error" : "warn");
    const Period endpointPeriod = EndpointPeriodAt(e, st);
    if (!e.lastOk)
    {
      _snprintf(zh, sizeof(zh), "%s · %s", e.name, e.lastError[0] ? e.lastError : "尚未连接");
      _snprintf(en, sizeof(en), "%s · %s", e.name, e.lastError[0] ? e.lastError : "not connected");
    }
    else
    {
      char balance[96] = {0};
      FormatBalance(balance, sizeof(balance), e);
      if (e.scheduleEnabled)
      {
        const int minutes = EndpointMinutesUntilChange(e, st);
        const Period nextPeriod = minutes >= 0 ? EndpointPeriodAt(e, AddMinutes(st, minutes)) : Period::Normal;
        const int hours = minutes > 0 ? minutes / 60 : 0;
        const int mins = minutes > 0 ? minutes % 60 : 0;
        _snprintf(zh, sizeof(zh), "%s · 余额 %s · %s · 距%s %d:%02d",
                  e.name, balance, PeriodZh(endpointPeriod), PeriodZh(nextPeriod), hours, mins);
        _snprintf(en, sizeof(en), "%s · balance %s · %s · %s in %d:%02d",
                  e.name, balance, PeriodEn(endpointPeriod), PeriodEn(nextPeriod), hours, mins);
      }
      else if (e.hasUsage)
      {
        _snprintf(zh, sizeof(zh), "%s · 余额 %s", e.name, balance);
        _snprintf(en, sizeof(en), "%s · balance %s", e.name, balance);
      }
      else
      {
        _snprintf(zh, sizeof(zh), "%s · 余额 %s", e.name, balance);
        _snprintf(en, sizeof(en), "%s · balance %s", e.name, balance);
      }
    }
    AppendText(out, outSize, off, "%s{\"id\":\"endpoint_%d\",\"type\":\"status\",\"level\":\"%s\",\"textZh\":",
               firstParam ? "" : ",", i, rowLevel);
    AppendJsonString(out, outSize, off, zh);
    AppendText(out, outSize, off, ",\"textEn\":");
    AppendJsonString(out, outSize, off, en);
    AppendText(out, outSize, off,
               ",\"toggleId\":\"accounts[%d].taskbar_visible\","
               "\"toggleLabelZh\":\"任务栏显示\",\"toggleLabelEn\":\"Show on taskbar\","
               "\"toggleValue\":%d}",
               i, e.taskbarVisible ? 1 : 0);
    firstParam = false;
  }

  AppendText(out, outSize, off, "%s{\"id\":\"refresh_row\",\"type\":\"row\",\"controls\":["
                               "{\"id\":\"auto_poll\",\"type\":\"bool\",\"labelZh\":\"自动刷新\",\"labelEn\":\"Auto refresh\",\"value\":%d},",
             firstParam ? "" : ",", s.autoPoll ? 1 : 0);
  firstParam = false;
  AppendText(out, outSize, off,
             "{\"id\":\"poll_seconds\",\"type\":\"text\",\"input\":\"number\",\"compact\":true,"
             "\"min\":1,\"max\":60,\"labelZh\":\"刷新间隔\",\"labelEn\":\"Interval\","
             "\"suffixZh\":\"秒\",\"suffixEn\":\"s\",\"value\":\"%d\"}",
             s.pollSeconds);
  AppendText(out, outSize, off, "]}");
  AppendText(out, outSize, off, "%s{\"id\":\"show_advanced\",\"type\":\"bool\",\"labelZh\":\"显示高级接口字段\",\"labelEn\":\"Show advanced API fields\",\"value\":%d}",
             firstParam ? "" : ",", s.showAdvanced ? 1 : 0);
  firstParam = false;

  AppendText(out, outSize, off, "%s{\"id\":\"accounts\",\"type\":\"group\",\"labelZh\":\"接口与模型\",\"labelEn\":\"APIs and models\",\"layout\":\"rows\",\"fieldsWrap\":true,\"hideCount\":true,\"rowToggle\":[\"enabled\"],\"addHintZh\":\"例如 DeepSeek\",\"addHintEn\":\"e.g. DeepSeek\",\"fields\":[",
             firstParam ? "" : ",");
  firstParam = false;
  bool advancedFields = s.showAdvanced;
  bool modelFetchPending = false;
  for (int i = 0; i < s.n; ++i)
  {
    if (!s.e[i].preset[0] || !std::strcmp(s.e[i].preset, "custom"))
      advancedFields = true;
    if (s.e[i].modelFetchDue)
      modelFetchPending = true;
  }
  AddTextField(out, outSize, off, "name", "名称", "Name", "接口名称", "Endpoint name");
  AppendText(out, outSize, off, ",");
  {
    constexpr int presetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
    const char *values[presetCount] = {};
    const char *zh[presetCount] = {};
    const char *en[presetCount] = {};
    for (int i = 0; i < presetCount; ++i)
    {
      values[i] = kPresets[i].id;
      zh[i] = kPresets[i].zh;
      en[i] = kPresets[i].en;
    }
    AddSelectField(out, outSize, off, "preset", "厂商 / 接口预设", "Provider preset", values, zh, en,
                   presetCount, true);
  }
  AppendText(out, outSize, off, ",");
  AppendText(out, outSize, off,
             "{\"id\":\"models\",\"type\":\"list\",\"labelZh\":\"子模型\",\"labelEn\":\"Sub-models\","
             "\"separated\":true,\"inlineItems\":true,\"placeholderZh\":\"手动添加\",\"placeholderEn\":\"Add manually\","
             "\"removeLabelZh\":\"移除\",\"removeLabelEn\":\"Remove\",\"fetchOp\":\"fetch_models\","
             "\"fetchLabelZh\":\"从厂商获取\",\"fetchLabelEn\":\"Fetch from provider\"}");
  AppendText(out, outSize, off, ",");
  AddTextField(out, outSize, off, "api_key", "API Key", "API key", "API Key", "API key");
  AppendText(out, outSize, off, ",");
  if (advancedFields)
  {
    AddTextField(out, outSize, off, "url", "接口地址", "API URL", "https://...", "https://...");
    AppendText(out, outSize, off, ",");
    AddTextField(out, outSize, off, "models_url", "模型列表地址", "Models URL", "https://.../models", "https://.../models");
    AppendText(out, outSize, off, ",");
    {
      const char *values[] = {"none", "bearer", "header"};
      const char *zh[] = {"不认证", "Bearer", "自定义请求头"};
      const char *en[] = {"None", "Bearer", "Custom header"};
      AddSelectField(out, outSize, off, "auth", "认证方式", "Authentication", values, zh, en, 3);
    }
    AppendText(out, outSize, off, ",");
    AddTextField(out, outSize, off, "auth_header", "请求头名称", "Header name", "X-API-Key", "X-API-Key");
    AppendText(out, outSize, off, ",");
    {
      const char *values[] = {"GET", "POST"};
      const char *zh[] = {"GET", "POST"};
      const char *en[] = {"GET", "POST"};
      AddSelectField(out, outSize, off, "method", "请求方法", "Method", values, zh, en, 2);
    }
    AppendText(out, outSize, off, ",");
    AddTextField(out, outSize, off, "body", "请求体", "Request body", "{}", "{}");
    AppendText(out, outSize, off, ",");
    AddTextField(out, outSize, off, "usage_path", "用量路径", "Usage JSON path", "data.usage", "data.usage");
    AppendText(out, outSize, off, ",");
    AddTextField(out, outSize, off, "balance_path", "余额路径", "Balance JSON path", "data.balance", "data.balance");
    AppendText(out, outSize, off, ",");
    AddTextField(out, outSize, off, "unit", "单位", "Unit", "tokens", "tokens");
    AppendText(out, outSize, off, ",");
    AddTextField(out, outSize, off, "extra_headers", "附加请求头", "Extra headers", "X-Foo: bar", "X-Foo: bar");
    AppendText(out, outSize, off, ",");
  }
  AppendText(out, outSize, off, "{\"id\":\"enabled\",\"type\":\"bool\",\"labelZh\":\"启用\",\"labelEn\":\"Enabled\"}");
  AppendText(out, outSize, off, "],\"items\":[");
  for (int i = 0; i < s.n; ++i)
  {
    const Endpoint &e = s.e[i];
    AppendText(out, outSize, off, "%s{\"title\":", i ? "," : "");
    AppendJsonString(out, outSize, off, e.name);
    AppendText(out, outSize, off, ",\"values\":{\"enabled\":%d,\"taskbar_visible\":%d,\"name\":",
               e.enabled ? 1 : 0, e.taskbarVisible ? 1 : 0);
    AppendJsonString(out, outSize, off, e.name);
    AppendText(out, outSize, off, ",\"preset\":");
    AppendJsonString(out, outSize, off, e.preset);
    AppendText(out, outSize, off, ",\"models\":[");
    for (size_t mi = 0; mi < e.models.size(); ++mi)
    {
      if (mi)
        AppendText(out, outSize, off, ",");
      AppendJsonString(out, outSize, off, e.models[mi].c_str());
    }
    AppendText(out, outSize, off, "]");
    AppendText(out, outSize, off, ",\"schedule_enabled\":%d", e.scheduleEnabled ? 1 : 0);
    AppendText(out, outSize, off, ",\"peak_start\":\"%02d:%02d\",\"peak_end\":\"%02d:%02d\",\"valley_start\":\"%02d:%02d\",\"valley_end\":\"%02d:%02d\"",
               e.peakStart / 60, e.peakStart % 60, e.peakEnd / 60, e.peakEnd % 60,
               e.valleyStart / 60, e.valleyStart % 60, e.valleyEnd / 60, e.valleyEnd % 60);
    AppendText(out, outSize, off, ",\"url\":");
    AppendJsonString(out, outSize, off, e.url);
    AppendText(out, outSize, off, ",\"models_url\":");
    AppendJsonString(out, outSize, off, e.modelsUrl);
    AppendText(out, outSize, off, ",\"auth\":");
    AppendJsonString(out, outSize, off, e.auth);
    AppendText(out, outSize, off, ",\"auth_header\":");
    AppendJsonString(out, outSize, off, e.authHeader);
    AppendText(out, outSize, off, ",\"api_key\":");
    AppendJsonString(out, outSize, off, e.apiKey[0] ? "********" : "");
    AppendText(out, outSize, off, ",\"method\":");
    AppendJsonString(out, outSize, off, e.method);
    AppendText(out, outSize, off, ",\"body\":");
    AppendJsonString(out, outSize, off, e.body);
    AppendText(out, outSize, off, ",\"usage_path\":");
    AppendJsonString(out, outSize, off, e.usagePath);
    AppendText(out, outSize, off, ",\"balance_path\":");
    AppendJsonString(out, outSize, off, e.balancePath);
    AppendText(out, outSize, off, ",\"unit\":");
    AppendJsonString(out, outSize, off, e.unit);
    AppendText(out, outSize, off, ",\"currency\":");
    AppendJsonString(out, outSize, off, e.currency);
    AppendText(out, outSize, off, ",\"extra_headers\":");
    AppendJsonString(out, outSize, off, e.extraHeaders);
    AppendText(out, outSize, off, "}}");
  }
  AppendText(out, outSize, off, "]");
  if (modelFetchPending)
    AppendText(out, outSize, off, ",\"waiting\":\"models\"");
  AppendText(out, outSize, off, "}");
  AppendText(out, outSize, off, "%s{\"id\":\"processes\",\"type\":\"group\",\"labelZh\":\"显示条件\",\"labelEn\":\"Show when\",\"layout\":\"rows\",\"fieldsWrap\":true,\"hideCount\":true,\"rowToggle\":[\"enabled\"],\"addHintZh\":\"例如 code.exe\",\"addHintEn\":\"e.g. code.exe\",\"fields\":[",
             firstParam ? "" : ",");
  AppendText(out, outSize, off, "{\"id\":\"enabled\",\"type\":\"bool\",\"labelZh\":\"启用\",\"labelEn\":\"Enabled\"},");
  AddTextField(out, outSize, off, "name", "进程名", "Process", "game.exe 或 game*", "game.exe or game*");
  AppendText(out, outSize, off, "],\"items\":[");
  for (int i = 0; i < s.processN; ++i)
  {
    const ProcessRule &p = s.process[i];
    AppendText(out, outSize, off, "%s{\"title\":", i ? "," : "");
    AppendJsonString(out, outSize, off, p.name);
    AppendText(out, outSize, off, ",\"values\":{\"enabled\":%d,\"name\":", p.enabled ? 1 : 0);
    AppendJsonString(out, outSize, off, p.name);
    AppendText(out, outSize, off, "}}");
  }
  AppendText(out, outSize, off, "]}");
  AppendText(out, outSize, off, "],\"settingsFile\":\"ComputeManager.ini\",\"summaryZh\":\"指定进程运行时显示多厂商模型的半透明算力挂件\",\"summaryEn\":\"A translucent compute widget for vendor models, shown while a watched process runs\"}");
  return off < outSize ? off : 0;
}

bool Wide(const char *src, std::wstring *out)
{
  if (!src || !out)
    return false;
  const int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);
  if (n <= 0)
    return false;
  out->assign((size_t)n, L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, src, -1, &(*out)[0], n) <= 0)
    return false;
  out->resize(n - 1);
  return true;
}

bool HttpCall(const Endpoint &e, std::string *response, int *status, char *error, int errorSize)
{
  if (error && errorSize)
    error[0] = 0;
  if (!g_session)
  {
    _snprintf(error, errorSize, "WinHTTP unavailable");
    return false;
  }
  std::string expandedUrl;
  ExpandModel(e.url, e.model, &expandedUrl);
  std::wstring wideUrl;
  if (!Wide(expandedUrl.c_str(), &wideUrl))
  {
    _snprintf(error, errorSize, "URL is not UTF-8");
    return false;
  }
  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof(uc);
  uc.dwSchemeLength = (DWORD)-1;
  uc.dwHostNameLength = (DWORD)-1;
  uc.dwUrlPathLength = (DWORD)-1;
  uc.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &uc) || !uc.lpszHostName || !uc.dwHostNameLength ||
      (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS))
  {
    _snprintf(error, errorSize, "invalid URL (HTTP or HTTPS required)");
    return false;
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring method;
  if (!Wide(e.method, &method))
  {
    _snprintf(error, errorSize, "invalid HTTP method");
    return false;
  }
  std::wstring object;
  if (uc.lpszUrlPath && uc.dwUrlPathLength)
    object.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.lpszExtraInfo && uc.dwExtraInfoLength)
    object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  if (object.empty())
    object = L"/";
  const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET connect = WinHttpConnect(g_session, host.c_str(), uc.nPort, 0);
  if (!connect)
  {
    _snprintf(error, errorSize, "connect failed (%lu)", GetLastError());
    return false;
  }
  HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), object.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request)
  {
    const DWORD err = GetLastError();
    WinHttpCloseHandle(connect);
    _snprintf(error, errorSize, "request failed (%lu)", err);
    return false;
  }
  WinHttpSetTimeouts(request, 5000, 5000, 5000, 5000);

  std::wstring headers;
  auto addHeader = [&headers](const char *name, const char *value) -> bool {
    if (!name || !value || !*name || std::strpbrk(name, "\r\n") || std::strpbrk(value, "\r\n"))
      return false;
    std::wstring wname;
    std::wstring wvalue;
    if (!Wide(name, &wname) || !Wide(value, &wvalue))
      return false;
    headers += wname;
    headers += L": ";
    headers += wvalue;
    headers += L"\r\n";
    return true;
  };
  bool headerOk = true;
  if (!std::strcmp(e.auth, "bearer"))
  {
    char value[kMaxKey + 32] = {0};
    _snprintf(value, sizeof(value), "Bearer %s", e.apiKey);
    headerOk = headerOk && addHeader("Authorization", value);
  }
  else if (!std::strcmp(e.auth, "header"))
    headerOk = headerOk && addHeader(e.authHeader, e.apiKey);

  std::string expandedHeaders;
  ExpandModel(e.extraHeaders, e.model, &expandedHeaders);
  const char *extra = expandedHeaders.c_str();
  while (extra && *extra)
  {
    const char *semi = std::strchr(extra, ';');
    const size_t len = semi ? (size_t)(semi - extra) : std::strlen(extra);
    std::string item(extra, len);
    const size_t colon = item.find(':');
    if (colon != std::string::npos)
    {
      std::string name = item.substr(0, colon);
      std::string value = item.substr(colon + 1);
      while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.pop_back();
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.erase(value.begin());
      headerOk = headerOk && addHeader(name.c_str(), value.c_str());
    }
    extra = semi ? semi + 1 : extra + len;
  }
  if (!std::strcmp(e.method, "POST"))
    headerOk = headerOk && addHeader("Content-Type", "application/json; charset=utf-8");
  if (!headerOk)
  {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    _snprintf(error, errorSize, "invalid request header");
    return false;
  }

  std::string expandedBody;
  ExpandModel(e.body, e.model, &expandedBody);
  const DWORD bodySize = (DWORD)expandedBody.size();
  const BOOL sent = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                               : headers.c_str(),
                                       headers.empty() ? 0 : (DWORD)-1,
                                       bodySize ? (LPVOID)expandedBody.data() : WINHTTP_NO_REQUEST_DATA,
                                       bodySize, bodySize, 0);
  if (!sent || !WinHttpReceiveResponse(request, nullptr))
  {
    const DWORD err = GetLastError();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    _snprintf(error, errorSize, "HTTP request failed (%lu)", err);
    return false;
  }
  DWORD code = 0;
  DWORD codeLen = sizeof(code);
  DWORD index = 0;
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeLen, &index))
  {
    const DWORD err = GetLastError();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    _snprintf(error, errorSize, "status query failed (%lu)", err);
    return false;
  }
  if (status)
    *status = (int)code;

  std::string body;
  char buffer[8192];
  bool readOk = true;
  char readFailure[96] = {0};
  for (;;)
  {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available))
    {
      _snprintf(readFailure, sizeof(readFailure), "response read failed (%lu)", GetLastError());
      readOk = false;
      break;
    }
    if (!available)
      break;
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer, available < sizeof(buffer) ? available : (DWORD)sizeof(buffer), &read))
    {
      _snprintf(readFailure, sizeof(readFailure), "response read failed (%lu)", GetLastError());
      readOk = false;
      break;
    }
    if (!read)
      break;
    body.append(buffer, read);
    if (body.size() > 4 * 1024 * 1024)
    {
      _snprintf(readFailure, sizeof(readFailure), "response too large");
      readOk = false;
      break;
    }
  }
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  if (code < 200 || code >= 300)
  {
    _snprintf(error, errorSize, "HTTP %lu", code);
    return false;
  }
  if (!readOk)
  {
    _snprintf(error, errorSize, "%s", readFailure[0] ? readFailure : "response read failed");
    return false;
  }
  if (response)
    *response = body;
  return true;
}

int EndpointPeakWindows(const Endpoint &e, int *starts, int *ends)
{
  int count = 0;
  if (e.peakStart >= 0 && e.peakEnd >= 0)
  {
    starts[count] = e.peakStart;
    ends[count++] = e.peakEnd;
  }
  if (e.peak2Start >= 0 && e.peak2End >= 0)
  {
    starts[count] = e.peak2Start;
    ends[count++] = e.peak2End;
  }
  return count;
}

Period EndpointPeriodAt(const Endpoint &e, const SYSTEMTIME &time)
{
  if (!e.scheduleEnabled)
    return Period::Normal;
  int starts[2] = {}, ends[2] = {};
  const int count = EndpointPeakWindows(e, starts, ends);
  return count ? PeriodAtWindows(time.wHour * 60 + time.wMinute, time.wDayOfWeek, starts, ends, count,
                                 e.scheduleWeekdayMask)
               : Period::Valley;
}

int EndpointMinutesUntilChange(const Endpoint &e, const SYSTEMTIME &time)
{
  if (!e.scheduleEnabled)
    return -1;
  int starts[2] = {}, ends[2] = {};
  const int count = EndpointPeakWindows(e, starts, ends);
  if (!count)
    return -1;
  return MinutesUntilWindowsChange(time.wHour * 60 + time.wMinute, time.wDayOfWeek, starts, ends, count,
                                   e.scheduleWeekdayMask);
}

SYSTEMTIME AddMinutes(const SYSTEMTIME &time, int minutes)
{
  FILETIME ft;
  SystemTimeToFileTime(&time, &ft);
  ULARGE_INTEGER value;
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  value.QuadPart += (ULONGLONG)minutes * 60ULL * 10000000ULL;
  ft.dwLowDateTime = value.LowPart;
  ft.dwHighDateTime = value.HighPart;
  SYSTEMTIME result;
  FileTimeToSystemTime(&ft, &result);
  return result;
}

void FetchPendingSchedules()
{
  Settings snapshot;
  EnterCriticalSection(&g_lock);
  snapshot = g_set;
  LeaveCriticalSection(&g_lock);
  const unsigned long long now = GetTickCount64();
  for (int i = 0; i < snapshot.n; ++i)
  {
    const Endpoint &before = snapshot.e[i];
    if (!before.enabled || !before.scheduleFetchDue || before.scheduleFetchDue > now ||
        !before.scheduleUrl[0])
      continue;

    Endpoint probe = before;
    CopyText(probe.url, sizeof(probe.url), before.scheduleUrl);
    CopyText(probe.method, sizeof(probe.method), "GET");
    CopyText(probe.auth, sizeof(probe.auth), "none");
    probe.body[0] = 0;
    probe.usagePath[0] = 0;
    probe.balancePath[0] = 0;
    probe.extraHeaders[0] = 0;
    std::string body;
    int status = 0;
    char error[kMaxError] = {0};
    const bool ok = HttpCall(probe, &body, &status, error, (int)sizeof(error));
    int startsUtc[4] = {}, endsUtc[4] = {}, count = 0;
    unsigned weekdayMask = 0x7f;
    const bool parsed =
        ok && LooksLikeDeepSeek(before) &&
        ParseDeepSeekPeakWindows(body.c_str(), startsUtc, endsUtc, &count, &weekdayMask);

    int startsLocal[4] = {}, endsLocal[4] = {};
    if (parsed)
    {
      TIME_ZONE_INFORMATION tz = {};
      const DWORD zone = GetTimeZoneInformation(&tz);
      int bias = tz.Bias;
      bias += zone == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias;
      for (int r = 0; r < count; ++r)
      {
        startsLocal[r] = (startsUtc[r] - bias + 2 * 24 * 60) % (24 * 60);
        endsLocal[r] = (endsUtc[r] - bias + 2 * 24 * 60) % (24 * 60);
      }
    }

    EnterCriticalSection(&g_lock);
    if (i < g_set.n && g_set.e[i].revision == before.revision)
    {
      Endpoint &current = g_set.e[i];
      if (parsed)
      {
        current.scheduleEnabled = true;
        current.scheduleError[0] = 0;
        current.scheduleFetchDue = GetTickCount64() + 24ULL * 60ULL * 60ULL * 1000ULL;
        current.peakStart = startsLocal[0];
        current.peakEnd = endsLocal[0];
        current.peak2Start = count > 1 ? startsLocal[1] : -1;
        current.peak2End = count > 1 ? endsLocal[1] : -1;
        current.scheduleWeekdayMask = weekdayMask;
      }
      else
      {
        CopyText(current.scheduleError, sizeof(current.scheduleError),
                 ok ? "vendor pricing format not recognized" : error);
        // Keep a previously verified schedule through a transient refresh failure. If there is no
        // verified schedule yet, retry soon instead of treating "not fetched" as "no peak hours".
        current.scheduleFetchDue = GetTickCount64() + 15ULL * 60ULL * 1000ULL;
      }
    }
    LeaveCriticalSection(&g_lock);
    SaveSettings();
    LogLine("feature %s: vendor schedule for %s: %s", kId, before.name, parsed ? "ok" : "failed");
    return;
  }
}

void FetchPendingModels()
{
  Settings snapshot;
  EnterCriticalSection(&g_lock);
  snapshot = g_set;
  LeaveCriticalSection(&g_lock);
  const unsigned long long now = GetTickCount64();
  for (int i = 0; i < snapshot.n; ++i)
  {
    const Endpoint &before = snapshot.e[i];
    if (!before.enabled || !before.modelFetchDue || before.modelFetchDue > now || !before.modelsUrl[0])
      continue;

    Endpoint probe = before;
    CopyText(probe.url, sizeof(probe.url), before.modelsUrl);
    CopyText(probe.method, sizeof(probe.method), "GET");
    probe.body[0] = 0;
    probe.usagePath[0] = 0;
    probe.balancePath[0] = 0;
    std::string body;
    int status = 0;
    char error[kMaxError] = {0};
    const bool ok = HttpCall(probe, &body, &status, error, (int)sizeof(error));
    std::vector<std::string> models;
    const bool parsed = ok && apex::compute_manager::ExtractModelIds(body.c_str(), &models);
    int peakStart = 0, peakEnd = 0, valleyStart = 0, valleyEnd = 0;
    const bool vendorSchedule =
        ok && ExtractVendorSchedule(body.c_str(), &peakStart, &peakEnd, &valleyStart, &valleyEnd);

    EnterCriticalSection(&g_lock);
    if (i < g_set.n && g_set.e[i].revision == before.revision)
    {
      Endpoint &current = g_set.e[i];
      current.modelFetchDue = 0;
      if (parsed)
      {
        for (const std::string &model : models)
        {
          bool exists = false;
          for (const std::string &old : current.models)
            exists = exists || old == model;
          if (!exists && current.models.size() < 64)
            current.models.push_back(model);
        }
        current.modelsError[0] = 0;
        if (!current.model[0] && current.models.size() == 1)
          CopyText(current.model, sizeof(current.model), current.models[0].c_str());
      }
      else
        CopyText(current.modelsError, sizeof(current.modelsError),
                 ok ? "model list format not recognized" : error);
      if (vendorSchedule)
      {
        current.scheduleEnabled = true;
        current.scheduleError[0] = 0;
        current.peakStart = peakStart;
        current.peakEnd = peakEnd;
        current.peak2Start = -1;
        current.peak2End = -1;
        current.valleyStart = valleyStart;
        current.valleyEnd = valleyEnd;
        current.scheduleWeekdayMask = 0x7fu;
      }
    }
    LeaveCriticalSection(&g_lock);
    LogLine("feature %s: model discovery for %s: %s", kId, before.name,
            parsed ? "ok" : "failed");
    if (parsed)
      return; // one list per worker turn keeps a slow provider from delaying the others forever
  }
}

void ApplyPollResult(int index, const Endpoint &before, bool ok, int status, const std::string &body,
                     const char *error)
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  if (index < 0 || index >= s.n || s.e[index].revision != before.revision)
  {
    LeaveCriticalSection(&g_lock);
    return;
  }
  Endpoint &e = s.e[index];
  e.lastOk = ok;
  e.lastStatus = status;
  e.lastPollTick = GetTickCount64();
  CopyText(e.lastError, sizeof(e.lastError), ok ? "" : (error ? error : "request failed"));
  if (ok)
  {
    double value = 0;
    if (before.usagePath[0])
    {
      if (!ExtractJsonNumber(body.c_str(), before.usagePath, &value))
      {
        e.lastOk = false;
        CopyText(e.lastError, sizeof(e.lastError), "usage JSON path not found");
      }
      else
      {
        bool reset = false;
        const double delta = e.hasRawUsage ? UsageDelta(e.rawUsage, value, &reset) : 0.0;
        SYSTEMTIME local;
        GetLocalTime(&local);
        const Period period = EndpointPeriodAt(e, local);
        const int bucket = period == Period::Peak ? 1 : (period == Period::Valley ? 2 : 0);
        if (e.hasRawUsage)
          e.period[bucket] += delta;
        e.rawUsage = value;
        e.usage = value;
        e.hasRawUsage = true;
        e.hasUsage = true;
      }
    }
    if (before.balancePath[0])
    {
      if (!ExtractJsonNumber(body.c_str(), before.balancePath, &value))
      {
        e.lastOk = false;
        CopyText(e.lastError, sizeof(e.lastError), "balance JSON path not found");
      }
      else
      {
        e.balance = value;
        e.hasBalance = true;
      }
    }
  }
  const bool finalOk = e.lastOk;
  g_set = s;
  LeaveCriticalSection(&g_lock);
  WriteLog("%s: %s status=%d", before.name, finalOk ? "ok" : "error", status);
}

void PollAll()
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);
  bool changed = false;
  for (int i = 0; i < s.n; ++i)
  {
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
      break;
    const Endpoint e = s.e[i];
    if (!e.enabled || !e.url[0])
      continue;
    std::string body;
    int status = 0;
    char error[kMaxError] = {0};
    const bool ok = HttpCall(e, &body, &status, error, (int)sizeof(error));
    ApplyPollResult(i, e, ok, status, body, error);
    changed = true;
  }
  if (changed)
    SaveSettings();
}

bool AnyWatchedProcessRunning()
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);
  if (s.processN <= 0)
    return false;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
  {
    LogLine("feature %s: process snapshot failed (%lu)", kId, GetLastError());
    return false;
  }
  PROCESSENTRY32W pe;
  memset(&pe, 0, sizeof(pe));
  pe.dwSize = sizeof(pe);
  bool matched = false;
  if (Process32FirstW(snap, &pe))
  {
    do
    {
      char name[260] = {0};
      if (WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, (int)sizeof(name) - 1, nullptr, nullptr) <= 0)
        continue;
      char normal[260] = {0};
      apex::match::NormaliseName(name, normal, (int)sizeof(normal));
      for (int i = 0; i < s.processN; ++i)
      {
        if (!s.process[i].enabled || !s.process[i].name[0])
          continue;
        if (apex::match::Pattern(s.process[i].name, normal))
        {
          matched = true;
          break;
        }
      }
      if (matched)
        break;
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return matched;
}

constexpr int kOverlayMinWidth = 180;
constexpr int kOverlayMaxWidth = 320;
constexpr int kOverlayPad = 12;
constexpr int kOverlayMaxRows = 4;
constexpr int kTaskbarMaxColumns = 4;

struct TaskbarGeometry
{
  HWND window = nullptr;
  RECT taskbar = {};
  RECT monitor = {};
  apex::taskbar::Display display = {};
};

apex::taskbar::Rect LayoutRect(const RECT &r)
{
  return {r.left, r.top, r.right, r.bottom};
}

bool CurrentTaskbarGeometry(int fontPixels, TaskbarGeometry *out)
{
  if (!out)
    return false;
  HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (!taskbar)
    return false;
  RECT taskbarRect = {};
  if (!GetWindowRect(taskbar, &taskbarRect))
    return false;
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor))
    return false;
  out->window = taskbar;
  out->taskbar = taskbarRect;
  out->monitor = monitor.rcMonitor;
  out->display = apex::taskbar::ChooseDisplay(
      LayoutRect(taskbarRect), LayoutRect(monitor.rcMonitor), fontPixels);
  return true;
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

HFONT g_uiFont = nullptr;
HFONT g_uiSmallFont = nullptr;
HFONT g_uiTaskbarFont = nullptr;
LOGFONTW g_uiTaskbarLogFont = {};
int g_uiTaskbarFontSize = 0;
bool g_overlayVisible = false;

bool OverlayChinese()
{
  return (GetUserDefaultUILanguage() & 0x3ff) == 0x04;
}

int TaskbarInlineGap()
{
  return std::max(9, -(int)g_uiTaskbarLogFont.lfHeight);
}

int ClampTaskbarFontSize(int size)
{
  return std::max(9, std::min(16, size));
}

int SharedTaskbarFontSize()
{
  const int size = (g_host && g_host->taskbarFontSize) ? g_host->taskbarFontSize() : 12;
  return ClampTaskbarFontSize(size);
}

const char *SharedTaskbarTextColor()
{
  const char *color = (g_host && g_host->taskbarTextColor) ? g_host->taskbarTextColor() : nullptr;
  return color && *color ? color : "auto";
}

int EffectiveTaskbarFontSize()
{
  int size = SharedTaskbarFontSize();
  TaskbarGeometry geometry;
  if (!CurrentTaskbarGeometry(size, &geometry))
    return size;
  const int thickness = apex::taskbar::Thickness(LayoutRect(geometry.taskbar), geometry.display.edge);
  const int cap = apex::taskbar::FontCap(thickness, geometry.display.lineCount);
  if (cap >= 9)
    size = std::min(size, cap);
  return size;
}

void EnsureTaskbarFont()
{
  const int size = EffectiveTaskbarFontSize();
  if (g_uiTaskbarFont && g_uiTaskbarFontSize == size)
    return;
  if (g_uiTaskbarFont)
    DeleteObject(g_uiTaskbarFont);
  LOGFONTW font = {};
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    font = metrics.lfMessageFont;
  if (!font.lfFaceName[0])
    std::wcsncpy(font.lfFaceName, L"Segoe UI", (sizeof(font.lfFaceName) / sizeof(font.lfFaceName[0])) - 1);
  font.lfHeight = -size;
  font.lfWeight = FW_NORMAL;
  font.lfItalic = FALSE;
  font.lfUnderline = FALSE;
  font.lfStrikeOut = FALSE;
  font.lfQuality = ANTIALIASED_QUALITY;
  font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
  g_uiTaskbarLogFont = font;
  g_uiTaskbarFont = CreateFontIndirectW(&font);
  g_uiTaskbarFontSize = g_uiTaskbarFont ? size : 0;
}

int TaskbarTextWidth(const char *text)
{
  if (!text || !*text)
    return 0;
  wchar_t wide[1024] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide,
                          (int)(sizeof(wide) / sizeof(wide[0]))) <= 0)
    return 0;
  if (!g_gdiplusReady)
  {
    HDC dc = GetDC(nullptr);
    const int width = dc ? OverlayTextWidth(dc, g_uiTaskbarFont, text) : 0;
    if (dc)
      ReleaseDC(nullptr, dc);
    return width;
  }

  Gdiplus::Bitmap scratch(1, 1, PixelFormat32bppPARGB);
  Gdiplus::Graphics graphics(&scratch);
  Gdiplus::FontFamily primaryFamily(g_uiTaskbarLogFont.lfFaceName);
  Gdiplus::FontFamily chineseFamily(L"Microsoft YaHei UI");
  Gdiplus::FontFamily latinFamily(L"Segoe UI");
  Gdiplus::FontFamily *family = &primaryFamily;
  if (primaryFamily.GetLastStatus() != Gdiplus::Ok || !primaryFamily.IsAvailable())
    family = &chineseFamily;
  if (family->GetLastStatus() != Gdiplus::Ok || !family->IsAvailable())
    family = &latinFamily;
  const int fontPixels = std::max(9, -(int)g_uiTaskbarLogFont.lfHeight);
  Gdiplus::Font font(family, (Gdiplus::REAL)fontPixels, Gdiplus::FontStyleRegular,
                     Gdiplus::UnitPixel);
  Gdiplus::StringFormat format;
  format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
  Gdiplus::RectF layout(0, 0, 4096, 4096);
  Gdiplus::RectF bounds;
  if (graphics.MeasureString(wide, -1, &font, layout, &format, &bounds) != Gdiplus::Ok)
    return 0;
  return (int)(bounds.Width + 1.999f);
}

int OverlayTextWidth(HDC dc, HFONT font, const char *text)
{
  if (!dc || !text || !*text)
    return 0;
  wchar_t wide[512] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))) <= 0)
    return 0;
  HGDIOBJ old = SelectObject(dc, font ? font : g_uiFont);
  SIZE size = {};
  GetTextExtentPoint32W(dc, wide, (int)wcslen(wide), &size);
  SelectObject(dc, old);
  return size.cx;
}

int OverlayWidth(const Settings &s)
{
  HDC dc = g_overlay ? GetDC(g_overlay) : nullptr;
  if (!dc)
    return kOverlayMinWidth;
  SYSTEMTIME local;
  GetLocalTime(&local);
  const bool chinese = OverlayChinese();
  int content = 0;
  int shown = 0;
  for (int i = 0; i < s.n && shown < kOverlayMaxRows; ++i)
  {
    const Endpoint &e = s.e[i];
    if (!e.enabled || !e.taskbarVisible)
      continue;
    ++shown;
    char balance[96] = {0};
    char line[256] = {0};
    FormatBalance(balance, sizeof(balance), e);
    _snprintf(line, sizeof(line), "%s    %s", e.model[0] ? e.model : e.name, balance);
    content = std::max(content, OverlayTextWidth(dc, g_uiFont, line));
    if (e.hasUsage)
    {
      char usage[64] = {0};
      char usageLine[128] = {0};
      FormatNumber(usage, sizeof(usage), e.usage);
      _snprintf(usageLine, sizeof(usageLine), "%s %s %s", chinese ? "用量" : "usage", usage, e.unit);
      content = std::max(content, OverlayTextWidth(dc, g_uiSmallFont, usageLine));
    }
    if (e.scheduleEnabled)
    {
      const int next = EndpointMinutesUntilChange(e, local);
      if (next >= 0)
      {
        const Period nextPeriod = EndpointPeriodAt(e, AddMinutes(local, next));
        char countdown[128] = {0};
        FormatCountdown(countdown, sizeof(countdown), next, nextPeriod, chinese);
        content = std::max(content, OverlayTextWidth(dc, g_uiFont, countdown));
      }
      content = std::max(content, 190);
    }
  }
  ReleaseDC(g_overlay, dc);
  const int width = content + kOverlayPad * 2 + 15;
  return std::max(kOverlayMinWidth, std::min(kOverlayMaxWidth, width));
}

int OverlayHeight(const Settings &s)
{
  int height = kOverlayPad * 2;
  int shown = 0;
  int hidden = 0;
  for (int i = 0; i < s.n; ++i)
  {
    if (!s.e[i].enabled || !s.e[i].taskbarVisible)
      continue;
    if (shown >= kOverlayMaxRows)
    {
      ++hidden;
      continue;
    }
    ++shown;
    height += 22;
    if (s.e[i].hasUsage)
      height += 16;
    if (s.e[i].scheduleEnabled)
      height += 44;
    height += 5;
  }
  if (hidden)
    height += 17;
  return shown ? height : kOverlayPad * 2 + 28;
}

void FormatTaskbarPeriod(char *out, int outSize, int minutes)
{
  const int hours = minutes / 60;
  const int mins = minutes % 60;
  _snprintf(out, outSize, "-%d:%02d", hours, mins);
}

struct TaskbarParts
{
  char title[320] = {0};
  char balance[96] = {0};
  char remaining[64] = {0};
  Period current = Period::Normal;
  Period next = Period::Normal;
  int minutesUntilChange = 0;
  bool hasRemaining = false;
};

TaskbarParts BuildTaskbarParts(const Endpoint &e, const SYSTEMTIME &now)
{
  TaskbarParts parts;
  _snprintf(parts.title, sizeof(parts.title), "%s", e.name[0] ? e.name : (e.model[0] ? e.model : "API"));
  FormatBalance(parts.balance, sizeof(parts.balance), e);
  if (!e.scheduleEnabled)
    return parts;
  parts.current = EndpointPeriodAt(e, now);
  const int next = EndpointMinutesUntilChange(e, now);
  if (next < 0)
    return parts;
  parts.minutesUntilChange = next;
  parts.next = EndpointPeriodAt(e, AddMinutes(now, next));
  FormatTaskbarPeriod(parts.remaining, sizeof(parts.remaining), next);
  parts.hasRemaining = true;
  return parts;
}

int TaskbarPartsWidth(const TaskbarParts &parts, const apex::taskbar::Display &display)
{
  if (display.singleLine)
  {
    int width = TaskbarTextWidth(parts.title) + TaskbarInlineGap() + TaskbarTextWidth(parts.balance);
    if (parts.hasRemaining)
      width += 8 + 8 + 8 + TaskbarTextWidth(parts.remaining);
    return width;
  }
  int width = TaskbarTextWidth(parts.balance);
  if (parts.hasRemaining)
    width += 8 + 8 + 8 + TaskbarTextWidth(parts.remaining);
  return std::max(TaskbarTextWidth(parts.title), width);
}

COLORREF TaskbarDotColor(Period period)
{
  if (period == Period::Peak)
    return RGB(224, 96, 104);
  if (period == Period::Valley)
    return RGB(78, 151, 219);
  return RGB(150, 154, 160);
}

int TaskbarOverlayExtent(const Settings &s, const apex::taskbar::Display &display)
{
  SYSTEMTIME local;
  GetLocalTime(&local);
  int columns[kTaskbarMaxColumns] = {};
  int shown = 0;
  for (int i = 0; i < s.n && shown < kTaskbarMaxColumns; ++i)
  {
    const Endpoint &e = s.e[i];
    if (!e.enabled || !e.taskbarVisible)
      continue;
    const TaskbarParts parts = BuildTaskbarParts(e, local);
    const int column = shown;
    columns[column] = std::max(columns[column], TaskbarPartsWidth(parts, display) + 4);
    ++shown;
  }
  int content = 0;
  for (int i = 0; i < kTaskbarMaxColumns && columns[i]; ++i)
    content += columns[i] + (i ? 12 : 0);
  return std::max(96, std::min(620, content + kOverlayPad * 2 + 6));
}

bool PaintTaskbarOverlay(const Settings &s, DWORD *pixels, int width, int height,
                         const apex::taskbar::Display &display)
{
  if (!g_gdiplusReady || !pixels || width <= 0 || height <= 0)
    return false;
  Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                         reinterpret_cast<BYTE *>(pixels));
  if (bitmap.GetLastStatus() != Gdiplus::Ok)
    return false;
  Gdiplus::Graphics graphics(&bitmap);
  if (graphics.GetLastStatus() != Gdiplus::Ok)
    return false;
  graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
  graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

  SYSTEMTIME local;
  GetLocalTime(&local);
  const bool chinese = OverlayChinese();
  Endpoint visible[kTaskbarMaxColumns];
  int columnWidths[kTaskbarMaxColumns] = {};
  int shown = 0;
  for (int i = 0; i < s.n && shown < kTaskbarMaxColumns; ++i)
  {
    if (!s.e[i].enabled || !s.e[i].taskbarVisible)
      continue;
    visible[shown] = s.e[i];
    const TaskbarParts parts = BuildTaskbarParts(visible[shown], local);
    columnWidths[shown] = std::max(columnWidths[shown], TaskbarPartsWidth(parts, display) + 4);
    ++shown;
  }

  int drawWidth = width;
  int drawHeight = height;
  if (display.vertical)
  {
    // Rotate the logical horizontal line clockwise into the side taskbar. The window itself stays a plain
    // layered rectangle; only the GDI+ coordinate system turns.
    graphics.TranslateTransform((Gdiplus::REAL)width, 0.0f);
    graphics.RotateTransform(90.0f);
    drawWidth = height;
    drawHeight = width;
  }
  const int textPixels = std::max(9, -(int)g_uiTaskbarLogFont.lfHeight);
  const apex::taskbar::TwoLineRows rows =
      apex::taskbar::CompactTwoLineRows(drawHeight, textPixels);
  const int rowHeight = display.lineCount == 1 ? drawHeight : rows.lineHeight;
  int columnLeft[kTaskbarMaxColumns] = {};
  int x = kOverlayPad;
  for (int c = 0; c < kTaskbarMaxColumns && columnWidths[c]; ++c)
  {
    columnLeft[c] = x;
    x += columnWidths[c] + 12;
  }

  Gdiplus::FontFamily primaryFamily(g_uiTaskbarLogFont.lfFaceName);
  Gdiplus::FontFamily chineseFamily(L"Microsoft YaHei UI");
  Gdiplus::FontFamily latinFamily(L"Segoe UI");
  Gdiplus::FontFamily *family = &primaryFamily;
  if (primaryFamily.GetLastStatus() != Gdiplus::Ok || !primaryFamily.IsAvailable())
    family = &chineseFamily;
  if (family->GetLastStatus() != Gdiplus::Ok || !family->IsAvailable())
    family = &latinFamily;
  const int fontPixels = textPixels;
  Gdiplus::Font font(family, (Gdiplus::REAL)fontPixels, Gdiplus::FontStyleRegular,
                     Gdiplus::UnitPixel);
  const COLORREF color = TaskbarTextColor();
  Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
  Gdiplus::StringFormat format;
  format.SetAlignment(Gdiplus::StringAlignmentNear);
  format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
  format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
  format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

  for (int i = 0; i < shown; ++i)
  {
    const int column = i;
    const TaskbarParts parts = BuildTaskbarParts(visible[i], local);
    const int titleWidth = TaskbarTextWidth(parts.title);
    const int balanceWidth = TaskbarTextWidth(parts.balance);
    wchar_t titleW[512] = {0};
    MultiByteToWideChar(CP_UTF8, 0, parts.title, -1, titleW,
                        (int)(sizeof(titleW) / sizeof(titleW[0])));
    const int titleY = display.singleLine ? 0 : rows.titleTop;
    Gdiplus::RectF titleRect((Gdiplus::REAL)columnLeft[column], (Gdiplus::REAL)titleY,
                             (Gdiplus::REAL)(columnWidths[column] + 2), (Gdiplus::REAL)rowHeight);
    if (display.singleLine)
    {
      Gdiplus::RectF inlineTitle((Gdiplus::REAL)columnLeft[column], 0,
                                 (Gdiplus::REAL)(titleWidth + 2), (Gdiplus::REAL)rowHeight);
      graphics.DrawString(titleW, -1, &font, inlineTitle, &format, &brush);
    }
    else
      graphics.DrawString(titleW, -1, &font, titleRect, &format, &brush);

    wchar_t balanceW[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, parts.balance, -1, balanceW,
                        (int)(sizeof(balanceW) / sizeof(balanceW[0])));
    const int balanceX =
        display.singleLine ? columnLeft[column] + titleWidth + TaskbarInlineGap() : columnLeft[column];
    const int balanceY = display.singleLine ? 0 : rows.balanceTop;
    Gdiplus::RectF balanceRect((Gdiplus::REAL)balanceX, (Gdiplus::REAL)balanceY,
                               (Gdiplus::REAL)(balanceWidth + 2), (Gdiplus::REAL)rowHeight);
    graphics.DrawString(balanceW, -1, &font, balanceRect, &format, &brush);
    if (parts.hasRemaining)
    {
      const int dotX = balanceX + balanceWidth + 8;
      const int centerY = balanceY + rowHeight / 2;
      const COLORREF dotColor = TaskbarDotColor(parts.current);
      const bool blinkOff = parts.minutesUntilChange <= 5 && ((GetTickCount64() / 1000ULL) & 1ULL) != 0;
      if (!blinkOff)
      {
        Gdiplus::SolidBrush dotBrush(
            Gdiplus::Color(245, GetRValue(dotColor), GetGValue(dotColor), GetBValue(dotColor)));
        graphics.FillEllipse(&dotBrush, (Gdiplus::REAL)dotX, (Gdiplus::REAL)(centerY - 4.0f), 8.0f, 8.0f);
      }

      wchar_t remainingW[128] = {0};
      MultiByteToWideChar(CP_UTF8, 0, parts.remaining, -1, remainingW,
                          (int)(sizeof(remainingW) / sizeof(remainingW[0])));
      const int remainingX = dotX + 8 + 8;
      Gdiplus::RectF remainingRect((Gdiplus::REAL)remainingX, (Gdiplus::REAL)balanceY,
                                   (Gdiplus::REAL)(TaskbarTextWidth(parts.remaining) + 2),
                                   (Gdiplus::REAL)rowHeight);
      graphics.DrawString(remainingW, -1, &font, remainingRect, &format, &brush);
    }
  }
  if (!shown)
  {
    wchar_t empty[64] = {0};
    MultiByteToWideChar(CP_UTF8, 0, chinese ? "尚无模型" : "No models", -1, empty,
                        (int)(sizeof(empty) / sizeof(empty[0])));
    Gdiplus::RectF r((Gdiplus::REAL)kOverlayPad, 0, (Gdiplus::REAL)(drawWidth - kOverlayPad),
                     (Gdiplus::REAL)drawHeight);
    graphics.DrawString(empty, -1, &font, r, &format, &brush);
  }
  graphics.Flush(Gdiplus::FlushIntentionSync);
  return true;
}

void ApplyTaskbarTextRegion(HWND hwnd, const Settings &s, const apex::taskbar::Display &display)
{
  RECT client;
  GetClientRect(hwnd, &client);
  Endpoint visible[kTaskbarMaxColumns];
  int columnWidths[kTaskbarMaxColumns] = {};
  int shown = 0;
  SYSTEMTIME local;
  GetLocalTime(&local);
  for (int i = 0; i < s.n && shown < kTaskbarMaxColumns; ++i)
  {
    if (!s.e[i].enabled || !s.e[i].taskbarVisible)
      continue;
    visible[shown] = s.e[i];
    const TaskbarParts parts = BuildTaskbarParts(visible[shown], local);
    columnWidths[shown] = std::max(columnWidths[shown], TaskbarPartsWidth(parts, display) + 4);
    ++shown;
  }

  int columnLeft[kTaskbarMaxColumns] = {};
  int x = kOverlayPad;
  for (int c = 0; c < kTaskbarMaxColumns && columnWidths[c]; ++c)
  {
    columnLeft[c] = x;
    x += columnWidths[c] + 12;
  }
  HRGN total = CreateRectRgn(0, 0, 0, 0);
  // The visible window region must cover the WHOLE line the GDI+ renderer is allowed
  // to draw. The old region was built from GDI TextOut paths; GDI+ has a different
  // baseline and glyph box, so it clipped the new text into horizontal slices.
  // A line rectangle is deliberately used here: it is stable across fonts, sizes
  // and renderers, while still keeping the transparent gaps of the taskbar clickable.
  for (int i = 0; i < shown; ++i)
  {
    const int column = i;
    RECT lineRect = {};
    if (display.vertical)
    {
      lineRect.left = 0;
      lineRect.top = columnLeft[column];
      lineRect.right = client.right;
      lineRect.bottom = std::min((int)client.bottom, columnLeft[column] + columnWidths[column] + 4);
    }
    else
    {
      lineRect.left = columnLeft[column];
      lineRect.top = 0;
      lineRect.right = std::min((int)client.right, columnLeft[column] + columnWidths[column] + 4);
      lineRect.bottom = client.bottom;
    }
    HRGN lineRegion = CreateRectRgn(lineRect.left, lineRect.top, lineRect.right, lineRect.bottom);
    if (lineRegion)
    {
      CombineRgn(total, total, lineRegion, RGN_OR);
      DeleteObject(lineRegion);
    }
  }
  SetWindowRgn(hwnd, total, TRUE);
}

COLORREF TaskbarTextColor()
{
  unsigned rgb = 0;
  if (ParseHexColor(SharedTaskbarTextColor(), &rgb))
    return RGB((rgb >> 16) & 0xffu, (rgb >> 8) & 0xffu, rgb & 0xffu);
  return ::apex::common::SystemIsLightTheme() ? RGB(76, 80, 86) : RGB(188, 191, 196);
}

void RenderTaskbarLayered(HWND hwnd, const Settings &s)
{
  RECT client;
  GetClientRect(hwnd, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 0 || height <= 0)
    return;
  TaskbarGeometry geometry;
  apex::taskbar::Display display = {};
  display.edge = apex::taskbar::Edge::Bottom;
  display.lineCount = 2;
  if (CurrentTaskbarGeometry(std::max(9, -(int)g_uiTaskbarLogFont.lfHeight), &geometry))
    display = geometry.display;
  HDC screen = GetDC(nullptr);
  if (!screen)
    return;
  HDC mem = CreateCompatibleDC(screen);
  if (!mem)
  {
    ReleaseDC(nullptr, screen);
    return;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap)
  {
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
  ApplyTaskbarTextRegion(hwnd, s, display);
  DWORD *pixels = static_cast<DWORD *>(bits);
  if (!PaintTaskbarOverlay(s, pixels, width, height, display))
    ZeroMemory(pixels, (size_t)width * (size_t)height * sizeof(DWORD));
  POINT source = {0, 0};
  SIZE size = {width, height};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(hwnd, screen, nullptr, &size, mem, &source, 0, &blend, ULW_ALPHA);
  SelectObject(mem, oldBitmap);
  DeleteObject(bitmap);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

void PaintOverlay(HWND hwnd)
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  if (g_isTaskbar)
  {
    RenderTaskbarLayered(hwnd, s);
    EndPaint(hwnd, &ps);
    return;
  }
  RECT client;
  GetClientRect(hwnd, &client);
  const bool light = apex::common::SystemIsLightTheme();
  const COLORREF bg = light ? RGB(248, 248, 248) : RGB(22, 24, 22);
  const COLORREF border = light ? RGB(220, 220, 220) : RGB(55, 58, 54);
  const COLORREF fg = light ? RGB(34, 36, 33) : RGB(240, 240, 234);
  const COLORREF sub = light ? RGB(100, 104, 99) : RGB(171, 176, 168);

  SetLayeredWindowAttributes(hwnd, 0, light ? 232 : 218, LWA_ALPHA);
  HBRUSH bgBrush = CreateSolidBrush(bg);
  HPEN borderPen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ oldBrush = SelectObject(dc, bgBrush);
  HGDIOBJ oldPen = SelectObject(dc, borderPen);
  RoundRect(dc, 0, 0, client.right, client.bottom, 20, 20);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(bgBrush);
  DeleteObject(borderPen);

  SetBkMode(dc, TRANSPARENT);
  SYSTEMTIME local;
  GetLocalTime(&local);
  const bool chinese = OverlayChinese();
  HGDIOBJ oldFont = SelectObject(dc, g_uiFont);
  int y = kOverlayPad;
  int shown = 0;
  int hidden = 0;
  for (int i = 0; i < s.n; ++i)
  {
    const Endpoint &e = s.e[i];
    if (!e.enabled)
      continue;
    if (shown >= kOverlayMaxRows)
    {
      ++hidden;
      continue;
    }
    char modelLine[256] = {0};
    char balance[96] = {0};
    FormatBalance(balance, sizeof(balance), e);
    const char *modelName = e.model[0] ? e.model : e.name;
    _snprintf(modelLine, sizeof(modelLine), "%s    %s", modelName, balance);
    wchar_t modelW[512] = {0};
    MultiByteToWideChar(CP_UTF8, 0, modelLine, -1, modelW, (int)(sizeof(modelW) / sizeof(modelW[0])));
    const COLORREF dot = !e.lastOk ? RGB(214, 91, 82) : RGB(76, 175, 110);
    HBRUSH dotBrush = CreateSolidBrush(dot);
    HGDIOBJ oldDot = SelectObject(dc, dotBrush);
    Ellipse(dc, kOverlayPad, y + 8, kOverlayPad + 7, y + 15);
    SelectObject(dc, oldDot);
    DeleteObject(dotBrush);

    SetTextColor(dc, fg);
    RECT rowRect = {kOverlayPad + 15, y, client.right - kOverlayPad, y + 22};
    DrawTextW(dc, modelW, -1, &rowRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += 22;

    int nextChange = -1;
    Period nextPeriod = Period::Normal;
    if (e.scheduleEnabled)
    {
      nextChange = EndpointMinutesUntilChange(e, local);
      if (nextChange >= 0)
      {
        nextPeriod = EndpointPeriodAt(e, AddMinutes(local, nextChange));
        char countdown[128] = {0};
        FormatCountdown(countdown, sizeof(countdown), nextChange, nextPeriod, chinese);
        wchar_t countW[160] = {0};
        MultiByteToWideChar(CP_UTF8, 0, countdown, -1, countW, (int)(sizeof(countW) / sizeof(countW[0])));
        RECT countRect = {kOverlayPad + 15, y, client.right - kOverlayPad, y + 18};
        DrawTextW(dc, countW, -1, &countRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 18;
      }
    }

    if (e.hasUsage)
    {
      char usageText[64] = {0};
      char usageLine[128] = {0};
      FormatNumber(usageText, sizeof(usageText), e.usage);
      _snprintf(usageLine, sizeof(usageLine), "%s %s %s", chinese ? "用量" : "usage", usageText, e.unit);
      wchar_t usageW[256] = {0};
      MultiByteToWideChar(CP_UTF8, 0, usageLine, -1, usageW, (int)(sizeof(usageW) / sizeof(usageW[0])));
      SelectObject(dc, g_uiSmallFont);
      SetTextColor(dc, sub);
      RECT usageRect = {kOverlayPad + 15, y, client.right - kOverlayPad, y + 16};
      DrawTextW(dc, usageW, -1, &usageRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
      y += 16;
    }
    else if (!e.lastOk && e.lastError[0])
    {
      wchar_t errorW[512] = {0};
      MultiByteToWideChar(CP_UTF8, 0, e.lastError, -1, errorW,
                          (int)(sizeof(errorW) / sizeof(errorW[0])));
      SelectObject(dc, g_uiSmallFont);
      SetTextColor(dc, sub);
      RECT errRect = {kOverlayPad + 15, y, client.right - kOverlayPad, y + 16};
      DrawTextW(dc, errorW, -1, &errRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
      y += 16;
    }

    if (e.scheduleEnabled)
    {
      const int left = kOverlayPad;
      const int right = client.right - kOverlayPad;
      const int width = right - left;
      const COLORREF peakColor = light ? RGB(235, 145, 72) : RGB(246, 159, 83);
      const COLORREF valleyColor = light ? RGB(64, 151, 207) : RGB(82, 169, 225);
      int runStart = left;
      Period runPeriod =
          EndpointPeriodAt(e, AddMinutes(local, -local.wHour * 60 - local.wMinute));
      for (int px = 0; px < width; ++px)
      {
        const int minute = (px * 24 * 60) / (width > 0 ? width : 1);
        const Period period =
            EndpointPeriodAt(e, AddMinutes(local, minute - local.wHour * 60 - local.wMinute));
        if (period != runPeriod || px == width - 1)
        {
          const COLORREF color = runPeriod == Period::Peak ? peakColor
                                                          : (runPeriod == Period::Valley ? valleyColor : border);
          HBRUSH brush = CreateSolidBrush(color);
          RECT segment = {runStart, y + 4, left + (px == width - 1 ? width : px), y + 8};
          FillRect(dc, &segment, brush);
          DeleteObject(brush);
          runStart = left + px;
          runPeriod = period;
        }
      }
      SelectObject(dc, g_uiSmallFont);
      SetTextColor(dc, sub);
      for (int tick = 0; tick <= 24; tick += 6)
      {
        char label[8] = {0};
        _snprintf(label, sizeof(label), "%02d", tick == 24 ? 0 : tick);
        wchar_t labelW[16] = {0};
        MultiByteToWideChar(CP_UTF8, 0, label, -1, labelW, (int)(sizeof(labelW) / sizeof(labelW[0])));
        int cx = left + width * tick / 24;
        RECT tickRect = {cx - 12, y + 9, cx + 12, y + 24};
        DrawTextW(dc, labelW, -1, &tickRect, DT_CENTER | DT_SINGLELINE);
      }
      y += 25;
    }
    SelectObject(dc, g_uiFont);
    y += 5;
    ++shown;
  }

  if (hidden)
  {
    char more[64] = {0};
    _snprintf(more, sizeof(more), chinese ? "+%d 个模型" : "+%d models", hidden);
    wchar_t moreW[64] = {0};
    MultiByteToWideChar(CP_UTF8, 0, more, -1, moreW, (int)(sizeof(moreW) / sizeof(moreW[0])));
    SelectObject(dc, g_uiSmallFont);
    SetTextColor(dc, sub);
    RECT moreRect = {kOverlayPad + 15, y, client.right - kOverlayPad, y + 17};
    DrawTextW(dc, moreW, -1, &moreRect, DT_LEFT | DT_SINGLELINE);
    y += 17;
  }

  if (!shown)
  {
    SetTextColor(dc, sub);
    wchar_t empty[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, chinese ? "尚未添加接口" : "No endpoints added", -1, empty,
                        (int)(sizeof(empty) / sizeof(empty[0])));
    RECT emptyRect = {kOverlayPad, y, client.right - kOverlayPad, y + 28};
    DrawTextW(dc, empty, -1, &emptyRect, DT_LEFT | DT_SINGLELINE);
  }
  SelectObject(dc, oldFont);
  EndPaint(hwnd, &ps);
}

void ApplyOverlayRoundRegion(HWND hwnd, int width, int height)
{
  HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 20, 20);
  if (region)
    SetWindowRgn(hwnd, region, TRUE); // the window owns the region after this call
}

struct TaskbarOccupantScan
{
  HWND taskbar = nullptr;
  HWND overlay = nullptr;
  DWORD taskbarPid = 0;
  DWORD selfPid = 0;
  RECT band = {};
  RECT items[32] = {};
  int count = 0;
  int leftmost = 0;
  int rightmost = 0;
};

bool TaskbarBandWindow(HWND hwnd, const TaskbarOccupantScan &scan)
{
  if (!hwnd || hwnd == scan.taskbar || hwnd == scan.overlay || !IsWindowVisible(hwnd))
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid || pid == scan.taskbarPid || pid == scan.selfPid)
    return false;
  RECT r = {};
  if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top)
    return false;
  const int taskbarHeight = scan.band.bottom - scan.band.top;
  if (r.bottom - r.top > taskbarHeight + 2)
    return false;
  // The window must occupy the taskbar's vertical band, not merely cover part of it in passing.
  if (r.top < scan.band.top - 2 || r.bottom > scan.band.bottom + 2)
    return false;
  return r.right > scan.band.left && r.left < scan.band.right;
}

BOOL CALLBACK CollectTaskbarChild(HWND hwnd, LPARAM parameter)
{
  TaskbarOccupantScan *scan = reinterpret_cast<TaskbarOccupantScan *>(parameter);
  if (TaskbarBandWindow(hwnd, *scan) && scan->count < (int)(sizeof(scan->items) / sizeof(scan->items[0])))
  {
    RECT r = {};
    GetWindowRect(hwnd, &r);
    scan->items[scan->count++] = r;
    scan->leftmost = scan->count == 1 ? (int)r.left : std::min(scan->leftmost, (int)r.left);
    scan->rightmost = std::max(scan->rightmost, (int)r.right);
  }
  return TRUE;
}

BOOL CALLBACK CollectTaskbarTopLevel(HWND hwnd, LPARAM parameter)
{
  TaskbarOccupantScan *scan = reinterpret_cast<TaskbarOccupantScan *>(parameter);
  if (!TaskbarBandWindow(hwnd, *scan))
    return TRUE;
  // A same-purpose taskbar overlay is a tool/layered window. This filter keeps ordinary application windows that
  // happen to touch the taskbar edge from pushing ComputeManager around.
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (!(ex & (WS_EX_TOOLWINDOW | WS_EX_LAYERED)))
    return TRUE;
  wchar_t cls[64] = {0};
  GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));
  if (!std::wcscmp(cls, L"#32768") || !_wcsicmp(cls, L"tooltips_class32"))
    return TRUE;
  RECT r = {};
  GetWindowRect(hwnd, &r);
  if (scan->count < (int)(sizeof(scan->items) / sizeof(scan->items[0])))
  {
    scan->items[scan->count++] = r;
    scan->leftmost = scan->count == 1 ? (int)r.left : std::min(scan->leftmost, (int)r.left);
    scan->rightmost = std::max(scan->rightmost, (int)r.right);
  }
  return TRUE;
}

void TaskbarOccupants(HWND taskbar, HWND overlay, const RECT &taskbarRect, TaskbarOccupantScan *out)
{
  TaskbarOccupantScan scan;
  scan.taskbar = taskbar;
  scan.overlay = overlay;
  scan.selfPid = GetCurrentProcessId();
  GetWindowThreadProcessId(taskbar, &scan.taskbarPid);
  scan.band = taskbarRect;
  EnumChildWindows(taskbar, CollectTaskbarChild, reinterpret_cast<LPARAM>(&scan));
  EnumWindows(CollectTaskbarTopLevel, reinterpret_cast<LPARAM>(&scan));
  *out = scan;
}

void UsableTaskbarSpan(const TaskbarGeometry &geometry, long *start, long *end)
{
  const RECT &bar = geometry.taskbar;
  if (geometry.display.vertical)
  {
    *start = bar.top + 4;
    *end = bar.bottom - 4;
  }
  else
  {
    *start = bar.left + 4;
    *end = bar.right - 4;
  }

  HWND notify = FindWindowExW(geometry.window, nullptr, L"TrayNotifyWnd", nullptr);
  RECT notifyRect = {};
  if (!notify || !GetWindowRect(notify, &notifyRect))
    return;
  if (geometry.display.vertical)
  {
    const long center = (notifyRect.top + notifyRect.bottom) / 2;
    if (center >= (bar.top + bar.bottom) / 2)
      *end = std::min(*end, (long)notifyRect.top - 4);
    else
      *start = std::max(*start, (long)notifyRect.bottom + 4);
  }
  else
  {
    const long center = (notifyRect.left + notifyRect.right) / 2;
    if (center >= (bar.left + bar.right) / 2)
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

void UpdateOverlayVisibility()
{
  if (!g_overlay || InterlockedCompareExchange(&g_uiStop, 0, 0) != 0)
    return;
  const bool enabled = !g_host || !g_host->featureEnabled || g_host->featureEnabled(kId) != 0;
  const bool taskbarEnabled =
      !g_host || !g_host->taskbarEnabled || g_host->taskbarEnabled() != 0;
  const bool show = enabled && taskbarEnabled && AnyWatchedProcessRunning();
  if (!show)
  {
    if (g_overlayVisible)
    {
      ShowWindow(g_overlay, SW_HIDE);
      g_overlayVisible = false;
    }
    return;
  }

  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);
  bool anyVisible = false;
  for (int i = 0; i < s.n && !anyVisible; ++i)
    anyVisible = s.e[i].enabled && s.e[i].taskbarVisible;
  if (!anyVisible)
  {
    ShowWindow(g_overlay, SW_HIDE);
    g_overlayVisible = false;
    return;
  }
  if (g_isTaskbar)
  {
    EnsureTaskbarFont();
    TaskbarGeometry geometry;
    const int fontPixels = std::max(9, -(int)g_uiTaskbarLogFont.lfHeight);
    if (!CurrentTaskbarGeometry(fontPixels, &geometry))
    {
      ShowWindow(g_overlay, SW_HIDE);
      g_overlayVisible = false;
      return;
    }
    const RECT &taskbarRect = geometry.taskbar;
    const int taskbarWidth = taskbarRect.right - taskbarRect.left;
    const int taskbarHeight = taskbarRect.bottom - taskbarRect.top;
    const int thickness = apex::taskbar::Thickness(LayoutRect(taskbarRect), geometry.display.edge);
    int extent = TaskbarOverlayExtent(s, geometry.display);
    long axisStart = 0;
    long axisEnd = 0;
    UsableTaskbarSpan(geometry, &axisStart, &axisEnd);
    const long usable = std::max(1L, axisEnd - axisStart - 8);
    extent = (int)std::max(1L, std::min((long)extent, usable));

    const int position = (g_host && g_host->taskbarPosition) ? g_host->taskbarPosition() : 1;
    // Windows offers "left aligned" taskbar icons as well as the centred default. When the icons own the left
    // side, asking for Apex's left area would draw on top of Start and the pinned apps, so the left choice uses
    // the safe right-hand run. Reading the system preference every timer tick makes the original position come
    // back as soon as Windows is switched to centred again.
    const bool nearEnd = apex::taskbar::UseNearEnd(
        position, geometry.display.vertical, TaskbarIconsAreLeftAligned());
    TaskbarOccupantScan occupants;
    TaskbarOccupants(geometry.window, g_overlay, taskbarRect, &occupants);
    apex::taskbar::Span blocked[32] = {};
    int blockedCount = 0;
    for (int i = 0; i < occupants.count; ++i)
    {
      blocked[blockedCount].start =
          geometry.display.vertical ? occupants.items[i].top : occupants.items[i].left;
      blocked[blockedCount].end =
          geometry.display.vertical ? occupants.items[i].bottom : occupants.items[i].right;
      ++blockedCount;
    }
    long placed = apex::taskbar::PlaceSpan(axisStart, axisEnd, extent, blocked, blockedCount, nearEnd, 4, 8);
    if (placed < 0)
    {
      // Other tools can fill the whole usable run. Staying on the requested side and overlapping one is less
      // surprising than obeying a UI that says "left" by drawing on the right.
      placed = nearEnd ? axisEnd - extent : axisStart;
    }

    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    if (geometry.display.vertical)
    {
      width = std::max(16, thickness - 4);
      height = extent;
      x = taskbarRect.left + (taskbarWidth - width) / 2;
      y = (int)placed;
    }
    else
    {
      width = extent;
      height = std::max(16, thickness - 4);
      x = (int)placed;
      y = taskbarRect.top + (taskbarHeight - height) / 2;
    }
    SetWindowPos(g_overlay, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_overlayVisible = true;
    InvalidateRect(g_overlay, nullptr, FALSE);
    return;
  }
  const int height = OverlayHeight(s);
  const int width = OverlayWidth(s);
  RECT work = {0, 0, 0, 0};
  SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
  int x = s.widgetX;
  int y = s.widgetY;
  if (x < 0 || y < 0)
  {
    x = work.right - width - 20;
    y = work.bottom - height - 20;
  }
  if (x + width > work.right)
    x = work.right - width - 8;
  if (y + height > work.bottom)
    y = work.bottom - height - 8;
  if (x < work.left + 8)
    x = work.left + 8;
  if (y < work.top + 8)
    y = work.top + 8;
  if (!g_overlayVisible)
  {
    SetWindowPos(g_overlay, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_overlayVisible = true;
  }
  else
    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, width, height,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
  ApplyOverlayRoundRegion(g_overlay, width, height);
  InvalidateRect(g_overlay, nullptr, FALSE);
}

void SnapOverlayToEdges(HWND hwnd)
{
  RECT r;
  GetWindowRect(hwnd, &r);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!monitor || !GetMonitorInfoW(monitor, &mi))
    return;
  const RECT &work = mi.rcWork;
  const int width = r.right - r.left;
  const int height = r.bottom - r.top;
  const int threshold = 26;
  int left = r.left;
  int top = r.top;
  const int dl = std::abs(r.left - work.left);
  const int dr = std::abs(work.right - r.right);
  const int dt = std::abs(r.top - work.top);
  const int db = std::abs(work.bottom - r.bottom);
  int best = threshold + 1;
  if (dl < best)
  {
    best = dl;
    left = work.left;
    top = r.top;
  }
  if (dr < best)
  {
    best = dr;
    left = work.right - width;
    top = r.top;
  }
  if (dt < best)
  {
    best = dt;
    left = r.left;
    top = work.top;
  }
  if (db < best)
  {
    best = db;
    left = r.left;
    top = work.bottom - height;
  }
  if (left + width > work.right)
    left = work.right - width;
  if (top + height > work.bottom)
    top = work.bottom - height;
  if (left < work.left)
    left = work.left;
  if (top < work.top)
    top = work.top;
  if (left != r.left || top != r.top)
    SetWindowPos(hwnd, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg)
  {
  case WM_NCHITTEST:
    // The taskbar copy is display-only. Returning HTTRANSPARENT makes Windows route every click, right-click
    // and wheel message to the taskbar underneath; the desktop fallback remains draggable.
    return g_isTaskbar ? HTTRANSPARENT : HTCAPTION;
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_PAINT:
    PaintOverlay(hwnd);
    return 0;
  case WM_TIMER:
    UpdateOverlayVisibility();
    return 0;
  case WM_EXITSIZEMOVE:
  {
    if (g_isTaskbar)
      return 0;
    SnapOverlayToEdges(hwnd);
    RECT r;
    GetWindowRect(hwnd, &r);
    EnterCriticalSection(&g_lock);
    g_set.widgetX = r.left;
    g_set.widgetY = r.top;
    LeaveCriticalSection(&g_lock);
    SaveSettings();
    return 0;
  }
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

DWORD WINAPI OverlayThread(LPVOID)
{
  g_uiThreadId = GetCurrentThreadId();
  WNDCLASSW wc = {};
  wc.lpfnWndProc = OverlayProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ApexComputeManagerOverlay";
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    LogLine("feature %s: overlay class registration failed (%lu)", kId, GetLastError());
    if (g_uiReady)
      SetEvent(g_uiReady);
    return 0;
  }
  const char *taskbarTest = std::getenv("APEX_CM_NO_TASKBAR");
  HWND taskbar = (taskbarTest && !std::strcmp(taskbarTest, "1"))
                     ? nullptr
                     : FindWindowW(L"Shell_TrayWnd", nullptr);
  g_isTaskbar = taskbar != nullptr;
  if (g_isTaskbar)
  {
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"ComputeManagerHost", WS_POPUP,
                              0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
  }
  LogLine(g_isTaskbar ? "feature %s: taskbar overlay anchored"
                      : "feature %s: taskbar unavailable; using desktop fallback", kId);
  const DWORD style = WS_POPUP;
  const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE |
                        (g_isTaskbar ? WS_EX_TRANSPARENT : 0);
  g_overlay = CreateWindowExW(exStyle, wc.lpszClassName, L"算力管家", style,
                               0, 0, kOverlayMinWidth, 40, g_isTaskbar ? g_owner : nullptr, nullptr,
                               wc.hInstance, nullptr);
  if (!g_overlay)
  {
    LogLine("feature %s: overlay window creation failed (%lu)", kId, GetLastError());
    if (g_uiReady)
      SetEvent(g_uiReady);
    return 0;
  }
  g_uiFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  g_uiSmallFont = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  EnsureTaskbarFont();
  SetTimer(g_overlay, 1, 500, nullptr);
  if (g_uiReady)
    SetEvent(g_uiReady);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  KillTimer(g_overlay, 1);
  if (g_uiFont)
    DeleteObject(g_uiFont);
  if (g_uiSmallFont)
    DeleteObject(g_uiSmallFont);
  if (g_uiTaskbarFont)
    DeleteObject(g_uiTaskbarFont);
  g_uiFont = nullptr;
  g_uiSmallFont = nullptr;
  g_uiTaskbarFont = nullptr;
  g_uiTaskbarFontSize = 0;
  g_overlay = nullptr;
  if (g_owner)
    DestroyWindow(g_owner);
  g_owner = nullptr;
  g_overlayVisible = false;
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}

void StopOverlay()
{
  InterlockedExchange(&g_uiStop, 1);
  if (g_overlay)
    PostMessageA(g_overlay, WM_CLOSE, 0, 0);
  else if (g_uiThreadId)
    PostThreadMessageA(g_uiThreadId, WM_QUIT, 0, 0);
  if (g_uiThread)
  {
    WaitForSingleObject(g_uiThread, 3000);
    CloseHandle(g_uiThread);
    g_uiThread = nullptr;
  }
  if (g_uiReady)
  {
    CloseHandle(g_uiReady);
    g_uiReady = nullptr;
  }
}

DWORD WINAPI Worker(LPVOID)
{
  while (!InterlockedCompareExchange(&g_stop, 0, 0))
  {
    bool enabled = true;
    if (g_host && g_host->featureEnabled)
      enabled = g_host->featureEnabled(kId) != 0;
    if (enabled)
    {
      FetchPendingSchedules();
      FetchPendingModels();
    }
    const unsigned long long now = GetTickCount64();
    bool due = false;
    if (enabled)
    {
      Settings s;
      unsigned long long nextTick = 0;
      EnterCriticalSection(&g_lock);
      s = g_set;
      nextTick = g_nextPollTick;
      LeaveCriticalSection(&g_lock);
      due = s.autoPoll && (nextTick == 0 || now >= nextTick);
    }
    if (due && enabled)
    {
      PollAll();
      Settings s;
      EnterCriticalSection(&g_lock);
      s = g_set;
      g_nextPollTick = GetTickCount64() + (unsigned long long)s.pollSeconds * 1000ULL;
      LeaveCriticalSection(&g_lock);
    }
    if (!g_wake)
      break;
    WaitForSingleObject(g_wake, 1000);
  }
  return 0;
}

bool ParseGroupPath(const char *path, const char *group, int maxItems, int *index, char *field, int fieldSize)
{
  if (!path || !group || !index || !field)
    return false;
  const size_t groupLen = std::strlen(group);
  if (std::strncmp(path, group, groupLen) || path[groupLen] != '[')
    return false;
  const char *number = path + groupLen + 1;
  char *end = nullptr;
  const long n = std::strtol(number, &end, 10);
  if (end == number || *end != ']' || end[1] != '.' || n < 0 || n >= maxItems)
    return false;
  CopyText(field, fieldSize, end + 2);
  *index = (int)n;
  return true;
}

int SetControl(const char *path, const char *value)
{
  if (!path || !value)
    return 0;
  if (!std::strcmp(path, "auto_poll"))
  {
    bool v = false;
    if (!ParseBool(value, &v))
      return 0;
    EnterCriticalSection(&g_lock);
    g_set.autoPoll = v;
    if (v)
      g_nextPollTick = 0;
    LeaveCriticalSection(&g_lock);
    if (g_wake)
      SetEvent(g_wake);
    return 1;
  }
  if (!std::strcmp(path, "show_advanced"))
  {
    bool v = false;
    if (!ParseBool(value, &v))
      return 0;
    EnterCriticalSection(&g_lock);
    g_set.showAdvanced = v;
    LeaveCriticalSection(&g_lock);
    return 1;
  }
  if (!std::strcmp(path, "poll_seconds"))
  {
    int v = 0;
    if (!ParseInt(value, 1, 60, &v))
      return 0;
    EnterCriticalSection(&g_lock);
    g_set.pollSeconds = v;
    g_nextPollTick = 0;
    LeaveCriticalSection(&g_lock);
    if (g_wake)
      SetEvent(g_wake);
    return 1;
  }
  int index = -1;
  char field[64] = {0};
  if (ParseGroupPath(path, "accounts", kMaxEndpoints, &index, field, (int)sizeof(field)))
  {
    EnterCriticalSection(&g_lock);
    bool ok = index < g_set.n && SetEndpointField(g_set.e[index], field, value);
    if (ok)
    {
      g_set.e[index].revision = ++g_nextRevision;
      g_nextPollTick = 0;
    }
    LeaveCriticalSection(&g_lock);
    if (ok && g_wake)
      SetEvent(g_wake);
    return ok ? 1 : 0;
  }
  if (ParseGroupPath(path, "processes", kMaxProcesses, &index, field, (int)sizeof(field)))
  {
    EnterCriticalSection(&g_lock);
    const bool ok = index < g_set.processN && SetProcessField(g_set.process[index], field, value);
    LeaveCriticalSection(&g_lock);
    return ok ? 1 : 0;
  }
  return 0;
}

int ListOp(const char *id, const char *op, const char *value, int index)
{
  if (!id || !op)
    return 0;
  const bool accounts = std::strcmp(id, "accounts") == 0;
  const bool processes = std::strcmp(id, "processes") == 0;
  int modelIndex = -1;
  char modelField[64] = {0};
  const bool modelList =
      ParseGroupPath(id, "accounts", kMaxEndpoints, &modelIndex, modelField, (int)sizeof(modelField)) &&
      !std::strcmp(modelField, "models");
  if (!accounts && !processes && !modelList)
    return 0;
  if (accounts && !std::strcmp(op, "fetch_models"))
  {
    bool scheduled = false;
    EnterCriticalSection(&g_lock);
    const unsigned long long now = GetTickCount64();
    for (int i = 0; i < g_set.n; ++i)
    {
      Endpoint &e = g_set.e[i];
      if (e.enabled && e.modelsUrl[0] && e.apiKey[0])
      {
        e.modelFetchDue = now;
        scheduled = true;
      }
    }
    LeaveCriticalSection(&g_lock);
    if (g_wake)
      SetEvent(g_wake);
    return scheduled ? 1 : 0;
  }
  if (modelList && !std::strcmp(op, "fetch_models"))
  {
    bool scheduled = false;
    EnterCriticalSection(&g_lock);
    if (modelIndex >= 0 && modelIndex < g_set.n)
    {
      Endpoint &e = g_set.e[modelIndex];
      if (e.modelsUrl[0] && e.apiKey[0])
      {
        e.modelFetchDue = GetTickCount64();
        scheduled = true;
      }
    }
    LeaveCriticalSection(&g_lock);
    if (scheduled && g_wake)
      SetEvent(g_wake);
    return scheduled ? 1 : 0;
  }
  bool changed = false;
  EnterCriticalSection(&g_lock);
  if (modelList && !std::strcmp(op, "add") && modelIndex >= 0 && modelIndex < g_set.n)
  {
    char model[256] = {0};
    TrimCopy(model, sizeof(model), value ? value : "");
    bool duplicate = !model[0];
    Endpoint &e = g_set.e[modelIndex];
    for (const std::string &old : e.models)
      duplicate = duplicate || old == model;
    if (!duplicate && e.models.size() < 64)
    {
      e.models.emplace_back(model);
      if (!e.model[0])
        CopyText(e.model, sizeof(e.model), model);
      changed = true;
    }
  }
  else if (modelList && !std::strcmp(op, "remove") && modelIndex >= 0 && modelIndex < g_set.n &&
           index >= 0 && index < (int)g_set.e[modelIndex].models.size())
  {
    Endpoint &e = g_set.e[modelIndex];
    const std::string removed = e.models[index];
    g_set.e[modelIndex].models.erase(g_set.e[modelIndex].models.begin() + index);
    if (!std::strcmp(e.model, removed.c_str()))
      e.model[0] = 0;
    changed = true;
  }
  else if (accounts && !std::strcmp(op, "add") && g_set.n < kMaxEndpoints)
  {
    Endpoint e;
    char name[kMaxName] = {0};
    TrimCopy(name, sizeof(name), value ? value : "");
    if (!name[0])
      _snprintf(name, sizeof(name), "接口 %d", g_set.n + 1);
    CopyText(e.name, sizeof(e.name), name);
    if (!std::strncmp(name, "http://", 7) || !std::strncmp(name, "https://", 8))
      CopyText(e.url, sizeof(e.url), name);
    e.revision = ++g_nextRevision;
    g_set.e[g_set.n++] = e;
    g_nextPollTick = 0;
    changed = true;
  }
  else if (accounts && !std::strcmp(op, "remove") && index >= 0 && index < g_set.n)
  {
    for (int i = index; i + 1 < g_set.n; ++i)
      g_set.e[i] = g_set.e[i + 1];
    g_set.e[--g_set.n] = Endpoint();
    g_nextPollTick = 0;
    changed = true;
  }
  else if (processes && !std::strcmp(op, "add") && g_set.processN < kMaxProcesses)
  {
    char name[kMaxProcessName] = {0};
    apex::match::NormaliseName(value ? value : "", name, (int)sizeof(name));
    bool duplicate = !name[0];
    for (int i = 0; !duplicate && i < g_set.processN; ++i)
      duplicate = std::strcmp(g_set.process[i].name, name) == 0;
    if (!duplicate)
    {
      ProcessRule p;
      CopyText(p.name, sizeof(p.name), name);
      g_set.process[g_set.processN++] = p;
      changed = true;
    }
  }
  else if (processes && !std::strcmp(op, "remove") && index >= 0 && index < g_set.processN)
  {
    for (int i = index; i + 1 < g_set.processN; ++i)
      g_set.process[i] = g_set.process[i + 1];
    g_set.process[--g_set.processN] = ProcessRule();
    changed = true;
  }
  LeaveCriticalSection(&g_lock);
  if (changed)
  {
    if (g_wake)
      SetEvent(g_wake);
    SaveSettings();
  }
  return changed ? 1 : 0;
}

int Init(const ApexHost *host)
{
  g_host = host;
  if (host && host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));
  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  InitializeCriticalSection(&g_ioLock);
  g_ioLockReady = true;
  OpenLog();
  LoadSettings();
  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!g_wake)
  {
    LogLine("feature %s: could not create the wake event (error %lu)", kId, GetLastError());
    if (g_session)
    {
      WinHttpCloseHandle(g_session);
      g_session = nullptr;
    }
    if (g_log)
    {
      fclose(g_log);
      g_log = nullptr;
    }
    DeleteCriticalSection(&g_ioLock);
    g_ioLockReady = false;
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
    return 1;
  }
  InterlockedExchange(&g_uiStop, 0);
  g_uiReady = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  g_uiThread = g_uiReady ? CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr) : nullptr;
  const bool overlayReady = g_uiThread &&
                            WaitForSingleObject(g_uiReady, 3000) == WAIT_OBJECT_0 && g_overlay != nullptr;
  if (!overlayReady)
  {
    LogLine("feature %s: could not start the desktop overlay", kId);
    StopOverlay();
    CloseHandle(g_wake);
    g_wake = nullptr;
    if (g_log)
    {
      fclose(g_log);
      g_log = nullptr;
    }
    DeleteCriticalSection(&g_ioLock);
    g_ioLockReady = false;
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
    return 1;
  }
  Gdiplus::GdiplusStartupInput gdiplusInput;
  g_gdiplusReady =
      Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok;
  if (!g_gdiplusReady)
    LogLine("feature %s: GDI+ text rendering is unavailable", kId);
  g_session = WinHttpOpen(L"Apex ComputeManager/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (!g_thread)
  {
    LogLine("feature %s: could not start worker (error %lu)", kId, GetLastError());
    StopOverlay();
    if (g_session)
    {
      WinHttpCloseHandle(g_session);
      g_session = nullptr;
    }
    if (g_gdiplusReady)
    {
      Gdiplus::GdiplusShutdown(g_gdiplusToken);
      g_gdiplusToken = 0;
      g_gdiplusReady = false;
    }
    CloseHandle(g_wake);
    g_wake = nullptr;
    if (g_log)
    {
      fclose(g_log);
      g_log = nullptr;
    }
    DeleteCriticalSection(&g_ioLock);
    g_ioLockReady = false;
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
    return 1;
  }
  LogLine("feature %s: started (%d endpoint(s))", kId, g_set.n);
  return 0;
}

void Shutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_wake)
    SetEvent(g_wake);
  if (g_thread)
  {
    // The worker can be inside a WinHTTP request. Its timeouts are bounded, and the lock and
    // session must not be destroyed while its entry point is still executing.
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_wake)
  {
    CloseHandle(g_wake);
    g_wake = nullptr;
  }
  if (g_session)
  {
    WinHttpCloseHandle(g_session);
    g_session = nullptr;
  }
  StopOverlay();
  if (g_gdiplusReady)
  {
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
    g_gdiplusReady = false;
  }
  if (g_log)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  if (g_lockReady)
  {
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
  }
  if (g_ioLockReady)
  {
    DeleteCriticalSection(&g_ioLock);
    g_ioLockReady = false;
  }
}

int Reload()
{
  LoadSettings();
  EnterCriticalSection(&g_lock);
  g_nextPollTick = 0;
  LeaveCriticalSection(&g_lock);
  if (g_wake)
    SetEvent(g_wake);
  return 1;
}

int Save() { return SaveSettings() ? 1 : 0; }

int TaskbarItems(ApexTaskbarArea *out, int max)
{
  bool visible = false;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_set.n && !visible; ++i)
    visible = g_set.e[i].enabled && g_set.e[i].taskbarVisible;
  LeaveCriticalSection(&g_lock);
  if (!visible)
    return 0;
  if (out && max > 0)
  {
    ZeroMemory(out, sizeof(ApexTaskbarArea));
    _snprintf(out[0].key, sizeof(out[0].key), "%s", kId);
    _snprintf(out[0].nameZh, sizeof(out[0].nameZh), "%s", "算力管家");
    _snprintf(out[0].nameEn, sizeof(out[0].nameEn), "%s", "Compute Manager");
  }
  return 1;
}

unsigned Flags() { return APEX_FEATURE_ENABLED; }

} // namespace

static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    kId,
    "算力管家",
    "Compute Manager",
    kVersion,
    Init,
    Shutdown,
    Reload,
    SettingsJson,
    SetControl,
    ListOp,
    nullptr,
    Save,
    nullptr,
    nullptr,
    Flags,
    TaskbarItems,
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
