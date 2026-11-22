// ---------------------------------------------------------------------------
// feature_volumelock_probe -- DOES THE FEATURE ACTUALLY SET THE VOLUME, ON A REAL DEVICE?
//
// WHY THIS EXISTS: a gate that loads a feature's DLL against a FAKE host answers "does the feature describe
// itself correctly". It cannot answer THE question about this feature, because this feature's whole job is to
// WRITE A REAL DEVICE'S VOLUME and there is no fake for that -- the value has to be read back off the endpoint.
//
// SO THIS IS THE END-TO-END TEST, and it now walks the WHOLE user path rather than a single write:
//
//   1. the settings document is dumped and PARSED as JSON (a document the page cannot read is a page that
//      draws nothing, and every other check here would still pass);
//   2. the candidate dropdown is checked -- it must offer the machine's real devices, because that is how a
//      device gets added at all (the user asked NOT to type a name: "不要让用户自己填");
//   3. a device is ADDED the way the page adds one (`setControl("addDevice", <description>)`);
//   4. the row's slider is moved the way a drag moves it, and the DEVICE'S OWN VOLUME IS READ BACK -- this is
//      the user's requirement that the fader changes the device immediately ("推子拉的时候，可以直接再次更改对应
//      设备的音量");
//   5. the row is removed and the device is left alone;
//   6. ⚠️ AND EVERY OTHER ENDPOINT IS CHECKED TO BE UNTOUCHED, because "其它设备不动" is the other half of the
//      requirement and a feature that set everything to 50% would pass every check above.
//
// ⚠️⚠️ IT WRITES REAL DEVICE VOLUMES. That is what it is for, and it is run with the user's permission on the
// device they named. It restores that device's level at the end.
//
// ⚠️ THE FEATURE'S OWN DIRECTORY IS A SCRATCH ONE. The real host hands it Plugins/VolumeLock/, where the USER's
// settings live; a probe that pointed there would overwrite them. (Measured: a process started from this
// toolchain CANNOT create a directory under %LOCALAPPDATA%\Temp -- CreateDirectoryA returns ERROR_ACCESS_DENIED
// -- so the scratch folder is made under the project's own build/ instead.)
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/feature_volumelock_probe.cpp \
//       -o build/temp/feature_volumelock_probe.exe -lole32 -loleaut32 -luuid
//
// RUN:
//   build/temp/feature_volumelock_probe.exe <dll-path> <device-description> [percent] [scratch-dir]
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../apex/abi.h"

// ---- the host the feature will be given -------------------------------------------------------
static char g_dir[MAX_PATH] = {0};
static char g_dirOverride[MAX_PATH] = {0};
static bool g_cleanUpScratch = false;
static int g_logLines = 0;

static void FakeLogLine(const char *text)
{
  ++g_logLines;
  printf("      [feature log] %s\n", text ? text : "");
}

static int FakeFeatureDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  _snprintf(out, outSize, "%s", g_dir);
  return 1;
}

static int FakeFeatureEnabled(const char *id)
{
  (void)id;
  return 1;
}

// ⚠️ THE FIELD ORDER IS THE ABI AND IT WAS COPIED OUT OF apex/abi.h: abiVersion, structSize, targetAt,
// injectDeltas, logLine, featureDir, activity, activeEngines, featureEnabled, hostUser.
static const ApexHost kFakeHost = {
    APEX_ABI_VERSION, sizeof(ApexHost), nullptr, nullptr, FakeLogLine, FakeFeatureDir, nullptr, nullptr,
    FakeFeatureEnabled, nullptr,
};

// ---- reading a real endpoint's volume ----------------------------------------------------------

static std::string Utf8(const wchar_t *w)
{
  if (!w)
    return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1)
    return "";
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

static std::string RegDesc(const std::string &endpointId)
{
  const size_t dot = endpointId.rfind('.');
  const std::string guid = (dot == std::string::npos) ? endpointId : endpointId.substr(dot + 1);
  std::string key = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guid +
                    "\\Properties";
  wchar_t wkey[512] = {0};
  MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, wkey, 511);
  HKEY h = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wkey, 0, KEY_READ, &h) != ERROR_SUCCESS)
    return "";
  wchar_t buf[512] = {0};
  DWORD cb = sizeof(buf), type = 0;
  std::string out;
  if (RegQueryValueExW(h, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", nullptr, &type,
                       reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ) && cb > 2)
    out = Utf8(buf);
  RegCloseKey(h);
  return out;
}

