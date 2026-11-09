// ---------------------------------------------------------------------------
// Probe for AppProxy.
//
//   feature_appproxy_probe.exe doc <AppProxy.dll> <scratch-dir/>
//   feature_appproxy_probe.exe hook <http|socks5> <AppProxyHook.payload> <scratch-dir/>
//
// DOC loads the real feature with APEX_APPPROXY_DRY=1, exercises its controls and
// checks the settings file without scanning a process.
//
// HOOK starts a direct server and a fake proxy on different loopback addresses.
// A connection before loading the payload must reach the direct server; the same
// code after loading the payload must complete the selected proxy handshake and
// receive the proxy's answer. Nothing leaves the machine and no input is touched.
// ---------------------------------------------------------------------------

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "abi.h"
#include "process_tree.h"
#include "../features/AppProxy/service_status.h"

typedef const ApexFeature *(*EntryFn)(void);

static char g_dir[512] = {0};
static int Fail(const char *what)
{
  printf("FAIL: %s (WSA error %d)\n", what, WSAGetLastError());
  return 1;
}

static int HostFeatureDir(char *out, int outSize)
{
  const int n = _snprintf(out, outSize, "%s", g_dir);
  return n > 0 ? n : 0;
}

static void HostLog(const char *text) { printf("  [feature] %s\n", text); }
static int HostEnabled(const char *) { return 1; }

static int TestProcessTree()
{
  appproxy::Config cfg;
  appproxy::Defaults(&cfg);
  appproxy::AddRule(&cfg, "chatgpt*", 1);
  appproxy::ProcessNode nodes[4] = {};
  nodes[0].pid = 100;
  nodes[0].parent = 1;
  strcpy(nodes[0].exe, "chatgpt.exe");
  nodes[1].pid = 200;
  nodes[1].parent = 100;
  strcpy(nodes[1].exe, "msedgewebview2.exe");
  nodes[2].pid = 300;
  nodes[2].parent = 1;
  strcpy(nodes[2].exe, "explorer.exe");
  nodes[3].pid = 400;
  nodes[3].parent = 100;
  strcpy(nodes[3].exe, "node.exe");
  if (!appproxy::InSelectedTree(cfg, nodes, 4, 1))
  {
    printf("FAIL: a WebView2 child did not inherit its selected parent\n");
    return 1;
  }
  if (appproxy::InSelectedTree(cfg, nodes, 4, 2))
  {
    printf("FAIL: an unrelated process matched the selected tree\n");
    return 1;
  }
  if (appproxy::InSelectedTree(cfg, nodes, 4, 3))
  {
    printf("FAIL: an arbitrary launcher child inherited the application rule\n");
    return 1;
  }
  printf("  process tree: WebView2 inherits the rule; unrelated and non-network children do not\n");
  return 0;
}

static int TestLegacyMigration()
{
  const char *legacy =
      "kind=http\r\nserver=127.0.0.9\r\nport=7897\r\nusername=u\r\npassword=p\r\n"
      "bypass_local=0\r\napp=chatgpt*|1\r\n";
  appproxy::Config cfg;
  appproxy::ParseConfig(legacy, (int)strlen(legacy), &cfg);
  if (cfg.profileCount != 1 || strcmp(cfg.profiles[0].name, "Default") != 0 ||
      cfg.profiles[0].kind != appproxy::kHttpConnect ||
      strcmp(cfg.profiles[0].server, "127.0.0.9") != 0 || cfg.profiles[0].port != 7897 ||
      cfg.ruleCount != 1 || strcmp(cfg.rules[0].profile, "Default") != 0)
  {
    printf("FAIL: the old single-proxy settings did not migrate to a Default profile\n");
    return 1;
  }
  printf("  legacy single-proxy settings migrate to a Default profile\n");
  return 0;
}

