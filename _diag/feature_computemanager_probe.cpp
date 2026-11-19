// ComputeManager's real DLL plus its pure schedule/JSON helpers.
//
// The probe uses a scratch folder under build/ and never touches the deployed settings. It proves the
// page document still fits Apex's 64 KiB transport with a full endpoint list, that a saved API key is
// masked in the document, and that the period arithmetic handles midnight and counter resets.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../apex/abi.h"
#include "../features/ComputeManager/compute_manager_logic.h"
#include "../common/taskbar_layout.h"

using apex::compute_manager::ExtractJsonNumber;
using apex::compute_manager::ExtractModelIds;
using apex::compute_manager::ExtractVendorSchedule;
using apex::compute_manager::ExpandModel;
using apex::compute_manager::MinutesUntilPeriodChange;
using apex::compute_manager::MinutesUntilWindowsChange;
using apex::compute_manager::ParseHhMm;
using apex::compute_manager::ParseDeepSeekPeakWindows;
using apex::compute_manager::ParseHexColor;
using apex::compute_manager::Period;
using apex::compute_manager::PeriodAt;
using apex::compute_manager::PeriodAtWindows;
using apex::compute_manager::UsageDelta;

typedef const ApexFeature *(*EntryFn)(void);

static int failures = 0;
static char g_dir[512] = {0};

static HWND g_ownOverlay = nullptr;

static BOOL CALLBACK FindOwnOverlay(HWND window, LPARAM expectedPid)
{
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != (DWORD)expectedPid)
    return TRUE;
  wchar_t cls[128] = {0};
  if (GetClassNameW(window, cls, (int)(sizeof(cls) / sizeof(cls[0]))) > 0 &&
      wcscmp(cls, L"ApexComputeManagerOverlay") == 0)
  {
    g_ownOverlay = window;
    return FALSE;
  }
  return TRUE;
}

static void Check(bool ok, const char *what)
{
  printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++failures;
}

static int HostFeatureDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  const int n = _snprintf(out, outSize, "%s", g_dir);
  return n > 0 ? n : 0;
}

static void HostLogLine(const char *text)
{
  printf("      [feature] %s\n", text ? text : "");
}

// Keep the worker's vendor refreshes off: this probe exercises the local ABI, settings and document
// paths, and a gate must never depend on the network. The production worker only fetches while the
// host says its feature is enabled, so this is the same path a disabled feature takes.
static int HostFeatureEnabled(const char *) { return 0; }
static int HostTaskbarFontSize() { return 15; }
static const char *HostTaskbarTextColor() { return "#12ABCD"; }

static bool Set(ApexFeature *f, const char *path, const char *value)
{
  return f->setControl && f->setControl(path, value) == 1;
}

static bool EndpointPath(char *out, int outSize, int index, const char *field)
{
  const int n = _snprintf(out, outSize, "accounts[%d].%s", index, field);
  return n > 0 && n < outSize;
}

static bool SetEndpoint(ApexFeature *f, int index, const char *field, const char *value)
{
  char path[128] = {0};
  return EndpointPath(path, (int)sizeof(path), index, field) && Set(f, path, value);
}

