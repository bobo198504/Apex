// ---------------------------------------------------------------------------
// MediaControl -- THE BRIGHTNESS OF EVERY MONITOR, AND THE VOLUME OF EVERY APPLICATION.
//
// WHAT IT IS FOR, in the user's own words: "多显示器亮度控制，支持多种协议；每个显示器一个拉杆，控制亮度；另外，
// 每个显示器可以熄屏，不是断开，也不要锁屏" and, for the other half, "可以单独每个应用的音量，就是把系统那个音量
// 合成器映射出来，更高效的使用".
//
// ⚠️⚠️ WINDOWS HAS NO SINGLE API FOR EITHER HALF, AND WHICH ONE WORKS IS A FACT ABOUT THE HARDWARE. That is the
// whole shape of this file, so it is worth stating before any code: brightness has THREE unrelated protocols
// (DDC/CI over the video cable, WMI for a laptop's internal panel, and a gamma ramp in the display driver), and
// a given monitor answers exactly one of them -- if any. So each monitor is PROBED once and remembers which
// protocol it answered, and the answer is written to this feature's log, because "the slider does nothing" has
// three completely different causes and only the log tells them apart.
//
// MEASURED ON THE MACHINE THIS WAS WRITTEN FOR (see _diag/media_probe.cpp, which is the program that asked):
//   * 2 monitors, both laptop-internal eDP panels (MONITOR\SDC4190 and MONITOR\BOE0A8D);
//   * DDC/CI: NOT supported by either (VCP 0x10 unreadable, no capabilities string);
//   * WMI root\WMI: ACCESS DENIED to a non-elevated process (PowerShell gets the same answer, so it is the
//     machine and not this code);
//   * gamma ramp: readable and writable on BOTH.
// So on this machine the gamma path is what actually moves the sliders -- and the other two are implemented
// anyway, because they are what an external monitor and a different laptop would use.
//
// ⚠️ "SCREEN OFF" IS A BLACK WINDOW, AND THAT IS NOT A COMPROMISE -- IT IS THE ONLY THING THAT CAN BE DONE PER
// MONITOR. The user asked for "不是断开，也不要锁屏": turning a monitor off for real means either DDC power
// mode 0xD6 (this machine's panels do not answer DDC at all) or the WMI backlight (denied here), and the
// system-wide "monitor off" (SC_MONITORPOWER) turns EVERY screen off and starts the system's own idle timer,
// which is a different feature wearing this one's clothes. A window that covers one monitor with black does
// exactly what was asked: the screen shows nothing, the display stays connected, the desktop does not lock, and
// it is undone by destroying a window. See the note on the off windows below for the recovery paths.
//
// ⚠️ AND GAMMA IS DELIBERATELY NOT PART OF "SCREEN OFF". It was the first design (black window + gamma to zero,
// "so it is really black") and it is pointless: the window is already telling the display to show pixel value
// zero, which is the darkest thing the panel can be told, so scaling the ramp on top of it changes nothing
// visible -- while it WOULD fight the brightness sliders for the same driver state. One mechanism per job.
//
// THE TWO HALVES ARE INDEPENDENT: the brightness half owns display state, the volume half owns WASAPI session
// state, and they share nothing but this file. The volume half follows the DEFAULT OUTPUT DEVICE: the sessions
// are enumerated from whatever endpoint is default right now, so switching sound cards shows that card's
// applications and the per-application volumes Windows keeps for it (the user asked for exactly this:
// "确定要做到切换声卡，它会也跟着切换系统保存的各应用音量").
// ---------------------------------------------------------------------------

#include "../../apex/abi.h"
#include "../../common/match.h"

#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace {

const char *kId = "MediaControl";
const char *kVersion = "1.0.0";

const int kMaxDisplays = 8;
const int kMaxSessions = 32;
const int kMaxHandlesPerSession = 8; // one application may own several sessions; they move together
const int kLogCap = 256 * 1024;

// HOW OFTEN THE SESSION LIST IS RE-ENUMERATED, AT MOST. Enumerating WASAPI sessions means a COM walk of every
// process that has ever opened an audio stream, and this is called from `quickItems` -- which the flyout calls
// ON EVERY FRAME WHILE A FADER IS BEING DRAGGED. Without a floor under it, one drag would enumerate the machine
// sixty times a second. 800 ms is short enough that opening the flyout after starting a new program shows it.
const DWORD kSessionCacheMs = 800;

// HOW OFTEN THE MONITOR LIST IS RE-CHECKED. `EnumDisplayMonitors` is cheap (microseconds); PROBING a monitor is
// not (DDC/CI is a slow serial protocol and one probe can take a tenth of a second), so the probe runs only
// when a monitor appears that was not there before. This is also the hot-plug path: plug a screen in and a row
// appears within two seconds, with no window message to miss.
const DWORD kMonitorPollMs = 2000;

// THE CONTROL THREAD'S CYCLE. It is not a poll of anything expensive -- it is the period at which a request
// made by the settings page becomes real, so it wants to be short enough to feel immediate (it is also
// signalled directly, and this is only the backstop).
const DWORD kIdleTickMs = 200;

// ⚠️ HOW LONG A SCREEN TAKES TO GO DARK OR COME BACK (the user's number: "熄屏和解除熄屏增加0.5秒的过度动画").
// ⚠️ UP HERE RATHER THAN BESIDE THE FADE CODE, because the screen-off window's own message handler uses the timer
// id (see BlackProc), and that function is defined above the fade itself.
const DWORD kBlackFadeMs = 500;
// The frame period while a fade is running -- 15 ms, the same as the flyout's own fade: smooth to the eye and cheap
// enough to run for half a second.
const UINT_PTR kBlackFadeTimer = 4;
const int kBlackFadeStepMs = 15;

// ⚠️ HOW OFTEN A MONITOR THAT ENDED UP ON THE GAMMA FALLBACK IS PROBED AGAIN. A failed probe is not a verdict
// (see EnumerateDisplays): a screen that has just been switched on has not finished coming up, and DDC/CI is a
// slow serial protocol that answers nothing until it has. Fifteen seconds is short enough that plugging a
// screen in and touching the slider "just works", and long enough that the walk costs nothing.
const DWORD kReprobeMs = 15000;

// ---- asking the screen what it is really at (see PollHardware) ---------------------------------
//
// ⚠️ `kWriteSettleMs` IS A PANEL'S OWN LATENCY, and it is why a read-back cannot happen immediately: a DDC/CI
// write goes out on a slow serial wire and the panel then acts. Reading before that would call a good write
// "ignored" and send the display off to be re-probed for no reason.
const DWORD kWriteSettleMs = 900;
// How often every screen is asked. Two seconds is a compromise between "the slider follows the laptop's
// brightness keys while the user is pressing them" and "a DDC/CI read every second on every screen".
const DWORD kPollHardwareMs = 2000;
// ... and how often while it is actively following somebody else (see PollHardware). ⚠️ 400 -> 200 AFTER THE USER'S
// REPORT: "SDC4190的亮度控制跟系统控制同步有延迟，没法做到实时同步，这个有改进的可能？" -- they are right that it
// can be improved, and the improvement is this number rather than anything clever: the settings page re-reads
// every 200 ms while it is told to (see the panel's `waiting` poll), so the FEATURE has to look at the hardware at
// least as often or the user sees two different rhythms. What is left is the panel's own round trip plus however
// long the display driver takes to publish the new value through WMI -- see the note in features.md for what that
// floor is.
const DWORD kPollHardwareFastMs = 200;
// How long the page is asked to keep re-reading after the screen was moved by something other than this feature.
const DWORD kExternalFollowMs = 8000;

// ---- the two halves' colours -------------------------------------------------------------------
//
// The panel's sliders carry their own `hue` (see `range` in abi.h) so that a slider and the thing it controls
// are one colour. These two numbers exist ONCE, in the feature that knows what they are for, and are sent to
// both the settings page and the flyout -- the flyout would otherwise draw them in the host's accent and the
// two surfaces would disagree about which half is which.
const unsigned kHueBrightness = 0xFFC24Bu; // warm: light
const unsigned kHueVolume = 0x78BEFFu;     // the panel's own accent blue: sound

// ⚠️ WINDOWS REFUSES A GAMMA RAMP WHOSE WHITE POINT IS BELOW HALF OF FULL SCALE (measured with
// `_diag/gamma_set_probe.cpp`: 50% accepted at white 32768, 48% refused, 10% refused). Half of full scale is
// 32767.5; a little above it, so that the edge cannot bite. It is a file-scope constant because THREE places
// need the same number -- the write (ApplyGamma), the read-back (ReadBackPercent) and therefore the mapping
// between them, and a second copy of it is how a slider and its own read-back would come to disagree.
const double kGammaWhiteFloor = 33000.0;

// ---- logging -----------------------------------------------------------------------------------

FILE *g_log = nullptr;
const ApexHost *g_host = nullptr;
char g_dir[512] = {0};

void Log(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[300] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_host->logLine(buf);
}

// THIS FEATURE'S OWN LOG, beside its settings. It exists for one question that has no other answer: WHICH
// PROTOCOL DID EACH MONITOR ANSWER? The slider is on a page, the monitor is on a desk, and "nothing happened"
// looks identical whether the monitor has no DDC, the process is not allowed to ask WMI, or the gamma write was
// refused. The feature cannot report that on the page (the ABI has no field for "this control is degraded"), so
// it is written down here, once per monitor, with the numbers that were read back.
void OpenLog()
{
  if (!g_dir[0])
    return;
  char path[560] = {0};
  if (_snprintf(path, sizeof(path), "%smediacontrol.log", g_dir) <= 0)
    return;
  g_log = fopen(path, "w");
}

