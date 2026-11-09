// ---------------------------------------------------------------------------
// AppProxy -- let selected programs reach TCP services through one configured
// HTTPS CONNECT or SOCKS5 proxy.
//
// THE SHAPE OF THE FEATURE
//   * the page owns one proxy server and a list of programs;
//   * this DLL watches for those programs and loads AppProxyHook.payload into
//     each ordinary 64-bit process;
//   * the payload intercepts Winsock connect()/WSAConnect() and completes the
//     proxy handshake before the application believes it is connected.
//
// WHY THE PAYLOAD IS A SEPARATE FILE. It must survive inside other programs,
// so it deliberately has no C/C++ runtime dependency. Keeping it out of this
// DLL also means a failure while loading the payload cannot unload the feature
// or the settings page.
//
// FIRST-VERSION BOUNDARY, stated on the page as well as here:
//   * TCP only; UDP and QUIC are not redirected;
//   * ordinary 64-bit, same-privilege desktop programs only;
//   * DNS is resolved by Windows before the proxy is contacted.
//
// Those are the costs of the user's chosen route: no kernel driver, no service,
// no registry and no install step.
// ---------------------------------------------------------------------------

#include "../../apex/abi.h"
#include "../../common/match.h"
#include "proxy_common.h"
#include "process_tree.h"
#include "service_status.h"

#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

// The CRT-less build still receives a tiny helper object from libmingwex that
// asks for atexit. Nothing in this feature registers a process-wide destructor,
// so accepting and ignoring it is exact rather than a workaround.
extern "C" int atexit(void (*)(void))
{
  return 0;
}

namespace {

const char *kId = "AppProxy";
const char *kVersion = "2.2.0";
const int kScanMs = 500;
const int kMaxTracked = 128;
const int kLogCap = 256 * 1024;
const char *kEnabledEvent = "Local\\ApexAppProxyEnabled.2";
const char *kHookLeaf = "AppProxyHook.payload";

struct Tracked
{
  DWORD pid = 0;
  HHOOK hook = nullptr;
  bool injected = false;
  ULONGLONG nextTryMs = 0;
  char exe[64] = {0};
  char why[120] = {0};
};

struct Status
{
  int enabled = 0;
  int matched = 0;
  int active = 0;
  int failed = 0;
};

const ApexHost *g_host = nullptr;
appproxy::Config g_cfg;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
char g_dir[512] = {0};
char g_configPath[560] = {0};
char g_logPath[560] = {0};
char g_sourcePayloadPath[560] = {0};
char g_payloadPath[560] = {0};

HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;
HANDLE g_enabledEvent = nullptr;
volatile LONG g_stop = 0;
HMODULE g_payload = nullptr;
HOOKPROC g_hookProc = nullptr;

// USER32 is reached through pointers rather than the import table. The build-layer gate runs inside a
// restricted loader context where statically importing USER32 makes LoadLibrary fail before DllMain; the real
// interactive host does not have that restriction. Keeping the calls dynamic makes the same DLL testable in
// both places and costs one table lookup at startup.
HMODULE g_user32 = nullptr;
typedef BOOL(WINAPI *EnumWindowsFn)(WNDENUMPROC, LPARAM);
typedef DWORD(WINAPI *GetWindowThreadProcessIdFn)(HWND, LPDWORD);
typedef BOOL(WINAPI *PostMessageFn)(HWND, UINT, WPARAM, LPARAM);
typedef HHOOK(WINAPI *SetWindowsHookExFn)(int, HOOKPROC, HINSTANCE, DWORD);
typedef BOOL(WINAPI *UnhookWindowsHookExFn)(HHOOK);
EnumWindowsFn g_enumWindows = nullptr;
GetWindowThreadProcessIdFn g_getWindowThreadProcessId = nullptr;
PostMessageFn g_postMessage = nullptr;
SetWindowsHookExFn g_setWindowsHookEx = nullptr;
UnhookWindowsHookExFn g_unhookWindowsHookEx = nullptr;

HMODULE g_advapi = nullptr;
HMODULE g_shell32 = nullptr;
typedef SC_HANDLE(WINAPI *OpenSCManagerFn)(LPCSTR, LPCSTR, DWORD);
typedef SC_HANDLE(WINAPI *OpenServiceFn)(SC_HANDLE, LPCSTR, DWORD);
typedef BOOL(WINAPI *QueryServiceStatusFn)(SC_HANDLE, LPSERVICE_STATUS);
typedef BOOL(WINAPI *CloseServiceHandleFn)(SC_HANDLE);
typedef BOOL(WINAPI *ShellExecuteExFn)(LPSHELLEXECUTEINFOA);
OpenSCManagerFn g_openSCManager = nullptr;
OpenServiceFn g_openService = nullptr;
QueryServiceStatusFn g_queryServiceStatus = nullptr;
CloseServiceHandleFn g_closeServiceHandle = nullptr;
ShellExecuteExFn g_shellExecuteEx = nullptr;

Tracked g_tracked[kMaxTracked];
int g_trackedCount = 0;
Status g_status;
FILETIME g_statusStamp = {0, 0};

int SaveSettings();

void LogHost(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char text[768];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  text[sizeof(text) - 1] = 0;
  g_host->logLine(text);
}

void RotateLog()
{
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExA(g_logPath, GetFileExInfoStandard, &fa))
    return;
  if (((ULONGLONG)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) <= (ULONGLONG)kLogCap)
    return;
  char oldPath[580] = {0};
  _snprintf(oldPath, sizeof(oldPath), "%s.1", g_logPath);
  DeleteFileA(oldPath);
  MoveFileA(g_logPath, oldPath);
}

void LogLine(const char *fmt, ...)
{
  char text[768];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  text[sizeof(text) - 1] = 0;
  HANDLE f = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE)
  {
    DWORD wrote = 0;
    WriteFile(f, text, (DWORD)strlen(text), &wrote, nullptr);
    WriteFile(f, "\r\n", 2, &wrote, nullptr);
    CloseHandle(f);
  }
  LogHost("appproxy: %s", text);
}

void FillPath(const char *name, char *out, int cap)
{
  _snprintf(out, cap, "%s%s", g_dir, name);
  out[cap - 1] = 0;
}

void PayloadLeaf(const char *path, char *out, int cap)
{
  const char *leaf = path ? path : "";
  for (const char *p = leaf; *p; ++p)
    if (*p == '\\' || *p == '/')
      leaf = p + 1;
  _snprintf(out, cap, "%s", leaf);
  out[cap - 1] = 0;
}

