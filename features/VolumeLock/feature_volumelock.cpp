// ---------------------------------------------------------------------------
// VolumeLock -- "THE MOMENT PLATANE UP2 SE APPEARS, PUT ITS VOLUME AT 50%".
//
// ⚠️ THE NAME THE USER SEES IS 音量保护 / Volume Guard, and it is the user's own correction (2026-10-08).
// The first name was 设备音量 / "Device volume", which describes the KNOB rather than the job: what this
// feature is for is protecting a speaker from a device that comes up at 100% -- 「主要是为了保护音箱」. The
// feature's ID stays `VolumeLock` because it is also the folder name, the settings file's stem and the plugin
// id in `apex.ini`; only the two display strings changed, and no user's configuration moved because of it.
//
// THE USER'S REQUEST, in their own words: 「只要它一出现，就把它的设备音量强制调到50%」 and then the reason:
// 「它没有物理旋钮，所以默认100%音量很炸，50%是比较舒服的音量」 Two facts follow from that sentence, and they
// are the whole design:
//
//   1. THE DEVICE HAS NO PHYSICAL KNOB, so the ONLY volume it has is the WASAPI endpoint master -- the slider
//      the Windows volume mixer draws for it. THERE IS NO HARDWARE GAIN TO SET. That was not assumed: the
//      read-only probe `_diag/up2_probe.cpp` walked the endpoint's topology on this machine and found NO
//      IAudioVolumeLevel node at all ("no IAudioVolumeLevel node on this endpoint's path"). So this feature
//      sets `IAudioEndpointVolume::SetMasterVolumeLevelScalar`, and there is no second candidate.
//
//   2. IT IS AT 100% OUT OF THE BOX AND THAT IS PAINFUL, so the correction has to happen WHEN IT APPEARS --
//      not on a timer, and not by fighting the user afterwards. The user chose "once, when it appears"
//      over "keep it pinned": a feature that re-asserts 50% would drag the slider back every time they
//      tried to set it to 70%.
//
// ⚠️⚠️ IT ONLY EVER TOUCHES THAT ONE DEVICE. The user was explicit: 「只要这个设置，其它设备不动�? Everything
// below is written around that promise:
//   * the match is on the endpoint's DEVICE DESCRIPTION, not its friendly name (see EndpointMatches);
//   * a row that does not match is never activated for volume, never written to, and never even opened;
//   * the ONLY write in this file is one SetMasterVolumeLevelScalar on a matched row, and it is read back.
//
// ⚠️ WHY THE MATCH IS NOT ON THE FRIENDLY NAME, and this is the single most important measurement in the
// file. On this machine every one of these endpoints is called "扬声�?/ Speakers" -- and the string that
// actually says PLATANE UP2 SE lives in a DIFFERENT property ({b3f8fa53-...},6). Worse, the friendly names
// that DO carry it are inconsistent: measured across the three endpoint instances on one machine --
//
//     "扬声�?(PLATANE UP2 SE)"      the one that was plugged in
//     "扬声�?(2- PLATANE UP2 SE)"   a DIFFERENT USB PORT, with Windows' own "2-" disambiguating prefix
//     "扬声�?(PLATANE UP2 SE)"      a third port
//
// -- so a friendly-name match either misses the port the device is actually in or catches a row that is not
// the one that just appeared. The device description is the same string for all three, which is what makes it
// the right key: it identifies the DEVICE, while the endpoint id identifies the PORT.
//
// ⚠️ AND THE DESCRIPTION IS NOT AVAILABLE FROM THE PROPERTY STORE, which cost a measurement to find: the
// store returns "(none)" for these rows. It is read out of the registry instead, under the endpoint's own id.
// See EndpointDescription.
//
// HOW IT LEARNS THAT THE DEVICE APPEARED -- EVENT DRIVEN, NOT POLLED:
//   RegNotifyChangeKeyValue on ...\MMDevices\Audio\Render. Plugging the device in, enabling it in the sound
//   control panel, or its driver re-registering all touch that key. The worker blocks in
//   WaitForMultipleObjects on that event plus its own shutdown event, so while nothing is happening this
//   costs one sleeping thread and NO periodic work at all -- which matters because this feature is idle
//   essentially all of the time. A poll would be the obvious alternative and it is the wrong shape: it wakes
//   up 30 times a minute forever to discover nothing.
//
//   ⚠️ THE NOTIFICATION IS COALESCED AND UNRELIABLE IN COUNT, so it is treated as a HINT rather than as an
//   event log: on every wake-up the whole endpoint list is re-read and compared against what this feature
//   last saw. That is also what makes "it was already plugged in when Apex started" work without a special
//   case -- see the FIRST-SIGHT note in the worker.
//
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "abi.h"

// ---------------------------------------------------------------------------------------------
// IDENTITY
// ---------------------------------------------------------------------------------------------
//
// ⚠️ NOTHING HERE IS SHARED WITH ANOTHER FEATURE. The user's rule is 「每个插件的进程名字都是独立的，不要混用�?
// this feature has its own id, its own folder, its own settings file, its own log, its own defaults, and it
// reads nobody else's list. The one thing it does reuse is the pure "does this string match this pattern"
// function, which contains no names at all -- see PatternMatch below.

static const char *kId = "VolumeLock";
static const char *kVersion = "1.0.0";

// The device this feature is for. It is a DEFAULT, not a constant: the whole point of the settings page is
// that the user can point it at a different box without a rebuild.
static const char *kDefaultMatch = "PLATANE UP2 SE";

// ⚠️ 50 IS THE USER'S NUMBER, GIVEN WITH ITS REASON: 「默�?00%音量很炸�?0%是比较舒服的音量�?
static const int kDefaultPercent = 50;

// The most rows this feature will remember between passes. The machine this was written on has 25 render
// endpoints, 3 of which match; the cap exists so a runaway enumeration cannot grow the array without end.
static const int kMaxEndpoints = 64;

// ⚠️ HOW MANY DEVICES THE USER MAY PROTECT (the `devices` list below). Deliberately generous for a
// hand-curated list: this is not the enumeration cap, it is how many ROWS the settings page keeps.
static const int kMaxRows = 32;

// ⚠️ THE LONGEST DEVICE DESCRIPTION THIS FEATURE WILL REMEMBER. `PLATANE UP2 SE` is 14; the longest on this
// machine is 24 ("适用于蓝牙�?音频的英特尔® 智音技�? is longer, but that is not a device the user protects).
// 128 leaves room for any realistic name and for the row's own line in the settings file.
static const int kMaxName = 128;

static const char *kSettingsName = "VolumeLock.ini";
static const char *kLogName = "volumelock.log";
static const long kLogCap = 256 * 1024;

// ⚠️ RETRY, BECAUSE THE USER'S RULE IS 「宁可做错，也不要放过�? A device that has just appeared in the registry
// has usually not finished bringing its audio stack up, so the FIRST activation attempt on a real plug-in can
// legitimately fail -- and a feature that gave up there would leave the box at 100% for the exact scenario it
// exists to prevent. Three attempts, 250 ms apart, is about three quarters of a second: long enough for the
// endpoint to answer, short enough that the burst is not the one the user hears.
//
// ⚠️ AND A FAILURE ALSO RE-ARMS (see the seen-table note in Pass), so this is not the only safety net --
// these attempts just make the common case fast instead of waiting for a whole extra pass.
static const int kApplyAttempts = 3;
static const int kApplyRetryMs = 250;

// ⚠️⚠️ THE ROW MODEL, AND WHY IT REPLACED A SINGLE PATTERN (the user asked for it, 2026-10-08).
//
// The first version had ONE match pattern and ONE percentage, which can only protect devices that all want
// the SAME level. The user asked for per-device levels, and then for the list itself to be built by PICKING
// from the devices the machine actually has rather than by typing a name: "这个设备列表默认应该是空的，用户添加
// 可以从已经有的设备里选择添加" and "不要让用户自己填".
//
// ⚠️⚠️ SO THE IDENTITY OF A ROW IS THE DEVICE DESCRIPTION, AND THAT CHOICE WAS MEASURED, NOT PREFERRED.
// `_diag/endpoint_identity_probe.cpp` asked Windows for a stable AND unique identity and the answer on this
// machine is that THERE ISN'T ONE:
//
//   * the ENDPOINT GUID is unique but PER PORT -- the same box in another USB port is a different guid
//     (measured: PLATANE4 7/8 appears twice, under `3&11522C85&0&0000` and `3&2F0C0199&0&0000`);
//   * the DEVICE DESCRIPTION is stable across ports but NOT unique -- two identical boxes share it;
//   * the only value that could be both is the device instance's serial, and on this machine EVERY endpoint's
//     instance tail is PORT-DERIVED (`3&17598EBD&0&0000`), i.e. the hardware publishes no serial at all.
//
// The user chose the description: "用设备描述（推荐）—�?跨口稳定". A device row therefore survives being moved
// to another USB port, and the price -- two identical boxes sharing one row -- is a price they accepted.
struct DeviceRow
{
  char desc[kMaxName] = {0}; // the DEVICE DESCRIPTION -- the row's identity (see the note above)
  bool protect = false;      // ⚠️ DEFAULT OFF: a device is only guarded once the user says so
  int percent = kDefaultPercent;
};

// ---------------------------------------------------------------------------------------------
// PATTERN MATCHING -- the ONE rule this feature shares with the others
// ---------------------------------------------------------------------------------------------
//
// ⚠️ THIS IS THE SAME RULE AS common/match.h AND common/config.h, AND IT IS DUPLICATED HERE FOR A REASON THAT
// IS WORTH STATING PLAINLY: features may include `common/`, but the two implementations that already exist
// are both written for something else -- common/match.h folds process names and strips paths, and
// common/config.h's PatternMatch serves a blacklist of EXECUTABLE NAMES. This feature matches a DEVICE
// DESCRIPTION, which is a display string with spaces and no path at all, so neither is a drop-in.
//
// ⚠️ THE SEMANTICS ARE COPIED EXACTLY, INCLUDING THE PART THAT LOOKS LIKE A DETAIL AND IS NOT:
//   * `*` matches any run of characters, `?` exactly one;
//   * with NO wildcard the match is EXACT, not a substring;
//   * BOTH ENDS ARE ANCHORED.
// The anchoring is the point. A substring match on a device name is the same trap as a substring match on a
// process name: the pattern "SE" would silently select half the audio devices on the machine, and the user
// would see it as "some other device's volume got changed" with nothing to explain it. Anchored, a user who
// writes `PLATANE` gets nothing (so they can see the pattern is wrong) and `PLATANE*` gets what they meant.
//
// ⚠️ AND THE CASE FOLDING IS INSIDE THIS FUNCTION, like PatternMatch has it, because the caller passing an
// already-folded string is an unwritten promise -- and the previous project lost a build to exactly that.
static bool FoldEqual(char a, char b)
{
  if (a >= 'A' && a <= 'Z')
    a = static_cast<char>(a - 'A' + 'a');
  if (b >= 'A' && b <= 'Z')
    b = static_cast<char>(b - 'A' + 'a');
  return a == b;
}