static int TestServiceStatus()
{
  const char *text =
      "state=running\r\ndetail=driver attached\r\nrelay_port=10931\r\npackets=12\r\n"
      "matches=4\r\nrewrites=3\r\nrelay_ok=2\r\nrelay_fail=1\r\nipv6_blocked=5\r\n"
      "quic_blocked=7\r\nlast_result=ok\r\nlast_app=firefox.exe\r\n"
      "last_target=203.0.113.10:443\r\nlast_error=\r\n";
  appproxy::ServiceStatus s;
  appproxy::ParseServiceStatus(text, (int)strlen(text), &s);
  if (!s.any || strcmp(s.state, "running") != 0 || strcmp(s.detail, "driver attached") != 0 ||
      s.relayPort != 10931 || s.packets != 12 || s.matches != 4 || s.rewrites != 3 ||
      s.relayOk != 2 || s.relayFail != 1 || s.ipv6Blocked != 5 || s.quicBlocked != 7 ||
      strcmp(s.lastResult, "ok") != 0 || strcmp(s.lastApp, "firefox.exe") != 0 ||
      strcmp(s.lastTarget, "203.0.113.10:443") != 0)
  {
    printf("FAIL: service-status.txt was not parsed into its named fields\n");
    return 1;
  }
  printf("  service status: named fields, relay counters and blocking counters parse\n");
  return 0;
}

static int Doc(const char *dll, const char *dir)
{
  _snprintf(g_dir, sizeof(g_dir), "%s", dir);
  SetEnvironmentVariableA("APEX_APPPROXY_DRY", "1");
  HMODULE mod = LoadLibraryA(dll);
  if (!mod)
  {
    printf("FAIL: cannot load feature DLL (error %lu)\n", GetLastError());
    return 1;
  }
  EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  const ApexFeature *f = entry ? entry() : nullptr;
  if (!f)
  {
    printf("FAIL: no ApexFeatureEntry\n");
    return 1;
  }
  printf("feature %s %s ABI %u\n", f->id, f->version, f->abiVersion);
  if (f->abiVersion != APEX_ABI_VERSION || f->structSize != sizeof(ApexFeature))
    return Fail("the feature's ABI declaration is not the host's");

  ApexHost host;
  memset(&host, 0, sizeof(host));
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLog;
  host.featureEnabled = HostEnabled;
  if (f->init(&host) != 0)
  {
    printf("FAIL: init refused\n");
    return 1;
  }

  if (!f->listOp || !f->listOp("proxies", "add", "Second", 0))
    return Fail("the second proxy profile was not added");
  if (!f->setControl || !f->setControl("proxies[0].kind", "http"))
    return Fail("the first profile's kind was refused");
  if (!f->setControl("proxies[0].server", "127.0.0.42"))
    return Fail("server was refused");
  if (f->setControl("proxies[0].port", "0"))
    return Fail("port 0 was accepted");
  if (!f->setControl("proxies[0].port", "18080"))
    return Fail("port 18080 was refused");
  if (!f->setControl("proxies[0].username", "alice"))
    return Fail("username was refused");
  if (!f->setControl("proxies[0].password", "secret"))
    return Fail("password was refused");
  if (!f->setControl("proxies[1].server", "127.0.0.43") ||
      !f->setControl("proxies[1].port", "18081"))
    return Fail("the second profile's server was refused");
  {
    const int bypassRc = f->setControl("bypassLocal", "0");
    if (!bypassRc)
      return Fail("bypassLocal=0 was refused");
  }
  if (!f->setControl("blockQuic", "1"))
    return Fail("blockQuic=1 was refused");
  if (!f->setControl("windivertDir", "C:\\Program Files\\ProxyBridge"))
    return Fail("windivertDir was refused");

  if (!f->listOp("apps", "add", "Example.EXE", 0))
    return Fail("Example.EXE was not added through the manual program field");
  if (!f->listOp("apps", "add", "C:\\Games\\*.game.exe", 0))
    return Fail("the wildcard application was not normalised and added");
  if (f->listOp("apps", "add", "apex.exe", 0))
    return Fail("apex.exe was accepted");
  if (!f->setControl("apps[1].enabled", "0"))
    return Fail("the second application could not be disabled");
  if (!f->setControl("apps[0].proxy", "Second"))
    return Fail("the first application could not select the second proxy");

  char doc[64 * 1024] = {0};
  const int n = f->settingsJson ? f->settingsJson(doc, (int)sizeof(doc)) : 0;
  if (n <= 0)
    return Fail("settingsJson returned nothing");
  FILE *out = fopen("build/_appproxy_doc.json", "wb");
  if (!out)
    return Fail("could not write the document");
  fwrite(doc, 1, (size_t)n, out);
  fclose(out);
  printf("  document written: %d bytes\n", n);

  FILE *ini = fopen("build/_appproxy_probe/AppProxy.ini", "rb");
  if (!ini)
    return Fail("the feature did not create AppProxy.ini");
  char text[8192] = {0};
  const size_t got = fread(text, 1, sizeof(text) - 1, ini);
  fclose(ini);
  text[got] = 0;
  if (!strstr(text, "profile=Default|http|127.0.0.42|18080|alice|secret\r\n") ||
      !strstr(text, "profile=Second|socks5|127.0.0.43|18081||\r\n") ||
      !strstr(text, "app=example.exe|1|Second\r\n") ||
      !strstr(text, "app=*.game.exe|0|Default\r\n"))
  {
    printf("FAIL: AppProxy.ini did not contain the expected values:\n%s\n", text);
    return 1;
  }
  if (f->shutdown)
    f->shutdown();
  if (TestProcessTree() != 0)
    return 1;
  if (TestLegacyMigration() != 0)
    return 1;
  if (TestServiceStatus() != 0)
    return 1;
  printf("OK: the page, validation, normalisation and settings file agree\n");
  return 0;
}

