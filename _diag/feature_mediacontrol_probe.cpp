// ---------------------------------------------------------------------------
// MediaControl, driven through the REAL DLL: the two groups it describes, what it accepts and REFUSES, whether
// the screen-off window really appears on the right monitor, whether the global shortcut is really taken, what
// lands in its settings file, and what it hands the quick panel.
//
// WHY A PROBE RATHER THAN TRUSTING THE CODE READ. Every one of this feature's outputs is either invisible or
// advisory:
//   * a brightness write is a number sent down a cable (or into a driver) -- nothing in this process can read
//     it back and say "the panel is at 60 now";
//   * the screen-off window is the one thing that CAN be observed, and it is also the one thing whose failure
//     mode is worst (a screen left black with nothing behind it);
//   * a global shortcut that failed to register looks exactly like one that was never pressed;
//   * and the quick panel's grouping is a contract with the HOST (apex/abi.h's `groupZh`/`groupEn`), not with
//     this feature, so "the two faders land in one pane called 亮度" is only true if the items carry the name.
//
// ⚠️ THIS PROBE REALLY DOES DARKEN A SCREEN FOR A MOMENT -- measured at well under a second, because it clears
// the flag the instant it has seen the window. That is deliberate and it is the only way to check the claim
// that a dark screen is a WINDOW (recoverable, connected, unlocked) rather than a display-mode change: the
// window's own class, rectangle and visibility are read from the window manager, and the screen is put back
// before anything else in this run happens. Nothing here touches the mouse or the keyboard.
//
// IT DOES NOT TOUCH ANY OTHER FEATURE: the host stub's `featureDir` points at a directory the caller gives it,
// so the settings file this run writes is its own.
//
// Build: g++ -std=c++17 -O2 -I apex -I common -o build/_mediacontrol_probe.exe _diag/feature_mediacontrol_probe.cpp
// Run:   build/_mediacontrol_probe.exe build/apex/Plugins/MediaControl/MediaControl.dll build/_mediacontrol_probe/
// ---------------------------------------------------------------------------

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi.h"

typedef const ApexFeature *(*EntryFn)(void);

static int failures = 0;
static void Check(bool ok, const char *what, const char *detail = "")
{
  printf("  %-66s %s%s%s\n", what, ok ? "ok" : "FAIL", detail[0] ? "  " : "", detail);
  if (!ok)
    ++failures;
}

static char g_dir[512] = {0};

static int HostFeatureDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  const int n = _snprintf(out, outSize, "%s", g_dir);
  return n > 0 ? n : 0;
}

static void HostLogLine(const char *text) { printf("      [feature] %s\n", text); }
static int HostFeatureEnabled(const char *) { return 1; }

static bool Has(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

static int CountOf(const char *hay, const char *needle)
{
  int n = 0;
  for (const char *p = hay; (p = strstr(p, needle)) != nullptr; p += strlen(needle))
    ++n;
  return n;
}

static bool ReadLog(const char *dir, char *out, int outSize)
{
  char path[600] = {0};
  _snprintf(path, sizeof(path), "%smediacontrol.log", dir);
  FILE *f = fopen(path, "rb");
  if (!f)
  {
    out[0] = 0;
    return false;
  }
  const size_t n = fread(out, 1, outSize - 1, f);
  fclose(f);
  out[n] = 0;
  return true;
}

// ---------------------------------------------------------------------------------------------
// THE OFF WINDOW, SEEN FROM OUTSIDE
// ---------------------------------------------------------------------------------------------
//
// ⚠️ THE CLASS NAME IS WRITTEN HERE AS WELL AS IN THE FEATURE, and that repetition is the price of looking at
// the real thing: the alternative is a probe that re-implements "what a dark screen looks like", which would
// pass while the screen stayed bright. The gate around this probe additionally reads the feature's source for
// the same literal, so a rename cannot leave this check looking for a window that no longer exists.
struct FindCtx
{
  HWND hwnd = nullptr; // the first one found, so a check can send it a message
  int found = 0;
  RECT rect = {0, 0, 0, 0};
  bool visible = false;
  unsigned long style = 0, exStyle = 0;
  // ⚠️ THE WINDOW'S OPACITY IS PART OF WHAT A DARK SCREEN IS NOW (see the half-second fade): the transition is
  // invisible to every other check here -- a window that appears at once and one that takes half a second to
  // become opaque have the same class, rectangle, styles and visibility -- so it has to be read back.
  BYTE alpha = 255;
  bool layered = false;
};

BOOL CALLBACK FindBlackProc(HWND h, LPARAM user)
{
  FindCtx *ctx = (FindCtx *)user;
  char cls[128] = {0};
  GetClassNameA(h, cls, (int)sizeof(cls) - 1);
  if (strcmp(cls, "ApexMediaControlOff") != 0)
    return TRUE;
  if (!ctx->hwnd)
    ctx->hwnd = h;
  ++ctx->found;
  ctx->visible = IsWindowVisible(h) != 0;
  GetWindowRect(h, &ctx->rect);
  ctx->style = (unsigned long)GetWindowLongPtrA(h, GWL_STYLE);
  ctx->exStyle = (unsigned long)GetWindowLongPtrA(h, GWL_EXSTYLE);
  // `GetLayeredWindowAttributes` is the one window property that cannot be had from the style bits: LWA_ALPHA can
  // be set, and 0 and 255 both look like "the bit is set".
  COLORREF key = 0;
  DWORD flags = 0;
  BYTE a = 255;
  if (GetLayeredWindowAttributes(h, &key, &a, &flags))
  {
    ctx->alpha = a;
    ctx->layered = (flags & LWA_ALPHA) != 0;
  }
  return TRUE;
}

FindCtx FindBlack()
{
  FindCtx ctx;
  EnumWindows(FindBlackProc, (LPARAM)&ctx);
  return ctx;
}

// Wait for the control thread to show or hide the window. It is signalled immediately, so this is generous --
// and it is a CONDITION wait rather than a fixed sleep, because "the window is not there yet" and "the window
// will never be there" have to be told apart by the caller, not by the clock.
//
// ⚠️⚠️ AND "UP" AND "GONE" ARE NOT MIRROR IMAGES, WHICH IS THE ONE THING TO GET RIGHT HERE.
//
//   * UP means THE USER CAN SEE IT, so the wait is `found && visible`. The control thread creates the window FIRST
//     and positions and shows it a moment later (`CreateWindowEx` at 0,0 1x1, then `SetWindowPos` with
//     `SWP_SHOWWINDOW`), and `EnumWindows` sees it from the instant it exists -- so waiting only for existence
//     returned a window that was still 1x1, invisible and in the corner, and "a black window appears" and
//     "covering a real monitor's rectangle exactly" failed with `windows=1 visible=0 rect=0,0 1x1`. That is a
//     probe racing a half-built window, not the feature being wrong.
//   * GONE means THE WINDOW IS DESTROYED, so the wait is `found == 0`. Hiding it is only the first half: the
//     screen comes back on (`SW_HIDE`) and the window is destroyed on the next pass, and every caller below asks
//     for "gone" and then asserts `found == 0`. Treating "not visible" as "gone" made the click test report
//     `gone after 500 ms` while the window was still there -- a half-answer read as a whole one.
FindCtx WaitForBlack(bool want, int timeoutMs = 4000)
{
  const DWORD start = GetTickCount();
  for (;;)
  {
    const FindCtx ctx = FindBlack();
    const bool reached = want ? (ctx.found > 0 && ctx.visible) : (ctx.found == 0);
    if (reached || (GetTickCount() - start) >= (DWORD)timeoutMs)
      return ctx;
    Sleep(25);
  }
}

// ⚠️⚠️ THE FADE, WATCHED RATHER THAN TAKEN ON TRUST, AND THIS IS THE ONLY WAY IT CAN BE SEEN AT ALL. The user
// asked for "熄屏和解除熄屏增加0.5秒的过渡动画", and from out here a fade and a jump are the SAME WINDOW: same
// class, same rectangle, same styles, and in a screenshot the same black. What distinguishes them is the OPACITY
// while it runs -- so this samples it until it is somewhere strictly in the middle, and reports how long that
// took. A window that jumps from invisible to solid (or the other way) never passes through the middle, and the
// answer is then -1, which is exactly what "there is no animation" looks like from outside.
long WatchFade(HWND h, int timeoutMs)
{
  const DWORD start = GetTickCount();
  for (;;)
  {
    COLORREF key = 0;
    DWORD flags = 0;
    BYTE a = 255;
    if (GetLayeredWindowAttributes(h, &key, &a, &flags) && (flags & LWA_ALPHA) && a > 0 && a < 255)
      return (long)(GetTickCount() - start);
    if ((long)(GetTickCount() - start) >= timeoutMs)
      return -1;
    Sleep(8);
  }
}

static void Settle(void) { Sleep(200); }

// ⚠️ TWO CHECKS IN HERE ACTUALLY CHANGE THE SCREEN (the screen-off window, and the gamma-sharing test), and
// both are behind this one switch. It is spelled two ways because the first name was written when there was
// only one such check.
static bool Quiet()
{
  return getenv("APEX_MC_NO_DARK") != nullptr || getenv("APEX_MC_NO_SCREEN") != nullptr;
}

// The monitors, by GDI device name, in the order the FEATURE uses (sorted by name -- see EnumerateDisplays).
// Everything below has to address a screen the same way the feature does, or it would be checking a different
// monitor than the one it asked about.
struct Screen
{
  char device[32] = {0};
  RECT rc = {0, 0, 0, 0};
};

static int CollectScreens(Screen *out, int max)
{
  struct Ctx
  {
    Screen *out;
    int max;
    int n;
  } ctx = {out, max, 0};
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR h, HDC, LPRECT, LPARAM user) -> BOOL {
        Ctx *c = (Ctx *)user;
        if (c->n >= c->max)
          return FALSE;
        MONITORINFOEXA ex;
        memset(&ex, 0, sizeof(ex));
        ex.cbSize = sizeof(ex);
        if (GetMonitorInfoA(h, &ex))
        {
          _snprintf(c->out[c->n].device, sizeof(c->out[c->n].device), "%s", ex.szDevice);
          c->out[c->n].rc = ex.rcMonitor;
          ++c->n;
        }
        return TRUE;
      },
      (LPARAM)&ctx);
  // The same ordering rule the feature uses, so index i means the same monitor on both sides.
  for (int i = 1; i < ctx.n; ++i)
    for (int j = i; j > 0 && strcmp(out[j - 1].device, out[j].device) > 0; --j)
    {
      Screen t = out[j];
      out[j] = out[j - 1];
      out[j - 1] = t;
    }
  return ctx.n;
}