void Stamp(char *out, int outSize)
{
  SYSTEMTIME st;
  GetLocalTime(&st);
  _snprintf(out, outSize, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// A LINE WITH A TIMESTAMP. Rewritten each run ("w") like the other feature's log, and written only when
// something CHANGES -- a log that grows while nothing happens hides the transition a reader is looking for.
void LogEvent(const char *fmt, ...)
{
  if (!g_log)
    return;
  char ts[32] = {0};
  Stamp(ts, (int)sizeof(ts));
  fprintf(g_log, "%s  ", ts);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fprintf(g_log, "\n");
  if (ftell(g_log) > kLogCap)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  else
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// THE SETTINGS, AND WHAT EACH MONITOR TURNED OUT TO BE
// ---------------------------------------------------------------------------------------------

// WHICH PROTOCOL A MONITOR ANSWERED. Probed once per monitor, remembered, and logged.
enum Protocol
{
  kProtoGamma = 0, // the fallback that always works: scale the display driver's gamma ramp
  kProtoDdc,       // DDC/CI over the video cable -- a real backlight change, external monitors
  kProtoWmi        // the laptop's own panel brightness, through root\WMI
};

struct Display
{
  // ---- identity: what survives a re-enumeration ----
  char device[32] = {0}; // "\\.\DISPLAY1" -- a SLOT, used only to talk to Windows, never to remember anything
  char edid[32] = {0};   // "SDC4190", pulled out of MONITOR\SDC4190\... -- how WMI is matched to a monitor
  // ⚠️⚠️ THE MONITOR'S OWN IDENTITY, AND THE SETTINGS FILE IS KEYED ON THIS RATHER THAN ON `device`.
  // The user's request, and the log of this very machine is the argument: "显示器身份不用\\\\.\\DISPLAY，而是用显示器
  // 自身的ID，\\\\.\\DISPLAY在一些情况下会变动". It does -- the same HKC screen appeared as `\\.\DISPLAY1` at
  // 00:03 and as `\\.\DISPLAY2` at 00:12, and a settings file keyed on that would have moved the user's brightness,
  // name and shortcut onto a different screen (or onto nothing) for no reason they could see. The identity comes
  // from `WmiMonitorID` (manufacturer + product code + serial number), which is the panel telling us what it is.
  // ⚠️ When it cannot be read, or when a panel reports no serial number, it falls back to the EDID name -- weaker
  // (two monitors of one model look alike) but still better than a slot number, and the file keeps its `device`
  // column so nothing is lost.
  char identity[96] = {0};
  char name[96] = {0};   // what the user reads ("显示器 1 · 2880x1800")
  HMONITOR hmon = nullptr;
  RECT rc = {0, 0, 0, 0};
  bool primary = false;

  // ---- how it is driven ----
  int protocol = kProtoGamma;
  bool probed = false;    // the slow probe has run for this monitor
  DWORD probedAt = 0;     // ... and when (a gamma-fallback monitor is probed again, see kReprobeMs)
  bool storedApplied = false; // the settings file's line for this monitor has been taken (see ApplyStored)
  // ⚠️ NO PHYSICAL-MONITOR HANDLE IS HELD HERE, ON PURPOSE -- one is taken for each DDC/CI call and released
  // immediately (see OpenPhysical). Holding one for the life of the process is what made the user's external
  // screen refuse to move when Twinkle Tray could move it perfectly.
  DWORD ddcMax = 100;     // its own full-scale value (0x10), which is NOT always 100
  char wmiInstance[256] = {0};
  bool wmiOk = false;
  HDC gammaDc = nullptr;              // a DC for this device, for the gamma ramp
  // ⚠️ TWO RAMPS, AND THEY ARE NOT THE SAME ONE (see ApplyGamma):
  //   * `gammaOriginal` is what was on the screen when this feature first looked -- restored on the way out, byte
  //     for byte, because that is the user's own state and this feature is a guest;
  //   * `gammaBase` is that ramp with its WHITE POINT NORMALISED TO FULL SCALE, which is the "100%" every
  //     percentage is measured from. Without the normalisation the slider's own scale would depend on whatever
  //     another dimming tool had left behind: measured on this machine, a vendor tool was holding the screen at
  //     92%, so this feature's 100% was 92% and its 0% was 46% -- two tools multiplying again, which is exactly
  //     what the user asked to be rid of ("能做成统一控制?").
  WORD gammaOriginal[3][256] = {{0}};
  WORD gammaBase[3][256] = {{0}};
  bool gammaBaseOk = false;
  // ⚠️ WHAT THIS FEATURE LAST WROTE, so it can tell its own work from somebody else's. A gamma ramp is ONE
  // piece of state per display that every tool on the machine shares, and the two questions that matter --
  // "is the current ramp ours?" and "has anything changed it since?" -- can only be answered by remembering
  // the bytes that were sent.
  WORD gammaWritten[3][256] = {{0}};
  bool gammaWrote = false;

  // ---- the user's settings for it ----
  int level = 100;        // 0..100, what the slider says
  int appliedLevel = -1;  // what the protocol was last told (so an unchanged value costs nothing)
  // ⚠️ WHAT THE USER CALLS IT (the user's own request: "显示器或音量的应用名，可以自定义名字，显示到快速面板").
  // Empty means "no opinion", and then the device's own name is used. It travels to BOTH surfaces -- the page's
  // row and the flyout's label -- because a name that only appears in one of them is a name the user has to
  // re-learn in the other.
  char alias[64] = {0};
  // ⚠️⚠️ THERE WAS A SWITCH HERE, AND IT WAS TAKEN OUT AGAIN -- WORTH REMEMBERING, BECAUSE THE REASON IS THE
  // SAME ONE THAT PUT IT IN. A screen Apex can only dim by changing the signal (the gamma fallback) shares one
  // colour ramp with any vendor tool dimming the same panel, and on this machine ASUS ScreenXpert drives the
  // second panel's backlight through a private channel that is neither readable nor writable from here -- so the
  // two dimmings MULTIPLY ("两个都调到最暗近乎看不见"), and nothing in this process can tell that it is happening.
  // The answer was a per-screen switch, and the user's verdict on living with it: "软件调光开关可以去了，这个没什么
  // 意义。默认打开就是了。现在这样也能接受。" A switch that has to be explained is worth less than the behaviour it
  // guards, and its default was "on" anyway -- so "on" is now the only behaviour: this feature dims every screen it
  // can, and the sharing is handled by the baseline normalisation in ApplyGamma (see the long note there: the two
  // tools overwrite each other instead of compounding).


  // ---- does the screen ACTUALLY do what it was told? (see PollHardware) ----
  //
  // ⚠️ THIS IS THE ANSWER TO "the slider moves and nothing happens", AND IT COSTS THREE NUMBERS. A write that
  // SUCCEEDS is not a write that was OBEYED: `SetVCPFeature` returns as soon as the command is on the wire
  // (DDC/CI has no acknowledgement) and a panel is free to ignore it -- measured on this machine, one external
  // screen obeys and another only pretends to. So the brightness is read BACK, and the answer is used for two
  // different things depending on when it changed:
  //   * right after a write, a value that did not arrive means the command was ignored  -> re-probe the panel;
  //   * long after one, a value that differs means SOMEBODY ELSE moved it (the monitor's own buttons, the
  //     laptop's brightness keys, a vendor tool) -> adopt it, so the slider follows the real screen.
  DWORD wroteAt = 0;       // when this feature last wrote (a read-back must wait for the panel to act)
  DWORD polledAt = 0;      // when the brightness was last read back
  // ⚠️⚠️ THE ONE BEFORE LAST, AND IT IS WHAT TELLS "IGNORED" APART FROM "SOMEBODY ELSE" -- WITHOUT A CLOCK.
  // Both failures look identical from outside ("the screen is not what I asked for"), and the first version
  // separated them by time ("a disagreement within six seconds is mine"), which is wrong in the user's own case:
  // dragging the fader and then pressing the laptop's brightness key lands inside any such window. The values
  // themselves say it instead: a panel that IGNORES a command stays at the value it had -- which is the one this
  // feature wrote last time -- while a screen somebody else moved is at a value this feature never wrote.
  int prevApplied = -1;
  DWORD externalUntil = 0; // while in the future, the page is told to keep re-reading (see SettingsJson)
  bool off = false;       // the user asked for this screen to be dark
  char hotkey[64] = {0};  // the global shortcut that toggles `off`, EMPTY BY DEFAULT (as asked for)
  int hotkeyId = 0;       // RegisterHotKey's id while it is registered, 0 when it is not
  unsigned hotkeyMods = 0;
  unsigned hotkeyVk = 0;

  // ---- the screen-off window ----
  HWND black = nullptr;
  bool blackUp = false;
  // ⚠️⚠️ AND IT FADES, BECAUSE THE USER ASKED FOR IT: "熄屏和解除熄屏增加0.5秒的过度动画". A screen that goes black
  // in one frame is a screen that looks like it crashed; half a second of it going dark reads as the screen
  // closing its eye, and the same on the way back. The window is therefore LAYERED (`WS_EX_LAYERED`) and its alpha
  // is driven per frame -- see StartBlackFade.
  //
  // ⚠️ THE ALPHA IS COMPUTED FROM THE CLOCK, NOT ACCUMULATED PER TICK. A timer that adds a fixed step shows every
  // hitch in the machine as a stutter in the fade; "how far through the fade are we" is one subtraction from
  // `fadeStart`, and it lands on the right value even if three ticks were missed.
  double blackAlpha = 0.0;  // 0 = invisible, 1 = solid black
  double blackTarget = 0.0; // where the fade is going
  double blackFrom = 0.0;   // where it started
  DWORD fadeStart = 0;
  bool fading = false;
  // ⚠️⚠️ AND THERE IS NO "REAL" SCREEN-OFF ANY MORE, WHICH IS ALSO A REVERSAL AND ALSO THE USER'S CALL. DDC/CI
  // VCP 0xD6 does turn a panel's own backlight off while the display stays connected -- and it was offered as a
  // per-machine switch because of that. Measured on the user's own external screen, though, standby drops the
  // display link far enough for Windows to see a hot-plug: "HKC0000熄屏方案不对，会熄屏，但有点像断开了，然后马上又连
  // 回来". That is the one thing the requirement rules out ("不是断开"), and a switch whose "on" position is a brief
  // disconnect is a switch that can break the requirement by accident. The user's decision: "熄屏用显示器电源这个方
  // 案也去掉，这样会变成断开显示器。统一采用现在的方案." So the black window is not a fallback any more -- it is the
  // only way this feature turns a screen off, on every screen.

  // ---- why the last write failed, for the log (never shown on the page: see OpenLog) ----
  char note[160] = {0};
};

struct Session
{
  char key[128] = {0};  // the match key: the lower-case process name, or "" for the system-sounds session
  char name[96] = {0};  // what the user reads ("chrome.exe" without the extension, or 系统声音)
  bool system = false;  // the pid-0 session, which is pinned to the top like Windows' own mixer does
  // ⚠️ THE USER'S OWN NAME FOR IT, empty when they have not given one (see the same field on Display). Kept by
  // `key` in the settings file, because a session id changes every time the program restarts and a process name
  // does not.
  char alias[64] = {0};
  float volume = 1.0f;
  bool mute = false;
  ISimpleAudioVolume *vol[kMaxHandlesPerSession] = {nullptr};
  int volCount = 0;
};

// ---- the shared state --------------------------------------------------------------------------
//
// ⚠️ TWO THREADS TOUCH THIS, AND THEY ARE NOT THE SAME TWO AS IN THE OTHER FEATURES:
//   * the HOST'S UI THREAD -- settingsJson, setControl, listOp, quickItems, init and shutdown all arrive
//     there, and that is where the session list (WASAPI) is enumerated;
//   * this feature's OWN CONTROL THREAD -- which owns the off windows, the hotkeys and the actual brightness
//     writes, because DDC/CI and WMI are SLOW (a DDC write can take a tenth of a second) and doing that on the
//     page's thread would make the slider stutter.
// So every field that both sides read is read or written under `g_lock`, and the slow work is a REQUEST: the UI
// thread changes a number, signals `g_wake`, and the control thread does the talking to Windows.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

Display g_disp[kMaxDisplays];
int g_dispCount = 0;
bool g_redetect = true; // the control thread re-enumerates the monitors before its next pass

Session g_sess[kMaxSessions];
int g_sessCount = 0;
DWORD g_sessStamp = 0; // when the session list was last enumerated (see kSessionCacheMs)

// ⚠️ THE USER'S NAMES FOR APPLICATIONS, AS READ FROM THE FILE, KEPT UNTIL THE PROGRAM THEY NAME IS SEEN.
// A name is stored against a PROGRAM ("chrome.exe"), and the file may be read before that program has an audio
// session -- so a name read now has to survive until the row it belongs to appears. Cleared only by being read
// again (the file is re-read on reload), never by time.
char g_pendingAliasKey[kMaxSessions][128] = {{0}};
char g_pendingAliasName[kMaxSessions][64] = {{0}};
int g_pendingAliasCount = 0;

// ⚠️⚠️ WHAT THE FILE SAID ABOUT EACH MONITOR, HELD UNTIL THAT MONITOR IS RECOGNISED -- AND NOW KEYED ON THE
// MONITOR'S OWN IDENTITY RATHER THAN ON `\\.\DISPLAY1` (see `Display::identity`).
//
// ⚠️ IT IS ALSO WHY THIS IS A TABLE AND NOT AN IMMEDIATE ASSIGNMENT. The file is read when Apex starts, but a
// display's identity can only be read AFTER it has been probed (`WmiMonitorID` through WMI), and a monitor that
// is plugged in later has no identity at all at that moment. So the file is parsed into this list, and each
// monitor takes what is its own as soon as it knows what it is -- which also fixes the older bug where a screen
// plugged in after startup never picked up the brightness the user had set for it.
struct StoredDisplay
{
  char identity[96];
  char device[32];
  int level;
  char hotkey[64];
  char alias[64];
};
StoredDisplay g_stored[kMaxDisplays];
int g_storedCount = 0;

// Which of the two groups the user put in the quick panel. Both default OFF: the user's rule is that a control
// reaches the flyout because they said so ("插件自己的控件要明确有开关映射到快速面板，才给").
bool g_quickBrightness = false;
bool g_quickVolume = false;

HANDLE g_wake = nullptr;   // signalled by the UI thread when there is something to do
HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;
volatile LONG g_offCount = 0; // how many screens are dark right now -- what the tray mark reads (see Flags)

// WMI is set up ONCE, on the control thread (WMI calls belong to the thread that initialised COM), and the
// answer -- including "this process may not ask" -- is remembered: a failed WMI query is not retried on every
// brightness change, because it is not going to start working.
IWbemServices *g_wmi = nullptr;
bool g_wmiTried = false;
bool g_wmiUsable = false;

// The off window's class. Registered once, on the control thread, which is also the thread that owns the
// windows -- a window belongs to the thread that created it, and its messages are only delivered when THAT
// thread pumps. That is why the control thread has a message loop at all.
const char *kBlackClass = "ApexMediaControlOff";
bool g_blackClassReady = false;

// ---------------------------------------------------------------------------------------------
// BRIGHTNESS -- three protocols behind one call
// ---------------------------------------------------------------------------------------------

// ⚠️ THE NAME A ROW IS SHOWN UNDER: the user's own name for it when they gave one, and the thing's own name
// otherwise. One function, because three surfaces draw it (the page's rows, the flyout's labels, the log) and a
// rule that differs between them is a rule the user has to learn twice.
void DisplayLabel(const Display &d, char *out, int outSize)
{
  _snprintf(out, outSize, "%s", d.alias[0] ? d.alias : d.name);
}

// The same for an application. ⚠️ AND THE SYSTEM-SOUNDS ROW IS THE ONE WHOSE DEFAULT NAME IS THIS FEATURE'S OWN
// WORDS RATHER THAN A PROGRAM'S, so it comes in both languages -- the user found the inconsistency: the settings
// page said "系统声音" while the flyout said "System sounds", because the flyout was handed the English string
// for both languages.
//
// ⚠️ THE TWO STRINGS ARE DECLARED HERE RATHER THAN WITH THE SESSION CODE FURTHER DOWN, because three surfaces
// draw this row's name and the first of them (this function) is above them all.
const char *kSystemSoundsZh = "系统声音";
const char *kSystemSoundsEn = "System sounds";

// ⚠️ DECLARED HERE BECAUSE THE FUNCTION BELOW IS THE FIRST USER OF IT (its definition sits with the WMI code,
// further down): the EDID names compared there are ASCII by construction.
void UpperAscii(char *s);

// ⚠️⚠️ AND WHAT THE PANEL CALLS ITSELF IS ASKED OF THE MONITOR (see `Display::identity`). `WmiMonitorID` carries
// the manufacturer, the product code and the serial number as arrays of UTF-16 units, and this turns them into one
// string. ⚠️ A serial number of all zeros is what a great many panels report, and it is treated as ABSENT: an
// identity of `HKC-0000-00000000` shared by two screens would be worse than the honest shorter one, because it
// would look unique and not be.
bool WmiMonitorIdentity(const char *edid, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;
  if (!g_wmiUsable || !g_wmi || !edid || !edid[0])
    return false;

  IEnumWbemClassObject *en = nullptr;
  BSTR lang = SysAllocString(L"WQL");
  BSTR q = SysAllocString(L"SELECT * FROM WmiMonitorID");
  const HRESULT hr = g_wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                                      &en);
  SysFreeString(lang);
  SysFreeString(q);
  if (FAILED(hr) || !en)
    return false;

  bool found = false;
  char upperEdid[64] = {0};
  _snprintf(upperEdid, sizeof(upperEdid), "%s", edid);
  UpperAscii(upperEdid);

  for (;;)
  {
    IWbemClassObject *obj = nullptr;
    ULONG got = 0;
    if (en->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0 || !obj)
      break;
    VARIANT v;
    VariantInit(&v);
    if (obj->Get(L"InstanceName", 0, &v, nullptr, nullptr) == S_OK && v.vt == VT_BSTR)
    {
      char inst[512] = {0};
      WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, inst, (int)sizeof(inst) - 1, nullptr, nullptr);
      char upperInst[512] = {0};
      _snprintf(upperInst, sizeof(upperInst), "%s", inst);
      UpperAscii(upperInst);
      if (strstr(upperInst, upperEdid))
      {
        const wchar_t *fields[3] = {L"ManufacturerName", L"ProductCodeID", L"SerialNumberID"};
        char part[3][64] = {{0}};
        for (int k = 0; k < 3; ++k)
        {
          VARIANT av;
          VariantInit(&av);
          if (obj->Get(fields[k], 0, &av, nullptr, nullptr) == S_OK && (av.vt & VT_ARRAY))
          {
            LONG lo = 0, hi = -1;
            int n = 0;
            if (SafeArrayGetLBound(av.parray, 1, &lo) == S_OK && SafeArrayGetUBound(av.parray, 1, &hi) == S_OK)
              for (LONG j = lo; j <= hi && n < (int)sizeof(part[0]) - 1; ++j)
              {
                LONG val = 0;
                if (SafeArrayGetElement(av.parray, &j, &val) == S_OK && val > 0 && val < 128)
                  part[k][n++] = (char)val;
              }
            VariantClear(&av);
          }
        }
        bool serial = part[2][0] != 0;
        if (serial)
        {
          bool allZero = true;
          for (const char *p = part[2]; *p; ++p)
            if (*p != '0')
              allZero = false;
          if (allZero)
            serial = false;
        }
        if (part[0][0] && part[1][0])
          _snprintf(out, outSize, "%s-%s%s%s", part[0], part[1], serial ? "-" : "", serial ? part[2] : "");
        else
          _snprintf(out, outSize, "%s", edid); // the panel would not name itself: the EDID name is the fallback
        found = out[0] != 0;
      }
    }
    VariantClear(&v);
    obj->Release();
    if (found)
      break;
  }
  en->Release();
  return found;
}

// ⚠️ AND TWO FORWARD DECLARATIONS, BOTH BECAUSE "DID MY WRITE ARRIVE?" IS ASKED OF EVERY PROTOCOL IN ONE PLACE
// (see ReadBackPercent and PollHardware) -- that place sits above the WMI section, and a read-back that cannot
// reach the WMI helper could not answer for the one protocol that has a real brightness to read.
void WmiMatchDisplay(Display &d);
bool WmiFindInstance(const wchar_t *className, const char *edid, const wchar_t *numberProp, char *keyOut,
                     int keyCap, char *pathOut, int pathCap, long *numberOut, int *seen);
// ... plus the two things that have to exist before the monitor walk uses them: the ASCII upper-caser the WMI
// name matching is built on, and the per-monitor settings lookup (both are defined further down).
void ApplyStored(Display &d);

void SessionLabel(const Session &s, bool zh, char *out, int outSize)
{
  if (s.alias[0])
    _snprintf(out, outSize, "%s", s.alias);
  else if (s.system)
    _snprintf(out, outSize, "%s", zh ? kSystemSoundsZh : kSystemSoundsEn);
  else
    _snprintf(out, outSize, "%s", s.name);
}

// THE GAMMA RAMP, WHICH IS THE FALLBACK THAT ALWAYS EXISTS.
//
// ⚠️ IT IS NOT A BACKLIGHT CHANGE and the log says so: it scales what the display driver sends to the panel, so
// the backlight burns exactly as much as before. What it does give is a working slider on a monitor that
// answers nothing else -- which, measured on this machine, is the second screen it has.
//
// ⚠️⚠️ AND IT IS *SHARED STATE*, WHICH IS THE WHOLE DIFFICULTY. `SetDeviceGammaRamp` writes ONE ramp per
// channel per display: there are no layers, no owners and no arbitration, so two programs that both dim a
// screen simply OVERWRITE each other. The second screen on the machine this was written for has a vendor tool
// that dims it the same way, and the user's report was that the two together "look additive" -- which they were,
// and the reason was this feature's fault:
//
//   * THE BASELINE WAS READ ONCE, AT STARTUP. If the vendor tool had already dimmed the screen to 60%, this
//     feature wrote it down as 100%, and its own 50% then delivered 30%. The user's own words: "现在两个调起来
//     像是叠加的效果，是各调各的吗？"
//   * AND IT WROTE THAT REMEMBERED BASELINE BACK on the way out, which threw the vendor tool's newer setting
//     away -- the other direction of the same mistake.
//
// ⚠️ SO THE BASELINE IS RE-CALIBRATED EVERY TIME, AND "WHOSE RAMP IS THIS?" IS ANSWERED BY MEMORY: the bytes
// this feature last wrote are kept, the ramp is read back before every change, and if what is there is not
// what was written, then SOMETHING ELSE OWNS THE SCREEN NOW -- and that state becomes the new 100%. The two
// tools' effects still multiply (they are two multipliers on one number; nothing can change that), but neither
// eats the other's setting any more, and the percentage on the slider always means "of what the screen is at
// now" rather than "of what it happened to be when Apex started".
//
// ⚠️ THE COMPARISON IS FUZZY ON PURPOSE. Some drivers store the ramp at 8 bits per channel, so a ramp read back
// is not bit-for-bit what was written -- a strict comparison would decide "somebody else changed it" after
// EVERY write, and every nudge of the slider would multiply the previous one (the screen would get steadily
// darker as the user dragged). Half of one 8-bit step is the tolerance, which is far below any real brightness
// change (1% of full scale is 655) and far above the quantisation.
bool RampNear(const WORD a[3][256], const WORD b[3][256])
{
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
    {
      const int diff = (int)a[c][i] - (int)b[c][i];
      if (diff > 512 || diff < -512)
        return false;
    }
  return true;
}

// Put the ramp back ONLY IF IT IS STILL OURS. If another tool has changed it since, that is ITS setting and this
// feature has no business overwriting it -- doing so is exactly the "the driver's brightness got eaten" half of
// the user's complaint. Used on the way out and when a monitor disappears.
void RestoreGammaIfOurs(Display &d)
{
  if (!d.gammaDc || !d.gammaBaseOk || !d.gammaWrote)
    return;
  WORD cur[3][256];
  if (!GetDeviceGammaRamp(d.gammaDc, cur) || !RampNear(cur, d.gammaWritten))
  {
    LogEvent("display %s: the ramp is not ours any more -- leaving it alone (another tool is driving this "
             "screen)",
             d.device);
    d.gammaWrote = false;
    return;
  }
  // ⚠️ THE **ORIGINAL** RAMP GOES BACK, NOT THE NORMALISED ONE. `gammaBase` is a working value this feature
  // derived (white point stretched to full scale); putting it back on exit would leave the screen BRIGHTER than
  // the user had it. What goes back is the ramp that was found here, byte for byte.
  SetDeviceGammaRamp(d.gammaDc, d.gammaOriginal);
  d.gammaWrote = false;
}

void ApplyGamma(Display &d)
{
  if (!d.gammaDc)
    return;

  // ⚠️⚠️ THE BASELINE IS READ ONCE AND THEN NEVER AGAIN, WHICH IS A REVERSAL OF THE PREVIOUS VERSION AND THE
  // USER'S OWN DECISION. The earlier rule was "read the ramp before every change, and if it is not ours, that
  // state becomes the new 100%" -- which stopped the two tools from eating each other's settings, and made their
  // effects MULTIPLY: with a vendor tool at its darkest and this feature at 0%, the screen ended up at
  // 0.5 x 0.33 = 17% ("两个都调到最暗近乎看不见"). The user's words for what they want instead: "能做成统一控制?"
  //
  // So: the ramp as it was when this feature first saw the display IS 100%, every write is that baseline times
  // the percentage, and the two tools therefore OVERWRITE each other (last one wins) rather than compounding.
  // That is a real trade and it is the one the user chose: the slider means an absolute brightness, and moving
  // it takes the screen there -- at the cost of walking over whatever the other tool had set.
  //
  // ⚠️ AND THE BASELINE IS NOT AN IDENTITY RAMP: a display with a colour profile loaded (or a night-light filter
  // running) already has a ramp of its own, and replacing it with `i * 257` would throw that away. Read once,
  // kept, and every level is a scaling OF IT -- which is also what lets `RestoreGammaIfOurs` put it back.
  if (!d.gammaBaseOk)
  {
    WORD cur[3][256];
    if (!GetDeviceGammaRamp(d.gammaDc, cur))
      return;
    memcpy(d.gammaOriginal, cur, sizeof(cur));
    memcpy(d.gammaBase, cur, sizeof(cur));
    // ⚠️⚠️ NORMALISE THE WHITE POINT TO FULL SCALE, AND THIS IS THE "统一控制" THE USER ASKED FOR. A ramp read
    // from a screen that another tool has already dimmed is a ramp whose maximum is BELOW full scale -- so a
    // percentage of it is a percentage of somebody else's setting, and the two tools compound again no matter how
    // carefully this feature remembers its own baseline. Scaling the curve so its white point is 65535 makes the
    // slider an ABSOLUTE scale: 100% is the brightest the panel can be, 0% is the API's floor, and moving the
    // slider REPLACES whatever was there rather than multiplying into it.
    // ⚠️ The SHAPE of the curve is kept (a colour profile's tone curve survives; only its amplitude is
    // stretched), and `gammaOriginal` is what goes back on the way out.
    if (d.gammaBase[0][255] > 0 && d.gammaBase[0][255] < 65535)
    {
      const double s = 65535.0 / (double)d.gammaBase[0][255];
      for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i)
        {
          int v = (int)((double)d.gammaBase[c][i] * s + 0.5);
          d.gammaBase[c][i] = (WORD)(v > 65535 ? 65535 : v);
        }
      LogEvent("display %s: the ramp on this screen had its white point at %.1f%% (something else had dimmed "
               "it) -- normalising it to full scale so the slider is an absolute brightness",
               d.device, 100.0 * (double)d.gammaOriginal[0][255] / 65535.0);
    }
    d.gammaBaseOk = true;
  }

  // ---- 2. write ours ----
  //
  // ⚠️⚠️ WINDOWS REFUSES A RAMP WHOSE WHITE POINT FALLS BELOW HALF OF FULL SCALE, SO THE SLIDER'S TRAVEL IS
  // MAPPED INTO WHAT THE API WILL ACTUALLY ACCEPT. Measured on this machine with `_diag/gamma_set_probe.cpp`:
  //
  //     90% / 80% / 60% accepted ... 50% accepted (white = 32768) ... 48% REFUSED ... 10% REFUSED
  //
  // That is a security floor in the display stack (a program must not be able to black the screen out through
  // gamma), and there is no way round it -- `SetDeviceGammaRamp` just returns FALSE, with no error code and no
  // message. The first version of this feature ignored that and asked for `level%` of the baseline directly, so
  // once ANY other tool had dimmed the screen (which multiplies with ours), the lower half of the slider became
  // a control that moved and did nothing visible. That is what the user was describing:
  //
  //     "现在两个调起来像是叠加的效果，是各调各的吗？"
  //
  // So: the floor is turned into the BOTTOM OF THE SLIDER rather than into a broken region of it. 0 means "as
  // dark as Windows will let this screen go given what is already on it", 100 means "the baseline, untouched",
  // and everything between is real. The mapping is relative to the baseline's own white point, because the
  // floor is an absolute number while the baseline is whatever the other tool left behind -- with the screen
  // already at 60%, the slider's usable travel is the top 17% of it, and it says so in the log instead of
  // pretending otherwise.
  const double baseWhite = d.gammaBase[0][255] > 0 ? (double)d.gammaBase[0][255] : 65535.0;
  double kMin = kGammaWhiteFloor / baseWhite;
  if (kMin > 1.0)
    kMin = 1.0;
  if (kMin < 0.0)
    kMin = 0.0;
  if (kMin >= 1.0 && d.level < 100)
  {
    // Nothing left to give: another tool has already pushed this screen to the floor. Said once, in the log,
    // because the slider moving with no effect is otherwise indistinguishable from a broken feature.
    static bool told = false;
    if (!told)
    {
      LogEvent("display %s: this screen is already at the darkest value Windows allows through a gamma ramp "
               "-- the slider has no room left below it",
               d.device);
      told = true;
    }
  }
  const double k = kMin + (1.0 - kMin) * ((double)d.level / 100.0);

  WORD ramp[3][256];
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
      ramp[c][i] = (WORD)(d.gammaBase[c][i] * k + 0.5);
  if (!SetDeviceGammaRamp(d.gammaDc, ramp))
  {
    _snprintf(d.note, sizeof(d.note), "SetDeviceGammaRamp refused");
    LogEvent("display %s: SetDeviceGammaRamp REFUSED at level %d (k=%.3f of a baseline whose white is %.0f) -- "
             "asking for a fresh protocol probe, because a screen that refuses this usually has a better path",
             d.device, d.level, k, baseWhite);
    // ⚠️ A REFUSAL IS EVIDENCE, NOT JUST A FAILURE. Windows refuses this for a screen that is being driven some
    // other way (or not at all), and on the machine this was written for the refusal appeared on an external
    // monitor that DDC/CI answers perfectly -- the probe had simply run before the screen was ready. So the
    // monitor goes back into the probe queue rather than sitting on a path that does not work.
    d.probed = false;
    d.probedAt = 0;
    if (g_wake)
      SetEvent(g_wake);
    return;
  }
  memcpy(d.gammaWritten, ramp, sizeof(ramp));
  d.gammaWrote = true;
}

// ⚠️⚠️ A DDC/CI HANDLE IS TAKEN FOR THE ONE CALL THAT NEEDS IT, AND RELEASED IMMEDIATELY -- IT IS NOT KEPT.
//
// The first version took one handle at probe time and held it for the life of the process, which is the obvious
// thing to do and is what broke the user's external screen: "HKC亮度调整还是没生效，之前用过软件twinkletray，是可以调
// 的". Everything about that screen checks out (`_diag/ddc_set_probe.cpp` writes 89 and reads 89 back, and so does
// Twinkle Tray), so the handle itself is the suspect -- `GetPhysicalMonitorsFromHMONITOR` answers for a display as
// it was at that moment, and a handle kept across a monitor being re-detected, a mode change or a sleep is a
// handle whose target may no longer exist. The cost of taking a fresh one is one enumeration per write, and a
// write already costs tens of milliseconds on the wire.
//
// ⚠️ The caller owns the handle and must call `DestroyPhysicalMonitor` on it.
bool OpenPhysical(Display &d, HANDLE *out)
{
  if (out)
    *out = nullptr;
  if (!d.hmon || !out)
    return false;
  DWORD n = 0;
  if (!GetNumberOfPhysicalMonitorsFromHMONITOR(d.hmon, &n) || n == 0)
    return false;
  PHYSICAL_MONITOR pm[8];
  const DWORD take = n < 8 ? n : 8;
  if (!GetPhysicalMonitorsFromHMONITOR(d.hmon, take, pm))
    return false;
  for (DWORD i = 1; i < take; ++i)
    DestroyPhysicalMonitor(pm[i].hPhysicalMonitor); // everything but the one being handed over
  *out = pm[0].hPhysicalMonitor;
  return true;
}

