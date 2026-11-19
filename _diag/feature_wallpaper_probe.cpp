// Wallpaper's real DLL, pure schedule/layout rules, and read-only system capabilities.
//
// This probe never changes the user's desktop. The DLL is started with APEX_WALLPAPER_NO_APPLY=1, which still
// generates the composite under the scratch folder but stops before IDesktopWallpaper and the lock-screen API.

#include <windows.h>
#include <shobjidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../apex/abi.h"
#include "../features/Wallpaper/wallpaper_logic.h"
#include "../features/Wallpaper/wallpaper_winrt.h"

typedef const ApexFeature *(*EntryFn)(void);

static int failures = 0;
static char g_dir[512] = {0};

static void Check(bool ok, const char *what)
{
  printf("  %-68s %s\n", what, ok ? "ok" : "FAIL");
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

static void HostLog(const char *text)
{
  printf("      [feature] %s\n", text ? text : "");
}

static int HostEnabled(const char *) { return 1; }

static bool Set(ApexFeature *f, const char *path, const char *value)
{
  return f->setControl && f->setControl(path, value) == 1;
}

static bool BlockNextDisplayCopy()
{
  char pattern[620] = {0};
  _snprintf(pattern, sizeof(pattern), "%sWallpaper.display-*-1.*", g_dir);
  wchar_t widePattern[620] = {0};
  MultiByteToWideChar(CP_UTF8, 0, pattern, -1, widePattern, 620);
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW(widePattern, &data);
  if (find == INVALID_HANDLE_VALUE)
    return false;
  FindClose(find);
  char name[520] = {0};
  if (!WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, sizeof(name), nullptr, nullptr))
    return false;
  const char *dash = strrchr(name, '-');
  const char *dot = strrchr(name, '.');
  if (!dash || !dot || dash > dot)
    return false;
  char target[620] = {0};
  const int n = _snprintf(target, sizeof(target), "%s%.*s-2%s", g_dir,
                          (int)(dash - name), name, dot);
  if (n <= 0 || n >= (int)sizeof(target))
    return false;
  wchar_t wideTarget[620] = {0};
  if (!MultiByteToWideChar(CP_UTF8, 0, target, -1, wideTarget, 620))
    return false;
  HANDLE blocker =
      CreateFileW(wideTarget, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (blocker == INVALID_HANDLE_VALUE)
    return false;
  CloseHandle(blocker);
  return true;
}

static bool EnsureDir(const char *path)
{
  if (CreateDirectoryA(path, nullptr))
    return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool WriteBmp(const char *path, int width, int height, unsigned char r, unsigned char g,
                     unsigned char b)
{
  const int stride = (width * 3 + 3) & ~3;
  const DWORD pixels = static_cast<DWORD>(stride * height);
  BITMAPFILEHEADER file = {};
  file.bfType = 0x4D42;
  file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  file.bfSize = file.bfOffBits + pixels;
  BITMAPINFOHEADER info = {};
  info.biSize = sizeof(info);
  info.biWidth = width;
  info.biHeight = height;
  info.biPlanes = 1;
  info.biBitCount = 24;
  info.biCompression = BI_RGB;
  info.biSizeImage = pixels;
  std::string row(static_cast<size_t>(stride), 0);
  for (int x = 0; x < width; ++x)
  {
    row[x * 3 + 0] = static_cast<char>(b);
    row[x * 3 + 1] = static_cast<char>(g);
    row[x * 3 + 2] = static_cast<char>(r);
  }
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = fwrite(&file, sizeof(file), 1, f) == 1 && fwrite(&info, sizeof(info), 1, f) == 1;
  for (int y = 0; y < height && ok; ++y)
    ok = fwrite(row.data(), row.size(), 1, f) == 1;
  fclose(f);
  return ok;
}

static void TestLogic()
{
  puts("== pure schedule and layout rules ==");
  int minute = -1;
  Check(apex::wallpaper::ParseClock("06:35", &minute) && minute == 395,
        "HH:MM parses to local minutes");
  Check(!apex::wallpaper::ParseClock("24:00", &minute), "an invalid hour is refused");
  Check(apex::wallpaper::InClockRange(23 * 60, 22 * 60, 2 * 60),
        "a time band crosses midnight");
  Check(!apex::wallpaper::InClockRange(720, 420, 420), "equal endpoints do not mean all day");

  int interval = -1;
  Check(apex::wallpaper::SelectVariant(apex::wallpaper::Schedule::Clock, 8 * 60, 7 * 60, 19 * 60,
                                       false, 0, 60, 4, &interval) ==
            apex::wallpaper::Variant::Day,
        "08:00 selects the day image");
  Check(apex::wallpaper::SelectVariant(apex::wallpaper::Schedule::Clock, 20 * 60, 7 * 60, 19 * 60,
                                       true, 0, 60, 4, &interval) ==
            apex::wallpaper::Variant::Night,
        "20:00 selects the night image");
  Check(apex::wallpaper::SelectVariant(apex::wallpaper::Schedule::Interval, 12 * 60, 420, 1140,
                                       true, 7200, 30, 4, &interval) ==
                apex::wallpaper::Variant::Interval &&
            interval == 0,
        "interval rotation uses wall-clock buckets");
  double latitude = 0, longitude = 0;
  Check(apex::wallpaper::ParseGeoJson("{\"latitude\":39.9,\"longitude\":116.4}", &latitude,
                                      &longitude) &&
            latitude > 39.0 && longitude > 116.0,
        "the geolocation response parser accepts latitude and longitude");
  Check(apex::wallpaper::ParseGeoJson("{\"lat\":-33.8,\"lon\":151.2}", &latitude, &longitude) &&
            latitude < 0 && longitude > 151.0,
        "the geolocation response parser accepts the compact public-IP fields");

  int rise = 0, set = 0;
  bool alwaysDay = false;
  const bool solar = apex::wallpaper::SolarTimes(39.9042, 116.4074, 2026, 11, 15, 480,
                                                  &rise, &set, &alwaysDay);
  Check(solar && rise >= 360 && rise <= 480, "Beijing's November sunrise is plausible");
  Check(solar && set >= 960 && set <= 1080, "Beijing's November sunset is plausible");
  Check(!alwaysDay, "Beijing is not in polar day in November");

  const apex::wallpaper::Rect monitors[] = {
      {1920, 0, 4480, 1440},
      {-1280, 200, 0, 1224},
  };
  apex::wallpaper::Rect bounds = {};
  Check(apex::wallpaper::VirtualBounds(monitors, 2, &bounds) && bounds.left == -1280 &&
            bounds.right == 4480 && bounds.top == 0 && bounds.bottom == 1440,
        "a monitor left of the primary one is included in the virtual bounds");
  const apex::wallpaper::Rect placed = apex::wallpaper::PlaceInCanvas(monitors[1], bounds);
  Check(placed.left == 0 && placed.top == 200 && placed.right == 1280 && placed.bottom == 1224,
        "the negative-origin monitor maps to the correct canvas rectangle");
  Check(apex::wallpaper::HasImageExtension("photo.PNG") &&
            !apex::wallpaper::HasImageExtension("notes.txt"),
        "only supported image extensions enter the library");
}

static bool ReadLockSupport(bool *supported, HRESULT *result)
{
  const apex::wallpaper::winrt::CombaseApi &api = apex::wallpaper::winrt::GetCombaseApi();
  if (!api.ok())
  {
    *result = E_NOTIMPL;
    return false;
  }
  HSTRING name = nullptr;
  HRESULT hr = api.windowsCreateString(
      apex::wallpaper::winrt::kPersonalizationClass,
      static_cast<UINT32>(wcslen(apex::wallpaper::winrt::kPersonalizationClass)), &name);
  if (FAILED(hr))
    return false;
  apex::wallpaper::winrt::IPersonalizationStatics *statics = nullptr;
  hr = api.roGetActivationFactory(name, apex::wallpaper::winrt::kIIDPersonalizationStatics,
                              reinterpret_cast<void **>(&statics));
  api.windowsDeleteString(name);
  if (FAILED(hr) || !statics)
  {
    *result = hr;
    return false;
  }
  apex::wallpaper::winrt::Boolean value = 0;
  hr = statics->IsSupported(&value);
  statics->Release();
  *result = hr;
  *supported = value != 0;
  return SUCCEEDED(hr);
}

static bool ReadDesktop(std::string *fingerprint, int *count)
{
  IDesktopWallpaper *desk = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                IID_IDesktopWallpaper, reinterpret_cast<void **>(&desk));
  if (FAILED(hr) || !desk)
    return false;
  UINT n = 0;
  hr = desk->GetMonitorDevicePathCount(&n);
  if (FAILED(hr))
  {
    desk->Release();
    return false;
  }
  *count = static_cast<int>(n);
  for (UINT i = 0; i < n; ++i)
  {
    LPWSTR id = nullptr;
    LPWSTR wallpaper = nullptr;
    if (SUCCEEDED(desk->GetMonitorDevicePathAt(i, &id)) && id)
      desk->GetWallpaper(id, &wallpaper);
    if (id)
    {
      char utf8[256] = {0};
      WideCharToMultiByte(CP_UTF8, 0, id, -1, utf8, sizeof(utf8), nullptr, nullptr);
      *fingerprint += utf8;
    }
    if (wallpaper)
    {
      char utf8[1024] = {0};
      WideCharToMultiByte(CP_UTF8, 0, wallpaper, -1, utf8, sizeof(utf8), nullptr, nullptr);
      *fingerprint += "|";
      *fingerprint += utf8;
    }
    *fingerprint += "\n";
    CoTaskMemFree(id);
    CoTaskMemFree(wallpaper);
  }
  desk->Release();
  return true;
}

static BOOL CALLBACK CountMonitor(HMONITOR, HDC, LPRECT, LPARAM data)
{
  int *count = reinterpret_cast<int *>(data);
  ++*count;
  return true;
}

static int MonitorCount()
{
  int count = 0;
  EnumDisplayMonitors(nullptr, nullptr, CountMonitor, reinterpret_cast<LPARAM>(&count));
  return count;
}

static bool ReadSpiWallpaper(std::string *wallpaper)
{
  wchar_t path[1024] = {0};
  if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                             static_cast<UINT>(sizeof(path) / sizeof(path[0])), path, 0))
    return false;
  char utf8[2048] = {0};
  WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, (int)sizeof(utf8), nullptr, nullptr);
  *wallpaper = utf8;
  return true;
}