bool PatternMatch(const char *text, const char *pattern)
{
  if (!text || !pattern)
    return false;

  while (*pattern)
  {
    if (*pattern == '*')
    {
      // Collapse runs of '*', then try the rest of the pattern at every position. The recursion is bounded
      // by the string length, and this runs on a handful of short device names per pass -- not per wheel.
      while (*pattern == '*')
        ++pattern;
      if (!*pattern)
        return true;
      for (const char *t = text; ; ++t)
      {
        if (PatternMatch(t, pattern))
          return true;
        if (!*t)
          return false;
      }
    }
    if (!*text)
      return false;
    if (*pattern == '?')
    {
      ++pattern;
      ++text;
      continue;
    }
    if (!FoldEqual(*text, *pattern))
      return false;
    ++pattern;
    ++text;
  }
  // Anchored at the end: the whole text must have been consumed.
  return *text == '\0';
}

// ---------------------------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------------------------

static const ApexHost *g_host = nullptr;
static char g_dir[512] = {0};
static char g_logPath[600] = {0};

static HANDLE g_thread = nullptr;
static HANDLE g_wake = nullptr;      // signalled to end the wait early (shutdown / a settings change)
static HANDLE g_regEvent = nullptr;  // the registry key's change event
static HANDLE g_regKey = nullptr;    // HKEY, opened once, re-armed after every notification
static volatile LONG g_stop = 0;

static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;

struct Settings
{
  // ⚠️ NO `enabled` FIELD, AND THAT IS DELIBERATE -- see the note in SettingsJson. The switch that turns this
  // feature off is the HOST's (the plugin list), and a feature does not keep a second answer to it.
  //
  // ⚠️⚠️ THE LIST IS THE FEATURE, AND IT STARTS EMPTY ON PURPOSE. The user: "这个设备列表默认应该是空的，
  // 用户添加可以从已经有的设备里选择添加。用户没添加到列表的设备不对进行保护（锁定）". A device that is not
  // in this list is NEVER activated for volume, never written to, and never even opened -- which is the same
  // promise the single-pattern version made, now made per device.
  //
  // ⚠️ AN EMPTY LIST IS NOT A BROKEN CONFIGURATION, IT IS THE SHIPPING DEFAULT: this feature protects nothing
  // until the user says which device to protect. That is the safe direction for a feature whose whole effect
  // is changing what the user hears.
  DeviceRow rows[kMaxRows];
  int rowCount = 0;
};

static Settings g_set;

// The endpoints seen on the previous pass, by IMMDevice id, with whether they were ACTIVE then. This is what
// turns "here is the current list" into "THIS ONE JUST APPEARED" -- see the worker.
struct SeenRow
{
  char id[256];
  bool active;
};
static SeenRow g_seen[kMaxEndpoints];
static int g_seenCount = 0;
static bool g_seenValid = false; // false until the first pass has run (see FIRST-SIGHT below)

// Counters for the settings page and the log. They answer "is this feature doing anything at all", which is
// the question a feature whose whole effect is one silent volume change has to be able to answer.
static volatile LONG g_applied = 0;
static volatile LONG g_lastPercent = -1;

// ⚠️ A LIVE SLIDER DRAG, HANDED FROM `setControl` (the host's UI thread) TO THE WORKER.
//
// The user asked for the slider to change the device as it is dragged -- "推子拉的时候，可以直接再次更改对应
// 设备的音�? -- and a drag produces a stream of values, not one. Writing each of them would put a COM call
// into the audio stack per mouse move, on the host's own UI thread, which is both slow and pointless: only
// the LAST value in a burst is a level the user chose.
//
// So `setControl` records the newest request and wakes the worker; the worker performs at most one write per
// pass. That is coalescing rather than throttling-by-timer -- the work is bounded by the worker's own rhythm
// instead of by a Stopwatch, and a single drag of the slider cannot queue up a backlog of writes.
static char g_pendingDesc[kMaxName] = {0};
static int g_pendingPercent = 0;
static bool g_pendingValid = false;

// ⚠️ THE DEVICE DESCRIPTIONS THE MACHINE REPORTED, kept fresh by the worker's own pass. This is what the
// settings page's "add a device" dropdown is built from, so it is the ONLY source of an addable device --
// see DeviceCandidates and AddDeviceByDescription.
//
// ⚠️ DECLARED HERE, ABOVE EVERY USER, BECAUSE ORDER IS THE ONLY THING THAT MAKES THAT TRUE. These were first
// written next to ListOp (much further down the file), and the compiler then reported the USES as undeclared
// -- which reads like a missing feature rather than a missing declaration.
static char g_candidates[kMaxEndpoints][kMaxName];
static int g_candidateCount = 0;
// ⚠️ WHETHER EACH CANDIDATE IS CONNECTED RIGHT NOW, PARALLEL TO g_candidates. The page draws "(not connected)"
// beside the ones that are not, so the user can see why a device they expect to be there is not -- and so that
// adding an absent device is clearly a deliberate act rather than a mistake.
static bool g_candidateOnline[kMaxEndpoints] = {false};
// Bumped whenever the candidate list is replaced, so a reader could tell "same list" from "a new one".
static volatile LONG g_deviceListStamp = 0;

// ---------------------------------------------------------------------------------------------
// LOGGING
// ---------------------------------------------------------------------------------------------

static void Log(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[360] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_host->logLine(buf);
}

// The feature's own log, beside itself. ⚠️ WHY IT EXISTS: this feature's effect is a volume the user may not
// be looking at, and the ONLY evidence that anything happened is that the slider moved. When they ask "did it
// do anything", there has to be a file that answers -- the host's log is shared and rotated.
//
// ⚠️ "w" ON PURPOSE: rewritten each run, and only transitions are written. A log that grows while nothing
// happens buries the one line that matters.
static FILE *g_log = nullptr;

static void OpenLog()
{
  if (!g_dir[0])
    return;
  if (_snprintf(g_logPath, sizeof(g_logPath), "%s%s", g_dir, kLogName) <= 0)
    return;
  g_log = fopen(g_logPath, "w");
}

static void LogLine(const char *fmt, ...)
{
  if (!g_log)
    return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  char text[320] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text) - 1, fmt, ap);
  va_end(ap);
  fprintf(g_log, "%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text);
  if (ftell(g_log) > kLogCap)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  else
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// SETTINGS FILE
// ---------------------------------------------------------------------------------------------

static bool ConfigPath(char *out, int outSize)
{
  if (!g_dir[0])
    return false;
  return _snprintf(out, outSize, "%s%s", g_dir, kSettingsName) > 0;
}

static void ParseBool(const char *v, bool *out)
{
  if (!v || !out)
    return;
  *out = (_stricmp(v, "1") == 0 || _stricmp(v, "true") == 0 || _stricmp(v, "yes") == 0 ||
          _stricmp(v, "on") == 0);
}

// One key per line, `#` starts a comment, unknown keys are ignored -- the same grammar every other feature's
// ini uses, which is what keeps the file hand-editable and forward-compatible.
//
// ⚠️ ONE LINE PER DEVICE: `device=<description>|<protect>|<percent>`. The same shape as MediaControl's
// `display=` rows (a feature's own list, one line each), and the separator inside the line is `|` because a
// DEVICE DESCRIPTION CONTAINS SPACES ("PLATANE UP2 SE") -- a space-separated format would have to quote them.
static void LoadSettings()
{
  Settings fresh; // ⚠️ NO DEFAULT ROW: the shipping state is "protect nothing" (see the note on `rows`)

  char path[600] = {0};
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

        // ⚠️ AN `enabled=` LINE IN AN OLDER FILE IS READ AND IGNORED -- it is an unknown key, which is the
        // same path every other retired key takes (see the note in LoadSettings). That is what makes an
        // upgrade cost nothing: the setting simply stops existing, and the file loses it on the next save.
        if (_stricmp(key, "device") == 0)
        {
          // `<description>|<protect>|<percent>` -- the description FIRST, because it is the only field that can
          // contain arbitrary text and the rest is fixed-shape after it.
          if (fresh.rowCount >= kMaxRows)
            continue; // silently full: the cap is a backstop, and dropping the overflow is better than
                      // overwriting a row the user made earlier in the file.
          char *bar1 = strchr(val, '|');
          if (!bar1)
            continue;
          *bar1 = 0;
          char *protectStr = bar1 + 1;
          char *bar2 = strchr(protectStr, '|');

          DeviceRow row;
          _snprintf(row.desc, sizeof(row.desc), "%s", val);
          if (!row.desc[0])
            continue; // an empty description is not a device

          bool protect = true;
          if (bar2)
          {
            *bar2 = 0;
            ParseBool(protectStr, &protect);
            int p = atoi(bar2 + 1);
            // ⚠️ CLAMPED HERE, NOT IN THE PAGE. The page cannot know what this feature will accept, and a
            // hand-edited file is a supported way to configure it (that is why the format is plain text).
            if (p < 0)
              p = 0;
            if (p > 100)
              p = 100;
            row.percent = p;
          }
          else
          {
            // ⚠️ TWO FIELDS IS AN OLDER/SHORTER LINE, AND IT IS ACCEPTED RATHER THAN DROPPED: a line written
            // by hand as `device=PLATANE UP2 SE|1` clearly means "protect it", and refusing it would make the
            // file's most obvious abbreviation the one thing that silently does nothing.
            ParseBool(protectStr, &protect);
            row.percent = kDefaultPercent;
          }
          row.protect = protect;
          fresh.rows[fresh.rowCount++] = row;
        }
        // `#`-comments and anything unrecognised are ignored, exactly like the other features.
      }
      fclose(f);
    }
  }

  EnterCriticalSection(&g_lock);
  g_set = fresh;
  LeaveCriticalSection(&g_lock);

  // A settings change must not wait for the next device event to take effect, so the worker is woken. It is
  // woken even when nothing needs re-applying: the cost is one pass over a short array.
  if (g_wake)
    SetEvent(g_wake);
}

static bool SaveSettings()
{
  char path[600] = {0};
  if (!ConfigPath(path, (int)sizeof(path)))
    return false;
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;

  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  fprintf(f, "# %s -- settings for the VolumeLock feature.\n", kId);
  fprintf(f, "# A listed device has its volume set when it appears; nothing else is ever touched.\n");
  fprintf(f, "# The list starts EMPTY: a device is protected only once it is added here.\n");
  fprintf(f, "#\n");
  fprintf(f, "# device=<description>|<protect>|<percent>\n");
  fprintf(f, "#   The description is the endpoint's DEVICE DESCRIPTION (not its friendly name) -- it is the\n");
  fprintf(f, "#   row's identity, so a device moved to another USB port keeps its settings.\n");
  fprintf(f, "#   `device=PLATANE UP2 SE|1|50`. A device that is not listed here is left completely alone.\n");
  fprintf(f, "#\n");
  fprintf(f, "# THERE IS NO `enabled` KEY: this feature is switched on and off in the plugin list, which is\n");
  fprintf(f, "# the host's own switch. (A file written by an older build may still carry an `enabled=` line;\n");
  fprintf(f, "# it is read as an unknown key and disappears the next time this file is written.)\n");
  for (int i = 0; i < s.rowCount; ++i)
    fprintf(f, "device=%s|%d|%d\n", s.rows[i].desc, s.rows[i].protect ? 1 : 0, s.rows[i].percent);
  fclose(f);
  return true;
}

