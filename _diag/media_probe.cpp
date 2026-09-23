// ---------------------------------------------------------------------------
// media_probe -- WHAT CAN THIS MACHINE ACTUALLY DO, per monitor and per audio session?
//
// WHY THIS FILE EXISTS: the Media Control feature has to control "the brightness of each monitor" and "the
// volume of each application", and Windows has NO single API for either. Each has three or four protocols with
// different support per device, and which ones work here is a question about THE HARDWARE, not about the code.
// So it is asked, once, with a program that only READS.
//
// ⚠️ IT WRITES NOTHING IT DOES NOT PUT BACK. The one write it makes is SetDeviceGammaRamp with the ramp it just
// read -- which is a no-op on the screen and the only way to learn whether a monitor accepts a gamma write at
// all (GetDeviceGammaRamp succeeds on adapters that quietly ignore the setter). Brightness is never changed.
//
// WHAT IT PRINTS, per monitor:
//   * the GDI device name (\\.\DISPLAY1), which is what identifies a monitor across runs;
//   * whether it is the PRIMARY one and its rectangle;
//   * the physical monitor handle's own description (DDC/CI is per PHYSICAL monitor, not per display);
//   * VCP 0x10 (brightness): current / max -- and VCP 0xD6 (power mode): current, the one a "screen off"
//     would write;
//   * the DDC capabilities string, so "does this monitor even have 0x10 / 0xD6" is read rather than guessed;
//   * WMI's WmiMonitorBrightness (the laptop panel path) and WmiMonitorID (the panel's own name);
//   * whether a gamma ramp can be written back.
//
// And per audio session: the endpoint, its master volume, and every session with its process name, display
// name, volume and mute state.
//
// BUILD (from the repo root, with the shared toolchain):
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/media_probe.cpp -o build/media_probe.exe \
//       -lole32 -loleaut32 -luuid -ldxva2 -lwbemuuid -luser32
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// MONITORS
// ---------------------------------------------------------------------------------------------

struct MonitorInfo
{
  HMONITOR h = nullptr;
  RECT rc = {0, 0, 0, 0};
  bool primary = false;
  std::string device;  // \\.\DISPLAY1
  std::string desc;    // what GDI says the adapter is
  bool hasBrightness = false;
  DWORD brightCur = 0, brightMax = 0;
  bool hasPower = false;
  DWORD powerCur = 0, powerMax = 0;
  std::vector<PHYSICAL_MONITOR> phys;
  std::string physDesc;
  std::string caps;
};

static BOOL CALLBACK EnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM user)
{
  auto *out = reinterpret_cast<std::vector<MonitorInfo> *>(user);
  MonitorInfo mi;
  mi.h = hMon;

  MONITORINFOEXA ex;
  memset(&ex, 0, sizeof(ex));
  ex.cbSize = sizeof(ex);
  if (GetMonitorInfoA(hMon, &ex))
  {
    mi.rc = ex.rcMonitor;
    mi.primary = (ex.dwFlags & MONITORINFOF_PRIMARY) != 0;
    mi.device = ex.szDevice;
  }

  DWORD n = 0;
  if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &n) && n > 0)
  {
    mi.phys.resize(n);
    if (GetPhysicalMonitorsFromHMONITOR(hMon, n, mi.phys.data()))
    {
      for (DWORD i = 0; i < n; ++i)
      {
        if (i == 0)
        {
          char tmp[256] = {0};
          WideCharToMultiByte(CP_UTF8, 0, mi.phys[i].szPhysicalMonitorDescription, -1, tmp, sizeof(tmp) - 1,
                              nullptr, nullptr);
          mi.physDesc = tmp;
        }
      }
      // VCP 0x10 -- brightness, the code every DDC/CI monitor has if it has any.
      DWORD cur = 0, max = 0;
      if (GetVCPFeatureAndVCPFeatureReply(mi.phys[0].hPhysicalMonitor, 0x10, nullptr, &cur, &max))
      {
        mi.hasBrightness = true;
        mi.brightCur = cur;
        mi.brightMax = max;
      }
      // VCP 0xD6 -- power mode. 1 = on, 2 = standby, 3 = suspend, 4 = off.
      DWORD pcur = 0, pmax = 0;
      if (GetVCPFeatureAndVCPFeatureReply(mi.phys[0].hPhysicalMonitor, 0xD6, nullptr, &pcur, &pmax))
      {
        mi.hasPower = true;
        mi.powerCur = pcur;
        mi.powerMax = pmax;
      }
      // The monitor's own DDC capability string: read rather than assumed.
      DWORD len = 0;
      if (GetCapabilitiesStringLength(mi.phys[0].hPhysicalMonitor, &len) && len > 0 && len < 100000)
      {
        std::vector<char> buf(len + 1, 0);
        if (CapabilitiesRequestAndCapabilitiesReply(mi.phys[0].hPhysicalMonitor, buf.data(), len))
          mi.caps = buf.data();
      }
    }
  }
  out->push_back(mi);
  return TRUE;
}