static void TestLogic()
{
  puts("== pure schedule and JSON helpers ==");

  int minute = -1;
  Check(ParseHhMm("09:30", &minute) && minute == 570, "HH:MM parses to minutes");
  Check(!ParseHhMm("24:00", &minute), "an invalid hour is refused");
  Check(!ParseHhMm("09:60", &minute), "an invalid minute is refused");
  unsigned rgb = 0;
  Check(ParseHexColor("#12aBcD", &rgb) && rgb == 0x12abcd,
        "a six-digit taskbar text colour is accepted");
  Check(!ParseHexColor("#12345", &rgb) && !ParseHexColor("not-a-colour", &rgb),
        "short or malformed taskbar colours are refused");

  Check(PeriodAt(23 * 60 + 30, 22 * 60, 2 * 60, 3 * 60, 6 * 60) == Period::Peak,
        "peak crosses midnight (23:30)");
  Check(PeriodAt(1 * 60 + 30, 22 * 60, 2 * 60, 3 * 60, 6 * 60) == Period::Peak,
        "peak crosses midnight (01:30)");
  Check(PeriodAt(4 * 60, 22 * 60, 2 * 60, 3 * 60, 6 * 60) == Period::Valley,
        "valley is selected outside peak");
  Check(PeriodAt(12 * 60, 22 * 60, 2 * 60, 3 * 60, 6 * 60) == Period::Normal,
        "the remaining time is normal");
  Check(MinutesUntilPeriodChange(25, 8 * 60 + 30, 30, 30, 8 * 60 + 30) == 5,
        "the widget knows a peak/valley switch is five minutes away");

  bool reset = false;
  Check(UsageDelta(100.0, 130.0, &reset) == 30.0 && !reset, "usage grows by the difference");
  Check(UsageDelta(100.0, 12.0, &reset) == 12.0 && reset, "a falling counter is a reset");
  std::string expanded;
  ExpandModel("https://api/{model}/usage", "vendor-alpha", &expanded);
  Check(expanded == "https://api/vendor-alpha/usage", "the model placeholder expands in a URL or body");

  const char *json =
      "{\"data\":{\"accounts\":[{\"unused\":1},{\"usage\":42.5}],\"balance\":\"7.25\"}}";
  double number = 0;
  Check(ExtractJsonNumber(json, "data.accounts[1].usage", &number) && number == 42.5,
        "a nested array path reads a usage number");
  Check(ExtractJsonNumber(json, "data.balance", &number) && number == 7.25,
        "a string number is accepted for a balance");
  std::vector<std::string> models;
  Check(ExtractModelIds("{\"data\":[{\"id\":\"model-a\"},{\"id\":\"model-b\"}]}", &models) &&
            models.size() == 2 && models[0] == "model-a",
        "OpenAI-compatible model lists are discovered");
  int peakStart = 0, peakEnd = 0, valleyStart = 0, valleyEnd = 0;
  Check(ExtractVendorSchedule("{\"schedule\":{\"peak_start\":\"08:30\",\"peak_end\":\"00:30\","
                              "\"valley_start\":\"00:30\",\"valley_end\":\"08:30\"}}",
                              &peakStart, &peakEnd, &valleyStart, &valleyEnd) &&
            peakStart == 510 && valleyEnd == 510,
        "a vendor response can supply the peak/valley schedule");

  const char *deepseekPricing =
      "Off-peak rates are half of the peak rates. Peak hours are 01:00 - 04:00 and 06:00 - 10:00 UTC, "
      "Monday through Friday, excluding Chinese public holidays. All other hours are off-peak.";
  int starts[4] = {}, ends[4] = {}, windowCount = 0;
  unsigned weekdayMask = 0;
  Check(ParseDeepSeekPeakWindows(deepseekPricing, starts, ends, &windowCount, &weekdayMask) &&
            windowCount == 2 && starts[0] == 60 && ends[0] == 240 && starts[1] == 360 &&
            ends[1] == 600 && weekdayMask == 0x3e,
        "DeepSeek's published UTC windows and weekdays are parsed from the official page");
  const int localStarts[2] = {9 * 60, 14 * 60};
  const int localEnds[2] = {12 * 60, 18 * 60};
  Check(PeriodAtWindows(9 * 60 + 30, 1, localStarts, localEnds, 2, weekdayMask) == Period::Peak,
        "a weekday inside the translated DeepSeek window is peak");
  Check(PeriodAtWindows(12 * 60 + 30, 1, localStarts, localEnds, 2, weekdayMask) == Period::Valley,
        "the gap between DeepSeek's two windows is valley");
  Check(PeriodAtWindows(9 * 60 + 30, 6, localStarts, localEnds, 2, weekdayMask) == Period::Valley,
        "weekends are valley even at a weekday peak clock time");
  Check(MinutesUntilWindowsChange(8 * 60 + 30, 1, localStarts, localEnds, 2, weekdayMask) == 30,
        "the countdown reaches the first weekday peak");
  Check(MinutesUntilWindowsChange(23 * 60 + 30, 5, localStarts, localEnds, 2, weekdayMask) ==
            57 * 60 + 30,
        "the countdown crosses the weekend to Monday's first peak");

  puts("\n== taskbar orientation and compact layout ==");
  using apex::taskbar::Display;
  using apex::taskbar::Edge;
  using apex::taskbar::Rect;
  using apex::taskbar::Span;
  const Rect monitor = {0, 0, 1920, 1080};
  const Rect bottom = {0, 1048, 1920, 1080};
  const Rect top = {0, 0, 1920, 32};
  const Rect left = {0, 0, 48, 1080};
  const Rect right = {1872, 0, 1920, 1080};
  Check(apex::taskbar::ClassifyEdge(bottom, monitor) == Edge::Bottom &&
            apex::taskbar::ClassifyEdge(top, monitor) == Edge::Top &&
            apex::taskbar::ClassifyEdge(left, monitor) == Edge::Left &&
            apex::taskbar::ClassifyEdge(right, monitor) == Edge::Right,
        "all four taskbar edges are classified from the real rectangles");

  Display compact = apex::taskbar::ChooseDisplay(bottom, monitor, 12);
  Display forty = apex::taskbar::ChooseDisplay(Rect{0, 1040, 1920, 1080}, monitor, 12);
  Display roomy = apex::taskbar::ChooseDisplay(Rect{0, 1032, 1920, 1080}, monitor, 12);
  Display vertical = apex::taskbar::ChooseDisplay(left, monitor, 12);
  Check(compact.singleLine && compact.lineCount == 1 &&
            forty.singleLine && forty.lineCount == 1,
        "compact taskbars use one readable line");
  Check(!roomy.singleLine && roomy.lineCount == 2,
        "a 48 px taskbar keeps the roomy two-line layout");
  const apex::taskbar::TwoLineRows rows = apex::taskbar::CompactTwoLineRows(44, 12);
  Check(rows.lineHeight == 16 && rows.balanceTop + rows.lineHeight <= 44 &&
            rows.balanceTop - rows.titleTop < 22,
        "two lines use compact font-based spacing instead of half the taskbar each");
  Check(vertical.vertical && vertical.singleLine && vertical.lineCount == 1,
        "a side taskbar is vertical and one rotated line deep");
  Check(apex::taskbar::UseNearEnd(0, false, true) &&
            !apex::taskbar::UseNearEnd(0, false, false) &&
            !apex::taskbar::UseNearEnd(0, true, true) &&
            apex::taskbar::UseNearEnd(1, false, false),
        "left-aligned icons temporarily route the left choice to the right, then restore it");
  int taskbarSize = 0;
  char taskbarColor[8] = {0};
  Check(apex::taskbar::ParseFontSize("15", &taskbarSize) && taskbarSize == 15 &&
            !apex::taskbar::ParseFontSize("17", &taskbarSize) &&
            apex::taskbar::NormaliseColor("#12abCD", taskbarColor, sizeof(taskbarColor)) &&
            !std::strcmp(taskbarColor, "#12ABCD") &&
            !apex::taskbar::NormaliseColor("red", taskbarColor, sizeof(taskbarColor)),
        "shared taskbar appearance is validated by one rule");

  const Span obstacles[] = {{8, 120}, {300, 380}, {850, 950}};
  Check(apex::taskbar::PlaceSpan(0, 1000, 80, obstacles, 3, false, 4, 8) == 128,
        "near-start placement moves past an overlapping tool but stays near the start");
  Check(apex::taskbar::PlaceSpan(0, 1000, 80, obstacles, 3, true, 4, 8) == 762,
        "near-end placement uses the far end and does not jump across the taskbar");
  Check(apex::taskbar::PlaceSpan(0, 100, 80, obstacles, 3, false, 4, 8) == -1,
        "no room returns no answer instead of silently choosing the opposite side");
}

