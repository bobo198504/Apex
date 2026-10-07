// WHAT DOES SMOOTHWHEEL **DO** WITH EACH DEVICE? -- checked against the BUILT DLL, not against the source.
//
// WHY THIS HALF EXISTS. _diag/app_device_probe.cpp proves the classifier (common/device.h) separates the three
// senders. That is not the same claim as "SmoothWheel passes a touchpad through": the feature has to ASK the
// classifier, on every message, in the right order, and DECLINE before any of its other rules can take the
// wheel. The two can disagree, and the failure would be silent -- a touchpad smoothed as if it were a mouse.
//
// WHAT IT RUNS: the real exported entry point of the real DLL, exactly as the host loads it (ABI version and
// struct size checked first), then `onWheel` with events built by hand. The verdict IS the return value:
//
//   1 = the feature TAKES the wheel (the host will swallow it and deliver its own smoothed stream)
//   0 = the feature leaves it ALONE (the message reaches the receiving program untouched)
//
// THE ORDER OF THE CASES IS PART OF THE TEST. The classifier is a state machine over the delta stream: a
// whole notch clears the evidence, a gesture that has varied is locked to touchpad, and a touch-tagged message
// marks the tracker. So the cases below are ordered the way gestures really arrive, and the comments say which
// state each one starts from.
//
// Build: g++ -std=c++17 -O2 -I apex -o build/_feature_device_probe.exe _diag/feature_device_probe.cpp
// Run:   build/_feature_device_probe.exe <SmoothWheel.dll> [scratch-dir]
//
// ⚠️ THE SCRATCH FOLDER IS A COMMAND-LINE ARGUMENT and it must point under build/: a feature's `init` is
// allowed to read (and create) its own settings file, and that file must never land in a deployed installation
// or next to the user's own settings.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "abi.h"

typedef const ApexFeature *(*EntryFn)(void);

static int failures = 0;
static void Check(const char *what, int got, int want)
{
  const bool ok = (got == want);
  if (!ok)
    ++failures;
  printf("  %-62s -> %d (want %d) %s\n", what, got, want, ok ? "ok" : "WRONG");
}

// ---- the host stub the DLL needs ---------------------------------------------------------------
static char g_dir[512] = {0};

static int HostFeatureDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  const int n = snprintf(out, (size_t)outSize, "%s", g_dir);
  return n > 0 ? n : 0;
}
static void HostLogLine(const char *text) { printf("      [feature] %s\n", text); }
static int HostFeatureEnabled(const char *) { return 1; }
// A plain, unlisted program with no wheel handler of its own -- i.e. the case where Apex is allowed to smooth
// at all. The exclude list is empty (the scratch folder has no settings file), so the name does not matter.
static int HostTargetAt(int, int, ApexTarget *out)
{
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->pid = 1;
  snprintf(out->exe, sizeof(out->exe), "explorer.exe");
  out->handlerState = APEX_HANDLER_ABSENT;
  return 1;
}
static void HostActivity(void) {}

static const ApexFeature *g_feature = nullptr;

// One wheel message, exactly as the host would hand it over.
static int Wheel(int delta, unsigned key = 0, unsigned long long extra = 0)
{
  ApexWheelEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.delta = delta;
  ev.x = 100;
  ev.y = 100;
  ev.key = key;
  ev.extraInfo = extra;
  return g_feature->onWheel(&ev);
}