bool PrepareRuntimePayload()
{
  FillPath(kHookLeaf, g_sourcePayloadPath, (int)sizeof(g_sourcePayloadPath));
  if (GetFileAttributesA(g_sourcePayloadPath) == INVALID_FILE_ATTRIBUTES)
  {
    LogLine("payload source is missing: %s", g_sourcePayloadPath);
    return false;
  }

  const ULONGLONG stamp = GetTickCount64();
  char leaf[128] = {0};
  _snprintf(leaf, sizeof(leaf), "AppProxyHook.runtime-%lu-%llu.payload",
            (unsigned long)GetCurrentProcessId(), (unsigned long long)stamp);
  leaf[sizeof(leaf) - 1] = 0;
  FillPath(leaf, g_payloadPath, (int)sizeof(g_payloadPath));

  // Remove runtime copies that no target process has mapped. A mapped one refuses deletion and is harmless;
  // after that target exits, the next Apex start removes it.
  char pattern[600] = {0};
  _snprintf(pattern, sizeof(pattern), "%sAppProxyHook.runtime-*.payload", g_dir);
  WIN32_FIND_DATAA fd;
  HANDLE find = FindFirstFileA(pattern, &fd);
  if (find != INVALID_HANDLE_VALUE)
  {
    do
    {
      char oldPath[560] = {0};
      FillPath(fd.cFileName, oldPath, (int)sizeof(oldPath));
      DeleteFileA(oldPath);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
  }

  if (!CopyFileA(g_sourcePayloadPath, g_payloadPath, FALSE))
  {
    LogLine("could not create this session's payload copy (%lu) -- injection stays off", GetLastError());
    g_payloadPath[0] = 0;
    return false;
  }

  char session[appproxy::kSessionCap] = {0};
  PayloadLeaf(g_payloadPath, session, (int)sizeof(session));
  EnterCriticalSection(&g_lock);
  appproxy::Copy(g_cfg.session, (int)sizeof(g_cfg.session), session);
  LeaveCriticalSection(&g_lock);
  SaveSettings();
  LogLine("payload session: %s", session);
  return true;
}

appproxy::Config CopyConfig()
{
  appproxy::Config c;
  EnterCriticalSection(&g_lock);
  c = g_cfg;
  LeaveCriticalSection(&g_lock);
  return c;
}

bool ReadConfigFile(appproxy::Config *out)
{
  HANDLE f = CreateFileA(g_configPath, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return false;
  const DWORD size = GetFileSize(f, nullptr);
  if (size == INVALID_FILE_SIZE || size > (DWORD)(appproxy::kConfigCap - 1))
  {
    CloseHandle(f);
    return false;
  }
  char text[appproxy::kConfigCap];
  DWORD n = 0;
  const BOOL ok = ReadFile(f, text, size, &n, nullptr);
  CloseHandle(f);
  if (!ok || n != size)
    return false;
  text[n] = 0;
  appproxy::ParseConfig(text, (int)n, out);
  return true;
}

bool WriteTextFileReplacing(const char *path, const char *text)
{
  char tmp[580] = {0};
  _snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return false;
  const size_t len = strlen(text);
  DWORD wrote = 0;
  const BOOL writeOk = WriteFile(f, text, (DWORD)len, &wrote, nullptr);
  const BOOL closeOk = CloseHandle(f);
  if (!writeOk || wrote != len || !closeOk)
  {
    DeleteFileA(tmp);
    return false;
  }
  if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    DeleteFileA(tmp);
    return false;
  }
  return true;
}

int SaveSettings()
{
  appproxy::Config c = CopyConfig();
  char text[appproxy::kConfigCap];
  int o = 0;
  auto add = [&](const char *fmt, ...) {
    if (o >= (int)sizeof(text) - 1)
      return;
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf(text + o, sizeof(text) - o - 1, fmt, ap);
    va_end(ap);
    if (n > 0)
      o += n;
    if (o > (int)sizeof(text) - 1)
      o = (int)sizeof(text) - 1;
  };

  add("# AppProxy settings. The password is stored in this folder in plain text.\r\n");
  add("# This file belongs to the feature and is never replaced by a deploy.\r\n");
  add("session=%s\r\n", c.session);
  add("windivert_dir=%s\r\n", c.windivertDir);
  add("bypass_local=%d\r\n", c.bypassLocal ? 1 : 0);
  add("block_quic=%d\r\n", c.blockQuic ? 1 : 0);
  for (int i = 0; i < c.profileCount; ++i)
  {
    const appproxy::Profile &p = c.profiles[i];
    add("profile=%s|%s|%s|%d|%s|%s\r\n", p.name,
        p.kind == appproxy::kHttpConnect ? "http" : "socks5", p.server, p.port, p.username,
        p.password);
  }
  for (int i = 0; i < c.ruleCount; ++i)
    add("app=%s|%d|%s\r\n", c.rules[i].pattern, c.rules[i].enabled ? 1 : 0,
        c.rules[i].profile);
  text[o] = 0;

  const bool ok = WriteTextFileReplacing(g_configPath, text);
  if (!ok)
    LogLine("could not write %s (error %lu)", g_configPath, GetLastError());
  return ok ? 1 : 0;
}

void EnsureEnabledEvent()
{
  if (g_enabledEvent)
    return;
  g_enabledEvent = CreateEventA(nullptr, TRUE, FALSE, kEnabledEvent);
  if (!g_enabledEvent)
    LogLine("could not create the enabled event (error %lu)", GetLastError());
}

void SetProxyActive(bool active)
{
  if (!g_enabledEvent)
    return;
  if (active)
    SetEvent(g_enabledEvent);
  else
    ResetEvent(g_enabledEvent);
}

bool CurrentFeatureEnabled()
{
  return g_host && g_host->featureEnabled && g_host->featureEnabled(kId) != 0;
}

bool LoadUser32()
{
  if (g_enumWindows)
    return true;
  g_user32 = GetModuleHandleA("user32.dll");
  if (!g_user32)
    g_user32 = LoadLibraryA("user32.dll");
  if (!g_user32)
    return false;
  g_enumWindows = (EnumWindowsFn)(void *)GetProcAddress(g_user32, "EnumWindows");
  g_getWindowThreadProcessId =
      (GetWindowThreadProcessIdFn)(void *)GetProcAddress(g_user32, "GetWindowThreadProcessId");
  g_postMessage = (PostMessageFn)(void *)GetProcAddress(g_user32, "PostMessageA");
  g_setWindowsHookEx = (SetWindowsHookExFn)(void *)GetProcAddress(g_user32, "SetWindowsHookExA");
  g_unhookWindowsHookEx =
      (UnhookWindowsHookExFn)(void *)GetProcAddress(g_user32, "UnhookWindowsHookEx");
  return g_enumWindows && g_getWindowThreadProcessId && g_postMessage && g_setWindowsHookEx &&
         g_unhookWindowsHookEx;
}

bool LoadServiceApis()
{
  if (!g_openSCManager)
  {
    g_advapi = GetModuleHandleA("advapi32.dll");
    if (!g_advapi)
      g_advapi = LoadLibraryA("advapi32.dll");
    if (g_advapi)
    {
      g_openSCManager = (OpenSCManagerFn)(void *)GetProcAddress(g_advapi, "OpenSCManagerA");
      g_openService = (OpenServiceFn)(void *)GetProcAddress(g_advapi, "OpenServiceA");
      g_queryServiceStatus =
          (QueryServiceStatusFn)(void *)GetProcAddress(g_advapi, "QueryServiceStatus");
      g_closeServiceHandle =
          (CloseServiceHandleFn)(void *)GetProcAddress(g_advapi, "CloseServiceHandle");
    }
  }
  if (!g_shellExecuteEx)
  {
    g_shell32 = GetModuleHandleA("shell32.dll");
    if (!g_shell32)
      g_shell32 = LoadLibraryA("shell32.dll");
    if (g_shell32)
      g_shellExecuteEx = (ShellExecuteExFn)(void *)GetProcAddress(g_shell32, "ShellExecuteExA");
  }
  return g_openSCManager && g_openService && g_queryServiceStatus && g_closeServiceHandle &&
         g_shellExecuteEx;
}

bool HasEnabledApps(const appproxy::Config &c)
{
  for (int i = 0; i < c.ruleCount; ++i)
    if (c.rules[i].enabled)
      return true;
  return false;
}

bool ConfigReady(const appproxy::Config &c)
{
  return appproxy::Valid(&c) && HasEnabledApps(c);
}

bool ServiceRunning()
{
  if (!LoadServiceApis())
    return false;
  SC_HANDLE scm = g_openSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm)
    return false;
  SC_HANDLE svc = g_openService(scm, "ApexAppProxy", SERVICE_QUERY_STATUS);
  bool running = false;
  if (svc)
  {
    SERVICE_STATUS status;
    if (g_queryServiceStatus(svc, &status))
      running = status.dwCurrentState == SERVICE_RUNNING;
    g_closeServiceHandle(svc);
  }
  g_closeServiceHandle(scm);
  return running;
}

