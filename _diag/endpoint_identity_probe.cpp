// ---------------------------------------------------------------------------
// endpoint_identity_probe -- WHAT IS A STABLE, UNIQUE IDENTITY FOR AN AUDIO ENDPOINT?
//
// ⚠️ WHY THIS IS A PROBE AND NOT A DECISION MADE AT THE KEYBOARD. The user asked for "设备名，或能确认它的唯一身份
// 是最好的" -- a stable AND unique identity -- and those two pull in opposite directions on this machine, which
// was already MEASURED:
//
//     the same device name appears under two USB ports with two different endpoint guids
//
// so the endpoint guid is unique but NOT stable across ports, and the device description is stable but NOT
// unique across two identical boxes. Before choosing, the question "is there a THIRD value that is both?" has
// to be asked of Windows rather than assumed -- and the candidate is the hardware id Windows builds from the
// device's own serial number (or its port, when the device publishes no serial).
//
// WHAT IT PRINTS, per render endpoint: the endpoint guid, the device description, the FRIENDLY name, the
// DEVICE INSTANCE PATH (`...&MI_00\9&2140394C&0&0000` -- the tail after `\` is the container id), the USB
// serial if the device exposes one, and whether that tail is STABLE (a serial) or PORT-DERIVED (a location).
//
// ⚠️ IT WRITES NOTHING. It opens keys for reading and activates endpoints for reading only.
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/endpoint_identity_probe.cpp \
//       -o build/endpoint_identity_probe.exe -lole32 -loleaut32 -luuid -lsetupapi
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <setupapi.h>
#include <devguid.h>

#include <cstdio>
#include <string>
#include <vector>

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

// The device description ({b3f8fa53-...},6) out of the registry, which is where this machine keeps it.
static std::string RegProp(const std::string &guid, const wchar_t *name)
{
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
  if (RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ) && cb > 2)
    out = Utf8(buf);
  RegCloseKey(h);
  return out;
}

// ⚠️ THE DEVICE INSTANCE PATH IS THE CANDIDATE FOR A REAL IDENTITY, AND IT IS A DIFFERENT VALUE FROM THE
// ENDPOINT GUID. It looks like:
//     {1}.USB\VID_350D&PID_0022&MI_00\9&2140394C&0&0000
// The part before the last `\` is the HARDWARE ID (`USB\VID_..&PID_..&MI_..`), and the part after it is the
// INSTANCE-SPECIFIC tail. On a device that publishes a USB serial number that tail CONTAINS THE SERIAL (so it
// is stable across ports); on one that does not, Windows builds it from the PORT (so it moves with the port).
static std::string RegInstancePath(const std::string &guid)
{
  return RegProp(guid, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},2");
}

int main()
{
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
  {
    printf("CoInitializeEx failed\n");
    return 2;
  }

  printf("==== AUDIO ENDPOINT IDENTITY (READ ONLY) ====\n\n");

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
  printf("%u render endpoint(s)\n", count);

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

    const size_t dot = id.rfind('.');
    const std::string guid = (dot == std::string::npos) ? id : id.substr(dot + 1);
    const std::string desc = RegProp(guid, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");
    const std::string friendly = RegProp(guid, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
    const std::string inst = RegInstancePath(guid);

    // ⚠️ ONLY THE INTERESTING ROWS: ACTIVE, or a PLATANE (the device under discussion). A listing of all 25
    // endpoints would bury the three that answer the question.
    const bool interesting = ((state & DEVICE_STATE_ACTIVE) != 0) ||
                             desc.find("PLATANE") != std::string::npos;
    if (!interesting)
    {
      dev->Release();
      continue;
    }

    printf("--------------------------------------------------------------\n");
    printf("  state       : %s\n", (state & DEVICE_STATE_ACTIVE) != 0 ? "ACTIVE" : "not active");
    printf("  endpoint id : %s\n", id.c_str());
    printf("  desc        : %s\n", desc.empty() ? "(none)" : desc.c_str());
    printf("  friendly    : %s\n", friendly.empty() ? "(none)" : friendly.c_str());
    printf("  instance    : %s\n", inst.empty() ? "(none)" : inst.c_str());

    // ⚠️ AND THE PART THAT DECIDES THE QUESTION: is the tail after the last backslash a SERIAL or a PORT?
    // A serial is stable across ports; a port is not. There is no API that says which, but the SHAPE is
    // readable: a serial is usually an alphanumeric run with no `&`, while Windows' location-derived tails
    // are built from `&`-separated hex pairs.
    const size_t bs = inst.rfind('\\');
    if (bs != std::string::npos)
    {
      const std::string tail = inst.substr(bs + 1);
      const bool looksPortDerived = tail.find('&') != std::string::npos;
      printf("  tail        : %s\n", tail.c_str());
      printf("  tail is     : %s\n",
             looksPortDerived ? "PORT-DERIVED (moves when the device is plugged in elsewhere)"
                              : "SERIAL-LIKE (should survive a different port)");
    }
    dev->Release();
  }

  printf("--------------------------------------------------------------\n");
  printf("\n==== WHAT THIS MEANS FOR \"A STABLE, UNIQUE IDENTITY\" ====\n");
  printf("  * ENDPOINT GUID  -- unique, but per PORT: the same box in another port is a different id.\n");
  printf("  * DEVICE DESC    -- stable across ports, but NOT unique: two identical boxes share it.\n");
  printf("  * INSTANCE TAIL  -- the only candidate that can be BOTH, and only when the device publishes a\n");
  printf("                      serial number. The line above says which kind this device has.\n");

  coll->Release();
  en->Release();
  CoUninitialize();
  return 0;
}
