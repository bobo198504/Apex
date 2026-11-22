// SystemStatus -- small, independently mappable pieces of machine state on the taskbar.
//
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// The feature owns its samplers, taskbar window, settings file, and the meaning of each mapping switch.
// The host only knows the generic ApexFeature ABI and the generic taskbar display registry.

#include "../../apex/abi.h"
#include "system_status_logic.h"

#include <activation.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <inspectable.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <roapi.h>
#include <wbemidl.h>
#include <winstring.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

using apex::system_status::FormatPercent;
using apex::system_status::FormatRate;
using apex::system_status::FormatTemperature;
using apex::system_status::ParseBool;
using apex::system_status::WeekdayEn;
using apex::system_status::WeekdayZh;

const char *kId = "SystemStatus";
const char *kVersion = "1.5.3";
constexpr int kMaxMetrics = 64;
constexpr int kMaxMetricId = 96;
constexpr int kMaxVolumes = 12;
constexpr int kMaxAddresses = 12;
constexpr int kMaxGpus = 8;
constexpr int kLogCap = 96 * 1024;

struct MetricSnapshot
{
  char id[kMaxMetricId] = {};
  char nameZh[96] = {};
  char nameEn[96] = {};
  char statusZh[512] = {};
  char statusEn[512] = {};
  char shortText[384] = {};
  char shortEn[384] = {};
  const char *level = "ok";
  bool hasUnitChoice = false;
  char unit[16] = "percent";
};

struct VisibleSetting
{
  char id[kMaxMetricId] = {};
  bool visible = false;
  char unit[16] = "percent";
};

struct Settings
{
  VisibleSetting values[kMaxMetrics] = {};
  int count = 0;
  bool legacyGpu = false;
  bool legacyStorage = false;
  bool legacyIp = false;
};

struct Snapshot
{
  MetricSnapshot metrics[kMaxMetrics] = {};
  int metricCount = 0;
  unsigned long long revision = 0;
};

struct VolumeReading
{
  char label[8] = {};
  unsigned long long total = 0;
  unsigned long long free = 0;
  double readBytesPerSecond = 0.0;
  double writeBytesPerSecond = 0.0;
  double temperature = 0.0;
  bool hasTemperature = false;
};

struct AddressReading
{
  char address[80] = {};
  char adapter[128] = {};
  int family = 0;
  unsigned long ifIndex = 0;
};

struct GpuReading
{
  char name[128] = {};
  char shortName[32] = {};
  unsigned long luidHigh = 0;
  unsigned long luidLow = 0;
  double percent = 0.0;
  unsigned long long localUsageBytes = 0;
  unsigned long long localBudgetBytes = 0;
  unsigned long long memoryUsageBytes = 0;
  unsigned long long memoryBudgetBytes = 0;
  double temperature = 0.0;
  bool hasTemperature = false;
};

struct Reading
{
  double download = 0.0;
  double upload = 0.0;
  double cpuPercent = 0.0;
  double cpuMhz = 0.0;
  double memoryPercent = 0.0;
  unsigned long long memoryUsedBytes = 0;
  double storagePercent = 0.0;
  double gpuPercent = 0.0;
  double cpuTemp = 0.0;
  double memoryTemp = 0.0;
  double gpuTemp = 0.0;
  double storageTemp = 0.0;
  bool hasCpuTemp = false;
  bool hasMemoryTemp = false;
  bool hasGpuTemp = false;
  bool hasStorageTemp = false;
  bool hasGpu = false;
  bool hasIp = false;
  bool hasLunar = false;
  char primaryVolume[8] = {};
  VolumeReading volumes[kMaxVolumes] = {};
  int volumeCount = 0;
  AddressReading addresses[kMaxAddresses] = {};
  int addressCount = 0;
  GpuReading gpus[kMaxGpus] = {};
  int gpuCount = 0;
  char lunarZh[192] = {};
  char lunarEn[192] = {};
  char lunarYearZh[64] = {};
  char lunarYearEn[64] = {};
};

Settings g_settings;
Snapshot g_snapshot;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
const ApexHost *g_host = nullptr;
char g_dir[512] = {0};
FILE *g_log = nullptr;

HANDLE g_sampleThread = nullptr;
HANDLE g_sampleWake = nullptr;
volatile LONG g_stop = 0;
unsigned long long g_snapshotRevision = 0;

unsigned long long g_prevIdle = 0;
unsigned long long g_prevKernel = 0;
unsigned long long g_prevUser = 0;
bool g_haveCpuBaseline = false;
unsigned long long g_prevInOctets = 0;
unsigned long long g_prevOutOctets = 0;
bool g_haveNetworkBaseline = false;
unsigned long long g_prevNetworkTick = 0;

PDH_HQUERY g_gpuQuery = nullptr;
PDH_HCOUNTER g_gpuCounter = nullptr;
bool g_gpuCounterReady = false;
PDH_HQUERY g_diskQuery = nullptr;
PDH_HCOUNTER g_diskReadCounter = nullptr;
PDH_HCOUNTER g_diskWriteCounter = nullptr;
bool g_diskCountersReady = false;

void CopyText(char *dst, int cap, const char *src)
{
  if (!dst || cap <= 0)
    return;
  if (!src)
  {
    dst[0] = 0;
    return;
  }
  int n = 0;
  while (src[n] && n + 1 < cap)
  {
    const unsigned char c = (unsigned char)src[n];
    if (c == '\r' || c == '\n' || (c < 0x20 && c != '\t'))
      break;
    ++n;
  }
  std::memcpy(dst, src, (size_t)n);
  dst[n] = 0;
}

void AppendText(char *out, int outSize, int &off, const char *fmt, ...)
{
  if (!out || off < 0 || off >= outSize)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + off, (size_t)(outSize - off), fmt, ap);
  va_end(ap);
  if (n > 0)
    off = std::min(outSize - 1, off + n);
}

void AppendJsonString(char *out, int outSize, int &off, const char *text)
{
  AppendText(out, outSize, off, "\"");
  if (text)
  {
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    {
      switch (*p)
      {
      case '"':
        AppendText(out, outSize, off, "\\\"");
        break;
      case '\\':
        AppendText(out, outSize, off, "\\\\");
        break;
      case '\n':
        AppendText(out, outSize, off, "\\n");
        break;
      case '\r':
        AppendText(out, outSize, off, "\\r");
        break;
      case '\t':
        AppendText(out, outSize, off, "\\t");
        break;
      default:
        if (*p < 0x20)
          AppendText(out, outSize, off, " ");
        else
          AppendText(out, outSize, off, "%c", *p);
        break;
      }
    }
  }
  AppendText(out, outSize, off, "\"");
}

bool ConfigPath(char *out, int outSize)
{
  if (!g_dir[0])
    return false;
  const int n = _snprintf(out, outSize, "%sSystemStatus.ini", g_dir);
  return n > 0 && n < outSize;
}

void LogLine(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[512] = {};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_host->logLine(buf);
}

void WriteLog(const char *fmt, ...)
{
  if (!g_log)
    return;
  char buf[512] = {};
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

void OpenLog()
{
  char path[560] = {};
  if (_snprintf(path, sizeof(path), "%ssystemstatus.log", g_dir) > 0)
    g_log = fopen(path, "w");
}

bool DefaultMetricVisible(const char *id)
{
  return !std::strcmp(id, "net_down") || !std::strcmp(id, "net_up") ||
         !std::strcmp(id, "cpu_usage") || !std::strcmp(id, "ram_usage") ||
         !std::strcmp(id, "clock");
}

bool SettingVisible(const Settings &settings, const char *id)
{
  for (int i = 0; i < settings.count; ++i)
    if (!std::strcmp(settings.values[i].id, id))
      return settings.values[i].visible;
  if (!std::strncmp(id, "gpu_", 4))
    return settings.legacyGpu;
  if (!std::strncmp(id, "disk_", 5))
    return settings.legacyStorage;
  if (!std::strncmp(id, "ip_", 3))
    return settings.legacyIp;
  return DefaultMetricVisible(id);
}

void SetSettingVisible(Settings &settings, const char *id, bool visible)
{
  if (!id || !*id)
    return;
  for (int i = 0; i < settings.count; ++i)
    if (!std::strcmp(settings.values[i].id, id))
    {
      settings.values[i].visible = visible;
      return;
    }
  if (settings.count >= kMaxMetrics)
    return;
  VisibleSetting &entry = settings.values[settings.count++];
  CopyText(entry.id, sizeof(entry.id), id);
  entry.visible = visible;
}

bool HasUnitChoice(const char *id)
{
  if (!id)
    return false;
  return !std::strcmp(id, "cpu_usage") || !std::strcmp(id, "ram_usage") ||
         (!std::strncmp(id, "gpu_", 4) &&
          std::strlen(id) >= 6 && !std::strcmp(id + std::strlen(id) - 6, "_usage"));
}

bool UnitAllowed(const char *id, const char *unit)
{
  if (!id || !unit)
    return false;
  if (!HasUnitChoice(id))
    return false;
  if (!std::strcmp(id, "cpu_usage"))
    return !std::strcmp(unit, "percent") || !std::strcmp(unit, "ghz");
  if (!std::strcmp(id, "ram_usage"))
    return !std::strcmp(unit, "percent") || !std::strcmp(unit, "gb");
  if (!std::strncmp(id, "gpu_", 4))
    return !std::strcmp(unit, "percent") || !std::strcmp(unit, "gb");
  return false;
}

const char *MetricUnit(const Settings &settings, const char *id)
{
  for (int i = 0; i < settings.count; ++i)
    if (!std::strcmp(settings.values[i].id, id))
      return settings.values[i].unit;
  return "percent";
}

bool SetMetricUnit(Settings &settings, const char *id, const char *unit)
{
  if (!HasUnitChoice(id) || !UnitAllowed(id, unit))
    return false;
  for (int i = 0; i < settings.count; ++i)
    if (!std::strcmp(settings.values[i].id, id))
    {
      CopyText(settings.values[i].unit, sizeof(settings.values[i].unit), unit);
      return true;
    }
  if (settings.count >= kMaxMetrics)
    return false;
  VisibleSetting &entry = settings.values[settings.count++];
  CopyText(entry.id, sizeof(entry.id), id);
  entry.visible = DefaultMetricVisible(id);
  CopyText(entry.unit, sizeof(entry.unit), unit);
  return true;
}

const char *MigratedKey(const char *key)
{
  if (!key)
    return nullptr;
  if (!std::strcmp(key, "show_download")) return "net_down";
  if (!std::strcmp(key, "show_upload")) return "net_up";
  if (!std::strcmp(key, "show_cpu")) return "cpu_usage";
  if (!std::strcmp(key, "show_memory")) return "ram_usage";
  if (!std::strcmp(key, "show_clock")) return "clock";
  if (!std::strcmp(key, "show_date")) return "date_calendar";
  if (!std::strcmp(key, "show_lunar")) return "lunar_date";
  return nullptr;
}

void LoadSettings()
{
  Settings fresh;
  char path[560] = {};
  if (ConfigPath(path, (int)sizeof(path)))
  {
    FILE *f = fopen(path, "rb");
    if (f)
    {
      char line[512] = {};
      while (fgets(line, sizeof(line), f))
      {
        char *eq = std::strchr(line, '=');
        if (!eq)
          continue;
        *eq = 0;
        char *key = line;
        char *value = eq + 1;
        while (*key == ' ' || *key == '\t')
          ++key;
        while (*value == ' ' || *value == '\t')
          ++value;
        char *end = value + std::strlen(value);
        while (end > value && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' ||
                              end[-1] == '\t'))
          *--end = 0;
        const size_t keyLength = std::strlen(key);
        if (keyLength > 5 && !std::strcmp(key + keyLength - 5, ".unit"))
        {
          char metric[kMaxMetricId] = {};
          const size_t metricLength = std::min(keyLength - 5, sizeof(metric) - 1);
          std::memcpy(metric, key, metricLength);
          metric[metricLength] = 0;
          SetMetricUnit(fresh, metric, value);
          continue;
        }
        bool on = false;
        if (!ParseBool(value, &on))
          continue;
        if (!std::strcmp(key, "show_cpu"))
        {
          SetSettingVisible(fresh, "cpu_usage", on);
          SetSettingVisible(fresh, "cpu_temp", on);
        }
        else if (!std::strcmp(key, "show_memory"))
        {
          SetSettingVisible(fresh, "ram_usage", on);
          SetSettingVisible(fresh, "ram_temp", on);
        }
        else if (!std::strcmp(key, "show_storage"))
          fresh.legacyStorage = on;
        else if (!std::strcmp(key, "show_gpu"))
          fresh.legacyGpu = on;
        else if (!std::strcmp(key, "show_ip") || !std::strcmp(key, "ip_local"))
          fresh.legacyIp = on;
        else
        {
          const char *mapped = MigratedKey(key);
          SetSettingVisible(fresh, mapped ? mapped : key, on);
        }
      }
      fclose(f);
    }
  }
  EnterCriticalSection(&g_lock);
  g_settings = fresh;
  LeaveCriticalSection(&g_lock);
}

