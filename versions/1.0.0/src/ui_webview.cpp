// ---------------------------------------------------------------------------
// THE SETTINGS PANEL'S WINDOW: a WebView2 host, and nothing else.
//
// WHAT THIS PROCESS IS: a window with a browser in it, showing one page that ships inside the exe. It has
// no wheel code and no settings logic of its own -- it draws what the host describes and reports what the
// user does, both through settings_ipc.h.
//
// WHY A BROWSER AT ALL, when the plugin next door draws its panel with plain Win32: because this panel has
// to be bilingual, follow the system's light/dark theme, and lay out a list of features that grows. Those
// are three things CSS and HTML already do well, and re-implementing them in Win32 means re-implementing
// them worse. The cost is measured and bounded: the browser process only exists while this window is open
// (the controller is created lazily and closed on quit), so a closed panel costs nothing.
//
// WHAT THE PAGE CANNOT DO, AND WHY THIS FILE EXISTS: the title bar. Windows draws it, so the page cannot
// reach it; the DWM attribute that makes it dark has to be set from the host process, which is what
// ApplyCaption below does. Everything else about the appearance is the page's.
//
// THE PAGE COMES FROM MEMORY, not from disk: it is compiled into the exe as a resource (see panel.rc).
// Nothing to lose, nothing to resolve relative to a working directory, and no HTML file lying next to the
// program for a user to edit by accident.
// ---------------------------------------------------------------------------

#include "settings_ipc.h"
#include "host.h"       // SystemIsLightTheme: the caption is one part the page cannot draw
#include "hostconfig.h" // ParseHostConfig + ThemeResolvesLight: the panel resolves the theme like anything else
#include "icons.h"      // the two resource ids, shared with panel.rc so they cannot drift apart