bool ReadServiceStatus(appproxy::ServiceStatus *out)
{
  appproxy::Defaults(out);
  char text[2048] = {0};
  char path[560] = {0};
  FillPath("service-status.txt", path, sizeof(path));
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = 0;
  appproxy::ParseServiceStatus(text, (int)n, out);
  return out->any != 0;
}

void BuildStatusNotes(const Status &status, const appproxy::Config &c, bool serviceRunning,
                      const appproxy::ServiceStatus &ss, char *noteZh, int zhCap,
                      char *noteEn, int enCap)
{
  if (!status.enabled)
  {
    _snprintf(noteZh, zhCap, "服务未启用");
    _snprintf(noteEn, enCap, "Service disabled");
  }
  else if (!ConfigReady(c))
  {
    _snprintf(noteZh, zhCap, "配置未完成");
    _snprintf(noteEn, enCap, "Setup incomplete");
  }
  else if (!serviceRunning)
  {
    if (ss.any && appproxy::EqNoCase(ss.state, "error") && ss.detail[0])
      _snprintf(noteZh, zhCap, "服务未连接 · 驱动启动失败");
    else
      _snprintf(noteZh, zhCap, "服务未连接");
    if (ss.any && appproxy::EqNoCase(ss.state, "error") && ss.detail[0])
      _snprintf(noteEn, enCap, "Service disconnected · Driver failed to start");
    else
      _snprintf(noteEn, enCap, "Service disconnected");
  }
  else if (ss.any && appproxy::EqNoCase(ss.state, "error"))
  {
    _snprintf(noteZh, zhCap, "服务连接失败");
    _snprintf(noteEn, enCap, "Service connection failed");
  }
  else if (appproxy::EqNoCase(ss.lastResult, "fail"))
  {
    _snprintf(noteZh, zhCap, "服务已连接 · 最近失败 · 成功 %d / 失败 %d", ss.relayOk,
              ss.relayFail);
    _snprintf(noteEn, enCap, "Service connected · Latest failed · %d ok / %d failed",
              ss.relayOk, ss.relayFail);
  }
  else if (appproxy::EqNoCase(ss.lastResult, "ok"))
  {
    _snprintf(noteZh, zhCap, "服务已连接 · 最近成功 · 成功 %d / 失败 %d", ss.relayOk,
              ss.relayFail);
    _snprintf(noteEn, enCap, "Service connected · Latest succeeded · %d ok / %d failed",
              ss.relayOk, ss.relayFail);
  }
  else if (ss.rewrites > 0)
  {
    _snprintf(noteZh, zhCap, "服务已连接 · 正在建立代理连接");
    _snprintf(noteEn, enCap, "Service connected · Establishing proxy connection");
  }
  else if (ss.ipv6Blocked > 0 || ss.quicBlocked > 0)
  {
    _snprintf(noteZh, zhCap, "服务已连接 · 等待应用回退到 IPv4");
    _snprintf(noteEn, enCap, "Service connected · Waiting for IPv4 fallback");
  }
  else if (ss.matches > 0)
  {
    _snprintf(noteZh, zhCap, "服务已连接 · 已匹配应用");
    _snprintf(noteEn, enCap, "Service connected · Application matched");
  }
  else if (ss.packets > 0)
  {
    _snprintf(noteZh, zhCap, "服务已连接 · 尚未匹配应用");
    _snprintf(noteEn, enCap, "Service connected · No matching application yet");
  }
  else
  {
    _snprintf(noteZh, zhCap, "服务已连接 · 等待应用连接");
    _snprintf(noteEn, enCap, "Service connected · Waiting for applications");
  }
}