bool SaveSettings()
{
  char path[560] = {};
  char temp[580] = {};
  if (!ConfigPath(path, (int)sizeof(path)) ||
      _snprintf(temp, sizeof(temp), "%s.tmp", path) <= 0)
    return false;
  Settings settings;
  Snapshot snapshot;
  EnterCriticalSection(&g_lock);
  settings = g_settings;
  snapshot = g_snapshot;
  LeaveCriticalSection(&g_lock);
  FILE *f = fopen(temp, "wb");
  if (!f)
    return false;
  fprintf(f, "# SystemStatus taskbar mappings\n");
  for (int i = 0; i < settings.count; ++i)
  {
    fprintf(f, "%s=%d\n", settings.values[i].id, settings.values[i].visible ? 1 : 0);
    if (HasUnitChoice(settings.values[i].id))
      fprintf(f, "%s.unit=%s\n", settings.values[i].id,
              UnitAllowed(settings.values[i].id, settings.values[i].unit)
                  ? settings.values[i].unit
                  : "percent");
  }
  for (int i = 0; i < snapshot.metricCount; ++i)
  {
    const char *id = snapshot.metrics[i].id;
    bool explicitEntry = false;
    for (int k = 0; k < settings.count; ++k)
      explicitEntry = explicitEntry || !std::strcmp(settings.values[k].id, id);
    if (!explicitEntry)
    {
      fprintf(f, "%s=%d\n", id, SettingVisible(settings, id) ? 1 : 0);
      if (HasUnitChoice(id))
        fprintf(f, "%s.unit=percent\n", id);
    }
  }
  fflush(f);
  fclose(f);
  if (!MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    DeleteFileA(temp);
    return false;
  }
  return true;
}

void FormatBytes(unsigned long long bytes, char *out, int outSize)
{
  static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = (double)bytes;
  int unit = 0;
  while (value >= 1024.0 && unit < 4)
  {
    value /= 1024.0;
    ++unit;
  }
  if (unit <= 1)
    _snprintf(out, outSize, "%.0f %s", value, units[unit]);
  else if (value < 10.0)
    _snprintf(out, outSize, "%.1f %s", value, units[unit]);
  else
    _snprintf(out, outSize, "%.0f %s", value, units[unit]);
}

const char *LunarMonthZh(int month)
{
  static const char *names[] = {
      "", "正月", "二月", "三月", "四月", "五月", "六月",
      "七月", "八月", "九月", "十月", "冬月", "腊月"};
  return month >= 1 && month <= 12 ? names[month] : "";
}

void LunarDayZh(int day, char *out, int outSize)
{
  static const char *digits[] = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  if (day <= 0 || day > 30)
  {
    _snprintf(out, outSize, "%d", day);
    return;
  }
  if (day <= 10)
    _snprintf(out, outSize, "初%s", digits[day]);
  else if (day < 20)
    _snprintf(out, outSize, "十%s", digits[day - 10]);
  else if (day == 20)
    _snprintf(out, outSize, "二十");
  else if (day < 30)
    _snprintf(out, outSize, "廿%s", digits[day - 20]);
  else
    _snprintf(out, outSize, "三十");
}

void SexagenaryYear(int gregorianYear, char *out, int outSize)
{
  static const char *stems[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
  static const char *branches[] = {
      "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
  const int year = gregorianYear - 4;
  const int stem = ((year % 10) + 10) % 10;
  const int branch = ((year % 12) + 12) % 12;
  _snprintf(out, outSize, "%s%s年", stems[stem], branches[branch]);
}

double CpuUsage()
{
  FILETIME idleTime = {}, kernelTime = {}, userTime = {};
  if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
    return 0.0;
  auto value = [](const FILETIME &ft) -> unsigned long long {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
  };
  const unsigned long long idle = value(idleTime);
  const unsigned long long kernel = value(kernelTime);
  const unsigned long long user = value(userTime);
  if (!g_haveCpuBaseline)
  {
    g_prevIdle = idle;
    g_prevKernel = kernel;
    g_prevUser = user;
    g_haveCpuBaseline = true;
    return 0.0;
  }
  const unsigned long long idleDelta = idle - g_prevIdle;
  const unsigned long long totalDelta = (kernel - g_prevKernel) + (user - g_prevUser);
  g_prevIdle = idle;
  g_prevKernel = kernel;
  g_prevUser = user;
  if (!totalDelta)
    return 0.0;
  return 100.0 * (double)(totalDelta - std::min(totalDelta, idleDelta)) / (double)totalDelta;
}

double MemoryUsage(unsigned long long *usedBytes)
{
  MEMORYSTATUSEX memory = {};
  memory.dwLength = sizeof(memory);
  if (!GlobalMemoryStatusEx(&memory) || !memory.ullTotalPhys)
    return 0.0;
  if (usedBytes)
    *usedBytes = memory.ullTotalPhys - memory.ullAvailPhys;
  return 100.0 * (double)(memory.ullTotalPhys - memory.ullAvailPhys) /
         (double)memory.ullTotalPhys;
}

double CpuMhz()
{
  struct ProcessorPowerInfo
  {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
  };
  SYSTEM_INFO info = {};
  GetSystemInfo(&info);
  const DWORD count = std::max<DWORD>(1, info.dwNumberOfProcessors);
  std::vector<ProcessorPowerInfo> values(count);
  if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, values.data(),
                             (ULONG)(values.size() * sizeof(values[0]))) != ERROR_SUCCESS)
    return 0.0;
  double total = 0.0;
  for (const ProcessorPowerInfo &value : values)
    total += value.CurrentMhz;
  return total / (double)values.size();
}

void CollectVolumes(Reading *reading)
{
  wchar_t drives[1024] = {};
  GetLogicalDriveStringsW((DWORD)(sizeof(drives) / sizeof(drives[0])) - 1, drives);
  unsigned long long totalAll = 0;
  unsigned long long freeAll = 0;
  wchar_t windowsDir[MAX_PATH] = {};
  GetWindowsDirectoryW(windowsDir, MAX_PATH);
  const wchar_t systemLetter = windowsDir[0];
  for (const wchar_t *p = drives; p && *p && reading->volumeCount < kMaxVolumes; p += std::wcslen(p) + 1)
  {
    if (GetDriveTypeW(p) != DRIVE_FIXED)
      continue;
    ULARGE_INTEGER freeBytes = {}, totalBytes = {}, available = {};
    if (!GetDiskFreeSpaceExW(p, &available, &totalBytes, &freeBytes) || !totalBytes.QuadPart)
      continue;
    VolumeReading &v = reading->volumes[reading->volumeCount++];
    v.label[0] = (char)p[0];
    v.label[1] = 0;
    v.total = totalBytes.QuadPart;
    v.free = freeBytes.QuadPart;
    totalAll += v.total;
    freeAll += v.free;
    if (p[0] == systemLetter || !reading->primaryVolume[0])
    {
      reading->primaryVolume[0] = (char)p[0];
      reading->primaryVolume[1] = 0;
    }
  }
  if (totalAll)
    reading->storagePercent = 100.0 * (double)(totalAll - freeAll) / (double)totalAll;
}

const char *AddressFamilyName(int family)
{
  return family == AF_INET ? "IPv4" : "IPv6";
}

void CollectAddresses(Reading *reading)
{
  ULONG size = 16 * 1024;
  std::vector<BYTE> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3; ++attempt)
  {
    buffer.resize(size);
    result = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
    if (result != ERROR_BUFFER_OVERFLOW)
      break;
  }
  if (result != NO_ERROR)
    return;
  for (IP_ADAPTER_ADDRESSES *adapter =
           reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
       adapter && reading->addressCount < kMaxAddresses; adapter = adapter->Next)
  {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;
    for (IP_ADAPTER_UNICAST_ADDRESS *ua = adapter->FirstUnicastAddress;
         ua && reading->addressCount < kMaxAddresses; ua = ua->Next)
    {
      if (!ua->Address.lpSockaddr)
        continue;
      AddressReading &address = reading->addresses[reading->addressCount];
      if (ua->Address.lpSockaddr->sa_family == AF_INET)
      {
        const sockaddr_in *v4 = reinterpret_cast<const sockaddr_in *>(ua->Address.lpSockaddr);
        const BYTE *b = reinterpret_cast<const BYTE *>(&v4->sin_addr);
        if (b[0] == 169 && b[1] == 254)
          continue;
        wchar_t wide[80] = {};
        if (!InetNtopW(AF_INET, &v4->sin_addr, wide,
                       (DWORD)(sizeof(wide) / sizeof(wide[0]))))
          continue;
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, address.address,
                            sizeof(address.address), nullptr, nullptr);
        address.family = AF_INET;
      }
      else if (ua->Address.lpSockaddr->sa_family == AF_INET6)
      {
        const sockaddr_in6 *v6 = reinterpret_cast<const sockaddr_in6 *>(ua->Address.lpSockaddr);
        const BYTE *b = v6->sin6_addr.s6_addr;
        const bool loopback =
            b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 &&
            b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
            b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 0 &&
            b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] == 1;
        const bool linkLocal = b[0] == 0xfe && (b[1] & 0xc0) == 0x80;
        if (loopback || linkLocal)
          continue;
        wchar_t wide[80] = {};
        if (!InetNtopW(AF_INET6, &v6->sin6_addr, wide,
                       (DWORD)(sizeof(wide) / sizeof(wide[0]))))
          continue;
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, address.address,
                            sizeof(address.address), nullptr, nullptr);
        address.family = AF_INET6;
      }
      else
        continue;
      WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, address.adapter,
                          sizeof(address.adapter), nullptr, nullptr);
      address.ifIndex = adapter->IfIndex;
      ++reading->addressCount;
    }
  }
  std::stable_sort(reading->addresses, reading->addresses + reading->addressCount,
                   [](const AddressReading &a, const AddressReading &b) {
                     return a.family == AF_INET && b.family != AF_INET;
                   });
  reading->hasIp = reading->addressCount > 0;
}

