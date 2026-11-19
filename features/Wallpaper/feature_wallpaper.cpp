// ---------------------------------------------------------------------------
// Wallpaper -- MULTI-MONITOR WALLPAPERS AND SYSTEM LOCK SCREEN.
//
// The feature owns its worker, its settings file, direct image copies, and the system wallpaper calls. The host
// only sees the normal ApexFeature ABI.
//
// Two Windows interfaces are used deliberately:
//   * IDesktopWallpaper -- the system's supported per-monitor desktop wallpaper interface. Setting through it
//     IS setting the system wallpaper; there is no shadow copy that another program would have to reproduce.
//   * Windows.System.UserProfile.LockScreen::SetImageFileAsync -- the current lock-screen interface.
//     UserProfilePersonalizationSettings remains only as a compatibility fallback.
//
// "Composite" is one generated PNG whose canvas is the real bounding rectangle of the virtual desktop and whose
// monitor rectangles keep their real positions, including monitors left of or above the primary one. That same
// image is used for the lock screen when the desktop is using separate images per monitor.
// ---------------------------------------------------------------------------

#include "../../apex/abi.h"
#include "wallpaper_logic.h"
#include "wallpaper_winrt.h"

#include <windows.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <gdiplus.h>
#include <sddl.h>

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <memory>

using apex::wallpaper::Rect;
using apex::wallpaper::Schedule;
using apex::wallpaper::Variant;

namespace {

const char *kId = "Wallpaper";
const char *kVersion = "1.1.28";

constexpr int kMaxDisplays = 12;
constexpr int kMaxMonitorSettings = 24;
constexpr int kMaxPath = 512;
constexpr int kMaxFile = 260;
constexpr int kMaxDevice = 256;
constexpr int kThumbnailDataSize = 16384;
constexpr int kMaxLog = 256 * 1024;
constexpr DWORD kCycleMs = 500;
constexpr DWORD kDisplayPollMs = 1000;
constexpr DWORD kExternalChangeGraceMs = 1000;

const char *kScheduleClock = "clock";
const char *kScheduleSolar = "solar";
const char *kScheduleInterval = "interval";

// WinRT interfaces that are present in the Windows metadata but absent from the MinGW SDK header used to build
// Apex. Their IIDs and method order come from Windows.System.winmd / Windows.Storage.winmd (26100 contracts).
using apex::wallpaper::winrt::IBooleanOperation;
using apex::wallpaper::winrt::IStorageFileOperation;
using apex::wallpaper::winrt::IStorageFileStatics;
using apex::wallpaper::winrt::IPersonalizationSettings;
using apex::wallpaper::winrt::IPersonalizationStatics;

struct MonitorSetting
{
  char identity[96] = {0};
  char wallpaper[kMaxPath] = {0};
  char style[16] = {0};
  bool locked = false;
};

struct Display
{
  char device[kMaxDevice] = {0};
  char identity[96] = {0};
  char edid[32] = {0};
  Rect rect = {0, 0, 0, 0};
  char currentPath[kMaxPath] = {0};
  char currentStyle[16] = {0};
  char appliedSource[kMaxPath] = {0};
  char appliedStyle[16] = {0};
  char thumbnailSource[kMaxPath] = {0};
  char thumbnail[kThumbnailDataSize] = {0};
};

struct Settings
{
  char schedule[16] = "clock";
  char lockPath[kMaxPath] = {0};
  int dayStart = 7 * 60;
  int nightStart = 19 * 60;
  double latitude = 39.9042;
  double longitude = 116.4074;
  bool locationKnown = false;
  int intervalMinutes = 60;
};

struct Snapshot
{
  Settings s;
  Display display[kMaxDisplays];
  int displayCount = 0;
  MonitorSetting monitors[kMaxMonitorSettings];
  int monitorCount = 0;
  char currentLock[kMaxPath] = {0};
  char currentLockThumbnail[kThumbnailDataSize] = {0};
  double latitude = 0.0;
  double longitude = 0.0;
  bool locationKnown = false;
};

struct Plan
{
  bool valid = false;
  bool syncLock = false;
  Variant variant = Variant::Day;
  int displayCount = 0;
  bool managed[kMaxDisplays] = {false};
  bool renderNeeded[kMaxDisplays] = {false};
  bool applyNeeded[kMaxDisplays] = {false};
  Display displays[kMaxDisplays];
  char displayPath[kMaxDisplays][kMaxPath] = {{0}};
  char renderPath[kMaxDisplays][kMaxPath] = {{0}};
  char style[kMaxDisplays][16] = {{0}};
  char lockSource[kMaxPath] = {0};
  char signature[16384] = {0};
};

const ApexHost *g_host = nullptr;
char g_dir[kMaxPath] = {0};
FILE *g_log = nullptr;

Settings g_settings;
Display g_displays[kMaxDisplays];
int g_displayCount = 0;
MonitorSetting g_monitors[kMaxMonitorSettings];
int g_monitorCount = 0;

bool g_apiReady = false;
bool g_lockSupported = false;
bool g_lockChecked = false;
bool g_lockApplyPending = false;
DWORD g_locationFetchedAt = 0;
char g_locationState[128] = "not requested";
char g_currentLock[kMaxPath] = {0};
char g_lockThumbnailSource[kMaxPath] = {0};
char g_currentLockThumbnail[kThumbnailDataSize] = {0};
char g_expectedGlobalWallpaper[kMaxPath] = {0};
char g_expectedGlobalWallpaperSystem[kMaxPath] = {0};
IWbemServices *g_wmi = nullptr;
bool g_wmiTried = false;
bool g_wmiUsable = false;

CRITICAL_SECTION g_lock;
CRITICAL_SECTION g_saveLock;
bool g_lockReady = false;
bool g_saveLockReady = false;
HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;
volatile LONG g_stop = 0;
ULONG_PTR g_gdiplusToken = 0;

bool g_noApply = false; // test-only process switch; no user setting can reach it
volatile LONG g_renderSerial = 0;
volatile LONG g_applyDirty = 0;
DWORD g_lastWallpaperApplyTick = 0;
bool g_restoreLockedPending = false;
DESKTOP_WALLPAPER_POSITION g_lastSystemPosition = DWPOS_FILL;
bool g_systemPositionKnown = false;
DESKTOP_WALLPAPER_POSITION g_expectedSystemPosition = DWPOS_FILL;
bool g_expectedSystemPositionPending = false;
bool g_spanPositionReported = false;

bool ValidStyle(const char *style);
bool FileNameExists(const char *path);

void MarkApplyDirty()
{
  InterlockedExchange(&g_applyDirty, 1);
}

void Log(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[400] = {0};
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
  char path[kMaxPath + 64] = {0};
  _snprintf(path, sizeof(path), "%sWallpaper.log", g_dir);
  g_log = fopen(path, "w");
}

void LogEvent(const char *fmt, ...)
{
  if (!g_log)
    return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(g_log, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  if (ftell(g_log) > kMaxLog)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  else
    fflush(g_log);
}

bool ToWide(const char *text, wchar_t *out, int outCount)
{
  if (!text || !out || outCount <= 0)
    return false;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, outCount);
  if (n <= 0)
  {
    out[0] = 0;
    return false;
  }
  return true;
}

bool ToUtf8(const wchar_t *text, char *out, int outCount)
{
  if (!text || !out || outCount <= 0)
    return false;
  const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, outCount, nullptr, nullptr);
  if (n <= 0)
  {
    out[0] = 0;
    return false;
  }
  return true;
}

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
  while (src[n] && n < cap - 1)
  {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = 0;
}

void TrimText(char *text)
{
  if (!text)
    return;
  char *begin = text;
  while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n')
    ++begin;
  if (begin != text)
    memmove(text, begin, strlen(begin) + 1);
  size_t n = strlen(text);
  while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' ||
                   text[n - 1] == '\n'))
    text[--n] = 0;
}

bool ParseBoolText(const char *text, bool *out)
{
  if (!text || !out)
    return false;
  if (!strcmp(text, "1") || !_stricmp(text, "true") || !_stricmp(text, "on") ||
      !_stricmp(text, "yes"))
  {
    *out = true;
    return true;
  }
  if (!strcmp(text, "0") || !_stricmp(text, "false") || !_stricmp(text, "off") ||
      !_stricmp(text, "no"))
  {
    *out = false;
    return true;
  }
  return false;
}

void UpperAscii(char *text)
{
  for (; text && *text; ++text)
    if (*text >= 'a' && *text <= 'z')
      *text = static_cast<char>(*text - 'a' + 'A');
}

bool EdidFromDeviceId(const wchar_t *deviceId, char *out, int outSize)
{
  if (!deviceId || !out || outSize <= 0)
    return false;
  char text[512] = {0};
  if (!ToUtf8(deviceId, text, (int)sizeof(text)))
    return false;
  const char *monitor = _strnicmp(text, "MONITOR\\", 8) == 0 ? text + 8 : text;
  const char *end = strchr(monitor, '\\');
  const size_t n = end ? static_cast<size_t>(end - monitor) : strlen(monitor);
  if (n == 0 || n >= static_cast<size_t>(outSize))
    return false;
  memcpy(out, monitor, n);
  out[n] = 0;
  UpperAscii(out);
  return out[0] != 0;
}

void CanonicalEdidIdentity(const char *edid, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  if (!edid || !edid[0])
    return;
  if (strlen(edid) == 7)
  {
    bool letters = true;
    for (int i = 0; i < 3; ++i)
      if (!((edid[i] >= 'A' && edid[i] <= 'Z') || (edid[i] >= 'a' && edid[i] <= 'z')))
        letters = false;
    if (letters)
    {
      _snprintf(out, outSize, "%.3s-%s", edid, edid + 3);
      return;
    }
  }
  CopyText(out, outSize, edid);
}

void WmiOpen()
{
  if (g_wmiTried)
    return;
  g_wmiTried = true;
  CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                       RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
  IWbemLocator *locator = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, reinterpret_cast<void **>(&locator));
  if (FAILED(hr) || !locator)
    return;
  IWbemServices *service = nullptr;
  BSTR ns = SysAllocString(L"ROOT\\WMI");
  hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &service);
  SysFreeString(ns);
  locator->Release();
  if (FAILED(hr) || !service)
  {
    LogEvent("WMI ROOT\\WMI unavailable (0x%08lx); monitor identity will use EDID", (unsigned long)hr);
    return;
  }
  CoSetProxyBlanket(service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  g_wmi = service;
  g_wmiUsable = true;
  LogEvent("WMI ROOT\\WMI opened for monitor identity");
}