bool RunServiceAction(const char *action)
{
  if (!action || !*action)
    return false;
  if (!LoadServiceApis())
    return false;
  char exe[560] = {0};
  FillPath("AppProxyService.exe", exe, sizeof(exe));
  if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
    return false;
  char params[1600] = {0};
  if (strcmp(action, "install") == 0)
  {
    appproxy::Config cfg = CopyConfig();
    if (!cfg.windivertDir[0])
      return false;
    _snprintf(params, sizeof(params), "--install \"%s\" \"%s\"", g_configPath,
              cfg.windivertDir);
  }
  else if (strcmp(action, "uninstall") == 0)
    _snprintf(params, sizeof(params), "--uninstall");
  else if (strcmp(action, "start") == 0)
    _snprintf(params, sizeof(params), "--start");
  else if (strcmp(action, "stop") == 0)
    _snprintf(params, sizeof(params), "--stop");
  else
    return false;
  params[sizeof(params) - 1] = 0;

  SHELLEXECUTEINFOA info;
  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = "runas";
  info.lpFile = exe;
  info.lpParameters = params;
  info.nShow = SW_HIDE;
  if (!g_shellExecuteEx(&info))
  {
    LogLine("service action %s could not start (error %lu)", action, GetLastError());
    return false;
  }
  if (info.hProcess)
  {
    WaitForSingleObject(info.hProcess, 15000);
    CloseHandle(info.hProcess);
  }
  LogLine("service action %s requested", action);
  return true;
}

bool LoadSettings()
{
  appproxy::Config next;
  appproxy::Defaults(&next);
  const bool existed = ReadConfigFile(&next);
  EnterCriticalSection(&g_lock);
  g_cfg = next;
  LeaveCriticalSection(&g_lock);
  if (!existed)
    SaveSettings();
  if (g_wake)
    SetEvent(g_wake);
  return true;
}

int ReloadSettings()
{
  const bool ok = LoadSettings();
  LogLine("settings reloaded%s", ok ? "" : " (kept the old values)");
  return ok ? 1 : 0;
}

void ExeLeaf(const char *path, char *out, int cap)
{
  const char *leaf = path ? path : "";
  for (const char *p = leaf; p && *p; ++p)
    if (*p == '\\' || *p == '/')
      leaf = p + 1;
  apex::match::NormaliseName(leaf, out, cap);
}

bool Is32BitTarget(HANDLE p)
{
  BOOL wow = FALSE;
  if (!IsWow64Process(p, &wow))
    return false;
  return wow != FALSE;
}

bool ModuleLoaded(DWORD pid, const char *leaf)
{
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE)
    return false;
  MODULEENTRY32 me;
  me.dwSize = sizeof(me);
  bool found = false;
  if (Module32First(snap, &me))
  {
    do
    {
      if (_stricmp(me.szModule, leaf) == 0)
      {
        found = true;
        break;
      }
    } while (Module32Next(snap, &me));
  }
  CloseHandle(snap);
  return found;
}

struct WindowSearch
{
  DWORD pid;
  HWND hwnd;
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM param)
{
  WindowSearch *search = (WindowSearch *)param;
  DWORD pid = 0;
  if (g_getWindowThreadProcessId)
    g_getWindowThreadProcessId(hwnd, &pid);
  if (pid == search->pid)
  {
    search->hwnd = hwnd;
    return FALSE;
  }
  return TRUE;
}

DWORD FindWindowThread(DWORD pid, HWND *window)
{
  WindowSearch search = {pid, nullptr};
  if (g_enumWindows)
    g_enumWindows(FindWindowProc, (LPARAM)&search);
  if (window)
    *window = search.hwnd;
  return search.hwnd && g_getWindowThreadProcessId ? g_getWindowThreadProcessId(search.hwnd, nullptr) : 0;
}

bool WaitForModule(DWORD pid, const char *leaf, int timeoutMs)
{
  const ULONGLONG start = GetTickCount64();
  while (GetTickCount64() - start < (ULONGLONG)timeoutMs)
  {
    if (ModuleLoaded(pid, leaf))
      return true;
    Sleep(50);
  }
  return false;
}

bool InjectWithHook(DWORD pid, HHOOK *hookOut, char *why, int whyCap)
{
  HWND window = nullptr;
  const DWORD tid = FindWindowThread(pid, &window);
  if (!tid || !g_payload || !g_hookProc || !g_setWindowsHookEx || !g_unhookWindowsHookEx)
    return false;

  SetLastError(0);
  HHOOK hook = g_setWindowsHookEx(WH_GETMESSAGE, g_hookProc, g_payload, tid);
  if (!hook)
  {
    _snprintf(why, whyCap, "SetWindowsHookEx failed (%lu)", GetLastError());
    return false;
  }
  const ULONGLONG start = GetTickCount64();
  while (GetTickCount64() - start < 1500 && !ModuleLoaded(pid, kHookLeaf))
  {
    if (window)
      g_postMessage(window, WM_NULL, 0, 0);
    Sleep(50);
  }
  if (!ModuleLoaded(pid, kHookLeaf))
  {
    g_unhookWindowsHookEx(hook);
    _snprintf(why, whyCap, "the hook was installed but the payload did not arrive");
    return false;
  }
  if (hookOut)
    *hookOut = hook;
  return true;
}

bool InjectWithRemoteThread(DWORD pid, char *why, int whyCap)
{
  HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                             PROCESS_VM_WRITE | PROCESS_VM_READ,
                         FALSE, pid);
  if (!p)
  {
    _snprintf(why, whyCap, "OpenProcess failed (%lu)", GetLastError());
    return false;
  }
  if (Is32BitTarget(p))
  {
    _snprintf(why, whyCap, "32-bit process (the first version carries an x64 payload only)");
    CloseHandle(p);
    return false;
  }
  const SIZE_T len = strlen(g_payloadPath) + 1;
  void *remote = VirtualAllocEx(p, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote)
  {
    _snprintf(why, whyCap, "VirtualAllocEx failed (%lu)", GetLastError());
    CloseHandle(p);
    return false;
  }
  SIZE_T wrote = 0;
  bool ok = false;
  HANDLE th = nullptr;
  DWORD exitCode = 0;
  if (WriteProcessMemory(p, remote, g_payloadPath, len, &wrote) && wrote == len)
  {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC load = GetProcAddress(k32, "LoadLibraryA");
    th = CreateRemoteThread(p, nullptr, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, nullptr);
  }
  if (th)
  {
    const DWORD wait = WaitForSingleObject(th, 5000);
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    // LoadLibraryA returns the module handle as the thread's exit code. That is stronger evidence than a module
    // enumeration that can be denied by an AppContainer/package boundary: several real WebView2 children loaded
    // the payload while their module list remained unreadable.
    ok = wait == WAIT_OBJECT_0 && exitCode != 0;
    if (ok && !ModuleLoaded(pid, kHookLeaf))
      LogLine("pid %lu loaded the payload (returned 0x%08lX) but its module list is not readable",
              (unsigned long)pid, (unsigned long)exitCode);
  }
  if (!ok)
    _snprintf(why, whyCap, "remote LoadLibrary failed (thread exit 0x%08lX, error %lu)",
              (unsigned long)exitCode, GetLastError());
  if (remote)
    VirtualFreeEx(p, remote, 0, MEM_RELEASE);
  CloseHandle(p);
  return ok;
}

