// ---------------------------------------------------------------------------
// AppProxyService -- driver-level TCP redirector for AppProxy.
//
// The service intentionally does NOT ship WinDivert. It loads WinDivert.dll from the folder selected on the
// AppProxy settings page (for local development that can be an existing WinDivert installation). WinDivert
// then loads its matching WinDivert64.sys as an on-demand kernel driver.
//
// TCP SYNs for selected applications are rewritten to a loopback relay. The relay connects the original
// destination through the selected SOCKS5 / HTTP proxy. UDP 443 is dropped for selected applications when
// "block QUIC" is enabled so Chromium falls back to TCP.
// ---------------------------------------------------------------------------

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "../proxy_common.h"
#include "../process_tree.h"
#include "windivert_api.h"

namespace {

const char *kServiceName = "ApexAppProxy";
const char *kServiceDisplay = "Apex App Proxy";
const char *kFirewallRule = "Apex App Proxy relay";

struct TargetV4
{
  unsigned address; // network byte order
  unsigned short port;
};

struct FlowMap
{
  unsigned short localPort;
  TargetV4 target;
  appproxy::Profile profile;
  char app[64];
  ULONGLONG expires;
};

static const int kMaxFlows = 1024;
static const int kMaxPacket = 65535;

WinDivertApi g_wd;
appproxy::Config g_cfg;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
CRITICAL_SECTION g_statusLock;
bool g_statusLockReady = false;
char g_configPath[MAX_PATH] = {0};
char g_windivertDir[MAX_PATH] = {0};
char g_statusPath[MAX_PATH] = {0};
char g_serviceExe[MAX_PATH] = {0};
char g_filter[384] = {0};
FlowMap g_flows[kMaxFlows];
int g_flowCount = 0;
HANDLE g_divert = INVALID_HANDLE_VALUE;
HANDLE g_stop = nullptr;
HANDLE g_status = nullptr;
volatile LONG g_serviceStop = 0;
SOCKET g_relayListener = INVALID_SOCKET;
unsigned short g_relayPort = 0;
HANDLE g_relayThread = nullptr;
ULONGLONG g_lastConfigCheck = 0;
LONG g_packetSeen = 0;
LONG g_ruleMatched = 0;
LONG g_rewriteCount = 0;
LONG g_ipv6Blocked = 0;
LONG g_relayOk = 0;
LONG g_relayFail = 0;
LONG g_acceptSeen = 0;
LONG g_acceptNoFlow = 0;
LONG g_sendFail = 0;
LONG g_quicBlocked = 0;
char g_lastResult[16] = {0};
char g_lastApp[64] = {0};
char g_lastTarget[96] = {0};
char g_lastError[160] = {0};
ULONGLONG g_lastStatusWrite = 0;
LONG g_statusWriting = 0;

void Log(const char *fmt, ...)
{
  char path[MAX_PATH] = {0};
  if (g_statusPath[0])
  {
    strcpy(path, g_statusPath);
    char *slash = strrchr(path, '\\');
    if (slash)
    {
      strcpy(slash + 1, "AppProxyService.log");
    }
  }
  if (!path[0])
    return;
  FILE *f = fopen(path, "ab");
  if (!f)
    return;
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  fprintf(f, "%s\r\n", line);
  fclose(f);
}

void WriteStatus(const char *state, const char *detail)
{
  if (!g_statusPath[0])
    return;
  if (InterlockedCompareExchange(&g_statusWriting, 1, 0) != 0)
    return;
  char lastResult[16] = {0};
  char lastApp[64] = {0};
  char lastTarget[96] = {0};
  char lastError[160] = {0};
  if (g_statusLockReady)
  {
    EnterCriticalSection(&g_statusLock);
    appproxy::Copy(lastResult, sizeof(lastResult), g_lastResult);
    appproxy::Copy(lastApp, sizeof(lastApp), g_lastApp);
    appproxy::Copy(lastTarget, sizeof(lastTarget), g_lastTarget);
    appproxy::Copy(lastError, sizeof(lastError), g_lastError);
    LeaveCriticalSection(&g_statusLock);
  }
  const long relayOk = (long)InterlockedCompareExchange(&g_relayOk, 0, 0);
  const long relayFail = (long)InterlockedCompareExchange(&g_relayFail, 0, 0);
  const long ipv6Blocked = (long)InterlockedCompareExchange(&g_ipv6Blocked, 0, 0);
  const long quicBlocked = (long)InterlockedCompareExchange(&g_quicBlocked, 0, 0);
  const long packets = (long)InterlockedCompareExchange(&g_packetSeen, 0, 0);
  const long matches = (long)InterlockedCompareExchange(&g_ruleMatched, 0, 0);
  const long rewrites = (long)InterlockedCompareExchange(&g_rewriteCount, 0, 0);
  FILE *f = fopen(g_statusPath, "wb");
  if (!f)
  {
    InterlockedExchange(&g_statusWriting, 0);
    return;
  }
  fprintf(f,
          "state=%s\r\ndetail=%s\r\nrelay_port=%u\r\npackets=%ld\r\nmatches=%ld\r\n"
          "rewrites=%ld\r\nrelay_ok=%ld\r\nrelay_fail=%ld\r\nipv6_blocked=%ld\r\n"
          "quic_blocked=%ld\r\nlast_result=%s\r\nlast_app=%s\r\nlast_target=%s\r\n"
          "last_error=%s\r\n",
          state, detail ? detail : "", (unsigned)g_relayPort, packets,
          matches, rewrites, relayOk, relayFail, ipv6Blocked,
          quicBlocked, lastResult, lastApp, lastTarget, lastError);
  fclose(f);
  g_lastStatusWrite = GetTickCount64();
  InterlockedExchange(&g_statusWriting, 0);
}

void MaybeWriteRunningStatus()
{
  if (GetTickCount64() - g_lastStatusWrite >= 1000)
    WriteStatus("running", "driver attached");
}

void FormatTarget(const TargetV4 &target, char *out, int cap)
{
  in_addr a;
  a.S_un.S_addr = target.address;
  char host[64] = {0};
  inet_ntop(AF_INET, &a, host, sizeof(host));
  snprintf(out, cap, "%s:%u", host, (unsigned)target.port);
}

void RecordMatch(const char *app, const char *target)
{
  if (!g_statusLockReady)
    return;
  EnterCriticalSection(&g_statusLock);
  appproxy::Copy(g_lastApp, sizeof(g_lastApp), app);
  appproxy::Copy(g_lastTarget, sizeof(g_lastTarget), target);
  LeaveCriticalSection(&g_statusLock);
}

void RecordRelayResult(bool ok, const char *app, const TargetV4 &target, const char *error)
{
  char targetText[96] = {0};
  FormatTarget(target, targetText, sizeof(targetText));
  if (ok)
    InterlockedIncrement(&g_relayOk);
  else
    InterlockedIncrement(&g_relayFail);
  if (g_statusLockReady)
  {
    EnterCriticalSection(&g_statusLock);
    appproxy::Copy(g_lastResult, sizeof(g_lastResult), ok ? "ok" : "fail");
    appproxy::Copy(g_lastApp, sizeof(g_lastApp), app);
    appproxy::Copy(g_lastTarget, sizeof(g_lastTarget), targetText);
    appproxy::Copy(g_lastError, sizeof(g_lastError), ok ? "" : error);
    LeaveCriticalSection(&g_statusLock);
  }
  MaybeWriteRunningStatus();
}

bool ReadWholeFile(const char *path, char *out, int cap)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  const size_t n = fread(out, 1, cap - 1, f);
  fclose(f);
  out[n] = 0;
  return true;
}

