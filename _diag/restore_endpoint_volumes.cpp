// ---------------------------------------------------------------------------
// restore_endpoint_volumes -- PUT BACK WHAT THE GATE'S OWN REVERSE-CHECK MOVED.
//
// ⚠️⚠️ WHY THIS FILE HAS TO EXIST, AND IT IS A MISTAKE I MADE, NOT A HYPOTHETICAL.
//
// To prove that the "其它设备不动" half of the VolumeLock requirement is really being checked, the matcher was
// deliberately broken to match EVERY endpoint -- and the probe was then run against the live machine. That is
// what makes the check meaningful, and it is also what set the volume of FIVE OTHER REAL DEVICES to 50%:
//
//     PLATANE1 1/2     1.0000 -> 0.5000
//     PLATANE2 3/4     1.0000 -> 0.5000
//     PLATANE3 5/6     1.0000 -> 0.5000
//     PLATANE4 7/8     1.0000 -> 0.5000
//     Realtek(R) Audio 0.2701 -> 0.5000
//
// The probe's BEFORE table recorded every one of those values, so they are recoverable EXACTLY -- and putting
// them back is not optional: the user's own mixer settings were changed by a test, and leaving them that way
// would be the test quietly editing their machine.
//
// ⚠️ THE VALUES BELOW ARE THE MEASURED "BEFORE", TYPED IN FROM THE PROBE'S OWN OUTPUT. They are not guesses
// and not round numbers -- note 0.2701, which is a value the user had actually set.
//
// This tool takes effect only for the endpoints it is told about, by device description, and prints what it
// found and what it did. It is NOT part of any feature and is NOT built by build.sh: it exists to undo one
// specific accident.
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/restore_endpoint_volumes.cpp \
//       -o build/restore_endpoint_volumes.exe -lole32 -loleaut32 -luuid
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cstdio>
#include <cstring>
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

// THE MEASURED "BEFORE" VALUES. Only these descriptions are touched; anything else on the machine is left
// exactly as it is, which is the whole point of naming them individually.
struct Fix
{
  const char *desc;
  float vol;
};

static const Fix kFixes[] = {
    {"PLATANE1 1/2", 1.0000f},
    {"PLATANE2 3/4", 1.0000f},
    {"PLATANE3 5/6", 1.0000f},
    {"PLATANE4 7/8", 1.0000f},
    {"Realtek(R) Audio", 0.2701f},
};

int main()
{
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
  {
    printf("CoInitializeEx failed\n");
    return 2;
  }

  printf("==== restoring the endpoints the gate's reverse-check moved ====\n\n");

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
  int fixed = 0, alreadyOk = 0, notFound = 0;

  for (const Fix &f : kFixes)
  {
    bool found = false;
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

      if (state == DEVICE_STATE_ACTIVE && id.size() && RegDesc(id) == f.desc)
      {
        found = true;
        IAudioEndpointVolume *vol = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(&vol))) &&
            vol)
        {
          float cur = -1.0f;
          vol->GetMasterVolumeLevelScalar(&cur);
          if (cur > f.vol - 0.0005f && cur < f.vol + 0.0005f)
          {
            printf("  \"%s\" already at %.4f -- nothing to do\n", f.desc, cur);
            ++alreadyOk;
          }
          else
          {
            const HRESULT hr = vol->SetMasterVolumeLevelScalar(f.vol, nullptr);
            float back = -1.0f;
            vol->GetMasterVolumeLevelScalar(&back);
            printf("  \"%s\"  %.4f -> %.4f  (asked %.4f, hr=0x%08lx)  %s\n", f.desc, cur, back, f.vol,
                   static_cast<unsigned long>(hr),
                   (back > f.vol - 0.0005f && back < f.vol + 0.0005f) ? "OK" : "*** DID NOT TAKE ***");
            if (back > f.vol - 0.0005f && back < f.vol + 0.0005f)
              ++fixed;
          }
          vol->Release();
        }
        dev->Release();
        break; // one active endpoint per description is what was moved
      }
      dev->Release();
    }
    if (!found)
    {
      printf("  \"%s\" -- not currently active, cannot restore\n", f.desc);
      ++notFound;
    }
  }

  coll->Release();
  en->Release();
  CoUninitialize();

  printf("\nrestored %d, already correct %d, not active %d\n", fixed, alreadyOk, notFound);
  printf("(the VolumeLock feature itself is unaffected by this -- it only ever touches PLATANE UP2 SE)\n");
  return 0;
}