// ---------------------------------------------------------------------------------------------
// ENDPOINT ENUMERATION AND THE ONE WRITE
// ---------------------------------------------------------------------------------------------

// ⚠️⚠️ IS THIS DEVICE STILL INSTALLED ON THIS MACHINE, OR WAS IT UNINSTALLED?
//
// THE USER'S QUESTION, which is what this answers (2026-10-08): 「像 Motu pro audio，这个已经卸载了，反而还�?// 列表里，是系统残留？�?-- yes, that is exactly what it is. An audio endpoint OUTLIVES the device: unplugging
// leaves a record, and so does uninstalling, because Windows keeps the endpoint so that a device returning to
// the same port gets its settings back. The user wanted the uninstalled ones gone, and said "如果做不到也没关�?.
//
// ⚠️⚠️ THE METHOD WAS CHOSEN BY MEASUREMENT AFTER TWO WRONG ONES, AND BOTH WRONG ONES ARE WORTH RECORDING:
//
//   1. THE HIGH BITS OF `DeviceState`. Every uninstalled endpoint on this machine reports a state whose high
//      bits are set (`MOTU Pro Audio` = 0x21000004, plain unplugged = 0x00000004), and that correlation looked
//      like an answer -- BUT `mmdeviceapi.h` says `#define DEVICE_STATEMASK_ALL 0xf`, i.e. ONLY THE LOW FOUR
//      BITS ARE DOCUMENTED AS STATE. The rest is an internal flag with no published meaning, and a rule built
//      on it would be a guess that happens to fit twelve samples.
//   2. THE FRIENDLY NAME. Useless here: every row on this machine is a generic "扬声�?.
//
// SO IT ASKS THE PLACE THAT ACTUALLY KNOWS: the device's own instance path (from the endpoint's registry
// properties) names its position under `HKLM\SYSTEM\CurrentControlSet\Enum`, and A DEVICE THAT HAS BEEN
// UNINSTALLED HAS NO ENTRY THERE ANY MORE. Measured: `MOTU Pro Audio`'s `VID_07FD&PID_0005` is absent from
// `Enum\USB` entirely, while the PLATANE ids are all present -- which is the difference between "uninstalled"
// and "unplugged" stated by the system rather than inferred from a flag.
//
// ⚠️ AND IT FAILS TOWARDS SHOWING THE DEVICE: if the instance path cannot be read, or the key cannot be
// opened, the answer is "still installed" -- because hiding a device the user owns is worse than showing one
// they removed, and the list is a menu they choose from, not an automatic action.
static bool DeviceStillInstalled(const char *endpointId)
{
  if (!endpointId || !endpointId[0])
    return true;

  const char *dot = strrchr(endpointId, '.');
  const char *guid = dot ? dot + 1 : endpointId;

  // The device instance path, e.g. `{1}.USB\VID_350D&PID_0022&MI_00\9&2140394C&0&0000`.
  char propPath[512] = {0};
  _snprintf(propPath, sizeof(propPath),
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%s\\Properties", guid);
  wchar_t wprop[600] = {0};
  MultiByteToWideChar(CP_UTF8, 0, propPath, -1, wprop, 599);

  HKEY hp = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wprop, 0, KEY_READ, &hp) != ERROR_SUCCESS)
    return true;
  wchar_t inst[512] = {0};
  DWORD cb = sizeof(inst), type = 0;
  const LONG rc = RegQueryValueExW(hp, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},2", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(inst), &cb);
  RegCloseKey(hp);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb <= 2)
    return true;

  char instUtf8[512] = {0};
  WideCharToMultiByte(CP_UTF8, 0, inst, -1, instUtf8, sizeof(instUtf8) - 1, nullptr, nullptr);

  // ⚠️ `{1}.` IS STRIPPED, AND THE REMAINDER IS THE ENUMERATOR PATH -- `USB\VID_..&PID_..\9&2140394C&0&0000`.
  // The FIRST component (before the first backslash) is the enumerator, and the device either has a key under
  // `Enum\<enumerator>\<hardware id>` or it does not.
  const char *path = instUtf8;
  if (path[0] == '{')
  {
    const char *close = strchr(path, '}');
    if (close && close[1] == '.')
      path = close + 2;
  }
  const char *firstSlash = strchr(path, '\\');
  if (!firstSlash || firstSlash == path)
    return true; // not a shape this rule understands: show the device

  char enumKey[512] = {0};
  _snprintf(enumKey, sizeof(enumKey), "SYSTEM\\CurrentControlSet\\Enum\\%.*s", (int)(firstSlash - path), path);
  wchar_t wenum[600] = {0};
  MultiByteToWideChar(CP_UTF8, 0, enumKey, -1, wenum, 599);

  HKEY he = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wenum, 0, KEY_READ, &he) != ERROR_SUCCESS)
    return true; // no enumerator key: do not hide anything on the strength of a missing parent
  RegCloseKey(he);

  // ⚠️ THE HARDWARE ID IS THE SECOND COMPONENT -- `USB\VID_350D&PID_0022&MI_00` -- and THAT is what the
  // enumerator is keyed by. The instance tail after it (`9&2140394C&0&0000`) is per-PORT, so it is deliberately
  // NOT part of the question: a device plugged into a different port is the same installed device.
  const char *secondSlash = strchr(firstSlash + 1, '\\');
  const int hwLen = secondSlash ? (int)(secondSlash - (firstSlash + 1)) : (int)strlen(firstSlash + 1);
  if (hwLen <= 0)
    return true;

  char hwKey[512] = {0};
  _snprintf(hwKey, sizeof(hwKey), "%s\\%.*s", enumKey, hwLen, firstSlash + 1);
  wchar_t whw[600] = {0};
  MultiByteToWideChar(CP_UTF8, 0, hwKey, -1, whw, 599);

  HKEY hh = nullptr;
  const bool installed = (RegOpenKeyExW(HKEY_LOCAL_MACHINE, whw, 0, KEY_READ, &hh) == ERROR_SUCCESS);
  if (hh)
    RegCloseKey(hh);
  return installed;
}

// The endpoint's DEVICE DESCRIPTION, out of the registry. See the header note: the property store does not
// carry this string on this machine, and the description is the only field that is identical across the USB
// ports while the friendly name is not.
//
// ⚠️ THE IMMDevice ID IS `{0.0.0.00000000}.{guid}` AND THE REGISTRY KEY IS NAMED BY THE SAME GUID, braces
// included. Splitting at the last '.' is what turns one into the other.
static bool EndpointDescription(const char *endpointId, char *out, int outSize)
{
  out[0] = 0;
  if (!endpointId || !endpointId[0])
    return false;

  const char *dot = strrchr(endpointId, '.');
  const char *guid = dot ? dot + 1 : endpointId;

  char key[512] = {0};
  _snprintf(key, sizeof(key),
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%s\\Properties", guid);

  wchar_t wkey[600] = {0};
  MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 599);

  HKEY h = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wkey, 0, KEY_READ, &h) != ERROR_SUCCESS)
    return false;

  // {b3f8fa53-...},6 is "PLATANE UP2 SE"; the friendly name is the fallback so that this feature still works
  // on a device whose driver does not publish a description.
  static const wchar_t *kNames[] = {
      L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6",
      L"{a45c254e-df1c-4efd-8020-67d146a850e0},2",
  };
  bool got = false;
  for (const wchar_t *name : kNames)
  {
    wchar_t buf[512] = {0};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && cb > 2)
    {
      WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, outSize, nullptr, nullptr);
      if (out[0])
      {
        got = true;
        break;
      }
    }
  }
  RegCloseKey(h);
  return got;
}

// ⚠️ THE ONLY WRITE IN THIS FILE, AND IT VERIFIES ITSELF. A volume change that is accepted and then ignored
// is indistinguishable from one that worked unless the value is read back -- the same lesson the brightness
// half of MediaControl learned the hard way (a DDC write returns long before the screen moves). So the value
// is read back and the RESULT is what gets logged, not the intent.
static bool ApplyVolume(const char *endpointId, int percent, float *readBackOut)
{
  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
      !en)
    return false;

  wchar_t wid[512] = {0};
  MultiByteToWideChar(CP_UTF8, 0, endpointId, -1, wid, 511);

  IMMDevice *dev = nullptr;
  bool ok = false;
  if (SUCCEEDED(en->GetDevice(wid, &dev)) && dev)
  {
    IAudioEndpointVolume *vol = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(&vol))) &&
        vol)
    {
      const float want = static_cast<float>(percent) / 100.0f;
      HRESULT hr = vol->SetMasterVolumeLevelScalar(want, nullptr);
      if (SUCCEEDED(hr))
      {
        float got = -1.0f;
        if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&got)))
        {
          // The read-back is the answer. A device that reports back something else is reported as what it
          // IS, not as what was asked for.
          if (readBackOut)
            *readBackOut = got;
          ok = (got > want - 0.005f && got < want + 0.005f);
        }
      }
      else
      {
        LogLine("SetMasterVolumeLevelScalar failed on %s (hr=0x%08lx)", endpointId,
                static_cast<unsigned long>(hr));
      }
      vol->Release();
    }
    dev->Release();
  }
  en->Release();
  return ok;
}

// THE USER'S ROW FOR THIS DEVICE, OR NULL IF THEY HAVE NOT ADDED IT. Identity is the device DESCRIPTION --
// see the note on DeviceRow for why that is the only value on this machine that survives a different USB port.
//
// ⚠️ AN EXACT, CASE-FOLDED COMPARISON, DELIBERATELY NOT PatternMatch. The rows are created by PICKING a device
// the machine reported, so there is nothing to be lenient about; a pattern language here would be a second way
// to name a device, and the case folding is there only because Windows itself is not consistent about the case
// of these strings between enumerations.
static const DeviceRow *FindRow(const Settings &s, const char *desc)
{
  if (!desc || !desc[0])
    return nullptr;
  for (int i = 0; i < s.rowCount; ++i)
  {
    if (_stricmp(s.rows[i].desc, desc) == 0)
      return &s.rows[i];
  }
  return nullptr;
}

// Forward declaration: `Pass` applies a slider drag through this, and the definition sits below it with the
// other enumeration helpers.
static void FindEndpointByDescription(const char *desc, char *out, int outSize);

