// ---------------------------------------------------------------------------
// apex_window_icon_probe -- the icon a RUNNING WINDOW is actually holding, and whether it follows the theme.
//
// WHY IT READS THE WINDOW AND NOT THE EXE. The resources can be perfect and the window still show the wrong
// mark, because a window's icon is a value that was SET on it -- a theme change has to trigger another
// WM_SETICON, and a class icon is cached at registration and never refreshed by Windows. Reading the .exe
// proves the artwork exists; only reading the window proves it was applied and re-applied.
//
// It matches windows by process name and class name and then measures the icon each one holds with
// WM_GETICON, falling back to the class icon (which is what the shell uses when a window has none of its
// own). The measurement is the same one the resource probe uses: mean luma, alpha-weighted.
//
// Build: g++ -std=c++17 -O2 -mconsole -o build/apex_window_icon_probe.exe _diag/apex_window_icon_probe.cpp -luser32 -lgdi32
// Run:   apex_window_icon_probe.exe <process-name> [class-name]
// Exit:  0 = every matching window has an icon and it loaded; 1 = a window has none, or the measurement
//        could not be taken. (It does NOT decide which mark is correct -- the theme decides that, and the
//        caller compares against what it expects.)
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct Found
{
  HWND wnd;
  DWORD pid;
  char cls[128];
  HICON icon;
  const char *source;
  double luma;
  int r, g, b;
  bool measured;
};

static double LumaOfIcon(HICON icon, int *outR, int *outG, int *outB)
{
  if (!icon)
    return -1;
  ICONINFO ii = {0};
  if (!GetIconInfo(icon, &ii))
    return -1;
  BITMAP bm = {0};
  if (!GetObject(ii.hbmColor, sizeof(bm), &bm))
  {
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return -1;
  }
  const int w = bm.bmWidth, h = bm.bmHeight;
  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  unsigned char *px = (unsigned char *)calloc((size_t)w * h * 4, 1);
  if (!px)
  {
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return -1;
  }
  HDC dc = GetDC(NULL);
  const int got = GetDIBits(dc, ii.hbmColor, 0, h, px, &bi, DIB_RGB_COLORS);
  ReleaseDC(NULL, dc);
  double sumR = 0, sumG = 0, sumB = 0, sumW = 0;
  if (got)
  {
    for (int i = 0; i < w * h; ++i)
    {
      const double a = px[i * 4 + 3] / 255.0;
      sumR += px[i * 4 + 2] * a;
      sumG += px[i * 4 + 1] * a;
      sumB += px[i * 4 + 0] * a;
      sumW += a;
    }
  }
  free(px);
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  if (!got || sumW <= 0)
    return -1;
  const double r = sumR / sumW, g = sumG / sumW, b = sumB / sumW;
  *outR = (int)(r + 0.5);
  *outG = (int)(g + 0.5);
  *outB = (int)(b + 0.5);
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

static const char *g_exeName = nullptr;
static const char *g_clsName = nullptr;
static Found g_found[16];
static int g_count = 0;

// Is this window's process the one we are looking for? By NAME, not pid: the caller knows the program, and
// the pid changes every run. (The same rule the settings panel uses to find the host.)
static bool ProcessMatches(HWND wnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId(wnd, &pid);
  if (!pid)
    return false;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return false;
  char path[MAX_PATH] = {0};
  DWORD n = sizeof(path);
  const bool ok = QueryFullProcessImageNameA(p, 0, path, &n) != 0;
  CloseHandle(p);
  if (!ok)
    return false;
  const char *base = strrchr(path, '\\');
  base = base ? base + 1 : path;
  return _stricmp(base, g_exeName) == 0;
}

static BOOL CALLBACK Visit(HWND wnd, LPARAM)
{
  if (g_count >= 16)
    return FALSE;
  if (!ProcessMatches(wnd))
    return TRUE;
  char cls[128] = {0};
  GetClassNameA(wnd, cls, sizeof(cls) - 1);
  if (g_clsName && *g_clsName && _stricmp(cls, g_clsName) != 0)
    return TRUE;

  Found &f = g_found[g_count++];
  f.wnd = wnd;
  strncpy(f.cls, cls, sizeof(f.cls) - 1);
  GetWindowThreadProcessId(wnd, &f.pid);
  f.icon = (HICON)SendMessageW(wnd, WM_GETICON, ICON_BIG, 0);
  f.source = "WM_GETICON(ICON_BIG)";
  if (!f.icon)
  {
    f.icon = (HICON)SendMessageW(wnd, WM_GETICON, ICON_SMALL, 0);
    f.source = "WM_GETICON(ICON_SMALL)";
  }
  if (!f.icon)
  {
    f.icon = (HICON)GetClassLongPtrW(wnd, GCLP_HICON);
    f.source = "class icon (GCLP_HICON)";
  }
  if (!f.icon)
  {
    f.icon = (HICON)GetClassLongPtrW(wnd, GCLP_HICONSM);
    f.source = "class icon (GCLP_HICONSM)";
  }
  f.measured = false;
  f.luma = -1;
  if (f.icon)
  {
    f.luma = LumaOfIcon(f.icon, &f.r, &f.g, &f.b);
    f.measured = f.luma >= 0;
  }
  return TRUE;
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("usage: apex_window_icon_probe.exe <process-name.exe> [class-name]\n");
    return 2;
  }
  g_exeName = argv[1];
  g_clsName = argc >= 3 ? argv[2] : "";

  // The window may not exist yet at the instant this is called, so wait for one rather than reporting a
  // failure that is really a race. (Measured: the panel's window appears well after its process does.)
  for (int attempt = 0; attempt < 40; ++attempt)
  {
    g_count = 0;
    EnumWindows(Visit, 0);
    if (g_count > 0)
      break;
    Sleep(250);
  }

  if (g_count == 0)
  {
    printf("no window found for %s%s%s\n", g_exeName, g_clsName && *g_clsName ? " class=" : "",
           g_clsName && *g_clsName ? g_clsName : "");
    return 1;
  }

  int failures = 0;
  for (int i = 0; i < g_count; ++i)
  {
    const Found &f = g_found[i];
    printf("window \"%s\" (pid %lu)\n", f.cls, (unsigned long)f.pid);
    if (!f.icon)
    {
      printf("  NO ICON AT ALL (no WM_GETICON value and no class icon)\n");
      failures++;
      continue;
    }
    if (!f.measured)
    {
      printf("  an icon handle is set (%s) but its pixels could not be read -- not an icon, or a shared one\n",
             f.source);
      failures++;
      continue;
    }
    printf("  %s: meanRGB=%d,%d,%d  luma=%.1f  -> %s\n", f.source, f.r, f.g, f.b, f.luma,
           f.luma > 128 ? "LIGHT mark" : "DARK mark");
  }

  if (failures)
  {
    printf("\nFAILED (%d window(s) without a usable icon)\n", failures);
    return 1;
  }
  printf("\nok\n");
  return 0;
}