// WebView2.h BEFORE wrl/client.h, which is the order that works with MinGW: the SDK header declares the
// COM interfaces and the IIDs, and the wrl wrapper is used on top of them. (Compiling without the SDK is
// not an error: build.sh skips this file, and the host reports that the panel is unavailable.)
#include <wrl/client.h>
#include "WebView2.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace apex {
namespace ui {

using Microsoft::WRL::ComPtr;

namespace {

HWND g_wnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_web;
IpcClient g_ipc;
bool g_ready = false;

// The liveness poll's timer id (see where it is set in RunPanel). A distinct number: WM_TIMER carries nothing
// but this, so two timers on one window must not share it.
#define kHostWatchTimer 1

// Whether the last poll found the host. Kept so the page is told on EVERY CHANGE rather than every 2 seconds
// -- the script call is cheap but a page that redraws its banner twice a second would be a bug of its own.
bool g_hostWasThere = true;

// The host's "something happened" notification (see settings_ipc.h). Registered once, in RunPanel: both
// processes ask Windows for the same id by name, so neither has to know the other's message numbers.
UINT g_activityMsg = 0;


// A log beside the exe, so a panel that fails to come up says why instead of just not appearing. (The host
// has its own; this one is the panel's, and the two are separate files because they are separate
// processes -- a shared file would be two writers with no lock.)
void Log(const char *fmt, ...)
{
  char path[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (char *slash = strrchr(path, '\\'))
    *(slash + 1) = 0;
  strcat(path, "apex-settings.log");
  FILE *f = fopen(path, "a");
  if (!f)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

// ---- the panel's own Windows chrome: the caption, and the window's icon ---------------------------------
//
// BOTH FOLLOW THE SYSTEM'S LIGHT/DARK SETTING, and both have to be re-applied when it changes -- this is the
// part of the window the page cannot reach, so if it is not done here the panel is a dark page under a light
// title bar, which is exactly what "the title bar is still light" meant when it was reported.
//
// THE CAPTION, via a DWM attribute. Measured both ways before relying on it (_diag/DARK_TITLEBAR.md):
// without this call the caption stayed at luma 243 against a dark page. Loaded on demand, because the
// attribute number differs between Windows builds (20 on 10 2004+, 19 before) and an unsupported call is
// cosmetic only -- never a failure.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_BORDER_COLOR 34 // Windows 11: a dark caption still draws a light 1px frame without this

// ---------------------------------------------------------------------------
// THE PAGE'S BACKGROUND COLOUR, in this process -- ONE implementation, three consumers.
//
//   * the 1px window frame (DWMWA_BORDER_COLOR below), which sits between the caption and the page;
//   * the WEBVIEW'S OWN BACKGROUND, which is what shows before the page has painted;
//   * this window's own erase colour, for the moment before the browser is up at all.
//
// WHY THEY MUST ALL BE HERE RATHER THAN WRITTEN OUT WHERE THEY ARE USED: the first two were already two
// copies of the same two hex values, and the user-visible failure of a mismatch is a bright line or a flash
// in a colour that belongs to no theme. Adding a third copy for the white-flash fix would have made that
// three. ⚠️ SO THESE MUST MATCH --bg IN panel.html FOR THE SAME APPEARANCE, and when that value is changed
// there, it is changed here.
//
// THE PALE LIGHT SURFACE IS DELIBERATE and is the user's own correction: "浅色UI，颜色饱和度改低些，让它看起来再白
// 淡些，现在太黄了". The mark's saturated cream (#F4EAC6) was being used as a page background and read as yellow
// over a whole window, so the light surfaces were desaturated -- see the note in panel.html, which is where the
// reasoning and the values live.
// ---------------------------------------------------------------------------
COLORREF PageBgColor(bool dark)
{
  return dark ? RGB(0x10, 0x13, 0x0c) : RGB(0xf8, 0xf7, 0xf4);
}

// ---------------------------------------------------------------------------
// THE APPEARANCE, before the page has told us: read out of apex.ini with the SAME parser and resolved by the
// SAME rule the host uses.
//
// ⚠️ WHY THIS EXISTS, and it was found by a gate rather than by eye: the controller is created BEFORE the page
// runs, so at that moment the panel has not been told which appearance is in force. It used to fall back to
// the SYSTEM's theme -- which is wrong whenever the user has PINNED one, and wrong in the worst direction: a
// pinned LIGHT theme on a dark system painted a #10130c (near-black) surface before the page came up, so the
// panel flashed BLACK instead of white. The user's report was the white flash; this was a second, darker
// version of it waiting behind the first fix.
//
// THE CONFIG IS READ, NOT GUESSED. apex.ini sits beside this exe (the portable layout -- see paths.h) and
// `ParseHostConfig` is a pure header, so the panel can read the user's own theme setting without a round trip
// to the host, without the host having to be running, and without a second copy of the format. It then
// resolves it through `ThemeResolvesLight`, which is the ONE place that question is answered.
//
// IF ANYTHING IS MISSING the system's answer is used, which is the same fallback the host takes when there is
// no settings file at all. That is a guess, but it is the product's own guess rather than a new one.
bool ResolveAppearanceFromConfig()
{
  char path[560] = {0};
  {
    char exe[MAX_PATH] = {0};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH))
      return !apex::host::SystemIsLightTheme();
    char *slash = strrchr(exe, '\\');
    if (!slash)
      return !apex::host::SystemIsLightTheme();
    *(slash + 1) = 0;
    _snprintf(path, sizeof(path), "%sapex.ini", exe);
  }
  HostConfig cfg;
  if (FILE *f = fopen(path, "rb"))
  {
    static char text[8192];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    apex::ParseHostConfig(text, cfg);
  }
  return apex::ThemeResolvesLight(cfg.theme, apex::host::SystemIsLightTheme());
}

void ApplyCaption(HWND h, bool dark)
{
  typedef HRESULT(WINAPI * DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
  static DwmSetFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE dwm = LoadLibraryA("dwmapi.dll"))
      fn = (DwmSetFn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
  }
  if (!fn || !h)
    return;
  const BOOL v = dark ? TRUE : FALSE;
  if (FAILED(fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof(v))))
    fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &v, sizeof(v));
  // The 1px window frame, in the page's own background, so it does not draw a bright line between the
  // caption and the page under it. One source for the colour -- see PageBgColor.
  const COLORREF border = PageBgColor(dark);
  fn(h, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

// The icon for the given appearance, through the same single mapping the host's tray uses (icons.h) -- this
// file does not know which artwork belongs to which appearance, and is not the place to decide it.
void ApplyWindowIcon(HWND h, bool dark)
{
  const int id = ApexMarkForAppearance(!dark);
  HINSTANCE inst = GetModuleHandleW(nullptr);
  const HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(id), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON), 0);
  const HICON small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(id), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  HICON oldBig = (HICON)SendMessageW(h, WM_SETICON, ICON_BIG, 0);
  HICON oldSmall = (HICON)SendMessageW(h, WM_SETICON, ICON_SMALL, 0);
  // Only a handle that loaded is set: a NULL would CLEAR the icon instead of leaving the previous one.
  if (big)
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
  if (small)
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small);
  if (oldBig && oldBig != big)
    DestroyIcon(oldBig);
  if (oldSmall && oldSmall != small)
    DestroyIcon(oldSmall);
}

// WHICH WAY THE PANEL'S CHROME SHOULD LOOK, and who decides it.
//
// The PAGE is the authority, not this process. It resolves the theme the same way the settings say: a pinned
// light/dark comes straight from `data-theme`, and `auto` is its own `prefers-color-scheme` -- and it reports
// the answer through the `nativeTheme` message. Re-deriving it here from the system would be a second answer
// to a question that already has one, and it shows the moment a theme is pinned: the page would be dark
// while this process painted a light caption and a mark chosen for a light background over it.
//
// `g_pinned` is why the page sends two fields and not one. The page only re-reports on a system change when
// it is following the system (`auto`); a pinned theme is unaffected by that change and stays silent, because
// reporting again would say nothing new. So this process has to know which of the two it is holding: on a
// system change it re-reads the system when the theme is `auto`, and re-applies what it already has when the
// theme is pinned. Without that distinction a pinned theme would be quietly undone by the next system change.
bool g_haveResolved = false;
bool g_resolvedDark = false;
bool g_pinned = false;

void ApplyResolvedAppearance(HWND h)
{
  if (!h)
    return;
  // THE PAGE'S ANSWER WHEN IT HAS ONE, otherwise the config's -- NOT the bare system theme. See
  // ResolveAppearanceFromConfig: the system's answer is only correct while the theme is `auto`, and the
  // window is drawn before the page can say which it is.
  const bool dark = g_haveResolved ? g_resolvedDark : !ResolveAppearanceFromConfig();
  ApplyCaption(h, dark);
  ApplyWindowIcon(h, dark);
}

