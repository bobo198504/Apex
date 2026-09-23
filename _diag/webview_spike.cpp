// SPIKE: can a MinGW-built exe host WebView2, with the loader linked STATICALLY?
//
// Everything about the settings UI depends on this one fact, so it is proven before any of it is
// written. Static linking matters for a portable app: it is the difference between "one exe beside the
// settings file" and "an exe plus a loader dll that can be lost".
//
// What it does: opens a window, brings up a WebView2 in it, and loads a page from memory (which is how
// the real panel will ship -- the HTML is embedded, not read from disk).
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <stdio.h>
#include "WebView2.h"

using Microsoft::WRL::ComPtr;

static const wchar_t *kWndClass = L"ApexSpikeWnd";
static ComPtr<ICoreWebView2Controller> g_controller;

// A callback is a COM object; these two are the minimum any host needs.
class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (!ppv)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown))
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
    else if (IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler))
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
    else
    {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG r = InterlockedDecrement(&m_ref);
    if (r == 0)
      delete this;
    return (ULONG)r;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override;
};

class ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (!ppv)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown))
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
    else if (IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler))
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
    else
    {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG r = InterlockedDecrement(&m_ref);
    if (r == 0)
      delete this;
    return (ULONG)r;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override;
};

static void Log(const char *fmt, ...)
{
  FILE *f = fopen("spike.txt", "a");
  if (!f)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

HRESULT EnvHandler::Invoke(HRESULT result, ICoreWebView2Environment *env)
{
  if (FAILED(result) || !env)
  {
    Log("env FAILED hr=0x%08lx", (unsigned long)result);
    return S_OK;
  }
  Log("env ok");
  HWND hwnd = FindWindowW(kWndClass, nullptr);
  if (!hwnd)
  {
    Log("no window");
    return S_OK;
  }
  env->CreateCoreWebView2Controller(hwnd, new ControllerHandler());
  return S_OK;
}

HRESULT ControllerHandler::Invoke(HRESULT result, ICoreWebView2Controller *controller)
{
  if (FAILED(result) || !controller)
  {
    Log("controller FAILED hr=0x%08lx", (unsigned long)result);
    return S_OK;
  }
  Log("controller ok");
  g_controller = controller;
  HWND hwnd = FindWindowW(kWndClass, nullptr);
  RECT rc;
  GetClientRect(hwnd, &rc);
  controller->put_Bounds(rc);
  controller->put_IsVisible(TRUE);

  ComPtr<ICoreWebView2> web;
  if (SUCCEEDED(controller->get_CoreWebView2(&web)) && web)
  {
    // THE TWO THINGS THE REAL PANEL DEPENDS ON, so neither is assumed:
    //   1. does prefers-color-scheme follow the SYSTEM (no host code per theme switch)?
    //   2. do the CJK glyphs actually render (the panel is bilingual and the product name is 端)?
    // The page paints what it detected in its own colours, so the screenshot IS the measurement.
    web->NavigateToString(
        L"<!doctype html><html><meta charset=utf-8><body style=\"margin:0;font:14px 'Segoe UI';\">"
        L"<style>"
        L":root{--bg:#faf3dc;--fg:#111;--sub:#6b6250;--line:#111}"
        L"@media (prefers-color-scheme: dark){:root{--bg:#141414;--fg:#f0e9d8;--sub:#9a9384;--line:#f0e9d8}}"
        L"body{background:var(--bg);color:var(--fg)}"
        L"</style>"
        L"<div style=\"padding:24px\">"
        L"<div style=\"font-size:26px;font-weight:600;letter-spacing:.04em\">端 / Apex</div>"
        L"<div style=\"margin-top:10px;color:var(--sub);font-size:13px\">"
        L"\u6ed1\u52a8\u6eda\u8f6e\uFF0C\u4e1d\u6ed1\u5982\u7ee2\u00b7 Smooth Wheel Scroll</div>"
        L"<div id=o style=\"margin-top:18px;font:12px Consolas,monospace\"></div>"
        L"<div style=\"margin-top:12px;height:9px;background:var(--line);border-radius:5px;width:240px\"></div>"
        L"<script>"
        L"var d=matchMedia('(prefers-color-scheme: dark)').matches;"
        L"document.getElementById('o').textContent="
        L"'prefers-color-scheme: '+(d?'DARK':'LIGHT')+"
        L"'  |  \\u4e2d\\u6587\\u6d4b\\u8bd5: \\u7aef\\u3001\\u4e1d\\u6ed1\\u3001\\u52a0\\u901f';"
        L"</script>"
        L"</div></body></html>");
    Log("navigated");
  }
  return S_OK;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_SIZE && g_controller)
  {
    RECT rc;
    GetClientRect(h, &rc);
    g_controller->put_Bounds(rc);
  }
  if (m == WM_DESTROY)
    PostQuitMessage(0);
  return DefWindowProcW(h, m, w, l);
}

