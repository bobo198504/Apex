// ---------------------------------------------------------------------------
// regnotify_probe -- DOES A DEVICE APPEARING ACTUALLY WAKE A RegNotifyChangeKeyValue WATCH?
//
// ⚠️⚠️ THIS IS THE LOAD-BEARING ASSUMPTION OF THE WHOLE VolumeLock DESIGN, AND IT IS NOT DOCUMENTED ANYWHERE.
//
// The feature does not poll: it arms a change notification on
//   HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render
// and blocks on it, on the theory that plugging a USB audio device in writes that key. If that theory is
// wrong, the feature is a program that waits for ever -- and its symptom is "it never does anything", which
// looks exactly like "nothing was ever plugged in". That is the worst kind of wrong to discover in
// production, so it is measured here first.
//
// What this probe does:
//   1. arms the watch exactly as the feature does;
//   2. prints the endpoint list, so the device under test can be seen in it;
//   3. waits for the notification and prints whether it arrived, and how long it took;
//   4. reports whether the FLAGS it would use catch the change.
//
// ⚠️ IT WRITES NOTHING AND CHANGES NO DEVICE. The only action it asks for is one the USER performs: plugging
// or unplugging, or enabling/disabling a device in the sound control panel. The probe just watches.
//
// ⚠️ AND IT WATCHES WITH SEVERAL FLAG SETS IN TURN, because "the notification did not arrive" has two very
// different explanations -- the key is not written at all when a device appears, or the flag asked for does
// not cover that write -- and only separating them tells the reader which one they are looking at.
//
// BUILD:
//   _tools/w64devkit/bin/g++ -std=c++17 -O2 -mconsole _diag/regnotify_probe.cpp \
//       -o build/regnotify_probe.exe -lole32 -loleaut32 -luuid
//
// RUN:
//   build/regnotify_probe.exe [seconds-to-wait]
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <string>
#include <vector>

static const wchar_t *kRenderKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";

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

// The same description lookup the feature uses, so this probe prints what the feature would see.
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

static void ListEndpoints(const char *when)
{
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
    int active = 0;
    std::string platanes;
    for (UINT i = 0; i < count; ++i)
    {
      IMMDevice *dev = nullptr;
      if (FAILED(coll->Item(i, &dev)) || !dev)
        continue;
      DWORD state = 0;
      dev->GetState(&state);
      if (state == DEVICE_STATE_ACTIVE)
      {
        ++active;
        LPWSTR wid = nullptr;
        std::string id;
        if (SUCCEEDED(dev->GetId(&wid)) && wid)
        {
          id = Utf8(wid);
          CoTaskMemFree(wid);
        }
        const std::string d = RegDesc(id);
        if (d.find("PLATANE") != std::string::npos)
          platanes += "      ACTIVE: \"" + d + "\"\n";
      }
      dev->Release();
    }
    printf("  %s: %u endpoint(s), %d active\n", when, count, active);
    if (!platanes.empty())
      printf("%s", platanes.c_str());
    coll->Release();
  }
  en->Release();
}

// ⚠️ waitForSingleObject on the notification, with a real timeout, so "nothing happened" is a RESULT rather
// than a hang. The probe never blocks indefinitely: a probe that hangs tells the reader nothing.
static bool WatchOneFlag(DWORD flags, const char *name, DWORD timeoutMs)
{
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HKEY h = nullptr;
  if (!ev ||
      RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRenderKey, 0, KEY_NOTIFY, &h) != ERROR_SUCCESS)
  {
    printf("  %-34s could not open the key\n", name);
    if (ev)
      CloseHandle(ev);
    return false;
  }

  const LONG rc = RegNotifyChangeKeyValue(h, TRUE, flags, ev, TRUE);
  if (rc != ERROR_SUCCESS)
  {
    printf("  %-34s RegNotifyChangeKeyValue failed (error %ld)\n", name, rc);
    RegCloseKey(h);
    CloseHandle(ev);
    return false;
  }

  printf("  %-34s armed, waiting up to %lu ms ... ", name, (unsigned long)timeoutMs);
  fflush(stdout);
  const DWORD start = GetTickCount();
  const DWORD w = WaitForSingleObject(ev, timeoutMs);
  const DWORD took = GetTickCount() - start;

  bool fired = false;
  if (w == WAIT_OBJECT_0)
  {
    printf("FIRED after %lu ms\n", (unsigned long)took);
    fired = true;
  }
  else if (w == WAIT_TIMEOUT)
  {
    printf("no notification in %lu ms\n", (unsigned long)took);
  }
  else
  {
    printf("wait returned %lu\n", (unsigned long)w);
  }

  RegCloseKey(h);
  CloseHandle(ev);
  return fired;
}

int main(int argc, char **argv)
{
  const DWORD totalMs = (argc > 1) ? (DWORD)(atoi(argv[1]) * 1000) : 30000;

  printf("==== RegNotifyChangeKeyValue ON THE AUDIO RENDER KEY ====\n");
  printf("key  : HKLM\\%ls\n", kRenderKey);
  printf("this is the assumption VolumeLock's whole design rests on:\n");
  printf("that a device appearing WRITES this key, so the feature can sleep instead of polling.\n\n");

  printf("---- the machine right now ----\n");
  ListEndpoints("now");

  printf("\n---- the watch, with each flag set ----\n");
  printf("(%lu s each, in turn -- the two below are the ones the feature considered)\n\n",
         (unsigned long)(totalMs / 1000));

  printf("⚠️  ACTION REQUIRED: while each watch is armed, UNPLUG AND REPLUG the device\n");
  printf("    (or disable/enable it in the sound control panel). Each watch gets %lu s.\n\n",
        (unsigned long)(totalMs / 1000));

  // ⚠️ TWO FLAG SETS, AND WHY BOTH: `LAST_SET` is what the feature uses (it is what a device's state write
  // produces). `NAME` catches a value being written under the key. If LAST_SET does not fire but NAME does,
  // the fix is a different flag rather than "give up and poll" -- and only running both says which.
  const bool lastSet = WatchOneFlag(REG_NOTIFY_CHANGE_LAST_SET, "REG_NOTIFY_CHANGE_LAST_SET", totalMs);
  const bool nameOnly = WatchOneFlag(REG_NOTIFY_CHANGE_NAME, "REG_NOTIFY_CHANGE_NAME", totalMs);

  printf("\n---- after ----\n");
  ListEndpoints("now");

  printf("\n==== CONCLUSION ====\n");
  if (lastSet)
    printf("  REG_NOTIFY_CHANGE_LAST_SET FIRED -- the feature's design holds: it can sleep and be woken.\n");
  else if (nameOnly)
    printf("  ⚠️ LAST_SET did NOT fire but NAME did. The feature should ask for NAME as well -- as written it\n"
           "     would sleep through a device appearing. THIS IS A REAL FINDING, not a probe problem.\n");
  else
    printf("  ⚠️ NEITHER fired. Either no device was actually plugged/unplugged during the run, or this key is\n"
           "     not written when one appears. Repeat the run and be sure the action happened during a watch\n"
           "     before concluding anything -- a probe that measured nothing must not be read as a negative.\n");

  return 0;
}