void CollectNetwork(Reading *reading, unsigned long long now)
{
  PMIB_IF_TABLE2 table = nullptr;
  if (GetIfTable2(&table) != NO_ERROR || !table)
    return;
  unsigned long long inOctets = 0;
  unsigned long long outOctets = 0;
  for (ULONG i = 0; i < table->NumEntries; ++i)
  {
    const MIB_IF_ROW2 &row = table->Table[i];
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.OperStatus != IfOperStatusUp)
      continue;
    inOctets += row.InOctets;
    outOctets += row.OutOctets;
  }
  FreeMibTable(table);
  if (g_haveNetworkBaseline && now > g_prevNetworkTick)
  {
    const double seconds = (double)(now - g_prevNetworkTick) / 1000.0;
    reading->download = inOctets >= g_prevInOctets
                            ? (double)(inOctets - g_prevInOctets) / seconds
                            : 0.0;
    reading->upload = outOctets >= g_prevOutOctets
                          ? (double)(outOctets - g_prevOutOctets) / seconds
                          : 0.0;
  }
  g_prevInOctets = inOctets;
  g_prevOutOctets = outOctets;
  g_prevNetworkTick = now;
  g_haveNetworkBaseline = true;
}

void InitializeGpuCounter()
{
  if (g_gpuQuery || PdhOpenQueryW(nullptr, 0, &g_gpuQuery) != (PDH_STATUS)ERROR_SUCCESS)
    return;
  const PDH_STATUS add = PdhAddEnglishCounterW(
      g_gpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpuCounter);
  if (add != (PDH_STATUS)ERROR_SUCCESS)
  {
    PdhCloseQuery(g_gpuQuery);
    g_gpuQuery = nullptr;
    g_gpuCounter = nullptr;
    return;
  }
  PdhCollectQueryData(g_gpuQuery);
  g_gpuCounterReady = true;
}

bool ContainsNoCase(const char *text, const char *needle)
{
  if (!text || !needle)
    return false;
  const size_t n = std::strlen(needle);
  for (const char *p = text; *p; ++p)
  {
    size_t i = 0;
    while (i < n && p[i] && std::tolower((unsigned char)p[i]) ==
                                 std::tolower((unsigned char)needle[i]))
      ++i;
    if (i == n)
      return true;
  }
  return false;
}

const char *ShortGpuName(const char *name)
{
  if (ContainsNoCase(name, "intel"))
    return "Intel";
  if (ContainsNoCase(name, "nvidia"))
    return "NVIDIA";
  if (ContainsNoCase(name, "radeon") || ContainsNoCase(name, "amd"))
    return "AMD";
  if (ContainsNoCase(name, "qualcomm"))
    return "Qualcomm";
  return "GPU";
}

bool DiscoverGpuAdapters(Reading *reading)
{
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || !factory)
    return false;
  for (UINT index = 0; reading->gpuCount < kMaxGpus; ++index)
  {
    IDXGIAdapter1 *adapter = nullptr;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    if (!adapter)
      continue;
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
    {
      GpuReading &gpu = reading->gpus[reading->gpuCount++];
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu.name, sizeof(gpu.name),
                          nullptr, nullptr);
      CopyText(gpu.shortName, sizeof(gpu.shortName), ShortGpuName(gpu.name));
      gpu.luidHigh = desc.AdapterLuid.HighPart;
      gpu.luidLow = desc.AdapterLuid.LowPart;
      IDXGIAdapter3 *adapter3 = nullptr;
      if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&adapter3)) &&
          adapter3)
      {
        DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo)))
        {
          gpu.localUsageBytes = memoryInfo.CurrentUsage;
          gpu.localBudgetBytes = memoryInfo.Budget;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO sharedInfo = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sharedInfo)))
        {
          if (gpu.localBudgetBytes)
          {
            gpu.memoryUsageBytes = gpu.localUsageBytes;
            gpu.memoryBudgetBytes = gpu.localBudgetBytes;
          }
          else if (sharedInfo.Budget)
          {
            gpu.memoryUsageBytes = sharedInfo.CurrentUsage;
            gpu.memoryBudgetBytes = sharedInfo.Budget;
          }
        }
        else if (gpu.localBudgetBytes)
        {
          gpu.memoryUsageBytes = gpu.localUsageBytes;
          gpu.memoryBudgetBytes = gpu.localBudgetBytes;
        }
        if (!gpu.memoryBudgetBytes)
          gpu.memoryBudgetBytes = desc.DedicatedVideoMemory;
        adapter3->Release();
      }
      if (!gpu.memoryBudgetBytes)
        gpu.memoryBudgetBytes = desc.DedicatedVideoMemory;
    }
    adapter->Release();
  }
  factory->Release();
  reading->hasGpu = reading->gpuCount > 0;
  return reading->hasGpu;
}

bool PdhLuid(const wchar_t *instance, unsigned long *high, unsigned long *low)
{
  if (!instance || !high || !low)
    return false;
  const wchar_t *p = std::wcsstr(instance, L"luid_");
  if (!p)
    return false;
  p += 5;
  wchar_t *end = nullptr;
  *high = std::wcstoul(p, &end, 0);
  if (!end || *end != L'_')
    return false;
  *low = std::wcstoul(end + 1, &end, 0);
  return end && (*end == 0 || *end == L'_');
}