void LoadConfig(bool force)
{
  const ULONGLONG now = GetTickCount64();
  if (!force && now - g_lastConfigCheck < 500)
    return;
  g_lastConfigCheck = now;
  char text[appproxy::kConfigCap];
  if (!ReadWholeFile(g_configPath, text, sizeof(text)))
  {
    EnterCriticalSection(&g_lock);
    appproxy::Defaults(&g_cfg);
    LeaveCriticalSection(&g_lock);
    return;
  }
  appproxy::Config cfg;
  appproxy::ParseConfig(text, (int)strlen(text), &cfg);
  if (!cfg.windivertDir[0])
    strcpy(cfg.windivertDir, g_windivertDir);
  EnterCriticalSection(&g_lock);
  g_cfg = cfg;
  LeaveCriticalSection(&g_lock);
}

void ProcessName(DWORD pid, char *out, int cap)
{
  out[0] = 0;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return;
  char path[MAX_PATH] = {0};
  DWORD n = MAX_PATH;
  if (QueryFullProcessImageNameA(p, 0, path, &n))
    apex::match::NormaliseName(path, out, cap);
  CloseHandle(p);
}

int ProcessTreeRule(const appproxy::Config &cfg, DWORD pid)
{
  const int kMaxProcesses = 1024;
  appproxy::ProcessNode rows[kMaxProcesses];
  int count = 0;
  int self = -1;
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
      r.pid = pe.th32ProcessID;
      r.parent = pe.th32ParentProcessID;
      apex::match::NormaliseName(pe.szExeFile, r.exe, sizeof(r.exe));
      if (pe.th32ProcessID == pid)
        self = count;
      ++count;
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  return self >= 0 ? appproxy::RuleForSelectedTree(cfg, rows, count, self) : -1;
}

DWORD TcpOwnerPid(unsigned short localPort)
{
  DWORD size = 0;
  if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) !=
      ERROR_INSUFFICIENT_BUFFER)
    return 0;
  MIB_TCPTABLE_OWNER_PID *table = (MIB_TCPTABLE_OWNER_PID *)malloc(size);
  if (!table)
    return 0;
  DWORD pid = 0;
  if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR)
  {
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
    {
      const MIB_TCPROW_OWNER_PID &r = table->table[i];
      if (ntohs((unsigned short)r.dwLocalPort) == localPort)
      {
        pid = r.dwOwningPid;
        break;
      }
    }
  }
  free(table);
  return pid;
}

DWORD UdpOwnerPid(unsigned short localPort)
{
  DWORD size = 0;
  if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) !=
      ERROR_INSUFFICIENT_BUFFER)
    return 0;
  MIB_UDPTABLE_OWNER_PID *table = (MIB_UDPTABLE_OWNER_PID *)malloc(size);
  if (!table)
    return 0;
  DWORD pid = 0;
  if (GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR)
  {
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
    {
      const MIB_UDPROW_OWNER_PID &r = table->table[i];
      if (ntohs((unsigned short)r.dwLocalPort) == localPort)
      {
        pid = r.dwOwningPid;
        break;
      }
    }
  }
  free(table);
  return pid;
}