int FindTracked(DWORD pid)
{
  for (int i = 0; i < g_trackedCount; ++i)
    if (g_tracked[i].pid == pid)
      return i;
  return -1;
}

void RemoveTrackedAt(int i)
{
  if (i < 0 || i >= g_trackedCount)
    return;
  if (g_tracked[i].hook)
    if (g_unhookWindowsHookEx)
      g_unhookWindowsHookEx(g_tracked[i].hook);
  for (int k = i; k < g_trackedCount - 1; ++k)
    g_tracked[k] = g_tracked[k + 1];
  --g_trackedCount;
}

bool ProcessAlive(DWORD pid)
{
  HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return false;
  const DWORD state = WaitForSingleObject(p, 0);
  CloseHandle(p);
  return state == WAIT_TIMEOUT;
}

bool InjectProcess(DWORD pid, const char *exe, Tracked *tracked)
{
  char why[120] = {0};
  HHOOK hook = nullptr;
  if (InjectWithHook(pid, &hook, why, (int)sizeof(why)) ||
      InjectWithRemoteThread(pid, why, (int)sizeof(why)))
  {
    tracked->pid = pid;
    tracked->hook = hook;
    tracked->injected = true;
    tracked->why[0] = 0;
    _snprintf(tracked->exe, sizeof(tracked->exe), "%s", exe);
    LogLine("attached to pid %lu (%s)%s", (unsigned long)pid, exe, hook ? " via message hook" : "");
    return true;
  }
  tracked->pid = pid;
  tracked->hook = nullptr;
  tracked->injected = false;
  tracked->nextTryMs = GetTickCount64() + 30000;
  _snprintf(tracked->exe, sizeof(tracked->exe), "%s", exe);
  _snprintf(tracked->why, sizeof(tracked->why), "%s", why);
  LogLine("could not attach to pid %lu (%s): %s", (unsigned long)pid, exe, why);
  return false;
}

void ScanProcesses(const appproxy::Config &cfg, Status *status)
{
  static const int kMaxProcesses = 1024;
  appproxy::ProcessNode rows[kMaxProcesses];
  int rowCount = 0;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return;
  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);
  if (Process32First(snap, &pe))
  {
    do
    {
      if (rowCount >= kMaxProcesses)
        break;
      appproxy::ProcessNode &r = rows[rowCount++];
      r.pid = (unsigned long)pe.th32ProcessID;
      r.parent = (unsigned long)pe.th32ParentProcessID;
      ExeLeaf(pe.szExeFile, r.exe, (int)sizeof(r.exe));
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);

  bool seen[kMaxTracked] = {false};
  for (int row = 0; row < rowCount; ++row)
  {
    const DWORD pid = rows[row].pid;
    const char *exe = rows[row].exe;
    if (pid == 0 || pid == GetCurrentProcessId())
      continue;
    // Apex's own two processes are never proxy targets, even if a hand-edited wildcard says so. Injecting the
    // payload into the settings browser would make the page's own network code a target and would also put a
    // hook inside Apex's independent UI process.
    if (apex::match::Pattern(exe, "apex.exe") || apex::match::Pattern(exe, "apex-settings.exe"))
      continue;
    if (!appproxy::InSelectedTree(cfg, rows, rowCount, row))
      continue;
    ++status->matched;
    int at = FindTracked(pid);
    if (at >= 0)
      seen[at] = true;
    if (at >= 0 && (g_tracked[at].injected || GetTickCount64() < g_tracked[at].nextTryMs))
    {
      if (g_tracked[at].injected)
        ++status->active;
      else
        ++status->failed;
      continue;
    }
    if (at < 0 && g_trackedCount >= kMaxTracked)
      continue;
    if (at < 0)
    {
      at = g_trackedCount++;
      seen[at] = true;
      g_tracked[at] = Tracked{};
    }
    if (InjectProcess(pid, exe, &g_tracked[at]))
      ++status->active;
    else
      ++status->failed;
  }

  for (int i = g_trackedCount - 1; i >= 0; --i)
  {
    if (!seen[i] || !ProcessAlive(g_tracked[i].pid))
      RemoveTrackedAt(i);
  }
}

void UnhookAll()
{
  while (g_trackedCount > 0)
    RemoveTrackedAt(g_trackedCount - 1);
}

DWORD WINAPI WorkerProc(LPVOID)
{
  while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
  {
    Status next;
    next.enabled = CurrentFeatureEnabled() ? 1 : 0;
    if (ServiceRunning())
    {
      SetProxyActive(false);
      UnhookAll();
      EnterCriticalSection(&g_lock);
      g_status = next;
      LeaveCriticalSection(&g_lock);
      WaitForSingleObject(g_wake, kScanMs);
      continue;
    }
    appproxy::Config cfg = CopyConfig();
    const bool ready = next.enabled && ConfigReady(cfg);
    SetProxyActive(ready);
    if (!ready)
      UnhookAll();
    else
      ScanProcesses(cfg, &next);

    EnterCriticalSection(&g_lock);
    g_status = next;
    LeaveCriticalSection(&g_lock);

    WaitForSingleObject(g_wake, kScanMs);
  }
  SetProxyActive(false);
  UnhookAll();
  return 0;
}

void Append(char *out, int outSize, int &off, const char *fmt, ...)
{
  if (off >= outSize - 1)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = _vsnprintf(out + off, outSize - off - 1, fmt, ap);
  va_end(ap);
  if (n > 0)
    off += n;
  if (off > outSize - 1)
    off = outSize - 1;
}

void AppendJson(char *out, int outSize, int &off, const char *s)
{
  Append(out, outSize, off, "\"");
  for (const char *p = s; p && *p; ++p)
  {
    if (*p == '"' || *p == '\\')
      Append(out, outSize, off, "\\%c", *p);
    else if (*p == '\n')
      Append(out, outSize, off, "\\n");
    else if (*p == '\r')
      Append(out, outSize, off, "\\r");
    else
      Append(out, outSize, off, "%c", *p);
  }
  Append(out, outSize, off, "\"");
}