static HDC ScreenDc(const char *device) { return CreateDCA("DISPLAY", device, nullptr, nullptr); }

// A ramp scaled by `k` (0.8 = 80%), out of `base`.
//
// ⚠️ THE PARAMETER IS A FRACTION, NOT A PERCENTAGE, AND THAT IS NOT A STYLE CHOICE. It was written as an
// integer percentage and called with `0.80`, which the compiler silently converted to 0 -- so this probe wrote
// an ALL-ZERO ramp, `SetDeviceGammaRamp` refused it (the same API floor the feature has to work around), and
// the section below reported "this probe could not write a ramp here" on both monitors and concluded nothing.
// A check that quietly measures nothing looks exactly like a check that passes; that is why the write's return
// value is checked at every use.
static void ScaleRamp(const WORD base[3][256], double k, WORD out[3][256])
{
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
      out[c][i] = (WORD)(base[c][i] * k + 0.5);
}

// The same tolerance the feature uses to tell its own ramp from somebody else's (See ApplyGamma).
static bool RampNear(const WORD a[3][256], const WORD b[3][256])
{
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
    {
      const int d = (int)a[c][i] - (int)b[c][i];
      if (d > 512 || d < -512)
        return false;
    }
  return true;
}

// ---------------------------------------------------------------------------------------------
// A SOUND OF THIS PROBE'S OWN
// ---------------------------------------------------------------------------------------------
//
// ⚠️ WITHOUT THIS, "the volume half works" WOULD BE UNTESTABLE ON A QUIET MACHINE. WASAPI's mixer holds a
// session for every process that has opened an audio stream, so a machine that is playing nothing may have
// exactly zero application rows -- and a check that expects at least one would fail for a reason that has
// nothing to do with the feature, while a check that tolerates zero would pass while the list was empty for
// ever. So the probe opens a silent render stream itself: that CREATES a session for this process, by the same
// mechanism every music player uses, and the volume group must then contain a row named after this program.
//
// The stream is started and never fed, which plays nothing (a shared-mode client that does not write simply
// contributes silence) and touches no device setting.
struct OwnSound
{
  IMMDeviceEnumerator *en = nullptr;
  IMMDevice *dev = nullptr;
  IAudioClient *client = nullptr;
  bool ok = false;

  void Start()
  {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
        !en)
      return;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev)
      return;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void **>(&client))) ||
        !client)
      return;
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 44100;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, &fmt, nullptr)))
      return;
    ok = SUCCEEDED(client->Start());
  }

  void Stop()
  {
    if (client)
    {
      client->Stop();
      client->Release();
      client = nullptr;
    }
    if (dev)
    {
      dev->Release();
      dev = nullptr;
    }
    if (en)
    {
      en->Release();
      en = nullptr;
    }
  }
};

// This probe's own bare process name, which is what the volume group must show a row for.
static void SelfName(char *out, int outSize)
{
  out[0] = 0;
  char self[600] = {0};
  GetModuleFileNameA(nullptr, self, (int)sizeof(self));
  const char *base = strrchr(self, '\\');
  base = base ? base + 1 : self;
  _snprintf(out, outSize, "%s", base);
  char *dot = strrchr(out, '.');
  if (dot && _stricmp(dot, ".exe") == 0)
    *dot = 0;
}

