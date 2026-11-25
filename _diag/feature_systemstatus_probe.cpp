// SystemStatus's real DLL, pure formatting rules, per-item taskbar mappings, and scratch-file ownership.
// The probe opens no visible window and never touches the user's deployed settings.

#include <winsock2.h>
#include <windows.h>

#include "../apex/abi.h"
#include "../features/SystemStatus/system_status_logic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

typedef const ApexFeature *(*EntryFn)(void);

using apex::system_status::IsVisible;
using apex::system_status::Metric;
using apex::system_status::ParseBool;
using apex::system_status::SetVisible;

static int g_failures = 0;
static char g_dir[512] = {0};

static void Check(bool ok, const char *what)
{
  printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
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

static int HostFeatureEnabled(const char *) { return 1; }
static int HostTaskbarEnabled() { return 1; }
static int HostTaskbarPosition() { return 1; }
static int HostTaskbarFontSize() { return 13; }
static const char *HostTaskbarTextColor() { return "#12ABCD"; }

static void TestFormatting()
{
  char text[64] = {};
  apex::system_status::FormatRate(0.0, text, sizeof(text));
  Check(!std::strcmp(text, "0 B/s"), "zero uses bytes per second");
  apex::system_status::FormatRate(1023.0, text, sizeof(text));
  Check(!std::strcmp(text, "1023 B/s"), "below 1024 stays in bytes per second");
  apex::system_status::FormatRate(1536.0, text, sizeof(text));
  Check(!std::strcmp(text, "1.5 KB/s"), "1024-boundary switches to KB and keeps one decimal");
  apex::system_status::FormatRate(1572864.0, text, sizeof(text));
  Check(!std::strcmp(text, "1.5 MB/s"), "large rates use MB/s");
  apex::system_status::FormatPercent(73.6, text, sizeof(text));
  Check(!std::strcmp(text, "74%"), "percentages are rounded and capped for reading");
  apex::system_status::FormatTemperature(false, 0.0, text, sizeof(text));
  Check(!std::strcmp(text, "--"), "a missing temperature stays honest instead of becoming zero");

  unsigned bits = apex::system_status::kDefaultVisible;
  bits = SetVisible(bits, Metric::kGpu, true);
  Check(IsVisible(bits, Metric::kGpu), "one metric can be mapped on");
  bits = SetVisible(bits, Metric::kGpu, false);
  Check(!IsVisible(bits, Metric::kGpu), "the same metric can be mapped off");
  bool on = false;
  Check(ParseBool("true", &on) && on && ParseBool("0", &on) && !on,
        "the private settings file accepts the same booleans as the page");
}

static bool Set(ApexFeature *feature, const char *id, const char *value)
{
  return feature->setControl && feature->setControl(id, value) == 1;
}

static std::vector<std::string> ToggleIds(const char *json)
{
  std::vector<std::string> ids;
  const char *needle = "\"toggleId\":\"";
  if (!json)
    return ids;
  for (const char *p = json; (p = std::strstr(p, needle)) != nullptr;)
  {
    p += std::strlen(needle);
    const char *end = std::strchr(p, '"');
    if (!end)
      break;
    ids.emplace_back(p, end);
    p = end + 1;
  }
  return ids;
}

int main(int argc, char **argv)
{
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char *dll = argc >= 2 ? argv[1] : "build/apex/Plugins/SystemStatus/SystemStatus.dll";
  const char *jsonPath = argc >= 3 ? argv[2] : "build/_systemstatus_document.json";
  if (argc >= 4)
    _snprintf(g_dir, sizeof(g_dir), "%s", argv[3]);

  _putenv_s("APEX_SYSTEMSTATUS_NO_TASKBAR", "1");
  TestFormatting();

  HMODULE module = LoadLibraryExA(
      dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module)
    module = LoadLibraryA(dll);
  if (!module)
  {
    printf("FAIL: cannot load %s (error %lu)\n", dll, GetLastError());
    return 1;
  }
  EntryFn entry = reinterpret_cast<EntryFn>((void *)GetProcAddress(module, "ApexFeatureEntry"));
  if (!entry)
  {
    puts("FAIL: no ApexFeatureEntry");
    FreeLibrary(module);
    return 1;
  }
  ApexFeature *feature = const_cast<ApexFeature *>(entry());
  Check(feature->abiVersion == APEX_ABI_VERSION && feature->structSize >= sizeof(ApexFeature),
        "the DLL matches the host ABI");

  ApexHost host = {};
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLogLine;
  host.featureEnabled = HostFeatureEnabled;
  host.taskbarEnabled = HostTaskbarEnabled;
  host.taskbarPosition = HostTaskbarPosition;
  host.taskbarFontSize = HostTaskbarFontSize;
  host.taskbarTextColor = HostTaskbarTextColor;
  Check(feature->init && feature->init(&host) == 0, "the feature starts");

  static char warmed[64 * 1024] = {};
  bool sampled = false;
  for (int tries = 0; tries < 30 && !sampled; ++tries)
  {
    const int n = feature->settingsJson(warmed, (int)sizeof(warmed));
    sampled = n > 0 &&
              std::strstr(warmed, "\"toggleId\":\"lunar_date\"") != nullptr &&
              std::strstr(warmed, "\"textZh\":\"") != nullptr &&
              std::strstr(warmed, "\"textZh\":\"农历") == nullptr;
    if (!sampled)
      Sleep(100);
  }
  Check(sampled, "the non-blocking sampler publishes live values after init returns");

  const char *migrationOnly = std::getenv("APEX_SYSTEMSTATUS_MIGRATE_ONLY");
  if (migrationOnly && !std::strcmp(migrationOnly, "1"))
  {
    ApexTaskbarArea migratedAreas[64] = {};
    const int migratedCount = feature->taskbarItems(migratedAreas, 64);
    bool intelUsage = false, nvidiaTemp = false, stableOnly = true;
    for (int i = 0; i < migratedCount; ++i)
    {
      const std::string key = migratedAreas[i].key;
      const std::string previous = migratedAreas[i].previousKey;
      stableOnly = stableOnly && key.find("gpu_00000000_") == std::string::npos;
      if (key.find("gpu_v8086_") != std::string::npos &&
          key.find("_usage") != std::string::npos)
      {
        intelUsage = previous ==
                     "SystemStatus:gpu_00000000_00000001_usage";
      }
      if (key.find("gpu_v10de_") != std::string::npos &&
          key.find("_temp") != std::string::npos)
      {
        nvidiaTemp = previous ==
                     "SystemStatus:gpu_00000000_00000002_temp";
      }
    }
    Check(intelUsage && nvidiaTemp && stableOnly,
          "legacy GPU mappings become stable hardware keys with in-place aliases");

    static char migratedDocument[64 * 1024] = {};
    const int migratedLength =
        feature->settingsJson(migratedDocument, (int)sizeof(migratedDocument));
    Check(migratedLength > 0 &&
              std::strstr(migratedDocument,
                          "\"toggleId\":\"gpu_v8086_") != nullptr &&
              std::strstr(migratedDocument,
                          "\"toggleId\":\"gpu_v10de_") != nullptr &&
              std::strstr(migratedDocument, "\"choiceValue\":\"gb\"") != nullptr,
          "the migrated cards keep their saved switch and unit");
    Check(feature->saveSettings && feature->saveSettings() == 1,
          "the stable GPU keys and aliases save back to SystemStatus.ini");
    if (feature->shutdown)
      feature->shutdown();
    FreeLibrary(module);
    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
  }

  const char *staticMode = std::getenv("APEX_SYSTEMSTATUS_STATIC");
  const char *realUnits = std::getenv("APEX_SYSTEMSTATUS_REAL_UNITS");
  if (realUnits && !std::strcmp(realUnits, "1"))
  {
    Set(feature, "cpu_usage_unit", "ghz");
    Set(feature, "ram_usage_unit", "gb");
    for (const std::string &id : ToggleIds(warmed))
    {
      if (id.rfind("gpu_", 0) == 0 && id.size() > 6 &&
          id.compare(id.size() - 6, 6, "_usage") == 0)
      {
        const std::string unit = id + "_unit";
        Set(feature, unit.c_str(), "gb");
      }
    }
    for (int tries = 0; tries < 20; ++tries)
    {
      feature->settingsJson(warmed, (int)sizeof(warmed));
      if (std::strstr(warmed, "\"choiceValue\":\"gb\"") &&
          std::strstr(warmed, "\"choiceValue\":\"ghz\""))
        break;
      Sleep(100);
    }
  }
  if (staticMode && !std::strcmp(staticMode, "1"))
  {
    Check(Set(feature, "cpu_usage_unit", "ghz") &&
              Set(feature, "ram_usage_unit", "gb") &&
              Set(feature, "gpu_00000000_00000001_usage_unit", "gb") &&
              Set(feature, "gpu_00000000_00000002_usage_unit", "gb") &&
              !Set(feature, "cpu_usage_unit", "meters"),
          "usage cards accept real-unit choices and refuse unknown units");
    bool unitsShown = false;
    for (int tries = 0; tries < 20 && !unitsShown; ++tries)
    {
      const int n = feature->settingsJson(warmed, (int)sizeof(warmed));
      unitsShown = n > 0 && std::strstr(warmed, "GHz") != nullptr &&
                   std::strstr(warmed, "显存") != nullptr;
      if (!unitsShown)
        Sleep(100);
    }
    Check(unitsShown, "the selected units reach the live status document");
  }

  const std::vector<std::string> ids = ToggleIds(warmed);
  Check(!ids.empty() && ids.size() <= 64,
        "the live document describes the currently valid dynamic metrics");
  Check(!Set(feature, "not_a_metric", "not-a-bool") && !Set(feature, "not_a_metric", "1"),
        "an unknown dynamic metric is refused");
  for (const std::string &id : ids)
  {
    Check(Set(feature, id.c_str(), "0"),
          "a dynamic taskbar item can be unmapped independently");
  }
  ApexTaskbarArea areas[16] = {};
  Check(feature->taskbarItems(areas, 16) == 0,
        "all switches off removes every dynamic taskbar slot");
  for (const std::string &id : ids)
    Check(Set(feature, id.c_str(), "1"), "a switch maps its own dynamic taskbar slot");
  ApexTaskbarArea allAreas[64] = {};
  const int areaCount = feature->taskbarItems(allAreas, 64);
  bool eachHasText = areaCount == (int)ids.size();
  bool lunarHasNoPrefix = false;
  for (int i = 0; i < areaCount && eachHasText; ++i)
  {
    char text[512] = {};
    eachHasText = allAreas[i].lineCount == 1 && allAreas[i].key[0] &&
                  feature->taskbarText &&
                  feature->taskbarText(allAreas[i].key, 1, text, sizeof(text)) == 1 &&
                  text[0];
    if (!std::strcmp(allAreas[i].key, "SystemStatus:lunar_date"))
      lunarHasNoPrefix = text[0] && std::strncmp(text, "农历", std::strlen("农历")) != 0;
    for (int j = 0; j < i; ++j)
      eachHasText = eachHasText && std::strcmp(allAreas[i].key, allAreas[j].key) != 0;
  }
  Check(eachHasText, "every switch registers one distinct one-line taskbar slot");
  Check(lunarHasNoPrefix, "the lunar taskbar text does not repeat the 农历 prefix");
  for (const std::string &id : ids)
    if (id != "cpu_usage")
      Set(feature, id.c_str(), "0");
  Check(feature->taskbarItems(areas, 16) == 1 &&
            !std::strcmp(areas[0].key, "SystemStatus:cpu_usage"),
        "each dynamic mapping switch is one General-page taskbar slot");
  Check(feature->saveSettings && feature->saveSettings() == 1, "the mappings save");

  static char document[64 * 1024] = {};
  const int length = feature->settingsJson(document, (int)sizeof(document));
  Check(length > 0 && length < (int)sizeof(document), "the controls document fits the panel transport");
  Check(std::strstr(document, "\"summaryZh\"") != nullptr &&
            std::strstr(document, "\"summaryEn\"") != nullptr,
        "the feature explains itself in both languages");
  Check(std::strstr(document, "\"live\":true") != nullptr,
        "the status cards ask the page to refresh");
  Check(std::strstr(document, "\"toggleId\":\"cpu_usage\"") != nullptr &&
            std::strstr(document, "\"toggleLabelZh\":\"任务栏显示\"") != nullptr,
        "each status card carries its own taskbar mapping button");
  Check(std::strstr(document, "taskbar_font_size") == nullptr &&
            std::strstr(document, "taskbar_text_color") == nullptr,
        "shared taskbar appearance is not duplicated in this feature");

  FILE *json = fopen(jsonPath, "wb");
  if (json)
  {
    fwrite(document, 1, (size_t)length, json);
    fclose(json);
  }
  else
    Check(false, "the probe can write its document under build/");

  if (feature->shutdown)
    feature->shutdown();
  FreeLibrary(module);
  printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
  return g_failures ? 1 : 0;
}