int SettingsJson(char *out, int outSize)
{
  appproxy::Config c;
  Status status;
  EnterCriticalSection(&g_lock);
  c = g_cfg;
  status = g_status;
  LeaveCriticalSection(&g_lock);
  if (!out || outSize < 128)
    return 0;

  int o = 0;
  Append(out, outSize, o, "{\"summaryZh\":");
  AppendJson(out, outSize, o, "为不同程序指定不同的 SOCKS5 或 HTTP 代理");
  Append(out, outSize, o, ",\"summaryEn\":");
  AppendJson(out, outSize, o, "Routes different programs through different SOCKS5 or HTTP CONNECT profiles");
  Append(out, outSize, o, ",\"settingsFile\":");
  AppendJson(out, outSize, o, "AppProxy.ini");
  Append(out, outSize, o, ",\"params\":[");

  appproxy::ServiceStatus serviceStatus;
  const bool statusRead = ReadServiceStatus(&serviceStatus);
  const bool serviceRunning = ServiceRunning();
  char noteZh[900] = {0};
  char noteEn[900] = {0};
  BuildStatusNotes(status, c, serviceRunning, serviceStatus, noteZh, sizeof(noteZh), noteEn,
                   sizeof(noteEn));
  const char *level = "warn";
  if (!status.enabled || !ConfigReady(c) || !serviceRunning ||
      (statusRead && appproxy::EqNoCase(serviceStatus.state, "error")) ||
      (statusRead && appproxy::EqNoCase(serviceStatus.lastResult, "fail")))
    level = "error";
  else if (statusRead && serviceStatus.relayOk > 0 &&
           appproxy::EqNoCase(serviceStatus.lastResult, "ok"))
    level = "ok";
  Append(out, outSize, o,
         "{\"id\":\"serviceStatus\",\"type\":\"status\",\"level\":");
  AppendJson(out, outSize, o, level);
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "textZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, noteZh);
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "textEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, noteEn);
  Append(out, outSize, o, ",\"live\":true,\"lines\":[");
  for (int i = 0; i < appproxy::kRecentLines; ++i)
  {
    char lineZh[220] = {0};
    char lineEn[220] = {0};
    if (i < serviceStatus.recentCount)
    {
      char recent[160] = {0};
      appproxy::Copy(recent, sizeof(recent), serviceStatus.recent[i]);
      char *bar = strchr(recent, '|');
      if (bar)
      {
        *bar++ = 0;
        _snprintf(lineZh, sizeof(lineZh), "%s → %s", recent, bar);
        _snprintf(lineEn, sizeof(lineEn), "%s -> %s", recent, bar);
      }
      else
        appproxy::Copy(lineZh, sizeof(lineZh), recent);
    }
    if (!lineZh[0])
    {
      appproxy::Copy(lineZh, sizeof(lineZh), "暂无最近连接");
      appproxy::Copy(lineEn, sizeof(lineEn), "No recent connection");
    }
    if (!lineEn[0])
      appproxy::Copy(lineEn, sizeof(lineEn), lineZh);
    Append(out, outSize, o, "%s{\"textZh\":", i ? "," : "");
    AppendJson(out, outSize, o, lineZh);
    Append(out, outSize, o, ",\"textEn\":");
    AppendJson(out, outSize, o, lineEn);
    Append(out, outSize, o, "}");
  }
  Append(out, outSize, o, "]},");

  Append(out, outSize, o, "{\"id\":\"bypassLocal\",\"type\":\"bool\",\"value\":%d,",
         c.bypassLocal ? 1 : 0);
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "本机和局域网直连");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Bypass this PC and the LAN");
  Append(out, outSize, o, "},");

  Append(out, outSize, o, "{\"id\":\"windivertDir\",\"type\":\"text\",\"value\":");
  AppendJson(out, outSize, o, c.windivertDir);
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "placeholderZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "包含 WinDivert.dll 和 WinDivert64.sys 的文件夹");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "placeholderEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Folder containing WinDivert.dll and WinDivert64.sys");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "WinDivert 目录（不随 Apex 分发）");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "WinDivert folder (not shipped with Apex)");
  Append(out, outSize, o, "},");

  Append(out, outSize, o,
         "{\"id\":\"serviceAction\",\"type\":\"select\",\"value\":\"install\",\"options\":["
         "{\"value\":\"install\",\"labelZh\":\"安装/修复服务\",\"labelEn\":\"Install / repair service\"},"
         "{\"value\":\"start\",\"labelZh\":\"启动服务\",\"labelEn\":\"Start service\"},"
         "{\"value\":\"stop\",\"labelZh\":\"停止服务\",\"labelEn\":\"Stop service\"},"
         "{\"value\":\"uninstall\",\"labelZh\":\"卸载服务\",\"labelEn\":\"Uninstall service\"}],");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "系统服务");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "System service");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "applyZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "执行");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "applyEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Run");
  Append(out, outSize, o, "},");

  Append(out, outSize, o, "{\"id\":\"blockQuic\",\"type\":\"bool\",\"value\":%d,",
         c.blockQuic ? 1 : 0);
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "阻止 QUIC，强制应用回退到 TCP 代理");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Block QUIC so the app falls back to the TCP proxy");
  Append(out, outSize, o, "},");

  // Proxy profiles. Each row is one server; applications below choose one by name.
  Append(out, outSize, o,
         "{\"id\":\"proxies\",\"type\":\"group\",\"layout\":\"rows\",\"fieldsWrap\":true,"
         "\"addHintZh\":\"代理名称，例如 Home\",\"addHintEn\":\"Proxy name, e.g. Home\",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "代理服务器");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Proxy servers");
  Append(out, outSize, o, ",\"fields\":[");
  Append(out, outSize, o,
         "{\"id\":\"kind\",\"type\":\"select\",\"value\":\"socks5\",\"options\":["
         "{\"value\":\"socks5\",\"labelZh\":\"SOCKS5\",\"labelEn\":\"SOCKS5\"},"
         "{\"value\":\"http\",\"labelZh\":\"HTTP CONNECT\",\"labelEn\":\"HTTP CONNECT\"}],");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "类型");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Type");
  Append(out, outSize, o, "},");
  Append(out, outSize, o, "{\"id\":\"server\",\"type\":\"text\",");
  AppendJson(out, outSize, o, "placeholderZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "127.0.0.1");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "placeholderEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "127.0.0.1");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "服务器");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Server");
  Append(out, outSize, o, "},");
  Append(out, outSize, o, "{\"id\":\"port\",\"type\":\"text\",");
  AppendJson(out, outSize, o, "placeholderZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "1080");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "placeholderEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "1080");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "端口");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Port");
  Append(out, outSize, o, "},");
  Append(out, outSize, o, "{\"id\":\"username\",\"type\":\"text\",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "用户名");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Username");
  Append(out, outSize, o, "},");
  Append(out, outSize, o, "{\"id\":\"password\",\"type\":\"text\",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "密码（明文保存在本文件夹）");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Password (plain text in this folder)");
  Append(out, outSize, o, "}],\"items\":[");
  for (int i = 0; i < c.profileCount; ++i)
  {
    const appproxy::Profile &p = c.profiles[i];
    char port[16] = {0};
    _snprintf(port, sizeof(port), "%d", p.port);
    Append(out, outSize, o, "%s{\"title\":", i ? "," : "");
    AppendJson(out, outSize, o, p.name);
    Append(out, outSize, o, ",\"values\":{\"kind\":\"%s\",\"server\":",
           p.kind == appproxy::kHttpConnect ? "http" : "socks5");
    AppendJson(out, outSize, o, p.server);
    Append(out, outSize, o, ",\"port\":");
    AppendJson(out, outSize, o, port);
    Append(out, outSize, o, ",\"username\":");
    AppendJson(out, outSize, o, p.username);
    Append(out, outSize, o, ",\"password\":");
    AppendJson(out, outSize, o, p.password);
    Append(out, outSize, o, "}}");
  }
  Append(out, outSize, o, "]},");

  Append(out, outSize, o,
         "{\"id\":\"apps\",\"type\":\"group\",\"layout\":\"rows\",\"rowToggle\":[\"enabled\"],");
  AppendJson(out, outSize, o, "addHintZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "程序名，例如 firefox.exe");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "addHintEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Program name, e.g. firefox.exe");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "走代理的程序");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Programs using the proxy");
  Append(out, outSize, o, ",\"fields\":[{\"id\":\"enabled\",\"type\":\"bool\",");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "使用代理");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Use proxy");
  Append(out, outSize, o, "},{\"id\":\"proxy\",\"type\":\"select\",\"value\":\"\",\"options\":[");
  for (int i = 0; i < c.profileCount; ++i)
  {
    Append(out, outSize, o, "%s{\"value\":", i ? "," : "");
    AppendJson(out, outSize, o, c.profiles[i].name);
    Append(out, outSize, o, ",\"labelZh\":");
    AppendJson(out, outSize, o, c.profiles[i].name);
    Append(out, outSize, o, ",\"labelEn\":");
    AppendJson(out, outSize, o, c.profiles[i].name);
    Append(out, outSize, o, "}");
  }
  Append(out, outSize, o, "],");
  AppendJson(out, outSize, o, "labelZh");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "使用代理");
  Append(out, outSize, o, ",");
  AppendJson(out, outSize, o, "labelEn");
  Append(out, outSize, o, ":");
  AppendJson(out, outSize, o, "Proxy");
  Append(out, outSize, o, "}],\"items\":[");
  for (int i = 0; i < c.ruleCount; ++i)
  {
    Append(out, outSize, o, "%s{\"title\":", i ? "," : "");
    AppendJson(out, outSize, o, c.rules[i].pattern);
    Append(out, outSize, o, ",\"values\":{\"enabled\":%d,\"proxy\":",
           c.rules[i].enabled ? 1 : 0);
    AppendJson(out, outSize, o, c.rules[i].profile);
    Append(out, outSize, o, "}}");
  }
  Append(out, outSize, o, "]}]");
  Append(out, outSize, o, "}");
  return o;
}