void ApplyDdc(Display &d)
{
  HANDLE phys = nullptr;
  if (!OpenPhysical(d, &phys) || !phys)
  {
    _snprintf(d.note, sizeof(d.note), "no physical monitor handle");
    LogEvent("display %s: could not open a DDC/CI handle -- falling back to gamma", d.device);
    d.protocol = kProtoGamma;
    ApplyGamma(d);
    return;
  }
  const DWORD v = (DWORD)((double)d.ddcMax * d.level / 100.0 + 0.5);
  const BOOL ok = SetVCPFeature(phys, 0x10, v);
  DestroyPhysicalMonitor(phys);
  if (!ok)
  {
    _snprintf(d.note, sizeof(d.note), "SetVCPFeature(0x10) refused");
    LogEvent("display %s: DDC SetVCPFeature(0x10, %lu) FAILED -- falling back to gamma", d.device,
             (unsigned long)v);
    d.protocol = kProtoGamma;
    ApplyGamma(d);
    return;
  }
}

// THE INTERNAL PANEL, THROUGH WMI. `WmiSetBrightness(Timeout, Brightness)`, where Brightness is already a
// percentage -- the class is defined that way, which is why no scaling happens here.
void ApplyWmi(Display &d)
{
  if (!g_wmiUsable || !g_wmi || !d.wmiInstance[0])
    return;
  IWbemClassObject *cls = nullptr;
  BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
  HRESULT hr = g_wmi->GetObject(clsName, 0, nullptr, &cls, nullptr);
  SysFreeString(clsName);
  if (FAILED(hr) || !cls)
  {
    LogEvent("display %s: WMI class unavailable (hr=0x%08lx) -- falling back to gamma", d.device,
             (unsigned long)hr);
    d.protocol = kProtoGamma;
    ApplyGamma(d);
    return;
  }
  IWbemClassObject *in = nullptr;
  BSTR methName = SysAllocString(L"WmiSetBrightness");
  // ⚠️⚠️ `GetMethod` HANDS BACK THE METHOD'S **SIGNATURE** (the input CLASS), NOT AN INSTANCE YOU CAN FILL IN.
  // Writing the parameters straight into it is what `WBEM_E_INVALID_PARAMETER` (0x80041008) meant in the first
  // deploy of this feature: the two `Put`s failed, `ExecMethod` was handed an empty argument set, and every
  // brightness change fell back to gamma with a log line that looked like a permission problem. The signature
  // has to be SPAWNED into an instance first -- which is the one line the MSDN shape has and this did not.
  IWbemClassObject *sig = nullptr;
  cls->GetMethod(methName, 0, &sig, nullptr);
  if (sig)
  {
    sig->SpawnInstance(0, &in);
    sig->Release();
  }
  if (!in)
    cls->SpawnInstance(0, &in); // a method with no declared input class still needs an instance to fill
  if (in)
  {
    VARIANT v;
    VariantInit(&v);
    // `Timeout` is a uint32 ("wait this many seconds"; 0 = do it now) and `Brightness` is a uint8 percentage.
    v.vt = VT_I4;
    v.lVal = 0;
    BSTR pTimeout = SysAllocString(L"Timeout");
    const HRESULT putTimeout = in->Put(pTimeout, 0, &v, 0);
    SysFreeString(pTimeout);
    v.vt = VT_UI1;
    v.bVal = (BYTE)(d.level < 0 ? 0 : (d.level > 100 ? 100 : d.level));
    BSTR pBright = SysAllocString(L"Brightness");
    const HRESULT putBright = in->Put(pBright, 0, &v, 0);
    SysFreeString(pBright);
    // ⚠️ THE `Put`s ARE CHECKED. An argument that never made it into the instance is a call that goes out with
    // a hole in it, and WMI answers that with a parameter error rather than with anything a reader can act on.
    if (FAILED(putTimeout) || FAILED(putBright))
      LogEvent("display %s: could not fill WmiSetBrightness's arguments (timeout hr=0x%08lx, brightness "
               "hr=0x%08lx)",
               d.device, (unsigned long)putTimeout, (unsigned long)putBright);

    // ⚠️ `objPath` AND NOT `inst`: this variable holds an OBJECT PATH (`WmiMonitorBrightnessMethods.
    // InstanceName="DISPLAY\..."`, straight out of `__RELPATH`), and calling it "the instance" is how the
    // version that passed the bare key value read as if it were correct. The name is the only place that
    // distinction is visible at the call site.
    BSTR objPath = nullptr;
    {
      // ⚠️ IT IS UTF-16 AND COMES FROM WMI ITSELF, so the feature's own UTF-8 copy is converted rather than
      // assumed to be the same bytes.
      const int wlen = MultiByteToWideChar(CP_UTF8, 0, d.wmiInstance, -1, nullptr, 0);
      if (wlen > 0)
      {
        wchar_t *wide = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wlen);
        if (wide)
        {
          MultiByteToWideChar(CP_UTF8, 0, d.wmiInstance, -1, wide, wlen);
          objPath = SysAllocString(wide);
          free(wide);
        }
      }
    }
    if (objPath)
    {
      IWbemClassObject *out = nullptr;
      hr = g_wmi->ExecMethod(objPath, methName, 0, nullptr, in, &out, nullptr);
      if (out)
        out->Release();
      SysFreeString(objPath);
    }
    else
    {
      // The object path could not even be converted to UTF-16, which is a failure of THIS call rather than
      // something to report as success just because the last `hr` happened to be fine.
      hr = E_FAIL;
    }
    in->Release();
    if (FAILED(hr))
    {
      LogEvent("display %s: WmiSetBrightness FAILED (hr=0x%08lx) -- falling back to gamma", d.device,
               (unsigned long)hr);
      d.protocol = kProtoGamma;
      ApplyGamma(d);
    }
  }
  SysFreeString(methName);
  cls->Release();
}

// The one place that decides how a level reaches a monitor.
void ApplyLevel(Display &d)
{
  switch (d.protocol)
  {
  case kProtoDdc: ApplyDdc(d); break;
  case kProtoWmi: ApplyWmi(d); break;
  default: ApplyGamma(d); break;
  }
  d.prevApplied = d.appliedLevel;
  d.appliedLevel = d.level;
  d.wroteAt = GetTickCount();
}

// ---------------------------------------------------------------------------------------------
// WHAT IS THE SCREEN ACTUALLY AT? -- asked of every protocol, in its own way
// ---------------------------------------------------------------------------------------------
//
// ⚠️⚠️ WHY THIS EXISTS, AND IT IS THE USER'S FIRST COMPLAINT: "HKC屏幕亮度控制不能用". The external screen answers
// DDC/CI perfectly when a probe writes to it (`_diag/ddc_set_probe.cpp`: write 89, read back 89, "THE SCREEN
// REALLY MOVED"), so the wiring, the driver and the arithmetic are all fine -- which left nowhere to look,
// because THE FEATURE ONLY LOGGED ITS FAILURES. A write that is accepted and then ignored by the panel is
// indistinguishable, from inside this process, from a write that worked.
//
// So every protocol can be asked what the screen is at, and that answer is used for the two questions that
// matter: "did my write arrive?" and "did somebody else move it?" (see PollHardware). Returns false when this
// display cannot be read back at all -- which is information too, and not a failure.
bool ReadBackPercent(Display &d, int *out)
{
  if (out)
    *out = -1;
  switch (d.protocol)
  {
  case kProtoDdc:
  {
    if (d.ddcMax == 0)
      return false;
    HANDLE phys = nullptr;
    if (!OpenPhysical(d, &phys) || !phys)
      return false;
    DWORD cur = 0, maxv = 0;
    const BOOL ok = GetVCPFeatureAndVCPFeatureReply(phys, 0x10, nullptr, &cur, &maxv);
    DestroyPhysicalMonitor(phys);
    if (!ok || maxv == 0)
      return false;
    if (out)
      *out = (int)((double)cur * 100.0 / (double)maxv + 0.5);
    return true;
  }
  case kProtoWmi:
  {
    long cur = 0;
    if (!WmiFindInstance(L"WmiMonitorBrightness", d.edid, L"CurrentBrightness", nullptr, 0, nullptr, 0, &cur,
                         nullptr))
      return false;
    if (cur < 0 || cur > 100)
      return false;
    if (out)
      *out = (int)cur;
    return true;
  }
  default:
  {
    // ⚠️⚠️ GAMMA IS READ BACK IN THE USER'S OWN UNITS, AND GETTING THAT WRONG IS A BUG THIS CODE ALREADY HAD.
    // The ramp on the screen says what fraction of the BASELINE's white point it is at -- but 0% of the slider is
    // NOT 0% of the ramp, it is the API's floor (kGammaWhiteFloor / baseline, about half). So a ramp reading of
    // "50%" and an `appliedLevel` of "0" are the same state, and the first version compared them directly: every
    // gamma write looked like a command the panel had ignored, every screen was sent off to be re-probed, and an
    // outside change was therefore never followed (the gate caught it as "the page was told -1"). The read-back
    // has to be inverted through the same mapping the write went through.
    if (!d.gammaDc || !d.gammaBaseOk || d.gammaBase[0][255] == 0)
      return false;
    WORD cur[3][256];
    if (!GetDeviceGammaRamp(d.gammaDc, cur))
      return false;
    const double baseWhite = (double)d.gammaBase[0][255];
    double kMin = kGammaWhiteFloor / baseWhite;
    if (kMin > 1.0)
      kMin = 1.0;
    if (kMin < 0.0)
      kMin = 0.0;
    const double k = (double)cur[0][255] / baseWhite;
    int level = 100;
    if (kMin < 1.0)
      level = (int)((k - kMin) / (1.0 - kMin) * 100.0 + 0.5);
    if (level < 0)
      level = 0;
    if (level > 100)
      level = 100;
    if (out)
      *out = level;
    return true;
  }
  }
}

// Every couple of seconds: ask each screen what it is at, and act on the answer.
//
// ⚠️⚠️ TWO DIFFERENT FACTS COME OUT OF THE SAME READING, AND THEY NEED DIFFERENT REACTIONS:
//
//   * "MY WRITE DID NOT ARRIVE" (the reading disagrees with what was written, moments after writing): the panel
//     accepted the command and ignored it. That is a dead end for this protocol, so the display goes back into
//     the probe queue -- exactly as a refused gamma write does. Without this, a slider can sit for ever on a
//     path that never worked, and the user's report is just "it does not work".
//   * "SOMEBODY ELSE MOVED IT" (the reading disagrees long afterwards): the monitor's own buttons, the laptop's
//     brightness keys, the vendor tool. The user asked for the slider to follow that -- "主屏SDC4190系统自带亮度
//     调节快捷键也可以调，它两在调节时，推子是否可以相互实时更新状态" -- so the value is ADOPTED, and the page is
//     asked to keep re-reading for a few seconds so the slider visibly moves with it (see `externalUntil`).
void PollHardware()
{
  const DWORD now = GetTickCount();
  for (int i = 0; i < g_dispCount; ++i)
  {
    Display &d = g_disp[i];
    // Not while a write is still settling: a panel takes a moment to act, and reading too early would call a
    // perfectly good write "ignored".
    if (d.appliedLevel < 0 || (d.wroteAt && (now - d.wroteAt) < kWriteSettleMs))
      continue;
    // ⚠️ AND IT ASKS FASTER WHILE IT IS FOLLOWING SOMETHING. The user's report of the slow one: "设置面板有跟着动，
    // 但是是一卡一卡那种，好像半秒读一次" -- the panel re-reads every 400 ms (that is `waiting`), so what they
    // were seeing was the FEATURE's own two-second poll arriving in steps. Two seconds is right when nothing is
    // happening; while somebody else is moving the screen (the `externalUntil` window opened by the last adopted
    // change), it is four hundred milliseconds, so the fader follows the brightness keys as they are pressed.
    const DWORD interval =
        (d.externalUntil && now < d.externalUntil) ? kPollHardwareFastMs : kPollHardwareMs;
    if (d.polledAt && (now - d.polledAt) < interval)
      continue;
    d.polledAt = now;

    int actual = -1;
    if (!ReadBackPercent(d, &actual) || actual < 0)
      continue;
    if (actual == d.appliedLevel)
      continue; // the screen is where it was put

    if (actual == d.prevApplied && d.prevApplied >= 0)
    {
      // ⚠️ IT NEVER MOVED -- IT IS STILL AT THE VALUE THIS FEATURE WROTE THE TIME BEFORE. `SetVCPFeature` and
      // `SetDeviceGammaRamp` both return as soon as the command is out (DDC/CI has no acknowledgement), and a
      // panel is free to ignore it; measured on this machine with `_diag/ddc_set_probe.cpp`, which had to write,
      // wait and read again to find out whether the screen really moved. A protocol that is accepted and ignored
      // is a dead end, so the display goes back into the probe queue -- exactly as a refused gamma write does.
      LogEvent("display %s: asked for %d but the panel is still at %d, the value it was given before -- the "
               "command was accepted and ignored; re-probing this screen's brightness path",
               d.device, d.appliedLevel, actual);
      d.probed = false;
      d.probedAt = 0;
      d.appliedLevel = -1; // ask again once a protocol that works is found
      d.prevApplied = -1;
      if (g_wake)
        SetEvent(g_wake);
      continue;
    }

    // Somebody else's doing: a value this feature never wrote. Follow it.
    LogEvent("display %s: the screen is at %d and this feature last set %d -- taking the real value (the "
             "monitor's own buttons, a system key, or another tool)",
             d.device, actual, d.appliedLevel);
    d.level = actual;
    d.prevApplied = d.appliedLevel;
    d.appliedLevel = actual;
    // ⚠️ AND THE PAGE IS TOLD BY MAKING IT ASK: `waiting` is the only channel there is (see SettingsJson), and
    // it is published for a window rather than for ever -- a panel that polled because a monitor's button was
    // pressed five minutes ago would be a panel that never stops.
    d.externalUntil = now + kExternalFollowMs;
  }
}

// ---------------------------------------------------------------------------------------------
// THE SCREEN-OFF WINDOW, AND THE THREE WAYS BACK
// ---------------------------------------------------------------------------------------------
//
// ⚠️ WHAT THIS IS: one borderless, topmost, opaque black window per monitor, exactly the size of that
// monitor's rectangle in virtual-desktop coordinates. It shows nothing, it is not in the taskbar or in
// alt-tab (`WS_EX_TOOLWINDOW`), it does not steal the keyboard when it appears (`SWP_NOACTIVATE`), and the
// mouse pointer over it is hidden. The display stays CONNECTED, the desktop does NOT lock, no power state
// changes, and no other monitor is affected -- which is the whole of what the user asked for.
//
// ⚠️ THREE WAYS BACK, AND THE USER ASKED FOR ALL OF THEM:
//   * a DOUBLE CLICK on the dark screen (its only visible affordance);
//   * ESC, once the dark window has been clicked and therefore has the keyboard (a window that never takes
//     focus can never receive a key, so the click is what makes ESC possible at all);
//   * the per-monitor GLOBAL SHORTCUT, empty by default -- which is also the only way back if the window were
//     ever not drawn, and the reason it is not optional.
// And a fourth that is not a mechanism but a consequence: the settings page's own switch for that monitor,
// reachable from another screen.

// Which monitor a window belongs to, or -1.
int DisplayOfWindow(HWND h)
{
  for (int i = 0; i < g_dispCount; ++i)
    if (g_disp[i].black == h)
      return i;
  return -1;
}

// ⚠️ TURNING A SCREEN BACK ON IS DONE THROUGH THE SAME FUNCTION THE PAGE USES, so that the window's own
// double-click, the shortcut and the settings page cannot end up disagreeing about what "off" means. It runs on
// the control thread (the window's messages arrive there), takes the lock, and signals the cycle -- the window
// is hidden by the next pass rather than inside this call, so a click cannot leave a half-destroyed window
// behind.
void SetOffLocked(int index, bool off);
void RefreshOffCountLocked(); // the tray mark's count, recomputed where the state changes
void StartBlackFade(Display &d, double target); // the half-second fade (see kBlackFadeMs)
void TickBlackFades();

// ⚠️⚠️ ANY MOUSE BUTTON BRINGS THE SCREEN BACK, NOT ONLY A DOUBLE CLICK, AND THE WINDOW GOES AT ONCE.
//
// The user's report: "熄屏后，双击或Esc后不能马上生效，只要鼠标在熄屏的屏幕上，都要生效". Both halves of that are
// real, and they have different causes:
//
//   * WINDOWS SPENDS THE FIRST CLICK ON ACTIVATING AN INACTIVE WINDOW. This window is shown with SWP_NOACTIVATE
//     (deliberately: darkening one screen must not stop the keyboard working on another), so the first click of
//     a double-click went to activating it and only the second could ever be a WM_LBUTTONDBLCLK -- which means
//     the user had to double-click TWICE, and the first attempt looked like nothing happening at all.
//   * AND THE WINDOW WAS HIDDEN BY THE NEXT PASS OF THE CONTROL THREAD, up to 200 ms later. Nothing is slow
//     here; the work was simply queued behind a clock. The screen must come back on the same click that asked
//     for it, so the window is hidden HERE and the state change (which the page reads) goes through the one
//     function that owns it.
//
// Escape stays, for a user who clicked first and then changed their mind -- and it is the same call.
LRESULT CALLBACK BlackProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  case WM_ERASEBKGND:
  case WM_PAINT:
  {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    // The darkest thing the panel can be told. (A brush rather than a bitmap: this window is one flat colour
    // and there is nothing to scale, cache or load.)
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    EndPaint(h, &ps);
    return 0;
  }
  // ⚠️ THE POINTER IS HIDDEN, NOT MERELY IGNORED: returning TRUE from WM_SETCURSOR without setting one leaves
  // whatever the last window set, so the arrow would sit in the middle of a black screen. SetCursor(NULL) is
  // what makes it disappear, and it comes back the moment the pointer leaves this window.
  case WM_SETCURSOR:
    SetCursor(nullptr);
    return TRUE;
  // ⚠️ ALL THREE BUTTONS, AND ON THE PRESS RATHER THAN THE RELEASE: the click that brings the screen back should
  // be the click the user already made, not one they have to complete. (A press that is held and dragged would
  // also bring it back, which is the forgiving direction.)
  case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
  case WM_MBUTTONDOWN:
  {
    const int i = DisplayOfWindow(h);
    if (i >= 0)
    {
      SetOffLocked(i, false);
      // ⚠️ AND THE FADE STARTS HERE, ON THIS MESSAGE, NOT AT THE NEXT PASS OF THE CONTROL THREAD -- that was the
      // "点了一下半天才亮" the user reported when the window was hidden by a clock. The screen now begins coming
      // back on the click itself; the synchronise pass picks the window up when the fade has finished.
      StartBlackFade(g_disp[i], 0.0);
    }
    return 0;
  }
  case WM_KEYDOWN:
    if (wp == VK_ESCAPE)
    {
      const int i = DisplayOfWindow(h);
      if (i >= 0)
      {
        SetOffLocked(i, false);
        StartBlackFade(g_disp[i], 0.0);
      }
      return 0;
    }
    break;
  // ⚠️ ONE FRAME OF THE FADE (see TickBlackFades). The timer belongs to this window, so its messages arrive on the
  // control thread among everything else that thread is doing.
  case WM_TIMER:
    if (wp == kBlackFadeTimer)
    {
      TickBlackFades();
      return 0;
    }
    break;
  // ⚠️ AND A SCREEN THAT APPEARS, MOVES OR CHANGES RESOLUTION WHILE IT IS DARK IS RE-LAID-OUT RATHER THAN LEFT
  // COVERING THE WRONG PIXELS. The control thread re-enumerates on this signal and the window is moved by the
  // next pass; nothing here touches the display state itself.
  case WM_DISPLAYCHANGE:
    g_redetect = true;
    if (g_wake)
      SetEvent(g_wake);
    return 0;
  default: break;
  }
  return DefWindowProcA(h, msg, wp, lp);
}

bool EnsureBlackClass()
{
  if (g_blackClassReady)
    return true;
  WNDCLASSEXA wc;
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = BlackProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.hCursor = nullptr;
  wc.lpszClassName = kBlackClass;
  // (RegisterClassExA fails with ERROR_CLASS_ALREADY_EXISTS if it is already there, which is not an error: the
  // class may have been registered by an earlier init in the same process.)
  if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    LogEvent("could not register the screen-off window class (error %lu)", (unsigned long)GetLastError());
    return false;
  }
  g_blackClassReady = true;
  return true;
}

// (The fade's timings live up with the other constants -- BlackProc needs the timer id.)