DWORD Tcp6OwnerPid(unsigned short localPort)
{
  DWORD size = 0;
  if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) !=
      ERROR_INSUFFICIENT_BUFFER)
    return 0;
  MIB_TCP6TABLE_OWNER_PID *table = (MIB_TCP6TABLE_OWNER_PID *)malloc(size);
  if (!table)
    return 0;
  DWORD pid = 0;
  if (GetExtendedTcpTable(table, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR)
  {
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
      if (ntohs((unsigned short)table->table[i].dwLocalPort) == localPort)
      {
        pid = table->table[i].dwOwningPid;
        break;
      }
  }
  free(table);
  return pid;
}

DWORD Udp6OwnerPid(unsigned short localPort)
{
  DWORD size = 0;
  if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) !=
      ERROR_INSUFFICIENT_BUFFER)
    return 0;
  MIB_UDP6TABLE_OWNER_PID *table = (MIB_UDP6TABLE_OWNER_PID *)malloc(size);
  if (!table)
    return 0;
  DWORD pid = 0;
  if (GetExtendedUdpTable(table, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR)
  {
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
      if (ntohs((unsigned short)table->table[i].dwLocalPort) == localPort)
      {
        pid = table->table[i].dwOwningPid;
        break;
      }
  }
  free(table);
  return pid;
}

bool LocalTarget(const TargetV4 &t)
{
  const unsigned char *p = (const unsigned char *)&t.address;
  return p[0] == 127 || p[0] == 10 || (p[0] == 192 && p[1] == 168) ||
         (p[0] == 172 && (p[1] & 0xF0) == 16) || (p[0] == 169 && p[1] == 254);
}

bool ResolveRule(DWORD pid, appproxy::Profile *profile)
{
  LoadConfig(false);
  appproxy::Config cfg;
  EnterCriticalSection(&g_lock);
  cfg = g_cfg;
  LeaveCriticalSection(&g_lock);
  if (!appproxy::Valid(&cfg))
    return false;
  const int rule = ProcessTreeRule(cfg, pid);
  if (rule < 0)
    return false;
  const int profileIndex = appproxy::FindProfile(&cfg, cfg.rules[rule].profile);
  if (profileIndex < 0 || !appproxy::ProfileValid(&cfg.profiles[profileIndex]))
    return false;
  *profile = cfg.profiles[profileIndex];
  return true;
}

void RememberFlow(unsigned short localPort, const TargetV4 &target,
                  const appproxy::Profile &profile, const char *app)
{
  EnterCriticalSection(&g_lock);
  const ULONGLONG now = GetTickCount64();
  for (int i = g_flowCount - 1; i >= 0; --i)
    if (g_flows[i].expires <= now || g_flows[i].localPort == localPort)
    {
      g_flows[i] = g_flows[g_flowCount - 1];
      --g_flowCount;
    }
  if (g_flowCount >= kMaxFlows)
  {
    g_flows[0] = g_flows[g_flowCount - 1];
    --g_flowCount;
  }
  FlowMap &m = g_flows[g_flowCount++];
  m.localPort = localPort;
  m.target = target;
  m.profile = profile;
  appproxy::Copy(m.app, sizeof(m.app), app);
  m.expires = now + 120000;
  LeaveCriticalSection(&g_lock);
}

bool FindFlow(unsigned short localPort, TargetV4 *target, appproxy::Profile *profile,
              char *app, int appCap)
{
  bool found = false;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_flowCount; ++i)
    if (g_flows[i].localPort == localPort)
    {
      if (target)
        *target = g_flows[i].target;
      if (profile)
        *profile = g_flows[i].profile;
      appproxy::Copy(app, appCap, g_flows[i].app);
      found = true;
      break;
    }
  LeaveCriticalSection(&g_lock);
  return found;
}

bool SendAll(SOCKET s, const char *data, int len)
{
  int off = 0;
  while (off < len)
  {
    const int n = send(s, data + off, len - off, 0);
    if (n <= 0)
      return false;
    off += n;
  }
  return true;
}

bool RecvExact(SOCKET s, char *data, int len)
{
  int off = 0;
  while (off < len)
  {
    const int n = recv(s, data + off, len - off, 0);
    if (n <= 0)
      return false;
    off += n;
  }
  return true;
}

SOCKET ConnectProxy(const appproxy::Profile &p)
{
  ADDRINFOA hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  char port[16];
  snprintf(port, sizeof(port), "%d", p.port);
  ADDRINFOA *list = nullptr;
  if (getaddrinfo(p.server, port, &hints, &list) != 0)
    return INVALID_SOCKET;
  SOCKET s = INVALID_SOCKET;
  for (ADDRINFOA *ai = list; ai; ai = ai->ai_next)
  {
    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET)
      continue;
    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
      break;
    closesocket(s);
    s = INVALID_SOCKET;
  }
  freeaddrinfo(list);
  return s;
}