// ONE PASS: read every render endpoint, work out which ones are ACTIVE now, compare against the last pass,
// and apply the volume to any MATCHED row that has just become active.
//
// ⚠️⚠️ FIRST SIGHT APPLIES TOO, AND THAT IS A DELIBERATE READING OF "APPEARS".
//
// The literal reading of 「它一出现�?is "the state changed while Apex was running". The other reading is "the
// device is here and the feature has not dealt with it yet" -- and the second one is what the user actually
// needs, because the situation they described is "it comes up at 100% and that is painful". If Apex happens
// to start after the box is already plugged in (or the box is never unplugged at all), the literal reading
// would leave it at 100% forever and the feature would look broken.
//
// So on the FIRST pass every matched ACTIVE endpoint is applied, and after that only transitions. What this
// deliberately does NOT do is re-assert: once the user moves the slider, nothing here drags it back. The
// user chose that explicitly ("插入/启用时触发一�? over "持续保持"), and it is the difference between a
// feature that corrects a default and one that fights its owner.
static void Pass()
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  // ⚠️ THE HOST'S SWITCH IS THE ONLY ONE THERE IS, AND IT IS ASKED ON EVERY PASS. This feature does its work
  // on its own thread, so nothing else would notice it being switched off in the plugin list -- that is the
  // whole reason `featureEnabled` exists in the ABI, and the same reason KeepAwake asks.
  //
  // ⚠️ IT USED TO BE `s.enabled && ...` -- a second switch of the feature's own, which the user spotted as
  // redundant ("插件列表那个就是插件的总开�?). One question, one answer.
  const bool enabled = !g_host || !g_host->featureEnabled || g_host->featureEnabled(kId) != 0;

  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
      !en)
  {
    LogLine("no MMDeviceEnumerator -- the pass is skipped");
    return;
  }

  IMMDeviceCollection *coll = nullptr;
  // ⚠️ THE FOUR STATES ARE OR'D BY HAND BECAUSE THERE IS NO `DEVICE_STATE_ALL` CONSTANT, and this is NOT the
  // same as asking for ACTIVE alone: the rows that matter are the ones that JUST BECAME active, and a list
  // that only ever contains active rows cannot tell "newly appeared" from "was always there".
  const DWORD kAllStates =
      DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED;

  SeenRow now[kMaxEndpoints];
  int nowCount = 0;

  // ⚠️⚠️ A FAILED ENUMERATION MUST NOT LOOK LIKE "NOTHING IS PLUGGED IN".
  //
  // The seen table is rebuilt from scratch every pass, so a pass that enumerated NOTHING -- because the call
  // failed, or because the cap was hit -- would record "the table is now empty", and on the following pass
  // every device would look BRAND NEW and be re-applied. That is the opposite of the quiet failure: it turns
  // a transient COM error into a volume change on a device the user is listening to.
  //
  // So a pass that could not produce a complete picture KEEPS THE PREVIOUS TABLE and returns without
  // recording anything. Missing one appearance is recoverable (the next pass sees it, because the table still
  // says "not active"); inventing one is not.
  bool complete = true;

  // ⚠️ THE CANDIDATE LIST IS REBUILT EVERY PASS, exactly like the seen table and for the same reason: it is
  // a picture of what the machine reports NOW, and a stale one would offer a device that has been deleted
  // from the system. It is only replaced when the pass completes -- see the note on `complete`.
  char freshCandidates[kMaxEndpoints][kMaxName];
  int freshCandidateCount = 0;
  bool freshCandidateOnline[kMaxEndpoints] = {false};

  if (SUCCEEDED(en->EnumAudioEndpoints(eRender, kAllStates, &coll)) && coll)
  {
    UINT count = 0;
    coll->GetCount(&count);

    // ⚠️ THE CAP IS A FAILURE, NOT A TRUNCATION. If a machine ever has more render endpoints than kMaxEndpoints
    // the table cannot describe it, and a truncated table would make the rows that did not fit look new on the
    // next pass. Saying so is better than silently working on a partial picture.
    if (count > (UINT)kMaxEndpoints)
    {
      LogLine("this machine has %u render endpoints, more than the %d this feature tracks -- the pass is "
              "skipped rather than run on a partial list",
              count, kMaxEndpoints);
      complete = false;
    }

    for (UINT i = 0; complete && i < count && nowCount < kMaxEndpoints; ++i)
    {
      IMMDevice *dev = nullptr;
      if (FAILED(coll->Item(i, &dev)) || !dev)
      {
        // ⚠️ ONE UNREADABLE ROW INVALIDATES THE PASS. It is tempting to `continue` here, and the first version
        // did -- but a row that could not be read is a row whose state is UNKNOWN, and recording it as absent
        // is exactly the mistake described above. (The rows after it are not read either, so the table would
        // also be short.)
        LogLine("could not read render endpoint %u of %u -- the pass is discarded", i, count);
        complete = false;
        break;
      }

      LPWSTR wid = nullptr;
      char id[256] = {0};
      if (SUCCEEDED(dev->GetId(&wid)) && wid)
      {
        WideCharToMultiByte(CP_UTF8, 0, wid, -1, id, sizeof(id) - 1, nullptr, nullptr);
        CoTaskMemFree(wid);
      }

      DWORD state = 0;
      dev->GetState(&state);
      // ⚠️⚠️ THE TEST IS A MASK, NOT AN EQUALITY, AND GETTING IT WRONG IS SILENT.
      //
      // `mmdeviceapi.h` defines the four states as 0x1 / 0x2 / 0x4 / 0x8 and says nothing about the HIGH BITS,
      // but Windows DOES set them on real endpoints: measured on this machine, `NVIDIA High Definition Audio`
      // reports **0x10000001** -- it IS active, and `state == DEVICE_STATE_ACTIVE` answers "no". The endpoint
      // is then skipped, and the device is silently never guarded.
      //
      // ⚠️ THE ENUMERATION MASK WAS ALREADY RIGHT (four states OR'd together cover the low nibble), which is
      // what made this hard to see: the endpoint IS returned by the enumeration, so it looks as though the
      // feature is considering it -- and the equality test one line later throws it away.
      const bool active = (state & DEVICE_STATE_ACTIVE) != 0;

      _snprintf(now[nowCount].id, sizeof(now[nowCount].id), "%s", id);
      now[nowCount].active = active;
      ++nowCount;

      // ⚠️⚠️ EVERY ENDPOINT'S DESCRIPTION IS COLLECTED, ACTIVE OR NOT, AND WHETHER OR NOT IT IS LISTED --
      // this is the CANDIDATE LIST the settings page offers for adding a device (see ListOp).
      //
      // ⚠️ INCLUDING THE ONES THAT ARE NOT ACTIVE IS THE POINT: the user's complaint was about a box that is
      // unplugged most of the time, and a dropdown that only ever offered whatever happened to be plugged in
      // at that moment could not add the very device the feature exists for.
      {
        char candDesc[256] = {0};
        if (EndpointDescription(id, candDesc, (int)sizeof(candDesc)) && candDesc[0])
        {
          // ⚠️⚠️ UNINSTALLED DEVICES ARE LEFT OUT; UNPLUGGED ONES ARE KEPT AND MARKED. The user's request:
          // 「像 Motu pro audio，这个已经卸载了，反而还在列表里�?+ 「都列，但是标出哪些现在不在……但是卸掉的�?          // 不能排除�? See DeviceStillInstalled for how "uninstalled" is told from "unplugged" -- and for why
          // the two obvious shortcuts (the state's high bits, the friendly name) were both rejected.
          if (!DeviceStillInstalled(id))
            continue;

          // ⚠️ DEDUPLICATED BY DESCRIPTION, AND THE ONLINE FLAG IS OR'D FOR A DUPLICATE: the same box on two
          // USB ports is two endpoints and ONE device (that is exactly the fact the row identity is built on),
          // so it is offered once -- and it counts as connected if ANY of its endpoints is.
          int found = -1;
          for (int c = 0; c < freshCandidateCount && found < 0; ++c)
          {
            if (_stricmp(freshCandidates[c], candDesc) == 0)
              found = c;
          }
          if (found >= 0)
          {
            if (active)
              freshCandidateOnline[found] = true;
          }
          else if (freshCandidateCount < kMaxEndpoints)
          {
            _snprintf(freshCandidates[freshCandidateCount], kMaxName, "%s", candDesc);
            freshCandidateOnline[freshCandidateCount] = active;
            ++freshCandidateCount;
          }
        }
      }

      // ⚠️ A ROW THAT IS NOT ACTIVE IS RECORDED AND THEN LEFT ALONE -- not matched, not opened, not written.
      // This is where 「其它设备不动�?is actually enforced, and it is enforced by not getting far enough to do
      // anything, rather than by a check just before the write.
      if (active && enabled && id[0])
      {
        char desc[256] = {0};
        if (EndpointDescription(id, desc, (int)sizeof(desc)))
        {
          // ⚠️⚠️ THE LOOKUP IS BY THE DEVICE DESCRIPTION, EXACTLY AND WITH NO WILDCARDS, AND THE LIST IS THE
          // ONLY THING THAT DECIDES. A device that is NOT in the user's list falls straight through this loop
          // and is never activated, never written to -- which is the promise the single-pattern version made
          // ("其它设备不动") now made per row.
          //
          // ⚠️ AND IT IS AN EXACT COMPARISON, NOT PatternMatch, BECAUSE THE ROWS COME FROM PICKING A REAL
          // DEVICE RATHER THAN FROM TYPING. A pattern language here would be a second way to name a device,
          // and the first version's wildcards existed only because the name had to be typed by hand. The user
          // asked for the opposite ("不要让用户自己填"), so the pattern code is gone from this path.
          const DeviceRow *row = FindRow(s, desc);
          if (row && row->protect)
          {
            // Was it active on the previous pass? If so, it is not "appearing" and nothing is applied.
            bool wasActive = false;
            if (g_seenValid)
            {
              for (int k = 0; k < g_seenCount; ++k)
              {
                if (strcmp(g_seen[k].id, id) == 0)
                {
                  wasActive = g_seen[k].active;
                  break;
                }
              }
            }

            if (!wasActive)
            {
            // ⚠️⚠️ A FAILED WRITE IS RETRIED, BECAUSE THE USER'S RULE IS 「宁可做错，也不要放过�?
            //
            // The first version got exactly one shot: if the device was still coming up when the volume was
            // set -- which is the NORMAL case, since the registry says it is active a moment before its audio
            // stack answers -- the activation could fail, the burst would play at 100%, and nothing would
            // ever try again because the endpoint was now "seen". That is the failure this feature exists to
            // prevent, so it is the one failure that must not be accepted quietly.
            //
            // The retries are bounded (kApplyAttempts, spaced kApplyRetryMs) and the row is then LEFT OUT of
            // the seen table, so the next pass treats it as newly appeared and tries again. Bounded retries
            // with a persistent re-arm is the difference between "keeps trying" and "spins for ever on a
            // device that will never accept a write".
            float got = -1.0f;
            bool ok = false;
            for (int attempt = 0; attempt < kApplyAttempts && !ok; ++attempt)
            {
              if (attempt > 0)
                Sleep(kApplyRetryMs);
              ok = ApplyVolume(id, row->percent, &got);
            }

            if (ok)
            {
              InterlockedIncrement(&g_applied);
              InterlockedExchange(&g_lastPercent, (LONG)(got * 100.0f + 0.5f));
              LogLine("APPLIED: \"%s\" -> asked %d%%, read back %.1f%%  (%s)", desc, row->percent,
                      got * 100.0f,
                      !g_seenValid ? "first sight -- the device was already here when Apex started"
                                   : "the endpoint just became active");
              // ⚠️ THE HOST'S LOG GETS A LINE TOO, because the page and the tray are where the user looks
              // first and this is the one moment this feature does anything at all.
              Log("volumelock: %s volume set to %d%% (asked %d%%)", desc, (int)(got * 100.0f + 0.5f),
                  row->percent);
            }
            else
            {
              LogLine("NOT APPLIED after %d attempt(s): \"%s\" (asked %d%%, last read back %.1f%%) -- the "
                      "endpoint will be treated as newly appeared on the next pass",
                      kApplyAttempts, desc, row->percent, got * 100.0f);
              Log("volumelock: could not set %s to %d%% (it will be retried)", desc, row->percent);
              // ⚠️ THEN IT IS NOT RECORDED AS SEEN-AND-ACTIVE. Dropping the row from the table is what makes
              // the next pass see it as "newly appeared" and try again -- the re-arm is the important half of
              // the retry. (The row is only dropped from the SEEN table, not from the enumeration: nothing
              // about the endpoint itself is changed.)
              now[nowCount - 1].active = false;
            }
          }
        }
      }
      // ⚠️ THIS CLOSES `if (active && enabled && id[0])` -- the four above close the four nested blocks
      // inside it, and the compiler reported the missing brace as "expected '}' before 'else'" at the `else`
      // of the enumeration, several hundred lines away from where it was.
      }
      dev->Release();    }
    coll->Release();
  }
  else
  {
    // The enumeration itself failed. Same rule as the row failures above: this is NOT "there is nothing
    // plugged in", so nothing is recorded.
    LogLine("EnumAudioEndpoints failed -- the pass is discarded and the previous picture is kept");
    complete = false;
  }
  en->Release();

  // ⚠️ ONLY A COMPLETE PASS UPDATES THE TABLE. See the note on `complete`: a partial or failed pass that wrote
  // its result here would make every device look newly appeared on the following pass, and re-apply the volume
  // to hardware the user is listening to.
  if (!complete)
    return;

  memcpy(g_seen, now, sizeof(SeenRow) * nowCount);
  g_seenCount = nowCount;
  g_seenValid = true;

  // The candidate list, for the same reason and under the same rule.
  EnterCriticalSection(&g_lock);
  memcpy(g_candidates, freshCandidates, sizeof(freshCandidates));
  memcpy(g_candidateOnline, freshCandidateOnline, sizeof(freshCandidateOnline));
  g_candidateCount = freshCandidateCount;
  LeaveCriticalSection(&g_lock);
  InterlockedIncrement(&g_deviceListStamp);

  // ⚠️⚠️ AND A SLIDER DRAG THAT ARRIVED SINCE THE LAST PASS IS APPLIED NOW -- this is the "推子拉的时候，可以
  // 直接再次更改对应设备的音�? half of the request, and it is the ONE place in this feature that writes to a
  // device that was already active.
  //
  // ⚠️ WHY IT DOES NOT BREAK THE "ONCE, WHEN IT APPEARS" RULE. That rule exists so the feature does not drag
  // a slider back while the user is setting it -- an automatic re-assert fights its owner. This is the
  // opposite direction: the user moved the control and asked for the device to follow. The distinction is WHO
  // moved it, and it is the whole difference between a guard and an obstacle.
  //
  // ⚠️ AND ONLY IF THE ROW IS STILL PROTECTED, re-checked here rather than trusted from when the drag started,
  // because the switch may have been turned off in between.
  char pendingDesc[kMaxName] = {0};
  int pendingPercent = 0;
  bool havePending = false;
  EnterCriticalSection(&g_lock);
  if (g_pendingValid)
  {
    _snprintf(pendingDesc, sizeof(pendingDesc), "%s", g_pendingDesc);
    pendingPercent = g_pendingPercent;
    havePending = true;
    g_pendingValid = false; // consumed: the next value to arrive supersedes it anyway
  }
  LeaveCriticalSection(&g_lock);

  if (havePending && pendingDesc[0])
  {
    // ⚠️ FOUND BY ENUMERATION, NOT BY REMEMBERING AN ENDPOINT ID. The row is identified by its DESCRIPTION
    // (see DeviceRow), and a device may be plugged into a different port than last time -- so the endpoint is
    // looked up fresh from what this pass just enumerated.
    char targetId[256] = {0};
    FindEndpointByDescription(pendingDesc, targetId, (int)sizeof(targetId));

    if (targetId[0])
    {
      float got = -1.0f;
      const bool ok = ApplyVolume(targetId, pendingPercent, &got);
      LogLine("%s: slider -> \"%s\" asked %d%%, read back %.1f%%", ok ? "SET" : "SET FAILED", pendingDesc,
              pendingPercent, got * 100.0f);
      if (!ok)
        Log("volumelock: could not apply the slider value to %s", pendingDesc);
    }
    else
    {
      // The device is not plugged in right now. That is NOT an error: the value is stored and will be applied
      // when it appears, which is what the feature is for.
      LogLine("slider set \"%s\" to %d%% -- stored; the device is not connected right now", pendingDesc,
              pendingPercent);
    }
  }
}