// The page's own answer. One call so the caption and the icon can never be applied in different modes --
// which they were, briefly, when the icon was set at class-registration time and the caption at
// window-creation time.
void SetResolvedAppearance(HWND h, bool dark, bool pinned)
{
  g_haveResolved = true;
  g_resolvedDark = dark;
  g_pinned = pinned;
  ApplyResolvedAppearance(h);
}

// A system theme change. `auto` follows it, a pinned theme does not (and the page will not have re-reported
// one), so the answer is taken from the system only in the first case -- RE-READ THROUGH THE CONFIG, so a
// pinned theme gets its own answer rather than the system's. (The pinned branch above already handles the
// known case; this is the one where the page has not reported yet, and reading the config is what keeps a
// pinned light theme from being flipped to dark by a system change that has nothing to do with it.)
void OnSystemThemeChanged(HWND h)
{
  if (g_haveResolved && g_pinned)
    ApplyResolvedAppearance(h);
  else
    SetResolvedAppearance(h, !ResolveAppearanceFromConfig(), false);
}

// ---- the page, out of the exe's own resources ----------------------------------------------------
bool LoadPanelHtml(char *out, int outSize)
{
  HRSRC res = FindResourceA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(256), RT_RCDATA);
  if (!res)
    return false;
  HGLOBAL g = LoadResource(GetModuleHandleA(nullptr), res);
  if (!g)
    return false;
  const DWORD n = SizeofResource(GetModuleHandleA(nullptr), res);
  const char *p = (const char *)LockResource(g);
  if (!p || n == 0 || (int)n >= outSize)
    return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// ---- C++ -> JS -----------------------------------------------------------------------------------
//
// The answers the host sends are handed to the page by CALLING A FUNCTION ON IT, which is the one direction
// that has no race: ExecuteScript is queued and runs in page order. Posting a message instead would arrive
// while the page might still be parsing.
void CallJs(const wchar_t *fn, const char *jsonArgOrNull)
{
  if (!g_web)
    return;
  wchar_t script[80] = {0};
  if (!jsonArgOrNull)
    _snwprintf(script, 80, L"window.%s()", fn);
  else
  {
    // The JSON is inserted as a STRING LITERAL and parsed on the page, not spliced in as code: the payload
    // contains user text (blacklist entries), and splicing it into a script would make that text code.
    // Escaped for a JS string: backslash and quote.
    static wchar_t wide[70000];
    int w = 0;
    wide[w++] = L'w';
    wide[w++] = L'i';
    wide[w++] = L'n';
    wide[w++] = L'd';
    wide[w++] = L'o';
    wide[w++] = L'w';
    wide[w++] = L'.';
    const wchar_t *f = fn;
    while (*f && w < (int)(sizeof(wide) / sizeof(wide[0])) - 8)
      wide[w++] = *f++;
    wide[w++] = L'(';
    wide[w++] = L'\'';
    for (const char *p = jsonArgOrNull; *p && w < (int)(sizeof(wide) / sizeof(wide[0])) - 8; ++p)
    {
      const unsigned char c = (unsigned char)*p;
      if (c == '\\' || c == '\'')
      {
        wide[w++] = L'\\';
        wide[w++] = (wchar_t)c;
      }
      else if (c < 0x80)
        wide[w++] = (wchar_t)c;
      else
      {
        // A UTF-8 byte sequence becomes a \uXXXX escape per byte would be wrong; instead the bytes are
        // decoded back to a code point and emitted as one. The page then reads the JSON as text and
        // JSON.parse turns the escapes back into the original characters.
        //
        // (Decoding is done here rather than handing raw bytes through because the script is UTF-16.)
        unsigned cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = c; extra = 0; }
        for (int i = 0; i < extra && p[1]; ++i)
        {
          ++p;
          cp = (cp << 6) | ((unsigned char)*p & 0x3F);
        }
        if (cp <= 0xFFFF)
          w += _snwprintf(wide + w, 8, L"\\u%04x", cp);
        else
        {
          cp -= 0x10000;
          w += _snwprintf(wide + w, 8, L"\\u%04x", 0xD800 + (cp >> 10));
          w += _snwprintf(wide + w, 8, L"\\u%04x", 0xDC00 + (cp & 0x3FF));
        }
      }
    }
    wide[w++] = L'\'';
    wide[w++] = L')';
    wide[w] = 0;
    // The script and its result are logged because a silent failure here is invisible: the page simply
    // never updates and every layer looks like it worked. (This was a real hunt -- the request reached the
    // host, the host answered, the document arrived, and the panel still said the host was not running.)
    Log("script[%d]: %.120ls", w, wide);
    HRESULT hr = g_web->ExecuteScript(wide, nullptr);
    Log("  ExecuteScript -> 0x%08lx", (unsigned long)hr);
    return;
  }
  static HRESULT hr2 = g_web->ExecuteScript(script, nullptr);
  Log("script simple: %.60ls -> 0x%08lx", script, (unsigned long)hr2);
}

// ---- JS -> C++: one request, from the page -------------------------------------------------------
//
// THE MESSAGES THE PAGE SENDS ARRIVE AS JSON, and the ones the host understands are a small fixed set --
// "snapshot", "setFeature", and so on. Anything else is ignored rather than guessed at, so a page that is
// newer or older than this exe degrades to doing nothing instead of doing something wrong.
class WebMessageHandler : public ICoreWebView2WebMessageReceivedEventHandler
{
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (!ppv)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown))
      *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(this);
    else if (IsEqualIID(riid, IID_ICoreWebView2WebMessageReceivedEventHandler))
      *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(this);
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
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) override;
};