// Start (or re-aim) a screen's fade. Called from the control thread, both by the synchronise pass and by the
// window's own click handler -- the latter directly, so a click starts the screen coming back on that very
// message rather than at the next pass of a 200 ms clock.
void StartBlackFade(Display &d, double target)
{
  // ⚠️⚠️ "ALREADY GOING THERE" IS NOT ENOUGH -- IT HAS TO BE "ALREADY THERE", TOO, AND THE DIFFERENCE IS THE
  // WHOLE FADE. `SyncOffWindows` re-aims this on every pass, so with only the running test a FINISHED fade was
  // restarted by the very next pass: the window reached alpha 0, the pass asked it to fade to 0 again, and since
  // the fade never ended the window was never hidden -- a screen "brought back" would have sat there invisible
  // and still swallowing every click for ever, which is worse than not animating at all.
  if (d.blackTarget == target && (d.fading || d.blackAlpha == target))
    return;
  d.blackFrom = d.blackAlpha;
  d.blackTarget = target;
  d.fadeStart = GetTickCount();
  d.fading = true;
  if (d.black)
    SetTimer(d.black, kBlackFadeTimer, kBlackFadeStepMs, nullptr);
}

// One frame of every fade that is running. Returns true when nothing is animating any more, so the caller can put
// the timer down.
void TickBlackFades()
{
  const DWORD now = GetTickCount();
  for (int i = 0; i < g_dispCount; ++i)
  {
    Display &d = g_disp[i];
    if (!d.fading || !d.black)
      continue;
    const DWORD elapsed = now - d.fadeStart;
    if (elapsed >= kBlackFadeMs)
    {
      d.blackAlpha = d.blackTarget;
      d.fading = false;
      KillTimer(d.black, kBlackFadeTimer);
      // ⚠️ AND THE PASS THAT HIDES THE WINDOW IS ASKED FOR AS SOON AS THE FADE ENDS, rather than waiting for the
      // control thread's next idle lap: the window is still there, still topmost and still eating the mouse, and
      // "half a second of animation" must not become "half a second plus however long the next lap takes".
      if (g_wake)
        SetEvent(g_wake);
    }
    else
    {
      const double t = (double)elapsed / (double)kBlackFadeMs;
      d.blackAlpha = d.blackFrom + (d.blackTarget - d.blackFrom) * t;
    }
    SetLayeredWindowAttributes(d.black, 0, (BYTE)(d.blackAlpha * 255.0 + 0.5), LWA_ALPHA);
  }
}

// Put every off window where its monitor is, and show or hide it to match the setting. Runs on the control thread
// only. ⚠️ THERE IS EXACTLY ONE WAY A SCREEN GOES OFF NOW (a black window -- see the note on the removed DDC power
// switch in the Display struct), so this function has one job instead of two.
void SyncOffWindows()
{
  for (int i = 0; i < g_dispCount; ++i)
  {
    Display &d = g_disp[i];
    const bool want = d.off;

    if (want && !d.blackUp)
    {
      if (!EnsureBlackClass())
        continue;
      if (!d.black)
      {
        // ⚠️ WS_EX_LAYERED IS FOR THE FADE (see the note on `blackAlpha`): the window is blitted at an alpha the
        // timer moves, so the screen goes dark over half a second instead of in one frame.
        d.black = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kBlackClass, "", WS_POPUP,
                                  0, 0, 1, 1, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
        if (!d.black)
        {
          LogEvent("display %s: could not create the screen-off window (error %lu)", d.device,
                   (unsigned long)GetLastError());
          // ⚠️ THE COUNT GOES BACK DOWN WITH IT (see RefreshOffCountLocked): a screen that could not be made
          // dark must not be reported as dark, or the one signal the user has -- the tray mark -- would be
          // describing a window that does not exist.
          EnterCriticalSection(&g_lock);
          d.off = false;
          RefreshOffCountLocked();
          LeaveCriticalSection(&g_lock);
          continue;
        }
      }
      // ⚠️ SWP_NOACTIVATE IS THE POINT: the window appears WITHOUT taking the focus, so darkening one screen
      // does not stop the keyboard from working on another one. It only takes focus if the user clicks it,
      // which is exactly when ESC has to start working.
      // ⚠️ AND IT STARTS INVISIBLE when this is a fresh fade, so the half second has somewhere to travel from.
      if (d.blackAlpha <= 0.0 && !d.fading)
        SetLayeredWindowAttributes(d.black, 0, 0, LWA_ALPHA);
      SetWindowPos(d.black, HWND_TOPMOST, d.rc.left, d.rc.top, d.rc.right - d.rc.left, d.rc.bottom - d.rc.top,
                   SWP_SHOWWINDOW | SWP_NOACTIVATE);
      SetCursor(nullptr);
      d.blackUp = true;
      StartBlackFade(d, 1.0);
      LogEvent("display %s (%s): screen off -- a black window at %ld,%ld %ldx%ld; click it, press Esc "
               "after clicking it, or use the shortcut to bring it back",
               d.device, d.name, d.rc.left, d.rc.top, d.rc.right - d.rc.left, d.rc.bottom - d.rc.top);
    }
    else if (!want && d.blackUp)
    {
      // ⚠️ THE WINDOW IS NOT HIDDEN UNTIL THE FADE HAS FINISHED -- and it keeps taking the mouse until then, which
      // is what stops a click from falling through to whatever is underneath while the screen is still half dark.
      // A later lap of this pass finds it finished and destroys it (see the branch below).
      if (d.black)
      {
        StartBlackFade(d, 0.0);
        if (!d.fading && d.blackAlpha <= 0.0)
        {
          ShowWindow(d.black, SW_HIDE);
          d.blackUp = false;
          LogEvent("display %s (%s): screen back on", d.device, d.name);
        }
      }
      else
      {
        d.blackUp = false;
      }
    }
    else if (want && d.blackUp && d.black)
    {
      // Already dark: keep the rectangle in step with the monitor (resolution change, arrangement change).
      SetWindowPos(d.black, HWND_TOPMOST, d.rc.left, d.rc.top, d.rc.right - d.rc.left, d.rc.bottom - d.rc.top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
    }
    if (!want && d.black && !d.blackUp)
    {
      DestroyWindow(d.black);
      d.black = nullptr;
      d.blackAlpha = 0.0;
      d.fading = false;
    }
  }
  // (The tray mark's count is NOT computed here any more -- see RefreshOffCountLocked.)
}

// ---------------------------------------------------------------------------------------------
// THE GLOBAL SHORTCUTS -- one per monitor, EMPTY BY DEFAULT
// ---------------------------------------------------------------------------------------------
//
// ⚠️ WHY THE FEATURE REGISTERS THESE ITSELF RATHER THAN ASKING THE HOST. The ABI has no hotkey channel, and it
// does not need one: `RegisterHotKey(NULL, ...)` posts `WM_HOTKEY` to the CALLING THREAD's queue, and this
// feature already owns a thread (the control thread) with a message loop in it. So a shortcut costs the host
// nothing and cannot be reached by any other feature -- which also means two features can never fight over one
// combination through the host.
//
// ⚠️ THE TEXT IS THE USER'S OWN RECORDED COMBINATION ("Ctrl+Alt+1"), in the same grammar the panel's hotkey
// control produces, and the parsing is HERE because the feature is what has to turn it into modifiers and a
// virtual key. A combination this parser does not understand is REFUSED (setControl answers 0, the page re-reads
// and shows what is really stored) rather than registered as something else.

struct KeyName
{
  const char *name;
  unsigned vk;
};

const KeyName kKeyNames[] = {
    {"Space", VK_SPACE},   {"Esc", VK_ESCAPE},     {"Escape", VK_ESCAPE}, {"Enter", VK_RETURN},
    {"Return", VK_RETURN}, {"Tab", VK_TAB},        {"Backspace", VK_BACK}, {"Insert", VK_INSERT},
    {"Delete", VK_DELETE}, {"Home", VK_HOME},      {"End", VK_END},       {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT}, {"Up", VK_UP},          {"Down", VK_DOWN},     {"Left", VK_LEFT},
    {"Right", VK_RIGHT},   {"PrintScreen", VK_SNAPSHOT}, {"Pause", VK_PAUSE},
};

// "Ctrl+Alt+1" -> MOD_CONTROL|MOD_ALT, '1'. Returns false for anything it does not fully understand: a
// half-understood combination is worse than none, because the user would believe it was registered.
bool ParseHotkey(const char *text, unsigned *mods, unsigned *vk)
{
  if (!text || !text[0] || !mods || !vk)
    return false;
  unsigned m = 0;
  char buf[64] = {0};
  _snprintf(buf, sizeof(buf), "%s", text);
  char *p = buf;
  for (;;)
  {
    char *plus = strchr(p, '+');
    char *part = p;
    if (plus)
      *plus = 0;
    if (!part[0])
      return false;
    if (plus)
    {
      // A modifier. Anything else in a non-final position is a combination we do not understand.
      if (_stricmp(part, "Ctrl") == 0 || _stricmp(part, "Control") == 0)
        m |= MOD_CONTROL;
      else if (_stricmp(part, "Alt") == 0)
        m |= MOD_ALT;
      else if (_stricmp(part, "Shift") == 0)
        m |= MOD_SHIFT;
      else if (_stricmp(part, "Win") == 0)
        m |= MOD_WIN;
      else
        return false;
      p = plus + 1;
      continue;
    }
    // The last part is the key itself.
    if (part[1] == 0)
    {
      const char c = part[0];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        *vk = (unsigned)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
      else
        *vk = (unsigned)(unsigned char)c; // a symbol key: VkKeyScan's own answer for it is the character
    }
    else if (part[0] == 'F' || part[0] == 'f')
    {
      const int n = atoi(part + 1);
      if (n < 1 || n > 24)
        return false;
      *vk = (unsigned)(VK_F1 + n - 1);
    }
    else
    {
      bool found = false;
      for (size_t i = 0; i < sizeof(kKeyNames) / sizeof(kKeyNames[0]); ++i)
      {
        if (_stricmp(part, kKeyNames[i].name) == 0)
        {
          *vk = kKeyNames[i].vk;
          found = true;
          break;
        }
      }
      if (!found)
        return false;
    }
    break;
  }
  // ⚠️ A COMBINATION WITH NO MODIFIER IS REFUSED, and this is a real decision rather than tidiness: a global
  // shortcut on a bare letter is a shortcut that fires while the user is typing -- including while they are
  // typing in this program's own settings page. (The page refuses to record one for the same reason.)
  if (m == 0)
    return false;
  *mods = m;
  return true;
}

// Registered shortcuts are keyed by the monitor's position, which is stable while the monitor list is stable.
unsigned HotkeyIdFor(int index) { return 0x4D01u + (unsigned)index; }

void SyncHotkeys()
{
  for (int i = 0; i < g_dispCount; ++i)
  {
    Display &d = g_disp[i];
    unsigned mods = 0, vk = 0;
    const bool want = ParseHotkey(d.hotkey, &mods, &vk);
    if (want && d.hotkeyId && mods == d.hotkeyMods && vk == d.hotkeyVk)
      continue; // already registered exactly like this
    if (d.hotkeyId)
    {
      UnregisterHotKey(nullptr, (int)d.hotkeyId);
      d.hotkeyId = 0;
      d.hotkeyMods = d.hotkeyVk = 0;
    }
    if (!want)
      continue;
    const unsigned id = HotkeyIdFor(i);
    if (RegisterHotKey(nullptr, (int)id, mods, vk))
    {
      d.hotkeyId = (int)id;
      d.hotkeyMods = mods;
      d.hotkeyVk = vk;
      LogEvent("display %s: shortcut %s registered", d.device, d.hotkey);
    }
    else
    {
      // ⚠️ SAID OUT LOUD, because this failure is invisible: the box on the page shows the combination the user
      // recorded, and the key simply never fires. The usual cause is another program already owning it.
      LogEvent("display %s: shortcut %s REFUSED by Windows (error %lu) -- something else owns it",
               d.device, d.hotkey, (unsigned long)GetLastError());
    }
  }
}

// A shortcut was pressed. Runs on the control thread, from its own message loop.
void OnHotkey(int id)
{
  for (int i = 0; i < g_dispCount; ++i)
  {
    if (g_disp[i].hotkeyId == id)
    {
      SetOffLocked(i, !g_disp[i].off);
      return;
    }
  }
}

// ⚠️⚠️ THE TRAY MARK'S COUNT FOLLOWS `d.off` -- THE USER'S INTENT -- AND NOT "IS THE WINDOW UP YET".
//
// The first version counted the windows at the END of the synchronise pass, which is a different moment from
// when the window becomes visible only by a few instructions -- and that was enough. The probe sets a screen
// off and checks `flags()` the instant it can SEE the window (that is what it is testing), and the two threads
// can be scheduled so that the window is already visible while the counting loop has not run: the gate then
// reports "a black window appears: ok" followed by "the host is told something is being held: FAIL", which
// reads like the tray bit is broken rather than like a race in this feature.
//
// So the count is written where the state changes, under the same lock, by the one function that decides it.
// `SetOffLocked` is called ON THE SETTINGS PAGE'S OWN THREAD (the page's `setControl`), so the bit is already
// true by the time that call returns -- no window, no scheduling and no second thread in between. The window
// follows a few hundred microseconds later, which is what the design says anyway: `off` is what the user asked
// for, and the window is how it is delivered.
void RefreshOffCountLocked()
{
  int n = 0;
  for (int i = 0; i < g_dispCount; ++i)
    if (g_disp[i].off)
      ++n;
  InterlockedExchange(&g_offCount, n);
}

// The one way "this screen is dark" is changed, whoever asked -- the page's switch, the window's double click,
// ESC, or the shortcut. It writes the state under the lock (the page reads it) and signals the control thread,
// which is what actually shows or hides the window on its next pass.
void SetOffLocked(int index, bool off)
{
  if (index < 0 || index >= kMaxDisplays)
    return;
  EnterCriticalSection(&g_lock);
  if (index < g_dispCount)
  {
    g_disp[index].off = off;
    RefreshOffCountLocked();
  }
  LeaveCriticalSection(&g_lock);
  if (g_wake)
    SetEvent(g_wake);
}

// ---------------------------------------------------------------------------------------------
// FINDING THE MONITORS, AND WORKING OUT HOW EACH ONE ANSWERS
// ---------------------------------------------------------------------------------------------

// Pull "SDC4190" out of "MONITOR\SDC4190\{4d36e96e-...}\0022". It is the one stable, human-checkable name a
// monitor has without asking it anything, and it is also what WMI's InstanceName carries -- which is how a WMI
// brightness instance is matched to the GDI device below.
void EdidFromDeviceId(const char *deviceId, char *out, int outSize)
{
  out[0] = 0;
  if (!deviceId || !out || outSize <= 1)
    return;
  const char *p = deviceId;
  if (_strnicmp(p, "MONITOR\\", 8) == 0)
    p += 8;
  int n = 0;
  while (p[n] && p[n] != '\\' && n < outSize - 1)
  {
    out[n] = p[n];
    ++n;
  }
  out[n] = 0;
}

struct ScanItem
{
  HMONITOR hmon = nullptr;
  RECT rc = {0, 0, 0, 0};
  bool primary = false;
  char device[32] = {0};
  char edid[32] = {0};
};

struct ScanCtx
{
  ScanItem items[kMaxDisplays];
  int count = 0;
};

BOOL CALLBACK ScanProc(HMONITOR hMon, HDC, LPRECT, LPARAM user)
{
  ScanCtx *ctx = (ScanCtx *)user;
  if (ctx->count >= kMaxDisplays)
    return FALSE;
  ScanItem &it = ctx->items[ctx->count];
  MONITORINFOEXA ex;
  memset(&ex, 0, sizeof(ex));
  ex.cbSize = sizeof(ex);
  if (!GetMonitorInfoA(hMon, &ex))
    return TRUE;
  it.hmon = hMon;
  it.rc = ex.rcMonitor;
  it.primary = (ex.dwFlags & MONITORINFOF_PRIMARY) != 0;
  _snprintf(it.device, sizeof(it.device), "%s", ex.szDevice);
  // The monitor's own device entry, which is where the EDID name lives.
  DISPLAY_DEVICEA mon;
  memset(&mon, 0, sizeof(mon));
  mon.cb = sizeof(mon);
  if (EnumDisplayDevicesA(ex.szDevice, 0, &mon, 0))
    EdidFromDeviceId(mon.DeviceID, it.edid, (int)sizeof(it.edid));
  ++ctx->count;
  return TRUE;
}

// (Declared here because the probe below runs before it is defined, and the two are one story: a monitor is
// probed, and WMI is one of the three answers it may give.)
void WmiMatchDisplay(Display &d);

// A monitor that has never been seen before is probed ONCE, in this order: DDC/CI first (it is a real backlight
// change), then WMI (the laptop's internal panel), then gamma (always works). The probe reads a value back
// rather than assuming: a monitor can answer the "what is your maximum brightness" question with nonsense, and
// a write to a monitor that only pretends to speak DDC is a write that silently does nothing.
void ProbeDisplay(Display &d)
{
  d.probed = true;
  d.probedAt = GetTickCount();
  d.protocol = kProtoGamma;

  // ⚠️⚠️ WHAT IS THIS MONITOR? ASKED FIRST, BECAUSE EVERYTHING ELSE IS REMEMBERED AGAINST THE ANSWER (see
  // `Display::identity`). Asked of the panel itself through `WmiMonitorID`, and it has to happen here rather than
  // at startup: WMI is opened on this thread, just before the monitors are walked.
  if (WmiMonitorIdentity(d.edid, d.identity, (int)sizeof(d.identity)))
    LogEvent("display %s (%s): the monitor identifies itself as %s", d.device, d.name, d.identity);
  else
  {
    _snprintf(d.identity, sizeof(d.identity), "%s", d.edid);
    LogEvent("display %s (%s): the panel would not name itself -- falling back to its EDID name (%s)",
             d.device, d.name, d.identity[0] ? d.identity : "unknown");
  }
  // ... and now it can take the brightness, the shortcut and the name the user gave THIS screen.
  EnterCriticalSection(&g_lock);
  ApplyStored(d);
  LeaveCriticalSection(&g_lock);

  // ---- gamma: open the device's own DC. NOTHING ELSE. ----
  //
  // ⚠️⚠️ THE BASELINE IS NOT READ HERE, AND IT USED TO BE -- WHICH IS WHY THE NORMALISATION IN ApplyGamma NEVER
  // RAN. This function set `gammaBase` and `gammaBaseOk = true` on the strength of a single read, so the later
  // "if I have no baseline yet" branch in ApplyGamma was always false: the white point was never normalised, the
  // original ramp was never kept for the way out, and the slider stayed a percentage of whatever another tool
  // had left on the screen. Two places deciding one thing is the failure this project has a rule about; the
  // baseline belongs to the code that WRITES it (ApplyGamma), and this one only opens the door.
  if (!d.gammaDc)
    d.gammaDc = CreateDCA("DISPLAY", d.device, nullptr, nullptr);

  // ---- DDC/CI ----
  //
  // ⚠️ THE PROBE READS THROUGH A HANDLE IT TAKES AND GIVES BACK (see OpenPhysical): nothing about DDC/CI is kept
  // between calls. What IS kept is the answer -- "this screen speaks it, and its full scale is 100".
  {
    HANDLE phys = nullptr;
    if (OpenPhysical(d, &phys) && phys)
    {
      DWORD cur = 0, maxv = 0;
      if (GetVCPFeatureAndVCPFeatureReply(phys, 0x10, nullptr, &cur, &maxv) && maxv > 0)
      {
        d.ddcMax = maxv;
        d.protocol = kProtoDdc;
        // The value the monitor reports is the truth about where the slider should start -- but only the FIRST
        // time, because after that the stored level is the user's preference and re-reading it would undo a
        // change the user made whenever the monitor list is re-scanned.
        if (d.appliedLevel < 0)
          d.level = (int)((double)cur * 100.0 / (double)maxv + 0.5);
        LogEvent("display %s (%s): DDC/CI answered -- current %lu of %lu", d.device, d.name,
                 (unsigned long)cur, (unsigned long)maxv);
        // ⚠️ AND ITS POWER MODE (VCP 0xD6) IS DELIBERATELY NOT EVEN READ ANY MORE. It used to be, to decide
        // whether "screen off" could turn this panel's backlight off for real -- and that whole path is gone (see
        // the note in the Display struct): the user's own screen dropped its display link on standby, so a real
        // power-off is a disconnect, and this feature does not disconnect anything. A probe that asked anyway
        // would only put a line in the log about a capability nothing uses.
      }
      DestroyPhysicalMonitor(phys);
    }
  }

  // ---- WMI, only if DDC did not work (two brightness owners would fight over one panel) ----
  if (d.protocol != kProtoDdc)
    WmiMatchDisplay(d);

  if (d.protocol == kProtoGamma)
    LogEvent("display %s (%s): no DDC/CI and no WMI -- driving it through the gamma ramp (the backlight does "
             "not change; this is the software fallback)",
             d.device, d.name);
}

// ---------------------------------------------------------------------------------------------
// WMI -- the internal panel
// ---------------------------------------------------------------------------------------------

// ⚠️ `CoInitializeSecurity` FIRST, AND THE REASON IS MEASURED: without it, `ConnectServer` answers
// WBEM_E_ACCESS_DENIED (0x80041003) to a READ, which looks exactly like "this machine has no such class". The
// first run of _diag/media_probe.cpp reported the laptop-panel path as missing for that one reason. In THIS
// process the call may already have been made by the host (the panel is WebView2, which has its own idea of COM
// security), in which case it fails with RPC_E_TOO_LATE -- which is fine and is why the result is only logged.
void WmiOpen()
{
  g_wmiTried = true;
  // The security call's own result is only logged: RPC_E_TOO_LATE means the host (or WebView2 inside it)
  // already made this call for the process, which is not a failure for anything below.
  const HRESULT sec = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
  IWbemLocator *loc = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void **>(&loc));
  if (FAILED(hr) || !loc)
  {
    LogEvent("WMI: no locator (hr=0x%08lx, security 0x%08lx) -- the internal-panel brightness path is "
             "unavailable",
             (unsigned long)hr, (unsigned long)sec);
    return;
  }
  IWbemServices *svc = nullptr;
  BSTR ns = SysAllocString(L"ROOT\\WMI");
  hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
  SysFreeString(ns);
  loc->Release();
  if (FAILED(hr) || !svc)
  {
    // The expected answer on the machine this was written for: "access denied" to a non-elevated process.
    LogEvent("WMI: could not open ROOT\\WMI (hr=0x%08lx) -- brightness through WMI is off for this run",
             (unsigned long)hr);
    return;
  }
  CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  g_wmi = svc;
  g_wmiUsable = true;
  LogEvent("WMI: ROOT\\WMI opened -- the internal-panel path is available");
}