// THE CURRENT ENDPOINT ID FOR A DEVICE DESCRIPTION, or an empty string if it is not connected right now.
//
// ⚠️ IT ENUMERATES RATHER THAN REMEMBERING. A row is identified by its description, so the endpoint behind it
// can move between USB ports -- and the id from a previous pass would then point at a port the device has
// left. Asking the system again is the only answer that is not a guess, and this runs at most once per pass.
static void FindEndpointByDescription(const char *desc, char *out, int outSize)
{
  out[0] = 0;
  if (!desc || !desc[0])
    return;

  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) ||
      !en)
    return;

  const DWORD kAllStates =
      DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED;
  IMMDeviceCollection *coll = nullptr;
  if (SUCCEEDED(en->EnumAudioEndpoints(eRender, kAllStates, &coll)) && coll)
  {
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count && !out[0]; ++i)
    {
      IMMDevice *dev = nullptr;
      if (FAILED(coll->Item(i, &dev)) || !dev)
        continue;

      DWORD state = 0;
      dev->GetState(&state);
      // ⚠️ ONLY AN ACTIVE ENDPOINT CAN BE WRITTEN TO. Asking for a device that is present-but-not-active
      // would produce a volume for something the user cannot hear, and it is not in the list for that reason.
      // ⚠️ A MASK, NOT AN EQUALITY -- see the note in Pass: real endpoints carry high bits.
      if ((state & DEVICE_STATE_ACTIVE) != 0)
      {
        LPWSTR wid = nullptr;
        char id[256] = {0};
        if (SUCCEEDED(dev->GetId(&wid)) && wid)
        {
          WideCharToMultiByte(CP_UTF8, 0, wid, -1, id, sizeof(id) - 1, nullptr, nullptr);
          CoTaskMemFree(wid);
        }
        char thisDesc[256] = {0};
        if (id[0] && EndpointDescription(id, thisDesc, (int)sizeof(thisDesc)) &&
            _stricmp(thisDesc, desc) == 0)
          _snprintf(out, outSize, "%s", id);
      }
      dev->Release();
    }
    coll->Release();
  }
  en->Release();
}

// ---------------------------------------------------------------------------------------------
// THE WORKER
// ---------------------------------------------------------------------------------------------
//
// It waits on TWO things and never on a timer: the registry's change event (the device appeared or went away)
// and this feature's own wake event (shutdown, or the settings were saved). A timeout of INFINITE is what
// keeps an idle machine idle -- see the header note on why this is not a poll.
//
// ⚠️ THE REGISTRY NOTIFICATION IS ONE-SHOT: RegNotifyChangeKeyValue fires once and then the caller must
// re-arm it. Forgetting to re-arm is the bug this shape invites, and its symptom is the worst kind -- the
// feature works exactly once, and looks like it works until the second time it is needed.
// Forward declaration: the COM trampoline below calls this, and defining the trampoline next to the wait
// loop it wraps is worth the one line.
DWORD WINAPI Worker(LPVOID);

// COM is initialised per thread and the worker is the only thread that touches it. COINIT_MULTITHREADED
// because nothing here needs a message pump or an apartment of its own, and an STA thread that never pumps
// messages deadlocks the marshalling of every call made into another process.
//
// ⚠️ IT IS DONE INSIDE THE WORKER, NOT IN init: entering an apartment on the HOST's main thread would change
// its threading model, and that thread is the one the input hook runs on.
struct ComScope
{
  bool ok = false;
  ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
  ~ComScope()
  {
    if (ok)
      CoUninitialize();
  }
};

static DWORD WINAPI WorkerWithCom(LPVOID p)
{
  ComScope com;
  if (!com.ok)
  {
    LogLine("CoInitializeEx failed -- this feature cannot read audio endpoints");
    return 1;
  }
  return Worker(p);
}

// The thread entry point is chosen here so the COM apartment is entered before any device call. A thin
// trampoline keeps that ordering explicit rather than depending on where a call happens to sit.
static HANDLE StartWorker()
{
  return CreateThread(nullptr, 0, WorkerWithCom, nullptr, 0, nullptr);
}