// Pull one field out of the page's JSON request. The page sends flat objects, so this is a substring hunt
// rather than a parser -- and it is deliberately tolerant: a missing field is an empty string, which the
// command handlers treat as "not supplied".
bool JsonString(const wchar_t *json, const wchar_t *key, char *out, int outSize)
{
  if (!json || !key || !out || outSize <= 0)
    return false;
  out[0] = 0;
  wchar_t pat[64] = {0};
  _snwprintf(pat, 64, L"\"%s\":", key);
  const wchar_t *at = wcsstr(json, pat);
  if (!at)
    return false;
  at += wcslen(pat);
  while (*at == L' ')
    ++at;
  int n = 0;
  if (*at == L'"')
  {
    ++at;
    while (*at && *at != L'"' && n < outSize - 1)
    {
      const wchar_t c = *at++;
      if (c < 0x80)
        out[n++] = (char)c;
      else if (c < 0x800)
      {
        if (n + 1 < outSize - 1)
        {
          out[n++] = (char)(0xC0 | (c >> 6));
          out[n++] = (char)(0x80 | (c & 0x3F));
        }
      }
      else
      {
        if (n + 2 < outSize - 1)
        {
          out[n++] = (char)(0xE0 | (c >> 12));
          out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
          out[n++] = (char)(0x80 | (c & 0x3F));
        }
      }
    }
  }
  else
  {
    // A bare number or true/false: copy the token.
    while (*at && *at != L',' && *at != L'}' && n < outSize - 1)
      out[n++] = (char)((*at < 0x80) ? *at : L'?'), ++at;
  }
  out[n] = 0;
  return n > 0;
}

HRESULT WebMessageHandler::Invoke(ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args)
{
  // ⚠️ get_WebMessageAsJson, NOT TryGetWebMessageAsString.
  //
  // The page posts an OBJECT (`postMessage({cmd:"snapshot", ...})`), and TryGetWebMessageAsString only
  // succeeds for a message that was posted as a plain STRING -- for anything else it returns E_INVALIDARG.
  // The first version used it and checked FAILED(...) -> return S_OK, so every single request from the page
  // was silently dropped and the panel reported "the Apex host is not running" while the host was running
  // perfectly well next to it.
  //
  // get_WebMessageAsJson gives the JSON text of whatever was posted, object or string, which is what the
  // reader below expects.
  LPWSTR json = nullptr;
  if (FAILED(args->get_WebMessageAsJson(&json)) || !json)
    return S_OK;

  char cmd[64] = {0};
  if (!JsonString(json, L"cmd", cmd, sizeof(cmd)))
  {
    Log("page message with no cmd: %ls", json);
    CoTaskMemFree(json);
    return S_OK;
  }
  Log("page -> host: %s", cmd);

  // ---- request -> the host ----
  char payload[1024] = {0};
  char num[64] = {0};

  if (strcmp(cmd, "snapshot") == 0)
  {
    g_ipc.Send(kIpcSnapshot, nullptr);
  }
  else if (strcmp(cmd, "describe") == 0)
  {
    if (JsonString(json, L"slot", num, sizeof(num)))
      g_ipc.Send(kIpcDescribe, (strcat(strcpy(payload, "slot="), num), payload));
  }
  else if (strcmp(cmd, "setFeature") == 0)
  {
    char slot[24] = {0}, id[80] = {0}, value[64] = {0};
    JsonString(json, L"slot", slot, sizeof(slot));
    JsonString(json, L"id", id, sizeof(id));
    JsonString(json, L"value", value, sizeof(value));
    _snprintf(payload, sizeof(payload), "slot=%s\nid=%s\nvalue=%s\n", slot, id, value);
    g_ipc.Send(kIpcSetFeature, payload);
  }
  else if (strcmp(cmd, "setHost") == 0)
  {
    char key[40] = {0}, value[80] = {0};
    JsonString(json, L"key", key, sizeof(key));
    JsonString(json, L"value", value, sizeof(value));
    _snprintf(payload, sizeof(payload), "key=%s\nvalue=%s\n", key, value);
    g_ipc.Send(kIpcSetHost, payload);
  }
  // A LIST CONTROL ON A FEATURE'S PAGE. The host process does not know what the list means -- it finds the
  // feature by slot and forwards "add"/"remove" untouched (see ApexFeature::listOp). `index` is sent as a
  // string like every other field, because the payload is line-oriented text.
  else if (strcmp(cmd, "listOp") == 0)
  {
    char slot[24] = {0}, id[80] = {0}, op[24] = {0}, value[300] = {0}, index[24] = {0};
    JsonString(json, L"slot", slot, sizeof(slot));
    JsonString(json, L"id", id, sizeof(id));
    JsonString(json, L"op", op, sizeof(op));
    JsonString(json, L"value", value, sizeof(value));
    JsonString(json, L"index", index, sizeof(index));
    _snprintf(payload, sizeof(payload), "slot=%s\nid=%s\nop=%s\nvalue=%s\nindex=%s\n", slot, id, op, value,
              index);
    g_ipc.Send(kIpcListOp, payload);
  }
  else if (strcmp(cmd, "featureOff") == 0)
  {
    char slot[24] = {0}, value[24] = {0};
    JsonString(json, L"slot", slot, sizeof(slot));
    JsonString(json, L"value", value, sizeof(value));
    _snprintf(payload, sizeof(payload), "slot=%s\nvalue=%s\n", slot, value);
    g_ipc.Send(kIpcFeatureOff, payload);
  }
  else if (strcmp(cmd, "openFile") == 0)
  {
    if (JsonString(json, L"slot", num, sizeof(num)))
      g_ipc.Send(kIpcOpenFile, (strcat(strcpy(payload, "slot="), num), payload));
  }
  else if (strcmp(cmd, "pageError") == 0)
  {
    // The page reporting its own failure. It cannot log anywhere the host can read, so it sends the text
    // here; without this a broken document or a script error is completely invisible from outside the web
    // view (measured: it looked exactly like "the host is not running").
    char what[400] = {0};
    JsonString(json, L"what", what, sizeof(what));
    Log("PAGE ERROR: %s", what);
  }
  else if (strcmp(cmd, "nativeTheme") == 0)
  {
    // The page tells the host which way its theme resolved: it is the only party that knows, because a
    // pinned light/dark lives in the page's own `data-theme` (see ApplyResolvedAppearance). Both parts of
    // the chrome the page cannot draw are set from it -- the caption AND the window icon -- so the two can
    // never disagree with the page or with each other. This is not forwarded to the other process at all; it
    // is purely this window's own appearance.
    char dark[16] = {0};
    char pinned[16] = {0};
    JsonString(json, L"dark", dark, sizeof(dark));
    JsonString(json, L"pinned", pinned, sizeof(pinned));
    SetResolvedAppearance(g_wnd, atoi(dark) != 0, atoi(pinned) != 0);
  }

  // THE ANSWER IS ALSO THE REFRESH. After a change, ask again, so what is shown is always what the host
  // holds rather than the page guessing what its own edit did. (A snapshot or a describe produces its own
  // document, so it does not need this.)
  //
  // ⚠️ EXCEPT setFeature, WHICH FIRES PER PIXEL OF A DRAG. A snapshot makes the page re-read the feature's
  // controls, and re-reading rebuilds the page -- which DESTROYS THE SLIDER THE USER IS HOLDING. Measured
  // from outside: the drag died after the first input event, so a slider could be clicked but not dragged.
  // Nothing is refreshed here on purpose: the page already knows the value it just sent, and host settings
  // and the feature list (all a snapshot carries) cannot have changed from a control move. A feature that
  // clamps a value out of range would need this back, but the panel only offers the range it was given.
  //
  // ⚠️ NOTHING IS DELIVERED TO THE PAGE HERE. The reply arrives as a WM_COPYDATA at our own window
  // procedure while the call above is still on the stack (the host answers synchronously), and WndProc is
  // the one place that knows whether the document is a snapshot or a feature's controls -- it picks the
  // function by the document's own shape. Handing it to the page here as well would both double-deliver it
  // and have to guess which kind it was, which is what the first version did (it always called the controls
  // entry point, so a snapshot went to the wrong function and the panel stayed empty).
  if (strcmp(cmd, "snapshot") != 0 && strcmp(cmd, "nativeTheme") != 0 &&
      strcmp(cmd, "describe") != 0 && strcmp(cmd, "pageError") != 0 &&
      strcmp(cmd, "setFeature") != 0)
    g_ipc.Send(kIpcSnapshot, nullptr);

  CoTaskMemFree(json);
  return S_OK;
}