struct Listener
{
  SOCKET socket;
  unsigned short port;
};

static bool MakeListener(const char *ip, Listener *listener)
{
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return false;
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = 0;
  inet_pton(AF_INET, ip, &a.sin_addr);
  sockaddr bound;
  int boundLen = (int)sizeof(bound);
  if (bind(s, (sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0 ||
      getsockname(s, &bound, &boundLen) != 0)
  {
    closesocket(s);
    return false;
  }
  listener->socket = s;
  listener->port = ntohs(((sockaddr_in *)&bound)->sin_port);
  return true;
}

static bool SendAll(SOCKET s, const char *data, int len)
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

static int RecvSome(SOCKET s, char *data, int cap)
{
  const int n = recv(s, data, cap, 0);
  return n > 0 ? n : 0;
}

static bool RecvExact(SOCKET s, char *data, int len)
{
  int off = 0;
  while (off < len)
  {
    const int n = RecvSome(s, data + off, len - off);
    if (n <= 0)
      return false;
    off += n;
  }
  return true;
}

struct DirectCtx
{
  Listener listener;
  volatile LONG done;
};

static DWORD WINAPI DirectProc(LPVOID param)
{
  DirectCtx *ctx = (DirectCtx *)param;
  SOCKET c = accept(ctx->listener.socket, nullptr, nullptr);
  if (c != INVALID_SOCKET)
  {
    SendAll(c, "DIRECT", 6);
    closesocket(c);
  }
  InterlockedExchange(&ctx->done, 1);
  return 0;
}

struct ProxyCtx
{
  Listener listener;
  bool socks;
  volatile LONG done;
  volatile LONG ok;
};

static bool ReadHeaders(SOCKET c, char *out, int cap, int *used)
{
  int n = 0;
  while (n < cap - 1)
  {
    const int got = RecvSome(c, out + n, cap - 1 - n);
    if (got <= 0)
      return false;
    n += got;
    out[n] = 0;
    if (strstr(out, "\r\n\r\n"))
    {
      *used = n;
      return true;
    }
  }
  return false;
}

static DWORD WINAPI ProxyProc(LPVOID param)
{
  ProxyCtx *ctx = (ProxyCtx *)param;
  SOCKET c = accept(ctx->listener.socket, nullptr, nullptr);
  bool ok = false;
  if (c != INVALID_SOCKET)
  {
    if (ctx->socks)
    {
      char greeting[4] = {0};
      if (RecvExact(c, greeting, 4) && greeting[0] == 5 && greeting[3] == 2)
      {
        SendAll(c, "\x05\x02", 2);
        char auth[260] = {0};
        ok = RecvExact(c, auth, 14) && (unsigned char)auth[1] == 5 &&
             memcmp(auth + 2, "alice", 5) == 0;
        SendAll(c, "\x01\x00", 2);
        char req[16] = {0};
        ok = ok && RecvExact(c, req, 10) && (unsigned char)req[0] == 5 &&
             (unsigned char)req[1] == 1 &&
             (unsigned char)req[3] == 1;
        SendAll(c, "\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", 10);
        ok = ok && SendAll(c, "PROXIED", 7);
      }
    }
    else
    {
      char headers[2048] = {0};
      int used = 0;
      ok = ReadHeaders(c, headers, (int)sizeof(headers), &used) &&
           strstr(headers, "CONNECT 127.0.0.1:") != nullptr &&
           strstr(headers, "Proxy-Authorization: Basic ") != nullptr;
      const char *reply = "HTTP/1.1 200 Connection Established\r\n\r\nPROXIED";
      ok = ok && SendAll(c, reply, (int)strlen(reply));
    }
    closesocket(c);
  }
  InterlockedExchange(&ctx->ok, ok ? 1 : 0);
  InterlockedExchange(&ctx->done, 1);
  return 0;
}

static int ConnectRead(unsigned short port, char *out, int cap)
{
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return -1;
  DWORD timeout = 3000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(s, (sockaddr *)&a, sizeof(a)) != 0)
  {
    closesocket(s);
    return -2;
  }
  const int n = RecvSome(s, out, cap - 1);
  if (n > 0)
    out[n] = 0;
  closesocket(s);
  return n;
}

static int ConnectExRead(unsigned short port, char *out, int cap)
{
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return -1;
  sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (sockaddr *)&local, sizeof(local)) != 0)
  {
    closesocket(s);
    return -1;
  }
  LPFN_CONNECTEX connectEx = nullptr;
  GUID guid = WSAID_CONNECTEX;
  DWORD got = 0;
  if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &connectEx,
               sizeof(connectEx), &got, nullptr, nullptr) != 0 ||
      !connectEx)
  {
    closesocket(s);
    return -1;
  }
  sockaddr_in target;
  memset(&target, 0, sizeof(target));
  target.sin_family = AF_INET;
  target.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
  WSAOVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  ov.hEvent = WSACreateEvent();
  DWORD sent = 0;
  BOOL ok = connectEx(s, (sockaddr *)&target, sizeof(target), nullptr, 0, &sent, &ov);
  const int firstError = WSAGetLastError();
  printf("  ConnectEx: ok=%d error=%d\n", (int)ok, firstError);
  if (!ok && firstError == WSA_IO_PENDING)
  {
    const DWORD wait = WaitForSingleObject(ov.hEvent, 5000);
    printf("  ConnectEx wait=%lu timeout=%d\n", (unsigned long)wait, (int)WSA_WAIT_TIMEOUT);
    if (wait == WAIT_OBJECT_0)
    {
      DWORD finishError = 0;
      DWORD flags = 0;
      ok = WSAGetOverlappedResult(s, &ov, &sent, FALSE, &flags);
      finishError = WSAGetLastError();
      printf("  ConnectEx finish ok=%d error=%lu sent=%lu\n", (int)ok, (unsigned long)finishError,
             (unsigned long)sent);
    }
  }
  if (ov.hEvent)
    WSACloseEvent(ov.hEvent);
  if (!ok)
  {
    closesocket(s);
    return -2;
  }
  DWORD timeout = 3000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
  const int n = RecvSome(s, out, cap - 1);
  if (n > 0)
    out[n] = 0;
  closesocket(s);
  return n;
}