bool SafeText(const char *v)
{
  if (!v)
    return false;
  for (const char *p = v; *p; ++p)
    if (*p == '\r' || *p == '\n')
      return false;
  return true;
}

int SetControl(const char *path, const char *value)
{
  if (!path || !value || !SafeText(value))
    return 0;
  if (strcmp(path, "serviceAction") == 0)
    return RunServiceAction(value) ? 1 : 0;
  bool changed = false;
  EnterCriticalSection(&g_lock);
  if (strcmp(path, "bypassLocal") == 0)
  {
    if ((value[0] == '0' || value[0] == '1') && value[1] == 0)
    {
      g_cfg.bypassLocal = value[0] == '1' ? 1 : 0;
      changed = true;
    }
  }
  else if (strcmp(path, "blockQuic") == 0)
  {
    if ((value[0] == '0' || value[0] == '1') && value[1] == 0)
    {
      g_cfg.blockQuic = value[0] == '1' ? 1 : 0;
      changed = true;
    }
  }
  else if (strcmp(path, "windivertDir") == 0)
  {
    appproxy::Copy(g_cfg.windivertDir, (int)sizeof(g_cfg.windivertDir), value);
    changed = true;
  }
  else if (strncmp(path, "proxies[", 8) == 0)
  {
    const char *dot = strchr(path, '.');
    const int index = atoi(path + 8);
    if (dot && index >= 0 && index < g_cfg.profileCount)
    {
      appproxy::Profile &p = g_cfg.profiles[index];
      if (strcmp(dot + 1, "kind") == 0)
      {
        if (_stricmp(value, "socks5") == 0)
        {
          p.kind = appproxy::kSocks5;
          changed = true;
        }
        else if (_stricmp(value, "http") == 0)
        {
          p.kind = appproxy::kHttpConnect;
          changed = true;
        }
      }
      else if (strcmp(dot + 1, "server") == 0)
      {
        bool safe = true;
        for (const char *q = value; *q; ++q)
          if (*q == ' ' || *q == '\t' || *q == '/' || *q == '\\' || *q == '|')
            safe = false;
        if (safe)
        {
          appproxy::Copy(p.server, (int)sizeof(p.server), value);
          changed = true;
        }
      }
      else if (strcmp(dot + 1, "port") == 0)
      {
        int port = 0;
        if (appproxy::ParseInt(value, &port) && port >= 1 && port <= 65535)
        {
          p.port = port;
          changed = true;
        }
      }
      else if (strcmp(dot + 1, "username") == 0)
      {
        if (!strchr(value, '|'))
        {
          appproxy::Copy(p.username, (int)sizeof(p.username), value);
          changed = true;
        }
      }
      else if (strcmp(dot + 1, "password") == 0)
      {
        if (!strchr(value, '|'))
        {
          appproxy::Copy(p.password, (int)sizeof(p.password), value);
          changed = true;
        }
      }
    }
  }
  else if (strncmp(path, "apps[", 5) == 0)
  {
    const char *dot = strchr(path, '.');
    const int index = atoi(path + 5);
    if (dot && index >= 0 && index < g_cfg.ruleCount)
    {
      if (strcmp(dot + 1, "enabled") == 0 &&
          (value[0] == '0' || value[0] == '1') && value[1] == 0)
      {
        g_cfg.rules[index].enabled = value[0] == '1' ? 1 : 0;
        changed = true;
      }
      else if (strcmp(dot + 1, "proxy") == 0 && appproxy::FindProfile(&g_cfg, value) >= 0)
      {
        appproxy::Copy(g_cfg.rules[index].profile, (int)sizeof(g_cfg.rules[index].profile), value);
        changed = true;
      }
    }
  }
  LeaveCriticalSection(&g_lock);
  if (changed)
  {
    SaveSettings();
    if (g_wake)
      SetEvent(g_wake);
  }
  return changed ? 1 : 0;
}