// Upper-case ASCII in place. Written out rather than borrowed from `_strupr`: the EDID names compared below are
// ASCII by construction, and a deprecated string function is not worth the warning.
void UpperAscii(char *s)
{
  for (; s && *s; ++s)
    if (*s >= 'a' && *s <= 'z')
      *s = (char)(*s - 'a' + 'A');
}

// FIND THE INSTANCE OF `className` THAT BELONGS TO THIS MONITOR, AND OPTIONALLY ONE NUMBER FROM IT.
//
// ⚠️⚠️ `WBEM_INFINITE`, NOT 0, AS THE TIMEOUT ON `Next` -- AND THAT ONE ARGUMENT WAS THE WHOLE DIFFERENCE
// BETWEEN "this panel has no WMI brightness" AND "this panel does". Measured on the machine this was written
// for: `WmiMonitorBrightnessMethods` has exactly one instance, `DISPLAY\SDC4190\...`, and it belongs to the
// main screen. With a timeout of 0 the enumerator answered "nothing yet" immediately, every monitor fell
// through to the gamma ramp, and the feature's own log said "no DDC/CI and no WMI" -- a claim about the HARDWARE
// that was really a claim about one argument. (`Next`'s timeout is in MILLISECONDS; the documented "do not
// wait" value for an enumerator is `WBEM_INFINITE`, and a blocking walk of one or two instances costs nothing.)
//
// ⚠️⚠️ AND IT HANDS BACK **`__RELPATH`** AS WELL AS THE KEY, BECAUSE THE KEY IS NOT A PATH. `ExecMethod` wants an
// OBJECT PATH, and the instance's `InstanceName` -- "DISPLAY\SDC4190\5&37538a9&a&UID4353_0" -- is only its KEY
// VALUE: passing that answers `WBEM_E_INVALID_PARAMETER` (0x80041008), which reads like a bad argument rather
// than like a bad address. `_diag/wmi_set_probe.cpp` tried all four spellings on this machine and only two work:
//
//     InstanceName value,        VT_I4 / VT_UI1  -> 0x80041008
//     Class.Key="value"                          -> 0x80041008
//     __RELPATH                                  -> S_OK
//     __PATH                                     -> S_OK      (both, either VARIANT type)
//
// `__RELPATH` is the one kept: it is the whole address without the machine name, so it cannot go stale when
// this folder is copied to another PC. (That probe writes only the brightness the panel is already at, which is
// what makes it safe to run -- it can prove the call works without moving the screen.)
bool WmiFindInstance(const wchar_t *className, const char *edid, const wchar_t *numberProp, char *keyOut,
                     int keyCap, char *pathOut, int pathCap, long *numberOut, int *seen)
{
  if (seen)
    *seen = 0;
  if (keyOut && keyCap > 0)
    keyOut[0] = 0;
  if (pathOut && pathCap > 0)
    pathOut[0] = 0;
  if (!g_wmiUsable || !g_wmi || !edid || !edid[0] || !className)
    return false;

  wchar_t wql[256] = {0};
  _snwprintf(wql, 255, L"SELECT * FROM %s", className);
  IEnumWbemClassObject *en = nullptr;
  BSTR lang = SysAllocString(L"WQL");
  BSTR q = SysAllocString(wql);
  HRESULT hr = g_wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
  SysFreeString(lang);
  SysFreeString(q);
  if (FAILED(hr) || !en)
    return false;

  char upperEdid[64] = {0};
  _snprintf(upperEdid, sizeof(upperEdid), "%s", edid);
  UpperAscii(upperEdid);

  bool found = false;
  // ⚠️ THE WHOLE CLASS IS WALKED EVEN AFTER A MATCH, so that `seen` is the class's real size rather than "one,
  // because I stopped looking". There are one or two of these per machine.
  for (;;)
  {
    IWbemClassObject *obj = nullptr;
    ULONG got = 0;
    if (en->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0 || !obj)
      break;
    if (seen)
      ++*seen;
    VARIANT v;
    VariantInit(&v);
    if (obj->Get(L"InstanceName", 0, &v, nullptr, nullptr) == S_OK && v.vt == VT_BSTR)
    {
      char inst[512] = {0};
      WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, inst, (int)sizeof(inst) - 1, nullptr, nullptr);
      // Case-insensitive containment: the instance path carries the EDID name with structure around it
      // ("DISPLAY\SDC4190\5&37538a9&a&UID4353_0").
      char upperInst[512] = {0};
      _snprintf(upperInst, sizeof(upperInst), "%s", inst);
      UpperAscii(upperInst);
      if (!found && strstr(upperInst, upperEdid))
      {
        found = true;
        if (keyOut && keyCap > 0)
          _snprintf(keyOut, keyCap, "%s", inst);
        // The address `ExecMethod` needs, straight from WMI rather than assembled here.
        if (pathOut && pathCap > 0)
        {
          VARIANT pv;
          VariantInit(&pv);
          if (obj->Get(L"__RELPATH", 0, &pv, nullptr, nullptr) == S_OK && pv.vt == VT_BSTR)
            WideCharToMultiByte(CP_UTF8, 0, pv.bstrVal, -1, pathOut, pathCap - 1, nullptr, nullptr);
          VariantClear(&pv);
        }
        if (numberOut && numberProp)
        {
          VARIANT nv;
          VariantInit(&nv);
          if (obj->Get(numberProp, 0, &nv, nullptr, nullptr) == S_OK)
          {
            *numberOut = (long)nv.lVal;
            VariantClear(&nv);
          }
        }
      }
    }
    VariantClear(&v);
    obj->Release();
  }
  en->Release();
  return found;
}

// Match one WmiMonitorBrightnessMethods instance to a monitor by its EDID name, and -- the first time only --
// take the panel's CURRENT brightness as the slider's starting point, exactly as the DDC/CI probe does.
void WmiMatchDisplay(Display &d)
{
  int seen = 0;
  char key[256] = {0};
  char relPath[256] = {0};
  if (WmiFindInstance(L"WmiMonitorBrightnessMethods", d.edid, nullptr, key, (int)sizeof(key), relPath,
                      (int)sizeof(relPath), nullptr, &seen) &&
      relPath[0])
  {
    // ⚠️ WHAT IS STORED IS THE `__RELPATH`, NOT THE KEY -- see the note on WmiFindInstance: the key is not an
    // object path, and handing it to ExecMethod is what 0x80041008 meant.
    _snprintf(d.wmiInstance, sizeof(d.wmiInstance), "%s", relPath);
    d.wmiOk = true;
    d.protocol = kProtoWmi;
    LogEvent("display %s (%s): WMI instance %s (key %s, %d instance(s) of the class)", d.device, d.name,
             d.wmiInstance, key[0] ? key : "?", seen);
    if (d.appliedLevel < 0)
    {
      long cur = 0;
      if (WmiFindInstance(L"WmiMonitorBrightness", d.edid, L"CurrentBrightness", nullptr, 0, nullptr, 0, &cur,
                          nullptr) &&
          cur >= 0 && cur <= 100)
        d.level = (int)cur;
    }
    return;
  }
  // ⚠️ SAID OUT LOUD WITH THE COUNT, because "the class had two instances and neither named this panel" and
  // "the class had none at all" are different facts about the hardware, and the log is the only place either can
  // be seen from (the ABI has no field for "this control is degraded").
  LogEvent("display %s (%s): no WmiMonitorBrightnessMethods instance matches \"%s\" (%d instance(s) of the "
           "class) -- this panel cannot be driven through WMI",
           d.device, d.name, d.edid[0] ? d.edid : "?", seen);
}

// ---------------------------------------------------------------------------------------------
// ENUMERATING AND KEEPING THE MONITOR LIST
// ---------------------------------------------------------------------------------------------
//
// ⚠️ THE LOCK IS HELD FOR THE MERGE AND NOT FOR THE PROBE, AND THAT IS DELIBERATE. A probe talks DDC/CI, which
// is a slow serial protocol (a tenth of a second is normal, and a monitor that is switched off can take
// seconds) -- and the settings page reads this table to draw its rows. Holding the lock across a probe would
// freeze the page for as long as a monitor takes to answer. What makes the split safe is that the table's
// SHAPE (which monitors, in what order) only ever changes on the control thread, under the lock; the fields a
// probe writes (`protocol`, `phys`, the gamma baseline, `note`) are read by nobody else, and the display name
// is already filled in before the lock is dropped.
//
// ⚠️⚠️ AND THE PROTOCOL IS RE-DECIDED, NOT DECIDED ONCE. That is the other half of "the machine changed", and
// the user found it by plugging a monitor in: Apex started while an external 4K screen was the PRIMARY and
// `\\.\DISPLAY1`, probed it, got no answer from DDC/CI (a monitor that has just been switched on has not
// finished coming up), and drove it through the gamma ramp from then on -- while the SAME monitor, appearing a
// few minutes later as `\\.\DISPLAY2`, answered `DDC/CI ... current 90 of 100`. Two rules come out of that, and
// both are needed:
//
//   * A DISPLAY DEVICE NAME IS NOT AN IDENTITY. `\\.\DISPLAY1` is a SLOT: unplug one screen and plug another
//     into the same port and the name is unchanged while the panel behind it is a different piece of hardware.
//     So the EDID name is compared on every scan, and a mismatch means "this is a new monitor" -- old handles
//     released, protocol re-probed, and the screen-off state cleared (a black window left over from the screen
//     that used to be there would be a black screen with nothing behind it).
//   * A FAILED PROBE IS NOT A VERDICT. DDC/CI failing at 00:03 and succeeding at 00:12 is the same monitor; it
//     was not ready. So a monitor that ended up on the gamma fallback is probed AGAIN every `kReprobeMs`, and a
//     gamma write that Windows refuses asks for the same re-probe immediately (a refusal there is itself
//     evidence that this screen wants a different protocol).
void ReleaseDisplayHandles(Display &d)
{
  if (d.gammaDc)
  {
    // Give the ramp back before letting go of the DC -- and only if it is still ours to give back (see
    // RestoreGammaIfOurs): a monitor that is being unplugged may already be driven by something else.
    RestoreGammaIfOurs(d);
    DeleteDC(d.gammaDc);
    d.gammaDc = nullptr;
  }
  if (d.black)
  {
    DestroyWindow(d.black);
    d.black = nullptr;
    d.blackUp = false;
    // ⚠️ AND THE FADE GOES WITH THE WINDOW (see StartBlackFade): the timer was this window's, and a fade left
    // "running" here would be a screen whose next dark window skips its own fade-in and appears already solid.
    d.blackAlpha = 0.0;
    d.fading = false;
  }
  if (d.hotkeyId)
  {
    UnregisterHotKey(nullptr, d.hotkeyId);
    d.hotkeyId = 0;
    d.hotkeyMods = d.hotkeyVk = 0;
  }
}

void EnumerateDisplays(bool probe)
{
  ScanCtx ctx;
  EnumDisplayMonitors(nullptr, nullptr, ScanProc, (LPARAM)&ctx);
  if (ctx.count <= 0)
    return; // a moment with no monitors at all: keep what we have rather than losing the user's settings

  // A stable order that does not depend on what the enumerator happened to return first: sort by device name.
  for (int i = 1; i < ctx.count; ++i)
    for (int j = i; j > 0 && strcmp(ctx.items[j - 1].device, ctx.items[j].device) > 0; --j)
    {
      ScanItem t = ctx.items[j];
      ctx.items[j] = ctx.items[j - 1];
      ctx.items[j - 1] = t;
    }

  bool added[kMaxDisplays] = {false};
  EnterCriticalSection(&g_lock);
  Display next[kMaxDisplays];
  int n = 0;
  for (int i = 0; i < ctx.count && n < kMaxDisplays; ++i)
  {
    const ScanItem &s = ctx.items[i];
    // Carry the existing row over -- the level, the shortcut and (above all) whether the screen is dark are the
    // user's, and a re-scan must never quietly undo them.
    int old = -1;
    for (int k = 0; k < g_dispCount; ++k)
      if (strcmp(g_disp[k].device, s.device) == 0)
      {
        old = k;
        break;
      }
    if (old >= 0)
    {
      Display d = g_disp[old];
      // ⚠️⚠️ IS IT THE SAME PANEL? `\\.\DISPLAY1` IS A SLOT, NOT AN IDENTITY -- and the user's own machine is
      // the proof: it started with an external 4K screen in slot 1 (probed, no DDC, switched to the gamma
      // fallback) and later had the laptop's own panel there, while the external screen reappeared as
      // `\\.\DISPLAY2` and answered DDC/CI perfectly. Carrying the old row over by device name alone kept the
      // old protocol, the old physical-monitor handle and the old gamma baseline for a screen that was no
      // longer attached.
      const bool samePanel = (s.edid[0] == 0) || (strcmp(d.edid, s.edid) == 0);
      if (!samePanel)
      {
        LogEvent("display %s: the panel changed (%s -> %s) -- re-probing its brightness path", s.device,
                 d.edid[0] ? d.edid : "?", s.edid);
        // Everything tied to the OLD screen goes: its DDC handle, its gamma DC and baseline, its dark window,
        // its shortcut. What stays is what belongs to the POSITION rather than to the panel -- the level and the
        // shortcut the user set -- because those are stored by device name in the settings file. `off` does NOT
        // stay: a new screen must never arrive already black.
        ReleaseDisplayHandles(d);
        d.protocol = kProtoGamma;
        d.probed = false;
        d.probedAt = 0;
        d.appliedLevel = -1;
        d.off = false;
        d.wmiOk = false;
        d.wmiInstance[0] = 0;
        // ⚠️ AND THIS ONE IS A DIFFERENT SCREEN, SO THE FILE MAY SPEAK FOR IT AGAIN (see ApplyStored).
        d.storedApplied = false;
        added[n] = true;
      }
      d.hmon = s.hmon;
      d.rc = s.rc;
      d.primary = s.primary;
      if (s.edid[0])
        _snprintf(d.edid, sizeof(d.edid), "%s", s.edid);
      next[n++] = d;
    }
    else
    {
      Display d;
      _snprintf(d.device, sizeof(d.device), "%s", s.device);
      _snprintf(d.edid, sizeof(d.edid), "%s", s.edid);
      d.hmon = s.hmon;
      d.rc = s.rc;
      d.primary = s.primary;
      added[n] = true;
      next[n++] = d;
      LogEvent("display %s: new monitor, %ldx%ld at %ld,%ld (edid %s)", s.device, s.rc.right - s.rc.left,
               s.rc.bottom - s.rc.top, s.rc.left, s.rc.top, s.edid[0] ? s.edid : "unknown");
    }
  }
  // A monitor that went away takes its window with it -- `SyncOffWindows` would otherwise keep hiding a window
  // whose Display is gone, and the window would never be destroyed.
  for (int k = 0; k < g_dispCount; ++k)
  {
    bool stillHere = false;
    for (int i = 0; i < n; ++i)
      if (strcmp(next[i].device, g_disp[k].device) == 0)
      {
        stillHere = true;
        break;
      }
    if (!stillHere)
    {
      LogEvent("display %s went away", g_disp[k].device);
      ReleaseDisplayHandles(g_disp[k]);
    }
  }
  for (int i = 0; i < n; ++i)
    g_disp[i] = next[i];
  // ⚠️ AND THE SHORTCUT IDS FOLLOW THE POSITION, so anything that was registered against an old index is
  // dropped here and re-registered by the next `SyncHotkeys` pass. (A monitor being added or removed shifts
  // every id after it; leaving one registered would make a shortcut fire for the wrong screen.)
  for (int i = 0; i < n; ++i)
  {
    if (g_disp[i].hotkeyId && (unsigned)g_disp[i].hotkeyId != HotkeyIdFor(i))
    {
      UnregisterHotKey(nullptr, g_disp[i].hotkeyId);
      g_disp[i].hotkeyId = 0;
      g_disp[i].hotkeyMods = g_disp[i].hotkeyVk = 0;
    }
    // The name the user reads: the panel's own model name plus its resolution, which is enough to tell two
    // screens apart and is language-independent (it is hardware data, not a label -- the page draws it as-is).
    _snprintf(g_disp[i].name, sizeof(g_disp[i].name), "%s %ldx%ld",
              g_disp[i].edid[0] ? g_disp[i].edid : g_disp[i].device,
              g_disp[i].rc.right - g_disp[i].rc.left, g_disp[i].rc.bottom - g_disp[i].rc.top);
  }
  g_dispCount = n;
  // A monitor that went away takes its "screen is dark" with it, so the tray mark is recomputed here too.
  RefreshOffCountLocked();
  LeaveCriticalSection(&g_lock);

  // The slow part, OUTSIDE the lock (see the note above): a probe talks DDC/CI, and the page reads this table.
  // `probe` is false only for the one scan `init` does, where blocking the host's startup on a serial protocol
  // would be rude -- the control thread probes on its very first pass instead.
  //
  // ⚠️ AND A MONITOR THAT LANDED ON THE GAMMA FALLBACK IS PROBED AGAIN LATER. A probe that fails is not a
  // verdict about the hardware: this machine's external screen answered nothing at 00:03 (just switched on) and
  // `current 90 of 100` at 00:12. Without the retry the feature keeps a permanent, wrong answer -- and the
  // slider it drives there was refused by Windows besides (see ApplyGamma), so the screen had NO working
  // brightness control at all while a perfectly good one was one DDC call away.
  if (probe)
  {
    const DWORD now = GetTickCount();
    for (int i = 0; i < n; ++i)
      if (added[i] || !g_disp[i].probed ||
          (g_disp[i].protocol == kProtoGamma && (now - g_disp[i].probedAt) >= kReprobeMs))
        ProbeDisplay(g_disp[i]);
  }
}