int main(int argc, char **argv)
{
  // A gate failure must be text in the terminal, never a system modal dialog. Error mode is process-wide and is
  // inherited by child work, so a missing entry point or a failing DLL is reported as an exit code.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char *dll = argc >= 2 ? argv[1] : "build/apex/Plugins/ComputeManager/ComputeManager.dll";
  const char *jsonPath = argc >= 3 ? argv[2] : "build/_computemanager_document.json";
  if (argc >= 4)
    _snprintf(g_dir, sizeof(g_dir), "%s", argv[3]);

  TestLogic();
  puts("== real DLL ==");

  HMODULE mod = LoadLibraryExA(dll, nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!mod)
    mod = LoadLibraryA(dll);
  if (!mod)
  {
    printf("FAIL: cannot load %s (error %lu)\n", dll, GetLastError());
    return 1;
  }

  EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, "ApexFeatureEntry");
  if (!entry)
  {
    puts("FAIL: no ApexFeatureEntry");
    FreeLibrary(mod);
    return 1;
  }
  ApexFeature *f = (ApexFeature *)entry();
  Check(f->abiVersion == APEX_ABI_VERSION && f->structSize >= sizeof(ApexFeature),
        "the DLL matches the host ABI");

  ApexHost host;
  memset(&host, 0, sizeof(host));
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLogLine;
  host.featureEnabled = HostFeatureEnabled;
  host.taskbarFontSize = HostTaskbarFontSize;
  host.taskbarTextColor = HostTaskbarTextColor;
  Check(f->init && f->init(&host) == 0, "the feature starts");
  // ⚠️ A WINDOW CLASS SAYS WHAT A WINDOW IS, NOT WHOSE IT IS. The user's running Apex can own a visible
  // ComputeManager overlay at the same moment this probe starts its own hidden one. FindWindowW then returns
  // whichever happens to be enumerated first, so the probe's verdict depended on the user's desktop state.
  // Match the probe's process id first, exactly as the project's UI probes do.
  g_ownOverlay = nullptr;
  EnumWindows(FindOwnOverlay, (LPARAM)GetCurrentProcessId());
  HWND overlay = g_ownOverlay;
  Check(overlay != nullptr && !IsWindowVisible(overlay), "the desktop widget exists but starts hidden");
  Check(Set(f, "auto_poll", "0"), "automatic polling can be switched off for the probe");
  Check(Set(f, "poll_seconds", "1") && !Set(f, "poll_seconds", "61") &&
            !Set(f, "poll_seconds", "0"),
        "refresh bounds are still owned by the feature");
  Check(f->listOp("processes", "add", "code.exe", 0) == 1, "a process can gate the widget");

  for (int i = 0; i < 12; ++i)
  {
    char name[64] = {0};
    _snprintf(name, sizeof(name), "Provider %02d", i + 1);
    Check(f->listOp("accounts", "add", name, 0) == 1, i == 0 ? "an endpoint can be added" : "the endpoint list grows");
  }
  for (int i = 0; i < 12; ++i)
    Check(SetEndpoint(f, i, "preset", "deepseek"), i == 0 ? "a provider preset fills the model endpoint" :
                                                                 "the provider preset applies to every model row");
  static char presetDocument[64 * 1024] = {0};
  const int presetLength = f->settingsJson(presetDocument, (int)sizeof(presetDocument));
  Check(presetLength > 0 &&
            strstr(presetDocument, "\"models_url\":\"https://api.deepseek.com/models\"") != nullptr,
        "the preset supplies the model-list URL");
  Check(strstr(presetDocument, "{\"id\":\"url\",\"type\":\"text\"") == nullptr,
        "provider presets hide the advanced API fields");
  Check(strstr(presetDocument, "\"schedule_enabled\":0") != nullptr &&
            strstr(presetDocument, "\"currency\":\"CNY\"") != nullptr,
        "a preset supplies currency but never invents a peak/valley schedule");
  for (int i = 0; i < 12; ++i)
    Check(SetEndpoint(f, i, "preset", "custom"), i == 0 ? "the probe can return to custom before network use" :
                                                            "the custom row restores advanced-field mode");
  Check(f->listOp("accounts[0].models", "add", "manually-added-model", 0) == 1,
        "a sub-model can be added manually under one endpoint");
  Check(SetEndpoint(f, 0, "taskbar_visible", "0") && SetEndpoint(f, 0, "taskbar_visible", "1"),
        "each endpoint has its own taskbar visibility switch");

  const std::string longBody(360, 'x');
  const std::string longHeaders(170, 'h');
  const std::string longUrl = "https://example.invalid/very/long/usage/endpoint/for/the/document/limit/" +
                              std::string(210, 'u');
  for (int i = 0; i < 12; ++i)
  {
    char name[64] = {0};
    char key[64] = {0};
    char path[128] = {0};
    _snprintf(name, sizeof(name), "Provider %02d with a deliberately long display name", i + 1);
    _snprintf(key, sizeof(key), "secret-token-%02d", i + 1);
    _snprintf(path, sizeof(path), "data.accounts[%d].usage", i);
    if (!SetEndpoint(f, i, "name", name) ||
        !SetEndpoint(f, i, "model", i == 0 ? "vendor-model-alpha" : "vendor-model-beta") ||
        !SetEndpoint(f, i, "url", i == 0 ? "https://api.example.invalid/usage" : longUrl.c_str()) ||
        !SetEndpoint(f, i, "auth", "header") ||
        !SetEndpoint(f, i, "auth_header", "X-Test-Key") ||
        !SetEndpoint(f, i, "api_key", key) ||
        !SetEndpoint(f, i, "method", "POST") ||
        !SetEndpoint(f, i, "body", longBody.c_str()) ||
        !SetEndpoint(f, i, "usage_path", path) ||
        !SetEndpoint(f, i, "balance_path", "data.balance") ||
        !SetEndpoint(f, i, "unit", "credits") ||
        !SetEndpoint(f, i, "extra_headers", longHeaders.c_str()))
    {
      Check(false, "every endpoint field is accepted");
      break;
    }
    if (i == 11)
      Check(true, "every endpoint field is accepted");
  }

  Check(SetEndpoint(f, 0, "api_key", "********"), "the page's API-key mask is not stored as the key");
  Check(f->saveSettings && f->saveSettings() == 1, "the settings save succeeds");

  static char document[64 * 1024] = {0};
  const int n = f->settingsJson(document, (int)sizeof(document));
  Check(n > 0 && n < (int)sizeof(document), "twelve full endpoints still fit the 64 KiB document");
  Check(strstr(document, "\"id\":\"accounts\"") != nullptr, "the endpoint group is described");
  Check(strstr(document, "\"id\":\"processes\"") != nullptr, "the process trigger group is described");
  Check(strstr(presetDocument, "\"id\":\"refresh_row\",\"type\":\"row\"") != nullptr,
        "related short settings share rows");
  Check(strstr(presetDocument, "\"id\":\"model\",\"type\":\"text\"") == nullptr,
        "the unrelated current-model field is absent");
  Check(strstr(document, "\"id\":\"endpoint_11\"") != nullptr, "the twelfth endpoint has its own status card");
  Check(strstr(document, "\"id\":\"overall\"") == nullptr &&
            strstr(document, "\"toggleId\":\"accounts[0].taskbar_visible\"") != nullptr,
        "endpoint status is one line with its own taskbar switch and no aggregate card");
  Check(strstr(document, "\"id\":\"taskbar_row\"") == nullptr &&
            strstr(document, "\"id\":\"taskbar_font_size\"") == nullptr &&
            strstr(document, "\"id\":\"taskbar_text_color\"") == nullptr,
        "taskbar text appearance is not duplicated inside the feature page");
  Check(strstr(document, "\"api_key\":\"********\"") != nullptr, "the document masks a saved API key");
  Check(strstr(document, "secret-token-01") == nullptr, "the real API key is not in the page document");
  if (f->taskbarItems)
  {
    ApexTaskbarArea area = {};
    Check(f->taskbarItems(&area, 1) == 1 && area.key[0],
          "the feature declares its live taskbar area to the host");
  }
  else
    Check(false, "the feature declares its live taskbar area to the host");

  if (n > 0)
  {
    FILE *out = fopen(jsonPath, "wb");
    if (out)
    {
      fwrite(document, 1, (size_t)n, out);
      fclose(out);
    }
    else
      Check(false, "the probe can write its document under build/");
  }

  if (f->shutdown)
    f->shutdown();
  FreeLibrary(mod);

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