struct Row
{
  std::string id;
  std::string desc;
  bool active = false;
  float vol = -1.0f;
};

static void Snapshot(std::vector<Row> &rows)
{
  rows.clear();
  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
      !en)
    return;
  const DWORD kAll =
      DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED;
  IMMDeviceCollection *coll = nullptr;
  if (SUCCEEDED(en->EnumAudioEndpoints(eRender, kAll, &coll)) && coll)
  {
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
      IMMDevice *dev = nullptr;
      if (FAILED(coll->Item(i, &dev)) || !dev)
        continue;
      LPWSTR wid = nullptr;
      Row r;
      if (SUCCEEDED(dev->GetId(&wid)) && wid)
      {
        r.id = Utf8(wid);
        CoTaskMemFree(wid);
      }
      DWORD state = 0;
      dev->GetState(&state);
      r.active = (state & DEVICE_STATE_ACTIVE) != 0;
      r.desc = RegDesc(r.id);
      IAudioEndpointVolume *vol = nullptr;
      if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(&vol))) &&
          vol)
      {
        vol->GetMasterVolumeLevelScalar(&r.vol);
        vol->Release();
      }
      rows.push_back(r);
      dev->Release();
    }
    coll->Release();
  }
  en->Release();
}

static float VolumeOf(const std::vector<Row> &rows, const std::string &desc)
{
  for (const Row &r : rows)
  {
    if (r.active && r.desc == desc)
      return r.vol;
  }
  return -1.0f;
}

// Put ONE device's volume back where it was. See the note at the end of Run: a probe that changes a real
// device restores it, because the level it found belongs to the user.
static void RestoreDeviceVolume(const std::string &desc, float volume)
{
  if (volume < 0.0f || !desc.size())
    return;
  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
      !en)
    return;
  const DWORD kAll =
      DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED;
  IMMDeviceCollection *coll = nullptr;
  if (SUCCEEDED(en->EnumAudioEndpoints(eRender, kAll, &coll)) && coll)
  {
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
      IMMDevice *dev = nullptr;
      if (FAILED(coll->Item(i, &dev)) || !dev)
        continue;
      DWORD state = 0;
      dev->GetState(&state);
      LPWSTR wid = nullptr;
      std::string id;
      if (SUCCEEDED(dev->GetId(&wid)) && wid)
      {
        id = Utf8(wid);
        CoTaskMemFree(wid);
      }
      if ((state & DEVICE_STATE_ACTIVE) != 0 && id.size() && RegDesc(id) == desc)
      {
        IAudioEndpointVolume *vol = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(&vol))) &&
            vol)
        {
          vol->SetMasterVolumeLevelScalar(volume, nullptr);
          vol->Release();
        }
        dev->Release();
        break;
      }
      dev->Release();
    }
    coll->Release();
  }
  en->Release();
}