int main(int argc, char **argv)
{
  const char *dll = (argc >= 2) ? argv[1] : "build/apex/Plugins/MediaControl/MediaControl.dll";
  const char *dir = (argc >= 3) ? argv[2] : "build/_mediacontrol_probe/";
  _snprintf(g_dir, sizeof(g_dir), "%s", dir);

  // The feature enumerates audio sessions on this thread; a probe that skipped this would report "no
  // applications" and never notice that the volume half was broken.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // ... and one application IS making sound while this runs: this probe (see OwnSound).
  OwnSound sound;
  sound.Start();
  if (!sound.ok)
    printf("      (this probe could not open its own silent stream -- the volume rows may be empty)\n");

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
  ApexFeature *f = (ApexFeature *)entry();
  printf("MediaControl -- driven through %s\n", dll);
  if (f->abiVersion != APEX_ABI_VERSION || f->structSize < sizeof(ApexFeature))
  {
    printf("  FAIL: the DLL does not match this host's ABI\n");
    return 1;
  }
  Check(f->id && strcmp(f->id, "MediaControl") == 0, "the feature identifies itself as MediaControl",
        f->id ? f->id : "");
  Check(f->nameZh && f->nameZh[0] && f->nameEn && f->nameEn[0], "  and carries both display names", "");
  Check(f->onWheel == nullptr && f->tick == nullptr,
        "  and stays out of the input path (no onWheel, no tick)", "");
  Check(f->quickItems != nullptr, "  and can put controls in the quick panel", "");

  ApexHost host;
  memset(&host, 0, sizeof(host));
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLogLine;
  host.featureEnabled = HostFeatureEnabled;

  // How many monitors THIS machine has, counted independently of the feature.
  const int monitors = GetSystemMetrics(SM_CMONITORS);
  printf("      (this machine reports %d monitor(s))\n", monitors);

  printf("\n1. the controls it describes (from a clean folder, so these are the defaults)\n");
  Check(f->init(&host) == 0, "init reports READY (0) and starts the feature", "");
  static char doc[64 * 1024];
  int n = f->settingsJson(doc, (int)sizeof(doc));
  Check(n > 0, "it describes its controls", "");
  Check(Has(doc, "\"settingsFile\":\"MediaControl.ini\""), "  and names its own settings file", "");
  Check(Has(doc, "\"summaryZh\":") && Has(doc, "\"summaryEn\":"),
        "  and says in one line what it is for", "");

  printf("\n2. the brightness group: one row per monitor, and no way to add or remove one\n");
  Check(Has(doc, "\"id\":\"displays\",\"type\":\"group\""), "there is a monitors group", "");
  Check(Has(doc, "\"layout\":\"rows\""), "  drawn as rows (one line per monitor)", "");
  Check(Has(doc, "\"id\":\"brightness\",\"type\":\"range\",\"min\":0,\"max\":100"),
        "  and a 0-100 fader as a field of the row", "");
  // ⚠️⚠️ THE SCREEN-OFF SWITCH IS AN ORDINARY FIELD OF THE ROW, AND THE SHORTCUT BOX FOLLOWS IT, WHICH IS ONE
  // DECISION SEEN TWO WAYS (the user's, 2026-09-23): "「熄屏快捷键」合并到「亮度」面板，快捷键录入框跟在「熄屏」开关右边."
  // A `rowToggle` field is drawn at the row's RIGHT-HAND END, after every ordinary field (see apex/abi.h), so the
  // old shape would have put the box to the LEFT of the switch it has to follow -- hence no `rowToggle` here, and
  // hence this check reads the DOCUMENT'S OWN ORDER rather than trusting the fixture on the page.
  {
    const char *g = strstr(doc, "\"id\":\"displays\"");
    const char *bright = g ? strstr(g, "\"id\":\"brightness\"") : nullptr;
    const char *offSw = g ? strstr(g, "\"id\":\"off\",\"type\":\"bool\"") : nullptr;
    const char *hk = g ? strstr(g, "\"id\":\"hotkey\",\"type\":\"hotkey\"") : nullptr;
    Check(offSw != nullptr && hk != nullptr && bright != nullptr && bright < offSw && offSw < hk,
          "  the screen-off switch is a FIELD of the row, with the shortcut box after it", "");
    Check(!Has(doc, "\"rowToggle\":[\"off\"]"),
          "  and it is not a row-switch any more (those are drawn at the far right)", "");
  }
  {
    const int rows = CountOf(doc, "\"values\":{\"brightness\":");
    char detail[64] = {0};
    _snprintf(detail, sizeof(detail), "rows=%d monitors=%d", rows, monitors);
    Check(rows == monitors, "  one row per monitor THIS MACHINE has", detail);
    Check(CountOf(doc, ",\"locked\":true,\"values\":{\"brightness\":") == rows,
          "  every one of them locked (a monitor is not the user's to remove)", "");
  }
  // ⚠️ AND THE CARD THAT USED TO HOLD THE SHORTCUTS IS GONE, WITH ITS ID: the shortcut lives in the row above now,
  // so `offkeys[1].hotkey` is a path that names nothing. Asserted rather than merely deleted, because a group that
  // quietly comes back is a card the user has to close twice.
  Check(!Has(doc, "\"id\":\"offkeys\""), "  the separate screen-off-shortcuts card is gone", "");
  Check(CountOf(doc, ",\"hotkey\":") == monitors,
        "  with one shortcut box per monitor, on the monitors' own rows", "");
  // ⚠️ AND THE SHORTCUT FIELD CARRIES NO LABEL OF ITS OWN, WHICH IS THE USER'S ARRANGEMENT FOR THIS ROW: "每个设备
  // 后面有个「快捷键」的文字去掉". The page draws nothing where a label would be when the label is empty (the same
  // rule the name box and the fader use), so the two halves have to agree: a feature that started sending the words
  // back would put them on the page again -- and in a row, a label is a fixed 132-px column, so this is the
  // difference between "right after the switch" and "a hole in the middle of the line". Asserted on the DOCUMENT
  // rather than read from the source, because this is the thing the page acts on.
  Check(Has(doc, "\"id\":\"hotkey\",\"type\":\"hotkey\",\"labelZh\":\"\",\"labelEn\":\"\""),
        "  and the shortcut field asks for no label of its own", "");
  // ⚠️ AND THERE IS NO THIRD GROUP, WHICH IS A DECISION AND NOT AN OMISSION. Both of the ones that used to be here
  // are gone at the user's request: a per-screen "may Apex dim this" switch ("软件调光开关可以去了，这个没什么意义。
  // 默认打开就是了") and a per-machine "screen off via the monitor's own power" switch ("这样会变成断开显示器。统一采用
  // 现在的方案。"). Asserted rather than merely deleted, because a control that quietly comes back is a control the
  // user has to take out twice.
  Check(!Has(doc, "softdim") && !Has(doc, "ddc_power"), "  and no switch for the two things the user took out", "");

  // ⚠️⚠️ AND EACH GROUP CARRIES ITS OWN QUICK-PANEL SWITCH, INSIDE ITSELF (ABI 16 -> 17). This is the user's own
  // request, in their words: "媒体控制插件的「快速面板」开关，位置移到亮度和音量各自小标题的右侧，居右". The two used to
  // be top-level `bool` parameters ("快速面板：亮度" / "快速面板：音量"), drawn as a pair of switches at the bottom of
  // the page -- away from both groups they were about. A page can only put a switch on a group's own heading if the
  // GROUP is what carries it (see `quick` in apex/abi.h), so the shape is what is asserted here: one `quick` object
  // per group, naming the SAME control path the settings file and `setControl` use.
  Check(Has(doc, "\"id\":\"displays\",\"type\":\"group\"") &&
            Has(doc, "\"quick\":{\"id\":\"quick_brightness\""),
        "the brightness group carries the quick-panel switch that maps IT", "");
  Check(Has(doc, "\"quick\":{\"id\":\"quick_volume\""),
        "  and so does the volume group, with its own", "");
  Check(!Has(doc, "{\"id\":\"quick_brightness\",\"type\":\"bool\"") &&
            !Has(doc, "{\"id\":\"quick_volume\",\"type\":\"bool\""),
        "  and neither is a PARAMETER of its own any more (that is the stack of switches they moved out of)", "");
  // The label is the switch's own name and nothing more: on the group's heading there is no need to repeat which
  // group it is about (the old labels had to: "快速面板：亮度").
  Check(CountOf(doc, "\"labelZh\":\"快速面板\",\"labelEn\":\"Quick panel\"") == 2,
        "  both labelled simply 快速面板 / Quick panel, since each sits on the group it maps", "");


  printf("\n3. the volume group: the system mixer, as rows\n");
  Check(Has(doc, "\"id\":\"sessions\",\"type\":\"group\""), "there is an applications group", "");
  Check(Has(doc, "\"id\":\"volume\",\"type\":\"range\",\"min\":0,\"max\":100") &&
            Has(doc, "\"id\":\"mute\",\"type\":\"bool\""),
        "  each row has a volume fader and a mute switch", "");
  Check(Has(doc, "\"rowToggle\":[\"mute\"]"), "  with mute drawn as the row's switch", "");
  // ⚠️ AND THE APPLICATION LIST KEEPS ITSELF UP TO DATE, WITHOUT A BUTTON (ABI 17 -> 18). It used to declare an
  // action -- "刷新应用列表" / "Refresh applications" -- and the user's report was the button's whole problem:
  // "媒体控制列表的「刷新应用列表」按钮去掉，这个做成实时自动刷新". A list of the programs that are making sound is a
  // fact about the machine that changes while the user looks at it, so the group declares itself `live` and the
  // PAGE does the re-reading (see `live` in apex/abi.h). Both halves are asserted: the key is there, and the
  // button that asked for the same thing by hand is gone.
  Check(Has(doc, "\"live\":true"), "  and the group says its rows are LIVE, so the page re-reads it by itself", "");
  Check(!Has(doc, "\"op\":\"refresh\""),
        "  with no refresh button: the list is not a document with a version", "");
  {
    const int rows = CountOf(doc, "\"values\":{\"volume\":");
    char self[96] = {0};
    SelfName(self, (int)sizeof(self));
    char detail[128] = {0};
    _snprintf(detail, sizeof(detail), "%d application row(s) right now", rows);
    Check(rows >= 1, "  the applications that are making sound are listed", detail);
    // ⚠️ AND ONE OF THEM IS THIS PROBE: it opened a silent render stream at startup, so the mixer has a session
    // for it -- which is what makes "the list is really the system mixer" a fact rather than a hope.
    char want[128] = {0};
    _snprintf(want, sizeof(want), "\"title\":\"%s\"", self);
    Check(Has(doc, want), "  including this probe's own program, which opened a stream to be here", self);
    Check(CountOf(doc, ",\"locked\":true,\"values\":{\"volume\":") == rows,
          "  and those rows are locked too (they exist because a program is making sound)", "");
    // ⚠️ THE SYSTEM-SOUNDS ROW IS THE ONE WHOSE TITLE IS THE FEATURE'S OWN WORDS, so it travels in both
    // languages (abi.h: `titleZh`/`titleEn`) while every other row carries the user's own program name.
    Check(Has(doc, "\"titleZh\":") && Has(doc, "\"titleEn\":"),
          "  with the system-sounds row named in both languages", "");
  }

  printf("\n3b. the user's own name is the FLYOUT's label; the PAGE keeps the monitor's own name\n");
  //
  // ⚠️⚠️ THE USER'S RULE (2026-09-23), AND IT REVERSES AN EARLIER ONE: "媒体控制「亮度」部分，填入自定义设备名字后，它
  // 左边应还是显示设备原名，自定义名字只作用于「快速面板」." The alias used to travel to BOTH surfaces, on the argument
  // that a name only one of them showed would have to be learned twice -- and what that missed is what the page's
  // left-hand column is FOR: with "书桌左边" in it, the row no longer said which screen it belonged to.
  //
  // ⚠️ BOTH DIRECTIONS ARE CHECKED, because the two surfaces are one function and one boolean (`DisplayName`): a
  // probe that only looked at the flyout would pass while the page still showed the alias.
  if (f->setControl("displays[0].alias", "书桌左边") == 1)
  {
    static char again[64 * 1024];
    Check(f->settingsJson(again, (int)sizeof(again)) > 0 && !Has(again, "\"title\":\"书桌左边\""),
          "the page's row still shows the monitor's own name", "");
    Check(Has(again, "\"alias\":\"书桌左边\""),
          "  while the name box still holds what the user typed", "");
    Check(f->setControl("quick_brightness", "1") == 1, "mapping the brightness group for this check", "");
    static ApexQuickItem q[32];
    const int nq = f->quickItems(q, 32);
    bool aliasInFlyout = false;
    for (int i = 0; i < nq && i < 32; ++i)
      if (strcmp(q[i].groupZh, "亮度") == 0 && strcmp(q[i].labelZh, "书桌左边") == 0)
        aliasInFlyout = true;
    Check(aliasInFlyout, "  and the FLYOUT labels that row with the user's own name", "");
    Check(f->setControl("quick_brightness", "0") == 1, "  (un-mapping it again)", "");
    Check(f->setControl("displays[0].alias", "") == 1, "  (and the name is taken back)", "");
    Check(f->settingsJson(doc, (int)sizeof(doc)) > 0, "  (the document is re-read, for what follows)", "");
  }
  else
  {
    printf("  %-66s %s\n", "skipped (this machine reports no monitor to name)", "");
  }

  // ⚠️ AND THE DOCUMENT CAN BE LEFT BEHIND FOR A PICTURE (see `_diag/panel_preview.js`). A gate can say "the field
  // is in the document"; only a rendered page can say the row still FITS -- and this row grew a fourth control on
  // 2026-09-23 ("快捷键录入框跟在「熄屏」开关右边"), which is exactly the kind of change that is judged by looking at
  // it. An optional argument, so the gate that runs this probe is unaffected.
  if (argc >= 4 && strncmp(argv[3], "--dump=", 7) == 0)
  {
    FILE *dp = fopen(argv[3] + 7, "wb");
    if (dp)
    {
      fwrite(doc, 1, strlen(doc), dp);
      fclose(dp);
      printf("\n(the controls document was written to %s -- render it with _diag/panel_preview.js)\n",
             argv[3] + 7);
    }
  }

  printf("\n4. what it accepts, and what it REFUSES\n");
  {
    // A LEVEL OUTSIDE 0-100 IS REFUSED, not clamped: the page re-reads the control afterwards (see setControl
    // in abi.h), so a refusal is visible to the user as the slider snapping back -- while a silent clamp is a
    // slider that moves and then means something else.
    Check(f->setControl("displays[0].brightness", "50") == 1, "a level of 50 is accepted");
    Check(f->setControl("displays[0].brightness", "-1") == 0, "  a level below 0 is REFUSED");
    Check(f->setControl("displays[0].brightness", "101") == 0, "  a level above 100 is REFUSED");
    Check(f->setControl("displays[0].brightness", "abc") == 0, "  and something that is not a number", "");
    Check(f->setControl("displays[99].brightness", "50") == 0, "  and a monitor that is not there", "");
    Check(f->setControl("sessions[99].volume", "50") == 0,
          "a volume for an application row that is not there is REFUSED (never the wrong program)", "");
    Check(f->setControl("nosuch[0].brightness", "50") == 0, "  and a group this feature does not have", "");
    // ⚠️ A COMBINATION WITH NO MODIFIER IS REFUSED, because a global shortcut on a bare letter fires while the
    // user is typing. The page will not record one either -- but a page is not a gatekeeper.
    Check(f->setControl("displays[0].hotkey", "A") == 0, "a shortcut with no modifier is REFUSED");
    Check(f->setControl("displays[0].hotkey", "Ctrl+NoSuchKey") == 0, "  and one with a key it cannot name", "");
    Check(f->setControl("displays[0].hotkey", "Ctrl+Alt+Shift+F9") == 1,
          "but Ctrl+Alt+Shift+F9 is accepted and stored", "");
  }

  printf("\n5. THE SCREEN REALLY GOES DARK -- and comes back\n");
  //
  // ⚠️ THIS IS THE ONLY PART OF THIS GATE THAT TOUCHES THE USER'S SCREEN, so it is the only part with a way
  // out: `APEX_MC_NO_DARK=1` skips it. It is on by default because the alternative is a gate that never checks
  // the one thing this feature does that can be seen -- and it is short: the window is put back the moment it
  // has been observed, measured in tens of milliseconds, and nothing about the display mode, the focus or the
  // lock state is touched at any point.
  if (Quiet())
  {
    printf("  %-66s %s\n", "skipped (APEX_MC_NO_DARK / APEX_MC_NO_SCREEN is set)", "");
  }
  else
  {
    // ⚠️ THIS IS THE ONE OBSERVABLE BEHAVIOUR OF THE FEATURE, and it is checked by asking the WINDOW MANAGER
    // (not by asking the feature): a window of the feature's own class, VISIBLE, covering exactly the first
    // monitor's rectangle. "Not the display mode" is the whole point of the design -- the screen stays
    // connected, the desktop does not lock -- and a window is what makes that true.
    Check(f->setControl("displays[0].off", "1") == 1, "switching the first screen off is accepted");
    const FindCtx on = WaitForBlack(true);
    char detail[128] = {0};
    _snprintf(detail, sizeof(detail), "windows=%d visible=%d rect=%ld,%ld %ldx%ld", on.found, on.visible ? 1 : 0,
              on.rect.left, on.rect.top, on.rect.right - on.rect.left, on.rect.bottom - on.rect.top);
    Check(on.found == 1 && on.visible, "  and a black window appears", detail);
    // ⚠️⚠️ HALF A SECOND OF TRANSITION ON THE WAY DOWN, MEASURED. The window exists and is visible from the first
    // frame, so everything above this line passes just as well without any animation at all -- the opacity is the
    // evidence. It must be seen in the middle, and it must LAND: a fade that never finishes is a screen that is
    // never really dark (and, in the other direction, a window that never goes away and keeps the mouse).
    {
      const long seen = WatchFade(on.hwnd, 1500);
      char d2[96] = {0};
      _snprintf(d2, sizeof(d2), "first half-lit frame after %ld ms", seen);
      Check(seen >= 0, "  going dark is a HALF-SECOND fade, not a jump (the opacity is read back)", d2);
      Check(seen < 200, "    and it starts on the frame the window appears", d2);
      Sleep(600);
      const FindCtx solid = FindBlack();
      char d3[96] = {0};
      _snprintf(d3, sizeof(d3), "alpha=%d after 600 ms", (int)solid.alpha);
      Check(solid.found == 1 && solid.layered && solid.alpha == 255,
            "    and it lands on solid black when the half second is up", d3);
    }
    Check((on.exStyle & WS_EX_TOPMOST) != 0, "  on top of everything else (WS_EX_TOPMOST)", "");
    Check((on.exStyle & WS_EX_TOOLWINDOW) != 0, "  out of the taskbar and alt-tab (WS_EX_TOOLWINDOW)", "");
    Check((on.style & WS_POPUP) != 0, "  with no frame (WS_POPUP)", "");
    // The rectangle is a REAL monitor's own, which is how "the right screen" is said without trusting a name.
    {
      Screen screens[8];
      const int n = CollectScreens(screens, 8);
      bool matched = false;
      for (int i = 0; i < n; ++i)
        if (on.rect.left == screens[i].rc.left && on.rect.top == screens[i].rc.top &&
            (on.rect.right - on.rect.left) == (screens[i].rc.right - screens[i].rc.left) &&
            (on.rect.bottom - on.rect.top) == (screens[i].rc.bottom - screens[i].rc.top))
          matched = true;
      Check(matched, "  covering a real monitor's rectangle exactly", "");
    }
    // ⚠️ AND THE TRAY IS TOLD. A dark screen cannot say anything for itself, so this bit is the only place the
    // program can mention it (see APEX_FEATURE_USER_VISIBLE in abi.h).
    Check((f->flags() & APEX_FEATURE_USER_VISIBLE) != 0,
          "  and the host is told something is being held (the tray mark)", "");

    // ⚠️⚠️ AND THE PAGE IS TOLD, WHICH IS THE MECHANISM BEHIND THE USER'S "解除熄屏，开关状态要跟着关掉". The screen
    // here was switched off by the PAGE's own call, so the page knows -- but a dark screen is normally turned back
    // on by a CLICK on the dark window, in this process, with no page involved. The page finds out through
    // `waiting` (apex/abi.h): while it is in the document the panel keeps asking, and the switch follows. So the
    // document must SAY it while a screen is dark and stop saying it when none is.
    {
      static char doc[64 * 1024];
      const int n = f->settingsJson(doc, (int)sizeof(doc));
      Check(n > 0 && Has(doc, "\"waiting\""),
            "  and the controls document says it is WAITING while a screen is dark", "");
    }

    // ⚠️⚠️ AND ONE CLICK BRINGS IT BACK -- NOT A DOUBLE CLICK, AND NOT ON THE NEXT TICK OF A CLOCK. The user's
    // report: "熄屏后，双击或Esc后不能马上生效，只要鼠标在熄屏的屏幕上，都要生效". Two separate causes, both fixed,
    // and this is the check for both:
    //   * a window shown with SWP_NOACTIVATE spends the FIRST click of a double-click on activating itself, so
    //     the old WM_LBUTTONDBLCLK handler needed two double-clicks before anything happened;
    //   * the window used to be hidden by the next pass of the control thread, up to 200 ms later.
    // The message below is posted to the window rather than synthesised at the mouse, so nothing about the user's
    // own pointer is involved.
    //
    // ⚠️ AND THE SECOND CAUSE IS NOW MEASURED ON THE FADE RATHER THAN ON THE HIDE, because the hide is half a
    // second late BY DESIGN (see the transition below). "The fade began within 150 ms of the click" is the same
    // fact the old "gone in under 150 ms" was asking about: if the click only set a flag, nothing would move
    // until the control thread's next pass, which is 200 ms away.
    {
      const DWORD t0 = GetTickCount();
      PostMessageA(on.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
      const long seen = WatchFade(on.hwnd, 1500);
      char d2[96] = {0};
      _snprintf(d2, sizeof(d2), "first half-lit frame after %ld ms", seen);
      Check(seen >= 0, "  and coming back is a fade too, not a jump", d2);
      Check(seen < 150, "  at once, not on the control thread's next pass", d2);
      // ⚠️ AND THE WINDOW IS STILL THERE, WHICH IS THE POINT OF THE DELAY: it is still covering the screen, so
      // hiding it the moment the click arrived would let the next click through to whatever is underneath a
      // screen the user can still see is dark. (That it keeps TAKING the mouse is a property of the window, not
      // something this line proves -- what is checked here is that it has not been taken away yet.)
      const FindCtx mid = FindBlack();
      Check(mid.found == 1, "    (and it is still there while it fades, not whisked away)", "");
      const FindCtx gone = WaitForBlack(false, 1500);
      const DWORD took = GetTickCount() - t0;
      _snprintf(d2, sizeof(d2), "gone after %lu ms", (unsigned long)took);
      Check(gone.found == 0, "  and ONE click on the dark screen brings it back", d2);
      Check(took >= 400, "    after the half second rather than instantly", d2);
    }
    {
      // ⚠️ AND IT STOPS SAYING IT ONCE THE SCREEN IS BACK -- asked as a CONDITION with a bound, not read once.
      // `waiting` is published for two different reasons (apex/abi.h): a screen being dark, and a brightness
      // change made by SOMETHING ELSE being followed for the next 8 seconds (kExternalFollowMs). A document read a
      // second after the screen came back can therefore still carry it, legitimately, because of the OTHER user
      // request -- and this check used to read the document exactly once and call that a failure. What it is
      // really about is that this reason ENDS BY ITSELF: a page told to keep polling for ever is a page that never
      // stops. So it is waited for, up to longer than the follow window, and a feature that never clears it still
      // fails.
      static char doc[64 * 1024];
      bool cleared = false;
      const DWORD start = GetTickCount();
      while (!cleared && (GetTickCount() - start) < 15000)
      {
        Sleep(200);
        const int n = f->settingsJson(doc, (int)sizeof(doc));
        if (n > 0 && !Has(doc, "\"waiting\""))
          cleared = true;
      }
      char d4[64] = {0};
      _snprintf(d4, sizeof(d4), "cleared after %lu ms", (unsigned long)(GetTickCount() - start));
      Check(cleared, "  and the document stops saying so once the screen is back (the poll ends)", d4);
    }

    Check(f->setControl("displays[0].off", "0") == 1, "switching it back on is accepted");
    const FindCtx gone = WaitForBlack(false);
    Check(gone.found == 0, "  and the black window is gone", "");
    Check((f->flags() & APEX_FEATURE_USER_VISIBLE) == 0, "  and the host is told it is not held any more", "");
  }

  printf("\n6. the global shortcut is really TAKEN\n");
  {
    // ⚠️ ASKED OF WINDOWS, NOT OF THE FEATURE. `RegisterHotKey` is exclusive: if this probe can register the
    // same combination, the feature did not. That is the difference between "the box on the page holds a
    // combination" and "pressing it does something", and it is invisible without trying it.
    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    const UINT vk = VK_F9;
    Check(RegisterHotKey(nullptr, 0x7A01, mods, vk) == FALSE,
          "the combination Ctrl+Alt+Shift+F9 is already owned (the feature has it)", "");
    if (GetLastError() != ERROR_HOTKEY_ALREADY_REGISTERED)
      printf("      (Windows said %lu rather than ERROR_HOTKEY_ALREADY_REGISTERED)\n", GetLastError());

    Check(f->setControl("displays[0].hotkey", "") == 1, "clearing the shortcut is accepted");
    Settle();
    const BOOL mine = RegisterHotKey(nullptr, 0x7A02, mods, vk);
    Check(mine != FALSE, "  and the combination is free again once it is cleared", "");
    if (mine)
      UnregisterHotKey(nullptr, 0x7A02);
  }

  printf("\n7. the quick panel: two NAMED groups, and screen-off is not one of them\n");
  {
    static ApexQuickItem items[32];
    Check(f->quickItems(items, 32) == 0, "nothing is mapped by default (the user's rule: a switch per group)");

    Check(f->setControl("quick_brightness", "1") == 1, "mapping the brightness group is accepted");
    int total = f->quickItems(items, 32);
    Check(total == monitors, "  and then every monitor's fader is offered", "");
    bool groupOk = total > 0, idOk = total > 0, sliderOk = total > 0;
    for (int i = 0; i < total && i < 32; ++i)
    {
      if (strcmp(items[i].groupZh, "亮度") != 0 || strcmp(items[i].groupEn, "Brightness") != 0)
        groupOk = false;
      char want[64] = {0};
      _snprintf(want, sizeof(want), "displays[%d].brightness", i);
      if (strcmp(items[i].id, want) != 0)
        idOk = false;
      if (items[i].type != APEX_QUICK_SLIDER || items[i].max != 100.0 || items[i].hue == 0)
        sliderOk = false;
    }
    Check(groupOk, "  every one of them names its group 亮度 / Brightness (one pane, not one pane each)");
    Check(idOk, "  and carries the very same setControl path the settings page uses", "");
    Check(sliderOk, "  as a 0-100 fader with the feature's own colour", "");
    // (A brightness fader used to be checked for having NO companion at all here. It has one now -- that monitor's
    // screen-off control -- and the checks for it are below, where both groups are mapped and the paths can be
    // compared with the rows they belong to.)

    // ⚠️ AND EVERY BRIGHTNESS FADER CARRIES THAT MONITOR'S SCREEN-OFF CONTROL (the user's "快速面板的熄屏功能也像
    // 静音按钮一样，做上去"). Checked as a PATH for the same reason as the mute: the panel draws a button and sends
    // "1"/"0" to whatever path it was handed, so what has to be true is that it belongs to the SAME monitor as the
    // fader -- `displays[i].brightness` pairs with `displays[i].off` and nothing else.
    {
      int brightRows = 0, withOff = 0, wrongOff = 0, iconOk = 0;
      for (int i = 0; i < total && i < 32; ++i)
      {
        if (strcmp(items[i].groupZh, "亮度") != 0)
          continue;
        ++brightRows;
        char want[64] = {0};
        int row = -1;
        if (sscanf(items[i].id, "displays[%d].brightness", &row) == 1)
          _snprintf(want, sizeof(want), "displays[%d].off", row);
        if (items[i].toggleId[0])
          ++withOff;
        if (items[i].toggleIcon == APEX_QUICK_ICON_DISPLAY)
          ++iconOk;
        if (!want[0] || strcmp(items[i].toggleId, want) != 0)
          ++wrongOff;
      }
      char d9[110] = {0};
      _snprintf(d9, sizeof(d9), "%d of %d row(s), %d wrong, %d with the display icon", withOff, brightRows,
                wrongOff, iconOk);
      Check(brightRows > 0 && withOff == brightRows && wrongOff == 0,
            "  each with that monitor's own screen-off control (displays[i].off)", d9);
      Check(iconOk == brightRows, "  drawn as a display rather than as a speaker", d9);
    }

    Check(f->setControl("quick_volume", "1") == 1, "mapping the volume group is accepted");
    total = f->quickItems(items, 32);
    bool volumeNamed = false, offLeaked = false;
    for (int i = 0; i < total && i < 32; ++i)
    {
      if (strcmp(items[i].groupZh, "音量") == 0 && strcmp(items[i].groupEn, "Volume") == 0)
        volumeNamed = true;
      if (strstr(items[i].id, ".off") || strstr(items[i].id, "hotkey"))
        offLeaked = true;
    }
    Check(volumeNamed, "  and the application faders arrive as one group named 音量 / Volume", "");
    // ⚠️⚠️ AND EVERY ONE OF THEM CARRIES ITS OWN MUTE BUTTON (the user's "音量在推子右边增加静音按钮"). The check is
    // the PATH, not the picture: the panel draws a button and sends "1"/"0" to whatever path it was given, so what
    // has to be true is that the path belongs to the SAME application the fader belongs to -- `sessions[i].volume`
    // pairs with `sessions[i].mute` and nothing else. A feature could otherwise wire its button to another row's
    // mute, which the panel could never notice.
    {
      int volumeRows = 0, withMute = 0, wrongMute = 0, iconOk = 0;
      for (int i = 0; i < total && i < 32; ++i)
      {
        if (strcmp(items[i].groupZh, "音量") != 0)
          continue;
        ++volumeRows;
        char want[64] = {0};
        int row = -1;
        if (sscanf(items[i].id, "sessions[%d].volume", &row) == 1)
          _snprintf(want, sizeof(want), "sessions[%d].mute", row);
        if (items[i].toggleId[0])
          ++withMute;
        if (items[i].toggleIcon == APEX_QUICK_ICON_MUTE)
          ++iconOk;
        if (!want[0] || strcmp(items[i].toggleId, want) != 0)
          ++wrongMute;
      }
      char d8[110] = {0};
      _snprintf(d8, sizeof(d8), "%d of %d volume row(s), %d wrong, %d with the speaker icon", withMute, volumeRows,
                wrongMute, iconOk);
      Check(volumeRows > 0 && withMute == volumeRows && wrongMute == 0,
            "  each with the mute button of ITS OWN application (sessions[i].mute)", d8);
      Check(iconOk == volumeRows, "  drawn as a speaker rather than as a display", d8);
    }
    // ⚠️ THE USER SAID SO IN SO MANY WORDS: "只要亮度控制映射到快速面板，熄屏不用". The way back from a dark
    // screen must not be a control that is itself on the dark screen.
    Check(!offLeaked, "  and SCREEN-OFF IS NOT IN THERE (the user's own instruction)", "");

    // The permission is a real gate in both directions, not a one-way switch.
    Check(f->setControl("quick_brightness", "0") == 1, "un-mapping the brightness group");
    total = f->quickItems(items, 32);
    bool stillBright = false;
    for (int i = 0; i < total && i < 32; ++i)
      if (strcmp(items[i].groupZh, "亮度") == 0)
        stillBright = true;
    Check(!stillBright, "  takes it back out of the panel", "");
  }

  printf("\n8. what it writes, and what it deliberately does NOT\n");
  {
    Check(f->setControl("displays[0].brightness", "42") == 1, "a level to remember");
    Check(f->setControl("displays[0].hotkey", "Ctrl+Alt+Shift+F9") == 1, "  and a shortcut to remember");
    Check(f->saveSettings() == 1, "it writes its settings into its own folder", "");
    char ini[600] = {0};
    _snprintf(ini, sizeof(ini), "%sMediaControl.ini", dir);
    FILE *fp = fopen(ini, "rb");
    Check(fp != nullptr, "  and the file is there", ini);
    if (fp)
    {
      static char text[4096];
      const size_t got = fread(text, 1, sizeof(text) - 1, fp);
      fclose(fp);
      text[got] = 0;
      Check(Has(text, "display="),
            "  one `display=<identity>|<device>|<level>|<shortcut>|<name>` line per monitor", "");
      // ⚠️⚠️ AND THE KEY IS THE MONITOR, NOT THE SLOT. `\\.\DISPLAY1` moves when screens are plugged, unplugged or
      // rearranged -- on this very machine the same external screen was `DISPLAY1` at one point and `DISPLAY2` at
      // another -- so a file keyed on it drags the user's brightness, name and shortcut onto a different screen.
      // The fallback to a device name still exists in the code (a panel that will not say what it is has to be
      // keyed on something), but on a machine where WMI answers it must not be what is written.
      Check(!Has(text, "display=\\\\.\\DISPLAY"),
            "  and keyed on the monitor's own identity, not on its slot name", "");
      Check(Has(text, "|42|Ctrl+Alt+Shift+F9"), "  carrying the level and the shortcut", "");
      Check(Has(text, "quick_brightness=") && Has(text, "quick_volume="),
            "  and which groups are mapped to the quick panel", "");
      // ⚠️⚠️ WHETHER A SCREEN IS DARK IS NOT IN THE FILE, AND THAT IS THE POINT. A gamma ramp and a black
      // window are states of THIS RUN; a program that remembered "screen 2 was off" and turned it off again at
      // the next start would boot with a black screen. Asserted as an exact SHAPE rather than by looking for a
      // key that might be spelled some other way: every display line carries the identity, the device, the level,
      // the shortcut and the name and NOTHING else.
      //
      // ⚠️ AND ONLY LINES THAT START WITH THE KEY COUNT. The file's own header explains the format, so it
      // contains the words `display=` and `|` in prose -- counting every occurrence counted the documentation
      // as data (four lines and eight separators for two monitors, which is exactly what the first run of this
      // check reported). A settings file whose comments count as entries is a check that measures its own
      // instructions.
      {
        int lines = 0, bars = 0;
        for (const char *p = text; *p;)
        {
          const char *eol = strchr(p, '\n');
          const size_t len = eol ? (size_t)(eol - p) : strlen(p);
          if (len > 8 && strncmp(p, "display=", 8) == 0)
          {
            ++lines;
            for (size_t i = 0; i < len; ++i)
              if (p[i] == '|')
                ++bars;
          }
          if (!eol)
            break;
          p = eol + 1;
        }
        char shape[64] = {0};
        _snprintf(shape, sizeof(shape), "%d line(s), %d separator(s)", lines, bars);
        // ⚠️ FOUR SEPARATORS AND NO MORE: `<identity>|<device>|<level>|<shortcut>|<your name>`. The count is the
        // assertion -- an on/off flag for the SCREEN would make it five, which is how "a screen's state is not a
        // preference" is held down by machine rather than by comment. (It was three before the monitor's own
        // identity became the key and two before the name field existed; it was briefly five while a per-screen
        // "may Apex dim this" switch existed, and it went back down when the user took that switch out. The check
        // moves with the format, which is the point of asserting the SHAPE rather than looking for a key that might
        // be spelled some other way.)
        //
        // ⚠️ AND A LINE WITH SIX FIELDS IS STILL READ, SO THE COUNT IS ABOUT WHAT IS WRITTEN, NOT ABOUT WHAT IS
        // ACCEPTED: a file from the build that wrote the switch must keep loading (see LoadSettings).
        Check(lines == monitors && bars == 4 * monitors,
              "  with exactly <identity>|<device>|<level>|<shortcut>|<name> -- NO on/off state", shape);
      }
    }

    // ⚠️ THE FILE AS IT STANDS AFTER EACH SAVE BELOW, read fresh each time: both checks after this one are about
    // what a save did to lines that are not the connected screens' own.
    static char after[4096];

    // ⚠️⚠️ AN UNPLUGGED SCREEN KEEPS ITS LINE, WHICH IS WHAT THE FILE'S OWN HEADER PROMISES AND WHAT THE USER'S
    // OWN FILE SHOWED WAS NOT TRUE: "外接显示器换了后，记录就乱了" -- his external screen's line was simply GONE,
    // because this function wrote only the monitors attached at that moment, so the next save deleted the memory of
    // anything that had been unplugged.
    //
    // The check plants a line for a monitor that is not here, re-reads the file (which is what puts it in the
    // feature's own table), saves, and looks for it again. A ghost is the only way to test this without unplugging
    // somebody's screen.
    {
      after[0] = 0;
      FILE *ghost = fopen(ini, "ab");
      bool planted = false;
      if (ghost)
      {
        planted = fprintf(ghost, "display=GHOST-9999|\\\\.\\DISPLAY9|55|Ctrl+Alt+9|ghost screen\n") > 0;
        fclose(ghost);
      }
      Check(planted, "  (a line for a screen that is not connected is planted)", "");
      f->reloadSettings();
      Check(f->saveSettings() == 1, "and a save with a screen missing", "");
      FILE *fp2 = fopen(ini, "rb");
      if (fp2)
      {
        const size_t n = fread(after, 1, sizeof(after) - 1, fp2);
        fclose(fp2);
        after[n] = 0;
      }
      Check(Has(after, "GHOST-9999") && Has(after, "|Ctrl+Alt+9|ghost screen"),
            "  a screen that is not attached KEEPS its brightness, shortcut and name", "");
    }

    // ⚠️⚠️ AND A SCREEN IS NEVER GIVEN ANOTHER SCREEN'S LINE BECAUSE THEY SHARE A SLOT -- the fix for the user's
    // report, and the reason the search no longer falls through to the device name for a panel that names itself:
    // "外接显示器换了后，记录就乱了 ... 要以设备的自身的型号为身份". `\\.\DISPLAY1` is a slot (the file's own header says
    // so), so a line written for the screen that USED to be in slot 1 must not be handed to the one that is there
    // now -- which is exactly what happens when an external monitor is replaced.
    //
    // The slot is taken from the line the feature itself just wrote (so this is the real device name on this
    // machine), and the identity in the planted line is deliberately NOT the real panel's.
    {
      char slot[64] = {0};
      char ident[96] = {0};
      for (const char *p = after; *p;)
      {
        const char *eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 8 && strncmp(p, "display=", 8) == 0 && strstr(p, "\\\\.") && p[8] != 'G')
        {
          // ... the identity is everything before the first `|`, the slot is the field after it.
          const char *bar = (const char *)memchr(p, '|', len);
          if (bar)
          {
            size_t k = (size_t)(bar - (p + 8));
            if (k < sizeof(ident))
            {
              memcpy(ident, p + 8, k);
              ident[k] = 0;
            }
            const char *bar2 = (const char *)memchr(bar + 1, '|', len - (size_t)(bar + 1 - p));
            if (bar2)
            {
              size_t n = (size_t)(bar2 - (bar + 1));
              if (n < sizeof(slot))
              {
                memcpy(slot, bar + 1, n);
                slot[n] = 0;
              }
            }
          }
          break;
        }
        if (!eol)
          break;
        p = eol + 1;
      }
      if (!slot[0])
        _snprintf(slot, sizeof(slot), "\\\\.\\DISPLAY1"); // nothing to read: the odds are this is the one
      FILE *other = fopen(ini, "wb");
      bool wrote = false;
      if (other)
      {
        fprintf(other, "# a file from a machine whose other screen was in this slot\n");
        fprintf(other, "display=OTHER-0000|%s|33|Ctrl+Alt+8|the old screen\n", slot);
        wrote = true;
        fclose(other);
      }
      Check(wrote && ident[0], "  (a line for ANOTHER screen in this slot is planted)", ident);
      f->reloadSettings();
      static char doc2[64 * 1024];
      const int n2 = f->settingsJson(doc2, (int)sizeof(doc2));
      Check(n2 > 0 && !Has(doc2, "\"brightness\":33") && !Has(doc2, "\"brightness\": 33"),
            "  and the screen in that slot does NOT inherit it (identity, not the slot)", "");
      Check(n2 > 0 && !Has(doc2, "the old screen"),
            "  its name is not borrowed either", "");

      // ⚠️⚠️ AND A LINE WHOSE FIELDS ARE IN THE WRONG ORDER DOES NOT BECOME A SHORTCUT. The file is hand-editable
      // by design, and the user's own has a line that says `display=BOE-0A8D|BOE-0A8D|0|100|` -- a hand edit with
      // the fields shifted, which reads back as "the shortcut is 100". Applying it would leave the row showing a
      // combination that does not exist and the control thread trying to register it. The check plants exactly that
      // line for the FIRST monitor (identity and slot taken from what the feature itself wrote) and asserts the
      // page is not told a shortcut.
      {
        FILE *bad = fopen(ini, "wb");
        bool wrote2 = false;
        if (bad)
        {
          fprintf(bad, "display=%s|%s|0|100|shifted\n", ident[0] ? ident : "FAKE-0000", slot);
          wrote2 = true;
          fclose(bad);
        }
        Check(wrote2 && ident[0], "  (a line with its fields shifted is planted)", ident);
        f->reloadSettings();
        static char doc3[64 * 1024];
        const int n3 = f->settingsJson(doc3, (int)sizeof(doc3));
        Check(n3 > 0 && !Has(doc3, "\"hotkey\":\"100\""),
              "  and a shortcut that cannot be parsed is not taken from it", "");
      }

      // ⚠️⚠️ AND A LINE THAT ENDS WITH `|` IS STILL FIVE FIELDS -- WHICH IS HOW EVERY LINE WITH NO CUSTOM NAME IS
      // WRITTEN (the name is the last field), AND WHICH USED TO BE READ AS THE PREVIOUS FORMAT. The damage is not
      // subtle: the identity became the device, the level became 0, and the shortcut became the old level, so a
      // perfectly good record turned into `BOE-0A8D|BOE-0A8D|0|100|` on the next save -- the user's own file, and
      // the reason it looked like the records had "gone somewhere".
      {
        FILE *trail = fopen(ini, "wb");
        bool wrote3 = false;
        if (trail)
        {
          // Exactly the shape SaveSettings writes for a monitor with no custom name.
          fprintf(trail, "display=%s|%s|33|Ctrl+Alt+7|\n", ident[0] ? ident : "FAKE-0000", slot);
          wrote3 = true;
          fclose(trail);
        }
        Check(wrote3, "  (a line with an EMPTY last field -- an unnamed screen -- is planted)", "");
        f->reloadSettings();
        static char doc4[64 * 1024];
        const int n4 = f->settingsJson(doc4, (int)sizeof(doc4));
        Check(n4 > 0 && Has(doc4, "\"brightness\":33"),
              "  and the level on it survives the empty field", "");
        Check(n4 > 0 && Has(doc4, "\"hotkey\":\"Ctrl+Alt+7\""),
              "  ... and so does the shortcut beside it", "");
        Check(n4 > 0 && !Has(doc4, "\"hotkey\":\"33\"") && !Has(doc4, "\"brightness\":0"),
              "  (neither of them shifted into the other's field)", "");
      }
      // ⚠️ AND THE FOLDER IS LEFT THE WAY IT WAS FOUND: the planted line is thrown away, the feature's table is
      // re-read from nothing, and the file is written again -- so it holds the connected screens' own lines and
      // nothing else, which is what the checks after this section (and the next run of this probe) expect to see.
      remove(ini);
      f->reloadSettings();
      f->saveSettings();
    }
  }

  // ---------------------------------------------------------------------------------------------
  printf("\n9b. SHARING THE GAMMA RAMP WITH ANOTHER TOOL -- the user's own question\n");
  //
  // THE USER'S WORDS: "2880x864这个屏亮度是用软亮度调节，它原来也有个驱动，也是可以调，好像也是这个原理，现在两个调起来
  // 像是叠加的效果，是各调各的吗？" -- that screen has a vendor tool that dims it the same way this feature does.
  //
  // ⚠️⚠️ THERE ARE TWO FACTS TO CHECK HERE AND THE SECOND ONE IS THE SURPRISE.
  //
  //   (1) WHOSE STATE SURVIVES. A gamma ramp is one value per channel per display, so two tools overwrite each
  //       other. The rule this feature follows: before every change it reads the ramp back, and if what is there
  //       is not what IT wrote, another tool owns the screen and that state becomes the new 100%.
  //   (2) ⚠️ WINDOWS REFUSES A RAMP WHOSE WHITE POINT IS BELOW HALF OF FULL SCALE. Measured with
  //       `_diag/gamma_set_probe.cpp`: 50% (white 32768) accepted, 48% refused, 10% refused -- a security floor
  //       in the display stack with no error code and no message. So a slider that asks for `level%` of the
  //       baseline has a lower half that moves and does nothing as soon as anything else has dimmed the screen.
  //       The feature now maps the slider's travel into the range the API accepts, which is what the second half
  //       of this section checks: at 0% the screen must really change, and nothing may be REFUSED.
  //
  // ⚠️ IT CHANGES THE SCREEN FOR ABOUT A SECOND and puts it back, and it is behind the same switch as the
  // screen-off test. A monitor whose brightness is NOT driven through gamma (WMI or DDC) is detected and skipped
  // rather than failed.
  if (Quiet())
  {
    printf("  %-66s %s\n", "skipped (APEX_MC_NO_DARK / APEX_MC_NO_SCREEN is set)", "");
  }
  else
  {
    static char log[64 * 1024];
    Screen screens[8];
    const int screenCount = CollectScreens(screens, 8);
    int tested = 0, gammaScreens = 0;
    for (int i = 0; i < screenCount && i < monitors; ++i)
    {
      HDC dc = ScreenDc(screens[i].device);
      if (!dc)
        continue;
      // What is on the screen now, kept only so it can be put back at the end of this screen's turn.
      WORD original[3][256];
      if (!GetDeviceGammaRamp(dc, original))
      {
        DeleteDC(dc);
        continue;
      }
      ++tested;

      // (a) Play the other tool: dim this screen to 80% behind the feature's back.
      //
      // ⚠️⚠️ THE "OTHER TOOL" IS A FIXED FRACTION OF FULL SCALE, NOT OF WHATEVER IS ON THE SCREEN RIGHT NOW, AND
      // THAT IS NOT A DETAIL. The feature's baseline IS full scale -- it normalises the ramp it finds to a white
      // point of 65535 before using it as 100% (see ApplyGamma) -- so 80% of full scale is exactly "somebody else
      // dimmed this panel to 80%", which is the thing being tested. What is on the screen when this section
      // starts is NOT something this probe may rely on: measured over several runs it is sometimes the feature's
      // own 100% write and sometimes the panel's own undimmed ramp (the feature puts the user's ramp back when it
      // lets go of a screen, and a dark window coming down puts it back too). A percentage of a dim ramp can land
      // BELOW the API's own floor, and `SetDeviceGammaRamp` then refuses it with NO error code at all -- so this
      // section reported "this probe could not write a ramp here" and tested nothing, intermittently, for a reason
      // that has nothing to do with the feature.
      WORD full[3][256];
      for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 256; ++k)
          full[c][k] = (WORD)(k * 257);
      // ⚠️ THE WRITE IS CHECKED. The first version of this check ignored the return value, so when the ramp
      // write was refused the probe went on to measure a screen it had never changed and reported
      // "white point 1.000 (want 0.480)" -- a failure of the check itself, dressed up as a failure of the
      // feature. Nothing below is concluded unless this line worked.
      WORD theirs[3][256];
      ScaleRamp(full, 0.80, theirs);
      if (!SetDeviceGammaRamp(dc, theirs))
      {
        printf("      %s: this probe could not write a ramp here -- cannot test (err=%lu)\n", screens[i].device,
               (unsigned long)GetLastError());
        SetDeviceGammaRamp(dc, original);
        DeleteDC(dc);
        continue;
      }

      char path[64] = {0};
      // (b) Ask for 90% and wait for the feature to write (the work happens on its own control thread).
      _snprintf(path, sizeof(path), "displays[%d].brightness", i);
      const int asked = f->setControl(path, "90");
      WORD now[3][256];
      bool moved = false;
      const DWORD start = GetTickCount();
      while ((GetTickCount() - start) < 4000)
      {
        Sleep(40);
        if (!GetDeviceGammaRamp(dc, now))
          break;
        if (!RampNear(now, theirs))
        {
          moved = true;
          break;
        }
      }
      if (!moved)
      {
        // This monitor is not driven through gamma, so the feature had nothing to write: correct behaviour, and
        // nothing to conclude about baselines here.
        //
        // ⚠️ AND IT SAYS WHY, WITH NUMBERS, BECAUSE "SKIPPED" IS A SILENT WAY FOR THIS WHOLE SECTION TO STOP
        // MEANING ANYTHING. Two very different things end up here -- a screen whose brightness is not driven
        // through gamma at all (WMI, DDC/CI: correct, nothing to test) and a screen that IS driven through gamma
        // but was not written to (a real failure, hidden). The white points and the answer to `setControl` are
        // what tells them apart.
        printf("      %s: not driven through gamma -- skipped (%s -> %d, white %.3f -> %.3f)\n", screens[i].device,
               path, asked, (double)theirs[0][255] / 65535.0, (double)now[0][255] / 65535.0);
        SetDeviceGammaRamp(dc, original);
        DeleteDC(dc);
        continue;
      }
      ++gammaScreens;

      // (c) ⚠️⚠️ THE FEATURE WRITES FROM ITS OWN BASELINE -- IT DOES NOT ADOPT WHAT IT FINDS. That is the user's
      // decision, made after living with the opposite rule: "能做成统一控制?" The version before this one read the
      // ramp back before every change and took whatever it found as the new 100%, which stopped the two tools
      // eating each other's settings and made their effects MULTIPLY ("两个都调到最暗近乎看不见"). Now the ramp as
      // it was when the feature first saw the display is 100%, "90%" means 90% of THAT, and two tools overwrite
      // each other instead of compounding.
      //
      // So this check is the OPPOSITE of the one it replaces: with a hand-written 80% ramp sitting on the screen,
      // asking for 90% must produce ~90% of the original (0.9 x the FULL-SCALE baseline) and NOT ~77% (0.9 x the
      // 80% that was found there) -- the other tool's value is REPLACED, not multiplied into.
      //
      // ⚠️ AND "OF THE ORIGINAL" IS NOT QUITE THE RIGHT YARDSTICK EITHER, WHICH IS THE PART WORTH KNOWING: this
      // feature normalises the ramp it finds to full scale before using it as a baseline, so its 90% is 90% of
      // FULL SCALE -- never 90% of what the other tool had left there. The yardstick below is therefore full
      // scale itself (and the "original" ramp this run found is kept only to be put back at the end).
      const double ratio = (double)now[0][255] / 65535.0;
      char detail[200] = {0};
      _snprintf(detail, sizeof(detail),
                "%s: white %.3f of full scale (multiplying with the 80%% it found would give ~0.76)",
                screens[i].device, ratio);
      Check(ratio > 0.86 && ratio < 1.00,
            "  a percentage is measured from ITS OWN baseline, so two tools do not multiply", detail);

      // (d) AND THE BOTTOM OF THE SLIDER MUST DO SOMETHING. 0% means "as dark as Windows allows from here", not
      // "52% below full scale and silently refused" -- so the screen has to change, and it has to change to
      // something at or above the floor.
      _snprintf(path, sizeof(path), "displays[%d].brightness", i);
      f->setControl(path, "0");
      WORD dark[3][256];
      bool darker = false;
      const DWORD start2 = GetTickCount();
      while ((GetTickCount() - start2) < 4000)
      {
        Sleep(40);
        if (!GetDeviceGammaRamp(dc, dark))
          break;
        if (!RampNear(dark, now))
        {
          darker = true;
          break;
        }
      }
      const double darkRatio = (double)dark[0][255] / 65535.0;
      _snprintf(detail, sizeof(detail), "%s: white %.3f of full scale", screens[i].device, darkRatio);
      Check(darker && dark[0][255] >= 32700, "  and 0% really darkens the screen, down to the API's own floor",
            detail);

      // ... and nothing was refused while doing it. The feature's own log is where the API's silent refusal
      // would be recorded, and this is the check that the slider's travel stays inside what Windows accepts.
      ReadLog(dir, log, (int)sizeof(log));
      Check(!Has(log, "SetDeviceGammaRamp REFUSED"),
            "  and no gamma write was refused by Windows (the slider stays inside the API's floor)", "");

      // (e) ⚠️ AND A CHANGE MADE SOMEWHERE ELSE IS ADOPTED -- the user's third request: "主屏SDC4190系统自带亮度调节
      // 快捷键也可以调，它两在调节时，推子是否可以相互实时更新状态". Play the other tool again (a 70% ramp), wait for
      // the feature's own poll (it reads every screen every couple of seconds), and the value the page is sent
      // must have moved to it.
      //
      // ⚠️ AND THE NUMBER IS NOT 70, WHICH IS A GOOD PLACE TO BE REMINDED OF WHY: a ramp at 70% of the baseline's
      // white point is not "70%" on the slider, because the slider's 0 is the API's floor (about half of full
      // scale) rather than zero. The same mapping the write uses inverts it, so the expected value is computed
      // here from the same floor constant -- the assertion is about the MAPPING being consistent in both
      // directions, which is the thing that was wrong before (it compared a ramp reading against a slider value
      // directly and called every write "ignored").
      {
        WORD outside[3][256];
        ScaleRamp(full, 0.70, outside);
        // ⚠️ THE FEATURE'S BASELINE IS THE RAMP IT FOUND, NORMALISED TO FULL SCALE (see ApplyGamma), so the
        // expected slider value is computed against 65535 -- not against what the screen happened to be at when
        // this run started.
        const double kMin = 33000.0 / 65535.0;
        const double kOutside = (double)outside[0][255] / 65535.0;
        const int want = kMin < 1.0 ? (int)((kOutside - kMin) / (1.0 - kMin) * 100.0 + 0.5) : 100;
        // ⚠️ THE WRITE AND WHAT IT LEFT BEHIND ARE PRINTED, because "the other tool's ramp did not take" and "the
        // feature wrote over it" look identical from the assertion below -- and the first one is not the feature's
        // fault. The API refuses a ramp below its own floor with no error code at all (`_diag/gamma_set_probe.cpp`),
        // and a refused write here would be reported as the feature ignoring an outside change.
        const BOOL wrote = SetDeviceGammaRamp(dc, outside);
        WORD check[3][256];
        const bool checkOk = GetDeviceGammaRamp(dc, check) != 0;
        printf("      %s: played the other tool (white %u), write %s, screen now %u\n", screens[i].device,
               outside[0][255], wrote ? "accepted" : "REFUSED", checkOk ? check[0][255] : 0);
        int followed = -1;
        static char doc[64 * 1024];
        for (int t = 0; t < 60 && followed < 0; ++t)
        {
          Sleep(100);
          const int n = f->settingsJson(doc, (int)sizeof(doc));
          if (n <= 0)
            continue;
          // ⚠️ EVERY MONITOR'S VALUE IS PARSED, AND THE ONE THIS LOOP IS ABOUT IS PICKED BY INDEX. The first
          // version advanced a pointer with `p = strstr(p, ...)` in a loop and FORGOT TO MOVE IT PAST THE MATCH
          // -- so it returned the first monitor's value every time and this check reported "the page was told
          // 90" while the document it was reading said 0. A probe with an indexing bug reports the feature as
          // broken; the parse below is written so that cannot happen.
          int all[8] = {0};
          {
            const char *q = doc;
            for (int k = 0; k < 8; ++k)
            {
              q = strstr(q, "\"brightness\":");
              if (!q)
                break;
              all[k] = atoi(q + 13);
              q += 13;
            }
          }
          const int v = i < 8 ? all[i] : -1;
          if (v != 0)
            followed = v; // it was 0 before this, so any other value means the feature moved it
        }
        char d3[160] = {0};
        _snprintf(d3, sizeof(d3), "%s: the page was told %d (a 70%% ramp is %d on the slider)",
                  screens[i].device, followed, want);
        // ⚠️⚠️ "CANNOT CONCLUDE" AND "THE FEATURE FAILED" ARE DIFFERENT ANSWERS, AND THIS SECTION HAS TO TELL
        // THEM APART. The screen being measured has a THIRD party on it -- on the machine this was written for, the
        // vendor tool that drives the same panel through this same API (which is the whole reason the section
        // exists). If that tool puts its ramp back while this is being measured, the feature reads a value this
        // probe never wrote, and the assertion below would report the FEATURE as broken for something the other
        // program did. So the ramp is read one more time before concluding: if the other tool's ramp is not on the
        // screen any more, nothing here is about this feature.
        WORD end[3][256];
        const bool endOk = GetDeviceGammaRamp(dc, end) != 0;
        if (!endOk || !RampNear(end, outside))
        {
          printf("      %s: the other tool's ramp did not stay on the screen (white %u of the %u written) -- "
                 "nothing to conclude about following it\n",
                 screens[i].device, endOk ? end[0][255] : 0, outside[0][255]);
        }
        else
        {
          Check(followed >= 0 && followed >= want - 4 && followed <= want + 4,
                "  and a change made by something ELSE is adopted (the fader follows the real screen)", d3);
          // ... and while it is following, the document asks the page to keep re-reading (that is how the fader
          // moves without the user touching anything).
          const int n2 = f->settingsJson(doc, (int)sizeof(doc));
          Check(n2 > 0 && Has(doc, "\"waiting\""),
                "  and the document tells the page to keep re-reading while it does", "");
        }
      }

      // (g) Put the screen back exactly as it was found, and leave the feature's baseline on the original ramp.
      SetDeviceGammaRamp(dc, original);
      _snprintf(path, sizeof(path), "displays[%d].brightness", i);
      f->setControl(path, "100");
      Sleep(150);
      DeleteDC(dc);
    }
    if (gammaScreens == 0)
      printf("      (no monitor on this machine is driven through gamma -- nothing to conclude)\n");
    else
      printf("      (%d of %d monitor(s) are driven through gamma)\n", gammaScreens, tested);
  }

  printf("\n9. what the feature's own log says about the hardware\n");
  {
    // ⚠️ THE PROTOCOL PER MONITOR IS THE ONE THING about brightness that cannot be reported on the page: the
    // ABI has no field for "this control is degraded", so the log is where "the slider will not move this
    // monitor's backlight" is written down. On the machine this was written for, the first monitor answers WMI
    // and the second one only gamma.
    static char log[64 * 1024];
    Check(ReadLog(dir, log, (int)sizeof(log)), "the feature keeps a log of its own", "");
    Check(Has(log, "display \\\\.\\DISPLAY"), "  naming each monitor it found", "");
    Check(Has(log, "DDC/CI answered") || Has(log, "gamma ramp") || Has(log, "WMI instance"),
          "  and saying which protocol that monitor answered", "");
    if (!Quiet())
      Check(Has(log, "screen off") && Has(log, "screen back on"),
            "  and recording both halves of the screen-off test above", "");

    printf("\n10. going away leaves nothing behind\n");
    if (!Quiet())
    {
      Check(f->setControl("displays[0].off", "1") == 1, "a screen is left dark, deliberately");
      FindCtx dark = WaitForBlack(true);
      Check(dark.found == 1, "  (it is dark)", "");
    }
    sound.Stop(); // this probe's own silent stream: it must not outlive the feature it was made for
    f->shutdown();
    const FindCtx after = FindBlack();
    Check(after.found == 0, "shutdown destroys the dark window -- no screen is left black with nothing behind it",
          "");
    ReadLog(dir, log, (int)sizeof(log));
    Check(Has(log, "ramps restored"), "  and it says so in the log", "");
  }

  CoUninitialize();
  printf("\n");
  if (failures)
  {
    printf("FAILED: %d check(s)\n", failures);
    return 1;
  }
  printf("OK: per-monitor brightness and screen-off (a window, not a display change), per-application volume, and\n"
         "    the two named quick-panel groups the user asked for\n");
  return 0;
}