bool HttpHandshake(SOCKET s, const TargetV4 &target, const appproxy::Profile &p)
{
  in_addr a;
  a.S_un.S_addr = target.address;
  char host[64];
  inet_ntop(AF_INET, &a, host, sizeof(host));
  char authority[96];
  snprintf(authority, sizeof(authority), "%s:%u", host, (unsigned)target.port);
  char request[2048];
  const int n = appproxy::BuildHttpConnect(authority, p.username, p.password, request,
                                            sizeof(request));
  if (n <= 0 || !SendAll(s, request, n))
    return false;
  char response[4096];
  int used = 0;
  while (used < (int)sizeof(response) - 1)
  {
    const int got = recv(s, response + used, 1, 0);
    if (got <= 0)
      return false;
    ++used;
    response[used] = 0;
    if (used >= 4 && memcmp(response + used - 4, "\r\n\r\n", 4) == 0)
      return strstr(response, " 200 ") != nullptr;
  }
  return false;
}

bool SocksHandshake(SOCKET s, const TargetV4 &target, const appproxy::Profile &p)
{
  const bool auth = p.username[0] != 0;
  unsigned char greeting[4];
  int n = appproxy::BuildSocks5Greeting(auth, greeting, sizeof(greeting));
  if (n <= 0 || !SendAll(s, (char *)greeting, n))
    return false;
  unsigned char choice[2];
  if (!RecvExact(s, (char *)choice, 2) || choice[0] != 5 || choice[1] == 0xFF)
    return false;
  if (choice[1] == 2)
  {
    unsigned char authData[512];
    n = appproxy::BuildSocks5Auth(p.username, p.password, authData, sizeof(authData));
    if (n <= 0 || !SendAll(s, (char *)authData, n))
      return false;
    unsigned char answer[2];
    if (!RecvExact(s, (char *)answer, 2) || answer[1] != 0)
      return false;
  }
  else if (choice[1] != 0)
    return false;
  unsigned char req[16];
  n = appproxy::BuildSocks5Connect(1, (const unsigned char *)&target.address, 4, target.port, req,
                                    sizeof(req));
  if (n <= 0 || !SendAll(s, (char *)req, n))
    return false;
  unsigned char head[4];
  if (!RecvExact(s, (char *)head, 4) || head[1] != 0)
    return false;
  char drain[260];
  int rest = 0;
  if (head[3] == 1)
    rest = 6;
  else if (head[3] == 3)
  {
    unsigned char len;
    if (!RecvExact(s, (char *)&len, 1))
      return false;
    rest = len + 2;
  }
  else if (head[3] == 4)
    rest = 18;
  else
    return false;
  return RecvExact(s, drain, rest);
}

void RelayConnection(SOCKET client, const TargetV4 &target, const appproxy::Profile &p,
                     const char *app)
{
  SOCKET proxy = ConnectProxy(p);
  if (proxy == INVALID_SOCKET)
  {
    Log("relay: could not connect to proxy for %s", app && app[0] ? app : "selected app");
    RecordRelayResult(false, app, target, "could not connect to the proxy server");
    closesocket(client);
    return;
  }
  if (!(p.kind == appproxy::kHttpConnect ? HttpHandshake(proxy, target, p)
                                          : SocksHandshake(proxy, target, p)))
  {
    Log("relay: proxy handshake failed for %s port %u", app && app[0] ? app : "selected app",
        (unsigned)target.port);
    RecordRelayResult(false, app, target, "the proxy handshake failed");
    closesocket(proxy);
    closesocket(client);
    return;
  }
  RecordRelayResult(true, app, target, "");
  u_long nb = 1;
  ioctlsocket(client, FIONBIO, &nb);
  ioctlsocket(proxy, FIONBIO, &nb);
  char buf[32768];
  for (;;)
  {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(client, &r);
    FD_SET(proxy, &r);
    timeval tv = {1, 0};
    if (select(0, &r, nullptr, nullptr, &tv) <= 0)
      continue;
    if (FD_ISSET(client, &r))
    {
      int n = recv(client, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      if (!SendAll(proxy, buf, n))
        break;
    }
    if (FD_ISSET(proxy, &r))
    {
      int n = recv(proxy, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      if (!SendAll(client, buf, n))
        break;
    }
  }
  closesocket(proxy);
  closesocket(client);
}

struct RelayJob
{
  SOCKET client;
  TargetV4 target;
  appproxy::Profile profile;
  char app[64];
};

DWORD WINAPI RelayJobProc(LPVOID param)
{
  RelayJob *job = (RelayJob *)param;
  RelayConnection(job->client, job->target, job->profile, job->app);
  delete job;
  return 0;
}

DWORD WINAPI RelayAcceptProc(LPVOID)
{
  for (;;)
  {
    SOCKET client = accept(g_relayListener, nullptr, nullptr);
    if (client == INVALID_SOCKET)
    {
      if (InterlockedCompareExchange(&g_serviceStop, 0, 0))
        break;
      Sleep(50);
      continue;
    }
    if (InterlockedIncrement(&g_acceptSeen) <= 5)
      Log("relay accepted connection #%ld", (long)InterlockedCompareExchange(&g_acceptSeen, 0, 0));
    sockaddr_in peer = {};
    int len = sizeof(peer);
    getpeername(client, (sockaddr *)&peer, &len);
    TargetV4 target = {};
    appproxy::Profile profile = {};
    char app[64] = {0};
    if (!FindFlow(ntohs(peer.sin_port), &target, &profile, app, sizeof(app)))
    {
      if (InterlockedIncrement(&g_acceptNoFlow) <= 5)
        Log("relay connection had no tracked flow: peerPort=%u", (unsigned)ntohs(peer.sin_port));
      closesocket(client);
      continue;
    }
    const unsigned char peer0 = (unsigned char)(ntohl(peer.sin_addr.s_addr) >> 24);
    if (peer0 != 127 && peer.sin_addr.s_addr != target.address)
    {
      closesocket(client);
      continue;
    }
    if (InterlockedCompareExchange(&g_acceptSeen, 0, 0) <= 5)
      Log("relay mapped peerPort=%u app=%s targetPort=%u", (unsigned)ntohs(peer.sin_port), app,
          (unsigned)target.port);
    RelayJob *job = new RelayJob{client, target, profile};
    appproxy::Copy(job->app, sizeof(job->app), app);
    HANDLE t = CreateThread(nullptr, 0, RelayJobProc, job, 0, nullptr);
    if (t)
      CloseHandle(t);
    else
    {
      delete job;
      closesocket(client);
    }
  }
  return 0;
}

bool StartRelay()
{
  g_relayListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_relayListener == INVALID_SOCKET)
    return false;
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = 0;
  // Bind every local IPv4 address, not only 127.0.0.1. A redirected packet keeps the application's local
  // address and swaps only the endpoints, so its destination is that address at the relay port.
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(g_relayListener, (sockaddr *)&a, sizeof(a)) != 0 || listen(g_relayListener, 128) != 0)
    return false;
  int len = sizeof(a);
  getsockname(g_relayListener, (sockaddr *)&a, &len);
  g_relayPort = ntohs(a.sin_port);
  g_relayThread = CreateThread(nullptr, 0, RelayAcceptProc, nullptr, 0, nullptr);
  return g_relayThread != nullptr;
}