static void PrintCaps(const std::string &caps)
{
  if (caps.empty())
  {
    printf("      capabilities: <none returned>\n");
    return;
  }
  // Only the vcp() list is interesting, and it is one long line: pull it out.
  size_t at = caps.find("vcp(");
  if (at == std::string::npos)
  {
    printf("      capabilities: %s\n", caps.c_str());
    return;
  }
  size_t end = caps.find(')', at);
  std::string list = caps.substr(at, end == std::string::npos ? std::string::npos : end - at + 1);
  if (list.size() > 700)
    list = list.substr(0, 700) + " ...(truncated)";
  printf("      capabilities: %s\n", list.c_str());
}

// WHAT THE ADAPTERS SAY THEY ARE. This is here because DDC/CI failing ("VCP 0x10 NOT SUPPORTED") has two very
// different causes -- a monitor that does not implement it, and a display that is not a real monitor at all
// (an indirect/virtual display, a capture card, a KVM) -- and the DeviceID below tells them apart.
static void Adapters()
{
  printf("==== ADAPTERS AND MONITORS (EnumDisplayDevices) ====\n");
  DISPLAY_DEVICEA ad;
  memset(&ad, 0, sizeof(ad));
  ad.cb = sizeof(ad);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &ad, 0); ++i)
  {
    printf("  adapter %lu: %s   flags=0x%08lx%s\n", (unsigned long)i, ad.DeviceName,
           (unsigned long)ad.StateFlags,
           (ad.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? "  (primary)" : "");
    printf("      desc=\"%s\"  id=%s  key=%s\n", ad.DeviceString, ad.DeviceID, ad.DeviceKey);
    DISPLAY_DEVICEA mon;
    memset(&mon, 0, sizeof(mon));
    mon.cb = sizeof(mon);
    for (DWORD j = 0; EnumDisplayDevicesA(ad.DeviceName, j, &mon, 0); ++j)
      printf("      monitor %lu: \"%s\"  id=%s  flags=0x%08lx\n", (unsigned long)j, mon.DeviceString,
             mon.DeviceID, (unsigned long)mon.StateFlags);
    memset(&ad, 0, sizeof(ad));
    ad.cb = sizeof(ad);
  }
  printf("\n");
}

static void Monitors(){
  printf("==== MONITORS (GDI + DDC/CI) ====\n");
  std::vector<MonitorInfo> mons;
  EnumDisplayMonitors(nullptr, nullptr, EnumProc, reinterpret_cast<LPARAM>(&mons));
  printf("  %d monitor(s)\n", (int)mons.size());
  for (size_t i = 0; i < mons.size(); ++i)
  {
    MonitorInfo &mi = mons[i];
    printf("  [%d] %s%s  rect=%ld,%ld %ldx%ld\n", (int)i, mi.device.c_str(), mi.primary ? " (primary)" : "",
           mi.rc.left, mi.rc.top, mi.rc.right - mi.rc.left, mi.rc.bottom - mi.rc.top);
    printf("      physical monitors: %d  desc=\"%s\"\n", (int)mi.phys.size(), mi.physDesc.c_str());
    if (mi.hasBrightness)
      printf("      DDC VCP 0x10 brightness: %lu / %lu\n", (unsigned long)mi.brightCur,
             (unsigned long)mi.brightMax);
    else
      printf("      DDC VCP 0x10 brightness: NOT SUPPORTED\n");
    if (mi.hasPower)
      printf("      DDC VCP 0xD6 power mode: %lu (max %lu)   [1=on 2=standby 3=suspend 4=off]\n",
             (unsigned long)mi.powerCur, (unsigned long)mi.powerMax);
    else
      printf("      DDC VCP 0xD6 power mode: NOT SUPPORTED\n");
    PrintCaps(mi.caps);

    // GAMMA: can a ramp be written for THIS adapter? Written back unchanged, so the screen does not move.
    HDC dc = CreateDCA("DISPLAY", mi.device.c_str(), nullptr, nullptr);
    if (dc)
    {
      WORD ramp[3][256];
      if (GetDeviceGammaRamp(dc, ramp))
      {
        const BOOL ok = SetDeviceGammaRamp(dc, ramp);
        printf("      gamma ramp: readable, write-back %s\n", ok ? "ACCEPTED" : "REFUSED");
      }
      else
        printf("      gamma ramp: NOT readable\n");
      DeleteDC(dc);
    }
    else
      printf("      gamma ramp: could not open a DC for %s\n", mi.device.c_str());
  }
  // The physical handles are a resource; DDC/CI is a slow serial protocol and leaving them open is rude.
  for (MonitorInfo &mi : mons)
    if (!mi.phys.empty())
      DestroyPhysicalMonitors((DWORD)mi.phys.size(), mi.phys.data());
  printf("\n");
}