DWORD WINAPI Worker(LPVOID)
{
  for (;;)
  {
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
      break;

    // Re-arm before waiting. A fresh event handle each round is deliberate: the previous one has already
    // been signalled, and reusing it would return immediately for ever.
    if (g_regEvent)
    {
      CloseHandle(g_regEvent);
      g_regEvent = nullptr;
    }
    if (g_regKey)
    {
      RegCloseKey(static_cast<HKEY>(g_regKey));
      g_regKey = nullptr;
    }

    if (g_wake)
      ResetEvent(g_wake);

    // ⚠️⚠️ THREE WAYS TO ARM THE WATCH, AND ALL THREE END WITH A PASS RUNNING.
    //
    // The notification is the cheap path, and it is the one that normally happens. But the two ways it can
    // fail must NOT both collapse into "wait for ever" or into "poll slowly":
    //
    //   * the KEY cannot be opened (a permissions change, a locked-down machine) -- previously this fell
    //     through with `g_regEvent` still allocated but never armed, so the wait below used INFINITE with an
    //     event that would never be signalled: the feature would run exactly ONE pass and then sleep for the
    //     rest of the session. That is the worst possible failure here -- it looks alive and never fires.
    //   * the NOTIFY call fails -- that one did fall back to a poll, but at 2 s.
    //
    // ⚠️ AND THE POLL IS 500 ms, NOT 2 s, BECAUSE OF THE USER'S OWN RULE: 「宁可做错，也不要放过�? A device
    // arriving at 100% and being corrected half a second later is a brief burst; the same device missed for
    // two seconds is a burst the user actually hears. The poll only runs at all on a machine where the
    // notification could not be armed, so the cost of the shorter interval is paid only where it is needed.
    bool armed = false;
    g_regEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HKEY h = nullptr;
    if (g_regEvent &&
        RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0,
                      KEY_NOTIFY, &h) == ERROR_SUCCESS &&
        h)
    {
      if (RegNotifyChangeKeyValue(h, TRUE, REG_NOTIFY_CHANGE_LAST_SET, g_regEvent, TRUE) == ERROR_SUCCESS)
      {
        g_regKey = h;
        armed = true;
      }
      else
      {
        LogLine("RegNotifyChangeKeyValue failed (error %lu) -- falling back to a 500 ms poll",
                (unsigned long)GetLastError());
        RegCloseKey(h);
      }
    }
    else
    {
      LogLine("could not open the MMDevices Render key for notification (error %lu) -- falling back to a "
              "500 ms poll",
              (unsigned long)GetLastError());
    }

    if (!armed && g_regEvent)
    {
      // Not armed, so nothing will ever signal it: close it, because leaving it around is what made the
      // first version wait on it for ever.
      CloseHandle(g_regEvent);
      g_regEvent = nullptr;
    }

    // ⚠️ THE FIRST PASS HAPPENS BEFORE THE FIRST WAIT, and that ordering is what makes "the device was already
    // plugged in when Apex started" work. Waiting first would mean doing nothing until the next device event,
    // which may be days.
    Pass();

    // ⚠️ AND WHEN THE WATCH IS NOT ARMED THE PASS IS *NOT* SKIPPED WHILE TRYING TO RE-ARM. The loop re-attempts
    // the notification every round (cheap: two registry calls), so a transient failure heals itself rather
    // than leaving the machine on the poll for the rest of the session.
    HANDLE waits[2] = {g_wake, g_regEvent};
    DWORD n = (g_regEvent ? 2 : 1);
    const DWORD timeout = g_regEvent ? INFINITE : 500;
    WaitForMultipleObjects(n, waits, FALSE, timeout);
  }

  if (g_regKey)
  {
    RegCloseKey(static_cast<HKEY>(g_regKey));
    g_regKey = nullptr;
  }
  if (g_regEvent)
  {
    CloseHandle(g_regEvent);
    g_regEvent = nullptr;
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------
// THE SETTINGS PAGE -- what this feature has, described rather than drawn
// ---------------------------------------------------------------------------------------------

static void Append(char *out, int outSize, int &off, const char *fmt, ...)
{
  if (off >= outSize - 1)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + off, outSize - off - 1, fmt, ap);
  va_end(ap);
  if (n > 0)
    off += n;
  if (off > outSize - 1)
    off = outSize - 1;
}

static void AppendJsonString(char *out, int outSize, int &off, const char *s)
{
  Append(out, outSize, off, "\"");
  for (const char *p = s; p && *p; ++p)
  {
    if (*p == '"' || *p == '\\')
      Append(out, outSize, off, "\\%c", *p);
    else if (*p == '\n')
      Append(out, outSize, off, "\\n");
    else
      Append(out, outSize, off, "%c", *p);
  }
  Append(out, outSize, off, "\"");
}

static int SettingsJson(char *out, int outSize)
{
  Settings s;
  EnterCriticalSection(&g_lock);
  s = g_set;
  LeaveCriticalSection(&g_lock);

  if (!out || outSize < 64)
    return 0;
  int off = 0;
  Append(out, outSize, off, "{\"summaryZh\":");
  AppendJsonString(out, outSize, off, "指定设备一出现就把它保护到固定音量，其它设备不动");
  Append(out, outSize, off, ",\"summaryEn\":");
  AppendJsonString(out, outSize, off,
                   "Guards one device at a fixed volume when it appears; no other device is touched");
  Append(out, outSize, off, ",\"params\":[");

  // ⚠️⚠️ THERE IS NO `enabled` CONTROL ON THIS PAGE, AND ITS ABSENCE IS THE POINT (the user, 2026-10-08):
  //
  //     「音量保护插件面板最上面�?启用"功能重复了，可以去掉，每个设备单�?保护"相当于各个设备的启动�?  //       插件列表那个就是插件的总开关�?  //
  // They are right, and it was worse than redundant -- it was a switch that lied. THE PLUGIN LIST'S SWITCH IS
  // THE HOST'S (`apex.ini`'s `off` list), and the host implements "off" by NOT CALLING a feature; this
  // feature's worker asks `featureEnabled` and stops working, so the two agreed in EFFECT. But a second
  // switch in the feature's own file means two answers to one question, and the one on the page is the one
  // whose state the user cannot see in the plugin list -- so "it is switched on in the list but does nothing"
  // was reachable by turning THIS one off, with nothing on screen connecting the two.
  //
  // ⚠️ SO THE ROW'S OWN `protect` IS THE ONLY SWITCH THIS FEATURE OWNS, which matches how the user reads it:
  // the plugin list is the master, and each row decides whether ITS device is guarded.

  // ⚠️⚠️ THE DEVICE LIST -- ONE ROW PER DEVICE THE USER HAS ADDED, EACH WITH ITS OWN LEVEL.
  //
  // `layout:"rows"` is the shape a LIST OF THINGS WITH SWITCHES needs (see apex/abi.h), and it is what makes
  // every field live: no selection, no Edit/Save, and a slider that can be dragged directly. It is the same
  // shape KeepAwake's program list uses, and the reason it is right here is the same -- there is nothing to
  // fill in a form, there is a device and a number.
  //
  // ⚠️ AND `live` IS SET, BECAUSE THE MACHINE'S DEVICE LIST CHANGES UNDER THE PAGE: plugging a box in adds a
  // device, and the user is looking at this page precisely to add it. Without `live` the list they can pick
  // from would be a snapshot from whenever the page opened.
  //
  // ⚠️⚠️ AND `noAdd` IS SET -- THERE IS NO TEXT BOX AND NO "ADD" BUTTON ON THIS GROUP (the user, 2026-10-08):
  //
  //     「插件上面的"从设备列表中选择"是可以填的，还有那个"添加"按钮，这两个控件没什么意思，可以去掉�?  //
  // ⚠️ THEY WERE A LEFTOVER FROM THE FIRST VERSION OF THE LIST, AND THE USER IS RIGHT THAT THEY ARE POINTLESS
  // HERE. `addHint` is the ABI's add control for a `rows` group -- a text box plus a button -- and it was
  // written when the only way to name a device was to type it. This group does not work that way: a device is
  // ADDED BY CHOOSING IT FROM THE `select` BELOW, which is the whole point of "不要让用户自己填".
  //
  // So the group carried TWO add controls: the dropdown that does the job, and a text box that
  // `listOp("add")` refuses. The second one was worse than useless -- it invited the user to type a name,
  // accepted the keystrokes, and then silently did nothing, because the feature refuses free text by design.
  //
  // ⚠️ `noAdd` IS THE RIGHT KEY RATHER THAN "just leave addHint out": without either, the page falls back to
  // a plain "append an item" button (see the else-branch in panel.js), which would ask the FEATURE to invent a
  // device. `noAdd` is exactly "no new rows are created from this group" -- the same thing MediaControl's
  // machine-provided rows say.
  Append(out, outSize, off,
         "{\"id\":\"devices\",\"type\":\"group\",\"layout\":\"rows\",\"live\":true,\"noAdd\":true,");
  AppendJsonString(out, outSize, off, "labelZh");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "受保护的设备");
  Append(out, outSize, off, ",");
  AppendJsonString(out, outSize, off, "labelEn");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "Protected devices");

  // The schema of ONE row. `title` is the device description -- the user's own data, so it has no translation.
  // ⚠️ `locked` IS NOT USED: every row here is the user's to remove (unlike MediaControl's monitors, which are
  // the machine's). Removing a row is how a device stops being protected, and that has to be possible.
  Append(out, outSize, off, ",\"fields\":[");
  Append(out, outSize, off, "{\"id\":\"protect\",\"type\":\"bool\",");
  AppendJsonString(out, outSize, off, "labelZh");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "保护");
  Append(out, outSize, off, ",");
  AppendJsonString(out, outSize, off, "labelEn");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "Guard");
  Append(out, outSize, off, "},");
  Append(out, outSize, off,
         "{\"id\":\"percent\",\"type\":\"range\",\"min\":0,\"max\":100,\"step\":1,\"def\":%d,"
         "\"unit\":\"%%\",\"hue\":\"#78BEFF\",",
         kDefaultPercent);
  AppendJsonString(out, outSize, off, "labelZh");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "音量");
  Append(out, outSize, off, ",");
  AppendJsonString(out, outSize, off, "labelEn");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "Volume");
  Append(out, outSize, off, "}]");

  // The rows themselves.
  Append(out, outSize, off, ",\"items\":[");
  for (int i = 0; i < s.rowCount; ++i)
  {
    Append(out, outSize, off, "%s{\"title\":", i ? "," : "");
    AppendJsonString(out, outSize, off, s.rows[i].desc);
    Append(out, outSize, off, ",\"values\":{\"protect\":%d,\"percent\":%d}}", s.rows[i].protect ? 1 : 0,
           s.rows[i].percent);
  }
  // ⚠️ `]` CLOSES THE ITEMS ARRAY AND `}` CLOSES THE GROUP -- two characters, and leaving one out produced a
  // document that ended `"items":[},`: the page would have drawn no rows at all and said nothing about why.
  // (Caught by dumping the document and reading it -- which is what `_diag/dump_feature_json.cpp` is for. The
  // chart gate counted the brackets and found them balanced, and the document was still wrong.)
  Append(out, outSize, off, "]},");

  // ⚠️ THE CANDIDATE DROPDOWN -- how a device is added without anyone typing its name (see ListOp).
  //
  // ⚠️ IT IS BUILT FROM THE DEVICES THE MACHINE REPORTED, AND A DEVICE ALREADY ON THE LIST IS LEFT OUT: the
  // dropdown's only job is "add one", so offering one that is already there would be a choice that does
  // nothing when taken. When every reported device is already listed, the control says so by being empty --
  // and the page draws an empty dropdown, which is the honest picture.
  //
  // ⚠️ THE `select` TYPE IS THE ABI'S OWN (abi.h lists it among the control types), and `options[].value` is
  // what comes back through `setControl` -- the description itself, never an index (an index would break the
  // moment the machine's device order changed, which it does on every enumeration).
  Append(out, outSize, off, "{\"id\":\"addDevice\",\"type\":\"select\",\"value\":\"\",\"options\":[");
  {
    char pending[kMaxEndpoints][kMaxName];
    bool pendingOnline[kMaxEndpoints] = {false};
    int pendingCount = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_candidateCount && pendingCount < kMaxEndpoints; ++i)
    {
      bool already = false;
      for (int r = 0; r < s.rowCount && !already; ++r)
      {
        if (_stricmp(s.rows[r].desc, g_candidates[i]) == 0)
          already = true;
      }
      if (!already)
      {
        _snprintf(pending[pendingCount], kMaxName, "%s", g_candidates[i]);
        pendingOnline[pendingCount] = g_candidateOnline[i];
        ++pendingCount;
      }
    }
    LeaveCriticalSection(&g_lock);

    for (int i = 0; i < pendingCount; ++i)
    {
      Append(out, outSize, off, "%s{\"value\":", i ? "," : "");
      AppendJsonString(out, outSize, off, pending[i]);
      // ⚠️⚠️ THE LABEL IS THE DESCRIPTION, AND A DEVICE THAT IS NOT CONNECTED SAYS SO (the user, 2026-10-08:
      // 「都列，但是标出哪些现在不在�?. The VALUE stays the bare description -- that is what the feature
      // matches on -- while only the LABEL carries the note, because the two have different jobs: the value
      // identifies a device for the rest of the program's life, and the label is read once by a person.
      //
      // ⚠️ WHY IT MATTERS AT ALL: this device may well be the reason the user opened the page, and a dropdown
      // that silently offers a box that is not plugged in makes "why did nothing happen when I chose it" the
      // next question. Saying so turns it into an expected outcome.
      char labelZh[kMaxName + 32] = {0};
      char labelEn[kMaxName + 32] = {0};
      if (pendingOnline[i])
      {
        _snprintf(labelZh, sizeof(labelZh), "%s", pending[i]);
        _snprintf(labelEn, sizeof(labelEn), "%s", pending[i]);
      }
      else
      {
        _snprintf(labelZh, sizeof(labelZh), "%s（未连接）", pending[i]);
        _snprintf(labelEn, sizeof(labelEn), "%s (not connected)", pending[i]);
      }
      Append(out, outSize, off, ",");
      AppendJsonString(out, outSize, off, "labelZh");
      Append(out, outSize, off, ":");
      AppendJsonString(out, outSize, off, labelZh);
      Append(out, outSize, off, ",");
      AppendJsonString(out, outSize, off, "labelEn");
      Append(out, outSize, off, ":");
      AppendJsonString(out, outSize, off, labelEn);
      Append(out, outSize, off, "}");
    }
  }
  Append(out, outSize, off, "],");
  AppendJsonString(out, outSize, off, "labelZh");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "添加设备");
  Append(out, outSize, off, ",");
  AppendJsonString(out, outSize, off, "labelEn");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "Add a device");
  // ⚠️⚠️ AND THE CONFIRM BUTTON (apex/abi.h, ABI 22 -> 23) -- the user's own shape for it:
  //
  //     「不是给下拉框加"添加"，是在它右侧，另一个，列表选中设备后，点一下"添加"才生效」
  //
  // ⚠️ WHY THIS CONTROL NEEDS ONE AND THE LANGUAGE PICKER DOES NOT. A plain `select` sends the highlighted
  // option the moment it changes -- which is right for "which language" and WRONG for "add a device", for a
  // reason that is easy to miss: AN OPTION THAT IS ALREADY HIGHLIGHTED CANNOT BE CHOSEN, because choosing it
  // changes no value and fires no event. The first candidate in this list was therefore impossible to add, and
  // the control said nothing about it -- the user found it ("默认在预选列表的第一个设备就加不进去").
  //
  // ⚠️ THE LABEL GOES THROUGH THE SAME `AppendJsonString` AS EVERYTHING ELSE, and the two languages are the
  // feature's to give: the button is the feature's word ("添加"), not the page's.
  Append(out, outSize, off, ",");
  AppendJsonString(out, outSize, off, "applyZh");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "添加");
  Append(out, outSize, off, ",");
  AppendJsonString(out, outSize, off, "applyEn");
  Append(out, outSize, off, ":");
  AppendJsonString(out, outSize, off, "Add");
  // ⚠️ `}` CLOSES THE SELECT AND `]` CLOSES `params`. The `]` used to live at the end of the control that was
  // here before this one, and replacing that control took the bracket with it -- leaving a document the parser
  // rejected at the very end, hundreds of characters after the mistake.
  Append(out, outSize, off, "}]");

  // ⚠️ THE COUNTERS ARE PUBLISHED ON THE PAGE RATHER THAN ONLY IN THE LOG, because this feature's effect is
  // invisible: the user changes a setting, and the only way to know it is armed is to unplug the box and plug
  // it back in. The count answers "has it ever fired", and the read-back percentage answers "what did the
  // device say it ended up at" -- which is the difference between "we asked" and "it happened".
  const LONG applied = g_applied;
  const LONG lastPercent = g_lastPercent;
  char noteZh[300] = {0};
  char noteEn[300] = {0};
  if (s.rowCount == 0)
  {
    // ⚠️ THE EMPTY STATE IS THE SHIPPING STATE, SO IT SAYS WHAT TO DO RATHER THAN WHAT IS WRONG. A feature
    // whose list starts empty and says nothing would look broken on first run.
    _snprintf(noteEn, sizeof(noteEn), "No device is protected yet -- add one from the list above.");
    _snprintf(noteZh, sizeof(noteZh), "还没有保护任何设备 —— 从上面的列表里添加一个。");
  }
  else if (applied <= 0)
  {
    _snprintf(noteEn, sizeof(noteEn), "%d device(s) listed. Waiting for one to appear.", s.rowCount);
    _snprintf(noteZh, sizeof(noteZh), "已列出 %d 台设备，等待其中一台出现。", s.rowCount);
  }
  else
  {
    _snprintf(noteEn, sizeof(noteEn), "Applied %ld time(s); last device reported %ld%%.", applied, lastPercent);
    _snprintf(noteZh, sizeof(noteZh), "已应用 %ld 次；上一次设备回报 %ld%%。", applied, lastPercent);
  }
  Append(out, outSize, off, ",\"noteZh\":");
  AppendJsonString(out, outSize, off, noteZh);
  Append(out, outSize, off, ",\"noteEn\":");
  AppendJsonString(out, outSize, off, noteEn);

  Append(out, outSize, off, "}");
  // ⚠️⚠️ THE RETURN VALUE IS THE NUMBER OF BYTES WRITTEN, NOT A SUCCESS FLAG.
  //
  // This is the second place this contract bit, and the symptom was invisible: the gate writes the returned
  // count of bytes out of the buffer, so returning `1` produced a ONE-BYTE document ("{") that the JSON
  // parser then rejected -- while everything inside the feature looked completely correct. The page would
  // have been handed the same one byte.
  //
  // The convention is read off the other features rather than guessed: `SwsSettingsJson` ends with
  // `return off;`. `off` is the append cursor, so it IS the length.
  return off;
}

