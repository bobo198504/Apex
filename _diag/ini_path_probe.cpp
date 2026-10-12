// Why does the feature not write its settings file when the probe gives it a temporary folder?
//
// ⚠️ THE SUSPECT IS `_snprintf`'s RETURN VALUE, which is NOT the length written in the way `snprintf` is:
// on truncation it returns a NEGATIVE number, and it does not guarantee a terminator. `ConfigPath` is written
// as `return _snprintf(...) > 0;` -- so a path that somehow overflows would be reported as "no path" and
// `SaveSettings` would silently do nothing at all. This reproduces the exact expression the feature uses,
// with the exact buffer sizes, so the answer is not a guess.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

int main()
{
  // Exactly the shapes the feature uses: g_dir is 512, the path buffer is 600, the settings name is 14 chars.
  char g_dir[512] = {0};
  GetTempPathA(512, g_dir);
  char sub[512] = {0};
  _snprintf(sub, sizeof(sub), "%svolumelock_probe_%lu\\", g_dir, (unsigned long)GetCurrentProcessId());
  CreateDirectoryA(sub, nullptr);
  _snprintf(g_dir, sizeof(g_dir), "%s", sub);

  printf("g_dir   = \"%s\" (len %d)\n", g_dir, (int)strlen(g_dir));
  printf("dir exists = %d\n", (int)(GetFileAttributesA(g_dir) != INVALID_FILE_ATTRIBUTES));

  char path[600] = {0};
  const int n = _snprintf(path, sizeof(path), "%s%s", g_dir, "VolumeLock.ini");
  printf("_snprintf returned %d\n", n);
  printf("path    = \"%s\"\n", path);
  printf("ConfigPath would say: %s\n", n > 0 ? "TRUE (a path)" : "FALSE (no path)");

  // And the actual write, with the same call the feature makes.
  FILE *f = fopen(path, "wb");
  printf("fopen(\"wb\") = %s\n", f ? "OK" : "FAILED");
  if (f)
  {
    fprintf(f, "enabled=1\npercent=50\nmatch=PLATANE UP2 SE\n");
    fclose(f);
    printf("wrote it; size now = ", 0);
    FILE *r = fopen(path, "rb");
    if (r)
    {
      fseek(r, 0, SEEK_END);
      printf("%ld bytes\n", ftell(r));
      fclose(r);
    }
    remove(path);
  }
  else
  {
    printf("errno=%d (%s)\n", errno, strerror(errno));
  }
  return 0;
}