int ListOp(const char *id, const char *op, const char *value, int index)
{
  if (!id || !op)
    return 0;
  bool changed = false;
  if (strcmp(id, "proxies") == 0)
  {
    if (strcmp(op, "add") == 0 && value && *value)
    {
      EnterCriticalSection(&g_lock);
      changed = appproxy::AddProfile(&g_cfg, value);
      LeaveCriticalSection(&g_lock);
    }
    else if (strcmp(op, "remove") == 0)
    {
      EnterCriticalSection(&g_lock);
      changed = appproxy::RemoveProfile(&g_cfg, index);
      LeaveCriticalSection(&g_lock);
    }
  }
  else if (strcmp(id, "apps") == 0)
  {
    if (strcmp(op, "add") == 0 && value && *value)
    {
      char normal[appproxy::kMaxPattern] = {0};
      apex::match::NormaliseName(value, normal, (int)sizeof(normal));
      if (apex::match::Pattern(normal, "apex.exe") ||
          apex::match::Pattern(normal, "apex-settings.exe"))
        return 0;
      EnterCriticalSection(&g_lock);
      changed = appproxy::AddRule(&g_cfg, value, 1,
                                  g_cfg.profileCount > 0 ? g_cfg.profiles[0].name : "");
      LeaveCriticalSection(&g_lock);
    }
    else if (strcmp(op, "remove") == 0)
    {
      EnterCriticalSection(&g_lock);
      changed = appproxy::RemoveRule(&g_cfg, index);
      LeaveCriticalSection(&g_lock);
    }
  }
  if (changed)
  {
    SaveSettings();
    if (g_wake)
      SetEvent(g_wake);
  }
  return changed ? 1 : 0;
}

unsigned Flags()
{
  return APEX_FEATURE_ENABLED;
}

int Init(const ApexHost *host)
{
  if (!host || host->abiVersion != APEX_ABI_VERSION || host->structSize < sizeof(ApexHost))
    return 1;
  g_host = host;
  if (!host->featureDir || host->featureDir(g_dir, (int)sizeof(g_dir)) <= 0)
    return 2;
  if (g_dir[0] && g_dir[strlen(g_dir) - 1] != '\\')
  {
    const size_t n = strlen(g_dir);
    if (n + 2 < sizeof(g_dir))
    {
      g_dir[n] = '\\';
      g_dir[n + 1] = 0;
    }
  }
  FillPath("AppProxy.ini", g_configPath, (int)sizeof(g_configPath));
  FillPath("appproxy.log", g_logPath, (int)sizeof(g_logPath));

  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  appproxy::Defaults(&g_cfg);
  RotateLog();
  LoadSettings();
  if (!g_cfg.windivertDir[0] &&
      GetFileAttributesA("C:\\Program Files\\ProxyBridge\\WinDivert.dll") != INVALID_FILE_ATTRIBUTES)
  {
    appproxy::Copy(g_cfg.windivertDir, (int)sizeof(g_cfg.windivertDir), "C:\\Program Files\\ProxyBridge");
    SaveSettings();
    LogLine("WinDivert defaulted to the installed ProxyBridge folder");
  }
  g_status.enabled = CurrentFeatureEnabled() ? 1 : 0;

  if (GetEnvironmentVariableA("APEX_APPPROXY_DRY", nullptr, 0) > 0)
  {
    LogLine("started in DRY mode -- no process is scanned or injected");
    return 0;
  }

  if (!PrepareRuntimePayload())
    return 0;
  EnsureEnabledEvent();
  if (!LoadUser32())
    LogLine("user32 could not be loaded; message-hook injection is unavailable (remote-thread fallback remains)");

  g_payload = LoadLibraryA(g_payloadPath);
  if (!g_payload)
    LogLine("payload is missing or would not load: %s (error %lu)", g_payloadPath, GetLastError());
  else
  {
    g_hookProc = (HOOKPROC)(void *)GetProcAddress(g_payload, "ApexHookProc");
    if (!g_hookProc)
      LogLine("payload has no ApexHookProc export");
  }

  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
  if (!g_wake || !g_thread)
  {
    LogLine("worker could not start (error %lu)", GetLastError());
    if (g_thread)
      CloseHandle(g_thread);
    g_thread = nullptr;
    if (g_wake)
      CloseHandle(g_wake);
    g_wake = nullptr;
    if (g_payload)
      FreeLibrary(g_payload);
    g_payload = nullptr;
    return 3;
  }
  LogLine("started (%s)",
          ConfigReady(g_cfg) ? "a proxy is configured; watching for selected programs"
                             : "waiting for a proxy and at least one program");
  return 0;
}

void Shutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_wake)
    SetEvent(g_wake);
  if (g_thread)
  {
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  SetProxyActive(false);
  UnhookAll();
  if (g_enabledEvent)
  {
    CloseHandle(g_enabledEvent);
    g_enabledEvent = nullptr;
  }
  if (g_payload)
  {
    FreeLibrary(g_payload);
    g_payload = nullptr;
    g_hookProc = nullptr;
  }
  if (g_lockReady)
  {
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
  }
  g_host = nullptr;
}

ApexFeature kFeature;

} // namespace

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  // Filled on every call rather than by a dynamic global initialiser. The
  // `.crtless` build omits the CRT startup chain, so there must be no object
  // that requires a constructor before the host can read the contract.
  kFeature.abiVersion = APEX_ABI_VERSION;
  kFeature.structSize = sizeof(ApexFeature);
  kFeature.id = kId;
  kFeature.nameZh = "应用代理";
  kFeature.nameEn = "App Proxy";
  kFeature.version = kVersion;
  kFeature.init = Init;
  kFeature.shutdown = Shutdown;
  kFeature.reloadSettings = ReloadSettings;
  kFeature.settingsJson = SettingsJson;
  kFeature.setControl = SetControl;
  kFeature.listOp = ListOp;
  kFeature.quickItems = nullptr;
  kFeature.saveSettings = SaveSettings;
  kFeature.onWheel = nullptr;
  kFeature.tick = nullptr;
  kFeature.flags = Flags;
  return &kFeature;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(inst);
  return TRUE;
}