static int Hook(const char *kind, const char *payload, const char *dir, const char *route)
{
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return Fail("WSAStartup");
  _snprintf(g_dir, sizeof(g_dir), "%s", dir);
  char configPath[600];
  _snprintf(configPath, sizeof(configPath), "%sAppProxy.ini", dir);

  DirectCtx direct = {};
  ProxyCtx proxy = {};
  proxy.socks = _stricmp(kind, "socks5") == 0;
  if (!MakeListener("127.0.0.1", &direct.listener))
    return Fail("could not create the direct listener");
  if (!MakeListener("127.0.0.2", &proxy.listener))
    return Fail("could not create the proxy listener on 127.0.0.2");

  HANDLE dt = CreateThread(nullptr, 0, DirectProc, &direct, 0, nullptr);
  HANDLE pt = CreateThread(nullptr, 0, ProxyProc, &proxy, 0, nullptr);
  if (!dt || !pt)
    return Fail("could not start the probe servers");

  char exe[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, exe, (int)sizeof(exe));
  const char *leaf = exe;
  for (const char *p = exe; *p; ++p)
    if (*p == '\\' || *p == '/')
      leaf = p + 1;

  FILE *cfg = fopen(configPath, "wb");
  if (!cfg)
    return Fail("could not write the hook test configuration");
  fprintf(cfg, "bypass_local=0\r\nblock_quic=1\r\nprofile=Test|%s|127.0.0.2|%u|alice|secret\r\n"
               "app=%s|1|Test\r\n",
          proxy.socks ? "socks5" : "http", proxy.listener.port, leaf);
  fclose(cfg);

  char answer[64] = {0};
  int n = ConnectRead(direct.listener.port, answer, (int)sizeof(answer));
  if (n <= 0 || strcmp(answer, "DIRECT") != 0)
  {
    printf("FAIL: the control connection did not reach the direct server (answer=%s)\n", answer);
    return 1;
  }
  printf("  control connection: DIRECT\n");
  WaitForSingleObject(dt, 3000);
  CloseHandle(dt);
  closesocket(direct.listener.socket);

  SetEnvironmentVariableA("APEX_APPPROXY_TEST", "1");
  SetEnvironmentVariableA("APEX_APPPROXY_CONFIG_PATH", configPath);
  HMODULE payloadMod = LoadLibraryA(payload);
  if (!payloadMod)
  {
    printf("FAIL: cannot load the payload (error %lu)\n", GetLastError());
    return 1;
  }
  if (!GetProcAddress(payloadMod, "ApexHookProc"))
    return Fail("the payload has no ApexHookProc");
  typedef unsigned (*StatusFn)(void);
  StatusFn status = (StatusFn)(void *)GetProcAddress(payloadMod, "ApexAppProxyHookStatus");
  if (status)
    printf("  payload status: 0x%08x (ready bit=%u, patched slots=%u)\n", status(),
           status() & 1u, status() >> 8);

  {
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in quic;
    memset(&quic, 0, sizeof(quic));
    quic.sin_family = AF_INET;
    quic.sin_port = htons(443);
    inet_pton(AF_INET, "203.0.113.1", &quic.sin_addr);
    const int sent = udp == INVALID_SOCKET
                         ? -1
                         : sendto(udp, "x", 1, 0, (sockaddr *)&quic, sizeof(quic));
    const int quicError = WSAGetLastError();
    if (udp != INVALID_SOCKET)
      closesocket(udp);
    if (sent != SOCKET_ERROR || quicError != WSAENETUNREACH)
    {
      printf("FAIL: QUIC/UDP 443 was not rejected (sent=%d error=%d)\n", sent, quicError);
      return 1;
    }
    printf("  QUIC/UDP 443 rejection: ok\n");
  }

  memset(answer, 0, sizeof(answer));
  n = _stricmp(route, "connectex") == 0
          ? ConnectExRead(direct.listener.port, answer, (int)sizeof(answer))
          : ConnectRead(direct.listener.port, answer, (int)sizeof(answer));
  if (n <= 0 || strcmp(answer, "PROXIED") != 0)
  {
    typedef void (*StatsFn)(unsigned *);
    StatsFn stats = (StatsFn)(void *)GetProcAddress(payloadMod, "ApexAppProxyHookStats");
    unsigned values[7] = {0, 0, 0, 0, 0, 0, 0};
    if (stats)
      stats(values);
    printf("FAIL: the intercepted connection did not complete the proxy handshake (answer=%s, n=%d)\n",
           answer, n);
    printf("      calls=%u redirects=%u connectFailures=%u handshakeFailures=%u lastResult=%d lastWSA=%u connectEx=%u\n",
           values[0], values[1], values[2], values[3], (int)values[4], values[5], values[6]);
    return 1;
  }
  WaitForSingleObject(pt, 3000);
  CloseHandle(pt);
  closesocket(proxy.listener.socket);
  if (!proxy.ok)
    return Fail("the fake proxy rejected the handshake");
  printf("  intercepted connection: PROXIED (%s with authentication, route=%s)\n", kind, route);
  printf("OK: a matching TCP connection was redirected and authenticated\n");
  return 0;
}

int main(int argc, char **argv)
{
  if (argc >= 4 && strcmp(argv[1], "doc") == 0)
    return Doc(argv[2], argv[3]);
  if (argc >= 5 && strcmp(argv[1], "hook") == 0)
    return Hook(argv[2], argv[3], argv[4], argc >= 6 ? argv[5] : "connect");
  printf("usage:\n  %s doc <feature.dll> <scratch-dir/>\n"
         "  %s hook <http|socks5> <payload> <scratch-dir/> [connect|connectex]\n",
         argv[0], argv[0]);
  return 2;
}
