// ---------------------------------------------------------------------------
// up2_probe -- WHAT IS A "PLATANE UP2 SE" ENDPOINT, AND WHICH "VOLUME" CAN BE SET ON IT?
//
// WHY THIS FILE EXISTS: the user wants "the moment PLATANE UP2 SE appears, force ITS device volume to 50%".
// Two things had to be settled by measurement before any code was written, because both are facts about the
// DEVICE and the way Windows enumerated it -- not about the code:
//
//   1. WHICH ENDPOINTS COUNT AS "the device". Its friendly name is a generic "扬声器 / Speakers"; the string
//      that actually says PLATANE UP2 SE lives in a DIFFERENT property. And the endpoint id embeds the
//      PHYSICAL USB PORT (`...&MI_00\9&<serial>&0&0000`), so Windows makes a SEPARATE endpoint per port --
//      which is exactly why "only the instance that just appeared" is the rule that was asked for.
//
//   2. WHICH "VOLUME" IS THE ONE IN THE WINDOWS VOLUME MIXER. WASAPI's endpoint master volume
//      (IAudioEndpointVolume) is the slider; the box's own knob is a hardware gain that WASAPI does not
//      touch. This probe reports whether a hardware volume node is even reachable (IKsControl / KSPROPERTY
//      on the endpoint's topology), so "can we do B" is answered rather than guessed.
//
// ⚠️⚠️ THIS PROGRAM WRITES NOTHING. It never calls SetMasterVolumeLevel / SetMasterVolumeLevelScalar /
// SetMute, and never opens a device for writing. It is safe to run while the user is working.
//
// ⚠️ IT ALSO LISTS THE ENDPOINTS IT IS *NOT* GOING TO TOUCH. The user's other devices are explicitly out of
// scope ("其它设备不动"), so printing them makes it checkable that the match rule selects 5 rows and no more.
//
// WHAT IT PRINTS:
//   * every RENDER endpoint: its IMMDevice id, its state (ACTIVE / DISABLED / NOTPRESENT / UNPLUGGED),
//     its friendly name, its device description (this is where "PLATANE UP2 SE" lives), its master volume,
//     mute state, channel count, and whether it is the current default;
//   * which rows the match rule WOULD select, marked;
//   * for the matching rows: whether a hardware volume node is reachable, and the endpoint's own form factor.
//
// BUILD (from the repo root, with the shared toolchain):
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/up2_probe.cpp -o build/up2_probe.exe \
//       -lole32 -loleaut32 -luuid -ladvapi32
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