// ---------------------------------------------------------------------------------------------
// listOp -- the list of devices, and the candidate list the page picks from
// ---------------------------------------------------------------------------------------------
//
// ⚠️⚠️ HOW "CHOOSE FROM THE DEVICES" IS DONE WITHOUT A NEW ABI, AND WHY THIS IS THE RIGHT SHAPE.
//
// The user's requirement was explicit: "不要让用户自己填" -- do NOT make them type a device name -- and the
// ABI's own add control for a `rows` group is A TEXT BOX (`addHint`, see abi.h). Typing is exactly what the
// user ruled out, and it is also how a typo turns into a row that silently never matches anything.
//
// ⚠️ SO THE CANDIDATES TRAVEL AS A `select` CONTROL, WHICH THE ABI ALREADY HAS, AND ADDING A DEVICE IS A
// `select` EDIT. The page draws a dropdown of the devices this machine actually reports; choosing one calls
// `setControl("addDevice", "<description>")`, which appends the row. There is no text box anywhere in the
// path, the value can only be a device that exists, and the feature validates it anyway.
//
// ⚠️ AND THE ROW IS STILL ADDED THROUGH `listOp("add")` FOR THE REMOVAL SIDE TO BE SYMMETRIC: `remove` with an
// index is how a row goes away. `add` itself is refused here, because the only way to name a device that may
// be added is the description -- which arrives through `setControl` where it can be checked against the
// machine's own list. (Refusing is required by the ABI: "a feature must still refuse the op if it arrives
// anyway", because an older page will send it.)
static int ListOp(const char *id, const char *op, const char *value, int index)
{
  if (!id || !op)
    return 0;
  // ⚠️ ONLY THE ONE LIST THIS FEATURE HAS. An id it does not own is refused rather than ignored, so a page
  // mistake cannot silently append to something.
  if (_stricmp(id, "devices") != 0)
    return 0;

  if (_stricmp(op, "add") == 0)
  {
    // ⚠️ REFUSED ON PURPOSE -- the description has to come from the machine's own list, and that check lives
    // in AddDeviceByDescription (reached through `setControl("addDevice", ...)`). Accepting free text here
    // would put the typing back that the user asked to be rid of.
    return 0;
  }

  if (_stricmp(op, "remove") == 0)
  {
    bool removed = false;
    EnterCriticalSection(&g_lock);
    if (index >= 0 && index < g_set.rowCount)
    {
      for (int i = index; i < g_set.rowCount - 1; ++i)
        g_set.rows[i] = g_set.rows[i + 1];
      --g_set.rowCount;
      removed = true;
    }
    LeaveCriticalSection(&g_lock);
    if (removed)
      SaveSettings();
    // ⚠️ AND THE WORKER IS WOKEN EITHER WAY: removing a row changes what should be guarded, and a device the
    // user just took off the list must stop being touched from that moment -- not from the next replug.
    if (g_wake)
      SetEvent(g_wake);
    return removed ? 1 : 0;
  }

  return 0;
}

// ---------------------------------------------------------------------------------------------
// ADDING A DEVICE, BY THE ONLY ROUTE THAT CANNOT MISSPELL ONE
// ---------------------------------------------------------------------------------------------
//
// ⚠️ THE DESCRIPTION IS CHECKED AGAINST THE MACHINE'S OWN LIST RATHER THAN TRUSTED. The page sends back a
// string that came from a control this feature built, so in practice it is always one of them -- but the page
// is not a gatekeeper (abi.h says so in as many words), and a hand-edited settings file can carry anything.
// A row whose description matches no endpoint is a row that silently does nothing, which is the failure this
// check turns into a refusal.
static bool AddDeviceByDescription(const char *desc)
{
  if (!desc || !desc[0])
    return false;

  bool known = false;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_candidateCount && !known; ++i)
  {
    if (_stricmp(g_candidates[i], desc) == 0)
      known = true;
  }
  LeaveCriticalSection(&g_lock);

  if (!known)
  {
    LogLine("refused to add \"%s\": no endpoint on this machine reports that description", desc);
    return false;
  }

  bool added = false;
  EnterCriticalSection(&g_lock);
  // ⚠️ A DUPLICATE IS REFUSED, NOT MERGED: the row's identity IS the description, so two rows with the same
  // one would be two answers to one question -- and the settings file would carry both.
  bool exists = false;
  for (int i = 0; i < g_set.rowCount; ++i)
  {
    if (_stricmp(g_set.rows[i].desc, desc) == 0)
      exists = true;
  }
  if (!exists && g_set.rowCount < kMaxRows)
  {
    DeviceRow &row = g_set.rows[g_set.rowCount];
    _snprintf(row.desc, sizeof(row.desc), "%s", desc);
    // ⚠️ A NEW ROW IS **PROTECTED** AND AT 50%. Adding a device is an act with a purpose -- the user picked it
    // out of a list because they want it guarded -- so a fresh row that arrived switched off would need a
    // second click to mean anything. (The FEATURE's own switch is the one that starts off; see the note on the
    // empty list in Settings.)
    row.protect = true;
    row.percent = kDefaultPercent;
    ++g_set.rowCount;
    added = true;
  }
  LeaveCriticalSection(&g_lock);

  if (added)
  {
    SaveSettings();
    LogLine("added \"%s\" to the protected list at %d%%", desc, kDefaultPercent);
  }
  // ⚠️ THE WORKER RUNS A PASS EITHER WAY -- on a successful add so the device is guarded immediately (the
  // user's level takes effect now, not at the next replug), and on a duplicate so nothing needs doing but the
  // page's re-read is not held up.
  if (g_wake)
    SetEvent(g_wake);
  return added;
}

