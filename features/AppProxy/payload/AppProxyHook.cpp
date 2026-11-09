// ---------------------------------------------------------------------------
// AppProxyHook -- the user-mode payload loaded into an application selected on
// the AppProxy page.
//
// WHAT IT DOES. It replaces the program's IAT entries for Winsock's `connect`,
// `WSAConnect` and `WSAIoctl` (the route by which Chromium obtains ConnectEx).
// A matching blocking connect is sent to the configured proxy first, the HTTP
// CONNECT / SOCKS5 handshake is completed on that same socket, and only then
// does the program receive success. ConnectEx keeps its asynchronous completion
// shape by completing immediately to a local bridge, which performs the proxy
// handshake on a second socket and relays the stream.
//
// WHAT IT DELIBERATELY DOES NOT DO. It knows nothing about UDP, QUIC, DNS or a
// program's own TLS implementation. Those are the honest boundaries of a
// portable, no-driver first version. A UDP connection is left alone; a 32-bit or
// protected process is rejected by the feature before this payload is loaded.
//
// NO C RUNTIME. This file may be loaded into programs built with a different
// CRT, or with no CRT at all. It therefore avoids stdio, stdlib, std::string and
// every allocation. The small string routines live in proxy_common.h.
// ---------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <tlhelp32.h>

#include "../proxy_common.h"
#include "../process_tree.h"

// The payload is linked with -nostdlib. These three tiny routines are the
// only C-runtime symbols the compiler emits for fixed-size struct copies and
// zeroing; providing them locally keeps the final PE dependent on Windows
// components only.
extern "C" void *memset(void *dst, int value, size_t count)
{
  unsigned char *p = (unsigned char *)dst;
  for (size_t i = 0; i < count; ++i)
    p[i] = (unsigned char)value;
  return dst;
}

extern "C" void *memcpy(void *dst, const void *src, size_t count)
{
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  for (size_t i = 0; i < count; ++i)
    d[i] = s[i];
  return dst;
}

