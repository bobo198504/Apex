// ---------------------------------------------------------------------------
// feature_doc_dump -- WRITE OUT EXACTLY WHAT A FEATURE'S settingsJson RETURNS.
//
// WHY: a feature page that is empty has (at least) three causes that look identical from the settings window --
// the document is empty, it is truncated, or it is not JSON -- and the page cannot say which (it draws what it
// is given). This prints the bytes and their length, and the gate around it can parse them. Same host stub as
// the other feature probes: a scratch folder, no window, nothing touched.
//
// Build: g++ -std=c++17 -O2 -I apex -I common -o build/_doc_dump.exe _diag/feature_doc_dump.cpp
// Run:   build/_doc_dump.exe <feature.dll> <scratch-dir/> <out.json>
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "abi.h"

typedef const ApexFeature *(*EntryFn)(void);

static char g_dir[512] = {0};
static int HostFeatureDir(char *out, int outSize)
{
  const int n = _snprintf(out, outSize, "%s", g_dir);
  return n > 0 ? n : 0;
}
static void HostLogLine(const char *text) { printf("  [feature] %s\n", text); }
static int HostFeatureEnabled(const char *) { return 1; }

int main(int argc, char **argv)
{
  const char *dll = argc > 1 ? argv[1] : "";
  const char *dir = argc > 2 ? argv[2] : "build/_doc_dump/";
  const char *outPath = argc > 3 ? argv[3] : "build/_doc.json";
  _snprintf(g_dir, sizeof(g_dir), "%s", dir);

  HMODULE mod = LoadLibraryA(dll);
  if (!mod)
  {
    printf("cannot load %s (err %lu)\n", dll, GetLastError());
    return 2;
  }
  EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  const ApexFeature *f = entry ? entry() : nullptr;
  if (!f)
  {
    printf("no ApexFeatureEntry\n");
    return 2;
  }
  printf("feature \"%s\" ABI %u (host speaks %u), structSize %u (host %u)\n", f->id ? f->id : "?",
         f->abiVersion, APEX_ABI_VERSION, f->structSize, (unsigned)sizeof(ApexFeature));

  ApexHost host;
  memset(&host, 0, sizeof(host));
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLogLine;
  host.featureEnabled = HostFeatureEnabled;
  const int rc = f->init ? f->init(&host) : 0;
  printf("init -> %d (0 = ready)\n", rc);

  static char doc[64 * 1024];
  memset(doc, 0, sizeof(doc));
  const int n = f->settingsJson ? f->settingsJson(doc, (int)sizeof(doc)) : 0;
  printf("settingsJson -> %d byte(s)\n", n);
  if (n > 0)
    printf("first  120: %.120s\n", doc);
  if (n > 200)
    printf("last   120: %.120s\n", doc + n - 120);

  FILE *fp = fopen(outPath, "wb");
  if (fp)
  {
    if (n > 0)
      fwrite(doc, 1, (size_t)n, fp);
    fclose(fp);
    printf("written to %s\n", outPath);
  }
  if (f->shutdown)
    f->shutdown();
  return 0;
}