// ---------------------------------------------------------------------------------------------
// setControl -- the page's writes arrive here
// ---------------------------------------------------------------------------------------------
//
// ⚠️ EVERY VALUE IS VALIDATED HERE, because the page cannot know what this feature accepts and a
// hand-edited ini is a supported way in. Reply 0 to REFUSE: the page then re-reads the control and shows
// what the feature actually has, which is how a refused value corrects itself in front of the user.
static int ApplyControl(const char *path, const char *value)
{
  if (!path || !value)
    return 0;

  // ⚠️ `enabled` IS REFUSED BECAUSE IT NO LONGER EXISTS -- see the note in SettingsJson. Refusing rather
  // than accepting-and-ignoring matters here: a page cached from an older build still sends it, and an
  // accepted-but-inert control would report success for a switch nothing reads.
  if (_stricmp(path, "enabled") == 0)
    return 0;

  // ⚠️ THE `select` CONTROL THAT ADDS A ROW. Its `value` is the description of a device the machine reported;
  // choosing one appends the row. See the note on ListOp for why this is a dropdown and not the typed add box
  // the ABI offers for a `rows` group.
  if (_stricmp(path, "addDevice") == 0)
  {
    return AddDeviceByDescription(value) ? 1 : 0;
  }

  // ⚠️⚠️ A ROW'S FIELDS ARRIVE AS `devices[<i>].<field>`, AND THE ROW MOVES THROUGH THE SAME `setControl`
  // AS EVERY OTHER CONTROL -- the page does not get a private channel for a list, and the feature is the one
  // that validates. Both fields are handled here rather than by type, because the VALUE is what decides:
  //
  //   protect -- a bool, stored as-is.
  //   percent -- ⚠️⚠️ AND THIS ONE WRITES TO THE DEVICE IMMEDIATELY, WHICH IS THE USER'S OWN REQUEST:
  //              "推子拉的时候，可以直接再次更改对应设备的音�?. Dragging the slider is the way the user finds the
  //              level they want, and having to unplug the box to hear each attempt would make the control
  //              useless for the job it is there to do.
  if (_strnicmp(path, "devices[", 8) == 0)
  {
    const char *p = path + 8;
    const int index = atoi(p);
    const char *close = strchr(p, ']');
    const char *field = close ? close + 1 : nullptr;
    if (!field || *field != '.')
      return 0; // not a field of a row
    ++field;

    bool isProtect = (_stricmp(field, "protect") == 0);
    bool isPercent = (_stricmp(field, "percent") == 0);
    if (!isProtect && !isPercent)
      return 0;

    char desc[kMaxName] = {0};
    bool protect = false;
    int percent = kDefaultPercent;
    bool haveRow = false;
    EnterCriticalSection(&g_lock);
    if (index >= 0 && index < g_set.rowCount)
    {
      haveRow = true;
      _snprintf(desc, sizeof(desc), "%s", g_set.rows[index].desc);
      if (isProtect)
      {
        bool v = true;
        ParseBool(value, &v);
        // ⚠️ TURNING THE SWITCH OFF ALSO SILENCES THE ROW IMMEDIATELY, and that is the same request seen
        // from the other side: a user who unguards a device while it is playing expects to be able to set it
        // themselves again, not to have this feature keep holding it until the next replug.
        if (!v)
          g_set.rows[index].protect = false;
        else
          g_set.rows[index].protect = true;
      }
      else
      {
        int p = atoi(value);
        if (p < 0)
          p = 0;
        if (p > 100)
          p = 100;
        g_set.rows[index].percent = p;
      }
      protect = g_set.rows[index].protect;
      percent = g_set.rows[index].percent;
    }
    LeaveCriticalSection(&g_lock);

    if (!haveRow)
      return 0; // a row the feature does not have -- the page re-reads and redraws

    SaveSettings();

    // ⚠️ THE LEVEL IS WRITTEN TO THE DEVICE NOW, ON THE WORKER RATHER THAN HERE. `setControl` runs on the
    // host's UI thread, and a COM call into the audio stack from there would be felt by the host's own
    // message loop. Setting the value and waking the worker is what keeps that work where it belongs -- and
    // it also coalesces: ten slider positions in a burst become however many passes the worker actually runs.
    if (isPercent && protect && desc[0])
    {
      EnterCriticalSection(&g_lock);
      _snprintf(g_pendingDesc, sizeof(g_pendingDesc), "%s", desc);
      g_pendingPercent = percent;
      g_pendingValid = true;
      LeaveCriticalSection(&g_lock);
    }
    if (g_wake)
      SetEvent(g_wake);
    return 1;
  }

  return 0;
}

// ---------------------------------------------------------------------------------------------
// THE EXPORTED FEATURE
// ---------------------------------------------------------------------------------------------

// ⚠️⚠️ `init` RETURNS **0 FOR "I AM READY"** AND NON-ZERO TO REFUSE. The direction is spelled out because
// `abi.h` once said the opposite and the mistake cost an afternoon: a feature that refuses is UNLOADED by
// the host, and anything it started keeps running inside unmapped memory. So: if the thread fails to start,
// refuse BEFORE anything is left behind.
static int VolumeLockInit(const ApexHost *host)
{
  g_host = host;
  if (host && host->featureDir)
    host->featureDir(g_dir, (int)sizeof(g_dir));

  InitializeCriticalSection(&g_lock);
  g_lockReady = true;
  OpenLog();

  // CoInitialize is NOT needed on this thread: the COM calls all happen on the WORKER, which initialises COM
  // itself below. Initialising it here would attach the host's main thread to a COM apartment it never asked
  // for -- the host owns the input hook and must not have its threading model changed by a feature.
  LoadSettings();

  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  // ⚠️ WorkerWithCom, NOT Worker: the COM apartment has to be entered before any device call, and the
  // trampoline is where that ordering lives (see its note below).
  g_thread = StartWorker();
  if (!g_thread)
  {
    Log("volumelock: could not start the worker thread (error %lu) -- refusing to load",
        (unsigned long)GetLastError());
    return 1; // NON-ZERO MEANS REFUSE
  }

  // ⚠️ THE ROW COUNT IS WORTH LOGGING AND THE FEATURE'S OWN SWITCH IS NOT: there is no such switch any more
  // (see SettingsJson), and "how many devices am I guarding" is the question a reader of this line has.
  Log("volumelock: started (%d device row(s))", g_set.rowCount);
  return 0; // READY
}

static void VolumeLockShutdown()
{
  InterlockedExchange(&g_stop, 1);
  if (g_wake)
    SetEvent(g_wake);
  if (g_regEvent)
    SetEvent(g_regEvent); // wake the registry wait too, so shutdown is not held up by it
  if (g_thread)
  {
    if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0)
      LogLine("the worker did not stop in time; it will end with the process");
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_wake)
  {
    CloseHandle(g_wake);
    g_wake = nullptr;
  }
  if (g_log)
  {
    fclose(g_log);
    g_log = nullptr;
  }
  if (g_lockReady)
    DeleteCriticalSection(&g_lock);
}

static int VolumeLockReload()
{
  LoadSettings();
  return 1;
}

static int VolumeLockSetControl(const char *path, const char *value) { return ApplyControl(path, value); }
static int VolumeLockListOp(const char *id, const char *op, const char *value, int index)
{
  return ListOp(id, op, value, index);
}

static unsigned VolumeLockFlags()
{
  // ⚠️ ALWAYS ENABLED: this bit means "this feature is loaded and usable", NOT "its switch is on". The
  // host's switch is honoured by the WORKER (see Pass), because a feature with its own thread would otherwise
  // keep working while the panel showed it as off.
  return APEX_FEATURE_ENABLED;
}

// ⚠️ THE FIELD ORDER IS THE ABI, AND IT IS NOT THE ORDER ANYONE WOULD GUESS. It was read out of
// `apex/abi.h` rather than written from memory: `settingsJson`, `setControl`, `listOp`, `quickItems`,
// `saveSettings`, `onWheel`, `tick`, `flags`. There is no `curve` field at all -- a feature that has one
// sends it as part of the JSON from `settingsJson`, which is why the first attempt here invented a slot
// that does not exist and the compiler refused the struct.
static const ApexFeature kFeature = {
    APEX_ABI_VERSION,
    sizeof(ApexFeature),
    kId,
    "音量保护",       // nameZh -- the user's own name for it (2026-10-08): 设备音量 described the KNOB, and
                      // what this feature actually does is guard a speaker against a device that comes up at
                      // 100%. The id stays `VolumeLock` (it is also the folder name and the settings file's
                      // stem -- see abi.h), so only the words the user reads changed.
    "Volume Guard",   // nameEn
    kVersion,
    VolumeLockInit,
    VolumeLockShutdown,
    VolumeLockReload,
    SettingsJson,
    VolumeLockSetControl,
    VolumeLockListOp, // listOp -- the device rows: `remove` takes one off the list (see ListOp)
    nullptr,          // quickItems -- nothing here belongs one click away
    nullptr,          // saveSettings -- this feature saves on every accepted edit itself
    nullptr,          // onWheel -- this feature has nothing to do with the wheel
    nullptr,          // tick -- it runs on its own thread instead
    VolumeLockFlags,
    nullptr,          // taskbarItems
    nullptr,          // taskbarText
};

// ---------------------------------------------------------------------------------------------
// EXPORTS
// ---------------------------------------------------------------------------------------------
//
// The names and the shape are what apex/loader_win.cpp looks for: ONE exported function called
// `ApexFeatureEntry`, `__cdecl`, returning a pointer to the feature's own static description. The name and
// the calling convention are the contract -- a feature that gets either wrong is simply never loaded, and
// the only other symptom is a line in the host's log.
//
// ⚠️ `ApexFeatureAbi` IS NOT PART OF THAT CONTRACT and nothing calls it; it is exported for the gate, which
// reads the DLL the way the loader does and checks the version it declares matches the one ABI this tree is
// built against. (The Rust feature exports the same pair for the same reason.)
extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void) { return &kFeature; }

extern "C" __declspec(dllexport) unsigned __cdecl ApexFeatureAbi(void) { return APEX_ABI_VERSION; }