namespace {

using appproxy::Config;

typedef int(WSAAPI *ConnectFn)(SOCKET, const struct sockaddr *, int);
typedef int(WSAAPI *WSAConnectFn)(SOCKET, const struct sockaddr *, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef int(WSAAPI *SelectFn)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
typedef int(WSAAPI *GetSockOptFn)(SOCKET, int, int, char *, int *);
typedef int(WSAAPI *GetAddrInfoFn)(const char *, const char *, const ADDRINFOA *, ADDRINFOA **);
typedef void(WSAAPI *FreeAddrInfoFn)(ADDRINFOA *);
typedef LRESULT(WINAPI *CallNextHookExFn)(HHOOK, int, WPARAM, LPARAM);
typedef int(WSAAPI *WSAIoctlFn)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD,
                                LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int(WSAAPI *SendToFn)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int(WSAAPI *WSASendToFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                 const struct sockaddr *, int, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);

const char *kEnabledEvent = "Local\\ApexAppProxyEnabled.2";
const DWORD kHandshakeTimeoutMs = 10000;

HMODULE g_self = nullptr;
bool g_skipInstall = false;
bool g_winsockReady = false;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

ConnectFn g_realConnect = nullptr;
WSAConnectFn g_realWSAConnect = nullptr;
SelectFn g_realSelect = nullptr;
GetSockOptFn g_realGetSockOpt = nullptr;
GetAddrInfoFn g_realGetAddrInfo = nullptr;
FreeAddrInfoFn g_realFreeAddrInfo = nullptr;
CallNextHookExFn g_callNextHookEx = nullptr;
WSAIoctlFn g_realWSAIoctl = nullptr;
LPFN_CONNECTEX g_realConnectEx = nullptr;
SendToFn g_realSendTo = nullptr;
WSASendToFn g_realWSASendTo = nullptr;

char g_configPath[MAX_PATH] = {0};
char g_logPath[MAX_PATH] = {0};
char g_exeName[96] = {0};
char g_payloadLeaf[128] = {0};
Config g_cfg;
bool g_cfgLoaded = false;
FILETIME g_cfgWrite = {0, 0};
DWORD g_cfgSize = 0;
ULONGLONG g_lastPatchMs = 0;
int g_lastPatchCount = 0;
volatile LONG g_calls = 0;
volatile LONG g_redirects = 0;
volatile LONG g_connectFailures = 0;
volatile LONG g_handshakeFailures = 0;
volatile LONG g_lastResult = -999;
volatile LONG g_lastWsaError = 0;
volatile LONG g_connectExCalls = 0;
volatile LONG g_firstConnectLogged = 0;
volatile LONG g_firstConnectExLogged = 0;
volatile LONG g_firstRelayLogged = 0;
volatile LONG g_firstHookConnect = 0;
volatile LONG g_firstHookWSAConnect = 0;
volatile LONG g_firstHookWSAIoctlConnectEx = 0;
volatile LONG g_firstHookConnectEx = 0;
volatile LONG g_firstQuicBlocked = 0;
volatile LONG g_firstContextDenied = 0;
volatile LONG g_firstProxyBypass = 0;
volatile LONG g_firstLocalBypass = 0;

struct TargetAddr
{
  int family;
  unsigned char bytes[16];
  int port;
};

// ConnectEx is commonly used by Chromium/WebView2. It is asynchronous and cannot be replaced by the same
// "finish the proxy handshake before returning" trick as connect(). Instead, a tiny per-process loopback
// bridge accepts the app's already-completed ConnectEx, performs the proxy handshake on a SECOND socket, and
// relays bytes. That preserves the original overlapped completion semantics.
struct BridgeMapping
{
  int family;
  unsigned short localPort;
  TargetAddr target;
  appproxy::Profile profile;
  ULONGLONG expires;
};

static const int kMaxBridgeMappings = 256;
BridgeMapping g_mappings[kMaxBridgeMappings];
int g_mappingCount = 0;
SOCKET g_bridge4 = INVALID_SOCKET;
SOCKET g_bridge6 = INVALID_SOCKET;
unsigned short g_bridge4Port = 0;
unsigned short g_bridge6Port = 0;
volatile LONG g_bridgeStarted = 0;
HANDLE g_bridgeThread4 = nullptr;
HANDLE g_bridgeThread6 = nullptr;

bool IsApexHost()
{
  char exe[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, exe, (int)sizeof(exe));
  const char *leaf = exe;
  for (const char *p = exe; *p; ++p)
    if (*p == '\\' || *p == '/')
      leaf = p + 1;
  return appproxy::EqNoCase(leaf, "apex.exe") || appproxy::EqNoCase(leaf, "apex-settings.exe");
}

void DirOf(char *path)
{
  for (int i = appproxy::Len(path) - 1; i >= 0; --i)
  {
    if (path[i] == '\\' || path[i] == '/')
    {
      path[i + 1] = 0;
      return;
    }
  }
}

void InitPaths(HMODULE self)
{
  char selfPath[MAX_PATH] = {0};
  GetModuleFileNameA(self, selfPath, (int)sizeof(selfPath));
  DirOf(selfPath);
  const char *payloadLeaf = selfPath;
  for (const char *p = selfPath; *p; ++p)
    if (*p == '\\' || *p == '/')
      payloadLeaf = p + 1;
  appproxy::Copy(g_payloadLeaf, (int)sizeof(g_payloadLeaf), payloadLeaf);

  char env[MAX_PATH] = {0};
  const DWORD n = GetEnvironmentVariableA("APEX_APPPROXY_CONFIG_PATH", env, (DWORD)sizeof(env));
  if (n > 0 && n < sizeof(env))
    appproxy::Copy(g_configPath, (int)sizeof(g_configPath), env);
  else
  {
    appproxy::Copy(g_configPath, (int)sizeof(g_configPath), selfPath);
    appproxy::Append(g_configPath, (int)sizeof(g_configPath),
                     appproxy::Len(g_configPath), "AppProxy.ini");
  }

  char exe[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, exe, (int)sizeof(exe));
  const char *leaf = exe;
  for (const char *p = exe; *p; ++p)
    if (*p == '\\' || *p == '/')
      leaf = p + 1;
  appproxy::Copy(g_exeName, (int)sizeof(g_exeName), leaf);

  appproxy::Copy(g_logPath, (int)sizeof(g_logPath), selfPath);
  appproxy::Append(g_logPath, (int)sizeof(g_logPath), appproxy::Len(g_logPath), "appproxy.log");
}

void NoteOnce(volatile LONG *flag, const char *text, DWORD code)
{
  if (!flag || !g_logPath[0] || InterlockedCompareExchange(flag, 1, 0) != 0)
    return;
  HANDLE f = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return;
  DWORD wrote = 0;
  WriteFile(f, text, (DWORD)appproxy::Len(text), &wrote, nullptr);
  WriteFile(f, " (pid ", 6, &wrote, nullptr);
  char pid[16];
  DWORD v = GetCurrentProcessId();
  int n = 0;
  char rev[16];
  if (!v)
    pid[n++] = '0';
  while (v)
  {
    rev[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  int o = 0;
  while (n)
    pid[o++] = rev[--n];
  WriteFile(f, pid, (DWORD)o, &wrote, nullptr);
  if (code != 0)
  {
    WriteFile(f, " error=", 7, &wrote, nullptr);
    char number[16];
    DWORD v2 = code;
    n = 0;
    if (!v2)
      number[n++] = '0';
    while (v2)
    {
      rev[n++] = (char)('0' + v2 % 10);
      v2 /= 10;
    }
    o = 0;
    while (n)
      number[o++] = rev[--n];
    WriteFile(f, number, (DWORD)o, &wrote, nullptr);
  }
  WriteFile(f, ")\r\n", 3, &wrote, nullptr);
  CloseHandle(f);
}

bool ReadWholeFile(const char *path, char *out, int cap, DWORD *sizeOut)
{
  if (!path || !out || cap <= 1)
    return false;
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return false;
  DWORD size = GetFileSize(f, nullptr);
  if (size == INVALID_FILE_SIZE || size > (DWORD)(cap - 1))
  {
    CloseHandle(f);
    return false;
  }
  DWORD got = 0;
  const BOOL ok = ReadFile(f, out, size, &got, nullptr);
  CloseHandle(f);
  if (!ok || got != size)
    return false;
  out[got] = 0;
  if (sizeOut)
    *sizeOut = got;
  return true;
}

void LoadConfigIfChanged()
{
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExA(g_configPath, GetFileExInfoStandard, &fa))
    return;
  const bool same = g_cfgLoaded && fa.nFileSizeLow == g_cfgSize &&
                    fa.ftLastWriteTime.dwLowDateTime == g_cfgWrite.dwLowDateTime &&
                    fa.ftLastWriteTime.dwHighDateTime == g_cfgWrite.dwHighDateTime;
  if (same)
    return;

  char text[appproxy::kConfigCap] = {0};
  DWORD size = 0;
  if (!ReadWholeFile(g_configPath, text, (int)sizeof(text), &size))
    return;
  appproxy::ParseConfig(text, (int)size, &g_cfg);
  g_cfgSize = fa.nFileSizeLow;
  g_cfgWrite = fa.ftLastWriteTime;
  g_cfgLoaded = true;
}

bool AliveAndEnabled()
{
  if (GetEnvironmentVariableA("APEX_APPPROXY_TEST", nullptr, 0) > 0)
    return true;
  HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, kEnabledEvent);
  if (!ev)
    return false;
  const DWORD state = WaitForSingleObject(ev, 0);
  CloseHandle(ev);
  return state == WAIT_OBJECT_0;
}

int CurrentProcessRule(const Config &cfg)
{
  const int direct = appproxy::RuleForExe(&cfg, g_exeName);
  if (direct >= 0)
    return direct;
  const int kMaxProcesses = 1024;
  appproxy::ProcessNode rows[kMaxProcesses];
  int count = 0;
  int self = -1;
  const DWORD selfPid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return -1;
  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);
  if (Process32First(snap, &pe))
  {
    do
    {
      if (count >= kMaxProcesses)
        break;
      appproxy::ProcessNode &r = rows[count];
      r.pid = (unsigned long)pe.th32ProcessID;
      r.parent = (unsigned long)pe.th32ParentProcessID;
      char leaf[64] = {0};
      const char *name = pe.szExeFile;
      for (const char *p = pe.szExeFile; *p; ++p)
        if (*p == '\\' || *p == '/')
          name = p + 1;
      appproxy::Copy(r.exe, (int)sizeof(r.exe), name);
      if (pe.th32ProcessID == selfPid)
        self = count;
      ++count;
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  return self >= 0 ? appproxy::RuleForSelectedTree(cfg, rows, count, self) : -1;
}

bool ActiveContext(Config *configOut, appproxy::Profile *profileOut)
{
  EnterCriticalSection(&g_lock);
  LoadConfigIfChanged();
  const bool testMode = GetEnvironmentVariableA("APEX_APPPROXY_TEST", nullptr, 0) > 0;
  const bool sessionMatches = testMode ||
                              (g_cfg.session[0] && appproxy::EqNoCase(g_cfg.session, g_payloadLeaf));
  const bool alive = AliveAndEnabled();
  bool active = alive && sessionMatches && appproxy::Valid(&g_cfg);
  appproxy::Profile profile = {};
  int denyReason = 0;
  if (!alive)
    denyReason = 1;
  else if (!sessionMatches)
    denyReason = 2;
  else if (!appproxy::Valid(&g_cfg))
    denyReason = 3;
  if (active)
  {
    const int rule = CurrentProcessRule(g_cfg);
    const int profileIndex = rule >= 0 ? appproxy::FindProfile(&g_cfg, g_cfg.rules[rule].profile) : -1;
    active = profileIndex >= 0 && appproxy::ProfileValid(&g_cfg.profiles[profileIndex]);
    if (active)
      profile = g_cfg.profiles[profileIndex];
    else
      denyReason = 4;
  }
  if (active && configOut)
    *configOut = g_cfg;
  if (active && profileOut)
    *profileOut = profile;
  LeaveCriticalSection(&g_lock);
  if (!active)
    NoteOnce(&g_firstContextDenied,
             "AppProxy: first connection was bypassed by the feature context", (DWORD)denyReason);
  return active;
}

bool ShouldRedirect(SOCKET s)
{
  int type = 0;
  int len = (int)sizeof(type);
  if (!g_realGetSockOpt ||
      g_realGetSockOpt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &len) != 0 ||
      type != SOCK_STREAM)
    return false;
  return ActiveContext(nullptr, nullptr);
}

bool TargetFromSockaddr(const struct sockaddr *sa, int len, TargetAddr *out)
{
  if (!sa || !out)
    return false;
  if (sa->sa_family == AF_INET && len >= (int)sizeof(sockaddr_in))
  {
    const sockaddr_in *v4 = (const sockaddr_in *)sa;
    out->family = AF_INET;
    const unsigned char *p = (const unsigned char *)&v4->sin_addr;
    for (int i = 0; i < 4; ++i)
      out->bytes[i] = p[i];
    for (int i = 4; i < 16; ++i)
      out->bytes[i] = 0;
    out->port = (int)ntohs(v4->sin_port);
    return true;
  }
  if (sa->sa_family == AF_INET6 && len >= (int)sizeof(sockaddr_in6))
  {
    const sockaddr_in6 *v6 = (const sockaddr_in6 *)sa;
    out->family = AF_INET6;
    const unsigned char *p = (const unsigned char *)&v6->sin6_addr;
    for (int i = 0; i < 16; ++i)
      out->bytes[i] = p[i];
    out->port = (int)ntohs(v6->sin6_port);
    return true;
  }
  return false;
}

bool IsLocalTarget(const TargetAddr &t)
{
  if (t.family == AF_INET)
  {
    if (t.bytes[0] == 0 || t.bytes[0] == 127)
      return true;
    if (t.bytes[0] == 10)
      return true;
    if (t.bytes[0] == 172 && (t.bytes[1] & 0xF0) == 16)
      return true;
    if (t.bytes[0] == 192 && t.bytes[1] == 168)
      return true;
    if (t.bytes[0] == 169 && t.bytes[1] == 254)
      return true;
  }
  else if (t.family == AF_INET6)
  {
    bool zero = true;
    for (int i = 0; i < 15; ++i)
      if (t.bytes[i] != 0)
        zero = false;
    if (zero && t.bytes[15] == 1)
      return true;
    if ((t.bytes[0] & 0xFE) == 0xFC)
      return true;
    if (t.bytes[0] == 0xFE && (t.bytes[1] & 0xC0) == 0x80)
      return true;
  }
  return false;
}

bool ShouldRejectQuic(SOCKET s, const struct sockaddr *name, int namelen)
{
  if (!name)
    return false;
  int type = 0;
  int typeLen = (int)sizeof(type);
  if (!g_realGetSockOpt ||
      g_realGetSockOpt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &typeLen) != 0 ||
      type != SOCK_DGRAM)
    return false;
  Config cfg;
  if (!ActiveContext(&cfg, nullptr) || !cfg.blockQuic)
    return false;
  TargetAddr target;
  if (!TargetFromSockaddr(name, namelen, &target) || target.port != 443 || IsLocalTarget(target))
    return false;
  NoteOnce(&g_firstQuicBlocked,
           "AppProxy: rejected QUIC/UDP 443 so the application falls back to proxied TCP", 0);
  return true;
}

bool SameAddress(const sockaddr *sa, int len, const TargetAddr &t)
{
  TargetAddr other;
  if (!TargetFromSockaddr(sa, len, &other))
    return false;
  if (other.family != t.family)
    return false;
  const int n = t.family == AF_INET ? 4 : 16;
  for (int i = 0; i < n; ++i)
    if (other.bytes[i] != t.bytes[i])
      return false;
  return true;
}

bool WaitSocket(SOCKET s, bool writable, DWORD timeoutMs)
{
  if (!g_realSelect)
    return false;
  const ULONGLONG start = GetTickCount64();
  for (;;)
  {
    fd_set readSet;
    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    if (writable)
      FD_SET(s, &writeSet);
    else
      FD_SET(s, &readSet);
    FD_SET(s, &errorSet);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    const int rc = g_realSelect(0, writable ? nullptr : &readSet, writable ? &writeSet : nullptr, &errorSet, &tv);
    if (rc > 0)
      return FD_ISSET(s, writable ? &writeSet : &readSet) || FD_ISSET(s, &errorSet);
    if (rc == SOCKET_ERROR)
      return false;
    if (GetTickCount64() - start >= timeoutMs)
      return false;
  }
}

bool SendAll(SOCKET s, const char *data, int len)
{
  int sent = 0;
  while (sent < len)
  {
    if (!WaitSocket(s, true, kHandshakeTimeoutMs))
      return false;
    const int n = send(s, data + sent, len - sent, 0);
    if (n == SOCKET_ERROR)
    {
      if (WSAGetLastError() == WSAEWOULDBLOCK)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    sent += n;
  }
  return true;
}

bool RecvSome(SOCKET s, char *data, int cap, int *got)
{
  if (!got)
    return false;
  *got = 0;
  if (!WaitSocket(s, false, kHandshakeTimeoutMs))
    return false;
  const int n = recv(s, data, cap, 0);
  if (n <= 0)
    return false;
  *got = n;
  return true;
}

bool RecvExact(SOCKET s, char *data, int len)
{
  int off = 0;
  while (off < len)
  {
    int got = 0;
    if (!RecvSome(s, data + off, len - off, &got))
      return false;
    off += got;
  }
  return true;
}

void AddressText(const TargetAddr &t, char *out, int cap)
{
  appproxy::Copy(out, cap, "");
  if (t.family == AF_INET)
  {
    char num[8];
    for (int i = 0; i < 4; ++i)
    {
      int v = t.bytes[i];
      int n = 0;
      if (v == 0)
        num[n++] = '0';
      else
      {
        char rev[4];
        int r = 0;
        while (v)
        {
          rev[r++] = (char)('0' + v % 10);
          v /= 10;
        }
        while (r)
          num[n++] = rev[--r];
      }
      num[n] = 0;
      appproxy::Append(out, cap, appproxy::Len(out), num);
      if (i != 3)
        appproxy::Append(out, cap, appproxy::Len(out), ".");
    }
    return;
  }

  appproxy::Append(out, cap, 0, "[");
  static const char *hex = "0123456789abcdef";
  for (int g = 0; g < 8; ++g)
  {
    const unsigned v = (unsigned)t.bytes[g * 2] << 8 | t.bytes[g * 2 + 1];
    char part[5];
    part[0] = hex[(v >> 12) & 15];
    part[1] = hex[(v >> 8) & 15];
    part[2] = hex[(v >> 4) & 15];
    part[3] = hex[v & 15];
    part[4] = 0;
    appproxy::Append(out, cap, appproxy::Len(out), part);
    if (g != 7)
      appproxy::Append(out, cap, appproxy::Len(out), ":");
  }
  appproxy::Append(out, cap, appproxy::Len(out), "]");
}

void AuthorityText(const TargetAddr &t, char *out, int cap)
{
  char host[96];
  AddressText(t, host, (int)sizeof(host));
  appproxy::Copy(out, cap, host);
  appproxy::Append(out, cap, appproxy::Len(out), ":");
  char port[8];
  int v = t.port;
  int n = 0;
  char rev[8];
  if (v == 0)
    port[n++] = '0';
  while (v)
  {
    rev[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  int o = 0;
  while (n)
    port[o++] = rev[--n];
  port[o] = 0;
  appproxy::Append(out, cap, appproxy::Len(out), port);
}

bool HttpHandshake(SOCKET s, const TargetAddr &target, const appproxy::Profile &profile)
{
  char authority[160];
  AuthorityText(target, authority, (int)sizeof(authority));
  char request[2048];
  const int n = appproxy::BuildHttpConnect(authority, profile.username, profile.password, request,
                                            (int)sizeof(request));
  if (n <= 0 || !SendAll(s, request, n))
    return false;

  char response[4096];
  int used = 0;
  while (used < (int)sizeof(response) - 1)
  {
    int got = 0;
    // One byte at a time: a proxy is allowed to put the first tunnelled bytes in
    // the same TCP segment as the 200 response. Reading a larger block here
    // would consume those bytes and silently lose the beginning of the stream.
    if (!RecvSome(s, response + used, 1, &got))
      return false;
    used += got;
    response[used] = 0;
    for (int i = 0; i + 3 < used; ++i)
    {
      if (response[i] == '\r' && response[i + 1] == '\n' && response[i + 2] == '\r' &&
          response[i + 3] == '\n')
      {
        bool code200 = false;
        for (int k = 0; k + 4 < used; ++k)
          if (response[k] == ' ' && response[k + 1] == '2' && response[k + 2] == '0' &&
              response[k + 3] == '0' && (response[k + 4] == ' ' || response[k + 4] == '\r'))
            code200 = true;
        return code200;
      }
    }
  }
  return false;
}

bool SocksHandshake(SOCKET s, const TargetAddr &target, const appproxy::Profile &profile)
{
  const int useAuth = profile.username[0] != 0;
  unsigned char greeting[4];
  const int gn = appproxy::BuildSocks5Greeting(useAuth, greeting, (int)sizeof(greeting));
  if (gn <= 0 || !SendAll(s, (const char *)greeting, gn))
    return false;
  unsigned char choice[2];
  if (!RecvExact(s, (char *)choice, 2) || choice[0] != 5)
    return false;
  if (choice[1] == 0xFF)
    return false;
  if (choice[1] == 2)
  {
    unsigned char auth[512];
    const int an = appproxy::BuildSocks5Auth(profile.username, profile.password, auth, (int)sizeof(auth));
    if (an <= 0 || !SendAll(s, (const char *)auth, an))
      return false;
    unsigned char answer[2];
    if (!RecvExact(s, (char *)answer, 2) || answer[1] != 0)
      return false;
  }
  else if (choice[1] != 0)
    return false;

  const int atyp = target.family == AF_INET ? 1 : 4;
  const int addrLen = target.family == AF_INET ? 4 : 16;
  unsigned char req[32];
  const int rn = appproxy::BuildSocks5Connect(atyp, target.bytes, addrLen, target.port, req,
                                               (int)sizeof(req));
  if (rn <= 0 || !SendAll(s, (const char *)req, rn))
    return false;

  unsigned char head[4];
  if (!RecvExact(s, (char *)head, 4) || head[0] != 5 || head[1] != 0)
    return false;
  int rest = 0;
  if (head[3] == 1)
    rest = 4 + 2;
  else if (head[3] == 3)
  {
    unsigned char len = 0;
    if (!RecvExact(s, (char *)&len, 1))
      return false;
    rest = len + 2;
  }
  else if (head[3] == 4)
    rest = 16 + 2;
  else
    return false;
  char drain[300];
  return RecvExact(s, drain, rest);
}

bool ConnectToProxy(SOCKET s, const appproxy::Profile &profile)
{
  ADDRINFOA hints;
  char *p = (char *)&hints;
  for (unsigned i = 0; i < sizeof(hints); ++i)
    p[i] = 0;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  char port[8];
  int v = profile.port;
  int n = 0;
  char rev[8];
  if (v == 0)
    port[n++] = '0';
  while (v)
  {
    rev[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  int o = 0;
  while (n)
    port[o++] = rev[--n];
  port[o] = 0;

  ADDRINFOA *list = nullptr;
  if (!g_realGetAddrInfo || g_realGetAddrInfo(profile.server, port, &hints, &list) != 0 || !list)
    return false;
  bool connected = false;
  for (ADDRINFOA *ai = list; ai && !connected; ai = ai->ai_next)
  {
    const int rc = g_realConnect(s, ai->ai_addr, (int)ai->ai_addrlen);
    if (rc == 0)
    {
      connected = true;
      break;
    }
    const int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEALREADY)
      continue;
    if (!WaitSocket(s, true, kHandshakeTimeoutMs))
      break;
    int soError = 0;
    int soLen = (int)sizeof(soError);
    if (g_realGetSockOpt && g_realGetSockOpt(s, SOL_SOCKET, SO_ERROR, (char *)&soError, &soLen) == 0 &&
        soError == 0)
      connected = true;
    break; // one socket cannot be reused safely for a second proxy address
  }
  if (g_realFreeAddrInfo)
    g_realFreeAddrInfo(list);
  return connected;
}

void AddBridgeMapping(int family, unsigned short localPort, const TargetAddr &target,
                      const appproxy::Profile &profile)
{
  EnterCriticalSection(&g_lock);
  const ULONGLONG now = GetTickCount64();
  for (int i = g_mappingCount - 1; i >= 0; --i)
  {
    if (g_mappings[i].expires <= now)
    {
      g_mappings[i] = g_mappings[g_mappingCount - 1];
      --g_mappingCount;
    }
  }
  if (g_mappingCount >= kMaxBridgeMappings)
  {
    g_mappings[0] = g_mappings[g_mappingCount - 1];
    --g_mappingCount;
  }
  BridgeMapping &m = g_mappings[g_mappingCount++];
  m.family = family;
  m.localPort = localPort;
  m.target = target;
  m.profile = profile;
  m.expires = now + 30000;
  LeaveCriticalSection(&g_lock);
}

bool TakeBridgeMapping(int family, unsigned short localPort, TargetAddr *target,
                       appproxy::Profile *profile)
{
  bool found = false;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_mappingCount; ++i)
  {
    if (g_mappings[i].family == family && g_mappings[i].localPort == localPort)
    {
      *target = g_mappings[i].target;
      *profile = g_mappings[i].profile;
      g_mappings[i] = g_mappings[g_mappingCount - 1];
      --g_mappingCount;
      found = true;
      break;
    }
  }
  LeaveCriticalSection(&g_lock);
  return found;
}

void RemoveBridgeMapping(int family, unsigned short localPort)
{
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_mappingCount; ++i)
  {
    if (g_mappings[i].family == family && g_mappings[i].localPort == localPort)
    {
      g_mappings[i] = g_mappings[g_mappingCount - 1];
      --g_mappingCount;
      break;
    }
  }
  LeaveCriticalSection(&g_lock);
}

struct RelayContext
{
  SOCKET client;
  TargetAddr target;
  appproxy::Profile profile;
};

static DWORD WINAPI RelayProc(LPVOID param)
{
  RelayContext *ctx = (RelayContext *)param;
  SOCKET client = ctx->client;
  TargetAddr target = ctx->target;
  appproxy::Profile profile = ctx->profile;
  HeapFree(GetProcessHeap(), 0, ctx);

  if (!AliveAndEnabled())
  {
    closesocket(client);
    return 0;
  }

  SOCKET proxy = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (proxy == INVALID_SOCKET)
  {
    closesocket(client);
    return 0;
  }
  u_long nonblocking = 1;
  ioctlsocket(client, FIONBIO, &nonblocking);
  ioctlsocket(proxy, FIONBIO, &nonblocking);
  if (!ConnectToProxy(proxy, profile))
  {
    NoteOnce(&g_firstRelayLogged, "AppProxy: ConnectEx bridge could not reach the proxy",
             (DWORD)WSAGetLastError());
    closesocket(proxy);
    closesocket(client);
    return 0;
  }
  if (!(profile.kind == appproxy::kHttpConnect ? HttpHandshake(proxy, target, profile)
                                               : SocksHandshake(proxy, target, profile)))
  {
    NoteOnce(&g_firstRelayLogged, "AppProxy: ConnectEx bridge proxy handshake failed",
             (DWORD)WSAGetLastError());
    closesocket(proxy);
    closesocket(client);
    return 0;
  }
  NoteOnce(&g_firstRelayLogged, "AppProxy: first ConnectEx bridge completed the proxy handshake", 0);

  bool clientOpen = true;
  bool proxyOpen = true;
  char buffer[32768];
  while (clientOpen || proxyOpen)
  {
    fd_set read;
    FD_ZERO(&read);
    if (clientOpen)
      FD_SET(client, &read);
    if (proxyOpen)
      FD_SET(proxy, &read);
    timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    const int rc = g_realSelect(0, &read, nullptr, nullptr, &tv);
    if (rc == SOCKET_ERROR)
      break;
    if (rc == 0)
      continue;

    if (clientOpen && FD_ISSET(client, &read))
    {
      const int n = recv(client, buffer, (int)sizeof(buffer), 0);
      if (n > 0)
      {
        if (!SendAll(proxy, buffer, n))
          break;
      }
      else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
      {
        clientOpen = false;
        shutdown(proxy, SD_SEND);
      }
    }
    if (proxyOpen && FD_ISSET(proxy, &read))
    {
      const int n = recv(proxy, buffer, (int)sizeof(buffer), 0);
      if (n > 0)
      {
        if (!SendAll(client, buffer, n))
          break;
      }
      else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
      {
        proxyOpen = false;
        shutdown(client, SD_SEND);
      }
    }
  }
  closesocket(proxy);
  closesocket(client);
  return 0;
}

static DWORD WINAPI BridgeProc(LPVOID param)
{
  SOCKET listener = (SOCKET)(ULONG_PTR)param;
  for (;;)
  {
    sockaddr_storage peer;
    int peerLen = (int)sizeof(peer);
    SOCKET client = accept(listener, (sockaddr *)&peer, &peerLen);
    if (client == INVALID_SOCKET)
    {
      if (WSAGetLastError() == WSAEINTR)
        return 0;
      Sleep(20);
      continue;
    }
    TargetAddr source;
    TargetAddr target;
    appproxy::Profile profile;
    if (!TargetFromSockaddr((sockaddr *)&peer, peerLen, &source) ||
        !TakeBridgeMapping(source.family, (unsigned short)source.port, &target, &profile))
    {
      closesocket(client);
      continue;
    }
    RelayContext *ctx = (RelayContext *)HeapAlloc(GetProcessHeap(), 0, sizeof(RelayContext));
    if (!ctx)
    {
      closesocket(client);
      continue;
    }
    ctx->client = client;
    ctx->target = target;
    ctx->profile = profile;
    HANDLE thread = CreateThread(nullptr, 0, RelayProc, ctx, 0, nullptr);
    if (thread)
      CloseHandle(thread);
    else
    {
      HeapFree(GetProcessHeap(), 0, ctx);
      closesocket(client);
    }
  }
}

bool MakeBridgeListener(int family, SOCKET *listener, unsigned short *port)
{
  SOCKET s = socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return false;
  if (family == AF_INET)
  {
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr *)&a, sizeof(a)) != 0 || listen(s, 32) != 0)
    {
      closesocket(s);
      return false;
    }
    int len = (int)sizeof(a);
    getsockname(s, (sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
  }
  else
  {
    sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = 0;
    a.sin6_addr = in6addr_loopback;
    if (bind(s, (sockaddr *)&a, sizeof(a)) != 0 || listen(s, 32) != 0)
    {
      closesocket(s);
      return false;
    }
    int len = (int)sizeof(a);
    getsockname(s, (sockaddr *)&a, &len);
    *port = ntohs(a.sin6_port);
  }
  *listener = s;
  return true;
}

bool EnsureBridge()
{
  if (InterlockedCompareExchange(&g_bridgeStarted, 1, 0) != 0)
    return true;
  g_bridge4 = INVALID_SOCKET;
  g_bridge6 = INVALID_SOCKET;
  if (!MakeBridgeListener(AF_INET, &g_bridge4, &g_bridge4Port))
    goto fail;
  g_bridgeThread4 = CreateThread(nullptr, 0, BridgeProc, (LPVOID)(ULONG_PTR)g_bridge4, 0, nullptr);
  if (!g_bridgeThread4)
    goto fail;
  g_bridge6 = INVALID_SOCKET;
  if (MakeBridgeListener(AF_INET6, &g_bridge6, &g_bridge6Port))
    g_bridgeThread6 = CreateThread(nullptr, 0, BridgeProc, (LPVOID)(ULONG_PTR)g_bridge6, 0, nullptr);
  return true;

fail:
  if (g_bridge4 != INVALID_SOCKET)
    closesocket(g_bridge4);
  if (g_bridge6 != INVALID_SOCKET)
    closesocket(g_bridge6);
  g_bridge4 = INVALID_SOCKET;
  g_bridge6 = INVALID_SOCKET;
  InterlockedExchange(&g_bridgeStarted, 0);
  return false;
}

bool IsProxyAddress(const TargetAddr &target, const appproxy::Profile &profile)
{
  ADDRINFOA hints;
  char *p = (char *)&hints;
  for (unsigned i = 0; i < sizeof(hints); ++i)
    p[i] = 0;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  ADDRINFOA *list = nullptr;
  if (!g_realGetAddrInfo || g_realGetAddrInfo(profile.server, nullptr, &hints, &list) != 0 || !list)
    return false;
  bool same = false;
  for (ADDRINFOA *ai = list; ai && !same; ai = ai->ai_next)
    same = SameAddress(ai->ai_addr, (int)ai->ai_addrlen, target);
  if (g_realFreeAddrInfo)
    g_realFreeAddrInfo(list);
  return same;
}

bool TryRedirect(SOCKET s, const struct sockaddr *name, int namelen, int *result)
{
  if (result)
    *result = SOCKET_ERROR;
  int type = 0;
  int typeLen = (int)sizeof(type);
  Config cfg;
  appproxy::Profile profile;
  if (!g_realGetSockOpt ||
      g_realGetSockOpt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &typeLen) != 0 ||
      type != SOCK_STREAM || !ActiveContext(&cfg, &profile))
    return false;
  InterlockedIncrement(&g_redirects);

  TargetAddr target;
  if (!TargetFromSockaddr(name, namelen, &target))
    return false;

  if (IsProxyAddress(target, profile))
  {
    NoteOnce(&g_firstProxyBypass, "AppProxy: target was the proxy server itself; bypassed", 0);
    return false;
  }
  if (cfg.bypassLocal && IsLocalTarget(target))
  {
    NoteOnce(&g_firstLocalBypass, "AppProxy: local/LAN target bypassed", 0);
    return false;
  }

  if (!ConnectToProxy(s, profile))
  {
    InterlockedIncrement(&g_connectFailures);
    WSASetLastError(WSAECONNREFUSED);
    return true;
  }
  const bool ok = profile.kind == appproxy::kHttpConnect ? HttpHandshake(s, target, profile)
                                                         : SocksHandshake(s, target, profile);
  if (!ok)
  {
    InterlockedIncrement(&g_handshakeFailures);
    WSASetLastError(WSAECONNREFUSED);
    return true;
  }
  if (result)
    *result = 0;
  return true;
}

int WSAAPI HookConnect(SOCKET s, const struct sockaddr *name, int namelen)
{
  NoteOnce(&g_firstHookConnect, "AppProxy: first call reached HookConnect", 0);
  InterlockedIncrement(&g_calls);
  if (ShouldRejectQuic(s, name, namelen))
  {
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  int redirectedResult = SOCKET_ERROR;
  int rc;
  if (TryRedirect(s, name, namelen, &redirectedResult))
  {
    if (redirectedResult == 0)
      NoteOnce(&g_firstConnectLogged, "AppProxy: first connect completed the proxy handshake", 0);
    else
      NoteOnce(&g_firstConnectLogged, "AppProxy: first connect to the proxy failed",
               (DWORD)WSAGetLastError());
    rc = redirectedResult;
  }
  else
    rc = g_realConnect ? g_realConnect(s, name, namelen) : SOCKET_ERROR;
  InterlockedExchange(&g_lastResult, rc);
  InterlockedExchange(&g_lastWsaError, WSAGetLastError());
  return rc;
}

int WSAAPI HookWSAConnect(SOCKET s, const struct sockaddr *name, int namelen, LPWSABUF,
                          LPWSABUF, LPQOS, LPQOS)
{
  NoteOnce(&g_firstHookWSAConnect, "AppProxy: first call reached HookWSAConnect", 0);
  InterlockedIncrement(&g_calls);
  if (ShouldRejectQuic(s, name, namelen))
  {
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  int redirectedResult = SOCKET_ERROR;
  int rc;
  if (TryRedirect(s, name, namelen, &redirectedResult))
  {
    if (redirectedResult == 0)
      NoteOnce(&g_firstConnectLogged, "AppProxy: first connect completed the proxy handshake", 0);
    else
      NoteOnce(&g_firstConnectLogged, "AppProxy: first connect to the proxy failed",
               (DWORD)WSAGetLastError());
    rc = redirectedResult;
  }
  else
    rc = g_realWSAConnect ? g_realWSAConnect(s, name, namelen, nullptr, nullptr, nullptr, nullptr)
                          : SOCKET_ERROR;
  InterlockedExchange(&g_lastResult, rc);
  InterlockedExchange(&g_lastWsaError, WSAGetLastError());
  return rc;
}

int WSAAPI HookSendTo(SOCKET s, const char *buffer, int len, int flags, const struct sockaddr *name,
                      int namelen)
{
  if (ShouldRejectQuic(s, name, namelen))
  {
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  return g_realSendTo ? g_realSendTo(s, buffer, len, flags, name, namelen) : SOCKET_ERROR;
}

int WSAAPI HookWSASendTo(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags,
                         const struct sockaddr *name, int namelen, LPWSAOVERLAPPED overlapped,
                         LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
  if (ShouldRejectQuic(s, name, namelen))
  {
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  return g_realWSASendTo ? g_realWSASendTo(s, buffers, count, sent, flags, name, namelen, overlapped,
                                            completion)
                         : SOCKET_ERROR;
}

bool SameGuid(const GUID &a, const GUID &b)
{
  const unsigned char *pa = (const unsigned char *)&a;
  const unsigned char *pb = (const unsigned char *)&b;
  for (int i = 0; i < (int)sizeof(GUID); ++i)
    if (pa[i] != pb[i])
      return false;
  return true;
}

const GUID kConnectExGuid = {0x25a207b9, 0xddf3, 0x4660, {0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e}};

BOOL PASCAL HookConnectEx(SOCKET s, const struct sockaddr *name, int namelen, PVOID sendBuffer,
                          DWORD sendLength, LPDWORD bytesSent, LPWSAOVERLAPPED overlapped)
{
  NoteOnce(&g_firstHookConnectEx, "AppProxy: first call reached HookConnectEx", 0);
  InterlockedIncrement(&g_connectExCalls);
  if (!g_realConnectEx)
  {
    WSASetLastError(WSAEOPNOTSUPP);
    return FALSE;
  }
  Config cfg;
  appproxy::Profile profile;
  if (!ActiveContext(&cfg, &profile))
    return g_realConnectEx(s, name, namelen, sendBuffer, sendLength, bytesSent, overlapped);

  TargetAddr target;
  if (!TargetFromSockaddr(name, namelen, &target))
  {
    WSASetLastError(WSAEAFNOSUPPORT);
    return FALSE;
  }
  if (IsProxyAddress(target, profile) || (cfg.bypassLocal && IsLocalTarget(target)))
    return g_realConnectEx(s, name, namelen, sendBuffer, sendLength, bytesSent, overlapped);
  NoteOnce(&g_firstConnectExLogged, "AppProxy: first ConnectEx was redirected to the local bridge", 0);
  if (!EnsureBridge())
  {
    NoteOnce(&g_firstRelayLogged, "AppProxy: ConnectEx local bridge could not start", GetLastError());
    WSASetLastError(WSAECONNREFUSED);
    return FALSE;
  }

  sockaddr_storage local;
  int localLen = (int)sizeof(local);
  if (getsockname(s, (sockaddr *)&local, &localLen) != 0 ||
      (local.ss_family != AF_INET && local.ss_family != AF_INET6))
  {
    WSASetLastError(WSAEINVAL);
    return FALSE;
  }
  int localPort = 0;
  if (local.ss_family == AF_INET)
    localPort = (int)ntohs(((sockaddr_in *)&local)->sin_port);
  else
    localPort = (int)ntohs(((sockaddr_in6 *)&local)->sin6_port);
  if (localPort == 0)
  {
    // ConnectEx requires a bound socket. Bind an ephemeral local port if the caller has not already done it.
    if (local.ss_family == AF_INET)
    {
      sockaddr_in any;
      memset(&any, 0, sizeof(any));
      any.sin_family = AF_INET;
      any.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(s, (sockaddr *)&any, sizeof(any)) != 0)
      {
        WSASetLastError(WSAEINVAL);
        return FALSE;
      }
    }
    else
    {
      sockaddr_in6 any;
      memset(&any, 0, sizeof(any));
      any.sin6_family = AF_INET6;
      any.sin6_addr = in6addr_any;
      if (bind(s, (sockaddr *)&any, sizeof(any)) != 0)
      {
        WSASetLastError(WSAEINVAL);
        return FALSE;
      }
    }
    localLen = (int)sizeof(local);
    getsockname(s, (sockaddr *)&local, &localLen);
    if (local.ss_family == AF_INET)
      localPort = (int)ntohs(((sockaddr_in *)&local)->sin_port);
    else
      localPort = (int)ntohs(((sockaddr_in6 *)&local)->sin6_port);
  }

  AddBridgeMapping(local.ss_family, (unsigned short)localPort, target, profile);
  BOOL ok = FALSE;
  if (local.ss_family == AF_INET)
  {
    if (g_bridge4 == INVALID_SOCKET)
    {
      RemoveBridgeMapping(AF_INET, (unsigned short)localPort);
      WSASetLastError(WSAECONNREFUSED);
      return FALSE;
    }
    sockaddr_in bridge;
    memset(&bridge, 0, sizeof(bridge));
    bridge.sin_family = AF_INET;
    bridge.sin_port = htons(g_bridge4Port);
    bridge.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = g_realConnectEx(s, (sockaddr *)&bridge, sizeof(bridge), sendBuffer, sendLength, bytesSent,
                         overlapped);
  }
  else
  {
    if (g_bridge6 == INVALID_SOCKET)
    {
      RemoveBridgeMapping(AF_INET6, (unsigned short)localPort);
      WSASetLastError(WSAECONNREFUSED);
      return FALSE;
    }
    sockaddr_in6 bridge;
    memset(&bridge, 0, sizeof(bridge));
    bridge.sin6_family = AF_INET6;
    bridge.sin6_port = htons(g_bridge6Port);
    bridge.sin6_addr = in6addr_loopback;
    ok = g_realConnectEx(s, (sockaddr *)&bridge, sizeof(bridge), sendBuffer, sendLength, bytesSent,
                         overlapped);
  }
  if (!ok && WSAGetLastError() != WSA_IO_PENDING)
    RemoveBridgeMapping(local.ss_family, (unsigned short)localPort);
  InterlockedExchange(&g_lastResult, ok ? 0 : -1);
  InterlockedExchange(&g_lastWsaError, WSAGetLastError());
  return ok;
}

int WSAAPI HookWSAIoctl(SOCKET s, DWORD code, LPVOID inBuffer, DWORD inSize, LPVOID outBuffer,
                        DWORD outSize, LPDWORD returned, LPWSAOVERLAPPED overlapped,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
  if (code == SIO_GET_EXTENSION_FUNCTION_POINTER && inBuffer && inSize >= sizeof(GUID) &&
      outBuffer && outSize >= sizeof(void *) && SameGuid(*(const GUID *)inBuffer, kConnectExGuid))
  {
    NoteOnce(&g_firstHookWSAIoctlConnectEx, "AppProxy: first ConnectEx extension lookup was intercepted", 0);
    LPFN_CONNECTEX real = nullptr;
    DWORD got = 0;
    const int rc = g_realWSAIoctl(s, code, inBuffer, inSize, &real, sizeof(real), &got, overlapped,
                                  completion);
    if (rc == 0 && real)
    {
      g_realConnectEx = real;
      *(void **)outBuffer = (void *)&HookConnectEx;
      if (returned)
        *returned = sizeof(void *);
      return 0;
    }
  }
  return g_realWSAIoctl(s, code, inBuffer, inSize, outBuffer, outSize, returned, overlapped, completion);
}

bool NameIs(const char *name, const char *want)
{
  return appproxy::EqNoCase(name, want);
}

int PatchOneSlot(ULONG_PTR *slot, ULONG_PTR replacement)
{
  if (!slot || *slot == replacement)
    return 0;
  DWORD oldProtect = 0;
  if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect))
    return 0;
  *slot = replacement;
  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(*slot), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
  return 1;
}

void PatchModuleImports(HMODULE mod, int *patched)
{
  if (!mod || mod == g_self)
    return;
  unsigned char *base = (unsigned char *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return;
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress || !dir.Size)
    return;
  IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
  for (; imp->Name; ++imp)
  {
    const char *dll = (const char *)(base + imp->Name);
    if (!NameIs(dll, "ws2_32.dll") && !NameIs(dll, "wsock32.dll"))
      continue;
    IMAGE_THUNK_DATA *orig = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk
                                                              ? imp->OriginalFirstThunk
                                                              : imp->FirstThunk));
    IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
    for (; orig->u1.AddressOfData; ++orig, ++iat)
    {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))
        continue;
      IMAGE_IMPORT_BY_NAME *byName = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
      const char *fn = (const char *)byName->Name;
      if (appproxy::EqNoCase(fn, "connect"))
        *patched += PatchOneSlot((ULONG_PTR *)&iat->u1.Function, (ULONG_PTR)&HookConnect);
      else if (appproxy::EqNoCase(fn, "WSAConnect"))
        *patched += PatchOneSlot((ULONG_PTR *)&iat->u1.Function, (ULONG_PTR)&HookWSAConnect);
      else if (appproxy::EqNoCase(fn, "WSAIoctl"))
        *patched += PatchOneSlot((ULONG_PTR *)&iat->u1.Function, (ULONG_PTR)&HookWSAIoctl);
      else if (appproxy::EqNoCase(fn, "sendto"))
        *patched += PatchOneSlot((ULONG_PTR *)&iat->u1.Function, (ULONG_PTR)&HookSendTo);
      else if (appproxy::EqNoCase(fn, "WSASendTo"))
        *patched += PatchOneSlot((ULONG_PTR *)&iat->u1.Function, (ULONG_PTR)&HookWSASendTo);
    }
  }
}