bool WmiMonitorIdentity(const char *edid, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;
  if (!g_wmiUsable || !g_wmi || !edid || !edid[0])
    return false;
  IEnumWbemClassObject *enumerator = nullptr;
  BSTR language = SysAllocString(L"WQL");
  BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorID");
  const HRESULT queryHr = g_wmi->ExecQuery(
      language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
  SysFreeString(language);
  SysFreeString(query);
  if (FAILED(queryHr) || !enumerator)
    return false;

  char upperEdid[64] = {0};
  CopyText(upperEdid, (int)sizeof(upperEdid), edid);
  UpperAscii(upperEdid);
  bool found = false;
  for (;;)
  {
    IWbemClassObject *object = nullptr;
    ULONG got = 0;
    if (enumerator->Next(WBEM_INFINITE, 1, &object, &got) != S_OK || got == 0 || !object)
      break;
    VARIANT instance;
    VariantInit(&instance);
    if (object->Get(L"InstanceName", 0, &instance, nullptr, nullptr) == S_OK &&
        instance.vt == VT_BSTR)
    {
      char text[512] = {0};
      WideCharToMultiByte(CP_UTF8, 0, instance.bstrVal, -1, text, (int)sizeof(text) - 1,
                          nullptr, nullptr);
      UpperAscii(text);
      if (strstr(text, upperEdid))
      {
        const wchar_t *fields[3] = {L"ManufacturerName", L"ProductCodeID", L"SerialNumberID"};
        char part[3][64] = {{0}};
        for (int field = 0; field < 3; ++field)
        {
          VARIANT array;
          VariantInit(&array);
          if (object->Get(fields[field], 0, &array, nullptr, nullptr) == S_OK &&
              (array.vt & VT_ARRAY))
          {
            LONG low = 0, high = -1;
            int n = 0;
            if (SafeArrayGetLBound(array.parray, 1, &low) == S_OK &&
                SafeArrayGetUBound(array.parray, 1, &high) == S_OK)
              for (LONG i = low; i <= high && n < (int)sizeof(part[0]) - 1; ++i)
              {
                LONG value = 0;
                if (SafeArrayGetElement(array.parray, &i, &value) == S_OK && value > 0 &&
                    value < 128)
                  part[field][n++] = static_cast<char>(value);
              }
            VariantClear(&array);
          }
        }
        bool serial = part[2][0] != 0;
        if (serial)
        {
          bool allZero = true;
          for (const char *p = part[2]; *p; ++p)
            if (*p != '0')
              allZero = false;
          if (allZero)
            serial = false;
        }
        if (part[0][0] && part[1][0])
          _snprintf(out, outSize, "%s-%s%s%s", part[0], part[1], serial ? "-" : "",
                    serial ? part[2] : "");
        else
          CanonicalEdidIdentity(edid, out, outSize);
        found = out[0] != 0;
      }
    }
    VariantClear(&instance);
    object->Release();
    if (found)
      break;
  }
  enumerator->Release();
  return found;
}

struct IdentityCacheEntry
{
  char edid[32];
  char identity[96];
};
IdentityCacheEntry g_identityCache[kMaxDisplays * 2];
int g_identityCacheCount = 0;

void ResolveMonitorIdentity(const char *edid, char *out, int outSize)
{
  out[0] = 0;
  if (!edid || !edid[0])
    return;
  for (int i = 0; i < g_identityCacheCount; ++i)
    if (!strcmp(g_identityCache[i].edid, edid))
    {
      CopyText(out, outSize, g_identityCache[i].identity);
      return;
    }
  char identity[96] = {0};
  if (!WmiMonitorIdentity(edid, identity, (int)sizeof(identity)))
    CanonicalEdidIdentity(edid, identity, (int)sizeof(identity));
  CopyText(out, outSize, identity);
  if (identity[0] && g_identityCacheCount < (int)(sizeof(g_identityCache) / sizeof(g_identityCache[0])))
  {
    IdentityCacheEntry &entry = g_identityCache[g_identityCacheCount++];
    CopyText(entry.edid, (int)sizeof(entry.edid), edid);
    CopyText(entry.identity, (int)sizeof(entry.identity), identity);
    LogEvent("monitor %s identity: %s", edid, identity);
  }
}

bool ValidSchedule(const char *value)
{
  return value && (!strcmp(value, kScheduleClock) || !strcmp(value, kScheduleSolar) ||
                   !strcmp(value, kScheduleInterval));
}

Schedule ParseSchedule(const char *text)
{
  if (!strcmp(text, kScheduleClock))
    return Schedule::Clock;
  if (!strcmp(text, kScheduleSolar))
    return Schedule::Solar;
  if (!strcmp(text, kScheduleInterval))
    return Schedule::Interval;
  return Schedule::Clock;
}

bool ConfigPath(char *out, int outSize)
{
  if (!g_dir[0])
    return false;
  const int n = _snprintf(out, outSize, "%sWallpaper.ini", g_dir);
  return n > 0 && n < outSize;
}

int HexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool EncodeHex(const char *text, char *out, int outSize)
{
  if (!text || !out || outSize < 3)
    return false;
  int off = 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
  {
    if (off + 3 >= outSize)
      return false;
    static const char kHex[] = "0123456789abcdef";
    out[off++] = kHex[*p >> 4];
    out[off++] = kHex[*p & 15];
  }
  out[off] = 0;
  return true;
}

bool DecodeHex(const char *text, char *out, int outSize)
{
  if (!text || !out || outSize <= 0)
    return false;
  int off = 0;
  while (text[0] && text[1])
  {
    const int hi = HexValue(text[0]), lo = HexValue(text[1]);
    if (hi < 0 || lo < 0 || off >= outSize - 1)
      return false;
    out[off++] = static_cast<char>((hi << 4) | lo);
    text += 2;
  }
  if (*text)
    return false;
  out[off] = 0;
  return true;
}

MonitorSetting *FindMonitorSettingLocked(const char *identity)
{
  for (int i = 0; i < g_monitorCount; ++i)
    if (!strcmp(g_monitors[i].identity, identity))
      return &g_monitors[i];
  return nullptr;
}

MonitorSetting *EnsureMonitorSettingLocked(const char *identity)
{
  MonitorSetting *existing = FindMonitorSettingLocked(identity);
  if (existing)
    return existing;
  if (g_monitorCount >= kMaxMonitorSettings)
    return nullptr;
  MonitorSetting *fresh = &g_monitors[g_monitorCount++];
  ZeroMemory(fresh, sizeof(*fresh));
  CopyText(fresh->identity, (int)sizeof(fresh->identity), identity);
  return fresh;
}

bool ResolveImageLocked(const Settings &s, const char *name, char *out, int outSize)
{
  if (!name || !name[0] || !out || outSize <= 0)
    return false;
  const bool absolute =
      (strlen(name) >= 2 && name[1] == ':') ||
      (strlen(name) >= 2 && (name[0] == '\\' || name[0] == '/') &&
       (name[1] == '\\' || name[1] == '/'));
  if (absolute)
  {
    CopyText(out, outSize, name);
    return true;
  }
  (void)s;
  return false;
}

void LoadSettings()
{
  Settings fresh;
  MonitorSetting monitors[kMaxMonitorSettings] = {};
  int monitorCount = 0;

  char path[kMaxPath + 64] = {0};
  if (ConfigPath(path, (int)sizeof(path)))
  {
    FILE *f = fopen(path, "rb");
    if (f)
    {
      char line[2048] = {0};
      while (fgets(line, sizeof(line), f))
      {
        char *p = line;
        while (*p == ' ' || *p == '\t')
          ++p;
        if (!*p || *p == '#' || *p == '\r' || *p == '\n')
          continue;
        char *eq = strchr(p, '=');
        if (!eq)
          continue;
        *eq = 0;
        char *key = p;
        char *value = eq + 1;
        TrimText(key);
        TrimText(value);

        if (!strcmp(key, "schedule") && ValidSchedule(value))
          CopyText(fresh.schedule, (int)sizeof(fresh.schedule), value);
        else if (!strcmp(key, "lock_file"))
          CopyText(fresh.lockPath, (int)sizeof(fresh.lockPath), value);
        else if (!strcmp(key, "day_start"))
          apex::wallpaper::ParseClock(value, &fresh.dayStart);
        else if (!strcmp(key, "night_start"))
          apex::wallpaper::ParseClock(value, &fresh.nightStart);
        else if (!strcmp(key, "auto_latitude"))
          fresh.locationKnown = apex::wallpaper::ParseDouble(value, &fresh.latitude);
        else if (!strcmp(key, "auto_longitude"))
        {
          double longitude = 0;
          if (apex::wallpaper::ParseDouble(value, &longitude))
            fresh.longitude = longitude;
        }
        else if (!strcmp(key, "interval_minutes"))
        {
          int number = 0;
          if (apex::wallpaper::ParseInt(value, &number))
            fresh.intervalMinutes = number < 1 ? 1 : number > 1440 ? 1440 : number;
        }
        else if (!strncmp(key, "monitor.", 8))
        {
          char *dot = strchr(key + 8, '.');
          if (!dot || monitorCount >= kMaxMonitorSettings)
            continue;
          *dot = 0;
          char identity[96] = {0};
          if (!DecodeHex(key + 8, identity, (int)sizeof(identity)))
            continue;
          MonitorSetting *ms = nullptr;
          for (int i = 0; i < monitorCount; ++i)
            if (!strcmp(monitors[i].identity, identity))
            {
              ms = &monitors[i];
              break;
            }
          if (!ms)
          {
            ms = &monitors[monitorCount++];
            CopyText(ms->identity, (int)sizeof(ms->identity), identity);
          }
          if (!strcmp(dot + 1, "wallpaper"))
            CopyText(ms->wallpaper, (int)sizeof(ms->wallpaper), value);
          else if (!strcmp(dot + 1, "style"))
            CopyText(ms->style, (int)sizeof(ms->style), value);
          else if (!strcmp(dot + 1, "locked"))
            ms->locked = atoi(value) != 0;
        }
      }
      fclose(f);
    }
  }

  if (fresh.latitude < -90.0 || fresh.latitude > 90.0 ||
      fresh.longitude < -180.0 || fresh.longitude > 180.0)
    fresh.locationKnown = false;

  EnterCriticalSection(&g_lock);
  g_settings = fresh;
  g_monitorCount = monitorCount;
  for (int i = 0; i < monitorCount; ++i)
    g_monitors[i] = monitors[i];
  LeaveCriticalSection(&g_lock);
}

bool SaveSettings()
{
  char path[kMaxPath + 64] = {0};
  char temp[kMaxPath + 80] = {0};
  if (!ConfigPath(path, (int)sizeof(path)))
    return false;
  struct SaveLock
  {
    bool held;
    SaveLock() : held(g_saveLockReady)
    {
      if (held)
        EnterCriticalSection(&g_saveLock);
    }
    ~SaveLock()
    {
      if (held)
        LeaveCriticalSection(&g_saveLock);
    }
  } saveLock;
  _snprintf(temp, sizeof(temp), "%s.tmp", path);

  Settings s;
  MonitorSetting monitors[kMaxMonitorSettings];
  int monitorCount = 0;
  EnterCriticalSection(&g_lock);
  s = g_settings;
  monitorCount = g_monitorCount;
  for (int i = 0; i < monitorCount; ++i)
    monitors[i] = g_monitors[i];
  LeaveCriticalSection(&g_lock);

  FILE *f = fopen(temp, "wb");
  if (!f)
    return false;
  char day[8] = {0}, night[8] = {0};
  _snprintf(day, sizeof(day), "%02d:%02d", s.dayStart / 60, s.dayStart % 60);
  _snprintf(night, sizeof(night), "%02d:%02d", s.nightStart / 60, s.nightStart % 60);
  fprintf(f, "# Apex Wallpaper -- this feature's own settings\n");
  fprintf(f, "schedule=%s\n", s.schedule);
  fprintf(f, "lock_file=%s\n", s.lockPath);
  fprintf(f, "day_start=%s\n", day);
  fprintf(f, "night_start=%s\n", night);
  if (s.locationKnown)
  {
    fprintf(f, "auto_latitude=%.6f\n", s.latitude);
    fprintf(f, "auto_longitude=%.6f\n", s.longitude);
  }
  fprintf(f, "interval_minutes=%d\n", s.intervalMinutes);
  for (int i = 0; i < monitorCount; ++i)
  {
    char encoded[kMaxDevice * 2 + 1] = {0};
    if (!EncodeHex(monitors[i].identity, encoded, (int)sizeof(encoded)))
      continue;
    fprintf(f, "monitor.%s.wallpaper=%s\n", encoded, monitors[i].wallpaper);
    fprintf(f, "monitor.%s.style=%s\n", encoded, monitors[i].style);
    fprintf(f, "monitor.%s.locked=%d\n", encoded, monitors[i].locked ? 1 : 0);
  }
  fflush(f);
  const bool wrote = ferror(f) == 0;
  fclose(f);
  if (!wrote)
  {
    DeleteFileA(temp);
    return false;
  }
  wchar_t wideTemp[kMaxPath + 80] = {0}, widePath[kMaxPath + 64] = {0};
  if (!ToWide(temp, wideTemp, (int)(sizeof(wideTemp) / sizeof(wideTemp[0]))) ||
      !ToWide(path, widePath, (int)(sizeof(widePath) / sizeof(widePath[0]))) ||
      !MoveFileExW(wideTemp, widePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    DeleteFileA(temp);
    return false;
  }
  return true;
}

int CompareDisplays(const void *a, const void *b)
{
  const Display *x = static_cast<const Display *>(a);
  const Display *y = static_cast<const Display *>(b);
  if (x->rect.left != y->rect.left)
    return x->rect.left < y->rect.left ? -1 : 1;
  if (x->rect.top != y->rect.top)
    return x->rect.top < y->rect.top ? -1 : 1;
  return strcmp(x->device, y->device);
}

const char *PositionId(DWORD position)
{
  switch (position)
  {
  case DWPOS_CENTER: return "center";
  case DWPOS_TILE: return "tile";
  case DWPOS_STRETCH: return "stretch";
  case DWPOS_FIT: return "fit";
  case DWPOS_FILL: return "fill";
  case DWPOS_SPAN: return "fill";
  default: return "fill";
  }
}

DESKTOP_WALLPAPER_POSITION PositionForId(const char *id)
{
  if (!strcmp(id, "center"))
    return DWPOS_CENTER;
  if (!strcmp(id, "tile"))
    return DWPOS_TILE;
  if (!strcmp(id, "stretch"))
    return DWPOS_STRETCH;
  if (!strcmp(id, "fit"))
    return DWPOS_FIT;
  return DWPOS_FILL;
}

void ReadRegistryString(HKEY root, const char *keyPath, const char *name, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  HKEY key = nullptr;
  if (RegOpenKeyExA(root, keyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
    return;
  DWORD type = 0, size = (DWORD)outSize;
  RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(out), &size);
  RegCloseKey(key);
  if (type != REG_SZ && type != REG_EXPAND_SZ)
    out[0] = 0;
  out[outSize - 1] = 0;
}

void ReadSingleDisplayState(char *path, int pathSize, char *style, int styleSize)
{
  if (path && pathSize > 0)
  {
    wchar_t wide[kMaxPath] = {0};
    path[0] = 0;
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                              static_cast<UINT>(sizeof(wide) / sizeof(wide[0])), wide, 0))
      ToUtf8(wide, path, pathSize);
  }
  if (style && styleSize > 0)
  {
    char value[32] = {0};
    ReadRegistryString(HKEY_CURRENT_USER, "Control Panel\\Desktop", "WallpaperStyle", value,
                       (int)sizeof(value));
    char tile[32] = {0};
    ReadRegistryString(HKEY_CURRENT_USER, "Control Panel\\Desktop", "TileWallpaper", tile,
                       (int)sizeof(tile));
    const char *mapped = "fill";
    if (!strcmp(tile, "1"))
      mapped = "tile";
    else if (!strcmp(value, "0"))
      mapped = "center";
    else if (!strcmp(value, "2"))
      mapped = "stretch";
    else if (!strcmp(value, "6"))
      mapped = "fit";
    else if (!strcmp(value, "22"))
      mapped = "fill";
    CopyText(style, styleSize, mapped);
  }
}

bool CurrentUserSidText(wchar_t *out, size_t outCount)
{
  if (!out || outCount == 0)
    return false;
  out[0] = 0;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  if (!bytes)
  {
    CloseHandle(token);
    return false;
  }
  TOKEN_USER *user = static_cast<TOKEN_USER *>(malloc(bytes));
  bool ok = user &&
            GetTokenInformation(token, TokenUser, user, bytes, &bytes) != FALSE;
  CloseHandle(token);
  if (!ok)
  {
    free(user);
    return false;
  }
  LPWSTR sid = nullptr;
  ok = ConvertSidToStringSidW(user->User.Sid, &sid) != FALSE;
  if (ok && sid)
  {
    wcsncpy(out, sid, outCount - 1);
    out[outCount - 1] = 0;
  }
  LocalFree(sid);
  free(user);
  return ok && out[0];
}

bool LockImageExtension(const wchar_t *name)
{
  const wchar_t *dot = name ? wcsrchr(name, L'.') : nullptr;
  return dot && (!_wcsicmp(dot, L".jpg") || !_wcsicmp(dot, L".jpeg") ||
                 !_wcsicmp(dot, L".png"));
}

void FindNewestLockScreenFile(const wchar_t *dir, int depth, wchar_t *out, size_t outCount,
                              FILETIME *bestTime)
{
  if (!dir || !out || !outCount || !bestTime)
    return;
  wchar_t pattern[kMaxPath] = {0};
  _snwprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%s\\*", dir);
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW(pattern, &data);
  if (find == INVALID_HANDLE_VALUE)
    return;
  do
  {
    if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
      continue;
    wchar_t path[kMaxPath] = {0};
    _snwprintf(path, sizeof(path) / sizeof(path[0]), L"%s\\%s", dir, data.cFileName);
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      if (depth > 0 && !wcsncmp(data.cFileName, L"LockScreen", 10))
        FindNewestLockScreenFile(path, depth - 1, out, outCount, bestTime);
      continue;
    }
    if (wcsncmp(data.cFileName, L"LockScreen", 10) || !LockImageExtension(data.cFileName))
      continue;
    if (!out[0] || CompareFileTime(&data.ftLastWriteTime, bestTime) > 0)
    {
      wcsncpy(out, path, outCount - 1);
      out[outCount - 1] = 0;
      *bestTime = data.ftLastWriteTime;
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
}

bool FindProtectedLockScreenImage(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;
  wchar_t sid[128] = {0};
  if (!CurrentUserSidText(sid, sizeof(sid) / sizeof(sid[0])))
    return false;
  wchar_t root[kMaxPath] = {0};
  _snwprintf(root, sizeof(root) / sizeof(root[0]),
             L"C:\\ProgramData\\Microsoft\\Windows\\SystemData\\%s\\ReadOnly", sid);
  FILETIME best = {};
  wchar_t path[kMaxPath] = {0};
  FindNewestLockScreenFile(root, 2, path, sizeof(path) / sizeof(path[0]), &best);
  return path[0] && ToUtf8(path, out, outSize);
}

struct SpotlightChoice
{
  wchar_t metadataPath[kMaxPath] = {0};
  FILETIME metadataTime = {};
  DWORD sizes[16] = {};
  int sizeCount = 0;
  SYSTEMTIME start = {};
  DWORD rotationSeconds = 0;
  bool startKnown = false;
};

bool ReadSpotlightMetadata(const wchar_t *path, SpotlightChoice *choice)
{
  if (!path || !choice)
    return false;
  FILE *file = _wfopen(path, L"rb");
  if (!file)
    return false;
  fseek(file, 0, SEEK_END);
  const long length = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (length <= 0 || length > 512 * 1024)
  {
    fclose(file);
    return false;
  }
  char *text = static_cast<char *>(malloc(static_cast<size_t>(length) + 1));
  if (!text)
  {
    fclose(file);
    return false;
  }
  const size_t got = fread(text, 1, static_cast<size_t>(length), file);
  fclose(file);
  text[got] = 0;
  if (!strstr(text, "LockScreen"))
  {
    free(text);
    return false;
  }

  SpotlightChoice parsed;
  const char *scan = text;
  while ((scan = strstr(scan, "landscapeImage")) != nullptr)
  {
    const char *size = strstr(scan, "fileSize");
    if (size && size - scan < 512)
    {
      const char *number = size + strlen("fileSize");
      while (*number && (*number < '0' || *number > '9'))
        ++number;
      if (*number && parsed.sizeCount < (int)(sizeof(parsed.sizes) / sizeof(parsed.sizes[0])))
      {
        const DWORD value = static_cast<DWORD>(strtoul(number, nullptr, 10));
        bool duplicate = false;
        for (int i = 0; i < parsed.sizeCount; ++i)
          if (parsed.sizes[i] == value)
            duplicate = true;
        if (value && !duplicate)
          parsed.sizes[parsed.sizeCount++] = value;
      }
    }
    scan += strlen("landscapeImage");
  }

  const char *period = strstr(text, "rotationPeriod");
  if (period)
  {
    const char *number = period + strlen("rotationPeriod");
    while (*number && (*number < '0' || *number > '9'))
      ++number;
    if (*number)
      parsed.rotationSeconds = static_cast<DWORD>(strtoul(number, nullptr, 10));
  }
  const char *start = strstr(text, "startTime");
  if (start)
  {
    const char *number = start + strlen("startTime");
    while (*number && (*number < '0' || *number > '9'))
      ++number;
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(number, "%4u-%2u-%2uT%2u:%2u:%2u", &year, &month, &day, &hour, &minute,
               &second) == 6)
    {
      parsed.start.wYear = static_cast<WORD>(year);
      parsed.start.wMonth = static_cast<WORD>(month);
      parsed.start.wDay = static_cast<WORD>(day);
      parsed.start.wHour = static_cast<WORD>(hour);
      parsed.start.wMinute = static_cast<WORD>(minute);
      parsed.start.wSecond = static_cast<WORD>(second);
      FILETIME ignored = {};
      parsed.startKnown = SystemTimeToFileTime(&parsed.start, &ignored) != FALSE;
    }
  }
  free(text);
  if (!parsed.sizeCount)
    return false;
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
    return false;
  parsed.metadataTime = attributes.ftLastWriteTime;
  wcsncpy(parsed.metadataPath, path, (sizeof(parsed.metadataPath) / sizeof(wchar_t)) - 1);
  *choice = parsed;
  return true;
}

void FindLatestSpotlightMetadata(const wchar_t *dir, int depth, SpotlightChoice *best)
{
  if (!dir || !best)
    return;
  wchar_t pattern[kMaxPath] = {0};
  _snwprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%s\\*", dir);
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW(pattern, &data);
  if (find == INVALID_HANDLE_VALUE)
    return;
  do
  {
    if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
      continue;
    wchar_t path[kMaxPath] = {0};
    _snwprintf(path, sizeof(path) / sizeof(path[0]), L"%s\\%s", dir, data.cFileName);
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      if (depth > 0)
        FindLatestSpotlightMetadata(path, depth - 1, best);
      continue;
    }
    SpotlightChoice candidate;
    if (!ReadSpotlightMetadata(path, &candidate))
      continue;
    if (!best->metadataPath[0] || CompareFileTime(&candidate.metadataTime, &best->metadataTime) > 0)
      *best = candidate;
  } while (FindNextFileW(find, &data));
  FindClose(find);
}

bool FindSpotlightLockScreenImage(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;
  wchar_t local[kMaxPath] = {0};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local,
                               static_cast<DWORD>(sizeof(local) / sizeof(local[0]))) ||
      !local[0])
    return false;
  wchar_t state[kMaxPath] = {0};
  _snwprintf(state, sizeof(state) / sizeof(state[0]),
             L"%s\\Packages\\Microsoft.Windows.ContentDeliveryManager_cw5n1h2txyewy\\LocalState",
             local);
  wchar_t creatives[kMaxPath] = {0};
  _snwprintf(creatives, sizeof(creatives) / sizeof(creatives[0]),
             L"%s\\ContentManagementSDK\\Creatives", state);
  SpotlightChoice choice;
  FindLatestSpotlightMetadata(creatives, 3, &choice);
  if (!choice.metadataPath[0] || choice.sizeCount <= 0)
    return false;

  int index = 0;
  if (choice.startKnown && choice.rotationSeconds)
  {
    FILETIME startTime = {};
    FILETIME nowTime = {};
    SYSTEMTIME now = {};
    GetSystemTime(&now);
    if (SystemTimeToFileTime(&choice.start, &startTime) &&
        SystemTimeToFileTime(&now, &nowTime))
    {
      ULARGE_INTEGER startValue = {{startTime.dwLowDateTime, startTime.dwHighDateTime}};
      ULARGE_INTEGER nowValue = {{nowTime.dwLowDateTime, nowTime.dwHighDateTime}};
      if (nowValue.QuadPart >= startValue.QuadPart)
        index = static_cast<int>(
            ((nowValue.QuadPart - startValue.QuadPart) / 10000000ULL / choice.rotationSeconds) %
            choice.sizeCount);
    }
  }
  const DWORD wantedSize = choice.sizes[index];
  wchar_t assets[kMaxPath] = {0};
  _snwprintf(assets, sizeof(assets) / sizeof(assets[0]), L"%s\\Assets", state);
  wchar_t assetsPattern[kMaxPath] = {0};
  _snwprintf(assetsPattern, sizeof(assetsPattern) / sizeof(assetsPattern[0]), L"%s\\*", assets);
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW(assetsPattern, &data);
  if (find == INVALID_HANDLE_VALUE)
    return false;
  bool found = false;
  do
  {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    ULARGE_INTEGER size = {{data.nFileSizeLow, data.nFileSizeHigh}};
    if (size.QuadPart != wantedSize)
      continue;
    wchar_t path[kMaxPath] = {0};
    _snwprintf(path, sizeof(path) / sizeof(path[0]), L"%s\\%s", assets, data.cFileName);
    found = ToUtf8(path, out, outSize);
    if (found)
      break;
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return found;
}