// ⚠️⚠️ KSPROPSETID_Audio IS WRITTEN OUT BY HAND HERE, AND THE VALUE WAS READ OUT OF `<ksmedia.h>` RATHER
// THAN REMEMBERED. This MinGW declares it through `DEFINE_GUIDSTRUCT`/`DEFINE_GUIDNAMED`, which resolves to
// an EXTERN GUID with no definition in any import library -- so the link fails with
// `undefined reference to 'KSPROPSETID_Audio'`, which reads like a missing library rather than a missing
// definition. (A first attempt at this constant was typed from memory and was WRONG; the header's own
// STATIC_KSPROPSETID_Audio line is the source, and it is quoted below.)
//   ksmedia.h:1037  #define STATIC_KSPROPSETID_Audio 0x45FFAAA0,0x6E1B,0x11D0,0xBC,0xF2,0x44,0x45,0x53,0x54,0x00,0x00
static const GUID kKsPropsetidAudio = {
    0x45FFAAA0, 0x6E1B, 0x11D0, {0xBC, 0xF2, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The pattern and the level are the two things the feature will later make configurable. They are constants
// here because this probe answers "does this rule select the right rows", not "what should the default be".
static const char *kPattern = "PLATANE UP2 SE";
static const float kLevel = 0.50f;

// ---------------------------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------------------------

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

static std::string Lower(const std::string &s)
{
  std::string o = s;
  for (char &c : o)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return o;
}

// Substring, not the anchored PatternMatch: this is a throwaway probe reading a console argument, and the
// feature itself will use common/match.h. Being lenient HERE is deliberate -- a probe that under-reports
// would hide the rows under test.
static bool Contains(const std::string &hay, const std::string &needle)
{
  return Lower(hay).find(Lower(needle)) != std::string::npos;
}

// This is the property that holds "PLATANE UP2 SE". PKEY_Device_FriendlyName holds the generic
// "扬声器 / Speakers", and that difference is the single most important finding this probe records.
static const PROPERTYKEY kDeviceDesc = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};

static std::string PropStr(IPropertyStore *props, const PROPERTYKEY &key)
{
  PROPVARIANT pv;
  PropVariantInit(&pv);
  std::string out;
  if (props && SUCCEEDED(props->GetValue(key, &pv)))
  {
    if (pv.vt == VT_LPWSTR && pv.pwszVal)
      out = Utf8(pv.pwszVal);
    else if (pv.vt == VT_BSTR && pv.bstrVal)
      out = Utf8(pv.bstrVal);
  }
  PropVariantClear(&pv);
  return out;
}

// ---------------------------------------------------------------------------------------------
// ⚠️ THE ENDPOINT'S "DEVICE DESCRIPTION" IS THE WHOLE POINT OF THIS FEATURE, AND IT IS ALSO THE ONE THING
// THE PROPERTY STORE DOES NOT RELIABLY GIVE BACK.
//
// Measured on this machine: PKEY_Device_FriendlyName returns the generic "扬声器 / Speakers" for every
// PLATANE row, and the description that actually reads "PLATANE UP2 SE" is not exposed through the store at
// all -- it was found in the registry under the endpoint's own IMMDevice id. So the registry is the SOURCE
// for this string, and the property store is only the fallback. The reverse order silently returns "nothing
// matched" on this machine, which is the same answer as "the device is not plugged in".
// ---------------------------------------------------------------------------------------------

static std::string RegEndpointDesc(const std::string &endpointId)
{
  // The IMMDevice id looks like  {0.0.0.00000000}.{guid}  and the registry keeps one key per endpoint that
  // is NAMED by the same guid (braces included).
  if (endpointId.empty())
    return "";
  const size_t dot = endpointId.rfind('.');
  const std::string guid = (dot == std::string::npos) ? endpointId : endpointId.substr(dot + 1);

  std::string key = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guid +
                    "\\Properties";
  wchar_t wkey[512] = {0};
  MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, wkey, 511);

  HKEY h = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wkey, 0, KEY_READ, &h) != ERROR_SUCCESS)
    return "";

  static const wchar_t *kKeys[] = {
      L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", // the one that says "PLATANE UP2 SE"
      L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", // friendly name, the generic fallback
  };
  std::string out;
  for (const wchar_t *name : kKeys)
  {
    wchar_t buf[512] = {0};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && cb > 2)
    {
      out = Utf8(buf);
      break;
    }
  }
  RegCloseKey(h);
  return out;
}

static std::string StateName(DWORD st)
{
  switch (st)
  {
  case DEVICE_STATE_ACTIVE:
    return "ACTIVE";
  case DEVICE_STATE_DISABLED:
    return "DISABLED";
  case DEVICE_STATE_NOTPRESENT:
    return "NOTPRESENT";
  case DEVICE_STATE_UNPLUGGED:
    return "UNPLUGGED";
  default:
    break;
  }
  char buf[32];
  sprintf(buf, "0x%lx", static_cast<unsigned long>(st));
  return buf;
}

// ---------------------------------------------------------------------------------------------
// THE HARDWARE-VOLUME QUESTION (the "A or B" fork, answered rather than assumed)
//
// ⚠️ THIS OPENS NOTHING FOR WRITE. It walks the endpoint's topology looking for a volume NODE and, if it
// finds one, READS its KSPROPERTY_AUDIO_VOLUMELEVEL range. A device that answers here does have a hardware
// gain; whether the user's UP2 SE knob is wired to it is a separate question, and one that only a real
// write would settle -- so this probe reports the READING, and says plainly that it did not write.
// ---------------------------------------------------------------------------------------------

struct HwVolume
{
  bool topologyOpened = false;
  bool volumeNodeFound = false;
  bool rangeRead = false;
  LONG min = 0, max = 0, step = 0, current = 0;
  bool querySucceeded = false;
  std::string note;
};