static bool WaitFor(FILE *unused, ApexFeature *f, char *document, int cap, const char *needle,
                    const char *file, int timeoutMs)
{
  (void)unused;
  const DWORD start = GetTickCount();
  while (GetTickCount() - start < static_cast<DWORD>(timeoutMs))
  {
    document[0] = 0;
    const int n = f->settingsJson(document, cap);
    bool fileExists = true;
    if (file)
    {
      wchar_t pattern[1024] = {0};
      MultiByteToWideChar(CP_UTF8, 0, file, -1, pattern, 1024);
      WIN32_FIND_DATAW data;
      HANDLE find = FindFirstFileW(pattern, &data);
      fileExists = find != INVALID_HANDLE_VALUE;
      if (find != INVALID_HANDLE_VALUE)
        FindClose(find);
    }
    if (n > 0 && n < cap && strstr(document, needle) &&
        fileExists)
      return true;
    Sleep(50);
  }
  return false;
}

int main(int argc, char **argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char *dll = argc >= 2 ? argv[1] : "build/apex/Plugins/Wallpaper/Wallpaper.dll";
  const char *documentPath = argc >= 3 ? argv[2] : "build/_wallpaper_document.json";
  if (argc >= 4)
    _snprintf(g_dir, sizeof(g_dir), "%s", argv[3]);
  if (!g_dir[0])
    return 2;
  EnsureDir(g_dir);

  const apex::wallpaper::winrt::CombaseApi &combase = apex::wallpaper::winrt::GetCombaseApi();
  const bool roOk = combase.ok() && SUCCEEDED(combase.roInitialize(RO_INIT_MULTITHREADED));
  TestLogic();

  puts("== read-only Windows interfaces ==");
  std::string before;
  int displayCount = 0;
  const bool desktopRead = ReadDesktop(&before, &displayCount);
  const int enumCount = MonitorCount();
  Check(enumCount > 0, "EnumDisplayMonitors returns the real monitor layout");
  printf("      IDesktopWallpaper is %s; display count %d\n",
         desktopRead ? "available" : "not available to this tool process", enumCount);
  std::string spiBefore;
  Check(ReadSpiWallpaper(&spiBefore), "SystemParametersInfo reports the current wallpaper");

  bool lockSupported = false;
  HRESULT lockHr = E_FAIL;
  const bool lockRead = ReadLockSupport(&lockSupported, &lockHr);
  Check(lockRead, "the public lock-screen settings interface answers IsSupported");
  printf("      lock screen reported as %s (hr=0x%08lx)\n",
         lockSupported ? "supported" : "not supported", static_cast<unsigned long>(lockHr));

  puts("== real feature DLL ==");
  SetEnvironmentVariableA("APEX_WALLPAPER_NO_APPLY", "1");
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
  EntryFn entry = reinterpret_cast<EntryFn>(reinterpret_cast<void *>(GetProcAddress(mod, "ApexFeatureEntry")));
  if (!entry)
  {
    puts("FAIL: no ApexFeatureEntry");
    FreeLibrary(mod);
    return 1;
  }
  ApexFeature *f = const_cast<ApexFeature *>(entry());
  printf("      ABI %u / %u, struct %u / %u\n", f->abiVersion, APEX_ABI_VERSION, f->structSize,
         (unsigned)sizeof(ApexFeature));
  Check(f->abiVersion == APEX_ABI_VERSION && f->structSize == sizeof(ApexFeature),
        "the DLL matches the host ABI");

  ApexHost host = {};
  host.abiVersion = APEX_ABI_VERSION;
  host.structSize = sizeof(host);
  host.featureDir = HostFeatureDir;
  host.logLine = HostLog;
  host.featureEnabled = HostEnabled;
  Check(f->init && f->init(&host) == 0, "the feature starts its worker");
  Check(Set(f, "schedule", "clock"), "the time-period mode is accepted");
  Check(Set(f, "schedule", "solar"), "the sunrise/sunset mode is accepted");
  Check(Set(f, "schedule", "interval"), "the interval mode is accepted");
  Check(Set(f, "schedule", "clock"), "the probe returns to time-period mode");
  Check(Set(f, "day_start", "06:45"), "the day start accepts HH:MM");
  Check(Set(f, "night_start", "18:30"), "the night start accepts HH:MM");
  Check(!Set(f, "day_start", "25:00"), "an invalid time is refused");
  Check(Set(f, "interval_minutes", "30"), "the interval is accepted");
  Check(!Set(f, "interval_minutes", "0"), "a zero interval is refused");

  char images[620] = {0};
  _snprintf(images, sizeof(images), "%s\\images", g_dir);
  EnsureDir(images);
  char wallA[620] = {0}, wallB[620] = {0}, notes[620] = {0};
  _snprintf(wallA, sizeof(wallA), "%s\\day.bmp", images);
  _snprintf(wallB, sizeof(wallB), "%s\\night.bmp", images);
  _snprintf(notes, sizeof(notes), "%s\\notes.txt", images);
  Check(WriteBmp(wallA, 320, 180, 235, 170, 70), "the day fixture bitmap is written");
  Check(WriteBmp(wallB, 180, 320, 25, 35, 75), "the night fixture bitmap is written");
  FILE *nf = fopen(notes, "wb");
  if (nf)
  {
    fputs("not an image\n", nf);
    fclose(nf);
  }
  char document[64 * 1024] = {0};
  Check(WaitFor(nullptr, f, document, (int)sizeof(document), "\"titleEn\":\"Display 1", nullptr, 8000),
        "the worker discovers and describes the real monitors");
  Check(WaitFor(nullptr, f, document, (int)sizeof(document), "data:image/jpeg;base64,", nullptr, 5000),
        "an unconfigured display is read without taking over its current wallpaper");
  Check(strstr(document, "\"lock_file\":\"\"") == nullptr,
        "the current Windows lock-screen image is resolved without choosing one");
  Check(strstr(document, "\"current/ base\"") == nullptr, "the document uses the feature's own field ids");
  char monitorWallpaper[96] = {0}, monitorStyle[96] = {0}, monitorLock[96] = {0};
  char lockPath[96] = {0};
  _snprintf(monitorWallpaper, sizeof(monitorWallpaper), "monitors[0].wallpaper");
  _snprintf(monitorStyle, sizeof(monitorStyle), "monitors[0].style");
  _snprintf(monitorLock, sizeof(monitorLock), "monitors[0].locked");
  _snprintf(lockPath, sizeof(lockPath), "lock[0].lock_file");
  Check(Set(f, monitorWallpaper, wallA), "one display wallpaper is copied into the plugin folder");
  Check(BlockNextDisplayCopy(), "the probe can occupy the next copy filename");
  Check(Set(f, monitorWallpaper, wallA),
        "rechoosing a wallpaper survives an occupied candidate filename");
  Check(Set(f, monitorStyle, "fill"), "the per-display fill mode is accepted");
  Check(Set(f, monitorStyle, "fit"), "a mode needing a plugin render is accepted");
  Check(!Set(f, monitorStyle, "span"), "the postponed span mode is refused");
  Check(!Set(f, monitorStyle, "warp"), "an unknown display mode is refused");
  Check(Set(f, monitorLock, "1"), "one display wallpaper can be locked");
  Check(!Set(f, monitorLock, "2"), "an invalid wallpaper-lock value is refused");
  Check(Set(f, lockPath, wallB), "the lock-screen image is copied into the plugin folder");

  char rendered[620] = {0};
  _snprintf(rendered, sizeof(rendered), "%sWallpaper.render-monitor-1-*.png", g_dir);
  Check(WaitFor(nullptr, f, document, (int)sizeof(document), "\"id\":\"monitors\"", rendered,
                12000),
        "the real display is rendered using its own mode without applying it");
  Check(strstr(document, "\"id\":\"library\"") == nullptr &&
            strstr(document, "wallpaper_dir") == nullptr,
        "the removed wallpaper folder controls are absent");
  Check(strstr(document, "\"id\":\"switching\"") == nullptr &&
            strstr(document, "\"schedule\"") == nullptr,
        "the retained switching implementation is not exposed in the page");
  Check(strstr(document, "\"id\":\"monitors\"") != nullptr, "the per-monitor controls are described");
  Check(strstr(document, "\"live\":true") != nullptr,
        "the monitor state is marked live so system changes reach the page");
  Check(strstr(document, "\"type\":\"file\"") != nullptr, "direct image controls are file fields");
  Check(strstr(document, "\"hidePath\":true") != nullptr, "wallpaper paths are hidden in the page");
  Check(strstr(document, "\"data:image/jpeg;base64,") != nullptr,
        "a current wallpaper thumbnail reaches the page");
  Check(strstr(document, "alternate_file") == nullptr, "the removed alternate image is absent");
  Check(strstr(document, "\"type\":\"pan\"") == nullptr, "the postponed span control is absent");
  Check(strstr(document, "\"value\":\"fit\"") != nullptr, "the per-display mode reaches the page");
  Check(strstr(document, "\"locked\":1") != nullptr, "the per-display lock reaches the page");

  FILE *doc = fopen(documentPath, "wb");
  if (doc)
  {
    const int n = f->settingsJson(document, (int)sizeof(document));
    if (n > 0)
      fwrite(document, 1, static_cast<size_t>(n), doc);
    fclose(doc);
  }
  Check(f->saveSettings && f->saveSettings() == 1, "the feature saves its settings");

  char ini[620] = {0};
  _snprintf(ini, sizeof(ini), "%sWallpaper.ini", g_dir);
  FILE *saved = fopen(ini, "rb");
  std::string configText;
  if (saved)
  {
    char buf[1024];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), saved)) > 0)
      configText.append(buf, n);
    fclose(saved);
  }
  Check(configText.find("schedule=clock") != std::string::npos,
        "the switching mode survives the settings write");
  Check(configText.find(".wallpaper=") != std::string::npos &&
            configText.find("Wallpaper.display-") != std::string::npos,
        "the copied display wallpaper survives the settings write");
  Check(configText.find("lock_file=") != std::string::npos &&
            configText.find("Wallpaper.lock-") != std::string::npos,
        "the copied lock-screen wallpaper survives the settings write");
  Check(configText.find(".style=fit") != std::string::npos,
        "the per-display mode survives the settings write");
  Check(configText.find(".locked=1") != std::string::npos,
        "the per-display wallpaper lock survives the settings write");
  Check(configText.find("Wallpaper.render-monitor-") == std::string::npos,
        "the settings store source images, not generated render paths");

  if (getenv("APEX_WALLPAPER_TEST_LOCATION"))
  {
    Check(Set(f, "schedule", "solar"), "the optional live-location check selects sunrise/sunset mode");
    Check(WaitFor(nullptr, f, document, (int)sizeof(document),
                  "approximate network location detected", nullptr, 15000),
          "network location is detected without asking the user for latitude/longitude");
  }

  if (f->shutdown)
    f->shutdown();
  FreeLibrary(mod);

  std::string after;
  int afterCount = 0;
  ReadDesktop(&after, &afterCount);
  std::string spiAfter;
  ReadSpiWallpaper(&spiAfter);
  Check(MonitorCount() == enumCount, "the probe leaves the same number of displays");
  Check(spiAfter == spiBefore, "the probe leaves the system wallpaper unchanged");

  if (roOk)
    combase.roUninitialize();
  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