void CollectGpuUsage(Reading *reading)
{
  if (!g_gpuCounterReady ||
      PdhCollectQueryData(g_gpuQuery) != (PDH_STATUS)ERROR_SUCCESS)
    return;
  DWORD size = 0;
  DWORD count = 0;
  PDH_STATUS status = PdhGetFormattedCounterArrayW(
      g_gpuCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
  if (status != (PDH_STATUS)PDH_MORE_DATA || !size)
    return;
  std::vector<BYTE> buffer(size + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
  auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
  status = PdhGetFormattedCounterArrayW(
      g_gpuCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
  if (status != (PDH_STATUS)ERROR_SUCCESS)
    return;
  for (DWORD i = 0; i < count; ++i)
  {
    if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
        items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
      continue;
    unsigned long high = 0, low = 0;
    if (!PdhLuid(items[i].szName, &high, &low))
      continue;
    for (int gpu = 0; gpu < reading->gpuCount; ++gpu)
    {
      if (reading->gpus[gpu].luidHigh == high && reading->gpus[gpu].luidLow == low)
      {
        reading->gpus[gpu].percent += items[i].FmtValue.doubleValue;
        break;
      }
    }
  }
  for (int gpu = 0; gpu < reading->gpuCount; ++gpu)
  {
    reading->gpus[gpu].percent =
        std::max(0.0, std::min(100.0, reading->gpus[gpu].percent));
    reading->gpuPercent = std::max(reading->gpuPercent, reading->gpus[gpu].percent);
  }
}

double AggregateGpuUsage()
{
  if (PdhCollectQueryData(g_gpuQuery) != (PDH_STATUS)ERROR_SUCCESS)
    return 0.0;
  DWORD size = 0;
  DWORD count = 0;
  PDH_STATUS status = PdhGetFormattedCounterArrayW(
      g_gpuCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
  if (status != (PDH_STATUS)PDH_MORE_DATA || !size)
    return 0.0;
  std::vector<BYTE> buffer(size + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
  auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
  status = PdhGetFormattedCounterArrayW(
      g_gpuCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
  if (status != (PDH_STATUS)ERROR_SUCCESS)
    return 0.0;
  double total = 0.0;
  for (DWORD i = 0; i < count; ++i)
  {
    if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
        items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
      total += items[i].FmtValue.doubleValue;
  }
  return std::max(0.0, std::min(100.0, total));
}

void InitializeDiskCounters()
{
  if (g_diskQuery ||
      PdhOpenQueryW(nullptr, 0, &g_diskQuery) != (PDH_STATUS)ERROR_SUCCESS)
    return;
  const PDH_STATUS read = PdhAddEnglishCounterW(
      g_diskQuery, L"\\LogicalDisk(*)\\Disk Read Bytes/sec", 0, &g_diskReadCounter);
  const PDH_STATUS write = PdhAddEnglishCounterW(
      g_diskQuery, L"\\LogicalDisk(*)\\Disk Write Bytes/sec", 0, &g_diskWriteCounter);
  if (read != (PDH_STATUS)ERROR_SUCCESS || write != (PDH_STATUS)ERROR_SUCCESS)
  {
    PdhCloseQuery(g_diskQuery);
    g_diskQuery = nullptr;
    g_diskReadCounter = nullptr;
    g_diskWriteCounter = nullptr;
    return;
  }
  PdhCollectQueryData(g_diskQuery);
  g_diskCountersReady = true;
}

void ApplyDiskRate(PDH_HCOUNTER counter, bool read, Reading *reading)
{
  if (!counter || !reading)
    return;
  DWORD size = 0;
  DWORD count = 0;
  PDH_STATUS status = PdhGetFormattedCounterArrayW(
      counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
  if (status != (PDH_STATUS)PDH_MORE_DATA || !size)
    return;
  std::vector<BYTE> buffer(size + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
  auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
  status = PdhGetFormattedCounterArrayW(
      counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
  if (status != (PDH_STATUS)ERROR_SUCCESS)
    return;
  for (DWORD i = 0; i < count; ++i)
  {
    if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
        items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
      continue;
    for (int volume = 0; volume < reading->volumeCount; ++volume)
    {
      const char label = reading->volumes[volume].label[0];
      if (items[i].szName[0] == (wchar_t)label && items[i].szName[1] == L':')
      {
        const double value = std::max(0.0, items[i].FmtValue.doubleValue);
        if (read)
          reading->volumes[volume].readBytesPerSecond = value;
        else
          reading->volumes[volume].writeBytesPerSecond = value;
        break;
      }
    }
  }
}

void CollectDiskSpeeds(Reading *reading)
{
  if (!g_diskCountersReady ||
      PdhCollectQueryData(g_diskQuery) != (PDH_STATUS)ERROR_SUCCESS)
    return;
  ApplyDiskRate(g_diskReadCounter, true, reading);
  ApplyDiskRate(g_diskWriteCounter, false, reading);
}

struct NvmlApi
{
  struct MemoryInfo
  {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
  };
  using InitFn = int(WINAPI *)();
  using ShutdownFn = int(WINAPI *)();
  using CountFn = int(WINAPI *)(unsigned *);
  using HandleFn = int(WINAPI *)(unsigned, void **);
  using TempFn = int(WINAPI *)(void *, int, unsigned *);
  using NameFn = int(WINAPI *)(void *, char *, unsigned);
  using MemoryFn = int(WINAPI *)(void *, MemoryInfo *);

  HMODULE module = nullptr;
  InitFn init = nullptr;
  ShutdownFn shutdown = nullptr;
  CountFn count = nullptr;
  HandleFn handle = nullptr;
  TempFn temperature = nullptr;
  NameFn name = nullptr;
  MemoryFn memory = nullptr;
  bool ready = false;

  bool Initialize()
  {
    module = LoadLibraryW(L"nvml.dll");
    if (!module)
      module = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!module)
      return false;
    init = reinterpret_cast<InitFn>((void *)GetProcAddress(module, "nvmlInit_v2"));
    if (!init)
      init = reinterpret_cast<InitFn>((void *)GetProcAddress(module, "nvmlInit"));
    shutdown = reinterpret_cast<ShutdownFn>((void *)GetProcAddress(module, "nvmlShutdown"));
    count = reinterpret_cast<CountFn>((void *)GetProcAddress(module, "nvmlDeviceGetCount_v2"));
    if (!count)
      count = reinterpret_cast<CountFn>((void *)GetProcAddress(module, "nvmlDeviceGetCount"));
    handle = reinterpret_cast<HandleFn>((void *)GetProcAddress(module, "nvmlDeviceGetHandleByIndex_v2"));
    if (!handle)
      handle = reinterpret_cast<HandleFn>((void *)GetProcAddress(module, "nvmlDeviceGetHandleByIndex"));
    temperature =
        reinterpret_cast<TempFn>((void *)GetProcAddress(module, "nvmlDeviceGetTemperature"));
    name = reinterpret_cast<NameFn>((void *)GetProcAddress(module, "nvmlDeviceGetName"));
    memory = reinterpret_cast<MemoryFn>((void *)GetProcAddress(module, "nvmlDeviceGetMemoryInfo"));
    ready = init && shutdown && count && handle && temperature && init() == 0;
    return ready;
  }

  void Read(Reading *reading)
  {
    if (!ready)
      return;
    unsigned devices = 0;
    if (count(&devices) != 0)
      return;
    for (unsigned i = 0; i < devices && i < kMaxGpus; ++i)
    {
      void *device = nullptr;
      if (handle(i, &device) != 0)
        continue;
      char deviceName[128] = {};
      if (name)
        name(device, deviceName, sizeof(deviceName));
      int gpuIndex = -1;
      for (int gpu = 0; gpu < reading->gpuCount; ++gpu)
        if (ContainsNoCase(reading->gpus[gpu].name, "nvidia") &&
            !reading->gpus[gpu].hasTemperature)
        {
          gpuIndex = gpu;
          break;
        }
      if (gpuIndex < 0 && reading->gpuCount < kMaxGpus)
      {
        gpuIndex = reading->gpuCount++;
        GpuReading &gpu = reading->gpus[gpuIndex];
        CopyText(gpu.name, sizeof(gpu.name),
                 deviceName[0] ? deviceName : "NVIDIA GPU");
        CopyText(gpu.shortName, sizeof(gpu.shortName), "NVIDIA");
      }
      unsigned temperatureC = 0;
      if (gpuIndex >= 0 && temperature(device, 0, &temperatureC) == 0)
      {
        reading->gpus[gpuIndex].temperature = temperatureC;
        reading->gpus[gpuIndex].hasTemperature = true;
        reading->gpuTemp = std::max(reading->gpuTemp, (double)temperatureC);
        reading->hasGpuTemp = true;
      }
      MemoryInfo memoryInfo = {};
      if (gpuIndex >= 0 && memory && memory(device, &memoryInfo) == 0 && memoryInfo.total)
      {
        reading->gpus[gpuIndex].memoryUsageBytes = memoryInfo.used;
        reading->gpus[gpuIndex].memoryBudgetBytes = memoryInfo.total;
      }
    }
    reading->hasGpu = reading->gpuCount > 0;
  }

  void Shutdown()
  {
    if (ready && shutdown)
      shutdown();
    ready = false;
    if (module)
      FreeLibrary(module);
    module = nullptr;
  }
};

NvmlApi g_nvml;

void VtToString(const VARIANT &value, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  if (value.vt == VT_BSTR && value.bstrVal)
    WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, out, outSize, nullptr, nullptr);
  else if (value.vt == VT_R8)
    _snprintf(out, outSize, "%.3f", value.dblVal);
  else if (value.vt == VT_I4)
    _snprintf(out, outSize, "%ld", (long)value.lVal);
}

bool SensorMatches(const char *name, const char *identifier, const char *needle)
{
  return (name && std::strstr(name, needle)) ||
         (identifier && std::strstr(identifier, needle));
}

struct WmiTemperatures
{
  IWbemServices *services = nullptr;
  bool connectTried = false;
  bool namespaceReady = false;
  unsigned long long nextPoll = 0;
  double cpu = 0.0;
  double memory = 0.0;
  double gpu = 0.0;
  double storage = 0.0;
  bool hasCpu = false;
  bool hasMemory = false;
  bool hasGpu = false;
  bool hasStorage = false;
  bool logged = false;

  void Release()
  {
    if (services)
      services->Release();
    services = nullptr;
  }

  bool Connect(const wchar_t *namespaceName)
  {
    IWbemLocator *locator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (void **)&locator);
    if (FAILED(hr) || !locator)
      return false;
    BSTR ns = SysAllocString(namespaceName);
    hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(ns);
    locator->Release();
    if (FAILED(hr) || !services)
    {
      services = nullptr;
      return false;
    }
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
  }

  void PollLhm()
  {
    IEnumWbemClassObject *enumerator = nullptr;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT Name, Identifier, SensorType, Value FROM Sensor");
    HRESULT hr = services->ExecQuery(
        language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &enumerator);
    SysFreeString(query);
    SysFreeString(language);
    if (FAILED(hr) || !enumerator)
      return;
    double cpu = 0.0, memory = 0.0, gpu = 0.0, storage = 0.0;
    bool hasCpu = false, hasMemory = false, hasGpu = false, hasStorage = false;
    for (;;)
    {
      IWbemClassObject *object = nullptr;
      ULONG returned = 0;
      hr = enumerator->Next(WBEM_INFINITE, 1, &object, &returned);
      if (FAILED(hr) || !returned || !object)
        break;
      VARIANT type = {}, name = {}, identifier = {}, value = {};
      VariantInit(&type);
      VariantInit(&name);
      VariantInit(&identifier);
      VariantInit(&value);
      object->Get(L"SensorType", 0, &type, nullptr, nullptr);
      object->Get(L"Name", 0, &name, nullptr, nullptr);
      object->Get(L"Identifier", 0, &identifier, nullptr, nullptr);
      object->Get(L"Value", 0, &value, nullptr, nullptr);
      if (type.vt == VT_BSTR && type.bstrVal && !_wcsicmp(type.bstrVal, L"Temperature"))
      {
        char nameText[128] = {}, idText[256] = {};
        VtToString(name, nameText, sizeof(nameText));
        VtToString(identifier, idText, sizeof(idText));
        double celsius = value.vt == VT_R8 ? value.dblVal : -1000.0;
        if (celsius > -100.0 && celsius < 200.0)
        {
          if (SensorMatches(nameText, idText, "gpu"))
          {
            gpu = std::max(gpu, celsius);
            hasGpu = true;
          }
          else if (SensorMatches(nameText, idText, "nvme") ||
                   SensorMatches(nameText, idText, "ssd") ||
                   SensorMatches(nameText, idText, "hdd") ||
                   SensorMatches(nameText, idText, "storage"))
          {
            storage = std::max(storage, celsius);
            hasStorage = true;
          }
          else if (SensorMatches(nameText, idText, "memory") ||
                   SensorMatches(nameText, idText, "dram"))
          {
            memory = std::max(memory, celsius);
            hasMemory = true;
          }
          else if (SensorMatches(nameText, idText, "cpu") ||
                   SensorMatches(nameText, idText, "core") ||
                   SensorMatches(nameText, idText, "package"))
          {
            cpu = std::max(cpu, celsius);
            hasCpu = true;
          }
        }
      }
      VariantClear(&type);
      VariantClear(&name);
      VariantClear(&identifier);
      VariantClear(&value);
      object->Release();
    }
    enumerator->Release();
    if (hasCpu || hasMemory || hasGpu || hasStorage)
    {
      if (hasCpu)
      {
        this->cpu = cpu;
        this->hasCpu = true;
      }
      if (hasGpu)
      {
        this->gpu = gpu;
        this->hasGpu = true;
      }
      if (hasMemory)
      {
        this->memory = memory;
        this->hasMemory = true;
      }
      if (hasStorage)
      {
        this->storage = storage;
        this->hasStorage = true;
      }
    }
  }

  void PollAcpi()
  {
    IEnumWbemClassObject *enumerator = nullptr;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    HRESULT hr = services->ExecQuery(
        language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &enumerator);
    SysFreeString(query);
    SysFreeString(language);
    if (FAILED(hr) || !enumerator)
      return;
    double celsius = 0.0;
    bool found = false;
    for (;;)
    {
      IWbemClassObject *object = nullptr;
      ULONG returned = 0;
      hr = enumerator->Next(WBEM_INFINITE, 1, &object, &returned);
      if (FAILED(hr) || !returned || !object)
        break;
      VARIANT value = {};
      VariantInit(&value);
      object->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr);
      if (value.vt == VT_I4)
      {
        const double c = (double)value.lVal / 10.0 - 273.15;
        if (c > -30.0 && c < 150.0)
        {
          celsius = std::max(celsius, c);
          found = true;
        }
      }
      VariantClear(&value);
      object->Release();
    }
    enumerator->Release();
    if (found)
    {
      cpu = celsius;
      hasCpu = true;
    }
  }

  void Update(unsigned long long now)
  {
    if (now < nextPoll)
      return;
    nextPoll = now + 5000;
    if (!connectTried)
    {
      connectTried = true;
      IWbemServices *lhm = nullptr;
      IWbemServices *acpi = nullptr;
      WmiTemperatures probe;
      if (probe.Connect(L"ROOT\\LibreHardwareMonitor"))
      {
        lhm = probe.services;
        probe.services = nullptr;
      }
      probe.Release();
      if (probe.Connect(L"ROOT\\OpenHardwareMonitor"))
      {
        if (lhm)
          probe.services->Release();
        else
          lhm = probe.services;
        probe.services = nullptr;
      }
      probe.Release();
      if (probe.Connect(L"ROOT\\WMI"))
      {
        acpi = probe.services;
        probe.services = nullptr;
      }
      probe.Release();
      if (lhm)
      {
        services = lhm;
        namespaceReady = true;
        PollLhm();
        Release();
      }
      if (acpi && !hasCpu)
      {
        services = acpi;
        namespaceReady = true;
        PollAcpi();
        Release();
      }
      else if (acpi)
        acpi->Release();
      if (!logged)
      {
        WriteLog("temperature sources: cpu=%s gpu=%s storage=%s",
                 hasCpu ? "yes" : "unavailable", hasGpu ? "yes" : "unavailable",
                 hasStorage ? "yes" : "unavailable");
        logged = true;
      }
    }
  }
};

WmiTemperatures g_wmi;

struct CombaseApi
{
  using RoInitializeFn = HRESULT(WINAPI *)(RO_INIT_TYPE);
  using RoUninitializeFn = void(WINAPI *)();
  using RoGetActivationFactoryFn = HRESULT(WINAPI *)(HSTRING, REFIID, void **);
  using CreateStringFn = HRESULT(WINAPI *)(LPCWSTR, UINT32, HSTRING *);
  using DeleteStringFn = HRESULT(WINAPI *)(HSTRING);
  using RawStringFn = PCWSTR(WINAPI *)(HSTRING, UINT32 *);

  HMODULE module = nullptr;
  RoInitializeFn roInitialize = nullptr;
  RoUninitializeFn roUninitialize = nullptr;
  RoGetActivationFactoryFn getFactory = nullptr;
  CreateStringFn createString = nullptr;
  DeleteStringFn deleteString = nullptr;
  RawStringFn rawString = nullptr;

  bool Load()
  {
    module = GetModuleHandleW(L"combase.dll");
    if (!module)
      module = LoadLibraryW(L"combase.dll");
    if (!module)
      return false;
    roInitialize = reinterpret_cast<RoInitializeFn>((void *)GetProcAddress(module, "RoInitialize"));
    roUninitialize =
        reinterpret_cast<RoUninitializeFn>((void *)GetProcAddress(module, "RoUninitialize"));
    getFactory = reinterpret_cast<RoGetActivationFactoryFn>(
        (void *)GetProcAddress(module, "RoGetActivationFactory"));
    createString = reinterpret_cast<CreateStringFn>(
        (void *)GetProcAddress(module, "WindowsCreateString"));
    deleteString = reinterpret_cast<DeleteStringFn>(
        (void *)GetProcAddress(module, "WindowsDeleteString"));
    rawString = reinterpret_cast<RawStringFn>(
        (void *)GetProcAddress(module, "WindowsGetStringRawBuffer"));
    return roInitialize && roUninitialize && getFactory && createString && deleteString && rawString;
  }
};

struct LunarReader
{
  // The MinGW copy of windows.globalization.h currently produces duplicate template specialisations
  // when combined with winsock2.h. Only four ICalendar methods are needed, so call them by the vtable
  // positions published in windows.globalization.idl instead of pulling in the whole generated header.
  static constexpr int kChangeCalendarSystemSlot = 13;
  static constexpr int kSetDateTimeSlot = 17;
  static constexpr int kGetMonthSlot = 39;
  static constexpr int kMonthAsFullStringSlot = 42;
  static constexpr int kGetDaySlot = 52;
  static constexpr int kDayAsStringSlot = 55;

  CombaseApi api;
  IActivationFactory *factory = nullptr;
  IInspectable *calendar = nullptr;
  bool initializedCom = false;
  bool ready = false;

  template <typename Fn>
  Fn CalendarMethod(int slot)
  {
    void **vtable = *reinterpret_cast<void ***>(calendar);
    return reinterpret_cast<Fn>(vtable[slot]);
  }

  bool Initialize()
  {
    if (!api.Load())
    {
      WriteLog("WinRT calendar: combase exports unavailable");
      return false;
    }
    const HRESULT init = api.roInitialize(RO_INIT_MULTITHREADED);
    initializedCom = SUCCEEDED(init);
    HSTRING className = nullptr;
    HSTRING calendarName = nullptr;
    if (FAILED(api.createString(L"Windows.Globalization.Calendar",
                                (UINT32)std::wcslen(L"Windows.Globalization.Calendar"),
                                &className)))
    {
      WriteLog("WinRT calendar: class name could not be created");
      return false;
    }
    static const IID kActivationFactoryIid = {
        0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    HRESULT hr = api.getFactory(className, kActivationFactoryIid, (void **)&factory);
    api.deleteString(className);
    if (FAILED(hr) || !factory)
    {
      WriteLog("WinRT calendar: activation factory failed (0x%08lx)", (unsigned long)hr);
      return false;
    }
    IInspectable *instance = nullptr;
    hr = factory->ActivateInstance(&instance);
    if (FAILED(hr) || !instance)
    {
      WriteLog("WinRT calendar: ActivateInstance failed (0x%08lx)", (unsigned long)hr);
      return false;
    }
    static const IID kCalendarIid = {
        0xca30221d, 0x86d9, 0x40fb, {0xa2, 0x6b, 0xd4, 0x4e, 0xb7, 0xcf, 0x08, 0xea}};
    hr = instance->QueryInterface(kCalendarIid, (void **)&calendar);
    instance->Release();
    if (FAILED(hr) || !calendar)
    {
      WriteLog("WinRT calendar: ICalendar is unavailable (0x%08lx)", (unsigned long)hr);
      return false;
    }
    // Ask the OS for CalendarIdentifiers.ChineseLunar rather than spelling the identifier
    // ourselves. It is the API's own token and can change spelling without a public constant.
    IInspectable *identifierStatics = nullptr;
    static const IID kCalendarIdentifiersStatics3Iid = {
        0x2c225423, 0x1fad, 0x40c0, {0x93, 0x34, 0xa8, 0xeb, 0x90, 0xdb, 0x04, 0xf5}};
    HSTRING identifiersClass = nullptr;
    if (SUCCEEDED(api.createString(
            L"Windows.Globalization.CalendarIdentifiers",
            (UINT32)std::wcslen(L"Windows.Globalization.CalendarIdentifiers"),
            &identifiersClass)))
    {
      api.getFactory(identifiersClass, kCalendarIdentifiersStatics3Iid,
                     (void **)&identifierStatics);
      api.deleteString(identifiersClass);
    }
    if (!identifierStatics)
    {
      WriteLog("WinRT calendar: CalendarIdentifiers is unavailable");
      return false;
    }
    using GetIdentifierFn = HRESULT(STDMETHODCALLTYPE *)(IInspectable *, HSTRING *);
    void **identifierVtable = *reinterpret_cast<void ***>(identifierStatics);
    const auto getChineseLunar =
        reinterpret_cast<GetIdentifierFn>(identifierVtable[6]);
    hr = getChineseLunar(identifierStatics, &calendarName);
    identifierStatics->Release();
    if (FAILED(hr) || !calendarName)
    {
      WriteLog("WinRT calendar: ChineseLunar identifier failed (0x%08lx)",
               (unsigned long)hr);
      return false;
    }
    using ChangeCalendarSystemFn = HRESULT(STDMETHODCALLTYPE *)(IInspectable *, HSTRING);
    const auto change = CalendarMethod<ChangeCalendarSystemFn>(kChangeCalendarSystemSlot);
    hr = change(calendar, calendarName);
    api.deleteString(calendarName);
    ready = SUCCEEDED(hr);
    if (!ready)
      WriteLog("WinRT ChineseLunar calendar is unavailable (0x%08lx)", (unsigned long)hr);
    return ready;
  }

  std::string HStringText(HSTRING value)
  {
    if (!value)
      return {};
    UINT32 length = 0;
    const wchar_t *raw = api.rawString(value, &length);
    if (!raw)
      return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, raw, (int)length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
      return {};
    std::string result((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, raw, (int)length, &result[0], bytes, nullptr, nullptr);
    return result;
  }

  bool Format(const SYSTEMTIME &local, char *zhOut, int zhSize, char *enOut, int enSize,
              char *yearOut, int yearSize)
  {
    if (!ready || !calendar || !zhOut || zhSize <= 0 || !enOut || enSize <= 0)
      return false;
    SYSTEMTIME utc = {};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc))
      utc = local;
    FILETIME fileTime = {};
    if (!SystemTimeToFileTime(&utc, &fileTime))
      return false;
    ULARGE_INTEGER ticks;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    using SetDateTimeFn = HRESULT(STDMETHODCALLTYPE *)(IInspectable *, INT64);
    const auto setDateTime = CalendarMethod<SetDateTimeFn>(kSetDateTimeSlot);
    if (FAILED(setDateTime(calendar, (INT64)ticks.QuadPart)))
      return false;
    HSTRING month = nullptr;
    HSTRING day = nullptr;
    using GetStringFn = HRESULT(STDMETHODCALLTYPE *)(IInspectable *, HSTRING *);
    const auto monthAsString = CalendarMethod<GetStringFn>(kMonthAsFullStringSlot);
    const auto dayAsString = CalendarMethod<GetStringFn>(kDayAsStringSlot);
    if (FAILED(monthAsString(calendar, &month)) ||
        FAILED(dayAsString(calendar, &day)))
    {
      if (month)
        api.deleteString(month);
      if (day)
        api.deleteString(day);
      return false;
    }
    const std::string monthText = HStringText(month);
    const std::string dayText = HStringText(day);
    api.deleteString(month);
    api.deleteString(day);
    if (monthText.empty() || dayText.empty())
      return false;
    using GetIntFn = HRESULT(STDMETHODCALLTYPE *)(IInspectable *, INT32 *);
    INT32 monthNumber = 0;
    INT32 dayNumber = 0;
    const auto getMonth = CalendarMethod<GetIntFn>(kGetMonthSlot);
    const auto getDay = CalendarMethod<GetIntFn>(kGetDaySlot);
    if (FAILED(getMonth(calendar, &monthNumber)) ||
        FAILED(getDay(calendar, &dayNumber)))
      return false;
    const bool leap =
        monthText.find("闰") != std::string::npos ||
        monthText.find("閏") != std::string::npos ||
        monthText.find("Leap") != std::string::npos ||
        monthText.find("leap") != std::string::npos;
    char dayZh[32] = {};
    LunarDayZh(dayNumber, dayZh, sizeof(dayZh));
    _snprintf(zhOut, zhSize, "%s%s%s", leap ? "闰" : "", LunarMonthZh(monthNumber),
              dayZh);
    _snprintf(enOut, enSize, "%s%d/%d", leap ? "leap " : "", (int)monthNumber,
              (int)dayNumber);
    char sexagenary[64] = {};
    SexagenaryYear(local.wYear, sexagenary, sizeof(sexagenary));
    if (yearOut && yearSize > 0)
      _snprintf(yearOut, yearSize, "%s", sexagenary);
    return true;
  }

  void Shutdown()
  {
    if (calendar)
      calendar->Release();
    calendar = nullptr;
    if (factory)
      factory->Release();
    factory = nullptr;
    if (initializedCom && api.roUninitialize)
      api.roUninitialize();
    initializedCom = false;
    ready = false;
  }
};

LunarReader g_lunar;

void StaticReading(Reading *reading)
{
  reading->download = 1.5 * 1024.0 * 1024.0;
  reading->upload = 148.0 * 1024.0;
  reading->cpuPercent = 42.0;
  reading->cpuMhz = 3200.0;
  reading->memoryPercent = 57.0;
  reading->memoryUsedBytes = 9ULL * 1024ULL * 1024ULL * 1024ULL;
  reading->storagePercent = 38.0;
  reading->gpuPercent = 13.0;
  reading->cpuTemp = 65.0;
  reading->memoryTemp = 40.0;
  reading->gpuTemp = 54.0;
  reading->hasCpuTemp = true;
  reading->hasMemoryTemp = true;
  reading->hasGpuTemp = true;
  reading->hasGpu = true;
  reading->hasIp = true;
  reading->hasLunar = true;
  CopyText(reading->primaryVolume, 8, "C");
  CopyText(reading->lunarZh, sizeof(reading->lunarZh), "十月初二");
  CopyText(reading->lunarEn, sizeof(reading->lunarEn), "10/2");
  CopyText(reading->lunarYearZh, sizeof(reading->lunarYearZh), "丙午年");
  CopyText(reading->lunarYearEn, sizeof(reading->lunarYearEn), "丙午年");
  VolumeReading &v = reading->volumes[reading->volumeCount++];
  CopyText(v.label, sizeof(v.label), "C");
  v.total = 512ULL * 1024ULL * 1024ULL * 1024ULL;
  v.free = 317ULL * 1024ULL * 1024ULL * 1024ULL;
  v.readBytesPerSecond = 12.0 * 1024.0 * 1024.0;
  v.writeBytesPerSecond = 2.5 * 1024.0 * 1024.0;
  v.temperature = 44.0;
  v.hasTemperature = true;
  AddressReading &a = reading->addresses[reading->addressCount++];
  CopyText(a.address, sizeof(a.address), "192.0.2.10");
  CopyText(a.adapter, sizeof(a.adapter), "Ethernet");
  a.family = AF_INET;
  {
    GpuReading &gpu = reading->gpus[reading->gpuCount++];
    CopyText(gpu.name, sizeof(gpu.name), "Intel Iris Xe Graphics");
    CopyText(gpu.shortName, sizeof(gpu.shortName), "Intel");
    gpu.luidLow = 1;
    gpu.percent = 4.0;
    gpu.localUsageBytes = 512ULL * 1024ULL * 1024ULL;
    gpu.localBudgetBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    gpu.memoryUsageBytes = gpu.localUsageBytes;
    gpu.memoryBudgetBytes = gpu.localBudgetBytes;
  }
  {
    GpuReading &gpu = reading->gpus[reading->gpuCount++];
    CopyText(gpu.name, sizeof(gpu.name), "NVIDIA GeForce RTX 4060 Laptop GPU");
    CopyText(gpu.shortName, sizeof(gpu.shortName), "NVIDIA");
    gpu.luidLow = 2;
    gpu.percent = 13.0;
    gpu.localUsageBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    gpu.localBudgetBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    gpu.memoryUsageBytes = gpu.localUsageBytes;
    gpu.memoryBudgetBytes = gpu.localBudgetBytes;
    gpu.temperature = 54.0;
    gpu.hasTemperature = true;
  }
}

void CollectReading(Reading *reading)
{
  const char *staticTest = std::getenv("APEX_SYSTEMSTATUS_STATIC");
  if (staticTest && !std::strcmp(staticTest, "1"))
  {
    StaticReading(reading);
    return;
  }
  const unsigned long long now = GetTickCount64();
  reading->cpuPercent = CpuUsage();
  reading->cpuMhz = CpuMhz();
  reading->memoryPercent = MemoryUsage(&reading->memoryUsedBytes);
  CollectNetwork(reading, now);
  CollectVolumes(reading);
  static bool diskInitialized = false;
  if (!diskInitialized)
  {
    InitializeDiskCounters();
    diskInitialized = true;
  }
  CollectDiskSpeeds(reading);
  CollectAddresses(reading);
  static bool gpuInitialized = false;
  if (!gpuInitialized)
  {
    InitializeGpuCounter();
    g_nvml.Initialize();
    gpuInitialized = true;
  }
  DiscoverGpuAdapters(reading);
  CollectGpuUsage(reading);
  if (!reading->gpuCount)
    reading->gpuPercent = AggregateGpuUsage();
  g_nvml.Read(reading);
  reading->hasGpu = reading->hasGpu || g_gpuCounterReady;
  reading->hasGpu = reading->hasGpu || reading->gpuPercent > 0.0;
  g_wmi.Update(now);
  if (g_wmi.hasCpu)
  {
    reading->cpuTemp = g_wmi.cpu;
    reading->hasCpuTemp = true;
  }
  if (g_wmi.hasMemory)
  {
    reading->memoryTemp = g_wmi.memory;
    reading->hasMemoryTemp = true;
  }
  if (g_wmi.hasGpu && !reading->hasGpuTemp)
  {
    reading->gpuTemp = g_wmi.gpu;
    reading->hasGpuTemp = true;
    for (int gpu = 0; gpu < reading->gpuCount; ++gpu)
      if (!reading->gpus[gpu].hasTemperature)
      {
        reading->gpus[gpu].temperature = g_wmi.gpu;
        reading->gpus[gpu].hasTemperature = true;
        break;
      }
  }
  if (g_wmi.hasStorage)
  {
    reading->storageTemp = g_wmi.storage;
    reading->hasStorageTemp = true;
    if (reading->volumeCount > 0)
    {
      reading->volumes[0].temperature = g_wmi.storage;
      reading->volumes[0].hasTemperature = true;
    }
  }
  SYSTEMTIME local;
  GetLocalTime(&local);
  reading->hasLunar =
      g_lunar.Format(local, reading->lunarZh, sizeof(reading->lunarZh),
                     reading->lunarEn, sizeof(reading->lunarEn),
                     reading->lunarYearZh, sizeof(reading->lunarYearZh));
  CopyText(reading->lunarYearEn, sizeof(reading->lunarYearEn), reading->lunarYearZh);
}

int AddMetric(Snapshot &snapshot, const char *id, const char *nameZh, const char *nameEn)
{
  if (!id || !*id || snapshot.metricCount >= kMaxMetrics)
    return -1;
  const int index = snapshot.metricCount++;
  MetricSnapshot &metric = snapshot.metrics[index];
  CopyText(metric.id, sizeof(metric.id), id);
  CopyText(metric.nameZh, sizeof(metric.nameZh), nameZh);
  CopyText(metric.nameEn, sizeof(metric.nameEn), nameEn);
  return index;
}

unsigned long AddressHash(const char *text)
{
  unsigned long hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p)
  {
    hash ^= *p;
    hash *= 16777619u;
  }
  return hash;
}

void BuildSnapshot(const Reading &reading, const Settings &settings)
{
  Snapshot next;
  next.revision = ++g_snapshotRevision;
  SYSTEMTIME local;
  GetLocalTime(&local);
  char down[64] = {}, up[64] = {}, percent[64] = {}, temp[64] = {};
  FormatRate(reading.download, down, sizeof(down));
  FormatRate(reading.upload, up, sizeof(up));
  int metric = AddMetric(next, "net_down", "下载速度", "Download");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    _snprintf(m.statusZh, sizeof(m.statusZh), "下载速度 · ↓ %s", down);
    _snprintf(m.statusEn, sizeof(m.statusEn), "Download · %s", down);
    _snprintf(m.shortText, sizeof(m.shortText), "↓\xE2\x80\x89%s", down);
    _snprintf(m.shortEn, sizeof(m.shortEn), "D %s", down);
  }
  metric = AddMetric(next, "net_up", "上传速度", "Upload");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    _snprintf(m.statusZh, sizeof(m.statusZh), "上传速度 · ↑ %s", up);
    _snprintf(m.statusEn, sizeof(m.statusEn), "Upload · %s", up);
    _snprintf(m.shortText, sizeof(m.shortText), "↑\xE2\x80\x89%s", up);
    _snprintf(m.shortEn, sizeof(m.shortEn), "U %s", up);
  }
  metric = AddMetric(next, "cpu_usage", "CPU 占用", "CPU usage");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    m.hasUnitChoice = true;
    CopyText(m.unit, sizeof(m.unit), MetricUnit(settings, m.id));
    if (!std::strcmp(m.unit, "ghz"))
    {
      char ghz[32] = "--";
      if (reading.cpuMhz > 0.0)
        _snprintf(ghz, sizeof(ghz), "%.2f GHz", reading.cpuMhz / 1000.0);
      _snprintf(m.statusZh, sizeof(m.statusZh), "CPU 频率 · %s", ghz);
      _snprintf(m.statusEn, sizeof(m.statusEn), "CPU frequency · %s", ghz);
      _snprintf(m.shortText, sizeof(m.shortText), "CPU %s", ghz);
      _snprintf(m.shortEn, sizeof(m.shortEn), "CPU %s", ghz);
    }
    else
    {
      FormatPercent(reading.cpuPercent, percent, sizeof(percent));
      _snprintf(m.statusZh, sizeof(m.statusZh), "CPU 占用 · %s", percent);
      _snprintf(m.statusEn, sizeof(m.statusEn), "CPU usage · %s", percent);
      _snprintf(m.shortText, sizeof(m.shortText), "CPU %s", percent);
      _snprintf(m.shortEn, sizeof(m.shortEn), "CPU %s", percent);
    }
  }
  metric = AddMetric(next, "cpu_temp", "CPU 温度", "CPU temperature");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    FormatTemperature(reading.hasCpuTemp, reading.cpuTemp, temp, sizeof(temp));
    _snprintf(m.statusZh, sizeof(m.statusZh), "CPU 温度 · %s", temp);
    _snprintf(m.statusEn, sizeof(m.statusEn), "CPU temperature · %s", temp);
    _snprintf(m.shortText, sizeof(m.shortText), "CPU %s", temp);
    _snprintf(m.shortEn, sizeof(m.shortEn), "CPU %s", temp);
    if (!reading.hasCpuTemp)
      m.level = "warn";
  }
  metric = AddMetric(next, "ram_usage", "内存占用", "Memory usage");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    m.hasUnitChoice = true;
    CopyText(m.unit, sizeof(m.unit), MetricUnit(settings, m.id));
    if (!std::strcmp(m.unit, "gb"))
    {
      char used[64] = {};
      FormatBytes(reading.memoryUsedBytes, used, sizeof(used));
      _snprintf(m.statusZh, sizeof(m.statusZh), "内存占用 · %s", used);
      _snprintf(m.statusEn, sizeof(m.statusEn), "Memory usage · %s", used);
      _snprintf(m.shortText, sizeof(m.shortText), "RAM %s", used);
      _snprintf(m.shortEn, sizeof(m.shortEn), "RAM %s", used);
    }
    else
    {
      FormatPercent(reading.memoryPercent, percent, sizeof(percent));
      _snprintf(m.statusZh, sizeof(m.statusZh), "内存占用 · %s", percent);
      _snprintf(m.statusEn, sizeof(m.statusEn), "Memory usage · %s", percent);
      _snprintf(m.shortText, sizeof(m.shortText), "RAM %s", percent);
      _snprintf(m.shortEn, sizeof(m.shortEn), "RAM %s", percent);
    }
  }
  metric = AddMetric(next, "ram_temp", "内存温度", "Memory temperature");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    FormatTemperature(reading.hasMemoryTemp, reading.memoryTemp, temp, sizeof(temp));
    _snprintf(m.statusZh, sizeof(m.statusZh), "内存温度 · %s", temp);
    _snprintf(m.statusEn, sizeof(m.statusEn), "Memory temperature · %s", temp);
    _snprintf(m.shortText, sizeof(m.shortText), "RAM %s", temp);
    _snprintf(m.shortEn, sizeof(m.shortEn), "RAM %s", temp);
    if (!reading.hasMemoryTemp)
      m.level = "warn";
  }
  for (int gpuIndex = 0; gpuIndex < reading.gpuCount; ++gpuIndex)
  {
    const GpuReading &gpu = reading.gpus[gpuIndex];
    char id[kMaxMetricId] = {};
    char zhName[96] = {}, enName[96] = {};
    _snprintf(zhName, sizeof(zhName), "%s 占用",
              gpu.shortName[0] ? gpu.shortName : "GPU");
    _snprintf(enName, sizeof(enName), "%s usage",
              gpu.shortName[0] ? gpu.shortName : "GPU");
    _snprintf(id, sizeof(id), "gpu_%08lx_%08lx_usage", gpu.luidHigh, gpu.luidLow);
    metric = AddMetric(next, id, zhName, enName);
    if (metric >= 0)
    {
      MetricSnapshot &m = next.metrics[metric];
      m.hasUnitChoice = true;
      CopyText(m.unit, sizeof(m.unit), MetricUnit(settings, m.id));
      if (!std::strcmp(m.unit, "gb"))
      {
        char used[64] = "--", budget[64] = "--";
        if (gpu.memoryUsageBytes)
          FormatBytes(gpu.memoryUsageBytes, used, sizeof(used));
        if (gpu.memoryBudgetBytes)
          FormatBytes(gpu.memoryBudgetBytes, budget, sizeof(budget));
        _snprintf(m.statusZh, sizeof(m.statusZh), "%s 显存 · %s / %s",
                  gpu.name, used, budget);
        _snprintf(m.statusEn, sizeof(m.statusEn), "%s VRAM · %s / %s",
                  gpu.name, used, budget);
        _snprintf(m.shortText, sizeof(m.shortText), "%s %s",
                  gpu.shortName[0] ? gpu.shortName : "GPU", used);
        _snprintf(m.shortEn, sizeof(m.shortEn), "%s %s",
                  gpu.shortName[0] ? gpu.shortName : "GPU", used);
      }
      else
      {
        FormatPercent(gpu.percent, percent, sizeof(percent));
        _snprintf(m.statusZh, sizeof(m.statusZh), "%s 占用 · %s", gpu.name, percent);
        _snprintf(m.statusEn, sizeof(m.statusEn), "%s usage · %s", gpu.name, percent);
        _snprintf(m.shortText, sizeof(m.shortText), "%s %s",
                  gpu.shortName[0] ? gpu.shortName : "GPU", percent);
        _snprintf(m.shortEn, sizeof(m.shortEn), "%s %s",
                  gpu.shortName[0] ? gpu.shortName : "GPU", percent);
      }
    }
    _snprintf(zhName, sizeof(zhName), "%s 温度",
              gpu.shortName[0] ? gpu.shortName : "GPU");
    _snprintf(enName, sizeof(enName), "%s temperature",
              gpu.shortName[0] ? gpu.shortName : "GPU");
    _snprintf(id, sizeof(id), "gpu_%08lx_%08lx_temp", gpu.luidHigh, gpu.luidLow);
    metric = AddMetric(next, id, zhName, enName);
    if (metric >= 0)
    {
      MetricSnapshot &m = next.metrics[metric];
      FormatTemperature(gpu.hasTemperature, gpu.temperature, temp, sizeof(temp));
      _snprintf(m.statusZh, sizeof(m.statusZh), "%s 温度 · %s", gpu.name, temp);
      _snprintf(m.statusEn, sizeof(m.statusEn), "%s temperature · %s", gpu.name, temp);
      _snprintf(m.shortText, sizeof(m.shortText), "%s %s",
                gpu.shortName[0] ? gpu.shortName : "GPU", temp);
      _snprintf(m.shortEn, sizeof(m.shortEn), "%s %s",
                gpu.shortName[0] ? gpu.shortName : "GPU", temp);
      if (!gpu.hasTemperature)
        m.level = "warn";
    }
  }
  for (int volumeIndex = 0; volumeIndex < reading.volumeCount; ++volumeIndex)
  {
    const VolumeReading &volume = reading.volumes[volumeIndex];
    const double used =
        volume.total ? 100.0 * (double)(volume.total - volume.free) / (double)volume.total : 0.0;
    char id[kMaxMetricId] = {};
    char usedText[32] = {}, totalText[64] = {}, freeText[64] = {};
    FormatPercent(used, usedText, sizeof(usedText));
    FormatBytes(volume.total, totalText, sizeof(totalText));
    FormatBytes(volume.free, freeText, sizeof(freeText));
    char zhName[96] = {}, enName[96] = {};
    _snprintf(zhName, sizeof(zhName), "磁盘 %s 占用", volume.label);
    _snprintf(enName, sizeof(enName), "Disk %s usage", volume.label);
    _snprintf(id, sizeof(id), "disk_%s_usage", volume.label);
    metric = AddMetric(next, id, zhName, enName);
    if (metric >= 0)
    {
      MetricSnapshot &m = next.metrics[metric];
      _snprintf(m.statusZh, sizeof(m.statusZh), "%s: · 已用 %s · 剩余 %s / %s",
                volume.label, usedText, freeText, totalText);
      _snprintf(m.statusEn, sizeof(m.statusEn), "%s: · %s used · %s free of %s",
                volume.label, usedText, freeText, totalText);
      _snprintf(m.shortText, sizeof(m.shortText), "%s %s", volume.label, usedText);
      _snprintf(m.shortEn, sizeof(m.shortEn), "%s %s", volume.label, usedText);
    }
    char rate[64] = {};
    _snprintf(zhName, sizeof(zhName), "磁盘 %s 读取", volume.label);
    _snprintf(enName, sizeof(enName), "Disk %s read", volume.label);
    _snprintf(id, sizeof(id), "disk_%s_read", volume.label);
    metric = AddMetric(next, id, zhName, enName);
    if (metric >= 0)
    {
      MetricSnapshot &m = next.metrics[metric];
      FormatRate(volume.readBytesPerSecond, rate, sizeof(rate));
      _snprintf(m.statusZh, sizeof(m.statusZh), "磁盘 %s 读取 · %s", volume.label, rate);
      _snprintf(m.statusEn, sizeof(m.statusEn), "Disk %s read · %s", volume.label, rate);
      _snprintf(m.shortText, sizeof(m.shortText), "%s ↓ %s", volume.label, rate);
      _snprintf(m.shortEn, sizeof(m.shortEn), "%s R %s", volume.label, rate);
    }
    _snprintf(zhName, sizeof(zhName), "磁盘 %s 写入", volume.label);
    _snprintf(enName, sizeof(enName), "Disk %s write", volume.label);
    _snprintf(id, sizeof(id), "disk_%s_write", volume.label);
    metric = AddMetric(next, id, zhName, enName);
    if (metric >= 0)
    {
      MetricSnapshot &m = next.metrics[metric];
      FormatRate(volume.writeBytesPerSecond, rate, sizeof(rate));
      _snprintf(m.statusZh, sizeof(m.statusZh), "磁盘 %s 写入 · %s", volume.label, rate);
      _snprintf(m.statusEn, sizeof(m.statusEn), "Disk %s write · %s", volume.label, rate);
      _snprintf(m.shortText, sizeof(m.shortText), "%s ↑ %s", volume.label, rate);
      _snprintf(m.shortEn, sizeof(m.shortEn), "%s W %s", volume.label, rate);
    }
    _snprintf(zhName, sizeof(zhName), "磁盘 %s 温度", volume.label);
    _snprintf(enName, sizeof(enName), "Disk %s temperature", volume.label);
    _snprintf(id, sizeof(id), "disk_%s_temp", volume.label);
    metric = AddMetric(next, id, zhName, enName);
    if (metric >= 0)
    {
      MetricSnapshot &m = next.metrics[metric];
      FormatTemperature(volume.hasTemperature, volume.temperature, temp, sizeof(temp));
      _snprintf(m.statusZh, sizeof(m.statusZh), "磁盘 %s 温度 · %s", volume.label, temp);
      _snprintf(m.statusEn, sizeof(m.statusEn), "Disk %s temperature · %s",
                volume.label, temp);
      _snprintf(m.shortText, sizeof(m.shortText), "%s %s", volume.label, temp);
      _snprintf(m.shortEn, sizeof(m.shortEn), "%s %s", volume.label, temp);
      if (!volume.hasTemperature)
        m.level = "warn";
    }
  }
  for (int i = 0; i < reading.addressCount; ++i)
  {
    const AddressReading &address = reading.addresses[i];
    char id[kMaxMetricId] = {};
    char zhName[96] = {}, enName[96] = {};
    _snprintf(id, sizeof(id), "ip_%lu_%d_%08lx", address.ifIndex, address.family,
              AddressHash(address.address));
    _snprintf(zhName, sizeof(zhName), "本机 IP · %s", address.adapter);
    _snprintf(enName, sizeof(enName), "Local IP · %s", address.adapter);
    metric = AddMetric(next, id, zhName, enName);
    if (metric >= 0)
    {
      MetricSnapshot &m = next.metrics[metric];
      _snprintf(m.statusZh, sizeof(m.statusZh), "%s · %s · %s",
                AddressFamilyName(address.family), address.address, address.adapter);
      _snprintf(m.statusEn, sizeof(m.statusEn), "%s · %s · %s",
                AddressFamilyName(address.family), address.address, address.adapter);
      _snprintf(m.shortText, sizeof(m.shortText), "IP %s", address.address);
      _snprintf(m.shortEn, sizeof(m.shortEn), "IP %s", address.address);
    }
  }
  metric = AddMetric(next, "clock", "时间", "Time");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    _snprintf(m.statusZh, sizeof(m.statusZh), "时间 · %02u:%02u:%02u",
              local.wHour, local.wMinute, local.wSecond);
    _snprintf(m.statusEn, sizeof(m.statusEn), "Time · %02u:%02u:%02u",
              local.wHour, local.wMinute, local.wSecond);
    _snprintf(m.shortText, sizeof(m.shortText), "%02u:%02u:%02u",
              local.wHour, local.wMinute, local.wSecond);
    _snprintf(m.shortEn, sizeof(m.shortEn), "%02u:%02u:%02u",
              local.wHour, local.wMinute, local.wSecond);
  }
  metric = AddMetric(next, "date_calendar", "日期", "Date");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    _snprintf(m.statusZh, sizeof(m.statusZh), "日期 · %04u-%02u-%02u %s",
              local.wYear, local.wMonth, local.wDay, WeekdayZh(local.wDayOfWeek));
    _snprintf(m.statusEn, sizeof(m.statusEn), "Date · %04u-%02u-%02u %s",
              local.wYear, local.wMonth, local.wDay, WeekdayEn(local.wDayOfWeek));
    _snprintf(m.shortText, sizeof(m.shortText), "%04u-%02u-%02u",
              local.wYear, local.wMonth, local.wDay);
    _snprintf(m.shortEn, sizeof(m.shortEn), "%04u-%02u-%02u",
              local.wYear, local.wMonth, local.wDay);
  }
  metric = AddMetric(next, "lunar_date", "农历", "Lunar date");
  if (metric >= 0)
  {
    MetricSnapshot &m = next.metrics[metric];
    if (reading.hasLunar)
    {
      _snprintf(m.statusZh, sizeof(m.statusZh), "%s", reading.lunarZh);
      _snprintf(m.statusEn, sizeof(m.statusEn), "%s", reading.lunarEn);
      _snprintf(m.shortText, sizeof(m.shortText), "%s", reading.lunarZh);
      _snprintf(m.shortEn, sizeof(m.shortEn), "%s", reading.lunarEn);
    }
    else
    {
      m.level = "warn";
      _snprintf(m.statusZh, sizeof(m.statusZh), "系统不支持");
      _snprintf(m.statusEn, sizeof(m.statusEn), "unavailable");
      _snprintf(m.shortText, sizeof(m.shortText), "--");
      _snprintf(m.shortEn, sizeof(m.shortEn), "--");
    }
  }
  EnterCriticalSection(&g_lock);
  g_snapshot = next;
  LeaveCriticalSection(&g_lock);
}