// ---------------------------------------------------------------------------------------------
// THE CONTROL THREAD
// ---------------------------------------------------------------------------------------------
//
// ONE THREAD, THREE JOBS, AND THEY ARE ONE THREAD ON PURPOSE:
//   * it OWNS THE OFF WINDOWS (a window is only sent messages while the thread that created it pumps), and it
//     is therefore the thread that turns a double click or an ESC into "that screen is on again";
//   * it OWNS THE GLOBAL SHORTCUTS (`RegisterHotKey(NULL, ...)` posts WM_HOTKEY to the CALLING thread's queue);
//   * it does the SLOW brightness writes (DDC/CI, WMI), which must not happen on the settings page's thread.
// A cycle is: pump whatever messages arrived, re-scan the monitors when due, apply any level that changed,
// bring the dark windows in line with the settings, and make the shortcuts match. Then it waits -- on `g_wake`
// (the page changed something) or on the next message, whichever comes first.
DWORD WINAPI ControlThread(LPVOID)
{
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WmiOpen();

  DWORD lastScan = 0;
  for (;;)
  {
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
      break;

    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      // ⚠️ WM_HOTKEY ARRIVES WITH A NULL WINDOW (`RegisterHotKey` with a null window posts to the thread), so
      // it has to be handled here rather than reaching any window procedure.
      if (msg.message == WM_HOTKEY)
        OnHotkey((int)msg.wParam);
      else
      {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
      }
    }

    const DWORD now = GetTickCount();
    EnterCriticalSection(&g_lock);
    const bool due = g_redetect || (now - lastScan >= kMonitorPollMs);
    g_redetect = false;
    LeaveCriticalSection(&g_lock);
    if (due)
    {
      EnumerateDisplays(true);
      lastScan = GetTickCount();
    }

    // ---- the levels that changed since the last pass ----
    //
    // The pending set is collected under the lock and the WRITES happen outside it: a DDC write can take a
    // tenth of a second, and the settings page reads this table to draw its sliders.
    int pending[kMaxDisplays];
    int pendingCount = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_dispCount; ++i)
      if (g_disp[i].appliedLevel != g_disp[i].level)
        pending[pendingCount++] = i;
    LeaveCriticalSection(&g_lock);
    for (int k = 0; k < pendingCount; ++k)
    {
      const int i = pending[k];
      const int want = g_disp[i].level;
      ApplyLevel(g_disp[i]);
      EnterCriticalSection(&g_lock);
      g_disp[i].appliedLevel = want;
      LeaveCriticalSection(&g_lock);
    }

    SyncOffWindows();
    SyncHotkeys();
    PollHardware(); // did the screen do what it was told, or has something else moved it?

    MsgWaitForMultipleObjects(1, &g_wake, FALSE, kIdleTickMs, QS_ALLINPUT);
  }

  // ⚠️ LEAVING NOTHING BEHIND, IN THIS ORDER, AND EVERY LINE OF IT MATTERS:
  //   * OUR gamma ramps go back -- and ONLY ours (see RestoreGammaIfOurs: a screen another tool has taken over
  //     since must be left exactly as that tool set it);
  //   * the dark windows are destroyed (a screen left black with no program behind it is unrecoverable);
  //   * the shortcuts are unregistered (Windows would keep them until the process ends anyway, but a host that
  //     unloads this DLL keeps running).
  for (int i = 0; i < g_dispCount; ++i)
  {
    Display &d = g_disp[i];
    RestoreGammaIfOurs(d);
    if (d.black)
    {
      DestroyWindow(d.black);
      d.black = nullptr;
      d.blackUp = false;
      d.blackAlpha = 0.0;
      d.fading = false;
    }
    if (d.hotkeyId)
    {
      UnregisterHotKey(nullptr, d.hotkeyId);
      d.hotkeyId = 0;
    }
    // (No DDC/CI handle to release: each call takes its own -- see OpenPhysical.)
  }
  InterlockedExchange(&g_offCount, 0);
  if (g_wmi)
  {
    g_wmi->Release();
    g_wmi = nullptr;
    g_wmiUsable = false;
  }
  CoUninitialize();
  LogEvent("control thread stopped; ramps restored, dark windows destroyed, shortcuts released");
  return 0;
}

// ---------------------------------------------------------------------------------------------
// THE APPLICATIONS THAT ARE MAKING SOUND -- the system volume mixer, in this program
// ---------------------------------------------------------------------------------------------
//
// ⚠️ THIS HALF LIVES ON THE HOST'S UI THREAD INSTEAD OF THE CONTROL THREAD, and the split is by cost: a WASAPI
// session walk is a COM query over the processes that own an audio stream, which is milliseconds -- not the
// tenths of a second a DDC write can take. The page asks for these controls, so the page's own thread is where
// they are read, and no hand-off is needed.
//
// ⚠️ IT FOLLOWS THE DEFAULT OUTPUT DEVICE, WHICH IS THE USER'S OWN REQUIREMENT: "确定要做到切换声卡，它会也跟着
// 切换系统保存的各应用音量". The endpoint is re-fetched on every enumeration and the sessions are whatever that
// endpoint has, so switching sound cards switches the list -- and each application's volume is the number
// WINDOWS keeps for it on that device, because that is what the session itself reports.
//
// ⚠️ AND THE LIST IS SORTED SO THAT IT DOES NOT SHUFFLE. `sessions[i]` is a `setControl` PATH: the page draws
// what the last document said and sends the index back, so an index that moves between the two would change
// the volume of the wrong program. The system-sounds session is pinned first (as Windows' own mixer does) and
// everything else is ordered by name, which is a stable order that does not depend on the order COM happened
// to enumerate the machine in.

// ⚠️ ONE PLACE TURNS A SESSION'S 0..1 VOLUME INTO THE 0..100 THE CONTROLS USE, AND EVERY READER GOES THROUGH
// IT. The first version wrote the arithmetic out at each site, and the flyout's copy forgot the `(int)` cast --
// so a fader was sent `100.5`, which is outside its own `max`. That is the kind of value the host is right not
// to trust: a control whose value is off its own scale is a control that lies about where it is. The clamp is
// here for the same reason: a session CAN report above 1.0, and the page's range is 0..100.
int SessionPercent(float v)
{
  int p = (int)(v * 100.0f + 0.5f);
  if (p < 0)
    p = 0;
  if (p > 100)
    p = 100;
  return p;
}

void ReleaseSession(Session &s)
{
  for (int i = 0; i < s.volCount; ++i)
    if (s.vol[i])
    {
      s.vol[i]->Release();
      s.vol[i] = nullptr;
    }
  s.volCount = 0;
}

void ReleaseAllSessions()
{
  for (int i = 0; i < g_sessCount; ++i)
    ReleaseSession(g_sess[i]);
  g_sessCount = 0;
}

// The bare process name of an audio session's owner, as the user knows it: "chrome.exe". Empty when the
// process cannot be opened (it can be gone, or protected), in which case the caller falls back to the pid.
void SessionProcessName(DWORD pid, char *out, int outSize)
{
  out[0] = 0;
  if (pid == 0)
    return;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return;
  wchar_t wide[512] = {0};
  DWORD n = 512;
  if (QueryFullProcessImageNameW(h, 0, wide, &n))
  {
    char utf8[1024] = {0};
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)sizeof(utf8) - 1, nullptr, nullptr) > 0)
      apex::match::NormaliseName(utf8, out, outSize); // folds case and strips the path -- one rule, one place
  }
  CloseHandle(h);
}

// (The two system-sounds strings are declared with SessionLabel, above: three surfaces draw that row's name.)

// ⚠️ THERE IS NO `force` PARAMETER ANY MORE (ABI 17 -> 18). It existed for one caller -- the volume group's
// "refresh" button, which asked for a fresh enumeration regardless of the throttle -- and that button is gone:
// the group declares itself `live` and the page re-reads on its own, so every caller now wants the throttled
// behaviour. A parameter no caller ever passes true is a second behaviour nobody exercises.
void RefreshSessions()
{
  const DWORD now = GetTickCount();
  if (g_sessStamp && (now - g_sessStamp) < kSessionCacheMs)
    return; // see kSessionCacheMs: this is called from `quickItems`, which runs on every frame of a drag
  g_sessStamp = now;

  ReleaseAllSessions();

  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void **>(&en))) ||
      !en)
    return;
  IMMDevice *dev = nullptr;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev)
  {
    en->Release();
    return;
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
      for (int i = 0; i < count && g_sessCount < kMaxSessions; ++i)
      {
        IAudioSessionControl *ctrl = nullptr;
        if (FAILED(sessions->GetSession(i, &ctrl)) || !ctrl)
          continue;
        ISimpleAudioVolume *sv = nullptr;
        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&sv))) &&
            sv)
        {
          DWORD pid = 0;
          // ⚠️⚠️ "IS THIS THE SYSTEM-SOUNDS SESSION?" IS ASKED OF WASAPI, NOT INFERRED FROM THE PID.
          // The first version decided it with `pid == 0`, which is what that session usually is on the machine
          // this was written on -- and on the user's machine the same session reports **pid 8**, so it was taken
          // for an ordinary application: its process name could not be read, the row fell back to "(pid 8)", it
          // lost its pinned place at the top of the list, and its bilingual name went with it. The user's report
          // was three words long: "系统音量变成 pid8". `IsSystemSoundsSession` is the API for this question and
          // it does not care what the pid happens to be.
          BOOL isSystem = FALSE;
          IAudioSessionControl2 *ctrl2 = nullptr;
          if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                             reinterpret_cast<void **>(&ctrl2))) &&
              ctrl2)
          {
            ctrl2->GetProcessId(&pid);
            isSystem = (ctrl2->IsSystemSoundsSession() == S_OK);
            ctrl2->Release();
          }

          char key[128] = {0};
          const bool system = (isSystem != FALSE);
          if (system)
            _snprintf(key, sizeof(key), "#system");
          else
          {
            char exe[96] = {0};
            SessionProcessName(pid, exe, (int)sizeof(exe));
            if (exe[0])
              _snprintf(key, sizeof(key), "%s", exe);
            else
              _snprintf(key, sizeof(key), "#pid%lu", (unsigned long)pid);
          }

          // One row per APPLICATION, not per session: a program that opens two streams is one slider in
          // Windows' mixer, and two sliders that move the same application's volume separately would be a
          // worse mixer than the system's.
          Session *dst = nullptr;
          for (int k = 0; k < g_sessCount; ++k)
            if (strcmp(g_sess[k].key, key) == 0)
            {
              dst = &g_sess[k];
              break;
            }
          if (!dst)
          {
            dst = &g_sess[g_sessCount++];
            _snprintf(dst->key, sizeof(dst->key), "%s", key);
            dst->system = system;
            if (system)
            {
              _snprintf(dst->name, sizeof(dst->name), "%s", kSystemSoundsEn);
            }
            else
            {
              // The extension is dropped: "chrome" is what the user calls the program, and the mixer shows it
              // that way too.
              char disp[96] = {0};
              _snprintf(disp, sizeof(disp), "%s", key);
              char *dot = strrchr(disp, '.');
              if (dot && _stricmp(dot, ".exe") == 0)
                *dot = 0;
              if (disp[0] == '#')
                _snprintf(dst->name, sizeof(dst->name), "(pid %lu)", (unsigned long)pid);
              else
                _snprintf(dst->name, sizeof(dst->name), "%s", disp);
            }
          }
          if (dst->volCount < kMaxHandlesPerSession)
          {
            dst->vol[dst->volCount++] = sv; // kept AddRef'd; released on the next enumeration or at shutdown
          }
          else
          {
            sv->Release();
          }
          // The displayed value is the first session's: sessions of one application that disagree are rare, and
          // picking one of them is what Windows' own mixer does.
          if (dst->volCount == 1)
          {
            float v = 1.0f;
            BOOL m = FALSE;
            sv->GetMasterVolume(&v);
            sv->GetMute(&m);
            dst->volume = v;
            dst->mute = (m != FALSE);
          }
        }
        ctrl->Release();
      }
      sessions->Release();
    }
    mgr->Release();
  }
  dev->Release();
  en->Release();

  // Sort -- the system row first, then by name (see the note above about stable indices).
  for (int i = 1; i < g_sessCount; ++i)
    for (int j = i; j > 0; --j)
    {
      Session &a = g_sess[j - 1];
      Session &b = g_sess[j];
      const bool swap = (a.system != b.system) ? (!a.system && b.system) : (strcmp(a.name, b.name) > 0);
      if (!swap)
        break;
      Session t = a;
      a = b;
      b = t;
    }

  // ⚠️ AND THE NAMES THE FILE GAVE ARE PUT BACK ON, EVERY TIME THE LIST IS REBUILT. A session list is enumerated
  // from scratch (the connections are re-made), so the alias would be lost on every refresh without this -- and
  // the user's name for a program would survive about two seconds.
  for (int i = 0; i < g_pendingAliasCount; ++i)
    for (int k = 0; k < g_sessCount; ++k)
      if (strcmp(g_sess[k].key, g_pendingAliasKey[i]) == 0)
        _snprintf(g_sess[k].alias, sizeof(g_sess[k].alias), "%s", g_pendingAliasName[i]);
}

// ---------------------------------------------------------------------------------------------
// THE SETTINGS FILE
// ---------------------------------------------------------------------------------------------
//
// ⚠️ WHAT IS *NOT* IN IT: whether a screen is currently dark. A gamma ramp and a black window are both states of
// THIS RUN -- the window dies with the process and the ramp is put back on the way out -- so a program that
// remembered "screen 2 was off" and turned it off again at the next start would be a program that boots with a
// black screen. The level and the shortcut are preferences; the on/off switch is a moment.
//
// `display=<device>|<level>|<shortcut>` -- one line per monitor, keyed on the GDI device name (\\.\DISPLAY1),
// which is the only name that survives a restart and a re-plug. A monitor that is not there when the file is
// read keeps its line: unplugging a laptop from a dock must not throw away the brightness the user chose.

bool ConfigPath(char *out, int outSize)
{
  if (!g_dir[0])
    return false;
  const int n = _snprintf(out, outSize, "%sMediaControl.ini", g_dir);
  return n > 0 && n < outSize;
}

bool ParseBoolText(const char *v, bool *out)
{
  if (!v || !out)
    return false;
  if (strcmp(v, "1") == 0 || _stricmp(v, "true") == 0 || _stricmp(v, "on") == 0 || _stricmp(v, "yes") == 0)
  {
    *out = true;
    return true;
  }
  if (strcmp(v, "0") == 0 || _stricmp(v, "false") == 0 || _stricmp(v, "off") == 0 || _stricmp(v, "no") == 0)
  {
    *out = false;
    return true;
  }
  return false;
}

void LoadSettings();
void ApplyStored(Display &d); // one monitor takes what the file said about it (declared before LoadSettings uses it)

void LoadSettings()
{
  bool qb = false, qv = false;
  // A re-read REPLACES the names rather than adding to them: the file is the truth, and names are also written
  // back from the page as they are typed (see ApplyControl), so a growing list would keep resurrecting a name
  // the user had cleared.
  g_pendingAliasCount = 0;
  g_storedCount = 0;

  char path[560] = {0};
  if (ConfigPath(path, (int)sizeof(path)))
  {
    FILE *f = fopen(path, "rb");
    if (f)
    {
      char line[512] = {0};
      while (fgets(line, sizeof(line) - 1, f))
      {
        char *hash = strchr(line, '#');
        if (hash)
          *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq)
          continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        while (*key == ' ' || *key == '\t')
          ++key;
        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t'))
          *--kend = 0;
        while (*val == ' ' || *val == '\t')
          ++val;
        char *vend = val + strlen(val);
        while (vend > val && (vend[-1] == '\r' || vend[-1] == '\n' || vend[-1] == ' ' || vend[-1] == '\t'))
          *--vend = 0;
        if (!key[0])
          continue;

        if (_stricmp(key, "quick_brightness") == 0)
          ParseBoolText(val, &qb);
        else if (_stricmp(key, "quick_volume") == 0)
          ParseBoolText(val, &qv);
        else if (_stricmp(key, "display") == 0)
        {
          // TWO SHAPES, AND THE FIRST FIELD SAYS WHICH:
          //   the monitor's identity: `HKC-0000-00000000|\\\\.\\DISPLAY2|80|Ctrl+Alt+1|my screen`
          //   the older slot-keyed one: `\\\\.\\DISPLAY1|80|Ctrl+Alt+1|my screen`
          // ⚠️ An identity never starts with `\\.\` (it is a manufacturer/product/serial, or an EDID name), so the
          // test is exact rather than a guess -- and an older file is read correctly, which is the whole point of
          // keeping the fallback: an upgrade must not cost the user their brightness, names or shortcuts.
          // ⚠️ AND A SIXTH FIELD IS ACCEPTED AND IGNORED, WHICH IS NOT SLOPPINESS: a build that shipped for a few
          // hours wrote one there (a per-screen "may Apex dim this" switch that the user then had taken out -- see
          // the note in the Display struct). A file with it must keep working, and the next save simply does not
          // write it: the only field that ever lived there said "yes" for every screen anyway.
          char f[6][96] = {{0}};
          int nf = 0;
          const char *p = val;
          while (nf < 6 && p && *p)
          {
            const char *bar = strchr(p, '|');
            const size_t len = bar ? (size_t)(bar - p) : strlen(p);
            if (len < sizeof(f[0]))
            {
              memcpy(f[nf], p, len);
              f[nf][len] = 0;
            }
            ++nf;
            p = bar ? bar + 1 : nullptr;
          }
          StoredDisplay sd;
          if (nf >= 5 && strncmp(f[0], "\\\\", 2) != 0)
          {
            _snprintf(sd.identity, sizeof(sd.identity), "%s", f[0]);
            _snprintf(sd.device, sizeof(sd.device), "%s", f[1]);
            sd.level = atoi(f[2]);
            _snprintf(sd.hotkey, sizeof(sd.hotkey), "%s", f[3]);
            _snprintf(sd.alias, sizeof(sd.alias), "%s", f[4]);
          }
          else
          {
            // The older shape: the device name was the key.
            _snprintf(sd.identity, sizeof(sd.identity), "%s", "");
            _snprintf(sd.device, sizeof(sd.device), "%s", f[0]);
            sd.level = atoi(f[1]);
            _snprintf(sd.hotkey, sizeof(sd.hotkey), "%s", f[2]);
            _snprintf(sd.alias, sizeof(sd.alias), "%s", nf >= 4 ? f[3] : "");
          }
          if (sd.level < 0)
            sd.level = 0;
          if (sd.level > 100)
            sd.level = 100;
          if (g_storedCount < kMaxDisplays)
            g_stored[g_storedCount++] = sd;
        }
        else if (_stricmp(key, "session") == 0)
        {
          // `session=chrome.exe|browser` -- the user's own name for an application. ⚠️ KEYED BY THE PROGRAM, not
          // by a session id: a session id is different every time the program starts, so a name kept against one
          // would be lost the next morning.
          char sessKey[128] = {0};
          int n = 0;
          while (val[n] && val[n] != '|' && n < (int)sizeof(sessKey) - 1)
          {
            sessKey[n] = val[n];
            ++n;
          }
          sessKey[n] = 0;
          const char *bar = strchr(val, '|');
          const char *name = bar ? bar + 1 : "";
          for (int i = 0; i < g_sessCount; ++i)
            if (strcmp(g_sess[i].key, sessKey) == 0)
              _snprintf(g_sess[i].alias, sizeof(g_sess[i].alias), "%s", name);
          // ⚠️ AND A NAME FOR A PROGRAM THAT IS NOT RUNNING RIGHT NOW IS NOT LOST: the file is read BEFORE the
          // first session enumeration in some runs and after it in others, so the names are collected first and
          // applied again after every refresh (see ApplyPendingSessionAliases).
          if (g_pendingAliasCount < kMaxSessions)
            _snprintf(g_pendingAliasKey[g_pendingAliasCount], sizeof(g_pendingAliasKey[0]), "%s", sessKey),
                _snprintf(g_pendingAliasName[g_pendingAliasCount], sizeof(g_pendingAliasName[0]), "%s", name),
                ++g_pendingAliasCount;
        }
        // anything else: ignored on purpose, like every other settings file here
      }
      fclose(f);
    }
  }

  EnterCriticalSection(&g_lock);
  g_quickBrightness = qb;
  g_quickVolume = qv;
  // ⚠️ AND EVERY MONITOR THAT IS ALREADY KNOWN TAKES ITS OWN SETTINGS AGAIN -- but only if it has not already
  // taken them (see ApplyStored): a re-read of the file is when the file is allowed to speak, and that is here.
  for (int i = 0; i < g_dispCount; ++i)
  {
    g_disp[i].storedApplied = false;
    ApplyStored(g_disp[i]);
  }
  LeaveCriticalSection(&g_lock);
  if (g_wake)
    SetEvent(g_wake);
}

// Give one monitor whatever the settings file said about it -- ⚠️ ONCE, THE FIRST TIME IT IS RECOGNISED.
//
// ⚠️⚠️ "ONCE" IS THE WHOLE POINT, AND IT WAS THE BUG THE USER FOUND: "BOE0A8D屏自定义名字不能保存". That screen is
// driven through gamma, and a gamma-fallback screen is RE-PROBED every 15 seconds (kReprobeMs, because a failed
// probe is not a verdict). Every one of those probes applied the file again -- so a name the user had just typed
// was overwritten by the older name in the file before anything had saved it, and the page's own re-read then
// showed the old one. The file is a STARTING POINT, not a truth that keeps reasserting itself over the page.
//
// ⚠️ It is reset when the file is re-read (`LoadSettings`), so a genuine reload does apply it again.
void ApplyStored(Display &d)
{
  if (d.storedApplied)
    return;
  d.storedApplied = true;
  const StoredDisplay *best = nullptr;
  for (int i = 0; i < g_storedCount; ++i)
  {
    if (d.identity[0] && g_stored[i].identity[0] && strcmp(d.identity, g_stored[i].identity) == 0)
    {
      best = &g_stored[i];
      break;
    }
  }
  if (!best)
    for (int i = 0; i < g_storedCount; ++i)
      if (g_stored[i].device[0] && strcmp(d.device, g_stored[i].device) == 0)
      {
        best = &g_stored[i];
        break;
      }
  if (!best)
    return;
  d.level = best->level;
  d.appliedLevel = -1; // it has to be written out again: the file's value is the truth now
  _snprintf(d.hotkey, sizeof(d.hotkey), "%s", best->hotkey);
  _snprintf(d.alias, sizeof(d.alias), "%s", best->alias);
}