bool Reinject(bool original, const unsigned char *packet, UINT len, WINDIVERT_ADDRESS *addr)
{
  // Mark our own sends so the `not impostor` filter cannot recapture them in a loop.
  addr->Impostor = 1;
  if (!original)
  {
    addr->IPChecksum = 0;
    addr->TCPChecksum = 0;
    addr->UDPChecksum = 0;
    g_wd.CalcChecksums((void *)(uintptr_t)packet, len, addr, 0);
  }
  UINT sent = 0;
  const bool ok = g_wd.Send(g_divert, packet, len, &sent, addr) && sent == len;
  if (!ok && InterlockedIncrement(&g_sendFail) <= 5)
    Log("WinDivertSend failed: error=%lu sent=%u len=%u outbound=%u",
        GetLastError(), (unsigned)sent, (unsigned)len, (unsigned)addr->Outbound);
  return ok;
}

bool ShouldSkipTarget(const TargetV4 &t, const appproxy::Profile &p)
{
  if (LocalTarget(t))
    return true;
  sockaddr_in proxy = {};
  proxy.sin_family = AF_INET;
  inet_pton(AF_INET, p.server, &proxy.sin_addr);
  return proxy.sin_addr.s_addr == t.address;
}

void FormatTarget6(const WINDIVERT_IPV6HDR *ip, unsigned short port, char *out, int cap)
{
  char host[80] = {0};
  inet_ntop(AF_INET6, (void *)ip->DstAddr, host, sizeof(host));
  snprintf(out, cap, "[%s]:%u", host, (unsigned)port);
}

bool LocalTarget6(const WINDIVERT_IPV6HDR *ip)
{
  const unsigned char *a = (const unsigned char *)ip->DstAddr;
  bool loopback = true;
  for (int i = 0; i < 15; ++i)
    if (a[i] != 0)
      loopback = false;
  if (loopback && a[15] == 1)
    return true;
  if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80)
    return true; // link-local
  return (a[0] & 0xfe) == 0xfc; // unique-local
}

bool BlockQuicEnabled()
{
  EnterCriticalSection(&g_lock);
  const bool enabled = g_cfg.blockQuic != 0;
  LeaveCriticalSection(&g_lock);
  return enabled;
}

