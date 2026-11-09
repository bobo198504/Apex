// READ-ONLY: package identity and AppContainer status for an executable name.
#include <windows.h>
#include <appmodel.h>
#include <tlhelp32.h>

#include <cstdio>

int main(int argc, char **argv)
{
  const char *name = argc > 1 ? argv[1] : "ChatGPT.exe";
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 1;
  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);
  if (Process32First(snap, &pe))
  {
    do
    {
      if (_stricmp(pe.szExeFile, name) != 0)
        continue;
      HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
      UINT32 packageLen = 0;
      LONG packageRc = p ? GetPackageFullName(p, &packageLen, nullptr) : -1;
      wchar_t package[512] = {0};
      if (packageRc == ERROR_INSUFFICIENT_BUFFER && packageLen < sizeof(package) / sizeof(package[0]))
        GetPackageFullName(p, &packageLen, package);

      HANDLE token = nullptr;
      BOOL appContainer = FALSE;
      DWORD got = 0;
      if (p && OpenProcessToken(p, TOKEN_QUERY, &token))
        GetTokenInformation(token, TokenIsAppContainer, &appContainer, sizeof(appContainer), &got);
      printf("pid=%lu packageRc=%ld package=\"%ls\" appContainer=%d\n",
             (unsigned long)pe.th32ProcessID, (long)packageRc, package, (int)appContainer);
      if (token)
        CloseHandle(token);
      if (p)
        CloseHandle(p);
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  return 0;
}