DWORD WINAPI SampleThread(LPVOID)
{
  const bool staticTest = [] {
    const char *v = std::getenv("APEX_SYSTEMSTATUS_STATIC");
    return v && !std::strcmp(v, "1");
  }();
  if (!staticTest)
  {
    g_lunar.Initialize();
    InitializeGpuCounter();
    g_nvml.Initialize();
  }
  while (!InterlockedCompareExchange(&g_stop, 0, 0))
  {
    const bool enabled = !g_host || !g_host->featureEnabled ||
                         g_host->featureEnabled(kId) != 0;
    if (enabled)
    {
      Reading reading;
      CollectReading(&reading);
      Settings settings;
      EnterCriticalSection(&g_lock);
      settings = g_settings;
      LeaveCriticalSection(&g_lock);
      BuildSnapshot(reading, settings);
    }
    if (!g_sampleWake)
      break;
    WaitForSingleObject(g_sampleWake, enabled ? 1000 : 2000);
  }
  if (!staticTest)
  {
    g_nvml.Shutdown();
    g_wmi.Release();
    g_lunar.Shutdown();
    if (g_gpuQuery)
      PdhCloseQuery(g_gpuQuery);
    if (g_diskQuery)
      PdhCloseQuery(g_diskQuery);
    g_gpuQuery = nullptr;
    g_gpuCounter = nullptr;
    g_gpuCounterReady = false;
    g_diskQuery = nullptr;
    g_diskReadCounter = nullptr;
    g_diskWriteCounter = nullptr;
    g_diskCountersReady = false;
  }
  return 0;
}

int SettingsJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  Settings settings;
  Snapshot snapshot;
  EnterCriticalSection(&g_lock);
  settings = g_settings;
  snapshot = g_snapshot;
  LeaveCriticalSection(&g_lock);
  int off = 0;
  AppendText(out, outSize, off,
             "{\"settingsFile\":\"SystemStatus.ini\","
             "\"summaryZh\":\"网速、硬件、IP 与日历状态，可逐项映射到任务栏\","
             "\"summaryEn\":\"Network, hardware, IP and calendar status, mapped to the taskbar item by item\","
             "\"params\":[");
  for (int i = 0; i < snapshot.metricCount; ++i)
  {
    const MetricSnapshot &metric = snapshot.metrics[i];
    if (i)
      AppendText(out, outSize, off, ",");
    AppendText(out, outSize, off,
               "{\"id\":\"metric_%d\",\"type\":\"status\",\"level\":\"%s\",\"live\":true,"
               "\"textZh\":",
               i, metric.level ? metric.level : "warn");
    AppendJsonString(out, outSize, off,
                     metric.statusZh[0] ? metric.statusZh : metric.nameZh);
    AppendText(out, outSize, off, ",\"textEn\":");
    AppendJsonString(out, outSize, off,
                     metric.statusEn[0] ? metric.statusEn : metric.nameEn);
    if (metric.hasUnitChoice)
    {
      AppendText(out, outSize, off, ",\"choiceId\":\"%s_unit\",\"choiceValue\":", metric.id);
      AppendJsonString(out, outSize, off, metric.unit);
      AppendText(out, outSize, off,
                 ",\"choiceSwitch\":true,\"choiceOffValue\":\"percent\",\"choiceOnValue\":\"");
      if (!std::strncmp(metric.id, "gpu_", 4))
        AppendText(out, outSize, off, "gb");
      else if (!std::strcmp(metric.id, "cpu_usage"))
        AppendText(out, outSize, off, "ghz");
      else
        AppendText(out, outSize, off, "gb");
      AppendText(out, outSize, off,
                 "\",\"choiceLabelZh\":\"实际单位\",\"choiceLabelEn\":\"Real unit\"");
    }
    AppendText(out, outSize, off,
               ",\"toggleId\":\"%s\",\"toggleLabelZh\":\"任务栏显示\","
               "\"toggleLabelEn\":\"Show on taskbar\",\"toggleValue\":%d}",
               metric.id, SettingVisible(settings, metric.id) ? 1 : 0);
  }
  AppendText(out, outSize, off, "]}");
  return off < outSize ? off : outSize - 1;
}