static HwVolume ProbeHardwareVolume(IMMDevice *dev)
{
  HwVolume hw;
  IDeviceTopology *topo = nullptr;
  if (FAILED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void **>(&topo))) ||
      !topo)
  {
    hw.note = "IDeviceTopology unavailable";
    return hw;
  }
  hw.topologyOpened = true;

  IConnector *conn = nullptr;
  if (FAILED(topo->GetConnector(0, &conn)) || !conn)
  {
    hw.note = "no connector 0";
    topo->Release();
    return hw;
  }

  IConnector *connected = nullptr;
  if (FAILED(conn->GetConnectedTo(&connected)) || !connected)
  {
    hw.note = "connector 0 has nothing connected";
    conn->Release();
    topo->Release();
    return hw;
  }

  IPart *part = nullptr;
  if (FAILED(connected->QueryInterface(__uuidof(IPart), reinterpret_cast<void **>(&part))) || !part)
  {
    hw.note = "connected device has no IPart";
    connected->Release();
    conn->Release();
    topo->Release();
    return hw;
  }

  // ⚠️ THE SUBUNIT IS NOT ALWAYS 0, AND ASKING A DEVICE FOR A SUBUNIT IT DOES NOT HAVE RETURNS E_INVALIDARG
  // -- which looks exactly like "this device has no hardware volume". So the sub-units are ENUMERATED and
  // each one is asked whether it controls the channels of the volume node. This is the difference between a
  // real answer and a plausible one, and it is the only reason this walk is longer than three lines.
  IDeviceTopology *vtopo = nullptr;
  if (SUCCEEDED(part->Activate(CLSCTX_ALL, __uuidof(IDeviceTopology), reinterpret_cast<void **>(&vtopo))) &&
      vtopo)
  {
    UINT subs = 0;
    vtopo->GetSubunitCount(&subs);
    for (UINT s = 0; s < subs && !hw.volumeNodeFound; ++s)
    {
      ISubUnit *sub = nullptr;
      if (FAILED(vtopo->GetSubunit(s, &sub)) || !sub)
        continue;

      // ⚠️ ISubUnit IS AN EMPTY INTERFACE (`struct ISubUnit : public IUnknown {}`) -- it declares no members
      // at all. So both the volume node and its control interface are asked of IPart, which the sub-unit
      // forwards to, and QueryInterface on the empty ISubUnit is what gets there.
      IPart *subPart = nullptr;
      if (FAILED(sub->QueryInterface(__uuidof(IPart), reinterpret_cast<void **>(&subPart))) || !subPart)
      {
        sub->Release();
        continue;
      }

      IAudioVolumeLevel *vol = nullptr;
      if (SUCCEEDED(subPart->Activate(CLSCTX_ALL, __uuidof(IAudioVolumeLevel),
                                      reinterpret_cast<void **>(&vol))) &&
          vol)
      {
        hw.volumeNodeFound = true;

        IControlInterface *ci = nullptr;
        UINT nci = 0;
        subPart->GetControlInterfaceCount(&nci);
        if (nci > 0)
          subPart->GetControlInterface(0, &ci);
        if (ci)
        {
          IKsControl *ks = nullptr;
          if (SUCCEEDED(ci->QueryInterface(__uuidof(IKsControl), reinterpret_cast<void **>(&ks))) && ks)
          {
            KSPROPERTY prop = {};
            prop.Set = kKsPropsetidAudio;
            prop.Id = KSPROPERTY_AUDIO_VOLUMELEVEL;
            prop.Flags = KSPROPERTY_TYPE_BASICSUPPORT;

            // The reply is a KSPROPERTY_DESCRIPTION followed by a variable-length members list. A fixed
            // oversized buffer is used and only the part that actually came back is decoded -- the tail's
            // layout differs between drivers, and decoding a guess would be worse than not decoding.
            BYTE buf[512] = {0};
            ULONG bytes = 0;
            HRESULT hr = ks->KsProperty(&prop, sizeof(prop), buf, sizeof(buf), &bytes);
            if (SUCCEEDED(hr) && bytes >= sizeof(KSPROPERTY_DESCRIPTION))
            {
              hw.rangeRead = true;
              KSPROPERTY_DESCRIPTION *d = reinterpret_cast<KSPROPERTY_DESCRIPTION *>(buf);
              hw.note = "hardware volume node present; BASICSUPPORT answered (a real gain node)";
              if (d->MembersListCount > 0 &&
                  bytes >= sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER))
              {
                hw.note = "hardware volume node present; range advertised by the driver";
              }
            }
            else
            {
              hw.note = "volume node present but BASICSUPPORT refused";
            }
            ks->Release();
          }
          ci->Release();
        }
        vol->Release();
      }
      subPart->Release();
      sub->Release();
    }
    vtopo->Release();
  }

  if (!hw.volumeNodeFound && hw.note.empty())
    hw.note = "no IAudioVolumeLevel node on this endpoint's path";

  part->Release();
  connected->Release();
  conn->Release();
  topo->Release();
  return hw;
}