int RefreshImports()
{
  const ULONGLONG now = GetTickCount64();
  if (now - g_lastPatchMs < 1000)
    return g_lastPatchCount;
  g_lastPatchMs = now;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE)
    return 0;
  int patched = 0;
  MODULEENTRY32 me;
  me.dwSize = sizeof(me);
  if (Module32First(snap, &me))
  {
    do
      PatchModuleImports((HMODULE)me.modBaseAddr, &patched);
    while (Module32Next(snap, &me));
  }
  CloseHandle(snap);
  g_lastPatchCount = patched;
  return patched;
}

bool LoadWinsock()
{
  HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
  if (!ws2)
    ws2 = LoadLibraryA("ws2_32.dll");
  if (!ws2)
    return false;
  g_realConnect = (ConnectFn)(void *)GetProcAddress(ws2, "connect");
  g_realWSAConnect = (WSAConnectFn)(void *)GetProcAddress(ws2, "WSAConnect");
  g_realSelect = (SelectFn)(void *)GetProcAddress(ws2, "select");
  g_realGetSockOpt = (GetSockOptFn)(void *)GetProcAddress(ws2, "getsockopt");
  g_realGetAddrInfo = (GetAddrInfoFn)(void *)GetProcAddress(ws2, "getaddrinfo");
  g_realFreeAddrInfo = (FreeAddrInfoFn)(void *)GetProcAddress(ws2, "freeaddrinfo");
  g_realWSAIoctl = (WSAIoctlFn)(void *)GetProcAddress(ws2, "WSAIoctl");
  g_realSendTo = (SendToFn)(void *)GetProcAddress(ws2, "sendto");
  g_realWSASendTo = (WSASendToFn)(void *)GetProcAddress(ws2, "WSASendTo");
  return g_realConnect && g_realWSAConnect && g_realSelect && g_realGetSockOpt &&
         g_realGetAddrInfo && g_realFreeAddrInfo && g_realWSAIoctl && g_realSendTo &&
         g_realWSASendTo;
}

} // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK ApexHookProc(int code, WPARAM wp, LPARAM lp)
{
  if (code >= 0 && !g_skipInstall)
    RefreshImports();
  return g_callNextHookEx ? g_callNextHookEx(nullptr, code, wp, lp) : 0;
}

