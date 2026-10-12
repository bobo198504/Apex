// Follow-up: WHY did CreateDirectoryA fail? The first probe printed "dir exists = 0" and then fopen failed
// with errno 2 -- so the directory really was not there. This prints the actual Win32 error and the exact
// bytes of the path, because the answer is in the path (a doubled separator, a trailing space, a missing
// backslash) and not in the return value.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

int main()
{
  char tmp[MAX_PATH] = {0};
  const DWORD n = GetTempPathA(MAX_PATH, tmp);
  printf("GetTempPathA returned %lu: \"%s\"\n", (unsigned long)n, tmp);
  printf("  last byte is '%c' (0x%02x)\n", tmp[n ? n - 1 : 0], (unsigned char)tmp[n ? n - 1 : 0]);

  char sub[MAX_PATH] = {0};
  const int sn = _snprintf(sub, sizeof(sub), "%svolumelock_probe_%lu\\", tmp,
                           (unsigned long)GetCurrentProcessId());
  printf("_snprintf for sub returned %d\n", sn);
  printf("sub = \"%s\" (len %d)\n", sub, (int)strlen(sub));

  SetLastError(0);
  const BOOL ok = CreateDirectoryA(sub, nullptr);
  printf("CreateDirectoryA = %d, GetLastError = %lu\n", (int)ok, (unsigned long)GetLastError());
  printf("  (183 = ERROR_ALREADY_EXISTS, 3 = ERROR_PATH_NOT_FOUND, 5 = ERROR_ACCESS_DENIED)\n");

  const DWORD attr = GetFileAttributesA(sub);
  printf("GetFileAttributesA = %s\n",
         attr == INVALID_FILE_ATTRIBUTES ? "INVALID (does not exist)" : "exists");

  // Now the same write the feature does, but reporting the REAL error.
  char path[MAX_PATH] = {0};
  _snprintf(path, sizeof(path), "%sVolumeLock.ini", sub);
  printf("\npath = \"%s\"\n", path);
  SetLastError(0);
  FILE *f = fopen(path, "wb");
  if (f)
  {
    fprintf(f, "enabled=1\n");
    fclose(f);
    printf("fopen OK -- the feature COULD have written here\n");
    remove(path);
    RemoveDirectoryA(sub);
  }
  else
  {
    printf("fopen FAILED, errno=%d (%s), GetLastError=%lu\n", errno, strerror(errno),
           (unsigned long)GetLastError());
  }
  return 0;
}