// ---- startup callbacks --------------------------------------------------------------------------
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

HRESULT EnvHandler::Invoke(HRESULT result, ICoreWebView2Environment *env)
{
  if (FAILED(result) || !env)
  {
    Log("WebView2 environment failed (0x%08lx)", (unsigned long)result);
    MessageBoxA(g_wnd, "WebView2 could not start.\n\nThe panel needs the Microsoft Edge WebView2 Runtime.",
                "Apex", MB_ICONINFORMATION);
    DestroyWindow(g_wnd);
    return S_OK;
  }
  Log("WebView2 environment ready");
  env->CreateCoreWebView2Controller(g_wnd, new ControllerHandler());
  return S_OK;
}

HRESULT ControllerHandler::Invoke(HRESULT result, ICoreWebView2Controller *controller)
{
  if (FAILED(result) || !controller)
  {
    Log("WebView2 controller failed (0x%08lx)", (unsigned long)result);
    return S_OK;
  }
  g_controller = controller;
  controller->get_CoreWebView2(&g_web);
  g_ready = true;

  RECT rc;
  GetClientRect(g_wnd, &rc);
  controller->put_Bounds(rc);

  // ⚠️ THE WHITE FLASH FIX, AND IT HAS TO BE DONE HERE RATHER THAN IN THE PAGE: a WebView2 control paints
  // WHITE from the moment it is visible until the page has drawn. The page cannot help -- it is not running
  // yet, and CSS has nothing to apply to a surface that is not showing the document. The user's report is
  // "主界面的打开的时候，会先白一下".
  //
  // put_DefaultBackgroundColor is the colour the CONTROLLER paints before the document has painted, so
  // setting it to the page's own background makes the flash be the panel's colour instead of white -- in
  // either theme, and with no timing assumptions at all (which is what makes this the right layer for it:
  // no deferral, nothing to get wrong when a load is slow).
  {
    // ⚠️ QUERIED BY IID, NOT WITH IID_PPV_ARGS. That macro expands to __uuidof, which MinGW can only resolve
    // for interfaces declared through its own __CRT_UUID_DECL -- and this SDK's headers declare theirs with
    // `EXTERN_C __declspec(selectany) const IID ...`, which is MSVC syntax. The result is a link error
    // ("undefined reference to __mingw_uuidof<ICoreWebView2Controller2>()"), not a compile error. The rest of
    // this file has always used the IID constants directly for the same reason.
    ICoreWebView2Controller2 *raw = nullptr;
    if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2, (void **)&raw)) && raw)
    {
      ComPtr<ICoreWebView2Controller2> c2;
      c2.Attach(raw);
      // ⚠️ THE CONFIG'S ANSWER, NOT THE SYSTEM'S -- this line runs BEFORE the page does, so a pinned theme
      // is only knowable from apex.ini. Using the system here is what painted a near-black surface under a
      // pinned LIGHT theme; see ResolveAppearanceFromConfig.
      const COLORREF bg = PageBgColor(g_haveResolved ? g_resolvedDark : !ResolveAppearanceFromConfig());
      COREWEBVIEW2_COLOR col;
      // The API takes 0xAARRGGBB; a COLORREF is 0x00BBGGRR, so the two channels swap.
      col.A = 255;
      col.R = GetRValue(bg);
      col.G = GetGValue(bg);
      col.B = GetBValue(bg);
      const HRESULT hr = c2->put_DefaultBackgroundColor(col);
      // ⚠️ LOGGED WITH ITS RESULT, because this whole fix is invisible from inside the process: the flash it
      // removes lasts a few frames, and whether it is gone can only be judged by eye. What CAN be recorded is
      // that the call was made and whether the runtime took it -- so a future "the panel flashes white again"
      // has somewhere to start. (Same reasoning as the tray's own log line: when the thing being changed is
      // displayed by somebody else, the log is the only evidence this process has.)
      Log("webview background: #%02x%02x%02x (0x%08lx)", (unsigned)col.R, (unsigned)col.G, (unsigned)col.B,
          (unsigned long)hr);
    }
    // NO FAILURE BRANCH ON PURPOSE. ICoreWebView2Controller2 exists on every supported runtime; if it is
    // somehow absent the only consequence is that the old white flash comes back, which is a cosmetic
    // regression and not something to refuse to draw the panel over. The log line above is what says so.
  }

  controller->put_IsVisible(TRUE);

  // Nothing in this page is a document: no context menu, no zoom via the keyboard, no dev tools gestures.
  // A settings panel that can be zoomed by accident is a settings panel with a layout bug waiting.
  if (ComPtr<ICoreWebView2Settings> s; SUCCEEDED(g_web->get_Settings(&s)) && s)
  {
    s->put_AreDefaultContextMenusEnabled(FALSE);
    s->put_IsZoomControlEnabled(FALSE);
    // The panel has no links and loads nothing from the network; a page that could navigate away from
    // itself would be a page that could be replaced by anything.
    s->put_IsWebMessageEnabled(TRUE);
  }

  EventRegistrationToken tok;
  g_web->add_WebMessageReceived(new WebMessageHandler(), &tok);

  static char html[512 * 1024];
  if (!LoadPanelHtml(html, (int)sizeof(html)))
  {
    Log("the panel's HTML resource is missing");
    MessageBoxA(g_wnd, "The panel resource is missing from this build.", "Apex", MB_ICONERROR);
    return S_OK;
  }

  // FROM MEMORY, so there is no file to lose and no working directory to get right. The page is UTF-8; the
  // API takes a wide string, so it is converted once here.
  {
    static wchar_t wide[600 * 1024];
    const int n = MultiByteToWideChar(CP_UTF8, 0, html, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])) - 1);
    Log("page: %d chars from %d bytes", n, (int)strlen(html));
    if (n > 0)
    {
      wide[n] = 0;
      // A marker from the END of the document, so a truncated load is visible in the log rather than
      // showing up as a missing element inside the page. (It did: the panel reported "missing element
      // #nohost" while the resource demonstrably contained it.)
      Log("  page ends with: %.24ls", wide + (n > 30 ? n - 30 : 0));
      g_web->NavigateToString(wide);
    }
    else
    {
      Log("  the page could not be converted (error %lu)", GetLastError());
    }
  }

  Log("panel loaded");

  // ⚠️ NOTHING IS PUSHED TO THE PAGE HERE, and the reason is worth recording because the first version did
  // it and it looked correct:
  //
  // NavigateToString only STARTS the load. A push right after it runs against a document that has not been
  // parsed yet, so `window.__apexSnapshot` does not exist, the call throws inside the web view, and
  // ExecuteScript still returns S_OK -- so every layer reports success and the panel stays empty.
  //
  // THE PAGE ASKS INSTEAD. Its own script calls `snapshot` once it is running, which is the only moment
  // that is guaranteed to be after its functions exist. That request arrives through the normal path and is
  // answered there. (The alternative -- an add_NavigationCompleted callback that pushes -- is also correct,
  // but it needs a second handler class for no gain, and "the page asks when it is ready" is a smaller rule
  // to remember.)
  g_ipc.Attach(g_wnd);
  return S_OK;
}