int SetControl(const char *path, const char *value)
{
  if (!path || !value)
    return 0;
  const size_t pathLength = std::strlen(path);
  if (pathLength > 5 && !std::strcmp(path + pathLength - 5, "_unit"))
  {
    char metric[kMaxMetricId] = {};
    const size_t metricLength = std::min(pathLength - 5, sizeof(metric) - 1);
    std::memcpy(metric, path, metricLength);
    metric[metricLength] = 0;
    bool changed = false;
    EnterCriticalSection(&g_lock);
    const char *before = MetricUnit(g_settings, metric);
    changed = std::strcmp(before, value) != 0 &&
              SetMetricUnit(g_settings, metric, value);
    LeaveCriticalSection(&g_lock);
    if (changed)
    {
      SaveSettings();
      if (g_sampleWake)
        SetEvent(g_sampleWake);
    }
    return UnitAllowed(metric, value) ? 1 : 0;
  }
  bool on = false;
  if (!ParseBool(value, &on))
    return 0;
  bool recognized = false;
  bool changed = false;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_snapshot.metricCount; ++i)
  {
    if (std::strcmp(path, g_snapshot.metrics[i].id))
      continue;
    recognized = true;
    const bool before = SettingVisible(g_settings, path);
    SetSettingVisible(g_settings, path, on);
    changed = before != on;
    break;
  }
  LeaveCriticalSection(&g_lock);
  if (changed)
  {
    SaveSettings();
    if (g_sampleWake)
      SetEvent(g_sampleWake);
  }
  return recognized ? 1 : 0;
}