extern "C" __declspec(dllexport) unsigned ApexAppProxyHookAbi(void)
{
  return 1;
}

extern "C" __declspec(dllexport) unsigned ApexAppProxyHookStatus(void)
{
  return (g_winsockReady ? 1u : 0u) | ((unsigned)g_lastPatchCount << 8);
}

extern "C" __declspec(dllexport) void ApexAppProxyHookStats(unsigned *out)
{
  if (!out)
    return;
  out[0] = (unsigned)InterlockedCompareExchange(&g_calls, 0, 0);
  out[1] = (unsigned)InterlockedCompareExchange(&g_redirects, 0, 0);
  out[2] = (unsigned)InterlockedCompareExchange(&g_connectFailures, 0, 0);
  out[3] = (unsigned)InterlockedCompareExchange(&g_handshakeFailures, 0, 0);
  out[4] = (unsigned)InterlockedCompareExchange(&g_lastResult, 0, 0);
  out[5] = (unsigned)InterlockedCompareExchange(&g_lastWsaError, 0, 0);
  out[6] = (unsigned)InterlockedCompareExchange(&g_connectExCalls, 0, 0);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    g_self = (HMODULE)inst;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
    InitPaths((HMODULE)inst);
    g_skipInstall = IsApexHost();
    if (!g_skipInstall && LoadWinsock())
    {
      HMODULE user32 = GetModuleHandleA("user32.dll");
      if (user32)
        g_callNextHookEx = (CallNextHookExFn)(void *)GetProcAddress(user32, "CallNextHookEx");
      g_lastPatchMs = 0;
      RefreshImports();
      g_winsockReady = true;
    }
  }
  else if (reason == DLL_PROCESS_DETACH && g_lockReady)
  {
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
  }
  return TRUE;
}
