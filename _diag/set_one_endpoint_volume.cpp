// ---------------------------------------------------------------------------
// set_one_endpoint_volume -- set ONE named endpoint's volume, for verifying the VolumeLock feature the way it
// actually happens.
//
// ⚠️ WHY THIS EXISTS: the end-to-end probe leaves the device wherever it found it, so running it twice proves
// less each time -- the second run starts at 50% and "passes" without the feature having to do anything. The
// situation this feature was written for is "the box came up at 100%", and reproducing that means putting the
// device back to 100% first. This tool does exactly that, for ONE named device, and reads it back.
//
// ⚠️ IT TOUCHES ONLY THE DESCRIPTION IT IS GIVEN. It exists to set up a test, not to configure a machine.
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/set_one_endpoint_volume.cpp \
//       -o build/set_one_endpoint_volume.exe -lole32 -loleaut32 -luuid
//
// RUN:
//   build/set_one_endpoint_volume.exe "PLATANE UP2 SE" 1.0
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cstdio>
#include <cstdlib>
#include <string>

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

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    printf("usage: set_one_endpoint_volume <device-description> <0.0..1.0>\n");
    return 2;
  }
  const char *wantDesc = argv[1];
  const float want = static_cast<float>(atof(argv[2]));

  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
  {
    printf("CoInitializeEx failed\n");
    return 2;
  }

  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
      !en)
  {
    printf("no MMDeviceEnumerator\n");
    return 2;
  }

  const DWORD kAll =
      DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED;
  IMMDeviceCollection *coll = nullptr;
  if (FAILED(en->EnumAudioEndpoints(eRender, kAll, &coll)) || !coll)
  {
    printf("EnumAudioEndpoints failed\n");
    return 2;
  }

  UINT count = 0;
  coll->GetCount(&count);
  int done = 0;

  for (UINT i = 0; i < count; ++i)
  {
    IMMDevice *dev = nullptr;
    if (FAILED(coll->Item(i, &dev)) || !dev)
      continue;
    LPWSTR wid = nullptr;
    std::string id;
    if (SUCCEEDED(dev->GetId(&wid)) && wid)
    {
      id = Utf8(wid);
      CoTaskMemFree(wid);
    }
    DWORD state = 0;
    dev->GetState(&state);
    if (state == DEVICE_STATE_ACTIVE && id.size() && RegDesc(id) == wantDesc)
    {
      IAudioEndpointVolume *vol = nullptr;
      if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(&vol))) &&
          vol)
      {
        float before = -1.0f, after = -1.0f;
        vol->GetMasterVolumeLevelScalar(&before);
        vol->SetMasterVolumeLevelScalar(want, nullptr);
        vol->GetMasterVolumeLevelScalar(&after);
        printf("\"%s\": %.4f -> %.4f (asked %.4f)  %s\n", wantDesc, before, after, want,
               (after > want - 0.005f && after < want + 0.005f) ? "OK" : "*** DID NOT TAKE ***");
        ++done;
        vol->Release();
      }
      dev->Release();
      break;
    }
    dev->Release();
  }

  if (!done)
    printf("no ACTIVE endpoint with description \"%s\"\n", wantDesc);

  coll->Release();
  en->Release();
  CoUninitialize();
  return done ? 0 : 1;
}