// ---------------------------------------------------------------------------------------------
// WMI -- the laptop-panel path (internal displays answer here and NOT over DDC/CI)
// ---------------------------------------------------------------------------------------------

static void Wmi()
{
  printf("==== WMI (root\\WMI) -- the internal-panel brightness path ====\n");
  IWbemLocator *loc = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void **>(&loc));
  if (FAILED(hr) || !loc)
  {
    printf("  WbemLocator unavailable (hr=0x%08lx)\n\n", (unsigned long)hr);
    return;
  }
  IWbemServices *svc = nullptr;
  BSTR ns = SysAllocString(L"ROOT\\WMI");
  hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
  SysFreeString(ns);
  if (FAILED(hr) || !svc)
  {
    printf("  could not connect to ROOT\\WMI (hr=0x%08lx)\n\n", (unsigned long)hr);
    loc->Release();
    return;
  }
  CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

  const wchar_t *queries[3] = {L"SELECT * FROM WmiMonitorBrightness",
                               L"SELECT * FROM WmiMonitorID",
                               L"SELECT * FROM WmiMonitorBrightnessMethods"};
  const char *names[3] = {"WmiMonitorBrightness", "WmiMonitorID", "WmiMonitorBrightnessMethods"};

  for (int q = 0; q < 3; ++q)
  {
    IEnumWbemClassObject *en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(queries[q]);
    hr = svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr) || !en)
    {
      printf("  %s: query failed (hr=0x%08lx)\n", names[q], (unsigned long)hr);
      continue;
    }
    int count = 0;
    for (;;)
    {
      IWbemClassObject *obj = nullptr;
      ULONG got = 0;
      if (en->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0 || !obj)
        break;
      ++count;
      VARIANT v;
      VariantInit(&v);
      if (obj->Get(L"InstanceName", 0, &v, nullptr, nullptr) == S_OK && v.vt == VT_BSTR)
      {
        char tmp[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, tmp, sizeof(tmp) - 1, nullptr, nullptr);
        printf("  %s instance: %s\n", names[q], tmp);
      }
      VariantClear(&v);
      VariantInit(&v);
      if (obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr) == S_OK)
      {
        printf("      CurrentBrightness = %ld\n", (long)v.lVal);
        VariantClear(&v);
      }
      VariantInit(&v);
      if (obj->Get(L"UserFriendlyName", 0, &v, nullptr, nullptr) == S_OK && (v.vt & VT_ARRAY))
      {
        char name[256] = {0};
        int n = 0;
        LONG lo = 0, hi = -1;
        if (SafeArrayGetLBound(v.parray, 1, &lo) == S_OK && SafeArrayGetUBound(v.parray, 1, &hi) == S_OK)
        {
          for (LONG k = lo; k <= hi && n < (int)sizeof(name) - 1; ++k)
          {
            LONG val = 0;
            if (SafeArrayGetElement(v.parray, &k, &val) == S_OK && val > 0 && val < 128)
              name[n++] = (char)val;
          }
        }
        printf("      UserFriendlyName = \"%s\"\n", name);
        VariantClear(&v);
      }
      obj->Release();
    }
    printf("  %s: %d instance(s)\n", names[q], count);
    en->Release();
  }
  svc->Release();
  loc->Release();
  printf("\n");
}