int main(int argc, char **argv)
{
  const char *dll = (argc > 1) ? argv[1] : "build/apex/Plugins/SmoothWheel/SmoothWheel.dll";
  snprintf(g_dir, sizeof(g_dir), "%s", (argc > 2) ? argv[2] : "build/_device_scratch");

  HMODULE mod = LoadLibraryA(dll);
  if (!mod)
  {
    printf("cannot load %s (err %lu)\n", dll, GetLastError());
    return 2;
  }
  EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  if (!entry)
  {
    printf("no ApexFeatureEntry in %s\n", dll);
    return 2;
  }
  const ApexFeature *f = entry();
  printf("what %s does with each device (ABI %u)\n", f->id, f->abiVersion);
  if (f->abiVersion != APEX_ABI_VERSION || f->structSize < sizeof(ApexFeature))
  {
    printf("  FAIL: the DLL does not match this host's ABI (%u, and this host speaks %u)\n",
           f->abiVersion, APEX_ABI_VERSION);
    return 1;
  }
  if (!f->onWheel)
  {
    printf("  FAIL: this feature has no onWheel at all\n");
    return 1;
  }

  ApexHost host;
  memset(&host, 0, sizeof(host));
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLogLine;
  host.featureEnabled = HostFeatureEnabled;
  host.targetAt = HostTargetAt;
  host.activity = HostActivity;
  if (f->init)
  {
    const int rc = f->init(&host);
    if (rc != 0)
    {
      printf("  FAIL: the feature refused to start (init returned %d)\n", rc);
      FreeLibrary(mod);
      return 1;
    }
  }
  g_feature = f;

  printf("\n== a notched mouse: taken, and the modifiers keep their own meanings ==\n");
  Check("a whole notch, no modifiers: TAKEN", Wheel(120), 1);
  Check("Ctrl+wheel is not ours: left alone", Wheel(120, 2), 0);
  Check("a zero delta is not a wheel: left alone", Wheel(0), 0);

  printf("\n== a free-spinning wheel: unknown for two messages, then taken ==\n");
  // The classification needs kDeviceMinSamples sub-notch values before it will name a step, and an
  // unidentified wheel must NOT be animated -- that is the half that used to leak a touchpad into the model.
  Check("1st constant-15 message: still unidentified, left alone", Wheel(15), 0);
  Check("2nd: still unidentified, left alone", Wheel(15), 0);
  for (int i = 3; i <= 6; ++i)
  {
    char what[96];
    snprintf(what, sizeof(what), "message %d: a fixed step is identified -> TAKEN", i);
    Check(what, Wheel(15), 1);
  }

  printf("\n== a touchpad WITHOUT the OS marker: the delta pattern has to catch it ==\n");
  Check("a whole notch first (a fresh gesture)", Wheel(120), 1);
  Check("1st irregular value: unidentified, left alone", Wheel(7), 0);
  Check("2nd irregular value: still unidentified, left alone", Wheel(22), 0);
  Check("3rd irregular value: a finger, not a step -> left alone", Wheel(3), 0);
  Check("and it STAYS left alone", Wheel(11), 0);
  Check("... through the rest of the gesture", Wheel(8), 0);
  Check("a whole notch again: a mouse is back -> TAKEN", Wheel(120), 1);

  printf("\n== the variation lock: a creeping touchpad must not turn into a 'wheel' ==\n");
  Check("a varying opening (30/28/26) -> left alone", Wheel(30), 0);
  Check("  ...", Wheel(28), 0);
  Check("  ...", Wheel(26), 0);
  // Eight identical small values in a row WOULD read as one fixed step; the lock is what stops it.
  for (int i = 0; i < 8; ++i)
    Check("a steady run after varying is STILL left alone", Wheel(12), 0);
  Check("a whole notch clears the gesture -> TAKEN", Wheel(120), 1);

  printf("\n== the OS touch/pen marker: it wins over the numbers ==\n");
  Check("a small touch-tagged value -> left alone", Wheel(7, 0, 0xFF515701ull), 0);
  // A touch-tagged WHOLE NOTCH is the case a numeric rule alone would get wrong: 120 looks like a mouse, and
  // the OS says otherwise. The low byte is the contact count, so it varies and is masked off.
  Check("a touch-tagged WHOLE NOTCH -> left alone", Wheel(120, 0, 0xFF515705ull), 0);
  // The marker belongs to the message it arrived on, so an untagged notch right after it is a mouse again.
  Check("an untagged whole notch after it -> TAKEN", Wheel(120), 1);

  printf("\n%s\n", failures ? "FAIL: the feature does not leave the devices it must leave alone"
                           : "OK: a mouse and a free-spinner are taken; a touchpad is left entirely alone");
  FreeLibrary(mod);
  return failures ? 1 : 0;
}