void ReadCurrentLockScreen()
{
  char path[kMaxPath] = {0};
  ReadRegistryString(HKEY_CURRENT_USER, "Control Panel\\Desktop", "LockScreenImage", path,
                     (int)sizeof(path));
  if (!path[0])
    ReadRegistryString(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen",
                       "LockScreenImage", path, (int)sizeof(path));
  if (!path[0])
    ReadRegistryString(HKEY_LOCAL_MACHINE,
                       "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PersonalizationCSP",
                       "LockScreenImagePath", path, (int)sizeof(path));
  if (path[0] && !FileNameExists(path))
    path[0] = 0;
  if (!path[0] && FindProtectedLockScreenImage(path, (int)sizeof(path)))
    LogEvent("lock screen source: protected Windows cache");
  if (!path[0] && FindSpotlightLockScreenImage(path, (int)sizeof(path)))
    LogEvent("lock screen source: current Spotlight cache asset");
  char previous[kMaxPath] = {0};
  EnterCriticalSection(&g_lock);
  CopyText(previous, (int)sizeof(previous), g_currentLock);
  CopyText(g_currentLock, (int)sizeof(g_currentLock), path);
  LeaveCriticalSection(&g_lock);
  if (strcmp(previous, path))
    LogEvent("lock screen image readable: %s", path[0] ? "yes" : "no");
}

struct MonitorEnumContext
{
  Display *displays;
  int count;
  int capacity;
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
  MonitorEnumContext *ctx = reinterpret_cast<MonitorEnumContext *>(data);
  if (!ctx || ctx->count >= ctx->capacity)
    return FALSE;
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info))
    return TRUE;
  DISPLAY_DEVICEW device = {};
  device.cb = sizeof(device);
  Display &out = ctx->displays[ctx->count++];
  ToUtf8(info.szDevice, out.device, (int)sizeof(out.device)); // temporary; replaced by the COM id below
  out.rect.left = info.rcMonitor.left;
  out.rect.top = info.rcMonitor.top;
  out.rect.right = info.rcMonitor.right;
  out.rect.bottom = info.rcMonitor.bottom;
  for (DWORD index = 0; index < 8; ++index)
  {
    ZeroMemory(&device, sizeof(device));
    device.cb = sizeof(device);
    if (!EnumDisplayDevicesW(info.szDevice, index, &device, 0))
      break;
    if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) && device.DeviceID[0] &&
        EdidFromDeviceId(device.DeviceID, out.edid, (int)sizeof(out.edid)))
      break;
  }
  if (out.edid[0])
    ResolveMonitorIdentity(out.edid, out.identity, (int)sizeof(out.identity));
  if (!out.identity[0])
    CopyText(out.identity, (int)sizeof(out.identity), out.device);
  return TRUE;
}

void PublishDisplays(Display *fresh, int n)
{
  qsort(fresh, (size_t)n, sizeof(fresh[0]), CompareDisplays);
  bool lockedWallpaperChanged = false;
  char globalCurrent[kMaxPath] = {0};
  wchar_t globalWide[kMaxPath] = {0};
  if (SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                            (UINT)(sizeof(globalWide) / sizeof(globalWide[0])), globalWide, 0))
    ToUtf8(globalWide, globalCurrent, (int)sizeof(globalCurrent));
  EnterCriticalSection(&g_lock);
  const DWORD now = GetTickCount();
  for (int i = 0; i < n; ++i)
  {
    MonitorSetting *setting = FindMonitorSettingLocked(fresh[i].identity);
    for (int j = 0; j < g_displayCount; ++j)
      if (!strcmp(fresh[i].identity, g_displays[j].identity))
      {
        CopyText(fresh[i].thumbnailSource, (int)sizeof(fresh[i].thumbnailSource),
                 g_displays[j].thumbnailSource);
        CopyText(fresh[i].thumbnail, (int)sizeof(fresh[i].thumbnail), g_displays[j].thumbnail);
        CopyText(fresh[i].appliedSource, (int)sizeof(fresh[i].appliedSource),
                 g_displays[j].appliedSource);
        CopyText(fresh[i].appliedStyle, (int)sizeof(fresh[i].appliedStyle),
                 g_displays[j].appliedStyle);
        const bool same = !strcmp(fresh[i].currentPath, g_displays[j].currentPath);
        const bool recentWrite =
            g_lastWallpaperApplyTick && now - g_lastWallpaperApplyTick < kExternalChangeGraceMs;
        const bool managed = setting && setting->wallpaper[0];
        const bool ownComposite =
            (g_expectedGlobalWallpaper[0] &&
             (!strcmp(fresh[i].currentPath, g_expectedGlobalWallpaper) ||
              (globalCurrent[0] && !strcmp(globalCurrent, g_expectedGlobalWallpaper)))) ||
            (g_expectedGlobalWallpaperSystem[0] &&
             (!strcmp(fresh[i].currentPath, g_expectedGlobalWallpaperSystem) ||
              (globalCurrent[0] && !strcmp(globalCurrent, g_expectedGlobalWallpaperSystem))));
        if (setting && setting->locked && fresh[i].currentPath[0] && !same && !recentWrite &&
            !ownComposite)
          lockedWallpaperChanged = true;
        if (!same && ((setting && setting->locked) || (managed && recentWrite)))
        {
          // Ignore the short interval where Windows still reports the previous image, and keep a locked display's
          // configured image/mode visible until the worker restores it.
          CopyText(fresh[i].currentPath, (int)sizeof(fresh[i].currentPath),
                   g_displays[j].currentPath);
          CopyText(fresh[i].currentStyle, (int)sizeof(fresh[i].currentStyle),
                   g_displays[j].currentStyle);
        }
        break;
      }
  }
  g_displayCount = n;
  for (int i = 0; i < n; ++i)
    g_displays[i] = fresh[i];
  LeaveCriticalSection(&g_lock);
  if (lockedWallpaperChanged)
  {
    g_restoreLockedPending = true;
    MarkApplyDirty();
    LogEvent("a locked display wallpaper changed outside Apex; restoring it");
  }
}

void CheckDisplayWallpaperChanges(IDesktopWallpaper *desk)
{
  if (!desk)
    return;
  UINT count = 0;
  if (FAILED(desk->GetMonitorDevicePathCount(&count)))
    return;
  bool lockedChanged = false;
  bool systemChanged = false;
  DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
  char style[16] = {0};
  if (SUCCEEDED(desk->GetPosition(&position)))
    CopyText(style, (int)sizeof(style), PositionId(position));
  const DWORD now = GetTickCount();
  char globalCurrent[kMaxPath] = {0};
  wchar_t globalWide[kMaxPath] = {0};
  if (SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                            (UINT)(sizeof(globalWide) / sizeof(globalWide[0])), globalWide, 0))
    ToUtf8(globalWide, globalCurrent, (int)sizeof(globalCurrent));
  for (UINT i = 0; i < count; ++i)
  {
    LPWSTR id = nullptr;
    wchar_t monitorId[kMaxDevice] = {0};
    RECT rect = {};
    if (FAILED(desk->GetMonitorDevicePathAt(i, &id)) || !id)
      continue;
    wcsncpy(monitorId, id, sizeof(monitorId) / sizeof(monitorId[0]) - 1);
    const bool matched = SUCCEEDED(desk->GetMonitorRECT(id, &rect));
    LPWSTR wallpaper = nullptr;
    char reported[kMaxPath] = {0};
    if (matched && SUCCEEDED(desk->GetWallpaper(id, &wallpaper)) && wallpaper)
      ToUtf8(wallpaper, reported, (int)sizeof(reported));
    CoTaskMemFree(wallpaper);
    CoTaskMemFree(id);
    if (!matched || !reported[0])
      continue;

    char expected[kMaxPath] = {0};
    char expectedGlobal[kMaxPath] = {0};
    char expectedGlobalSystem[kMaxPath] = {0};
    bool locked = false;
    int displayIndex = -1;
    EnterCriticalSection(&g_lock);
    for (int j = 0; j < g_displayCount; ++j)
      if (g_displays[j].rect.left == rect.left && g_displays[j].rect.top == rect.top &&
          g_displays[j].rect.right == rect.right && g_displays[j].rect.bottom == rect.bottom)
      {
        MonitorSetting *setting = FindMonitorSettingLocked(g_displays[j].identity);
        locked = setting && setting->locked;
        displayIndex = j;
        if (locked)
          CopyText(expected, (int)sizeof(expected), g_displays[j].currentPath);
        CopyText(expectedGlobal, (int)sizeof(expectedGlobal), g_expectedGlobalWallpaper);
        CopyText(expectedGlobalSystem, (int)sizeof(expectedGlobalSystem),
                 g_expectedGlobalWallpaperSystem);
        break;
      }
    LeaveCriticalSection(&g_lock);
    if (displayIndex < 0)
      continue;
    if (g_lastWallpaperApplyTick && now - g_lastWallpaperApplyTick < kExternalChangeGraceMs)
      continue;
    const bool ownComposite =
        (expectedGlobal[0] && globalCurrent[0] && !strcmp(globalCurrent, expectedGlobal)) ||
        (expectedGlobal[0] && !strcmp(reported, expectedGlobal)) ||
        (expectedGlobalSystem[0] &&
         ((globalCurrent[0] && !strcmp(globalCurrent, expectedGlobalSystem)) ||
          !strcmp(reported, expectedGlobalSystem)));
    if (locked)
    {
      if (expected[0] && strcmp(reported, expected) &&
          !ownComposite && (!expectedGlobal[0] || strcmp(reported, expectedGlobal)))
      {
        wchar_t expectedWide[kMaxPath] = {0};
        bool restored = false;
        if (FileNameExists(expected) &&
            ToWide(expected, expectedWide,
                   (int)(sizeof(expectedWide) / sizeof(expectedWide[0]))))
        {
          HRESULT hr = desk->SetPosition(DWPOS_FILL);
          if (SUCCEEDED(hr))
          {
            g_expectedSystemPosition = DWPOS_FILL;
            g_expectedSystemPositionPending = true;
            hr = desk->SetWallpaper(monitorId, expectedWide);
          }
          restored = SUCCEEDED(hr);
        }
        if (restored)
        {
          g_lastWallpaperApplyTick = GetTickCount();
          LogEvent("locked display wallpaper restored immediately");
        }
        else
          lockedChanged = true;
      }
      continue;
    }

    EnterCriticalSection(&g_lock);
    if (ownComposite)
    {
      LeaveCriticalSection(&g_lock);
      continue;
    }
    if (displayIndex < g_displayCount && strcmp(g_displays[displayIndex].currentPath, reported))
    {
      CopyText(g_displays[displayIndex].currentPath,
               (int)sizeof(g_displays[displayIndex].currentPath), reported);
      if (style[0])
        CopyText(g_displays[displayIndex].currentStyle,
                 (int)sizeof(g_displays[displayIndex].currentStyle), style);
      systemChanged = true;
    }
    LeaveCriticalSection(&g_lock);
  }
  if (lockedChanged && !g_restoreLockedPending)
  {
    g_restoreLockedPending = true;
    MarkApplyDirty();
    LogEvent("locked display wallpaper changed outside Apex; scheduling restore");
  }
  if (systemChanged)
    LogEvent("system wallpaper change detected; updating display thumbnails");
}

void ObserveSystemPosition(const char *observedId, const Display *displays, int displayCount)
{
  if (!ValidStyle(observedId))
    return;
  const DESKTOP_WALLPAPER_POSITION observed = PositionForId(observedId);
  if (!g_systemPositionKnown)
  {
    g_lastSystemPosition = observed;
    g_systemPositionKnown = true;
    return;
  }

  if (g_expectedSystemPositionPending)
  {
    if (observed == g_expectedSystemPosition)
    {
      g_lastSystemPosition = observed;
      g_expectedSystemPositionPending = false;
      return;
    }
    if (observed == g_lastSystemPosition)
      return; // Windows has not exposed our own write yet.
    g_expectedSystemPositionPending = false;
  }
  if (observed == g_lastSystemPosition)
    return;
  g_lastSystemPosition = observed;

  bool changed = false;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < displayCount; ++i)
  {
    MonitorSetting *setting = EnsureMonitorSettingLocked(displays[i].identity);
    if (!setting || setting->locked || !strcmp(setting->style, observedId))
      continue;
    CopyText(setting->style, (int)sizeof(setting->style), observedId);
    changed = true;
  }
  LeaveCriticalSection(&g_lock);

  if (changed)
  {
    SaveSettings();
    MarkApplyDirty();
    LogEvent("system wallpaper mode changed to %s; per-display settings follow it", observedId);
  }
}

bool DiscoverDisplays(IDesktopWallpaper *desk)
{
  Display fresh[kMaxDisplays] = {};
  MonitorEnumContext ctx = {fresh, 0, kMaxDisplays};
  if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&ctx)) ||
      ctx.count <= 0)
    return false;

  DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
  UINT deskCount = 0;
  bool positionObserved = false;
  char observedStyle[16] = {0};
  if (desk)
  {
    positionObserved = SUCCEEDED(desk->GetPosition(&position));
    if (positionObserved)
      CopyText(observedStyle, (int)sizeof(observedStyle), PositionId(position));
    desk->GetMonitorDevicePathCount(&deskCount);
  }
  for (int h = 0; h < ctx.count; ++h)
  {
    CopyText(fresh[h].currentStyle, (int)sizeof(fresh[h].currentStyle), PositionId(position));
    for (UINT i = 0; desk && i < deskCount; ++i)
    {
      LPWSTR id = nullptr;
      RECT rect = {0, 0, 0, 0};
      if (FAILED(desk->GetMonitorDevicePathAt(i, &id)) || !id)
        continue;
      if (SUCCEEDED(desk->GetMonitorRECT(id, &rect)) &&
          rect.left == fresh[h].rect.left && rect.top == fresh[h].rect.top &&
          rect.right == fresh[h].rect.right && rect.bottom == fresh[h].rect.bottom)
      {
        ToUtf8(id, fresh[h].device, (int)sizeof(fresh[h].device));
        LPWSTR wallpaper = nullptr;
        if (SUCCEEDED(desk->GetWallpaper(id, &wallpaper)) && wallpaper)
          ToUtf8(wallpaper, fresh[h].currentPath, (int)sizeof(fresh[h].currentPath));
        CoTaskMemFree(wallpaper);
      }
      CoTaskMemFree(id);
    }
  }
  if (!desk)
  {
    char current[kMaxPath] = {0}, style[16] = {0};
    ReadSingleDisplayState(current, (int)sizeof(current), style, (int)sizeof(style));
    for (int i = 0; i < ctx.count; ++i)
    {
      CopyText(fresh[i].currentPath, (int)sizeof(fresh[i].currentPath), current);
      CopyText(fresh[i].currentStyle, (int)sizeof(fresh[i].currentStyle), style);
    }
    positionObserved = ValidStyle(style);
    if (positionObserved)
      CopyText(observedStyle, (int)sizeof(observedStyle), style);
  }
  PublishDisplays(fresh, ctx.count);
  if (positionObserved)
  {
    if (desk && position == DWPOS_SPAN && !g_spanPositionReported)
    {
      LogEvent("system wallpaper mode is span; Wallpaper keeps its five supported modes");
      g_spanPositionReported = true;
    }
    ObserveSystemPosition(observedStyle, fresh, ctx.count);
  }
  return true;
}

void TakeSnapshot(Snapshot *out)
{
  if (!out)
    return;
  EnterCriticalSection(&g_lock);
  out->s = g_settings;
  out->displayCount = g_displayCount;
  for (int i = 0; i < g_displayCount; ++i)
    out->display[i] = g_displays[i];
  out->monitorCount = g_monitorCount;
  for (int i = 0; i < g_monitorCount; ++i)
    out->monitors[i] = g_monitors[i];
  out->latitude = g_settings.latitude;
  out->longitude = g_settings.longitude;
  out->locationKnown = g_settings.locationKnown;
  CopyText(out->currentLock, (int)sizeof(out->currentLock), g_currentLock);
  CopyText(out->currentLockThumbnail, (int)sizeof(out->currentLockThumbnail),
           g_currentLockThumbnail);
  LeaveCriticalSection(&g_lock);
}

const MonitorSetting *SnapshotMonitor(const Snapshot &s, const char *identity)
{
  for (int i = 0; i < s.monitorCount; ++i)
    if (!strcmp(s.monitors[i].identity, identity))
      return &s.monitors[i];
  return nullptr;
}

