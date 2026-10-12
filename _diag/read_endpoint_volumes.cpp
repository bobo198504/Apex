// ---------------------------------------------------------------------------
// read_endpoint_volumes -- WHAT IS EVERY ACTIVE ENDPOINT'S VOLUME RIGHT NOW?
//
// ⚠️ WHY THIS EXISTS: the C reference implementation in `PLATANE_Vol_Lock/` was RUN to prove it works, and it
// set four real PLATANE channels to 50%. That is exactly the kind of side effect the project's rules say to
// record and undo -- but undoing means knowing what the values WERE, and the honest way to find out is to read
// them rather than to assume.
//
// ⚠️ AND IT DOES NOT RESTORE ANYTHING: it only reports. A tool that both measures and acts cannot be used to
// decide whether acting is needed. (`_diag/restore_endpoint_volumes.cpp` is the one that writes, and it carries
// the values from the earlier incident as constants.)
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/read_endpoint_volumes.cpp \
//       -o build/temp/read_endpoint_volumes.exe -lole32 -loleaut32 -luuid
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cstdio>

int main()
{
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
      !en)
  {
    printf("no enumerator\n");
    return 1;
  }

  IMMDeviceCollection *coll = nullptr;
  const DWORD kAll =
      DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED;
  if (FAILED(en->EnumAudioEndpoints(eRender, kAll, &coll)) || !coll)
  {
    en->Release();
    printf("no collection\n");
    return 1;
  }

  UINT count = 0;
  coll->GetCount(&count);

  printf("==== RENDER ENDPOINT VOLUMES (read-only) ====\n\n");
  for (UINT i = 0; i < count; ++i)
  {
    IMMDevice *dev = nullptr;
    if (FAILED(coll->Item(i, &dev)) || !dev)
      continue;

    DWORD state = 0;
    dev->GetState(&state);
    // ⚠️ A MASK, NOT AN EQUALITY -- see the note in VolumeLock's Pass: Windows sets high bits on real
    // endpoints (0x10000001 was measured here), and an equality test silently drops those devices.
    const bool active = (state & DEVICE_STATE_ACTIVE) != 0;
    if (!active)
    {
      dev->Release();
      continue;
    }

    LPWSTR wid = nullptr;
    char id[512] = {0};
    if (SUCCEEDED(dev->GetId(&wid)) && wid)
    {
      WideCharToMultiByte(CP_UTF8, 0, wid, -1, id, sizeof(id) - 1, nullptr, nullptr);
      CoTaskMemFree(wid);
    }

    // The device description, from the registry key named by the endpoint's own guid.
    char desc[256] = {0};
    const char *dot = strrchr(id, '.');
    const char *guid = dot ? dot + 1 : id;
    char key[512] = {0};
    _snprintf(key, sizeof(key),
              "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%s\\Properties", guid);
    wchar_t wkey[600] = {0};
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 599);
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wkey, 0, KEY_READ, &h) == ERROR_SUCCESS)
    {
      wchar_t wdesc[256] = {0};
      DWORD cb = sizeof(wdesc), type = 0;
      if (RegQueryValueExW(h, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", nullptr, &type,
                           reinterpret_cast<LPBYTE>(wdesc), &cb) == ERROR_SUCCESS)
        WideCharToMultiByte(CP_UTF8, 0, wdesc, -1, desc, sizeof(desc) - 1, nullptr, nullptr);
      RegCloseKey(h);
    }

    IAudioEndpointVolume *vol = nullptr;
    float level = -1.0f;
    BOOL muted = FALSE;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void **>(&vol))) &&
        vol)
    {
      vol->GetMasterVolumeLevelScalar(&level);
      vol->GetMute(&muted);
      vol->Release();
    }

    printf("  %-38s %6.2f%%%s\n", desc[0] ? desc : "(no description)", level * 100.0f,
           muted ? "   [MUTED]" : "");
    dev->Release();
  }

  coll->Release();
  en->Release();
  CoUninitialize();
  return 0;
}