// ---- the window ----------------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  // THE WINDOW'S OWN ERASE, in the page's background colour. This covers the moment before the browser has
  // painted anything at all -- the controller's own background (set when it is created) takes over from
  // there. Without this the class's default brush erases to the system's window colour, which in a light
  // appearance is white: the same flash, one layer further down. See PageBgColor.
  case WM_ERASEBKGND:
  {
    RECT rc;
    GetClientRect(h, &rc);
    // The config's answer when the page has not reported yet -- see ResolveAppearanceFromConfig. (This runs
    // on every erase, so it is worth saying why it is cheap: the config is read only while `g_haveResolved`
    // is false, which is the first moments of the process, and one file read there is nothing next to
    // starting a browser.)
    const bool dark = g_haveResolved ? g_resolvedDark : !ResolveAppearanceFromConfig();
    HBRUSH br = CreateSolidBrush(PageBgColor(dark));
    FillRect((HDC)wp, &rc, br);
    DeleteObject(br);
    return 1; // handled: no default erase, so the colour above is the only one drawn
  }

  case WM_SIZE:
    if (g_controller)
    {
      RECT rc;
      GetClientRect(h, &rc);
      g_controller->put_Bounds(rc);
    }
    return 0;

  // IS THE HOST STILL THERE? (see where the timer is set in RunPanel). Asked from this process because it is
  // the one that can: a page cannot see another process's windows. The page is told only on a CHANGE, so a
  // healthy session produces no work at all.
  //
  // ⚠️ AND WHEN THE HOST IS GONE, THE PANEL GOES WITH IT. The user's request: "主界面在托盘进程退出，也要跟着
  // 关闭". The panel is a window onto the host -- it has nothing to show and nothing to edit without it, and a
  // panel that stayed up would be a window whose every control silently does nothing (the page already knows
  // this and dims itself, which is the honest thing to do but is not what a user wants to look at).
  //
  // THE CLOSE IS DEFERRED, NOT DONE HERE. This runs on a WM_TIMER, and destroying the window from inside its
  // own message is how a program reaches a half-torn-down state; a posted WM_CLOSE is delivered on a later
  // turn of the loop, after this handler has returned. It is also the same path the user's own close takes, so
  // the exit route is one route rather than two.
  case WM_TIMER:
    if (wp == kHostWatchTimer)
    {
      const bool there = g_ipc.HostIsRunning();
      if (there != g_hostWasThere)
      {
        g_hostWasThere = there;
        Log("host is %s", there ? "back" : "gone");
        CallJs(there ? L"__apexHostBack" : L"__apexHostGone", nullptr);
      }
      if (!there)
      {
        Log("the host is gone -- closing the panel with it");
        PostMessageA(h, WM_CLOSE, 0, 0);
        KillTimer(h, kHostWatchTimer); // and stop asking: the answer cannot change before we are gone
      }
      return 0;
    }
    break;

  case WM_COPYDATA:
  {
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
    Log("WndProc WM_COPYDATA dwData=%lu cbData=%lu", cds ? cds->dwData : 0UL,
        cds ? cds->cbData : 0UL);
    if (cds && cds->dwData == kIpcAnswer)
    {
      // The host answered a request. Hand it to the page; the page decides which function to call from the
      // document's own shape (it asks for a snapshot or for a feature's controls).
      static char doc[kIpcMaxDocument];
      const IpcDocument *d = (const IpcDocument *)cds->lpData;
      int n = (cds->cbData >= sizeof(IpcDocument)) ? d->length : (int)cds->cbData - (int)offsetof(IpcDocument, text);
      if (n > 0 && n < (int)sizeof(doc))
      {
        memcpy(doc, d->text, (size_t)n);
        doc[n] = 0;
        // A snapshot has "host" and "features"; controls have "params". The document's own shape decides
        // which entry point the page gets, so neither side needs a field saying which kind it is.
        const bool isSnap = strstr(doc, "\"host\"") != nullptr;
        Log("document %d bytes -> %s", n, isSnap ? "snapshot" : "controls");
        Log("  content: %s", doc);
        if (isSnap)
          CallJs(L"__apexSnapshot", doc);
        else
          CallJs(L"__apexControls", doc);
      }
      else
        Log("document rejected: n=%d cbData=%lu", n, cds->cbData);
      return 1;
    }
    // (A kIpcChanged branch lived here: the host telling the panel that something changed outside the panel,
    // so the page should re-read. It was unreachable -- the host has no such producer any more, because the
    // only things that used to change without the panel were the tray's own switches, and those are gone.
    // A message that can never arrive is worse than no message: it reads as a live path.)
    return 0;
  }

  case WM_SETTINGCHANGE:
  case WM_THEMECHANGED:
    // The system's theme may have changed. The PAGE follows prefers-color-scheme by itself (that is the whole
    // reason it is a web page) and re-reports when it is following the system; only the two parts Windows
    // draws are this process's job, and OnSystemThemeChanged decides which answer applies -- see the note on
    // g_pinned above, which is the difference between "follow the system" and "the system just overrode the
    // user's pinned theme".
    OnSystemThemeChanged(h);
    return 0;

  case WM_CLOSE:
    // Closing the panel must save what was changed, and the save is the HOST's job: it owns the files.
    g_ipc.Send(kIpcSave, nullptr);
    DestroyWindow(h);
    return 0;

  case WM_DESTROY:
    // THE CONTROLLER IS CLOSED EXPLICITLY. Simply dropping the reference leaves the browser process alive
    // for as long as this one, which would mean a settings panel that was closed still costs memory.
    if (g_controller)
    {
      g_controller->Close();
      g_controller.ReleaseAndGetAddressOf();
    }
    g_web.ReleaseAndGetAddressOf();
    PostQuitMessage(0);
    return 0;
  }

  // THE HOST SAYING SOMETHING HAPPENED (a wheel was taken, and how many). Checked here rather than in a case
  // label because the id is registered at run time -- the same reason the host checks "TaskbarCreated" the same
  // way.
  //
  // The count goes to the page, which is what moves the curve; this process does nothing with it beyond passing
  // it on. It is POSTED (see settings_ipc.h), so it can arrive while the page is mid-render, and the page
  // coalesces whatever it finds -- an animation does not need every single count.
  if (g_activityMsg != 0 && msg == g_activityMsg)
  {
    wchar_t script[64] = {0};
    _snwprintf(script, 64, L"window.__apexActivity(%u)", (unsigned)wp);
    if (g_web)
      g_web->ExecuteScript(script, nullptr);
    return 0;
  }

  return DefWindowProcA(h, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
// The panel's entry point. Returns when the window closes.
// ---------------------------------------------------------------------------
int RunPanel(HINSTANCE inst, bool startHidden)
{
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  // The "something happened" notification, by name so both processes agree without sharing a number.
  g_activityMsg = RegisterWindowMessageA(APEX_ACTIVITY_MSG_NAME);

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = APEX_SETTINGS_WND_CLASS;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // The class's own icon: only the DEFAULT for windows of this class, and taken through the same single
  // mapping (icons.h). ApplyResolvedAppearance sets the real one per window right after creation, and
  // re-sets it on a theme change, which a class icon never does by itself.
  // ⚠️ THE CONFIG, not the system: this runs before the page has reported, so a pinned theme is only knowable
  // from apex.ini. It matters here for the same reason as the background -- the window is on screen (title
  // bar icon included) before the page exists. See ResolveAppearanceFromConfig.
  const int classIcon = ApexMarkForAppearance(ResolveAppearanceFromConfig());
  wc.hIcon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(classIcon), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                               GetSystemMetrics(SM_CYICON), 0);
  RegisterClassA(&wc);

  // WHERE THE PANEL OPENS: the middle of the work area, clamped so it can never open off-screen.
  //
  // ⚠️ NOT CENTRED ON THE HOST'S WINDOW, which was the first version and was a real bug: the host's window
  // is a HIDDEN message-only window at 0,0 sized 0x0 -- that is all it needs to be, since it exists for the
  // tray icon to post to. Centring on it put the panel at about (-420,-330) and it opened off-screen where
  // it could be neither seen nor moved into view.
  //
  // The work area (not the full screen) is used so the panel does not open under the taskbar.
  const int w = 980, h = 700;
  RECT work = {0, 0, 0, 0};
  if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0) || work.right <= work.left)
  {
    work.left = 0;
    work.top = 0;
    work.right = GetSystemMetrics(SM_CXSCREEN);
    work.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  int x = work.left + ((work.right - work.left) - w) / 2;
  int y = work.top + ((work.bottom - work.top) - h) / 2;
  if (x < work.left)
    x = work.left; // a screen smaller than the panel: the top-left corner is the only option
  if (y < work.top)
    y = work.top;

  // THE CHROME BEFORE THE FIRST PAINT, so a light title bar cannot flash above a dark page. The system's
  // answer is used here because the page has not run yet; the page then reports its own resolution -- which
  // is the one that counts, since a pinned theme lives there -- and re-reports on a system change.
  g_wnd = CreateWindowExA(0, wc.lpszClassName, "Apex", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                          inst, nullptr);
  if (!g_wnd)
    return 1;
  ApplyResolvedAppearance(g_wnd);
  if (!startHidden)
    ShowWindow(g_wnd, SW_SHOW);

  // The browser's own data folder, beside the exe: the portable rule again. It is created here rather than
  // in the host because only the panel ever runs a browser.
  wchar_t udf[MAX_PATH] = {0};
  {
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t *slash = wcsrchr(exe, L'\\'))
      *(slash + 1) = 0;
    _snwprintf(udf, MAX_PATH, L"%sWebView2", exe);
  }
  CreateCoreWebView2EnvironmentWithOptions(nullptr, udf, nullptr, new EnvHandler());

  // THE HOST CAN DIE WHILE THIS PANEL IS OPEN, AND THE PAGE HAS TO BE TOLD. Without this the panel keeps
  // showing the last snapshot as if it were live, every edit disappears into a window that is not there, and
  // "I clicked and nothing happened" is the only symptom the user gets -- there is no reply to notice, because
  // the page's messages go out one-way (see hostCall in panel.html).
  //
  // The check runs here because THIS process is the one that can do it: it is a FindWindow in its own address
  // space, and the answer is a fact about Windows rather than about the session. The page's own attempt at
  // this (a setInterval whose body was empty) could never have worked -- a page cannot see another process's
  // windows at all.
  //
  // 2 s is slow enough to cost nothing and fast enough that the warning appears while the user is still
  // wondering why their click did nothing.
  SetTimer(g_wnd, kHostWatchTimer, 2000, nullptr);

  MSG msg;
  while (GetMessageA(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  CoUninitialize();
  return 0;
}

} // namespace ui
} // namespace apex
