#include <windows.h>
#include <cstdio>

typedef HANDLE(WINAPI *OpenFn)(const char *, int, short, unsigned long long);
typedef BOOL(WINAPI *CloseFn)(HANDLE);

int main()
{
  HMODULE dll = LoadLibraryA("C:\\Program Files\\ProxyBridge\\WinDivert.dll");
  if (!dll)
  {
    printf("LoadLibrary failed: %lu\n", GetLastError());
    return 1;
  }
  OpenFn open = (OpenFn)(void *)GetProcAddress(dll, "WinDivertOpen");
  CloseFn close = (CloseFn)(void *)GetProcAddress(dll, "WinDivertClose");
  const char *filters[] = {
      "true",
      "tcp",
      "tcp and tcp.Syn",
      "tcp.Syn and tcp.Ack == 0",
      "outbound and tcp.Syn",
      "not impostor and outbound and tcp.Syn",
      "udp.DstPort == 443",
      "outbound and udp.DstPort == 443",
      "not impostor and outbound and (tcp.Syn or udp.DstPort == 443)",
  };
  for (const char *filter : filters)
  {
    SetLastError(0);
    HANDLE h = open(filter, 0, 0, 0);
    printf("%-70s -> %p error=%lu\n", filter, h, GetLastError());
    if (h != INVALID_HANDLE_VALUE)
      close(h);
  }
  return 0;
}