static void CleanUpScratch()
{
  if (!g_cleanUpScratch || !g_dir[0])
    return;
  char pattern[MAX_PATH] = {0};
  _snprintf(pattern, sizeof(pattern), "%s*", g_dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h != INVALID_HANDLE_VALUE)
  {
    do
    {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        continue;
      char victim[MAX_PATH] = {0};
      _snprintf(victim, sizeof(victim), "%s%s", g_dir, fd.cFileName);
      DeleteFileA(victim);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryA(g_dir);
  g_cleanUpScratch = false;
}

static int Run(int argc, char **argv)
{
  if (argc < 3)
  {
    printf("usage: feature_volumelock_probe <dll-path> <device-description> [percent] [scratch-dir]\n");
    return 2;
  }
  const char *dllPath = argv[1];
  const char *device = argv[2];
  const int percent = (argc > 3) ? atoi(argv[3]) : 50;
  if (argc > 4 && argv[4][0])
    _snprintf(g_dirOverride, sizeof(g_dirOverride), "%s", argv[4]);

  // ⚠️⚠️ `--readonly` STOPS THE PROBE BEFORE ANYTHING IS ADDED, AND THE GATE'S DEFAULT PATH USES IT.
  //
  // The probe's value is that it walks the REAL user path -- add a device, move its fader, watch the endpoint
  // follow -- and that path writes a real device's volume. A gate runs on every commit, so its default must
  // not touch the user's audio; the steps that do are behind this flag.
  //
  // ⚠️ WHAT STILL RUNS READ-ONLY IS NOT NOTHING: the DLL loads, the contract is checked, the settings document
  // is produced and parsed, and THE MACHINE'S OWN DEVICE LIST is read (which is how the dropdown is checked at
  // all). Every one of those is a read, and together they are what a gate can honestly assert.
  const bool readOnly = (argc > 5 && strcmp(argv[5], "--readonly") == 0);

  printf("==== VolumeLock END-TO-END PROBE ====\n");
  printf("dll    : %s\n", dllPath);
  printf("device : %s\n", device);
  printf("percent: %d\n\n", percent);

  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
  {
    printf("CoInitializeEx failed\n");
    return 2;
  }

  // The scratch folder. See the header note on why it is not under %TEMP%.
  if (g_dirOverride[0])
  {
    CreateDirectoryA(g_dirOverride, nullptr);
    _snprintf(g_dir, sizeof(g_dir), "%s", g_dirOverride);
  }
  else
  {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char *slash = strrchr(exe, '\\'))
      *slash = 0;
    char sub[MAX_PATH] = {0};
    _snprintf(sub, sizeof(sub), "%s\\temp\\vl_probe_%lu\\", exe, (unsigned long)GetCurrentProcessId());
    char parent[MAX_PATH] = {0};
    _snprintf(parent, sizeof(parent), "%s\\temp", exe);
    CreateDirectoryA(parent, nullptr);
    if (!CreateDirectoryA(sub, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
      printf("could not create the scratch folder: %s (error %lu)\n", sub, (unsigned long)GetLastError());
      return 2;
    }
    _snprintf(g_dir, sizeof(g_dir), "%s", sub);
    g_cleanUpScratch = true;
  }
  printf("scratch dir: %s\n\n", g_dir);

  std::vector<Row> before;
  Snapshot(before);
  const float originalVolume = VolumeOf(before, device);
  printf("---- BEFORE ----\n");
  // ⚠️ ONLY A RUN THAT IS GOING TO WRITE NEEDS THE DEVICE TO BE THERE. The read-only run asserts things about
  // the DOCUMENT and about what the machine reports -- both of which are available whether or not this
  // particular device is plugged in, and the whole point of the read-only path is that it runs anywhere.
  if (!readOnly && originalVolume < 0.0f)
  {
    // ⚠️⚠️ "NOT PLUGGED IN" IS NOT A TEST FAILURE, AND SAYING SO PLAINLY IS THE WHOLE POINT OF THIS BRANCH.
    //
    // This probe was run while the device under test happened to be unplugged, and the output said
    // `FAIL: the device did not move to 50% (it is at -100.0%)` -- which reads as a defect in the feature and
    // is really a statement about the machine. A measurement tool that reports the state of the world as a
    // fault in the thing being measured is worse than one that reports nothing.
    //
    // ⚠️ AND IT IS A REAL POSSIBILITY, NOT A HYPOTHETICAL: this feature exists for a device the user unplugs,
    // and an earlier run of the test left it unplugged.
    printf("  \"%s\" is NOT CONNECTED right now -- no active endpoint reports that description.\n", device);
    printf("  The steps that need it cannot run. Plug the device in and run this again.\n");
    CoUninitialize();
    printf("\n==== RESULT: SKIPPED (the device is not connected) ====\n");
    return 3; // ⚠️ 3, NOT 1: "could not measure" is a different answer from "measured, and it is wrong".
  }
  printf("  \"%s\" is at %.1f%%%s\n\n", device, originalVolume * 100.0f,
         originalVolume < 0.0f ? " (not connected -- this is a read-only run)" : "");

  printf("---- LOADING THE FEATURE ----\n");
  HMODULE mod = LoadLibraryA(dllPath);
  if (!mod)
  {
    printf("  LoadLibrary failed (error %lu)\n", (unsigned long)GetLastError());
    return 2;
  }
  typedef const ApexFeature *(__cdecl *EntryFn)(void);
  EntryFn entry =
      reinterpret_cast<EntryFn>(reinterpret_cast<void *>(GetProcAddress(mod, "ApexFeatureEntry")));
  if (!entry)
  {
    printf("  no ApexFeatureEntry export -- this DLL is not a feature\n");
    return 2;
  }
  const ApexFeature *api = entry();
  printf("  id=%s  name=%s / %s  version=%s\n", api->id, api->nameZh, api->nameEn, api->version);
  // ⚠️ THE VERSION AND SIZE ARE PRINTED BECAUSE THE GATE ASSERTS THEM, and a gate that greps for a line the
  // probe never prints fails for a reason that has nothing to do with the feature. (It did: this line was
  // missing and the ABI check reported FAIL against a DLL that declares exactly the right version.)
  printf("  declared abi=%u  structSize=%u (host speaks %u / %u)\n", api->abiVersion, api->structSize,
         APEX_ABI_VERSION, (unsigned)sizeof(ApexFeature));
  if (api->abiVersion != APEX_ABI_VERSION || api->structSize < sizeof(ApexFeature))
  {
    printf("  REFUSED: the host would not load this DLL\n");
    return 1;
  }
  // ⚠️ `readOnly` IS DECLARED AT THE TOP OF Run (see the note there): the device check above needs it, and
  // every write below is behind it.
  const int rc = api->init(&kFakeHost);
  printf("  init() returned %d (%s)\n", rc, rc == 0 ? "READY" : "REFUSED");
  if (rc != 0)
    return 1;

  int failures = 0;
  static char json[16384];

  // ---- 1. THE DOCUMENT, AND THE CANDIDATES IT OFFERS ----
  //
  // ⚠️⚠️ THE CANDIDATES DO NOT EXIST UNTIL THE WORKER HAS RUN A PASS, AND THAT IS WHY THIS WAITS FOR THEM
  // RATHER THAN READING ONCE. They are collected from a real enumeration of this machine's endpoints (see the
  // candidate note in the source), and `init` returns before that enumeration has happened -- so the FIRST
  // document a freshly-loaded feature produces genuinely offers nothing, and a probe that read it once would
  // report "the dropdown is empty" about a feature that was simply not ready yet.
  //
  // ⚠️ WAITING FOR A CONDITION, NOT SLEEPING: the loop polls the document until the dropdown has at least one
  // candidate, so a slow machine costs patience and a fast one costs nothing.
  printf("\n---- 1. the settings document ----\n");
  int n = 0;
  int candidateCount = 0;
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    n = api->settingsJson(json, (int)sizeof(json));
    const char *o = strstr(json, "\"options\":[");
    const char *e = o ? strstr(o, "]") : nullptr;
    candidateCount = 0;
    for (const char *p = o; e && p < e; ++p)
    {
      if (strncmp(p, "\"value\":\"", 9) == 0 && p[9] != '"')
        ++candidateCount;
    }
    if (candidateCount > 0)
      break;
    Sleep(100);
  }
  printf("  settingsJson returned %d byte(s)\n", n);
  if (n <= 0 || (size_t)n != strlen(json))
  {
    printf("  FAIL: the return value is not the document's length\n");
    ++failures;
  }

  if (candidateCount > 0)
  {
    printf("  the dropdown offers %d candidate(s)\n", candidateCount);
    const bool mentioned = strstr(json, device) != nullptr;
    printf("  \"%s\" is among them: %s\n", device, mentioned ? "yes" : "no");
    if (!mentioned)
      printf("  (that device is not reported by this machine right now, or is already listed)\n");
  }
  else
  {
    printf("  FAIL: no device is offered, so nothing can be added\n");
    ++failures;
  }

  // ⚠️ THE DOCUMENT ITSELF IS PRINTED, BETWEEN MARKERS, SO THE GATE CAN PARSE THE REAL THING. Printing a
  // summary of it ("it has a group") would let the gate assert a description of the document instead of the
  // document -- and the failure that motivated this was a structural one the summary could not have shown.
  printf("\n---- DOCUMENT ----\n%s\n---- END DOCUMENT ----\n", json);

  if (readOnly)
  {
    printf("\n---- READ-ONLY: stopping before anything is added ----\n");
    printf("  (the steps that write a real device's volume are behind the live flag)\n");
    api->shutdown();
    FreeLibrary(mod);
    CoUninitialize();
    printf("\n==== RESULT: %s ====\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
  }

  // ---- 2. ADD THE DEVICE THE WAY THE PAGE DOES ----
  printf("\n---- 2. add \"%s\" ----\n", device);
  const int added = api->setControl("addDevice", device);
  printf("  setControl(addDevice) returned %d\n", added);
  if (added != 1)
  {
    printf("  FAIL: the device could not be added\n");
    ++failures;
  }

  // ---- 3. THE SLIDER, AND THE DEVICE ITSELF ----
  printf("\n---- 3. move the row's slider to %d%% ----\n", percent);
  char sliderPath[64] = {0};
  _snprintf(sliderPath, sizeof(sliderPath), "devices[0].percent");
  char pct[16] = {0};
  _snprintf(pct, sizeof(pct), "%d", percent);
  const int moved = api->setControl(sliderPath, pct);
  printf("  setControl(%s) returned %d\n", sliderPath, moved);
  if (moved != 1)
  {
    printf("  FAIL: the slider was refused\n");
    ++failures;
  }

  // ⚠️ WAIT FOR THE DEVICE TO ACTUALLY CHANGE, NOT FOR A FIXED TIME. The write happens on the feature's own
  // worker (see the pending-slider note in the source), so the probe polls the ENDPOINT until it reports the
  // requested level -- a fixed sleep would be either flaky or slow.
  std::vector<Row> after;
  bool changed = false;
  for (int attempt = 0; attempt < 60 && !changed; ++attempt)
  {
    Sleep(100);
    Snapshot(after);
    const float v = VolumeOf(after, device);
    if (v >= 0.0f && v > (percent / 100.0f) - 0.02f && v < (percent / 100.0f) + 0.02f)
      changed = true;
  }
  const float now = VolumeOf(after, device);
  printf("  the device now reports %.1f%%\n", now * 100.0f);
  if (changed)
  {
    printf("  OK: the fader really moved the device (this is the user's requirement)\n");
  }
  else
  {
    printf("  FAIL: the device did not move to %d%% (it is at %.1f%%)\n", percent, now * 100.0f);
    ++failures;
  }

  // ---- 4. EVERY OTHER ENDPOINT MUST BE UNTOUCHED ----
  printf("\n---- 4. other devices ----\n");
  int others = 0;
  for (const Row &a : after)
  {
    if (a.desc == device)
      continue;
    for (const Row &b : before)
    {
      if (b.id != a.id || b.vol < 0.0f || a.vol < 0.0f)
        continue;
      ++others;
      if (b.vol != a.vol)
      {
        printf("  FAIL: \"%s\" moved %.4f -> %.4f\n", a.desc.c_str(), b.vol, a.vol);
        ++failures;
      }
      break;
    }
  }
  printf("  %d other endpoint(s) compared: %s\n", others,
         failures == 0 ? "all unchanged" : "SEE FAILURES ABOVE");

  // ---- 5. REMOVE THE ROW ----
  printf("\n---- 5. remove the row ----\n");
  const int removed = api->listOp ? api->listOp("devices", "remove", "", 0) : 0;
  printf("  listOp(devices, remove, 0) returned %d\n", removed);
  if (removed != 1)
  {
    printf("  FAIL: the row could not be removed\n");
    ++failures;
  }
  // ⚠️ AND `add` MUST BE REFUSED -- an older page sends it, and accepting free text would put back the typing
  // the user asked to be rid of.
  const int addedByOp = api->listOp ? api->listOp("devices", "add", "nonsense", 0) : 0;
  printf("  listOp(devices, add, \"nonsense\") returned %d (0 = refused, which is correct)\n", addedByOp);
  if (addedByOp != 0)
  {
    printf("  FAIL: a typed name was accepted -- the user asked for choosing, not typing\n");
    ++failures;
  }

  // ---- 6. PUT THE DEVICE BACK ----
  //
  // ⚠️ A PROBE THAT CHANGES A REAL DEVICE PUTS IT BACK. The user's own level is theirs; leaving it at the test
  // value would be the test editing their machine.
  printf("\n---- 6. restoring the device ----\n");
  RestoreDeviceVolume(device, originalVolume);
  std::vector<Row> restored;
  Sleep(300);
  Snapshot(restored);
  printf("  \"%s\" is back at %.1f%% (was %.1f%%)\n", device, VolumeOf(restored, device) * 100.0f,
         originalVolume * 100.0f);

  api->shutdown();
  FreeLibrary(mod);
  CoUninitialize();

  printf("\n==== RESULT: %s ====\n", failures == 0 ? "PASS" : "FAIL");
  if (g_logLines == 0)
    printf("warning: the feature wrote no line to the host log\n");
  return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
  const int rc = Run(argc, argv);
  CleanUpScratch();
  return rc;
}
