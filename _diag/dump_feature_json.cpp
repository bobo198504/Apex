// ---------------------------------------------------------------------------
// dump_feature_json -- call ONE feature's settingsJson and write it out VERBATIM, so it can be looked at
// byte by byte.
//
// ⚠️ WHY THIS EXISTS: `check_feature_chart.sh` said VolumeLock's control document was not JSON ("position 1"),
// and its own temporary copy is deleted when it finishes -- so the failing bytes could not be inspected.
// Reading the document the way the HOST reads it, and writing it out untouched, is the only way to see what
// the page would actually be handed.
//
// It writes the raw bytes plus a hex dump of the head, because "position 1" means the problem is in the first
// few characters and a terminal that re-encodes them would hide exactly the thing being looked for.
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole -I apex -I common _diag/dump_feature_json.cpp \
//       -o build/dump_feature_json.exe -lole32 -loleaut32 -luuid
//
// RUN:
//   build/dump_feature_json.exe build/apex/Plugins/VolumeLock/VolumeLock.dll
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../apex/abi.h"

static int FakeFeatureDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  _snprintf(out, outSize, "%s", "C:\\Windows\\Temp\\dump_feature_json\\");
  return 1;
}

static int FakeFeatureEnabled(const char *id)
{
  (void)id;
  return 1;
}

static void FakeLogLine(const char *text) { printf("      [feature log] %s\n", text ? text : ""); }

// ⚠️ THE FIELD ORDER IS THE ABI AND IT WAS COPIED OUT OF apex/abi.h, NOT GUESSED: abiVersion, structSize,
// targetAt, injectDeltas, logLine, featureDir, activity, activeEngines, featureEnabled, hostUser.
static const ApexHost kHost = {
    APEX_ABI_VERSION,   // abiVersion
    sizeof(ApexHost),   // structSize
    nullptr,            // targetAt
    nullptr,            // injectDeltas
    FakeLogLine,        // logLine
    FakeFeatureDir,     // featureDir
    nullptr,            // activity
    nullptr,            // activeEngines
    FakeFeatureEnabled, // featureEnabled
    nullptr,            // hostUser
};

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("usage: dump_feature_json <feature-dll>\n");
    return 2;
  }

  HMODULE mod = LoadLibraryA(argv[1]);
  if (!mod)
  {
    printf("LoadLibrary failed (%lu)\n", (unsigned long)GetLastError());
    return 2;
  }
  typedef const ApexFeature *(__cdecl *EntryFn)(void);
  EntryFn entry = reinterpret_cast<EntryFn>(reinterpret_cast<void *>(GetProcAddress(mod, "ApexFeatureEntry")));
  if (!entry)
  {
    printf("no ApexFeatureEntry\n");
    return 2;
  }
  const ApexFeature *api = entry();
  printf("feature: %s (abi %u)\n", api->id, api->abiVersion);

  if (api->init(&kHost) != 0)
  {
    printf("init refused\n");
    return 1;
  }

  static char json[16384];
  memset(json, 0, sizeof(json));
  const int rc = api->settingsJson ? api->settingsJson(json, (int)sizeof(json)) : 0;
  printf("settingsJson returned %d\n", rc);

  const size_t len = strlen(json);
  printf("length: %zu bytes\n\n", len);
  printf("---- RAW ----\n%s\n---- END ----\n\n", json);

  printf("---- FIRST 80 BYTES, HEX ----\n");
  for (size_t i = 0; i < len && i < 80; ++i)
  {
    printf("%02x ", (unsigned char)json[i]);
    if ((i + 1) % 16 == 0)
      printf("\n");
  }
  printf("\n\n---- FIRST 120 BYTES, AS CHARACTERS (non-printable shown as '.') ----\n");
  for (size_t i = 0; i < len && i < 120; ++i)
  {
    const unsigned char c = (unsigned char)json[i];
    printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
  }
  printf("\n");

  // ⚠️ THE SPECIFIC QUESTION: what is at index 1? The gate said "position 1", which is the second character --
  // and if the string does not start with `{"` then the very first KEY is malformed.
  printf("\n---- DIAGNOSIS ----\n");
  if (len >= 2)
  {
    printf("byte[0] = 0x%02x '%c'\n", (unsigned char)json[0],
           (unsigned char)json[0] >= 0x20 ? json[0] : '.');
    printf("byte[1] = 0x%02x '%c'\n", (unsigned char)json[1],
           (unsigned char)json[1] >= 0x20 ? json[1] : '.');
    printf("byte[2] = 0x%02x '%c'\n", (unsigned char)json[2],
           (unsigned char)json[2] >= 0x20 ? json[2] : '.');
    if (json[0] != '{' || json[1] != '"')
      printf("*** the document does not begin with `{\"` -- the first key is malformed ***\n");
    else
      printf("the document begins correctly with `{\"`\n");
  }

  api->shutdown();
  FreeLibrary(mod);
  return 0;
}