// ---------------------------------------------------------------------------------------------
// AUDIO -- WASAPI: the endpoint, and one row per application session
// ---------------------------------------------------------------------------------------------

static std::string ProcNameOf(DWORD pid)
{
  if (pid == 0)
    return "(system)";
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return "(no access)";
  char path[MAX_PATH] = {0};
  DWORD n = sizeof(path);
  std::string out = "(unknown)";
  if (QueryFullProcessImageNameA(h, 0, path, &n))
  {
    const char *slash = strrchr(path, '\\');
    out = slash ? slash + 1 : path;
  }
  CloseHandle(h);
  return out;
}

static void Audio()
{
  printf("==== AUDIO (WASAPI session mixer) ====\n");
  IMMDeviceEnumerator *en = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en));
  if (FAILED(hr) || !en)
  {
    printf("  no device enumerator (hr=0x%08lx)\n\n", (unsigned long)hr);
    return;
  }
  IMMDevice *dev = nullptr;
  hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  if (FAILED(hr) || !dev)
  {
    printf("  no default render endpoint (hr=0x%08lx)\n\n", (unsigned long)hr);
    en->Release();
    return;
  }
  IPropertyStore *props = nullptr;
  if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
  {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
    {
      char tmp[512] = {0};
      WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, tmp, sizeof(tmp) - 1, nullptr, nullptr);
      printf("  endpoint: %s\n", tmp);
    }
    PropVariantClear(&pv);
    props->Release();
  }
  IAudioEndpointVolume *vol = nullptr;
  if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&vol))) &&
      vol)
  {
    float scalar = 0;
    BOOL mute = FALSE;
    vol->GetMasterVolumeLevelScalar(&scalar);
    vol->GetMute(&mute);
    printf("  master volume: %.3f  mute=%d\n", scalar, (int)mute);
    vol->Release();
  }

  IAudioSessionManager2 *mgr = nullptr;
  if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&mgr))) &&
      mgr)
  {
    IAudioSessionEnumerator *sessions = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions)) && sessions)
    {
      int count = 0;
      sessions->GetCount(&count);
      printf("  %d session(s)\n", count);
      for (int i = 0; i < count; ++i)
      {
        IAudioSessionControl *ctrl = nullptr;
        if (FAILED(sessions->GetSession(i, &ctrl)) || !ctrl)
          continue;
        DWORD pid = 0;
        IAudioSessionControl2 *ctrl2 = nullptr;
        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void **>(&ctrl2))) &&
            ctrl2)
        {
          ctrl2->GetProcessId(&pid);
        }
        LPWSTR disp = nullptr;
        if (SUCCEEDED(ctrl->GetDisplayName(&disp)) && disp)
          CoTaskMemFree(disp);
        ISimpleAudioVolume *sv = nullptr;
        float v = 0;
        BOOL mute = FALSE;
        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&sv))) && sv)
        {
          sv->GetMasterVolume(&v);
          sv->GetMute(&mute);
        }
        printf("  [%2d] pid=%-6lu %-28s volume=%.3f mute=%d  %s\n", i, (unsigned long)pid,
               ProcNameOf(pid).c_str(), v, (int)mute, sv ? "" : "(NOT ADJUSTABLE -- no ISimpleAudioVolume)");
        if (sv)
          sv->Release();
        if (ctrl2)
          ctrl2->Release();
        ctrl->Release();
      }
      sessions->Release();
    }
    mgr->Release();
  }
  dev->Release();
  en->Release();
  printf("\n");
}

int main()
{
  SetConsoleOutputCP(CP_UTF8);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // ⚠️ WITHOUT THIS, ConnectServer CAN COME BACK "ACCESS DENIED" (0x80041003) EVEN FOR A READ. A process that
  // never calls CoInitializeSecurity has its COM security set by the OS to something no WMI namespace accepts,
  // and the failure looks exactly like "this machine does not have that class". The first run of this probe
  // reported WBEM_E_ACCESS_DENIED for all three classes because of this one missing call, which would have been
  // read as "the laptop-panel path is unavailable here".
  HRESULT sec = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                     RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
  printf("CoInitializeSecurity -> 0x%08lx\n\n", (unsigned long)sec);
  Adapters();
  Monitors();
  Wmi();
  Audio();
  CoUninitialize();
  printf("done.\n");
  return 0;
}