void RunPacketLoop()
{
  unsigned char *packet = new unsigned char[kMaxPacket];
  WINDIVERT_ADDRESS addr;
  while (!InterlockedCompareExchange(&g_serviceStop, 0, 0))
  {
    UINT len = 0;
    memset(&addr, 0, sizeof(addr));
    if (!g_wd.Recv(g_divert, packet, kMaxPacket, &len, &addr))
    {
      if (GetLastError() == ERROR_NO_DATA)
        continue;
      break;
    }
    if (InterlockedIncrement(&g_packetSeen) == 1)
      Log("first packet seen by WinDivert");

    if (addr.IPv6)
    {
      if (len < sizeof(WINDIVERT_IPV6HDR))
      {
        Reinject(true, packet, len, &addr);
        continue;
      }
      WINDIVERT_IPV6HDR *ip6 = (WINDIVERT_IPV6HDR *)packet;
      if (ip6->NextHdr == IPPROTO_TCP && len >= sizeof(WINDIVERT_IPV6HDR) + sizeof(WINDIVERT_TCPHDR))
      {
        WINDIVERT_TCPHDR *tcp =
            (WINDIVERT_TCPHDR *)(packet + sizeof(WINDIVERT_IPV6HDR));
        if (!tcp->Syn || tcp->Ack)
        {
          Reinject(true, packet, len, &addr);
          continue;
        }
        const unsigned short localPort = ntohs(tcp->SrcPort);
        const DWORD pid = Tcp6OwnerPid(localPort);
        appproxy::Profile profile;
        if (pid && !LocalTarget6(ip6) && ResolveRule(pid, &profile))
        {
          char app[64] = {0};
          char target[96] = {0};
          ProcessName(pid, app, sizeof(app));
          FormatTarget6(ip6, ntohs(tcp->DstPort), target, sizeof(target));
          RecordMatch(app, target);
          InterlockedIncrement(&g_ruleMatched);
          if (InterlockedIncrement(&g_ipv6Blocked) == 1)
            Log("first selected IPv6 TCP connection blocked so the app falls back to IPv4");
          // The relay speaks IPv4. Dropping the selected IPv6 SYN makes Happy Eyeballs try the IPv4
          // address, whose SYN is then redirected through the configured proxy.
          MaybeWriteRunningStatus();
          continue;
        }
        Reinject(true, packet, len, &addr);
        continue;
      }
      if (ip6->NextHdr == IPPROTO_UDP && len >= sizeof(WINDIVERT_IPV6HDR) + sizeof(WINDIVERT_UDPHDR))
      {
        WINDIVERT_UDPHDR *udp =
            (WINDIVERT_UDPHDR *)(packet + sizeof(WINDIVERT_IPV6HDR));
        if (ntohs(udp->DstPort) != 443 || !BlockQuicEnabled())
        {
          Reinject(true, packet, len, &addr);
          continue;
        }
        const unsigned short localPort = ntohs(udp->SrcPort);
        const DWORD pid = Udp6OwnerPid(localPort);
        appproxy::Profile profile;
        if (pid && !LocalTarget6(ip6) && ResolveRule(pid, &profile))
        {
          char app[64] = {0};
          char target[96] = {0};
          ProcessName(pid, app, sizeof(app));
          FormatTarget6(ip6, 443, target, sizeof(target));
          RecordMatch(app, target);
          InterlockedIncrement(&g_ruleMatched);
          InterlockedIncrement(&g_quicBlocked);
          MaybeWriteRunningStatus();
          continue;
        }
      }
      Reinject(true, packet, len, &addr);
      continue;
    }

    WINDIVERT_IPHDR *ip = (WINDIVERT_IPHDR *)packet;
    const int ipLen = (ip->HdrLength & 0xF) * 4;
    if ((ip->Version & 0xF) != 4 || ipLen < 20 || (UINT)ipLen + 8 > len)
    {
      Reinject(true, packet, len, &addr);
      continue;
    }
    if (ip->Protocol == IPPROTO_TCP)
    {
      WINDIVERT_TCPHDR *tcp = (WINDIVERT_TCPHDR *)(packet + ipLen);
      // The relay's own reply comes from <local-IP>:relay. Put the original destination back and inject it
      // inbound, which is how the application's TCP stack sees a normal reply from the site it contacted.
      if (addr.Outbound && ntohs(tcp->SrcPort) == g_relayPort)
      {
        const unsigned short clientPort = ntohs(tcp->DstPort);
        TargetV4 original = {};
        appproxy::Profile profile = {};
        char app[64] = {0};
        if (FindFlow(clientPort, &original, &profile, app, sizeof(app)))
        {
          tcp->SrcPort = htons(original.port);
          const unsigned int from = ip->SrcAddr;
          ip->SrcAddr = ip->DstAddr;
          ip->DstAddr = from;
          addr.Outbound = 0;
          Reinject(false, packet, len, &addr);
          MaybeWriteRunningStatus();
          continue;
        }
        Reinject(true, packet, len, &addr);
        continue;
      }

      // Everything else here is application traffic. A fresh SYN decides the rule; every later packet on
      // that source port follows the tracked flow. Redirecting only the SYN (the first version did that)
      // leaves the handshake unable to complete.
      if (!addr.Outbound)
      {
        Reinject(true, packet, len, &addr);
        continue;
      }
      const unsigned short localPort = ntohs(tcp->SrcPort);
      const TargetV4 target = {ip->DstAddr, ntohs(tcp->DstPort)};
      appproxy::Profile profile = {};
      char app[64] = {0};
      bool tracked = FindFlow(localPort, nullptr, &profile, app, sizeof(app));
      if (!tracked)
      {
        if (!(tcp->Syn) || tcp->Ack)
        {
          Reinject(true, packet, len, &addr);
          continue;
        }
        const DWORD pid = TcpOwnerPid(localPort);
        if (!pid || !ResolveRule(pid, &profile))
        {
          Reinject(true, packet, len, &addr);
          continue;
        }
        ProcessName(pid, app, sizeof(app));
        if (ShouldSkipTarget(target, profile))
        {
          Reinject(true, packet, len, &addr);
          continue;
        }
        RememberFlow(localPort, target, profile, app);
        char targetText[96] = {0};
        FormatTarget(target, targetText, sizeof(targetText));
        RecordMatch(app, targetText);
        if (InterlockedIncrement(&g_ruleMatched) == 1)
          Log("first rule match: pid=%lu app=%s target=%s", (unsigned long)pid, app, targetText);
        if (InterlockedIncrement(&g_rewriteCount) == 1)
          Log("first TCP connection redirected to relay %u", (unsigned)g_relayPort);
      }
      tcp->DstPort = htons(g_relayPort);
      const unsigned int from = ip->SrcAddr;
      ip->SrcAddr = ip->DstAddr;
      ip->DstAddr = from;
      addr.Outbound = 0;
      Reinject(false, packet, len, &addr);
    }
    else if (ip->Protocol == IPPROTO_UDP)
    {
      WINDIVERT_UDPHDR *udp = (WINDIVERT_UDPHDR *)(packet + ipLen);
      if (ntohs(udp->DstPort) != 443)
      {
        Reinject(true, packet, len, &addr);
        continue;
      }
      const unsigned short localPort = ntohs(udp->SrcPort);
      DWORD pid = UdpOwnerPid(localPort);
      appproxy::Profile profile;
      if (!pid || !BlockQuicEnabled() || !ResolveRule(pid, &profile))
      {
        Reinject(true, packet, len, &addr);
        continue;
      }
      char app[64] = {0};
      char target[96] = {0};
      ProcessName(pid, app, sizeof(app));
      const TargetV4 quicTarget = {ip->DstAddr, 443};
      FormatTarget(quicTarget, target, sizeof(target));
      RecordMatch(app, target);
      InterlockedIncrement(&g_ruleMatched);
      InterlockedIncrement(&g_quicBlocked);
    }
    else
      Reinject(true, packet, len, &addr);
    MaybeWriteRunningStatus();
  }
  delete[] packet;
}