bool SaveSettings()
{
  char path[560] = {0};
  if (!ConfigPath(path, (int)sizeof(path)))
    return false;
  char tmp[600] = {0};
  if (_snprintf(tmp, sizeof(tmp), "%s.tmp", path) <= 0)
    return false;
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return false;
  fprintf(f, "# MediaControl -- this feature's settings. Written by the panel, and hand-editable.\n");
  fprintf(f, "#\n");
  fprintf(f, "# display=<monitor identity>|<gdi device>|<brightness 0-100>|<shortcut>|<your name>\n");
  fprintf(f, "#   One line per monitor. ⚠️ THE KEY IS THE MONITOR ITSELF (manufacturer-product-serial, read from\n");
  fprintf(f, "#   the panel through WMI), NOT `\\\\.\\DISPLAY1` -- that is a SLOT, and it moves when screens are\n");
  fprintf(f, "#   plugged, unplugged or rearranged, which would drag the settings onto the wrong screen. The GDI\n");
  fprintf(f, "#   name is kept beside it as a fallback for a panel that will not say what it is.\n");
  fprintf(f, "#   `display=HKC-0000-00000000|\\\\.\\DISPLAY2|80|Ctrl+Alt+1|desk`. A monitor that is not connected\n");
  fprintf(f, "#   right now keeps its line, so unplugging a dock does not throw the brightness away.\n");
  fprintf(f, "#   (A sixth field written by an early build of this version is read and ignored -- see LoadSettings.)\n");
  fprintf(f, "#   WHETHER A SCREEN IS DARK RIGHT NOW IS NOT STORED: it belongs to the running program.\n");
  fprintf(f, "# session=<program>|<your name> -- your own name for an application, shown in the quick panel\n");
  fprintf(f, "#   and on the page. Keyed by the program, not by a session id, so it survives a restart.\n");
  fprintf(f, "# quick_brightness / quick_volume put that group's faders in the quick panel (the flyout the\n");
  fprintf(f, "#   tray icon shows); 0 -- the default -- keeps them out of there.\n");
  EnterCriticalSection(&g_lock);
  const bool qb = g_quickBrightness, qv = g_quickVolume;
  for (int i = 0; i < g_dispCount; ++i)
    fprintf(f, "display=%s|%s|%d|%s|%s\n", g_disp[i].identity[0] ? g_disp[i].identity : g_disp[i].device,
            g_disp[i].device, g_disp[i].level, g_disp[i].hotkey, g_disp[i].alias);
  LeaveCriticalSection(&g_lock);
  for (int i = 0; i < g_sessCount; ++i)
    if (g_sess[i].alias[0])
      fprintf(f, "session=%s|%s\n", g_sess[i].key, g_sess[i].alias);
  fprintf(f, "quick_brightness=%d\n", qb ? 1 : 0);
  fprintf(f, "quick_volume=%d\n", qv ? 1 : 0);
  fclose(f);
  return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}

// ---------------------------------------------------------------------------------------------
// THE PANEL'S DOCUMENT
// ---------------------------------------------------------------------------------------------

void AppendText(char *out, int outSize, int &off, const char *fmt, ...)
{
  if (off >= outSize - 1)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + off, outSize - off, fmt, ap);
  va_end(ap);
  if (n > 0)
    off += n;
  if (off > outSize - 1)
    off = outSize - 1;
}

void AppendJsonString(char *out, int outSize, int &off, const char *s)
{
  AppendText(out, outSize, off, "\"");
  for (const char *p = s ? s : ""; *p; ++p)
  {
    if (*p == '"' || *p == '\\')
      AppendText(out, outSize, off, "\\%c", *p);
    else if ((unsigned char)*p < 0x20)
      AppendText(out, outSize, off, "\\u%04x", (unsigned char)*p);
    else
      AppendText(out, outSize, off, "%c", *p);
  }
  AppendText(out, outSize, off, "\"");
}

// One `bool` control that decides whether a group of this feature's controls may appear in the quick panel.
// THE SWITCH THAT MAPS A WHOLE GROUP INTO THE QUICK PANEL (`group.quick` in apex/abi.h, ABI 16 -> 17), written
// INSIDE the group it belongs to. The page draws it at the right-hand end of that group's own line -- which for
// these groups is their heading, because the rows come from the machine and there is nothing to add.
//
// ⚠️ IT REPLACED TWO TOP-LEVEL PARAMETERS (`quick_brightness` / `quick_volume`, drawn as a pair of switches at the
// bottom of the page), AND THE USER'S REASON IS THE WHOLE ARGUMENT FOR THIS KEY: "媒体控制插件的「快速面板」开关，
// 位置移到亮度和音量各自小标题的右侧，居右". A switch that maps one group belongs ON that group, where the thing it
// governs is; two switches about two different groups had ended up in a stack of their own, away from both.
//
// ⚠️ THE LABEL IS NOW JUST 快速面板, NOT "快速面板：亮度". The old label had to repeat its group's name because it sat
// apart from it; on the group's own heading that would be the heading said twice.
//
// ⚠️ AND THE `id`s DID NOT CHANGE (`quick_brightness` / `quick_volume`), so a user who had a group mapped keeps it
// mapped -- this moved the switch, it did not replace the setting.
void AddGroupQuickSwitch(char *out, int outSize, int &off, const char *id, bool value)
{
  AppendText(out, outSize, off, ",\"quick\":{\"id\":");
  AppendJsonString(out, outSize, off, id);
  AppendText(out, outSize, off, ",\"labelZh\":");
  AppendJsonString(out, outSize, off, "快速面板");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Quick panel");
  AppendText(out, outSize, off, ",\"value\":%d}", value ? 1 : 0);
}

void AddRangeField(char *out, int outSize, int &off, const char *id, const char *zh, const char *en,
                   const char *unit, unsigned hue)
{
  // The page wants the colour as CSS text and the flyout wants the same colour as a number (see ApexQuickItem),
  // so the number is the one home and this is the one conversion -- two literals would be two colours the day
  // one of them is edited.
  char hex[16] = {0};
  _snprintf(hex, sizeof(hex), "#%06X", hue & 0xFFFFFFu);
  AppendText(out, outSize, off, "{\"id\":");
  AppendJsonString(out, outSize, off, id);
  AppendText(out, outSize, off, ",\"type\":\"range\",\"min\":0,\"max\":100,\"step\":1,\"unit\":");
  AppendJsonString(out, outSize, off, unit);
  AppendText(out, outSize, off, ",\"hue\":");
  AppendJsonString(out, outSize, off, hex);
  AppendText(out, outSize, off, ",\"labelZh\":");
  AppendJsonString(out, outSize, off, zh);
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, en);
  AppendText(out, outSize, off, "}");
}

void AddBoolField(char *out, int outSize, int &off, const char *id, const char *zh, const char *en)
{
  AppendText(out, outSize, off, "{\"id\":");
  AppendJsonString(out, outSize, off, id);
  AppendText(out, outSize, off, ",\"type\":\"bool\",\"labelZh\":");
  AppendJsonString(out, outSize, off, zh);
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, en);
  AppendText(out, outSize, off, "}");
}

// A `text` field, which inside a rows group is the user's own name for that row (the ABI's `text` control; the
// page draws it as a one-line box and sends the typed value back through `setControl`).
void AddTextField(char *out, int outSize, int &off, const char *id, const char *zh, const char *en,
                  const char *phZh, const char *phEn)
{
  AppendText(out, outSize, off, "{\"id\":");
  AppendJsonString(out, outSize, off, id);
  AppendText(out, outSize, off, ",\"type\":\"text\",\"labelZh\":");
  AppendJsonString(out, outSize, off, zh);
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, en);
  AppendText(out, outSize, off, ",\"placeholderZh\":");
  AppendJsonString(out, outSize, off, phZh);
  AppendText(out, outSize, off, ",\"placeholderEn\":");
  AppendJsonString(out, outSize, off, phEn);
  AppendText(out, outSize, off, "}");
}

int SettingsJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  RefreshSessions();

  // ⚠️ THE TWO MAP SWITCHES ARE READ HERE, ONCE, AND THE DOCUMENT USES THEM WHERE THEY BELONG. They used to be
  // read near the end, where the two top-level switches they described were emitted; now each is written INSIDE
  // its own group (see AddGroupQuickSwitch), so the value has to be in hand before the first group is built.
  bool qb = false, qv = false;
  EnterCriticalSection(&g_lock);
  qb = g_quickBrightness;
  qv = g_quickVolume;
  LeaveCriticalSection(&g_lock);

  int off = 0;
  AppendText(out, outSize, off, "{\"params\":[");

  // ---- 1. BRIGHTNESS, one row per monitor ------------------------------------------------------
  //
  // ⚠️ `layout:"rows"` AND `locked` ON EVERY ROW. A monitor is not something the user added and cannot be
  // something they remove -- the rows come from the machine, and a per-monitor "remove" button would be a
  // button that means "unplug the screen". `locked` is exactly that statement in the ABI (see abi.h): no
  // remove button, and the feature refuses a remove as well.
  // ⚠️ `noAdd` ON ALL THREE GROUPS (apex/abi.h, ABI 13 -> 14): not one of these rows is the user's to create.
  // The page used to draw an Add button above each of them because it had no way to be told otherwise, and the
  // button could only ever ask for a monitor that does not exist, a screen that is not connected, or an
  // application that is not making sound -- all of which this feature refuses. The refusal is what makes the
  // button harmless and what makes it useless; `noAdd` is how the page is told not to offer it.
  AppendText(out, outSize, off, "{\"id\":\"displays\",\"type\":\"group\",\"layout\":\"rows\",\"noAdd\":true");
  // ⚠️⚠️ `waiting` WHILE A SCREEN IS DARK, AND IT IS THE WHOLE MECHANISM BEHIND THE USER'S
  // "解除熄屏，开关状态要跟着关掉".
  //
  // A screen is turned back on by a click on the dark window itself -- which happens in THIS process, on the
  // control thread, with no page involved. The page cannot know: it drew a switch that says "on" and nothing
  // will tell it otherwise until the user navigates away and back. The ABI has no "the feature changed
  // something, re-read me" call, and adding one would be a new channel for a single case.
  //
  // `waiting` already means exactly the right thing (apex/abi.h): "I am still waiting for something outside the
  // page -- keep asking". The panel polls while it is there, so the switch follows the screen within 400 ms,
  // and the poll STOPS the moment it is gone (which is the same delivery that says the screen is back). It was
  // written for a click-capture in another program; a click on a dark screen is the same shape of fact.
  // ⚠️⚠️ `waiting` WHILE THE PAGE HAS SOMETHING TO FOLLOW, AND THERE ARE NOW TWO SUCH THINGS (see `waiting` in
  // abi.h for what the field means to the panel).
  //
  //   * A SCREEN IS DARK: it is turned back on by a click on the dark window itself, in this process, with no
  //     page involved -- the page cannot know, so it is told to keep asking (the user's "解除熄屏，开关状态要跟着
  //     关掉").
  //   * THE SCREEN WAS MOVED BY SOMETHING ELSE (its own buttons, the laptop's brightness keys, a vendor tool):
  //     PollHardware adopts the real value, and the slider has to follow it while the user is pressing those
  //     keys (the user's "它两在调节时，推子是否可以相互实时更新状态"). `externalUntil` bounds it, so a monitor
  //     button pressed five minutes ago does not leave the panel polling for ever.
  {
    const DWORD now = GetTickCount();
    bool waiting = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_dispCount; ++i)
      if (g_disp[i].off || (g_disp[i].externalUntil && now < g_disp[i].externalUntil))
        waiting = true;
    LeaveCriticalSection(&g_lock);
    if (waiting)
      AppendText(out, outSize, off, ",\"waiting\":\"display\"");
  }
  AppendText(out, outSize, off, ",\"labelZh\":");
  AppendJsonString(out, outSize, off, "亮度");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Brightness");
  // ... and this group's own switch for the quick panel, on this group's own heading (see AddGroupQuickSwitch).
  AddGroupQuickSwitch(out, outSize, off, "quick_brightness", qb);
  AppendText(out, outSize, off, ",\"rowToggle\":[\"off\"],\"fields\":[");
  // ⚠️ THE NAME FIELD COMES FIRST AND CARRIES NO LABEL, WHICH IS THE USER'S OWN ARRANGEMENT: "把它放在每个设备的
  // 「亮度」「音量」位置就很合适，那个「名字」提示的也可以去掉" -- it sits where a label would, so the row reads
  // "SDC4190 2880x1800 | <your name> | fader 100% | screen off" instead of growing a sixth column. The page skips
  // an empty label (see textRow).
  AddTextField(out, outSize, off, "alias", "", "", "自定义", "custom");
  AppendText(out, outSize, off, ",");
  // ⚠️ NO LABEL ON THE FADER EITHER, AND THAT IS THE USER'S SECOND CUT AT THIS ROW: "设置里自定义名字输入框右边的
  // 「亮度」「音量」文字标签去掉，占水平位置，现在控件超出面板了". The row already says what it is -- the device's name
  // is at its left and "100 %" is at its right, and there is only one fader per row -- so the two words were
  // spending about eighty pixels to repeat the obvious, and pushing the row past the panel's right edge. The page
  // draws no label for an empty one (see rangeRow).
  AddRangeField(out, outSize, off, "brightness", "", "", "%", kHueBrightness);
  AppendText(out, outSize, off, ",");
  AddBoolField(out, outSize, off, "off", "熄屏", "Screen off");
  AppendText(out, outSize, off, "],\"items\":[");
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_dispCount; ++i)
  {
    char label[96] = {0};
    DisplayLabel(g_disp[i], label, (int)sizeof(label));
    AppendText(out, outSize, off, "%s{\"title\":", i ? "," : "");
    AppendJsonString(out, outSize, off, label);
    AppendText(out, outSize, off, ",\"locked\":true,\"values\":{\"brightness\":%d,\"off\":%d,\"alias\":",
               g_disp[i].level, g_disp[i].off ? 1 : 0);
    AppendJsonString(out, outSize, off, g_disp[i].alias);
    AppendText(out, outSize, off, "}}");
  }
  LeaveCriticalSection(&g_lock);
  // ⚠️ ONE COMMA CONVENTION FOR THE WHOLE DOCUMENT, AND IT IS "EVERY ITEM BUT THE FIRST CARRIES ITS OWN LEADING
  // COMMA". The first version mixed the two styles -- this group ended with a trailing comma while the helpers
  // below start with a leading one -- and the result was `]},,{`: an EMPTY PAGE, because a document that does
  // not parse is not a document. Nothing in the feature could see it (it assembles a string), and the panel
  // draws what it is given, so "the page is blank" was the only symptom. The gate that should have caught it is
  // `check_feature_chart.sh`, and it did not, because it only ever parsed ONE feature's document -- see the
  // note added there.
  AppendText(out, outSize, off, "]}");

  // (There was a "software dimming" group here: one switch per gamma-fallback screen, saying whether Apex was
  // allowed to dim it at all. It is gone -- see the note where the switch used to live in the Display struct. Worth
  // one line here because the group's ABSENCE is the decision: a control that has to be explained, whose default
  // was already "on", is worth less than the behaviour it guards.)

  // ---- 2. THE SCREEN-OFF SHORTCUTS, one row per monitor ----
  //
  // A group of its own rather than a third field in the row above: a fader, a shortcut box and a switch on one
  // line is three different readings of "this monitor", and the fader would be the one that paid for it. The
  // rows are the monitors again, so the two groups line up line for line.
  AppendText(out, outSize, off, ",{\"id\":\"offkeys\",\"type\":\"group\",\"layout\":\"rows\",\"noAdd\":true,\"labelZh\":");
  AppendJsonString(out, outSize, off, "熄屏快捷键");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Screen-off shortcuts");
  AppendText(out, outSize, off, ",\"fields\":[{\"id\":\"hotkey\",\"type\":\"hotkey\",\"labelZh\":");
  // ⚠️ THE SHORTCUT BOX CARRIES NO LABEL OF ITS OWN, WHICH IS THE SAME ARRANGEMENT AS THE NAME BOX AND THE FADER
  // IN THE GROUP ABOVE: the user's "每个设备后面有个「快捷键」的文字去掉" -- the row already says what it is (the
  // device is at its left, the box is where a shortcut goes and there is one control per row), and the two
  // characters were spending width that the middle of the row does not need to spend.
  AppendJsonString(out, outSize, off, "");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "");
  AppendText(out, outSize, off, ",\"placeholderZh\":");
  // ⚠️ AND THE HINT IS SHORT ENOUGH TO BE READ IN A BOX SIZED FOR A SHORTCUT. It used to be "点击后按下快捷键
  // （留空 = 不用）", which needs a box twice as wide as any combination does -- and the box is now as wide as the
  // longest thing it will hold (the user's "够装快捷键字就好，中间可以留空"), so a long hint would simply be cut
  // off. What it must still say is the one thing that is not obvious: you click it, and then you press the keys.
  // (Clearing it is Del or Backspace -- the user asked for that -- and an empty box is visible as empty.)
  AppendJsonString(out, outSize, off, "点击后按键");
  AppendText(out, outSize, off, ",\"placeholderEn\":");
  AppendJsonString(out, outSize, off, "click, then press");
  AppendText(out, outSize, off, "}],\"items\":[");
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_dispCount; ++i)
  {
    AppendText(out, outSize, off, "%s{\"title\":", i ? "," : "");
    AppendJsonString(out, outSize, off, g_disp[i].name);
    AppendText(out, outSize, off, ",\"locked\":true,\"values\":{\"hotkey\":");
    AppendJsonString(out, outSize, off, g_disp[i].hotkey);
    AppendText(out, outSize, off, "}}");
  }
  LeaveCriticalSection(&g_lock);
  AppendText(out, outSize, off, "]}");
  // (THE TWO TOP-LEVEL MAP SWITCHES USED TO BE EMITTED HERE -- "快速面板：亮度" and "快速面板：音量", a stack of
  // their own at the bottom of the page. They are now one `quick` object inside each of the two groups they
  // describe, which is where the user asked for them: "位置移到亮度和音量各自小标题的右侧，居右". A group above
  // carries its own; see AddGroupQuickSwitch.)
  // (There was a third switch here -- "screen off via the monitor's own power, some monitors blink and
  // re-handshake" -- and the user took it out: "熄屏用显示器电源这个方案也去掉，这样会变成断开显示器。统一采用现在的方
  // 案。" The blink it warned about IS the display link dropping, which is the one thing the requirement rules out,
  // so leaving it to a switch meant leaving a per-machine way to break the requirement by accident.)

  // ---- 4. THE APPLICATIONS' VOLUME ---------------------------------------------------------
  //
  // ⚠️ THE ROWS ARE LOCKED TOO, and here it means something slightly different: a row is not the user's to
  // delete because it is not the user's to create either -- it is there because the program is making sound.
  //
  // ⚠️⚠️ AND `live` IS WHAT KEEPS THAT LIST TRUE, WHICH REPLACED A BUTTON (ABI 17 -> 18). The group used to
  // declare an action -- "刷新应用列表" / "Refresh applications" -- and the user's report was the button's whole
  // problem in one sentence: "媒体控制列表的「刷新应用列表」按钮去掉，这个做成实时自动刷新". A list of the programs
  // that are making sound is not a document with a version; it is a fact about the machine that changes while
  // the user is looking at it, so asking them to press a button to be told it is asking them to keep pressing
  // it. `live` tells the PAGE that these rows are a picture of something outside it, and the page re-reads
  // (about once a second) for as long as the group is on screen -- see the note on `live` in apex/abi.h.
  //
  // ⚠️ AND NOTHING ON THIS SIDE HAD TO CHANGE TO MAKE IT WORK: `RefreshSessions()` is already called at the
  // top of SettingsJson, and it already throttles itself (kSessionCacheMs, 800 ms) because `quickItems` runs on
  // every frame of a fader drag. So the page's asking is what the button used to do, minus the button.
  AppendText(out, outSize, off, ",{\"id\":\"sessions\",\"type\":\"group\",\"layout\":\"rows\",\"noAdd\":true,\"live\":true,\"labelZh\":");
  AppendJsonString(out, outSize, off, "音量");
  AppendText(out, outSize, off, ",\"labelEn\":");
  AppendJsonString(out, outSize, off, "Volume");
  // ... and this group's own switch for the quick panel, exactly as on the brightness group above.
  AddGroupQuickSwitch(out, outSize, off, "quick_volume", qv);
  AppendText(out, outSize, off, ",\"rowToggle\":[\"mute\"],\"fields\":[");
  AddTextField(out, outSize, off, "alias", "", "", "自定义", "custom"); // first and unlabelled, as above
  AppendText(out, outSize, off, ",");
  AddRangeField(out, outSize, off, "volume", "", "", "%", kHueVolume); // unlabelled too, for the same reason
  AppendText(out, outSize, off, ",");
  AddBoolField(out, outSize, off, "mute", "静音", "Mute");
  AppendText(out, outSize, off, "],\"items\":[");
  for (int i = 0; i < g_sessCount; ++i)
  {
    const Session &s = g_sess[i];
    AppendText(out, outSize, off, "%s{", i ? "," : "");
    if (s.system && !s.alias[0])
    {
      // ⚠️ THE SYSTEM-SOUNDS ROW IS THE ONE WHOSE DEFAULT TITLE IS THIS FEATURE'S OWN WORDS, so it travels in
      // both languages and the page picks (apex/abi.h: `titleZh`/`titleEn`). The user found the inconsistency
      // this fixes: with an alias it is the user's own word like every other row, and without one it must follow
      // the interface language in BOTH surfaces -- the flyout had been handed the English string for both.
      AppendText(out, outSize, off, "\"titleZh\":");
      AppendJsonString(out, outSize, off, kSystemSoundsZh);
      AppendText(out, outSize, off, ",\"titleEn\":");
      AppendJsonString(out, outSize, off, kSystemSoundsEn);
    }
    else
    {
      char label[96] = {0};
      SessionLabel(s, true, label, (int)sizeof(label));
      AppendText(out, outSize, off, "\"title\":");
      AppendJsonString(out, outSize, off, label);
    }
    const int vol = SessionPercent(s.volume);
    AppendText(out, outSize, off, ",\"locked\":true,\"values\":{\"volume\":%d,\"mute\":%d,\"alias\":", vol,
               s.mute ? 1 : 0);
    AppendJsonString(out, outSize, off, s.alias);
    AppendText(out, outSize, off, "}}");
  }
  AppendText(out, outSize, off, "]}");

  AppendText(out, outSize, off, "],\"settingsFile\":\"MediaControl.ini\",\"summaryZh\":");
  AppendJsonString(out, outSize, off, "每个显示器的亮度和熄屏；每个应用的音量");
  AppendText(out, outSize, off, ",\"summaryEn\":");
  AppendJsonString(out, outSize, off,
                   "Brightness and screen-off for every monitor, and the volume of every application");
  AppendText(out, outSize, off, "}");
  if (off >= outSize)
    return 0; // a truncated document must be an error, not something the page tries to parse
  return off;
}