int ListOp(const char *, const char *, const char *, int) { return 0; }

int Init(const ApexHost *host)
{
  g_host = host;
  if (host && host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));
  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  OpenLog();
  LoadSettings();
  InterlockedExchange(&g_stop, 0);
  g_sampleWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_sampleThread = g_sampleWake
                       ? CreateThread(nullptr, 0, SampleThread, nullptr, 0, nullptr)
                       : nullptr;
  if (!g_sampleThread)
  {
    LogLine("feature %s: sampler thread did not start (%lu)", kId, GetLastError());
    if (g_sampleWake)
      CloseHandle(g_sampleWake);
    g_sampleWake = nullptr;
    if (g_log)
    {
      fclose(g_log);
      g_log = nullptr;
    }
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
    return 1;
  }
  LogLine("feature %s: started (%d saved mapping(s))", kId, g_settings.count);
  WriteLog("started");
  return 0;
}

void Shutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_sampleWake)
    SetEvent(g_sampleWake);
  if (g_sampleThread)
  {
    WaitForSingleObject(g_sampleThread, 5000);
    CloseHandle(g_sampleThread);
    g_sampleThread = nullptr;
  }
  if (g_sampleWake)
    CloseHandle(g_sampleWake);
  g_sampleWake = nullptr;
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
}

int Reload()
{
  LoadSettings();
  if (g_sampleWake)
    SetEvent(g_sampleWake);
  return 1;
}

int Save() { return SaveSettings() ? 1 : 0; }

int TaskbarText(const char *key, int chinese, char *out, int outSize)
{
  if (!key || !out || outSize <= 0)
    return 0;
  Snapshot snapshot;
  EnterCriticalSection(&g_lock);
  snapshot = g_snapshot;
  LeaveCriticalSection(&g_lock);
  for (int i = 0; i < snapshot.metricCount; ++i)
  {
    char expected[128] = {};
    _snprintf(expected, sizeof(expected), "%s:%s", kId, snapshot.metrics[i].id);
    if (std::strcmp(key, expected))
      continue;
    CopyText(out, outSize,
             chinese ? snapshot.metrics[i].shortText
                     : (snapshot.metrics[i].shortEn[0] ? snapshot.metrics[i].shortEn
                                                       : snapshot.metrics[i].shortText));
    return out[0] ? 1 : 0;
  }
  return 0;
}

int TaskbarItems(ApexTaskbarArea *out, int max)
{
  Settings settings;
  Snapshot snapshot;
  EnterCriticalSection(&g_lock);
  settings = g_settings;
  snapshot = g_snapshot;
  LeaveCriticalSection(&g_lock);
  int total = 0;
  for (int i = 0; i < snapshot.metricCount; ++i)
  {
    const MetricSnapshot &metric = snapshot.metrics[i];
    if (!SettingVisible(settings, metric.id))
      continue;
    if (out && total < max)
    {
      ApexTaskbarArea &area = out[total];
      ZeroMemory(&area, sizeof(area));
      _snprintf(area.key, sizeof(area.key), "%s:%s", kId, metric.id);
      CopyText(area.nameZh, sizeof(area.nameZh), metric.nameZh);
      CopyText(area.nameEn, sizeof(area.nameEn), metric.nameEn);
      area.lineCount = 1;
    }
    ++total;
  }
  return total;
}

unsigned Flags() { return APEX_FEATURE_ENABLED; }

} // namespace

static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    kId,
    "系统状态",
    "System Status",
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
    TaskbarText,
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