// THE CAPTION. Windows does not make a window's title bar follow the system's light/dark setting by
// itself -- an app that paints itself dark still gets a light caption above it, which is exactly what
// the first screenshot showed. The switch is a DWM attribute, and it is a TWO-WAY switch: the same call
// with FALSE turns a dark caption back to light, so this follows the setting rather than forcing a look.
//
// Loaded from dwmapi.dll on demand: the attribute number differs between Windows builds (20 on 10 2004
// and later, 19 before that) and an unsupported call is cosmetic-only, never a failure.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_BORDER_COLOR 34 // Windows 11: a dark caption still draws a light 1px frame without this

static void ApplyTitleBar(HWND h, bool dark)
{
  typedef HRESULT(WINAPI * DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
  static DwmSetWindowAttribute_t fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (dwm)
      fn = (DwmSetWindowAttribute_t)GetProcAddress(dwm, "DwmSetWindowAttribute");
  }
  if (!fn || !h)
  {
    Log("dwmapi unavailable: caption keeps the system colour");
    return;
  }
  const BOOL v = dark ? TRUE : FALSE;
  HRESULT hr = fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof(v));
  if (FAILED(hr))
    hr = fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &v, sizeof(v));
  Log("caption dark=%d (attribute 20 -> 0x%08lx)", dark ? 1 : 0, (unsigned long)hr);
  // The window frame, in the same colour the page uses for its background, so the 1px border does not
  // sit there as a bright line between the caption and the content.
  const COLORREF border = dark ? RGB(0x14, 0x14, 0x14) : RGB(0xfa, 0xf3, 0xdc);
  fn(h, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

// Is the system in light mode? AppsUseLightTheme = 1 means light; anything else (including a missing
// value) means dark, which is the safer default: a dark caption above a dark page is unremarkable,
// while a bright caption flashes against a dark one.
static bool SystemIsLight()
{
  HKEY k = nullptr;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ,
                    &k) != ERROR_SUCCESS)
    return false;
  DWORD v = 0, sz = sizeof(v), type = 0;
  const bool ok = RegQueryValueExA(k, "AppsUseLightTheme", nullptr, &type, (LPBYTE)&v, &sz) ==
                  ERROR_SUCCESS;
  RegCloseKey(k);
  return ok && v != 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
  DeleteFileA("spike.txt");
  Log("spike start");
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSW wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kWndClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, kWndClass, L"Apex spike", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80,
                              560, 420, nullptr, nullptr, inst, nullptr);
  Log("window %p", (void *)hwnd);

  // BEFORE the first paint, so there is no light caption to flash away.
  const bool light = SystemIsLight();
  Log("system is %s", light ? "LIGHT" : "DARK");
  // A/B: comment out to get the DEFAULT caption for comparison
  ApplyTitleBar(hwnd, !light);

  // A user data folder beside the exe, as the portable rule requires; the WebView2 browser process
  // needs somewhere writable and this is the folder the app already owns.
  wchar_t udf[MAX_PATH] = {0};
  {
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash)
      *(slash + 1) = 0;
    _snwprintf(udf, MAX_PATH, L"%sWebView2", exe);
  }
  Log("udf=%ls", udf);

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf, nullptr, new EnvHandler());
  Log("CreateEnv hr=0x%08lx", (unsigned long)hr);

  // Quit by itself so the spike cannot be left running by accident.
  //
  // The message must be matched on its window AND timer id: WM_TIMER carries no other identity, so a
  // bare `msg.message == WM_TIMER` also catches timers this window never set. The first version did
  // exactly that, destroyed the window while the controller was still being created, and the failure
  // surfaced as a misleading E_ABORT from WebView2.
  SetTimer(hwnd, 1, 60000, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0)
  {
    if (msg.message == WM_TIMER && msg.hwnd == hwnd && msg.wParam == 1)
    {
      Log("timeout, quitting");
      DestroyWindow(hwnd);
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_controller)
    g_controller->Close();
  Log("spike end");
  CoUninitialize();
  return 0;
}