int LocalOffsetMinutes()
{
  TIME_ZONE_INFORMATION tz;
  const DWORD state = GetTimeZoneInformation(&tz);
  LONG bias = tz.Bias;
  if (state == TIME_ZONE_ID_DAYLIGHT)
    bias += tz.DaylightBias;
  else if (state == TIME_ZONE_ID_STANDARD)
    bias += tz.StandardBias;
  return static_cast<int>(-bias);
}

bool FileNameExists(const char *path)
{
  wchar_t wide[kMaxPath] = {0};
  if (!ToWide(path, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
    return false;
  const DWORD attr = GetFileAttributesW(wide);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool SystemWallpaperCache(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  char appData[kMaxPath] = {0};
  const DWORD n = GetEnvironmentVariableA("APPDATA", appData, (DWORD)sizeof(appData));
  if (n == 0 || n >= sizeof(appData))
    return false;
  const int written =
      _snprintf(out, outSize, "%s\\Microsoft\\Windows\\Themes\\TranscodedWallpaper", appData);
  return written > 0 && written < outSize && FileNameExists(out);
}

bool SystemWallpaperCacheForDisplay(int displayIndex, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  char appData[kMaxPath] = {0};
  const DWORD n = GetEnvironmentVariableA("APPDATA", appData, (DWORD)sizeof(appData));
  if (n == 0 || n >= sizeof(appData))
    return false;
  if (displayIndex <= 0)
  {
    const int written =
        _snprintf(out, outSize, "%s\\Microsoft\\Windows\\Themes\\TranscodedWallpaper", appData);
    return written > 0 && written < outSize && FileNameExists(out);
  }
  const int written = _snprintf(out, outSize,
                                "%s\\Microsoft\\Windows\\Themes\\Transcoded_%03d", appData,
                                displayIndex - 1);
  return written > 0 && written < outSize && FileNameExists(out);
}

bool SystemWallpaperCachedFile(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;
  char appData[kMaxPath] = {0};
  const DWORD n = GetEnvironmentVariableA("APPDATA", appData, (DWORD)sizeof(appData));
  if (n == 0 || n >= sizeof(appData))
    return false;
  wchar_t directory[kMaxPath] = {0};
  _snwprintf(directory, sizeof(directory) / sizeof(directory[0]),
             L"%hs\\Microsoft\\Windows\\Themes\\CachedFiles", appData);
  wchar_t pattern[kMaxPath] = {0};
  _snwprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%s\\*", directory);
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW(pattern, &data);
  if (find == INVALID_HANDLE_VALUE)
    return false;
  wchar_t best[kMaxPath] = {0};
  FILETIME bestTime = {};
  do
  {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    const wchar_t *dot = wcsrchr(data.cFileName, L'.');
    if (!dot || (_wcsicmp(dot, L".jpg") && _wcsicmp(dot, L".jpeg") &&
                 _wcsicmp(dot, L".png") && _wcsicmp(dot, L".bmp")))
      continue;
    if (!best[0] || CompareFileTime(&data.ftLastWriteTime, &bestTime) > 0)
    {
      _snwprintf(best, sizeof(best) / sizeof(best[0]), L"%s\\%s", directory, data.cFileName);
      bestTime = data.ftLastWriteTime;
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return best[0] && ToUtf8(best, out, outSize) && FileNameExists(out);
}

void WallpaperFileKey(const char *path, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return;
  out[0] = 0;
  if (!path || !path[0])
    return;
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
  {
    CopyText(out, outSize, path);
    return;
  }
  _snprintf(out, outSize, "%s|%lu|%lu|%lu|%lu", path, (unsigned long)data.nFileSizeLow,
            (unsigned long)data.nFileSizeHigh, (unsigned long)data.ftLastWriteTime.dwLowDateTime,
            (unsigned long)data.ftLastWriteTime.dwHighDateTime);
}

bool HttpGetText(const wchar_t *host, INTERNET_PORT port, const wchar_t *path, char *out, int outSize)
{
  if (!host || !path || !out || outSize <= 1)
    return false;
  out[0] = 0;
  HINTERNET session = WinHttpOpen(L"Apex Wallpaper/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    return false;
  WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);
  HINTERNET connection = WinHttpConnect(session, host, port, 0);
  HINTERNET request = nullptr;
  if (connection)
    request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 port == 443 ? WINHTTP_FLAG_SECURE : 0);
  bool ok = false;
  if (request &&
      WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr))
  {
    int off = 0;
    while (off < outSize - 1)
    {
      DWORD available = 0, read = 0;
      if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
        break;
      DWORD take = available;
      if (take > static_cast<DWORD>(outSize - 1 - off))
        take = static_cast<DWORD>(outSize - 1 - off);
      if (!WinHttpReadData(request, out + off, take, &read) || read == 0)
        break;
      off += static_cast<int>(read);
    }
    out[off] = 0;
    ok = off > 0;
  }
  if (request)
    WinHttpCloseHandle(request);
  if (connection)
    WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}

bool FetchApproxLocation(double *latitude, double *longitude)
{
  char json[16384] = {0};
  bool parsed = false;
  if (HttpGetText(L"ipapi.co", 443, L"/json/", json, (int)sizeof(json)))
    parsed = apex::wallpaper::ParseGeoJson(json, latitude, longitude);
  if (!parsed)
  {
    json[0] = 0;
    if (HttpGetText(L"ipwho.is", 443, L"/", json, (int)sizeof(json)))
      parsed = apex::wallpaper::ParseGeoJson(json, latitude, longitude);
  }
  if (!parsed)
  {
    EnterCriticalSection(&g_lock);
    CopyText(g_locationState, (int)sizeof(g_locationState), "automatic location unavailable");
    g_locationFetchedAt = GetTickCount();
    if (g_settings.locationKnown)
    {
      *latitude = g_settings.latitude;
      *longitude = g_settings.longitude;
    }
    LeaveCriticalSection(&g_lock);
    return g_settings.locationKnown;
  }
  EnterCriticalSection(&g_lock);
  g_settings.latitude = *latitude;
  g_settings.longitude = *longitude;
  g_settings.locationKnown = true;
  g_locationFetchedAt = GetTickCount();
  CopyText(g_locationState, (int)sizeof(g_locationState), "approximate location detected");
  LeaveCriticalSection(&g_lock);
  LogEvent("approximate location detected (%.4f, %.4f)", *latitude, *longitude);
  return true;
}

bool ValidStyle(const char *style)
{
  return style && (!strcmp(style, "fill") || !strcmp(style, "fit") ||
                   !strcmp(style, "stretch") || !strcmp(style, "tile") ||
                   !strcmp(style, "center"));
}

bool BuildPlan(const Snapshot &s, Plan *plan, char *whyZh, int whyZhSize, char *whyEn, int whyEnSize)
{
  if (!plan)
    return false;
  *plan = Plan();
  const Settings &cfg = s.s;
  if (s.displayCount <= 0)
  {
    CopyText(whyZh, whyZhSize, "系统还没有返回显示器布局");
    CopyText(whyEn, whyEnSize, "Windows has not returned the monitor layout yet");
    return false;
  }

  SYSTEMTIME now;
  GetLocalTime(&now);
  const int minute = now.wHour * 60 + now.wMinute;
  int sunrise = 0, sunset = 0;
  bool alwaysDay = false;
  const bool solarOk =
      s.locationKnown &&
      apex::wallpaper::SolarTimes(s.latitude, s.longitude, now.wYear, now.wMonth, now.wDay,
                                  LocalOffsetMinutes(), &sunrise, &sunset, &alwaysDay);
  const bool solarDay = solarOk && apex::wallpaper::IsSolarDay(minute, sunrise, sunset, alwaysDay);
  if (!strcmp(cfg.schedule, kScheduleInterval))
  {
    plan->variant = Variant::Interval;
  }
  else if (!strcmp(cfg.schedule, kScheduleSolar) && solarOk)
    plan->variant = solarDay ? Variant::Day : Variant::Night;
  else
    plan->variant = apex::wallpaper::InClockRange(minute, cfg.dayStart, cfg.nightStart)
                        ? Variant::Day
                        : Variant::Night;
  plan->displayCount = s.displayCount;
  int managedDisplays = 0;
  for (int i = 0; i < s.displayCount; ++i)
  {
    plan->displays[i] = s.display[i];
    const MonitorSetting *ms = SnapshotMonitor(s, s.display[i].identity);
    char base[kMaxPath] = {0};
    CopyText(base, (int)sizeof(base),
             ms && ms->wallpaper[0] ? ms->wallpaper : s.display[i].currentPath);
    if (!ms || !ms->wallpaper[0])
    {
      if (base[0])
        CopyText(plan->displayPath[i], (int)sizeof(plan->displayPath[i]), base);
      continue;
    }

    char chosen[kMaxPath] = {0};
    CopyText(chosen, (int)sizeof(chosen), base);

    if (!chosen[0])
    {
      char zh[256] = {0}, en[256] = {0};
      _snprintf(zh, sizeof(zh), "显示器 %d 还没有壁纸路径", i + 1);
      _snprintf(en, sizeof(en), "Display %d has no wallpaper path", i + 1);
      CopyText(whyZh, whyZhSize, zh);
      CopyText(whyEn, whyEnSize, en);
      return false;
    }
    if (!ResolveImageLocked(cfg, chosen, plan->displayPath[i], kMaxPath) ||
        !FileNameExists(plan->displayPath[i]))
    {
      char zh[256] = {0}, en[256] = {0};
      _snprintf(zh, sizeof(zh), "显示器 %d 的壁纸文件不存在", i + 1);
      _snprintf(en, sizeof(en), "The wallpaper for display %d is missing", i + 1);
      CopyText(whyZh, whyZhSize, zh);
      CopyText(whyEn, whyEnSize, en);
      return false;
    }

    const char *style = ms && ValidStyle(ms->style)
                            ? ms->style
                            : (ValidStyle(s.display[i].currentStyle) ? s.display[i].currentStyle
                                                                    : "fill");
    CopyText(plan->style[i], (int)sizeof(plan->style[i]), style);
    plan->managed[i] = true;
    plan->applyNeeded[i] =
        !FileNameExists(s.display[i].currentPath) ||
        strcmp(s.display[i].appliedSource, plan->displayPath[i]) ||
        strcmp(s.display[i].appliedStyle, style) ||
        (ms && ms->locked && g_restoreLockedPending);
    // Windows' per-monitor API already fills each display independently. In fill mode the source image is the final
    // image, so call the system method directly; only modes Windows cannot express per-monitor independently need
    // the plugin's full-size render.
    plan->renderNeeded[i] = plan->applyNeeded[i] && strcmp(style, "fill") != 0;
    ++managedDisplays;
  }

  const char *lockChosen = cfg.lockPath[0] ? cfg.lockPath : s.currentLock;
  plan->syncLock = cfg.lockPath[0] != 0 && g_lockApplyPending;
  if (plan->syncLock)
  {
    if (!ResolveImageLocked(cfg, lockChosen, plan->lockSource, (int)sizeof(plan->lockSource)) ||
        !FileNameExists(plan->lockSource))
    {
      CopyText(whyZh, whyZhSize, "锁屏壁纸文件不存在");
      CopyText(whyEn, whyEnSize, "The lock-screen wallpaper file is missing");
      return false;
    }
  }
  if (managedDisplays == 0 && !plan->syncLock)
  {
    CopyText(whyZh, whyZhSize, "已读取系统当前壁纸；选择壁纸后才会接管显示");
    CopyText(whyEn, whyEnSize, "Current wallpapers were read; choosing one will take over that display");
    return false;
  }

  int off = 0;
  auto append = [&](const char *text) {
    if (!text || off >= (int)sizeof(plan->signature) - 1)
      return;
    const int n = _snprintf(plan->signature + off, sizeof(plan->signature) - off, "%s", text);
    if (n > 0)
      off += n;
  };
  append(cfg.schedule);
  append(plan->syncLock ? "|lock" : "|nolock");
  for (int i = 0; i < plan->displayCount; ++i)
  {
    append("|");
    append(plan->managed[i] ? "managed" : "current");
    append("|");
    append(plan->displays[i].device);
    append(plan->displayPath[i]);
    append("|");
    append(plan->style[i]);
    const MonitorSetting *lockedSetting = SnapshotMonitor(s, plan->displays[i].identity);
    append(lockedSetting && lockedSetting->locked ? "|locked" : "|unlocked");
    char rect[96] = {0};
    _snprintf(rect, sizeof(rect), ",%ld,%ld,%ld,%ld", plan->displays[i].rect.left,
              plan->displays[i].rect.top, plan->displays[i].rect.right,
              plan->displays[i].rect.bottom);
    append(rect);
  }
  append("|");
  append(plan->lockSource);
  plan->valid = off > 0;
  return plan->valid;
}

bool GetEncoderClsid(const WCHAR *mime, CLSID *clsid)
{
  UINT count = 0, size = 0;
  if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
    return false;
  Gdiplus::ImageCodecInfo *codecs = static_cast<Gdiplus::ImageCodecInfo *>(malloc(size));
  if (!codecs)
    return false;
  bool found = false;
  if (Gdiplus::GetImageEncoders(count, size, codecs) == Gdiplus::Ok)
    for (UINT i = 0; i < count; ++i)
      if (!wcscmp(codecs[i].MimeType, mime))
      {
        *clsid = codecs[i].Clsid;
        found = true;
        break;
      }
  free(codecs);
  return found;
}

void DrawCover(Gdiplus::Graphics &graphics, Gdiplus::Image &image, const Rect &rect, double panX,
               double panY);

bool SavePng(Gdiplus::Bitmap &bitmap, const char *path, char *error, int errorSize)
{
  wchar_t wide[kMaxPath] = {0};
  if (!ToWide(path, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
    return false;
  CLSID png = {};
  if (!GetEncoderClsid(L"image/png", &png) || bitmap.Save(wide, &png, nullptr) != Gdiplus::Ok)
  {
    CopyText(error, errorSize, "the PNG image could not be saved");
    return false;
  }
  return true;
}

void PurgeOldRenders()
{
  if (!g_dir[0])
    return;
  char active[kMaxDisplays][kMaxPath] = {};
  int activeCount = 0;
  EnterCriticalSection(&g_lock);
  activeCount = g_displayCount;
  for (int i = 0; i < activeCount; ++i)
    CopyText(active[i], kMaxPath, g_displays[i].currentPath);
  LeaveCriticalSection(&g_lock);
  const char *patterns[] = {"Wallpaper.render-monitor-*.png", "Wallpaper.render-composite-*.png",
                            "Wallpaper.render-lock-*.png", "Wallpaper.monitor-1.png",
                            "Wallpaper.composite.png", "Wallpaper.lock.png",
                            "Wallpaper.thumbnail-source-*.jpg"};
  for (const char *suffix : patterns)
  {
    char pattern[kMaxPath] = {0};
    _snprintf(pattern, sizeof(pattern), "%s%s", g_dir, suffix);
    wchar_t wide[kMaxPath] = {0};
    if (!ToWide(pattern, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
      continue;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(wide, &data);
    if (find == INVALID_HANDLE_VALUE)
      continue;
    do
    {
      if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      {
        char path[kMaxPath] = {0};
        _snprintf(path, sizeof(path), "%s%ls", g_dir, data.cFileName);
        bool inUse = false;
        for (int i = 0; i < activeCount; ++i)
          if (active[i][0] && !strcmp(active[i], path))
            inUse = true;
        if (inUse)
          continue;
        wchar_t widePath[kMaxPath] = {0};
        if (ToWide(path, widePath, (int)(sizeof(widePath) / sizeof(widePath[0]))))
          DeleteFileW(widePath); // an active wallpaper can refuse; leaving it is harmless
      }
    } while (FindNextFileW(find, &data));
    FindClose(find);
  }
}

unsigned long long IdentityHash(const char *text)
{
  unsigned long long value = 1469598103934665603ULL;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); p && *p; ++p)
  {
    value ^= *p;
    value *= 1099511628211ULL;
  }
  return value;
}

bool CopyChosenWallpaper(const char *source, const char *identity, bool lockImage, char *out,
                         int outSize)
{
  if (!source || !source[0] || !out || outSize <= 0)
    return false;
  char resolved[kMaxPath] = {0};
  EnterCriticalSection(&g_lock);
  ResolveImageLocked(g_settings, source, resolved, (int)sizeof(resolved));
  LeaveCriticalSection(&g_lock);
  if (!FileNameExists(resolved) || !apex::wallpaper::HasImageExtension(resolved))
    return false;
  const char *dot = strrchr(resolved, '.');
  if (!dot)
    return false;
  const unsigned long long hash =
      IdentityHash(lockImage ? "lock-screen" : (identity && identity[0] ? identity : "display"));
  wchar_t wideSource[kMaxPath] = {0};
  if (!ToWide(resolved, wideSource, (int)(sizeof(wideSource) / sizeof(wideSource[0]))))
    return false;

  // The process-local counter restarts at zero while Windows may still hold an older wallpaper copy open.
  // DeletePreviousCopy deliberately tolerates that (the active file cannot always be removed), so a reset
  // counter can land on a name that already exists. Treat the name as a suggestion, skip every occupied one,
  // and retry the two transient Windows sharing errors instead of rejecting the user's choice.
  static volatile LONG counter = 0;
  DWORD lastError = ERROR_FILE_EXISTS;
  char target[kMaxPath] = {0};
  wchar_t wideTarget[kMaxPath] = {0};
  for (int candidate = 0; candidate < 256; ++candidate)
  {
    const int n = _snprintf(target, sizeof(target), "%sWallpaper.%s-%08llx-%lu%s", g_dir,
                            lockImage ? "lock" : "display", hash,
                            (unsigned long)InterlockedIncrement(&counter), dot);
    if (n <= 0 || n >= (int)sizeof(target))
      break;
    if (!ToWide(target, wideTarget, (int)(sizeof(wideTarget) / sizeof(wideTarget[0]))))
      break;
    if (GetFileAttributesW(wideTarget) != INVALID_FILE_ATTRIBUTES)
      continue;
    for (int retry = 0; retry < 4; ++retry)
    {
      if (CopyFileW(wideSource, wideTarget, TRUE))
      {
        CopyText(out, outSize, target);
        return true;
      }
      lastError = GetLastError();
      if (lastError == ERROR_FILE_EXISTS || lastError == ERROR_ALREADY_EXISTS)
        break;
      if (lastError == ERROR_SHARING_VIOLATION || lastError == ERROR_LOCK_VIOLATION ||
          lastError == ERROR_ACCESS_DENIED)
      {
        Sleep(40);
        continue;
      }
      candidate = 256;
      break;
    }
  }
  out[0] = 0;
  LogEvent("wallpaper copy failed for %s (win32 %lu): %s", lockImage ? "lock-screen" : identity,
           (unsigned long)lastError, resolved);
  return false;
}

void DeletePreviousCopy(const char *path, const char *keep)
{
  if (!path || !path[0] || !keep || !strcmp(path, keep))
    return;
  const bool ours = !_strnicmp(path, g_dir, strlen(g_dir)) &&
                    (strstr(path, "Wallpaper.display-") || strstr(path, "Wallpaper.lock-"));
  if (!ours)
    return;
  wchar_t wide[kMaxPath] = {0};
  if (ToWide(path, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
    DeleteFileW(wide); // the active system wallpaper may refuse; that copy stays harmlessly
}

bool EncodeThumbnailDataUri(const char *source, char *out, int outSize)
{
  if (!source || !source[0] || !out || outSize <= 0)
    return false;
  out[0] = 0;
  wchar_t wide[kMaxPath] = {0};
  if (!ToWide(source, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
    return false;
  struct StagedCopy
  {
    wchar_t path[kMaxPath] = {0};
    ~StagedCopy()
    {
      if (path[0])
        DeleteFileW(path);
    }
  } staged;
  std::unique_ptr<Gdiplus::Image> image(new Gdiplus::Image(wide));
  if (image->GetLastStatus() != Gdiplus::Ok)
  {
    image.reset();
    if (!g_dir[0])
      return false;
    char target[kMaxPath] = {0};
    static volatile LONG copySerial = 0;
    const int n = _snprintf(target, sizeof(target), "%sWallpaper.thumbnail-source-%lu.jpg", g_dir,
                            (unsigned long)InterlockedIncrement(&copySerial));
    if (n <= 0 || n >= (int)sizeof(target) ||
        !ToWide(target, staged.path, (int)(sizeof(staged.path) / sizeof(staged.path[0]))) ||
        !CopyFileW(wide, staged.path, FALSE))
    {
      staged.path[0] = 0;
      return false;
    }
    image.reset(new Gdiplus::Image(staged.path));
    if (image->GetLastStatus() != Gdiplus::Ok)
      return false;
  }
  const int width = 192, height = 108;
  Gdiplus::Bitmap thumb(width, height, PixelFormat24bppRGB);
  Gdiplus::Graphics graphics(&thumb);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  Gdiplus::SolidBrush black(Gdiplus::Color(255, 0, 0, 0));
  graphics.FillRectangle(&black, 0, 0, width, height);
  DrawCover(graphics, *image, Rect{0, 0, width, height}, 50.0, 50.0);
  IStream *stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream)
    return false;
  CLSID jpeg = {};
  if (!GetEncoderClsid(L"image/jpeg", &jpeg))
  {
    stream->Release();
    return false;
  }
  ULONG quality = 68;
  Gdiplus::EncoderParameters parameters = {};
  parameters.Count = 1;
  parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
  parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
  parameters.Parameter[0].NumberOfValues = 1;
  parameters.Parameter[0].Value = &quality;
  if (thumb.Save(stream, &jpeg, &parameters) != Gdiplus::Ok)
  {
    stream->Release();
    return false;
  }
  HGLOBAL memory = nullptr;
  if (FAILED(GetHGlobalFromStream(stream, &memory)) || !memory)
  {
    stream->Release();
    return false;
  }
  const SIZE_T bytes = GlobalSize(memory);
  const SIZE_T required = sizeof("data:image/jpeg;base64,") - 1 + ((bytes + 2) / 3) * 4 + 1;
  if (required > static_cast<SIZE_T>(outSize))
  {
    stream->Release();
    return false;
  }
  const unsigned char *data = static_cast<const unsigned char *>(GlobalLock(memory));
  if (!data)
  {
    stream->Release();
    return false;
  }
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const char prefix[] = "data:image/jpeg;base64,";
  int off = 0;
  for (const char *p = prefix; *p && off < outSize - 1; ++p)
    out[off++] = *p;
  for (SIZE_T i = 0; i < bytes && off + 4 < outSize; i += 3)
  {
    const unsigned b0 = data[i];
    const unsigned b1 = i + 1 < bytes ? data[i + 1] : 0;
    const unsigned b2 = i + 2 < bytes ? data[i + 2] : 0;
    out[off++] = alphabet[b0 >> 2];
    out[off++] = alphabet[((b0 & 3) << 4) | (b1 >> 4)];
    out[off++] = i + 1 < bytes ? alphabet[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out[off++] = i + 2 < bytes ? alphabet[b2 & 63] : '=';
  }
  out[off] = 0;
  GlobalUnlock(memory);
  stream->Release();
  return off > (int)sizeof(prefix);
}

bool CaptureCurrentWallpaperForDisplay(const char *identity, char *out, int outSize)
{
  if (!identity || !out || outSize <= 0)
    return false;
  out[0] = 0;
  Display target;
  Display displays[kMaxDisplays] = {};
  int displayCount = 0;
  EnterCriticalSection(&g_lock);
  displayCount = g_displayCount;
  for (int i = 0; i < displayCount; ++i)
  {
    displays[i] = g_displays[i];
    if (!strcmp(g_displays[i].identity, identity))
      target = g_displays[i];
  }
  LeaveCriticalSection(&g_lock);
  if (!target.identity[0] || displayCount <= 0)
    return false;

  char source[kMaxPath] = {0};
  if (!SystemWallpaperCache(source, (int)sizeof(source)))
    ReadRegistryString(HKEY_CURRENT_USER, "Control Panel\\Desktop", "WallPaper", source,
                       (int)sizeof(source));
  if (!source[0] || !FileNameExists(source))
    return false;
  wchar_t wideSource[kMaxPath] = {0};
  if (!ToWide(source, wideSource, (int)(sizeof(wideSource) / sizeof(wideSource[0]))))
    return false;
  wchar_t stagedPath[kMaxPath] = {0};
  std::unique_ptr<Gdiplus::Image> image(new Gdiplus::Image(wideSource));
  if (image->GetLastStatus() != Gdiplus::Ok)
  {
    image.reset();
    char staged[kMaxPath] = {0};
    static volatile LONG captureCopySerial = 0;
    const int n = _snprintf(staged, sizeof(staged), "%sWallpaper.thumbnail-source-%lu.jpg", g_dir,
                            (unsigned long)InterlockedIncrement(&captureCopySerial));
    if (n <= 0 || n >= (int)sizeof(staged) ||
        !ToWide(staged, stagedPath, (int)(sizeof(stagedPath) / sizeof(stagedPath[0]))) ||
        !CopyFileW(wideSource, stagedPath, FALSE))
    {
      stagedPath[0] = 0;
      return false;
    }
    image.reset(new Gdiplus::Image(stagedPath));
    if (image->GetLastStatus() != Gdiplus::Ok)
    {
      image.reset();
      DeleteFileW(stagedPath);
      return false;
    }
  }

  Rect monitorRects[kMaxDisplays] = {};
  for (int i = 0; i < displayCount; ++i)
    monitorRects[i] = displays[i].rect;
  Rect bounds = {};
  if (!apex::wallpaper::VirtualBounds(monitorRects, displayCount, &bounds))
  {
    if (stagedPath[0])
    {
      image.reset();
      DeleteFileW(stagedPath);
    }
    return false;
  }
  const int imageWidth = static_cast<int>(image->GetWidth());
  const int imageHeight = static_cast<int>(image->GetHeight());
  const int desktopWidth = static_cast<int>(bounds.right - bounds.left);
  const int desktopHeight = static_cast<int>(bounds.bottom - bounds.top);
  const int monitorWidth = static_cast<int>(target.rect.right - target.rect.left);
  const int monitorHeight = static_cast<int>(target.rect.bottom - target.rect.top);
  if (imageWidth <= 0 || imageHeight <= 0 || desktopWidth <= 0 || desktopHeight <= 0 ||
      monitorWidth <= 0 || monitorHeight <= 0)
  {
    if (stagedPath[0])
    {
      image.reset();
      DeleteFileW(stagedPath);
    }
    return false;
  }

  int sourceX = 0, sourceY = 0, sourceW = imageWidth, sourceH = imageHeight;
  const double monitorAspect = static_cast<double>(monitorWidth) / monitorHeight;
  const double desktopAspect = static_cast<double>(desktopWidth) / desktopHeight;
  const double imageAspect = static_cast<double>(imageWidth) / imageHeight;
  if (imageWidth == monitorWidth && imageHeight == monitorHeight)
  {
    // TranscodedWallpaper is already this monitor's image.
  }
  else if (imageWidth == desktopWidth && imageHeight == desktopHeight)
  {
    sourceX = static_cast<int>(target.rect.left - bounds.left);
    sourceY = static_cast<int>(target.rect.top - bounds.top);
    sourceW = monitorWidth;
    sourceH = monitorHeight;
  }
  else if (std::fabs(monitorAspect - imageAspect) <= 0.02)
  {
    // A primary-monitor cache is often screen-sized while the virtual desktop also contains monitors left/above it.
  }
  else
  {
    if (std::fabs(desktopAspect - imageAspect) > 0.02)
    {
      if (stagedPath[0])
      {
        image.reset();
        DeleteFileW(stagedPath);
      }
      return false;
    }
    sourceX = static_cast<int>((target.rect.left - bounds.left) *
                               static_cast<long long>(imageWidth) / desktopWidth);
    sourceY = static_cast<int>((target.rect.top - bounds.top) *
                               static_cast<long long>(imageHeight) / desktopHeight);
    sourceW = static_cast<int>(static_cast<long long>(monitorWidth) * imageWidth / desktopWidth);
    sourceH = static_cast<int>(static_cast<long long>(monitorHeight) * imageHeight / desktopHeight);
  }
  Gdiplus::Bitmap bitmap(monitorWidth, monitorHeight, PixelFormat32bppARGB);
  Gdiplus::Graphics graphics(&bitmap);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  graphics.DrawImage(image.get(), Gdiplus::Rect(0, 0, monitorWidth, monitorHeight), sourceX,
                     sourceY, sourceW, sourceH, Gdiplus::UnitPixel);
  _snprintf(out, outSize, "%sWallpaper.display-%016llx-capture-%lu.png", g_dir,
            IdentityHash(identity), (unsigned long)InterlockedIncrement(&g_renderSerial));
  char error[128] = {0};
  const bool saved = SavePng(bitmap, out, error, (int)sizeof(error));
  image.reset();
  if (stagedPath[0])
    DeleteFileW(stagedPath);
  if (!saved)
    out[0] = 0;
  return saved;
}

void DrawCover(Gdiplus::Graphics &graphics, Gdiplus::Image &image, const Rect &rect, double panX,
               double panY)
{
  const int destW = rect.right - rect.left;
  const int destH = rect.bottom - rect.top;
  const int srcW = static_cast<int>(image.GetWidth());
  const int srcH = static_cast<int>(image.GetHeight());
  if (destW <= 0 || destH <= 0 || srcW <= 0 || srcH <= 0)
    return;
  double sx = 0.0, sy = 0.0, sw = srcW, sh = srcH;
  const double srcAspect = static_cast<double>(srcW) / srcH;
  const double destAspect = static_cast<double>(destW) / destH;
  if (srcAspect > destAspect)
  {
    sw = srcH * destAspect;
    sx = (srcW - sw) * (std::max(0.0, std::min(100.0, panX)) / 100.0);
  }
  else
  {
    sh = srcW / destAspect;
    sy = (srcH - sh) * (std::max(0.0, std::min(100.0, panY)) / 100.0);
  }
  graphics.SetClip(Gdiplus::Rect(rect.left, rect.top, destW, destH));
  graphics.DrawImage(&image, Gdiplus::Rect(rect.left, rect.top, destW, destH),
                     static_cast<INT>(sx), static_cast<INT>(sy), static_cast<INT>(sw),
                     static_cast<INT>(sh), Gdiplus::UnitPixel);
  graphics.ResetClip();
}

void DrawContained(Gdiplus::Graphics &graphics, Gdiplus::Image &image, const Rect &rect)
{
  const int destW = rect.right - rect.left;
  const int destH = rect.bottom - rect.top;
  const int srcW = static_cast<int>(image.GetWidth());
  const int srcH = static_cast<int>(image.GetHeight());
  if (destW <= 0 || destH <= 0 || srcW <= 0 || srcH <= 0)
    return;
  const double scale = std::min(static_cast<double>(destW) / srcW,
                                static_cast<double>(destH) / srcH);
  const int drawW = static_cast<int>(srcW * scale);
  const int drawH = static_cast<int>(srcH * scale);
  const int left = rect.left + (destW - drawW) / 2;
  const int top = rect.top + (destH - drawH) / 2;
  graphics.SetClip(Gdiplus::Rect(rect.left, rect.top, destW, destH));
  graphics.DrawImage(&image, left, top, drawW, drawH);
  graphics.ResetClip();
}

void DrawTiled(Gdiplus::Graphics &graphics, Gdiplus::Image &image, const Rect &rect)
{
  const int destW = rect.right - rect.left;
  const int destH = rect.bottom - rect.top;
  const int srcW = static_cast<int>(image.GetWidth());
  const int srcH = static_cast<int>(image.GetHeight());
  if (destW <= 0 || destH <= 0 || srcW <= 0 || srcH <= 0)
    return;
  graphics.SetClip(Gdiplus::Rect(rect.left, rect.top, destW, destH));
  for (int y = rect.top; y < rect.bottom; y += srcH)
    for (int x = rect.left; x < rect.right; x += srcW)
      graphics.DrawImage(&image, x, y, srcW, srcH);
  graphics.ResetClip();
}

bool RenderDisplayImage(const Plan &plan, int index, char *outPath, int outSize, char *error,
                        int errorSize)
{
  const int width = plan.displays[index].rect.right - plan.displays[index].rect.left;
  const int height = plan.displays[index].rect.bottom - plan.displays[index].rect.top;
  if (width <= 0 || height <= 0)
    return false;
  wchar_t source[kMaxPath] = {0};
  if (!ToWide(plan.displayPath[index], source, (int)(sizeof(source) / sizeof(source[0]))))
    return false;
  Gdiplus::Image image(source);
  if (image.GetLastStatus() != Gdiplus::Ok)
  {
    CopyText(error, errorSize, "a display wallpaper image could not be decoded");
    return false;
  }
  Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
  Gdiplus::Graphics graphics(&bitmap);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
  Gdiplus::SolidBrush background(Gdiplus::Color(255, 0, 0, 0));
  graphics.FillRectangle(&background, 0, 0, width, height);
  const Rect box = {0, 0, width, height};
  if (!strcmp(plan.style[index], "fit"))
    DrawContained(graphics, image, box);
  else if (!strcmp(plan.style[index], "stretch"))
    graphics.DrawImage(&image, 0, 0, width, height);
  else if (!strcmp(plan.style[index], "tile"))
    DrawTiled(graphics, image, box);
  else if (!strcmp(plan.style[index], "center"))
    graphics.DrawImage(&image, (width - (int)image.GetWidth()) / 2,
                       (height - (int)image.GetHeight()) / 2, (int)image.GetWidth(),
                       (int)image.GetHeight());
  else
    DrawCover(graphics, image, box, 50.0, 50.0);
  _snprintf(outPath, outSize, "%sWallpaper.render-monitor-%d-%lu.png", g_dir, index + 1,
            (unsigned long)InterlockedIncrement(&g_renderSerial));
  return SavePng(bitmap, outPath, error, errorSize);
}

bool PrepareDisplayImages(Plan *plan, char *error, int errorSize)
{
  if (!plan)
    return false;
  for (int i = 0; i < plan->displayCount; ++i)
  {
    // A display the feature has not taken over is read-only: BuildPlan keeps its current system path so the page
    // can show a thumbnail, but that path may be a protected slideshow image or a legacy transcoded file that
    // GDI+ cannot decode. Trying to render it made every newly attached unconfigured monitor fail the whole apply
    // cycle once a second and starved the page's live thumbnail update.
    if (!plan->managed[i] || !plan->renderNeeded[i] || !plan->displayPath[i][0])
      continue;
    if (!RenderDisplayImage(*plan, i, plan->renderPath[i], (int)sizeof(plan->renderPath[i]), error,
                            errorSize))
      return false;
  }
  return true;
}

void RefreshThumbnails()
{
  Display displays[kMaxDisplays] = {};
  MonitorSetting monitors[kMaxMonitorSettings] = {};
  int displayCount = 0, monitorCount = 0;
  char lockPath[kMaxPath] = {0};
  EnterCriticalSection(&g_lock);
  displayCount = g_displayCount;
  monitorCount = g_monitorCount;
  for (int i = 0; i < displayCount; ++i)
    displays[i] = g_displays[i];
  for (int i = 0; i < monitorCount; ++i)
    monitors[i] = g_monitors[i];
  CopyText(lockPath, (int)sizeof(lockPath), g_currentLock);
  LeaveCriticalSection(&g_lock);
  char cachePath[kMaxPath] = {0};
  SystemWallpaperCache(cachePath, (int)sizeof(cachePath));
  char perDisplayCache[kMaxDisplays][kMaxPath] = {};
  for (int i = 0; i < displayCount; ++i)
    SystemWallpaperCacheForDisplay(i, perDisplayCache[i], (int)sizeof(perDisplayCache[i]));
  char cachedFile[kMaxPath] = {0};
  SystemWallpaperCachedFile(cachedFile, (int)sizeof(cachedFile));
  char registryPath[kMaxPath] = {0};
  ReadRegistryString(HKEY_CURRENT_USER, "Control Panel\\Desktop", "WallPaper", registryPath,
                     (int)sizeof(registryPath));
  char history[4][kMaxPath] = {};
  for (int i = 0; i < 4; ++i)
  {
    char valueName[32] = {0};
    _snprintf(valueName, sizeof(valueName), "BackgroundHistoryPath%d", i);
    ReadRegistryString(HKEY_CURRENT_USER,
                       "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers",
                       valueName, history[i], (int)sizeof(history[i]));
  }

  for (int i = 0; i < displayCount; ++i)
  {
    const MonitorSetting *setting = nullptr;
    for (int j = 0; j < monitorCount; ++j)
      if (!strcmp(monitors[j].identity, displays[i].identity))
      {
        setting = &monitors[j];
        break;
      }
    const char *defaultWallpaper = "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    const char *candidates[11] = {
        displays[i].currentPath[0] ? displays[i].currentPath : nullptr,
        setting && setting->wallpaper[0] ? setting->wallpaper : nullptr,
        perDisplayCache[i][0] ? perDisplayCache[i] : nullptr,
        cachePath[0] ? cachePath : nullptr,
        cachedFile[0] ? cachedFile : nullptr,
        history[0][0] ? history[0] : nullptr,
        history[1][0] ? history[1] : nullptr,
        history[2][0] ? history[2] : nullptr,
        history[3][0] ? history[3] : nullptr,
        registryPath[0] ? registryPath : nullptr,
        FileNameExists(defaultWallpaper) ? defaultWallpaper : nullptr};
    char keyStorage[11][kMaxPath + 96] = {};
    const char *keys[11] = {};
    for (int c = 0; c < 11; ++c)
      if (candidates[c])
      {
        WallpaperFileKey(candidates[c], keyStorage[c], (int)sizeof(keyStorage[c]));
        keys[c] = keyStorage[c][0] ? keyStorage[c] : candidates[c];
      }
    char thumbnail[sizeof(displays[i].thumbnail)] = {0};
    const char *used = nullptr;
    const char *usedKey = nullptr;
    bool keepsCurrent = false;
    for (int c = 0; c < 11; ++c)
      if (candidates[c] && keys[c])
      {
        if (displays[i].thumbnail[0] && !strcmp(keys[c], displays[i].thumbnailSource))
        {
          keepsCurrent = true;
          break;
        }
        if (!EncodeThumbnailDataUri(candidates[c], thumbnail, (int)sizeof(thumbnail)))
          continue;
        used = candidates[c];
        usedKey = keys[c];
        break;
      }
    if (keepsCurrent)
      continue;
    if (!used)
    {
      static DWORD lastFailureLog = 0;
      const DWORD failedAt = GetTickCount();
      if (!lastFailureLog || failedAt - lastFailureLog >= 30000)
      {
        LogEvent("thumbnail unavailable for monitor %s (system=%s, per-display-cache=%s, cache=%s, "
                 "cached-file=%s, registry=%s)",
                 displays[i].identity, displays[i].currentPath[0] ? displays[i].currentPath : "none",
                 perDisplayCache[i][0] ? perDisplayCache[i] : "none",
                 cachePath[0] ? cachePath : "none", cachedFile[0] ? cachedFile : "none",
                 registryPath[0] ? registryPath : "none");
        lastFailureLog = failedAt;
      }
      continue;
    }
    EnterCriticalSection(&g_lock);
    for (int j = 0; j < g_displayCount; ++j)
      if (!strcmp(g_displays[j].identity, displays[i].identity))
      {
        CopyText(g_displays[j].thumbnailSource, (int)sizeof(g_displays[j].thumbnailSource),
                 usedKey);
        CopyText(g_displays[j].thumbnail, (int)sizeof(g_displays[j].thumbnail), thumbnail);
        break;
      }
    LeaveCriticalSection(&g_lock);
  }

  EnterCriticalSection(&g_lock);
  CopyText(lockPath, (int)sizeof(lockPath), g_currentLock);
  const bool lockNeedsThumbnail = lockPath[0] && strcmp(lockPath, g_lockThumbnailSource);
  LeaveCriticalSection(&g_lock);
  if (lockNeedsThumbnail)
  {
    char thumbnail[sizeof(g_currentLockThumbnail)] = {0};
    if (EncodeThumbnailDataUri(lockPath, thumbnail, (int)sizeof(thumbnail)))
    {
      EnterCriticalSection(&g_lock);
      CopyText(g_lockThumbnailSource, (int)sizeof(g_lockThumbnailSource), lockPath);
      CopyText(g_currentLockThumbnail, (int)sizeof(g_currentLockThumbnail), thumbnail);
      LeaveCriticalSection(&g_lock);
    }
  }
}

bool ComposeWallpaper(Plan &plan, char *outPath, int outSize, char *error, int errorSize)
{
  if (plan.displayCount <= 0)
    return false;
  Rect canvas, monitorRects[kMaxDisplays];
  for (int i = 0; i < plan.displayCount; ++i)
    monitorRects[i] = plan.displays[i].rect;
  if (!apex::wallpaper::VirtualBounds(monitorRects, plan.displayCount, &canvas))
    return false;
  const int width = canvas.right - canvas.left;
  const int height = canvas.bottom - canvas.top;
  if (width <= 0 || height <= 0 || width > 32767 || height > 32767 ||
      static_cast<long long>(width) * height > 160000000LL)
  {
    CopyText(error, errorSize, "virtual desktop is too large to compose");
    return false;
  }
  Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
  Gdiplus::Graphics graphics(&bitmap);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  Gdiplus::SolidBrush background(Gdiplus::Color(255, 0, 0, 0));
  graphics.FillRectangle(&background, 0, 0, width, height);
  for (int i = 0; i < plan.displayCount; ++i)
  {
    if (!plan.renderPath[i][0] && plan.displays[i].currentPath[0])
      CopyText(plan.renderPath[i], kMaxPath, plan.displays[i].currentPath);
    if (!plan.renderPath[i][0])
    {
      CopyText(error, errorSize, "a display has no image for the virtual desktop composite");
      return false;
    }
    wchar_t source[kMaxPath] = {0};
    if (!ToWide(plan.renderPath[i], source, (int)(sizeof(source) / sizeof(source[0]))))
      return false;
    Gdiplus::Image image(source);
    if (image.GetLastStatus() != Gdiplus::Ok)
      return false;
    const Rect placed = apex::wallpaper::PlaceInCanvas(plan.displays[i].rect, canvas);
    graphics.DrawImage(&image, static_cast<INT>(placed.left), static_cast<INT>(placed.top),
                       static_cast<INT>(placed.right - placed.left),
                       static_cast<INT>(placed.bottom - placed.top));
  }
  _snprintf(outPath, outSize, "%sWallpaper.render-composite-%lu.png", g_dir,
            (unsigned long)InterlockedIncrement(&g_renderSerial));
  return SavePng(bitmap, outPath, error, errorSize);
}

bool PrepareLockImage(const char *source, char *outPath, int outSize, char *error, int errorSize)
{
  wchar_t wideIn[kMaxPath] = {0};
  if (!ToWide(source, wideIn, (int)(sizeof(wideIn) / sizeof(wideIn[0]))))
    return false;
  Gdiplus::Image image(wideIn);
  if (image.GetLastStatus() != Gdiplus::Ok)
  {
    CopyText(error, errorSize, "the lock-screen image could not be decoded");
    return false;
  }
  _snprintf(outPath, outSize, "%sWallpaper.render-lock-%lu.png", g_dir,
            (unsigned long)InterlockedIncrement(&g_renderSerial));
  wchar_t wideOut[kMaxPath] = {0};
  if (!ToWide(outPath, wideOut, (int)(sizeof(wideOut) / sizeof(wideOut[0]))))
    return false;
  CLSID png = {};
  if (!GetEncoderClsid(L"image/png", &png))
    return false;
  if (image.Save(wideOut, &png, nullptr) != Gdiplus::Ok)
  {
    CopyText(error, errorSize, "the lock-screen PNG could not be saved");
    return false;
  }
  return true;
}

HRESULT WaitForAsync(IInspectable *operation, DWORD timeoutMs, HRESULT *operationError)
{
  if (!operation)
    return E_POINTER;
  IAsyncInfo *info = nullptr;
  HRESULT hr = operation->QueryInterface(apex::wallpaper::winrt::kIIDAsyncInfo,
                                         reinterpret_cast<void **>(&info));
  if (FAILED(hr) || !info)
    return FAILED(hr) ? hr : E_NOINTERFACE;
  const DWORD start = GetTickCount();
  hr = E_PENDING;
  while (!g_stop && GetTickCount() - start < timeoutMs)
  {
    AsyncStatus state = Started;
    if (FAILED(info->get_Status(&state)))
      break;
    if (state == Completed)
    {
      hr = S_OK;
      break;
    }
    if (state == Canceled)
    {
      hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
      break;
    }
    if (state == Error)
    {
      HRESULT detail = E_FAIL;
      info->get_ErrorCode(&detail);
      if (operationError)
        *operationError = detail;
      hr = detail;
      break;
    }
    Sleep(20);
  }
  info->Release();
  if (hr == E_PENDING)
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  return hr;
}

bool QueryLockScreenSupport()
{
  const apex::wallpaper::winrt::CombaseApi &api = apex::wallpaper::winrt::GetCombaseApi();
  if (!api.ok())
    return false;
  HSTRING className = nullptr;
  HRESULT hr = api.windowsCreateString(
      apex::wallpaper::winrt::kPersonalizationClass,
      static_cast<UINT32>(wcslen(apex::wallpaper::winrt::kPersonalizationClass)), &className);
  if (FAILED(hr))
    return false;
  IPersonalizationStatics *statics = nullptr;
  hr = api.roGetActivationFactory(className, apex::wallpaper::winrt::kIIDPersonalizationStatics,
                                  reinterpret_cast<void **>(&statics));
  api.windowsDeleteString(className);
  if (FAILED(hr) || !statics)
  {
    EnterCriticalSection(&g_lock);
    g_lockChecked = true;
    g_lockSupported = false;
    LeaveCriticalSection(&g_lock);
    return false;
  }
  apex::wallpaper::winrt::Boolean supported = 0;
  hr = statics->IsSupported(&supported);
  statics->Release();
  EnterCriticalSection(&g_lock);
  g_lockChecked = true;
  g_lockSupported = SUCCEEDED(hr) && supported != 0;
  LeaveCriticalSection(&g_lock);
  return SUCCEEDED(hr);
}

bool SetLockScreenImage(const char *path)
{
  const apex::wallpaper::winrt::CombaseApi &api = apex::wallpaper::winrt::GetCombaseApi();
  if (!api.ok())
  {
    LogEvent("lock screen: combase.dll WinRT exports are unavailable");
    return false;
  }
  wchar_t wide[kMaxPath] = {0};
  if (!ToWide(path, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
    return false;

  HSTRING className = nullptr;
  HRESULT hr = api.windowsCreateString(
      apex::wallpaper::winrt::kPersonalizationClass,
      static_cast<UINT32>(wcslen(apex::wallpaper::winrt::kPersonalizationClass)), &className);
  if (FAILED(hr))
    return false;
  IPersonalizationStatics *statics = nullptr;
  hr = api.roGetActivationFactory(className, apex::wallpaper::winrt::kIIDPersonalizationStatics,
                              reinterpret_cast<void **>(&statics));
  api.windowsDeleteString(className);
  if (FAILED(hr) || !statics)
  {
    LogEvent("lock screen: personalization factory unavailable (0x%08lx)", (unsigned long)hr);
    return false;
  }

  apex::wallpaper::winrt::Boolean supported = 0;
  hr = statics->IsSupported(&supported);
  EnterCriticalSection(&g_lock);
  g_lockChecked = true;
  g_lockSupported = SUCCEEDED(hr) && supported != 0;
  LeaveCriticalSection(&g_lock);
  if (FAILED(hr) || !supported)
  {
    statics->Release();
    return false;
  }

  IPersonalizationSettings *settings = nullptr;
  hr = statics->get_Current(&settings);
  statics->Release();
  if (FAILED(hr) || !settings)
    return false;

  HSTRING storageName = nullptr;
  if (FAILED(api.windowsCreateString(
          apex::wallpaper::winrt::kStorageFileClass,
          static_cast<UINT32>(wcslen(apex::wallpaper::winrt::kStorageFileClass)),
          &storageName)))
  {
    settings->Release();
    return false;
  }
  IStorageFileStatics *storageStatics = nullptr;
  hr = api.roGetActivationFactory(storageName, apex::wallpaper::winrt::kIIDStorageFileStatics,
                              reinterpret_cast<void **>(&storageStatics));
  api.windowsDeleteString(storageName);
  if (FAILED(hr) || !storageStatics)
  {
    settings->Release();
    return false;
  }

  HSTRING hpath = nullptr;
  if (FAILED(api.windowsCreateString(wide, static_cast<UINT32>(wcslen(wide)), &hpath)))
  {
    storageStatics->Release();
    settings->Release();
    return false;
  }
  IStorageFileOperation *fileOp = nullptr;
  hr = storageStatics->GetFileFromPathAsync(hpath, &fileOp);
  api.windowsDeleteString(hpath);
  storageStatics->Release();
  if (FAILED(hr) || !fileOp)
  {
    settings->Release();
    return false;
  }
  HRESULT asyncError = S_OK;
  hr = WaitForAsync(reinterpret_cast<IInspectable *>(fileOp), 10000, &asyncError);
  IUnknown *file = nullptr;
  if (SUCCEEDED(hr))
    hr = fileOp->GetResults(&file);
  fileOp->Release();
  if (FAILED(hr) || !file)
  {
    settings->Release();
    LogEvent("lock screen: StorageFile failed (0x%08lx, inner 0x%08lx)", (unsigned long)hr,
             (unsigned long)asyncError);
    return false;
  }

  // The current public API is Windows.System.UserProfile.LockScreen::SetImageFileAsync. Try it first and keep
  // the older UserProfilePersonalizationSettings call as a compatibility fallback for systems where the newer
  // factory is unavailable. Lock screens are global by design; there is no monitorID argument here.
  HSTRING lockClassName = nullptr;
  apex::wallpaper::winrt::ILockScreenStatics *lockStatics = nullptr;
  hr = api.windowsCreateString(
      apex::wallpaper::winrt::kLockScreenClass,
      static_cast<UINT32>(wcslen(apex::wallpaper::winrt::kLockScreenClass)), &lockClassName);
  if (SUCCEEDED(hr))
    hr = api.roGetActivationFactory(lockClassName, apex::wallpaper::winrt::kIIDLockScreenStatics,
                                    reinterpret_cast<void **>(&lockStatics));
  if (lockClassName)
    api.windowsDeleteString(lockClassName);
  if (SUCCEEDED(hr) && lockStatics)
  {
    apex::wallpaper::winrt::IAsyncAction *action = nullptr;
    hr = lockStatics->SetImageFileAsync(file, &action);
    if (SUCCEEDED(hr) && action)
    {
      asyncError = S_OK;
      hr = WaitForAsync(reinterpret_cast<IInspectable *>(action), 20000, &asyncError);
      if (SUCCEEDED(hr))
        hr = action->GetResults();
      action->Release();
    }
    lockStatics->Release();
    if (SUCCEEDED(hr))
    {
      file->Release();
      settings->Release();
      LogEvent("lock screen image set via LockScreen::SetImageFileAsync: %s", path);
      return true;
    }
    LogEvent("lock screen LockScreen API failed (0x%08lx); trying legacy interface", (unsigned long)hr);
  }

  IBooleanOperation *resultOp = nullptr;
  hr = settings->TrySetLockScreenImageAsync(file, &resultOp);
  if (SUCCEEDED(hr) && resultOp)
  {
    asyncError = S_OK;
    hr = WaitForAsync(reinterpret_cast<IInspectable *>(resultOp), 20000, &asyncError);
    apex::wallpaper::winrt::Boolean accepted = 0;
    if (SUCCEEDED(hr))
      hr = resultOp->GetResults(&accepted);
    if (SUCCEEDED(hr) && !accepted)
      hr = E_FAIL;
    resultOp->Release();
  }
  file->Release();
  settings->Release();
  LogEvent("lock screen image %s: %s (0x%08lx)", path, SUCCEEDED(hr) ? "set" : "failed",
           (unsigned long)hr);
  return SUCCEEDED(hr);
}

bool ApplyDesktopWallpaperFallback(const Plan &plan, char *error, int errorSize)
{
  // This is the compatibility fallback when the public per-monitor API cannot apply the change: render every managed
  // display to its exact monitor size, place those images on one virtual-desktop canvas, and let Windows apply that
  // composite through its traditional wallpaper entry point.
  Plan composed = plan;
  for (int i = 0; i < composed.displayCount; ++i)
  {
    if (!composed.managed[i] || !composed.displayPath[i][0])
      continue;
    if (!RenderDisplayImage(composed, i, composed.renderPath[i],
                            (int)sizeof(composed.renderPath[i]), error, errorSize))
      return false;
  }
  char fallback[kMaxPath] = {0};
  if (!ComposeWallpaper(composed, fallback, (int)sizeof(fallback), error, errorSize))
    return false;
  wchar_t wide[kMaxPath] = {0};
  if (!ToWide(fallback, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
    return false;
  IDesktopWallpaper *desk = nullptr;
  HRESULT positionResult = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                            IID_IDesktopWallpaper,
                                            reinterpret_cast<void **>(&desk));
  if (SUCCEEDED(positionResult) && desk)
  {
    const HRESULT spanResult = desk->SetPosition(DWPOS_SPAN);
    if (FAILED(spanResult))
      LogEvent("composite wallpaper could not select span mode (0x%08lx)",
               (unsigned long)spanResult);
    desk->Release();
  }
  if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wide,
                             SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
  {
    _snprintf(error, errorSize, "the wallpaper fallback failed (%lu)",
              (unsigned long)GetLastError());
    return false;
  }
  char systemPath[kMaxPath] = {0};
  wchar_t systemPathWide[kMaxPath] = {0};
  if (SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                            (UINT)(sizeof(systemPathWide) / sizeof(systemPathWide[0])),
                            systemPathWide, 0))
    ToUtf8(systemPathWide, systemPath, (int)sizeof(systemPath));
  EnterCriticalSection(&g_lock);
  CopyText(g_expectedGlobalWallpaper, (int)sizeof(g_expectedGlobalWallpaper), fallback);
  CopyText(g_expectedGlobalWallpaperSystem, (int)sizeof(g_expectedGlobalWallpaperSystem),
           systemPath[0] ? systemPath : fallback);
  LeaveCriticalSection(&g_lock);
  LogEvent("desktop composite used: %s", fallback);
  return true;
}

bool ApplyDesktopWallpaper(const Plan &plan, char *error, int errorSize)
{
  bool anyChanged = false;
  for (int i = 0; i < plan.displayCount; ++i)
    if (plan.managed[i] && plan.applyNeeded[i])
      anyChanged = true;
  if (!anyChanged)
    return true;
  LogEvent("desktop system apply requested");
  // On this machine IDesktopWallpaper accepts SetWallpaper (S_OK) but ASUS OLED Shifter immediately keeps the
  // system path, so the desktop does not change. The composed SystemParametersInfo path is the one that actually
  // takes effect here; keep the per-monitor IDesktopWallpaper code below as the fallback for other environments.
  if (ApplyDesktopWallpaperFallback(plan, error, errorSize))
    return true;
  LogEvent("desktop composite failed; trying per-monitor IDesktopWallpaper");
  IDesktopWallpaper *desk = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                IID_IDesktopWallpaper, reinterpret_cast<void **>(&desk));
  if (FAILED(hr) || !desk)
  {
    if (!ApplyDesktopWallpaperFallback(plan, error, errorSize))
    {
      if (!error[0])
        _snprintf(error, errorSize, "IDesktopWallpaper unavailable (0x%08lx)",
                  (unsigned long)hr);
      return false;
    }
    return true;
  }

  UINT monitorCount = 0;
  hr = desk->GetMonitorDevicePathCount(&monitorCount);
  for (int i = 0; i < plan.displayCount && SUCCEEDED(hr); ++i)
  {
    if (!plan.managed[i] || !plan.applyNeeded[i])
      continue;
    LPWSTR monitorID = nullptr;
    for (UINT j = 0; j < monitorCount; ++j)
    {
      LPWSTR candidate = nullptr;
      RECT rect = {};
      if (FAILED(desk->GetMonitorDevicePathAt(j, &candidate)) || !candidate)
        continue;
      if (SUCCEEDED(desk->GetMonitorRECT(candidate, &rect)) &&
          rect.left == plan.displays[i].rect.left && rect.top == plan.displays[i].rect.top &&
          rect.right == plan.displays[i].rect.right &&
          rect.bottom == plan.displays[i].rect.bottom)
      {
        monitorID = candidate;
        break;
      }
      CoTaskMemFree(candidate);
    }
    wchar_t cachedDevice[kMaxDevice] = {0};
    if (!monitorID &&
        ToWide(plan.displays[i].device, cachedDevice,
               (int)(sizeof(cachedDevice) / sizeof(cachedDevice[0]))))
      monitorID = cachedDevice;
    wchar_t path[kMaxPath] = {0};
    if (!monitorID ||
        !ToWide(plan.renderPath[i], path, (int)(sizeof(path) / sizeof(path[0]))))
    {
      if (monitorID != cachedDevice)
        CoTaskMemFree(monitorID);
      hr = E_INVALIDARG;
      break;
    }
    hr = desk->SetWallpaper(monitorID, path);
    LogEvent("desktop system SetWallpaper(%s): 0x%08lx",
             plan.displays[i].identity, (unsigned long)hr);
    if (monitorID != cachedDevice)
      CoTaskMemFree(monitorID);
  }
  desk->Release();
  if (FAILED(hr))
  {
    _snprintf(error, errorSize, "IDesktopWallpaper refused the change (0x%08lx)",
              (unsigned long)hr);
    return ApplyDesktopWallpaperFallback(plan, error, errorSize);
  }
  LogEvent("desktop system apply completed");
  return true;
}

bool ApplyPlan(Plan *plan)
{
  char error[256] = {0};
  bool anyApplyNeeded = false;
  bool anyRenderNeeded = false;
  for (int i = 0; i < plan->displayCount; ++i)
  {
    if (plan->managed[i] && plan->applyNeeded[i])
    {
      anyApplyNeeded = true;
      if (!plan->renderNeeded[i])
        CopyText(plan->renderPath[i], (int)sizeof(plan->renderPath[i]),
                 plan->displayPath[i]);
    }
    if (plan->managed[i] && plan->renderNeeded[i])
      anyRenderNeeded = true;
    else if (plan->managed[i])
      CopyText(plan->renderPath[i], (int)sizeof(plan->renderPath[i]),
               plan->displays[i].currentPath);
  }
  if (anyRenderNeeded && !PrepareDisplayImages(plan, error, (int)sizeof(error)))
  {
    LogEvent("display render failed: %s", error);
    return false;
  }

  if (g_noApply)
  {
    return true;
  }
  if (anyApplyNeeded && !ApplyDesktopWallpaper(*plan, error, (int)sizeof(error)))
  {
    LogEvent("desktop failed: %s", error);
    return false;
  }
  if (anyApplyNeeded)
    g_lastWallpaperApplyTick = GetTickCount();
  char thumbnails[kMaxDisplays][kThumbnailDataSize] = {};
  bool thumbnailReady[kMaxDisplays] = {};
  for (int i = 0; i < plan->displayCount; ++i)
    if (plan->managed[i] && plan->applyNeeded[i] && plan->renderPath[i][0])
      thumbnailReady[i] =
          EncodeThumbnailDataUri(plan->renderPath[i], thumbnails[i], (int)sizeof(thumbnails[i]));
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < plan->displayCount; ++i)
  {
    if (!plan->managed[i] || !plan->applyNeeded[i])
      continue;
    for (int j = 0; j < g_displayCount; ++j)
      if (!strcmp(plan->displays[i].identity, g_displays[j].identity))
      {
        CopyText(g_displays[j].currentPath, kMaxPath, plan->renderPath[i]);
        CopyText(g_displays[j].currentStyle, (int)sizeof(g_displays[j].currentStyle),
                 plan->style[i]);
        CopyText(g_displays[j].appliedSource, (int)sizeof(g_displays[j].appliedSource),
                 plan->displayPath[i]);
        CopyText(g_displays[j].appliedStyle, (int)sizeof(g_displays[j].appliedStyle),
                 plan->style[i]);
        if (thumbnailReady[i])
        {
          char thumbnailKey[kMaxPath + 96] = {0};
          WallpaperFileKey(plan->renderPath[i], thumbnailKey, (int)sizeof(thumbnailKey));
          CopyText(g_displays[j].thumbnailSource,
                   (int)sizeof(g_displays[j].thumbnailSource), thumbnailKey);
          CopyText(g_displays[j].thumbnail, (int)sizeof(g_displays[j].thumbnail), thumbnails[i]);
        }
        break;
      }
  }
  LeaveCriticalSection(&g_lock);
  if (anyApplyNeeded)
  {
    g_restoreLockedPending = false;
    PurgeOldRenders();
  }

  bool lockOk = false;
  bool lockNeeded = plan->syncLock;
  if (lockNeeded)
  {
    if (!plan->lockSource[0])
    {
      if (plan->displayCount == 1)
        CopyText(plan->lockSource, (int)sizeof(plan->lockSource), plan->renderPath[0]);
      else if (!ComposeWallpaper(*plan, plan->lockSource, (int)sizeof(plan->lockSource), error,
                                 (int)sizeof(error)))
      {
        LogEvent("lock composite failed: %s", error);
      }
    }
    if (plan->lockSource[0])
    {
      char lockImage[kMaxPath] = {0};
      if (PrepareLockImage(plan->lockSource, lockImage, (int)sizeof(lockImage), error,
                           (int)sizeof(error)))
        lockOk = SetLockScreenImage(lockImage);
      else
        LogEvent("lock image preparation failed: %s", error);
    }
  }
  if (lockOk)
  {
    EnterCriticalSection(&g_lock);
    CopyText(g_currentLock, (int)sizeof(g_currentLock), plan->lockSource);
    g_lockApplyPending = false;
    LeaveCriticalSection(&g_lock);
  }

  LogEvent("applied schedule=%s variant=%s displays=%d style=%s",
           g_settings.schedule,
           plan->variant == Variant::Night ? "night" : (plan->variant == Variant::Interval ? "interval" : "day"),
           plan->displayCount, plan->displayCount > 0 ? plan->style[0] : "none");
  RefreshThumbnails();
  return true;
}

bool CreateDesktopWallpaper(IDesktopWallpaper **out, char *error, int errorSize)
{
  if (!out)
    return false;
  *out = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                IID_IDesktopWallpaper, reinterpret_cast<void **>(out));
  if (FAILED(hr) || !*out)
  {
    _snprintf(error, errorSize, "IDesktopWallpaper unavailable (0x%08lx)", (unsigned long)hr);
    return false;
  }
  return true;
}

DWORD WINAPI Worker(void *)
{
  const apex::wallpaper::winrt::CombaseApi &winrt = apex::wallpaper::winrt::GetCombaseApi();
  HRESULT ro = winrt.ok() ? winrt.roInitialize(RO_INIT_MULTITHREADED) : E_NOTIMPL;
  const bool roOwned = SUCCEEDED(ro);
  WmiOpen();
  Gdiplus::GdiplusStartupInput gdiInput;
  if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr) != Gdiplus::Ok)
    g_gdiplusToken = 0;

  char lastSignature[sizeof(Plan::signature)] = {0};
  DWORD lastDisplayPoll = 0;
  DWORD lastLockPoll = 0;
  bool lastEnabled = false;
  while (!g_stop)
  {
    const bool enabled = g_host && g_host->featureEnabled && g_host->featureEnabled(kId) != 0;
    if (!enabled)
    {
      if (lastEnabled)
        Log("wallpaper: switched off in Apex");
      lastEnabled = false;
      lastSignature[0] = 0;
      WaitForSingleObject(g_wake, kCycleMs);
      ResetEvent(g_wake);
      continue;
    }
    lastEnabled = true;

    EnterCriticalSection(&g_lock);
    const bool checkLock = !g_lockChecked;
    LeaveCriticalSection(&g_lock);
    if (checkLock)
      QueryLockScreenSupport();
    const DWORD nowTick = GetTickCount();
    if (!lastLockPoll || nowTick - lastLockPoll >= 30000)
    {
      ReadCurrentLockScreen();
      lastLockPoll = nowTick;
    }
    {
      EnterCriticalSection(&g_lock);
      const bool needLocation =
          !strcmp(g_settings.schedule, kScheduleSolar) &&
          (!g_settings.locationKnown || !g_locationFetchedAt ||
           nowTick - g_locationFetchedAt >= 6u * 60u * 60u * 1000u);
      LeaveCriticalSection(&g_lock);
      if (needLocation)
      {
        double latitude = 0, longitude = 0;
        FetchApproxLocation(&latitude, &longitude);
      }
    }

    IDesktopWallpaper *desk = nullptr;
    char apiError[256] = {0};
    const bool apiReady = CreateDesktopWallpaper(&desk, apiError, (int)sizeof(apiError));
    EnterCriticalSection(&g_lock);
    const bool apiWasReady = g_apiReady;
    g_apiReady = apiReady;
    LeaveCriticalSection(&g_lock);
    if (apiReady && !apiWasReady)
      Log("wallpaper: IDesktopWallpaper is available");
    else if (!apiReady && apiWasReady)
      Log("wallpaper: IDesktopWallpaper unavailable; monitor discovery uses EnumDisplayMonitors");
    if (!apiReady && !apiWasReady)
      LogEvent("desktop API: %s", apiError);
    const DWORD now = GetTickCount();
    if (!lastDisplayPoll || now - lastDisplayPoll >= kDisplayPollMs)
    {
      DiscoverDisplays(desk);
      lastDisplayPoll = now;
    }
    CheckDisplayWallpaperChanges(desk);
    if (desk)
      desk->Release();
    RefreshThumbnails();

    Snapshot snapshot;
    TakeSnapshot(&snapshot);
    Plan plan;
    char whyZh[256] = {0}, whyEn[256] = {0};
    const bool forceApply = InterlockedExchange(&g_applyDirty, 0) != 0;
    if (forceApply)
      lastSignature[0] = 0;
    if (BuildPlan(snapshot, &plan, whyZh, (int)sizeof(whyZh), whyEn, (int)sizeof(whyEn)))
    {
      if (forceApply || strcmp(lastSignature, plan.signature))
      {
        if (ApplyPlan(&plan))
          CopyText(lastSignature, (int)sizeof(lastSignature), plan.signature);
      }
    }
    else
      lastSignature[0] = 0;

    WaitForSingleObject(g_wake, kCycleMs);
    ResetEvent(g_wake);
  }

  if (g_gdiplusToken)
  {
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
  }
  if (roOwned)
    winrt.roUninitialize();
  return 0;
}

void AppendText(char *out, int cap, int &off, const char *fmt, ...)
{
  if (!out || off < 0 || off >= cap)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + off, (size_t)(cap - off), fmt, ap);
  va_end(ap);
  if (n > 0)
    off += n;
  if (off >= cap)
    off = cap;
}

void AppendJsonString(char *out, int cap, int &off, const char *text)
{
  AppendText(out, cap, off, "\"");
  if (text)
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
    {
      switch (*p)
      {
      case '"': AppendText(out, cap, off, "\\\""); break;
      case '\\': AppendText(out, cap, off, "\\\\"); break;
      case '\b': AppendText(out, cap, off, "\\b"); break;
      case '\f': AppendText(out, cap, off, "\\f"); break;
      case '\n': AppendText(out, cap, off, "\\n"); break;
      case '\r': AppendText(out, cap, off, "\\r"); break;
      case '\t': AppendText(out, cap, off, "\\t"); break;
      default:
        if (*p < 0x20)
          AppendText(out, cap, off, "\\u%04x", *p);
        else
          AppendText(out, cap, off, "%c", *p);
        break;
      }
    }
  AppendText(out, cap, off, "\"");
}

void AddSelectOption(char *out, int cap, int &off, bool &first, const char *value,
                     const char *zh, const char *en)
{
  AppendText(out, cap, off, first ? "" : ",");
  first = false;
  AppendText(out, cap, off, "{\"value\":");
  AppendJsonString(out, cap, off, value ? value : "");
  AppendText(out, cap, off, ",\"labelZh\":");
  AppendJsonString(out, cap, off, zh ? zh : "");
  AppendText(out, cap, off, ",\"labelEn\":");
  AppendJsonString(out, cap, off, en ? en : "");
  AppendText(out, cap, off, "}");
}

void AddTextField(char *out, int cap, int &off, const char *id, const char *zh, const char *en,
                  const char *value, const char *placeholderZh, const char *placeholderEn)
{
  AppendText(out, cap, off, "{\"id\":");
  AppendJsonString(out, cap, off, id);
  AppendText(out, cap, off, ",\"type\":\"text\",\"labelZh\":");
  AppendJsonString(out, cap, off, zh);
  AppendText(out, cap, off, ",\"labelEn\":");
  AppendJsonString(out, cap, off, en);
  AppendText(out, cap, off, ",\"value\":");
  AppendJsonString(out, cap, off, value ? value : "");
  AppendText(out, cap, off, ",\"placeholderZh\":");
  AppendJsonString(out, cap, off, placeholderZh ? placeholderZh : "");
  AppendText(out, cap, off, ",\"placeholderEn\":");
  AppendJsonString(out, cap, off, placeholderEn ? placeholderEn : "");
  AppendText(out, cap, off, "}");
}

void AddFileField(char *out, int cap, int &off, const char *id, const char *zh, const char *en,
                  const char *kind, const char *placeholderZh, const char *placeholderEn)
{
  AppendText(out, cap, off, "{\"id\":");
  AppendJsonString(out, cap, off, id);
  AppendText(out, cap, off, ",\"type\":\"file\",\"kind\":");
  AppendJsonString(out, cap, off, kind);
  AppendText(out, cap, off, ",\"labelZh\":");
  AppendJsonString(out, cap, off, zh);
  AppendText(out, cap, off, ",\"labelEn\":");
  AppendJsonString(out, cap, off, en);
  AppendText(out, cap, off, ",\"placeholderZh\":");
  AppendJsonString(out, cap, off, placeholderZh ? placeholderZh : "");
  AppendText(out, cap, off, ",\"placeholderEn\":");
  AppendJsonString(out, cap, off, placeholderEn ? placeholderEn : "");
  AppendText(out, cap, off, "}");
}

void AddStyleField(char *out, int cap, int &off)
{
  AppendText(out, cap, off,
             "{\"id\":\"style\",\"type\":\"select\",\"labelZh\":\"显示方式\",\"labelEn\":\"Display mode\",\"options\":[");
  bool first = true;
  AddSelectOption(out, cap, off, first, "fill", "填充", "Fill");
  AddSelectOption(out, cap, off, first, "fit", "适应", "Fit");
  AddSelectOption(out, cap, off, first, "stretch", "拉伸", "Stretch");
  AddSelectOption(out, cap, off, first, "tile", "平铺", "Tile");
  AddSelectOption(out, cap, off, first, "center", "居中", "Center");
  AppendText(out, cap, off, "]}");
}

void AddImagePickerField(char *out, int cap, int &off, const char *id)
{
  AppendText(out, cap, off, "{\"id\":");
  AppendJsonString(out, cap, off, id);
  AppendText(out, cap, off,
             ",\"type\":\"file\",\"kind\":\"image\",\"hidePath\":true,\"previewId\":\"preview\","
             "\"buttonZh\":\"选择壁纸\",\"buttonEn\":\"Choose wallpaper\","
             "\"labelZh\":\"\",\"labelEn\":\"\"}");
}

void AddRangeField(char *out, int cap, int &off, const Settings &s)
{
  AppendText(out, cap, off,
             "{\"id\":\"interval_minutes\",\"type\":\"range\",\"min\":1,\"max\":1440,\"step\":1,"
             "\"value\":%d,\"def\":60,\"labelZh\":\"轮换间隔\",\"labelEn\":\"Rotation interval\","
             "\"unit\":\"min\",\"hue\":\"#78BEFF\"}",
             s.intervalMinutes);
}

int SettingsJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  Snapshot s;
  TakeSnapshot(&s);
  int off = 0;
  AppendText(out, outSize, off, "{\"params\":[");
  AppendText(out, outSize, off, "{\"id\":\"monitors\",\"type\":\"group\",\"layout\":\"rows\",\"noAdd\":true,\"live\":true,\"labelZh\":\"显示器壁纸\",\"labelEn\":\"Display wallpapers\",\"fields\":[");
  AddStyleField(out, outSize, off);
  AppendText(out, outSize, off, ",");
  AppendText(out, outSize, off,
             "{\"id\":\"locked\",\"type\":\"bool\",\"labelZh\":\"锁定壁纸\","
             "\"labelEn\":\"Lock wallpaper\"},");
  AddImagePickerField(out, outSize, off, "wallpaper");
  AppendText(out, outSize, off, "],\"items\":[");
  for (int i = 0; i < s.displayCount; ++i)
  {
    const MonitorSetting *ms = SnapshotMonitor(s, s.display[i].identity);
    if (i)
      AppendText(out, outSize, off, ",");
    char titleZh[128] = {0}, titleEn[128] = {0};
    _snprintf(titleZh, sizeof(titleZh), "显示器 %d · %s · %ld×%ld", i + 1,
              s.display[i].identity,
              (long)(s.display[i].rect.right - s.display[i].rect.left),
              (long)(s.display[i].rect.bottom - s.display[i].rect.top));
    _snprintf(titleEn, sizeof(titleEn), "Display %d · %s · %ldx%ld", i + 1,
              s.display[i].identity,
              (long)(s.display[i].rect.right - s.display[i].rect.left),
              (long)(s.display[i].rect.bottom - s.display[i].rect.top));
    AppendText(out, outSize, off, "{\"titleZh\":");
    AppendJsonString(out, outSize, off, titleZh);
    AppendText(out, outSize, off, ",\"titleEn\":");
    AppendJsonString(out, outSize, off, titleEn);
    AppendText(out, outSize, off, ",\"locked\":true,\"values\":{\"wallpaper\":");
    AppendJsonString(out, outSize, off, ms && ms->wallpaper[0] ? ms->wallpaper : s.display[i].currentPath);
    AppendText(out, outSize, off, ",\"style\":");
    AppendJsonString(out, outSize, off,
                     ms && ValidStyle(ms->style) ? ms->style :
                     (ValidStyle(s.display[i].currentStyle) ? s.display[i].currentStyle : "fill"));
    AppendText(out, outSize, off, ",\"locked\":%d", ms && ms->locked ? 1 : 0);
    AppendText(out, outSize, off, ",\"preview\":");
    AppendJsonString(out, outSize, off, s.display[i].thumbnail);
    AppendText(out, outSize, off, "}}");
  }
  AppendText(out, outSize, off, "]}");

  AppendText(out, outSize, off, ",{\"id\":\"lock\",\"type\":\"group\",\"layout\":\"rows\",\"noAdd\":true,\"fieldsWrap\":true,\"labelZh\":\"锁屏壁纸\",\"labelEn\":\"Lock screen\",\"fields\":[");
  AddImagePickerField(out, outSize, off, "lock_file");
  AppendText(out, outSize, off, "],\"items\":[{\"titleZh\":\"锁屏\",\"titleEn\":\"Lock screen\",\"locked\":true,\"values\":{\"lock_file\":");
  AppendJsonString(out, outSize, off, s.s.lockPath[0] ? s.s.lockPath : s.currentLock);
  AppendText(out, outSize, off, ",\"preview\":");
  AppendJsonString(out, outSize, off, s.currentLockThumbnail);
  AppendText(out, outSize, off, "}}]}");

  AppendText(out, outSize, off, ",{\"id\":\"switching\",\"type\":\"group\",\"layout\":\"rows\",\"noAdd\":true,\"fieldsWrap\":true,\"labelZh\":\"切换规则\",\"labelEn\":\"Switching\",\"fields\":[");
  AppendText(out, outSize, off, "{\"id\":\"schedule\",\"type\":\"select\",\"labelZh\":\"切换方式\",\"labelEn\":\"Mode\",\"options\":[");
  bool first = true;
  AddSelectOption(out, outSize, off, first, kScheduleClock, "时间段", "Time periods");
  AddSelectOption(out, outSize, off, first, kScheduleSolar, "日出日落", "Sunrise / sunset");
  AddSelectOption(out, outSize, off, first, kScheduleInterval, "切换间隔", "Interval");
  AppendText(out, outSize, off, "]},");
  AddTextField(out, outSize, off, "day_start", "日间开始", "Day starts", "", "07:00", "07:00");
  AppendText(out, outSize, off, ",");
  AddTextField(out, outSize, off, "night_start", "夜间开始", "Night starts", "", "19:00", "19:00");
  AppendText(out, outSize, off, ",");
  AddRangeField(out, outSize, off, s.s);
  char dayStart[8] = {0}, nightStart[8] = {0};
  _snprintf(dayStart, sizeof(dayStart), "%02d:%02d", s.s.dayStart / 60, s.s.dayStart % 60);
  _snprintf(nightStart, sizeof(nightStart), "%02d:%02d", s.s.nightStart / 60, s.s.nightStart % 60);
  AppendText(out, outSize, off, "],\"items\":[{\"titleZh\":\"当前规则\",\"titleEn\":\"Current rule\",\"locked\":true,\"values\":{\"schedule\":");
  AppendJsonString(out, outSize, off, s.s.schedule);
  AppendText(out, outSize, off, ",\"day_start\":");
  AppendJsonString(out, outSize, off, dayStart);
  AppendText(out, outSize, off, ",\"night_start\":");
  AppendJsonString(out, outSize, off, nightStart);
  AppendText(out, outSize, off, ",\"interval_minutes\":%d}}]}", s.s.intervalMinutes);

  AppendText(out, outSize, off,
             "],\"settingsFile\":\"Wallpaper.ini\","
             "\"summaryZh\":\"逐屏壁纸、锁屏与自动切换\","
             "\"summaryEn\":\"Per-display wallpaper, lock screen, and switching\"}");
  if (off >= outSize)
    return 0;
  return off;
}

bool SplitMonitorPath(const char *path, int *index, const char **field)
{
  if (!path || !index || !field || strncmp(path, "monitors[", 9))
    return false;
  const char *close = strchr(path + 9, ']');
  if (!close || close[1] != '.')
    return false;
  char number[16] = {0};
  const size_t n = static_cast<size_t>(close - (path + 9));
  if (n == 0 || n >= sizeof(number))
    return false;
  memcpy(number, path + 9, n);
  number[n] = 0;
  if (!apex::wallpaper::ParseInt(number, index) || *index < 0)
    return false;
  *field = close + 2;
  return true;
}

const char *GroupField(const char *path, const char *group)
{
  if (!path || !group)
    return nullptr;
  const size_t n = strlen(group);
  if (!strncmp(path, group, n) && path[n] == '.')
    return path + n + 1;
  if (strncmp(path, group, n) || path[n] != '[')
    return nullptr;
  const char *close = strchr(path + n, ']');
  if (!close || close[1] != '.')
    return nullptr;
  return close + 2;
}

int ApplyControl(const char *path, const char *value)
{
  if (!path || !value)
    return 0;

  int monitorIndex = 0;
  const char *monitorField = nullptr;
  if (SplitMonitorPath(path, &monitorIndex, &monitorField))
  {
    if (strcmp(monitorField, "wallpaper") && strcmp(monitorField, "style") &&
        strcmp(monitorField, "locked"))
      return 0;
    char identity[96] = {0};
    char previous[kMaxPath] = {0};
    char current[kMaxPath] = {0};
    EnterCriticalSection(&g_lock);
    if (monitorIndex < 0 || monitorIndex >= g_displayCount)
    {
      LeaveCriticalSection(&g_lock);
      return 0;
    }
    CopyText(identity, (int)sizeof(identity), g_displays[monitorIndex].identity);
    CopyText(current, (int)sizeof(current), g_displays[monitorIndex].currentPath);
    MonitorSetting *before = FindMonitorSettingLocked(identity);
    if (before)
      CopyText(previous, (int)sizeof(previous), before->wallpaper);
    LeaveCriticalSection(&g_lock);
    char text[kMaxPath] = {0};
    CopyText(text, (int)sizeof(text), value);
    TrimText(text);
    char copied[kMaxPath] = {0};
    bool lockOn = false;
    if (!strcmp(monitorField, "wallpaper"))
    {
      if (text[0])
      {
        if (!CopyChosenWallpaper(text, identity, false, copied, (int)sizeof(copied)))
          return 0;
      }
    }
    else if (!strcmp(monitorField, "style") && !ValidStyle(text))
      return 0;
    else if (!strcmp(monitorField, "locked"))
    {
      if (strcmp(text, "0") && strcmp(text, "1"))
        return 0;
      lockOn = text[0] == '1';
      if (lockOn && !previous[0])
      {
        bool captured =
            current[0] &&
            CopyChosenWallpaper(current, identity, false, copied, (int)sizeof(copied));
        if (!captured)
          captured = CaptureCurrentWallpaperForDisplay(identity, copied, (int)sizeof(copied));
        if (!captured)
          return 0;
      }
    }

    char preview[kThumbnailDataSize] = {0};
    const bool havePreview =
        copied[0] && EncodeThumbnailDataUri(copied, preview, (int)sizeof(preview));
    char previewKey[kMaxPath + 96] = {0};
    if (havePreview)
      WallpaperFileKey(copied, previewKey, (int)sizeof(previewKey));
    EnterCriticalSection(&g_lock);
    MonitorSetting *ms = EnsureMonitorSettingLocked(identity);
    if (!ms)
    {
      LeaveCriticalSection(&g_lock);
      return 0;
    }
    if (!strcmp(monitorField, "wallpaper") || copied[0])
    {
      CopyText(ms->wallpaper, (int)sizeof(ms->wallpaper), copied);
      if (havePreview)
        for (int i = 0; i < g_displayCount; ++i)
          if (!strcmp(g_displays[i].identity, identity))
          {
            CopyText(g_displays[i].thumbnailSource, (int)sizeof(g_displays[i].thumbnailSource),
                     previewKey);
            CopyText(g_displays[i].thumbnail, (int)sizeof(g_displays[i].thumbnail), preview);
            break;
          }
    }
    if (!strcmp(monitorField, "style"))
      CopyText(ms->style, (int)sizeof(ms->style), text);
    else if (!strcmp(monitorField, "locked"))
      ms->locked = lockOn;
    LeaveCriticalSection(&g_lock);
    if (!strcmp(monitorField, "wallpaper") || copied[0])
      DeletePreviousCopy(previous, copied);
    MarkApplyDirty();
    if (g_wake)
      SetEvent(g_wake);
    return 1;
  }

  const char *lockField = GroupField(path, "lock");
  const char *switchingField = GroupField(path, "switching");
  if (lockField)
    path = lockField;
  else if (switchingField)
    path = switchingField;

  if (!strcmp(path, "lock_file"))
  {
    char text[kMaxPath] = {0};
    char previous[kMaxPath] = {0};
    CopyText(text, (int)sizeof(text), value);
    TrimText(text);
    EnterCriticalSection(&g_lock);
    CopyText(previous, (int)sizeof(previous), g_settings.lockPath);
    LeaveCriticalSection(&g_lock);
    char copied[kMaxPath] = {0};
    if (text[0] && !CopyChosenWallpaper(text, "lock", true, copied, (int)sizeof(copied)))
      return 0;
    char preview[kThumbnailDataSize] = {0};
    const bool havePreview =
        copied[0] && EncodeThumbnailDataUri(copied, preview, (int)sizeof(preview));
    EnterCriticalSection(&g_lock);
    CopyText(g_settings.lockPath, (int)sizeof(g_settings.lockPath), copied);
    if (havePreview)
    {
      CopyText(g_currentLock, (int)sizeof(g_currentLock), copied);
      CopyText(g_lockThumbnailSource, (int)sizeof(g_lockThumbnailSource), copied);
      CopyText(g_currentLockThumbnail, (int)sizeof(g_currentLockThumbnail), preview);
    }
    if (copied[0])
      g_lockApplyPending = true;
    LeaveCriticalSection(&g_lock);
    DeletePreviousCopy(previous, copied);
    MarkApplyDirty();
    if (g_wake)
      SetEvent(g_wake);
    return 1;
  }

  bool changed = false;
  EnterCriticalSection(&g_lock);
  if (!strcmp(path, "schedule") && ValidSchedule(value))
  {
    CopyText(g_settings.schedule, (int)sizeof(g_settings.schedule), value);
    changed = true;
  }
  else if (!strcmp(path, "day_start") || !strcmp(path, "night_start"))
  {
    int minute = 0;
    if (!apex::wallpaper::ParseClock(value, &minute))
    {
      LeaveCriticalSection(&g_lock);
      return 0;
    }
    if (!strcmp(path, "day_start"))
      g_settings.dayStart = minute;
    else
      g_settings.nightStart = minute;
    changed = true;
  }
  else if (!strcmp(path, "interval_minutes"))
  {
    int number = 0;
    if (!apex::wallpaper::ParseInt(value, &number) || number < 1 || number > 1440)
    {
      LeaveCriticalSection(&g_lock);
      return 0;
    }
    g_settings.intervalMinutes = number;
    changed = true;
  }
  LeaveCriticalSection(&g_lock);
  if (changed)
  {
    MarkApplyDirty();
    if (g_wake)
      SetEvent(g_wake);
  }
  return changed ? 1 : 0;
}

int Init(const ApexHost *host)
{
  g_host = host;
  g_noApply = GetEnvironmentVariableA("APEX_WALLPAPER_NO_APPLY", nullptr, 0) > 0;
  if (host && host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));
  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  InitializeCriticalSection(&g_saveLock);
  g_saveLockReady = true;
  InterlockedExchange(&g_stop, 0);
  InterlockedExchange(&g_applyDirty, 0);
  OpenLog();
  LoadSettings();
  ReadCurrentLockScreen();
  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!g_wake)
  {
    Log("wallpaper: CreateEvent failed (%lu) -- refusing", (unsigned long)GetLastError());
    DeleteCriticalSection(&g_saveLock);
    g_saveLockReady = false;
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
    if (g_log)
    {
      fclose(g_log);
      g_log = nullptr;
    }
    return 1;
  }
  g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (!g_thread)
  {
    Log("wallpaper: worker thread failed (%lu) -- refusing", (unsigned long)GetLastError());
    CloseHandle(g_wake);
    g_wake = nullptr;
    DeleteCriticalSection(&g_saveLock);
    g_saveLockReady = false;
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
    if (g_log)
    {
      fclose(g_log);
      g_log = nullptr;
    }
    return 1;
  }
  Log("wallpaper: started (%s)", g_noApply ? "test mode, system unchanged" : "live");
  return 0;
}

void Shutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_wake)
    SetEvent(g_wake);
  if (g_thread)
  {
    WaitForSingleObject(g_thread, 30000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_wake)
  {
    CloseHandle(g_wake);
    g_wake = nullptr;
  }
  if (g_log)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  if (g_saveLockReady)
  {
    DeleteCriticalSection(&g_saveLock);
    g_saveLockReady = false;
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
  if (g_wake)
    SetEvent(g_wake);
  return 1;
}

int SetControl(const char *path, const char *value) { return ApplyControl(path, value); }
int Save() { return SaveSettings() ? 1 : 0; }

unsigned Flags()
{
  return APEX_FEATURE_ENABLED;
}

} // namespace

static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    kId,
    "壁纸设置",
    "Wallpaper",
    kVersion,
    Init,
    Shutdown,
    Reload,
    SettingsJson,
    SetControl,
    nullptr,
    nullptr,
    Save,
    nullptr,
    nullptr,
    Flags,
    nullptr, // taskbarItems
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