// ---------------------------------------------------------------------------------------------
// SETTING A CONTROL
// ---------------------------------------------------------------------------------------------

// ⚠️ `atoi` IS NOT A PARSER, AND THE DIFFERENCE IS A CONTROL THAT LIES. `atoi("abc")` is 0 -- a perfectly
// legal brightness -- so a value this feature cannot read would be silently accepted as "off the scale dark"
// instead of refused. The gate caught exactly that ("abc" was accepted), which is the whole reason `setControl`
// answers 0 to mean "no": the page re-reads the control afterwards and puts back what is really stored.
bool ParseIntText(const char *v, int *out)
{
  if (!v || !out || !v[0])
    return false;
  const char *p = v;
  if (*p == '+' || *p == '-')
    ++p;
  if (!*p)
    return false;
  for (const char *q = p; *q; ++q)
    if (*q < '0' || *q > '9')
      return false;
  *out = atoi(v);
  return true;
}

// "displays[2].brightness" -> group match, index 2, field "brightness".
bool SplitIndexed(const char *path, const char *group, int *index, const char **field)
{
  const size_t g = strlen(group);
  if (strncmp(path, group, g) != 0 || path[g] != '[')
    return false;
  const char *close = strchr(path, ']');
  if (!close || close[1] != '.')
    return false;
  // The index is parsed as strictly as a value is: "displays[x]" must not quietly mean displays[0].
  char num[16] = {0};
  const size_t len = (size_t)(close - (path + g + 1));
  if (len == 0 || len >= sizeof(num))
    return false;
  memcpy(num, path + g + 1, len);
  if (!ParseIntText(num, index))
    return false;
  *field = close + 2;
  return true;
}

int ApplyControl(const char *path, const char *value)
{
  if (!path || !value)
    return 0;

  // ---- the two quick-panel permissions: ordinary bools, read by `QuickItems` ----
  if (strcmp(path, "quick_brightness") == 0 || strcmp(path, "quick_volume") == 0)
  {
    bool v = false;
    if (!ParseBoolText(value, &v))
      return 0;
    EnterCriticalSection(&g_lock);
    if (strcmp(path, "quick_brightness") == 0)
      g_quickBrightness = v;
    else
      g_quickVolume = v;
    LeaveCriticalSection(&g_lock);
    if (g_wake)
      SetEvent(g_wake); // the flyout's own contents change with it; let the control thread look again
    return 1;
  }

  // ---- the monitors ----
  int index = 0;
  const char *field = nullptr;
  if (SplitIndexed(path, "displays", &index, &field))
  {
    // ⚠️⚠️ TURNING A SCREEN OFF GOES THROUGH `SetOffLocked` AND NOT THROUGH A WRITE OF ITS OWN, AND THAT IS NOT
    // TIDINESS. This function used to set `g_disp[index].off` right here -- which meant TWO places changed the
    // one piece of state the tray mark is computed from, and only one of them recomputed the mark. The result
    // was a mark that stayed lit after the screen came back: the gate caught it, stably, as "and the host is
    // told it is not held any more: FAIL". One writer, one recomputation, both in the function whose name says
    // what it does.
    if (strcmp(field, "off") == 0)
    {
      bool v = false;
      if (!ParseBoolText(value, &v))
        return 0;
      if (index < 0 || index >= g_dispCount)
        return 0;
      SetOffLocked(index, v); // takes the lock, refreshes the mark and wakes the control thread itself
      return 1;
    }

    EnterCriticalSection(&g_lock);
    const bool inRange = (index >= 0 && index < g_dispCount);
    bool ok = false;
    if (inRange && strcmp(field, "alias") == 0)
    {
      // ⚠️ THE USER'S NAME FOR THIS SCREEN, TRIMMED AND LENGTH-LIMITED, and EMPTY IS ACCEPTED -- it means "no
      // opinion", which is how a name is taken back. Refusing an empty value would make a name impossible to
      // remove, and refusing a long one would leave the page showing something the feature does not have (the
      // contract is: refuse => the page re-reads and shows the truth, so a silent truncation is the one thing
      // not allowed).
      char trimmed[64] = {0};
      int n = 0;
      const char *p = value;
      while (*p == ' ' || *p == '\t')
        ++p;
      for (; *p && n < (int)sizeof(trimmed) - 1; ++p)
        trimmed[n++] = *p;
      while (n > 0 && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t'))
        --n;
      trimmed[n] = 0;
      // ⚠️ THE SEPARATOR IS STRIPPED FROM A USER-SUPPLIED NAME, because this file's one-line-per-thing format uses
      // it: a name containing `|` would silently become a name plus a field, and the field it produced would be
      // nonsense the next time the file was read. Replaced rather than refused -- a stray `|` is not worth
      // rejecting a whole name over, and the page re-reads and shows what was really stored.
      for (int i = 0; trimmed[i]; ++i)
        if (trimmed[i] == '|')
          trimmed[i] = ' ';
      _snprintf(g_disp[index].alias, sizeof(g_disp[index].alias), "%s", trimmed);
      ok = true;
    }
    else if (inRange && strcmp(field, "brightness") == 0)
    {
      int level = 0;
      if (!ParseIntText(value, &level) || level < 0 || level > 100)
        ok = false; // refuse rather than clamp: the page re-reads and shows what is really stored
      else
      {
        g_disp[index].level = level;
        ok = true;
      }
    }
    LeaveCriticalSection(&g_lock);
    if (ok && g_wake)
      SetEvent(g_wake); // the control thread does the talking to Windows (see the note on the thread)
    return ok ? 1 : 0;
  }

  // (There was a `softdim` branch here -- `softdim[0].on`, addressed through a derived list of the gamma-fallback
  // screens. Both the group and the branch are gone; see the note in the Display struct.)

  // ---- the screen-off shortcuts ----
  if (SplitIndexed(path, "offkeys", &index, &field))
  {
    if (strcmp(field, "hotkey") != 0)
      return 0;
    // ⚠️ VALIDATED HERE, NOT ON THE PAGE. A combination this feature cannot register is REFUSED (the page
    // re-reads the control and puts back what is really stored); a combination with no modifier is refused too,
    // because a global shortcut on a bare letter fires while the user is typing.
    unsigned mods = 0, vk = 0;
    if (value[0] && !ParseHotkey(value, &mods, &vk))
      return 0;
    EnterCriticalSection(&g_lock);
    const bool inRange = (index >= 0 && index < g_dispCount);
    if (inRange)
      _snprintf(g_disp[index].hotkey, sizeof(g_disp[index].hotkey), "%s", value);
    LeaveCriticalSection(&g_lock);
    if (inRange && g_wake)
      SetEvent(g_wake); // register/unregister on the thread that owns the message queue
    return inRange ? 1 : 0;
  }

  // ---- the applications' volume ----
  if (SplitIndexed(path, "sessions", &index, &field))
  {
    if (index < 0 || index >= g_sessCount)
      return 0; // the list moved since the page read it: refuse rather than change the wrong program
    Session &s = g_sess[index];
    if (strcmp(field, "volume") == 0)
    {
      int v = 0;
      if (!ParseIntText(value, &v) || v < 0 || v > 100)
        return 0;
      const float f = (float)v / 100.0f;
      for (int i = 0; i < s.volCount; ++i)
        s.vol[i]->SetMasterVolume(f, nullptr);
      s.volume = f;
      return 1;
    }
    if (strcmp(field, "mute") == 0)
    {
      bool v = false;
      if (!ParseBoolText(value, &v))
        return 0;
      for (int i = 0; i < s.volCount; ++i)
        s.vol[i]->SetMute(v ? TRUE : FALSE, nullptr);
      s.mute = v;
      return 1;
    }
    if (strcmp(field, "alias") == 0)
    {
      // The same rule as a monitor's name: trimmed, empty means "no opinion" (which is how it is taken back).
      char trimmed[64] = {0};
      int n = 0;
      const char *p = value;
      while (*p == ' ' || *p == '\t')
        ++p;
      for (; *p && n < (int)sizeof(trimmed) - 1; ++p)
        trimmed[n++] = *p;
      while (n > 0 && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t'))
        --n;
      trimmed[n] = 0;
      for (int i = 0; trimmed[i]; ++i)
        if (trimmed[i] == '|')
          trimmed[i] = ' ';
      _snprintf(s.alias, sizeof(s.alias), "%s", trimmed);
      return 1;
    }
    return 0;
  }

  return 0;
}

// (THERE IS NO `ListOp` HERE ANY MORE, AND ITS ABSENCE IS THE DECISION. It had one op -- "refresh", sent by the
// button in the volume group's bar -- and that button is gone: the group declares `live` instead, so the PAGE
// keeps asking (see the note where the group is described, and `live` in apex/abi.h). A function that can only
// refuse is a function the next reader has to work out is dead.)

// ---------------------------------------------------------------------------------------------
// THE QUICK PANEL
// ---------------------------------------------------------------------------------------------
//
// ⚠️ TWO NAMED GROUPS, AND THAT IS WHAT THE USER ASKED FOR IN SO MANY WORDS: "这一组做一个快速面板开关，只要亮度
// 控制映射到快速面板，熄屏不用，分组名称为「亮度」" -- and the same for the volume half. So there is one switch
// per HALF (not one per monitor, and not one per application), and the switch that is off sends nothing at all
// (see abi.h: the permission is the switch, and the feature simply does not send what was not mapped).
//
// ⚠️ SCREEN-OFF IS DELIBERATELY ABSENT. The user said so ("熄屏不用"), and it also happens to be the right call
// for a panel the user reaches into without looking: the way back from a dark screen must not be a control that
// is itself on the dark screen.
static void QuickCopy(char *dst, int cap, const char *src)
{
  int n = 0;
  if (src)
    for (; src[n] && n < cap - 1; ++n)
      dst[n] = src[n];
  dst[n] = 0;
}

static void QuickSlider(ApexQuickItem *q, const char *id, const char *zh, const char *en, const char *groupZh,
                        const char *groupEn, double value, unsigned hue)
{
  ZeroMemory(q, sizeof(*q));
  QuickCopy(q->id, (int)sizeof(q->id), id);
  QuickCopy(q->labelZh, (int)sizeof(q->labelZh), zh);
  QuickCopy(q->labelEn, (int)sizeof(q->labelEn), en);
  QuickCopy(q->groupZh, (int)sizeof(q->groupZh), groupZh);
  QuickCopy(q->groupEn, (int)sizeof(q->groupEn), groupEn);
  QuickCopy(q->unit, (int)sizeof(q->unit), "%");
  q->type = APEX_QUICK_SLIDER;
  q->min = 0.0;
  q->max = 100.0;
  q->step = 1.0;
  q->value = value;
  q->hue = hue;
}

// ⚠️ AND THE SWITCH THAT GOES IN THE SAME ROW (ABI 14 -> 15). Two of them, one per thing the user asked for:
//
//   * the MUTE BUTTON on a volume fader -- "音量在推子右边增加静音按钮";
//   * the SCREEN-OFF control on a brightness fader -- "快速面板的熄屏功能也像静音按钮一样，做上去", asked for by
//     comparison with the first, which is why the two have to look related and must not look the same.
//
// ⚠️⚠️ AND THE SCREEN-OFF ONE REVERSES AN EARLIER DECISION, WHICH IS WORTH REMEMBERING RATHER THAN QUIETLY
// OVERWRITING. The flyout used to have no screen-off at all, and the reason was sound: "the way back from a dark
// screen must not be a control that is itself on the dark screen". What changed is the SHAPE of the request --
// as a switch of its own it would have been a row per monitor saying "off", which is exactly the trap; as a small
// button on the brightness fader of the row you are already looking at, it is one more thing in a row you use for
// that monitor anyway, and the user asked for it after living with the faders. The way back is unchanged and does
// not depend on this button: a click on the dark screen, Esc, or that screen's own shortcut.
//
// ⚠️ THE PATH IS THE SAME `displays[i].off` / `sessions[i].mute` THE SETTINGS PAGE WRITES, so there is one value
// with one home: the flyout's button and the page's own switch are two views of it, and whichever is used, the
// other catches up by reading the feature (the flyout polls, the page re-reads when it is told).
static void QuickMute(ApexQuickItem *q, int sessionIndex, bool muted)
{
  _snprintf(q->toggleId, sizeof(q->toggleId), "sessions[%d].mute", sessionIndex);
  q->toggleOn = muted ? 1 : 0;
  q->toggleIcon = APEX_QUICK_ICON_MUTE;
}

static void QuickScreenOff(ApexQuickItem *q, int displayIndex, bool off)
{
  _snprintf(q->toggleId, sizeof(q->toggleId), "displays[%d].off", displayIndex);
  q->toggleOn = off ? 1 : 0;
  q->toggleIcon = APEX_QUICK_ICON_DISPLAY;
}

int QuickItems(ApexQuickItem *out, int max)
{
  RefreshSessions(); // throttled: this runs once per frame while a fader is being dragged

  const char *kBrightGroupZh = "亮度";
  const char *kBrightGroupEn = "Brightness";
  const char *kVolumeGroupZh = "音量";
  const char *kVolumeGroupEn = "Volume";

  EnterCriticalSection(&g_lock);
  const bool qb = g_quickBrightness, qv = g_quickVolume;
  const int dispCount = g_dispCount;
  const int sessCount = g_sessCount;
  const int total = (qb ? dispCount : 0) + (qv ? sessCount : 0);
  if (out && max > 0)
  {
    int n = 0;
    if (qb)
      for (int i = 0; i < dispCount && n < max; ++i)
      {
        char id[64] = {0};
        char label[96] = {0};
        _snprintf(id, sizeof(id), "displays[%d].brightness", i);
        DisplayLabel(g_disp[i], label, (int)sizeof(label)); // the user's own name, if they gave one
        QuickSlider(&out[n], id, label, label, kBrightGroupZh, kBrightGroupEn, (double)g_disp[i].level,
                    kHueBrightness);
        // ... and that monitor's own screen-off control at the right of the same row (see QuickScreenOff).
        QuickScreenOff(&out[n], i, g_disp[i].off);
        ++n;
      }
    if (qv)
      for (int i = 0; i < sessCount && n < max; ++i)
      {
        char id[64] = {0};
        char zh[96] = {0}, en[96] = {0};
        _snprintf(id, sizeof(id), "sessions[%d].volume", i);
        // ⚠️ BOTH LANGUAGES, AND THAT IS THE FIX FOR "设置里显示的是系统音量，快速面板显示的是 System Volume": the
        // system-sounds row's name is this feature's OWN word, so it has to follow the reader -- which the panel
        // picks, because the feature does not know which language the flyout is in. (This call used to pass the
        // same English string for both.)
        SessionLabel(g_sess[i], true, zh, (int)sizeof(zh));
        SessionLabel(g_sess[i], false, en, (int)sizeof(en));
        QuickSlider(&out[n], id, zh, en, kVolumeGroupZh, kVolumeGroupEn,
                    (double)SessionPercent(g_sess[i].volume), kHueVolume);
        QuickMute(&out[n], i, g_sess[i].mute); // ... and the mute button at the right of that same row
        ++n;
      }
  }
  LeaveCriticalSection(&g_lock);
  return total;
}

// ---------------------------------------------------------------------------------------------
// THE ABI SURFACE
// ---------------------------------------------------------------------------------------------

bool g_comOwned = false; // we called CoInitializeEx and it was ours to undo

int McInit(const ApexHost *host)
{
  g_host = host;
  if (host && host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));
  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  OpenLog();

  // WASAPI lives on this thread (see the note on the session list), so COM has to be up here. The result is
  // remembered rather than checked: RPC_E_CHANGED_MODE means the host already initialised COM with a different
  // threading model, which is fine for every call this feature makes -- and it means the matching
  // CoUninitialize is NOT ours to make.
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  g_comOwned = SUCCEEDED(hr);
  if (FAILED(hr))
    Log("mediacontrol: CoInitializeEx returned 0x%08lx -- the host already owns COM; audio sessions may be "
        "unavailable", (unsigned long)hr);

  // A QUICK SCAN BEFORE THE THREAD STARTS, so the settings page has rows to draw the first time it is opened.
  // No probing here: a DDC probe can take a tenth of a second per monitor and this runs during the host's own
  // startup. The control thread probes on its first pass.
  EnumerateDisplays(false);
  LoadSettings();

  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_thread = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
  if (!g_thread)
  {
    // Without the thread there are no off windows, no shortcuts and no brightness writes -- which is all of the
    // display half. Refusing is the honest answer (see `init` in abi.h).
    Log("mediacontrol: could not start the control thread (error %lu) -- refusing to load",
        (unsigned long)GetLastError());
    return 1; // NON-ZERO MEANS REFUSE
  }
  Log("mediacontrol: started with %d monitor(s); brightness via %s", g_dispCount,
      g_dispCount > 0 && g_disp[0].protocol == kProtoGamma ? "the gamma ramp (no DDC/CI here)" : "the monitor");
  return 0; // READY
}

void McShutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_wake)
    SetEvent(g_wake);
  if (g_thread)
  {
    if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0)
      Log("mediacontrol: the control thread did not stop in time");
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_wake)
  {
    CloseHandle(g_wake);
    g_wake = nullptr;
  }
  // ⚠️ THE SESSION INTERFACES ARE RELEASED HERE, ON THE THREAD THAT CREATED THEM. A COM interface pointer must
  // be released on the thread that owns it, and the thread that enumerated these is this one.
  ReleaseAllSessions();
  if (g_comOwned)
    CoUninitialize();
  if (g_log)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  if (g_lockReady)
    DeleteCriticalSection(&g_lock);
}

int McReload()
{
  LoadSettings();
  return 1;
}

int McSetControl(const char *path, const char *value) { return ApplyControl(path, value); }
int McSave() { return SaveSettings() ? 1 : 0; }

unsigned McFlags()
{
  unsigned f = APEX_FEATURE_ENABLED;
  // ⚠️ AND THE TRAY SAYS A SCREEN IS DARK, WHICH IS THE ONLY PLACE IT CAN BE SAID: the user cannot see a screen
  // that is showing black, so "something of yours is being held" has to be visible somewhere else (see
  // APEX_FEATURE_USER_VISIBLE in abi.h). The mark's middle bar lights up while any screen is off.
  if (InterlockedCompareExchange(&g_offCount, 0, 0) > 0)
    f |= APEX_FEATURE_USER_VISIBLE;
  return f;
}

} // namespace

// ---------------------------------------------------------------------------
// THE EXPORTED STRUCTURE. `structSize` is checked by the host BEFORE anything below is read.
//
// NO onWheel AND NO tick: this feature has nothing to do with the wheel, so it cannot slow the input path down
// by existing. Everything it does happens either on the host's UI thread (the page's own calls) or on its own
// control thread -- never in the hook.
// ---------------------------------------------------------------------------
static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    kId,
    "媒体控制",
    "Media Control",
    kVersion,
    McInit,
    McShutdown,
    McReload,
    SettingsJson,
    McSetControl,
    // ⚠️ NO listOp ANY MORE, AND THAT IS NOT A LOSS OF A CHANNEL -- IT IS THE SAME REQUEST AS `live` SEEN FROM
    // THIS SIDE. The only op this feature ever had was "refresh", which the button in the volume group's bar sent;
    // the button is gone (the user: "「刷新应用列表」按钮去掉，这个做成实时自动刷新") and the group now declares
    // itself `live`, so the page does the asking and there is nothing left to refuse. The ABI's word for this is
    // exactly what it looks like: a feature with no list operations leaves the field null, and the host answers a
    // request that never comes with a plain refusal.
    nullptr, // listOp
    QuickItems,
    McSave,
    nullptr, // onWheel
    nullptr, // tick
    McFlags,
};

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  return &kFeature;
}