bool LoadWinDivert()
{
  if (!g_windivertDir[0])
    return false;
  char dll[MAX_PATH];
  snprintf(dll, sizeof(dll), "%s\\WinDivert.dll", g_windivertDir);
  g_wd.dll = LoadLibraryExA(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!g_wd.dll)
  {
    Log("LoadLibrary(%s) failed: %lu", dll, GetLastError());
    return false;
  }
  g_wd.Open = (WinDivertOpenFn)GetProcAddress(g_wd.dll, "WinDivertOpen");
  g_wd.Recv = (WinDivertRecvFn)GetProcAddress(g_wd.dll, "WinDivertRecv");
  g_wd.Send = (WinDivertSendFn)GetProcAddress(g_wd.dll, "WinDivertSend");
  g_wd.Shutdown = (WinDivertShutdownFn)GetProcAddress(g_wd.dll, "WinDivertShutdown");
  g_wd.Close = (WinDivertCloseFn)GetProcAddress(g_wd.dll, "WinDivertClose");
  g_wd.SetParam = (WinDivertSetParamFn)GetProcAddress(g_wd.dll, "WinDivertSetParam");
  g_wd.CalcChecksums =
      (WinDivertHelperCalcChecksumsFn)GetProcAddress(g_wd.dll, "WinDivertHelperCalcChecksums");
  return g_wd.Open && g_wd.Recv && g_wd.Send && g_wd.Shutdown && g_wd.Close && g_wd.CalcChecksums;
}

bool RunEngine()
{
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    return false;
  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  InitializeCriticalSection(&g_statusLock);
  g_statusLockReady = true;
  LoadConfig(true);
  if (!LoadWinDivert())
  {
    WriteStatus("error", "WinDivert.dll missing or invalid");
    return false;
  }
  if (!StartRelay())
  {
    WriteStatus("error", "relay listener failed");
    return false;
  }
  _snprintf(g_filter, sizeof(g_filter),
            "not impostor and ((tcp and (outbound or tcp.SrcPort == %u or tcp.DstPort == %u)) or "
            "(udp and outbound and udp.DstPort == 443))",
            (unsigned)g_relayPort, (unsigned)g_relayPort);
  g_filter[sizeof(g_filter) - 1] = 0;
  g_divert = (HANDLE)g_wd.Open(g_filter, 0, 30000, 0);
  if (g_divert == INVALID_HANDLE_VALUE)
  {
    char detail[160];
    snprintf(detail, sizeof(detail), "WinDivertOpen failed: %lu", GetLastError());
    Log("%s", detail);
    WriteStatus("error", detail);
    return false;
  }
  Log("engine running, relay=%u", (unsigned)g_relayPort);
  WriteStatus("running", "driver attached");
  RunPacketLoop();
  InterlockedExchange(&g_serviceStop, 1);
  g_wd.Shutdown(g_divert, 3);
  if (g_relayListener != INVALID_SOCKET)
    closesocket(g_relayListener);
  if (g_relayThread)
  {
    WaitForSingleObject(g_relayThread, 2000);
    CloseHandle(g_relayThread);
  }
  g_wd.Close(g_divert);
  WriteStatus("stopped", "service stopped");
  WSACleanup();
  return true;
}

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_serviceStatus = {};

void WINAPI ServiceCtrl(DWORD control)
{
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
  {
    InterlockedExchange(&g_serviceStop, 1);
    if (g_divert != INVALID_HANDLE_VALUE && g_wd.Shutdown)
      g_wd.Shutdown(g_divert, 3);
  }
}