// ---------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
  const char *pattern = (argc > 1) ? argv[1] : kPattern;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr))
  {
    printf("CoInitializeEx failed: 0x%08lx\n", static_cast<unsigned long>(hr));
    return 2;
  }

  printf("==== PLATANE UP2 SE PROBE (READ ONLY -- NOTHING IS WRITTEN) ====\n");
  printf("match pattern : \"%s\"  (substring, case-insensitive, for this probe only)\n", pattern);
  printf("target level  : %.0f%%  (reported, not applied)\n\n", kLevel * 100.0f);

  IMMDeviceEnumerator *en = nullptr;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                        reinterpret_cast<void **>(&en));
  if (FAILED(hr) || !en)
  {
    printf("no MMDeviceEnumerator: 0x%08lx\n", static_cast<unsigned long>(hr));
    CoUninitialize();
    return 2;
  }

  IMMDevice *defDev = nullptr;
  std::string defId;
  if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &defDev)) && defDev)
  {
    LPWSTR id = nullptr;
    if (SUCCEEDED(defDev->GetId(&id)) && id)
    {
      defId = Utf8(id);
      CoTaskMemFree(id);
    }
    defDev->Release();
  }
  printf("current default render endpoint:\n  %s\n\n", defId.empty() ? "(none)" : defId.c_str());

  IMMDeviceCollection *coll = nullptr;
  // ⚠️ THERE IS NO `DEVICE_STATE_ALL` CONSTANT. The four states are combined by hand, and this is NOT the
  // same as passing ACTIVE: the endpoints that matter to this feature are the ones that were just plugged
  // in or just enabled, and those are exactly the rows an ACTIVE-only listing would hide.
  const DWORD kAllStates =
      DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED;
  hr = en->EnumAudioEndpoints(eRender, kAllStates, &coll);
  if (FAILED(hr) || !coll)
  {
    printf("EnumAudioEndpoints failed: 0x%08lx\n", static_cast<unsigned long>(hr));
    en->Release();
    CoUninitialize();
    return 2;
  }

  UINT count = 0;
  coll->GetCount(&count);
  printf("==== %u RENDER ENDPOINT(S) ====\n\n", count);

  int matched = 0;
  std::vector<std::string> matchedIds;

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

    IPropertyStore *props = nullptr;
    std::string friendly, descx;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props)
    {
      friendly = PropStr(props, PKEY_Device_FriendlyName);
      props->Release();
    }
    // See RegEndpointDesc: on this machine the store does not carry the "PLATANE UP2 SE" string at all.
    descx = RegEndpointDesc(id);
    if (descx.empty())
      descx = friendly;

    // The match is against the DEVICE DESCRIPTION, because that is where "PLATANE UP2 SE" is. The friendly
    // name of every one of these rows is a generic "扬声器", so matching on it would select nothing.
    const bool hit = Contains(descx, pattern) || Contains(friendly, pattern);
    if (hit)
      ++matched;

    printf("%s [%u] state=%s\n", hit ? ">>" : "  ", i, StateName(state).c_str());
    printf("     id        : %s\n", id.c_str());
    printf("     friendly  : %s\n", friendly.empty() ? "(none)" : friendly.c_str());
    printf("     devdesc   : %s\n", descx.empty() ? "(none)" : descx.c_str());
    printf("     default   : %s\n", (!id.empty() && id == defId) ? "YES" : "no");

    IAudioEndpointVolume *vol = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(&vol))) &&
        vol)
    {
      float scalar = -1.0f;
      BOOL mute = FALSE;
      UINT channels = 0;
      vol->GetMasterVolumeLevelScalar(&scalar);
      vol->GetMute(&mute);
      vol->GetChannelCount(&channels);
      float db = -999.0f;
      vol->GetMasterVolumeLevel(&db);
      printf("     WASAPI    : master=%.4f (%.1f%%)  dB=%.2f  mute=%d  channels=%u\n", scalar,
             scalar * 100.0f, db, static_cast<int>(mute), channels);
      vol->Release();
    }
    else
    {
      printf("     WASAPI    : IAudioEndpointVolume NOT available (this is the row that gets skipped)\n");
    }

    if (hit)
    {
      HwVolume hw = ProbeHardwareVolume(dev);
      printf("     HW vol    : topology=%s node=%s range=%s\n", hw.topologyOpened ? "yes" : "no",
             hw.volumeNodeFound ? "FOUND" : "none", hw.rangeRead ? "read" : "not read");
      if (!hw.note.empty())
        printf("                 note: %s\n", hw.note.c_str());
      if (!id.empty())
        matchedIds.push_back(id);
    }

    printf("\n");
    dev->Release();
  }

  printf("==== MATCH RULE RESULT ====\n");
  printf("  rows matching \"%s\": %d\n", pattern, matched);
  for (const std::string &s : matchedIds)
    printf("    %s\n", s.c_str());
  printf("\n");
  printf("  ⚠️ Nothing was written. To answer \"which volume is the mixer slider\", compare the WASAPI\n");
  printf("     master value above against the slider drawn in the Windows volume mixer for this device.\n");
  printf("     To answer \"is there a second, hardware volume\", re-run while turning the box's own knob:\n");
  printf("     a hardware gain that is wired to the node above would move 'HW vol', not 'WASAPI'.\n");

  coll->Release();
  en->Release();
  CoUninitialize();
  return 0;
}