void WINAPI ServiceMain(DWORD, char **)
{
  g_statusHandle =
      RegisterServiceCtrlHandlerA(kServiceName, ServiceCtrl);
  if (!g_statusHandle)
    return;
  ZeroMemory(&g_serviceStatus, sizeof(g_serviceStatus));
  g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(g_statusHandle, &g_serviceStatus);
  g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(g_statusHandle, &g_serviceStatus);
  RunEngine();
  g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

bool RunHiddenCommand(const char *command)
{
  char mutableCommand[2048];
  snprintf(mutableCommand, sizeof(mutableCommand), "%s", command);
  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(nullptr, mutableCommand, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi))
    return false;
  WaitForSingleObject(pi.hProcess, 15000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

void AddFirewallRule(const char *serviceExe)
{
  char removeCommand[800];
  snprintf(removeCommand, sizeof(removeCommand),
           "netsh advfirewall firewall delete rule name=\"%s\"", kFirewallRule);
  RunHiddenCommand(removeCommand);
  char command[1600];
  snprintf(command, sizeof(command),
           "netsh advfirewall firewall add rule name=\"%s\" dir=in action=allow program=\"%s\" "
           "enable=yes profile=any",
           kFirewallRule, serviceExe);
  if (!RunHiddenCommand(command))
    Log("could not add the AppProxy firewall rule");
}

void RemoveFirewallRule()
{
  char command[800];
  snprintf(command, sizeof(command),
           "netsh advfirewall firewall delete rule name=\"%s\"", kFirewallRule);
  RunHiddenCommand(command);
}

bool InstallService(const char *configPath, const char *windivertDir)
{
  GetModuleFileNameA(nullptr, g_serviceExe, sizeof(g_serviceExe));
  SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm)
    return false;
  char command[MAX_PATH * 3];
  char statusPath[MAX_PATH];
  snprintf(statusPath, sizeof(statusPath), "%s", configPath);
  char *slash = strrchr(statusPath, '\\');
  if (slash)
    snprintf(slash + 1, sizeof(statusPath) - (int)(slash - statusPath) - 1,
             "service-status.txt");
  snprintf(command, sizeof(command), "\"%s\" --service \"%s\" \"%s\" \"%s\"", g_serviceExe,
           configPath, windivertDir, statusPath);
  SC_HANDLE svc = CreateServiceA(scm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS,
                                 SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 command, nullptr, nullptr, nullptr, nullptr, nullptr);
  if (!svc && GetLastError() == ERROR_SERVICE_EXISTS)
    svc = OpenServiceA(scm, kServiceName, SERVICE_ALL_ACCESS);
  bool ok = false;
  if (svc)
  {
    SERVICE_STATUS status;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status))
    {
      for (int i = 0; i < 100; ++i)
      {
        if (!QueryServiceStatus(svc, &status) || status.dwCurrentState == SERVICE_STOPPED)
          break;
        Sleep(100);
      }
    }
    ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, command,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ok = StartServiceA(svc, 0, nullptr) != FALSE || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    CloseServiceHandle(svc);
  }
  if (ok)
    AddFirewallRule(g_serviceExe);
  CloseServiceHandle(scm);
  return ok;
}

bool UninstallService()
{
  SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm)
    return false;
  SC_HANDLE svc = OpenServiceA(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
  if (!svc)
  {
    CloseServiceHandle(scm);
    return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
  }
  SERVICE_STATUS status;
  ControlService(svc, SERVICE_CONTROL_STOP, &status);
  bool ok = DeleteService(svc) != FALSE;
  RemoveFirewallRule();
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return ok;
}

bool StartInstalledService()
{
  SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm)
    return false;
  SC_HANDLE svc = OpenServiceA(scm, kServiceName, SERVICE_START);
  bool ok = false;
  if (svc)
  {
    ok = StartServiceA(svc, 0, nullptr) != FALSE || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    CloseServiceHandle(svc);
  }
  CloseServiceHandle(scm);
  return ok;
}

bool StopInstalledService()
{
  SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm)
    return false;
  SC_HANDLE svc = OpenServiceA(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
  bool ok = false;
  if (svc)
  {
    SERVICE_STATUS status;
    ok = ControlService(svc, SERVICE_CONTROL_STOP, &status) != FALSE ||
         GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    CloseServiceHandle(svc);
  }
  CloseServiceHandle(scm);
  return ok;
}

} // namespace

int main(int argc, char **argv)
{
  GetModuleFileNameA(nullptr, g_serviceExe, sizeof(g_serviceExe));
  if (argc >= 5 && strcmp(argv[1], "--service") == 0)
  {
    snprintf(g_configPath, sizeof(g_configPath), "%s", argv[2]);
    snprintf(g_windivertDir, sizeof(g_windivertDir), "%s", argv[3]);
    snprintf(g_statusPath, sizeof(g_statusPath), "%s", argv[4]);
    SERVICE_TABLE_ENTRYA table[] = {{(LPSTR)kServiceName, ServiceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherA(table) ? 0 : 1;
  }
  if (argc >= 4 && strcmp(argv[1], "--install") == 0)
    return InstallService(argv[2], argv[3]) ? 0 : 1;
  if (argc >= 2 && strcmp(argv[1], "--uninstall") == 0)
    return UninstallService() ? 0 : 1;
  if (argc >= 2 && strcmp(argv[1], "--start") == 0)
    return StartInstalledService() ? 0 : 1;
  if (argc >= 2 && strcmp(argv[1], "--stop") == 0)
    return StopInstalledService() ? 0 : 1;
  if (argc >= 5 && strcmp(argv[1], "--run") == 0)
  {
    snprintf(g_configPath, sizeof(g_configPath), "%s", argv[2]);
    snprintf(g_windivertDir, sizeof(g_windivertDir), "%s", argv[3]);
    snprintf(g_statusPath, sizeof(g_statusPath), "%s", argv[4]);
    return RunEngine() ? 0 : 1;
  }
  return 2;
}
