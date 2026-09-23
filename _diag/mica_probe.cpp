// ---------------------------------------------------------------------------
// mica_probe -- CAN A WINDOW MATERIAL REACH THE SCREEN BEHIND A WEBVIEW2 PAGE, AND WHO STEERS ITS
// LIGHT/DARK VARIANT?
//
// WHY IT EXISTS. The question is "can the settings panel use Mica", and three things decide the answer.
// None of them can be settled by reading documentation:
//
//   1. DOES THE MATERIAL SHOW THROUGH THE BROWSER? DWM draws the material BEHIND the window; the page has to
//      be transparent for it to be visible at all. Whether the transparent WebView2 really lets it through,
//      or paints something of its own over it, is a fact about this machine's runtime.
//   2. WHOSE THEME PICKS THE VARIANT? The panel can PIN light/dark AGAINST the system. If the material follows
//      the system instead of this window, "pinned light on a dark system" becomes a dark sheet with light
//      cards on it -- the one outcome that would make this idea a bad one.
//   3. WHAT IS LEFT WHEN THERE IS NO MATERIAL? Windows 10 has none, and Windows itself falls back to a solid
//      colour when the user switches "transparency effects" off. The fallback has to be known, not hoped for.
//
// HOW IT MEASURES, AND WHY NOT PrintWindow. _diag/apex_panel_shot.cpp captures a window with PrintWindow, which
// asks the WINDOW to draw itself -- and the material is drawn by DWM BEHIND the window, so PrintWindow cannot
// see it and would report a black rectangle no matter what. This probe reads the COMPOSITED SCREEN over its own
// rectangle instead, which is what the user's eye actually receives. DwmFlush() is called first so the frame
// being read is one that has already been composed.
//
// WHAT IT REPORTS -- numbers in a log, because "it looked like Mica to me" is not evidence:
//   * the HRESULT of every call, so "the runtime refused transparency" cannot hide behind a good-looking window;
//   * a SAMPLED BAND of the client area that the page leaves transparent: its mean colour, its mean luma and
//     how many DISTINCT colours it holds. A flat surface gives 1; a material tinted by the wallpaper gives many.
//     That difference is what separates "the material came through" from "you are looking at an unpainted window";
//   * the OPAQUE CARD the page draws, as a CONTROL POINT: it must read back exactly the colour the CSS asked
//     for, which is what proves the page is drawn ON TOP of the material rather than beside it;
//   * how many WM_ERASEBKGND and WM_PAINT calls happened. The window must have painted NOTHING for the material
//     to survive, and that is a fact about this process rather than a claim about the picture.
//
// ⚠️⚠️ RUN THIS OUTSIDE THE DSH SANDBOX. Measured 2026-09-21: inside the confined harness, WebView2's browser
// process CRASHES (0x80000003) and Windows puts an "msedgewebview2.exe - 应用程序错误" dialog on the user's
// screen -- which is exactly the disturbance this project's rules forbid. It is not this probe's fault: the
// product's own apex-settings.exe fails the same way there ("WebView2 controller failed (0x8000ffff)"), because
// the confined modes block the named pipes WebView2's IPC is built on. On a normal desktop it is fine.
//
// ⚠️ IT TAKES THE FOREGROUND FOR ABOUT TWO SECONDS PER RUN and puts itself on top: a screenshot of a partly
// covered window measures the covering window. It never touches the keyboard or the mouse, and it is a _diag
// probe -- NOT part of test/run_all.sh, and it does not touch build/apex or the user's own Apex.
//
// ⚠️ THE FLAGS, AND WHY EACH ONE EXISTS (they are the mechanism, measured one at a time):
//   --backdrop=mica|acrylic|micaalt|none   the DWM material (DWMWA_SYSTEMBACKDROP_TYPE)
//   --dark=0|1                             the window's own DWMWA_USE_IMMERSIVE_DARK_MODE
//   --colorkey=0|1                         layered window + a pure-black colour key, and the client area filled
//                                          with that key. MEASURED TO BE THE INGREDIENT THAT MATTERS: without it
//                                          the material never appears at all, however little the window paints.
//   --extend=0|1                           DwmExtendFrameIntoClientArea(-1). Measured to make the numbers
//                                          NOISIER (min 0 / max 255 / 12 distinct colours) on top of the colour
//                                          key, i.e. it is not the mechanism here.
//   --gdi=0|1                              draw a magenta block in the client area: the capture's own control.
//                                          If this does not read back, nothing else in the log means anything.
//
// Build + run: _diag/mica_probe.sh   (builds it, runs the combinations and prints the table)
// ---------------------------------------------------------------------------

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h> // the DWM process's CPU time -- see the perf line
#include <wrl/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WebView2.h"

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t *kWndClass = L"ApexMicaProbeWnd";

// THE CONTROL POINT'S COLOUR, in the two spellings that matter: what the CSS says, and what GetPixel must
// return. They are one colour written twice ON PURPOSE -- this is the assertion, not a duplicated constant.
const char *kCardCss = "#2f7fd4";
const int kCardR = 0x2f, kCardG = 0x7f, kCardB = 0xd4;

// ---- the DWM attribute numbers this probe needs ----------------------------------------------------------
//
// Loaded from dwmapi.dll on demand, like the panel does: the caption attribute's number differs between
// Windows builds (20 on 10 2004+, 19 before), and DWMWA_SYSTEMBACKDROP_TYPE (38) does not exist below
// Windows 11 build 22621 at all -- where the call simply fails and the probe must carry on and say so.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_BORDER_COLOR 34
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMWA_COLOR_NONE 0xFFFFFFFE // "do not draw the 1px frame at all"

// The values of DWM_SYSTEMBACKDROP_TYPE. Named here rather than included, because MinGW's dwmapi.h on this
// toolchain predates the enum.
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2      // Windows 11: Mica in its default variant
#define DWMSBT_TRANSIENTWINDOW 3 // Windows 11: Desktop Acrylic -- the one that samples the desktop LIVE
#define DWMSBT_TABBEDWINDOW 4    // Windows 11: Mica Alt

typedef HRESULT(WINAPI *DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT(WINAPI *DwmFlushFn)(void);
// DwmExtendFrameIntoClientArea takes a MARGINS; declared here rather than included, so the probe does not
// depend on which header MinGW's dwmapi.h happens to put it in.
typedef HRESULT(WINAPI *DwmExtendFn)(HWND, const void *);
struct ProbeMargins
{
  int left, right, top, bottom;
};

DwmSetFn DwmSet()
{
  static DwmSetFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE d = LoadLibraryA("dwmapi.dll"))
      fn = (DwmSetFn)(void *)GetProcAddress(d, "DwmSetWindowAttribute");
  }
  return fn;
}

void DwmFlushNow()
{
  static DwmFlushFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE d = LoadLibraryA("dwmapi.dll"))
      fn = (DwmFlushFn)(void *)GetProcAddress(d, "DwmFlush");
  }
  if (fn)
    fn();
}

// ---- the one log ---------------------------------------------------------------------------------------
// Flushed after every line: the caller waits for `probe done` by polling this file (a condition, not a sleep),
// and a buffered line is indistinguishable from a probe that never got there.
FILE *g_log = nullptr;
const char *g_logPath = "mica_probe.log";

void Log(const char *fmt, ...)
{
  if (!g_log)
  {
    g_log = fopen(g_logPath, "a");
    if (!g_log)
      return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

// ---- what this run was asked to do ---------------------------------------------------------------------
char g_backdrop[32] = "mica";
int g_dark = 0;
// ⚠️ HOW LONG THE WINDOW STAYS UP BEFORE THE MEASUREMENT IS TAKEN, and it is not cosmetic: the material has to
// be ACTIVE for the wall-clock sample to mean anything (see the perf line), and the CPU cost of keeping it on
// screen is exactly what the user asked about. 2.5 s is long enough for a CPU-time delta to be more than a
// rounding error and short enough not to be a nuisance.
DWORD g_holdMs = 2500;
DWORD g_holdStart = 0;
unsigned long long g_cpuSelf0 = 0, g_cpuDwm0 = 0;
int g_settleMs = 500;
int g_extend = 0;
// A GDI mark: a known colour drawn into the window's own surface. It is the capture's own control -- if the
// sampled mark is not that colour, nothing else the capture says can be believed.
int g_gdiMark = 0;
// The colour-key route: a LAYERED window whose client area is filled with a key colour is the other way a GDI
// window can have holes in it (Aero glass did the same thing before WS_EX_LAYERED reached child windows).
int g_colorkey = 0;
const int kMarkX = 700, kMarkY = 500, kMarkSize = 60;
char g_outBmp[512] = {0};

HWND g_wnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
bool g_navDone = false;
DWORD g_navTick = 0;
bool g_captured = false;
int g_erases = 0, g_paints = 0;

bool ArgValue(const char *flag, char *out, int n)
{
  out[0] = 0;
  const char *cl = GetCommandLineA();
  const char *at = cl ? strstr(cl, flag) : nullptr;
  if (!at)
    return false;
  at += strlen(flag);
  int i = 0;
  while (*at && *at != ' ' && *at != '"' && i < n - 1)
    out[i++] = *at++;
  out[i] = 0;
  return i > 0;
}

// ---- THE COST OF THE MATERIAL, in the two currencies a user can feel: CPU time here and in DWM ----------
//
// ⚠️ WHY DWM'S OWN CPU IS THE INTERESTING NUMBER. The material is not drawn by this process -- DWM draws it, for
// every window that asks for one. So "what does Mica cost" is mostly a question about dwm.exe, and this probe's
// own CPU time is only half the answer. Both are CPU time (kernel+user) deltas over the hold window, which is
// wall-clock too, so a busy machine shows up as a small number rather than as a wrong one.
//
// (NOT MEASURED HERE, AND SAID SO PLAINLY: GPU. Reading per-process GPU needs performance counters, not a Win32
// call, and the project's own rule is that a number has to come from somewhere that actually measures it.)
unsigned long long ProcCpuMs(HANDLE h)
{
  FILETIME created, exited, kernel, user;
  if (!h || !GetProcessTimes(h, &created, &exited, &kernel, &user))
    return 0;
  ULARGE_INTEGER k, u;
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  return (k.QuadPart + u.QuadPart) / 10000ULL; // 100 ns ticks -> ms
}

HANDLE DwmProcess()
{
  static HANDLE h = nullptr;
  static bool tried = false;
  if (tried)
    return h;
  tried = true;
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return h;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe))
    do
    {
      if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0)
      {
        // READ-ONLY access to another process's timings; if the system refuses, the line says so instead of
        // reporting a zero as if it were a measurement.
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        break;
      }
    } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
  Log("dwm.exe handle: %s", h ? "opened" : "NOT AVAILABLE (no dwm cpu number will be reported)");
  return h;
}

void StartHold()
{
  g_holdStart = GetTickCount();
  g_cpuSelf0 = ProcCpuMs(GetCurrentProcess());
  const HANDLE d = DwmProcess();
  g_cpuDwm0 = d ? ProcCpuMs(d) : 0;
}

// ⚠️ MAKING THIS WINDOW THE ACTIVE ONE IS PART OF THE MEASUREMENT, NOT POLITENESS. Documented (Microsoft's Mica
// page): Mica "falls back to a solid color ... when an app window on desktop deactivates". The first run of this
// probe measured exactly that fallback (#F3F3F3 / #202020 -- the solid colours) because SetForegroundWindow had
// been refused, and it read as "the effect is not obvious" when in fact the material had not been drawn at all.
//
// The attach-input dance is the standard way to get the foreground without injecting any input: the OS only
// lets the foreground process hand focus on, so this joins this thread's input queue to the current foreground
// window's thread for the duration of the call, and detaches immediately afterwards.
void ForceForeground(HWND h)
{
  HWND fg = GetForegroundWindow();
  const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  const DWORD myThread = GetCurrentThreadId();
  bool attached = false;
  if (fgThread && fgThread != myThread)
    attached = AttachThreadInput(fgThread, myThread, TRUE) != FALSE;
  ShowWindow(h, SW_SHOW);
  BringWindowToTop(h);
  SetForegroundWindow(h);
  SetActiveWindow(h);
  if (attached)
    AttachThreadInput(fgThread, myThread, FALSE);
}

// ---- the window's chrome: the material, the frame, the caption ------------------------------------------
void ApplyChrome(HWND h)
{
  DwmSetFn fn = DwmSet();
  if (!fn)
  {
    Log("dwmapi unavailable: no material, no caption control");
    return;
  }

  // ⚠️ THE OTHER WAY A GDI WINDOW CAN HAVE HOLES IN IT, and the reason it is here: "do not paint" was measured
  // NOT to be enough -- a window that never paints keeps an opaque WHITE surface and DWM composites it over the
  // material. A layered window with a colour key is transparent where it is filled with the key colour, which is
  // the one transparent surface a GDI window can hold (GDI cannot write alpha). The key is pure black, which no
  // part of this probe's page or band uses.
  if (g_colorkey)
  {
    const LONG_PTR ex = GetWindowLongPtrA(h, GWL_EXSTYLE);
    SetWindowLongPtrA(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    const BOOL ok = SetLayeredWindowAttributes(h, RGB(0, 0, 0), 0, LWA_COLORKEY);
    Log("layered + colour key black: %s", ok ? "applied" : "REFUSED");
    InvalidateRect(h, nullptr, TRUE); // the whole surface has to be re-laid as the key colour
    UpdateWindow(h);
  }

  // ⚠️ THE INGREDIENT THAT IS NOT AN ATTRIBUTE. Measured: with the backdrop set and the client area simply
  // NOT PAINTED, the window is still white -- a window that never paints keeps an opaque surface, and DWM
  // composites that surface OVER the material it just drew. Extending the frame with -1 margins makes the whole
  // window frame (the Aero "sheet of glass" trick), which is the region DWM draws the material into. Whether
  // that is what makes the material visible is exactly what the --extend flag is here to measure.
  if (g_extend)
  {
    static DwmExtendFn ext = nullptr;
    static bool tried = false;
    if (!tried)
    {
      tried = true;
      if (HMODULE d = LoadLibraryA("dwmapi.dll"))
        ext = (DwmExtendFn)(void *)GetProcAddress(d, "DwmExtendFrameIntoClientArea");
    }
    ProbeMargins m = {-1, -1, -1, -1}; // -1 = "extend the whole frame into the client area"
    Log("extend frame: -1 margins hr=0x%08lx", (unsigned long)(ext ? ext(h, &m) : E_FAIL));
  }

  int backdrop = DWMSBT_MAINWINDOW;
  if (strcmp(g_backdrop, "none") == 0)
    backdrop = DWMSBT_NONE;
  else if (strcmp(g_backdrop, "acrylic") == 0)
    backdrop = DWMSBT_TRANSIENTWINDOW;
  else if (strcmp(g_backdrop, "micaalt") == 0)
    backdrop = DWMSBT_TABBEDWINDOW;
  const HRESULT hr = fn(h, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
  Log("backdrop: %s (DWMSBT=%d) hr=0x%08lx", g_backdrop, backdrop, (unsigned long)hr);

  // ⚠️ THE PANEL CURRENTLY PAINTS THIS FRAME IN THE PAGE'S BACKGROUND (DWMWA_BORDER_COLOR), which is an OPAQUE
  // 1px line -- drawn over the material along the whole window edge. Asked for no frame at all here so the
  // measurement is not partly a measurement of the old border.
  const COLORREF none = (COLORREF)DWMWA_COLOR_NONE;
  const HRESULT bhr = fn(h, DWMWA_BORDER_COLOR, &none, sizeof(none));
  Log("border: DWMWA_COLOR_NONE hr=0x%08lx", (unsigned long)bhr);

  const BOOL dark = g_dark ? TRUE : FALSE;
  HRESULT chr = fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (FAILED(chr))
    chr = fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &dark, sizeof(dark));
  Log("caption: dark=%d hr=0x%08lx", g_dark, (unsigned long)chr);
}

// ---- the page: transparent everywhere except one opaque card -------------------------------------------
//
// ⚠️ NO `prefers-color-scheme` RULES AND NO COLOUR THAT DEPENDS ON g_dark. The page must look IDENTICAL in the
// two `--dark` runs, or a difference in the sampled band could be the page's own doing instead of the
// material's -- which is exactly the conclusion being tested for.
void BuildPage(char *out, int n)
{
  _snprintf(out, n,
            "<!doctype html><html><head><meta charset=utf-8><style>"
            "html,body{margin:0;height:100%%;background:transparent}"
            "body{color:#ffffff;font:14px 'Segoe UI',sans-serif;-webkit-user-select:none}"
            "#card{position:absolute;left:24px;top:24px;width:340px;padding:16px 18px;border-radius:10px;"
            "background:%s}"
            "</style></head><body><div id=card>"
            "<div style=\"font-size:19px;font-weight:600\">%s / dark=%d / extend=%d / ck=%d</div>"
            "<div style=\"margin-top:6px\">this card is opaque; everything around it is transparent</div>"
            "</div></body></html>",
            kCardCss, g_backdrop, g_dark, g_extend, g_colorkey);
}

// The canvas the sampled band lives in. Constants, not magic numbers used once: the same numbers are quoted in
// the script's report.
const int kBandX0 = 40, kBandX1 = 600, kBandY0 = 200, kBandY1 = 340, kBandStep = 20;

// ---- 32-bit BMP, no image library ----------------------------------------------------------------
// Copied in shape from _diag/apex_panel_shot.cpp, which learned the hard way that Gdiplus::Bitmap::Save
// segfaults on this toolchain AFTER a successful capture -- making a working capture look like a failure.
bool WriteBmp(const char *path, HBITMAP bmp, HDC mem, int w, int h)
{
  BITMAPINFOHEADER bih = {0};
  bih.biSize = sizeof(bih);
  bih.biWidth = w;
  bih.biHeight = h; // positive: bottom-up, which is what a BMP stores
  bih.biPlanes = 1;
  bih.biBitCount = 24;
  bih.biCompression = BI_RGB;

  const int stride = ((w * 3 + 3) / 4) * 4;
  const int dataSize = stride * h;
  unsigned char *rows = (unsigned char *)calloc((size_t)dataSize, 1);
  if (!rows)
    return false;
  if (!GetDIBits(mem, bmp, 0, h, rows, (BITMAPINFO *)&bih, DIB_RGB_COLORS))
  {
    free(rows);
    return false;
  }
  BITMAPFILEHEADER fh = {0};
  fh.bfType = 0x4D42; // "BM"
  fh.bfOffBits = sizeof(fh) + sizeof(bih);
  fh.bfSize = fh.bfOffBits + dataSize;
  FILE *f = fopen(path, "wb");
  if (!f)
  {
    free(rows);
    return false;
  }
  fwrite(&fh, sizeof(fh), 1, f);
  fwrite(&bih, sizeof(bih), 1, f);
  fwrite(rows, (size_t)dataSize, 1, f);
  fclose(f);
  free(rows);
  return true;
}

// ---- THE MEASUREMENT ------------------------------------------------------------------------------------
void Capture()
{
  g_captured = true;

  RECT r;
  GetWindowRect(g_wnd, &r);
  const int w = r.right - r.left, h = r.bottom - r.top;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  // ⚠️ THE INSTRUMENT'S OWN CONTROL, AND IT GOES FIRST. Three points OUTSIDE the window, read straight off the
  // screen. If these come back white too, then this process cannot see the desktop at all -- and every other
  // number in this log would be a statement about a blank capture rather than about the window.
  {
    const COLORREF o1 = GetPixel(screen, 5, 5);
    const COLORREF o2 = (r.left > 10) ? GetPixel(screen, r.left - 10, r.top + 200) : CLR_INVALID;
    const COLORREF o3 = GetPixel(screen, r.right + 10, r.top + 200);
    Log("outside the window (the capture can see the desktop): (5,5)=#%02X%02X%02X left=#%02X%02X%02X "
        "right=#%02X%02X%02X",
        GetRValue(o1), GetGValue(o1), GetBValue(o1), GetRValue(o2), GetGValue(o2), GetBValue(o2),
        GetRValue(o3), GetGValue(o3), GetBValue(o3));
  }
  DwmFlushNow(); // read the frame that has been composed, not the one before it
  const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
  ReleaseDC(nullptr, screen);
  Log("capture: %dx%d at %ld,%ld foreground=%d BitBlt=%s", w, h, r.left, r.top,
      GetForegroundWindow() == g_wnd ? 1 : 0, ok ? "ok" : "FAILED");

  // Client -> window coordinates, so the samples are stated in the page's own frame of reference.
  POINT o = {0, 0};
  ClientToScreen(g_wnd, &o);
  const int ox = o.x - r.left, oy = o.y - r.top;

  const COLORREF card = GetPixel(mem, ox + 120, oy + 70);
  const bool cardOk = card != CLR_INVALID && GetRValue(card) == kCardR && GetGValue(card) == kCardG &&
                      GetBValue(card) == kCardB;
  Log("card: #%02X%02X%02X (want %s) %s", GetRValue(card), GetGValue(card), GetBValue(card), kCardCss,
      cardOk ? "as drawn (the page IS on top of the material)" : "MISMATCH -- the capture is not trustworthy");

  // The GDI control point: this process drew it, so it must come back -- that is what says the capture sees
  // THIS window rather than a blank surface.
  if (g_gdiMark)
  {
    const COLORREF mk = GetPixel(mem, ox + kMarkX + 5, oy + kMarkY + 5);
    const bool mkOk = GetRValue(mk) == 0xFF && GetGValue(mk) == 0x00 && GetBValue(mk) == 0xFF;
    Log("gdi mark: #%02X%02X%02X (want #FF00FF) %s", GetRValue(mk), GetGValue(mk), GetBValue(mk),
        mkOk ? "as drawn -- the capture sees this window" : "MISSING -- the capture is not seeing this window");
  }
  else
    Log("gdi mark: not drawn");

  int n = 0, distinct = 0, minL = 999, maxL = -1;
  long sr = 0, sg = 0, sb = 0;
  double sl = 0;
  COLORREF seen[512];
  for (int y = kBandY0; y <= kBandY1; y += kBandStep)
    for (int x = kBandX0; x <= kBandX1; x += kBandStep)
    {
      const COLORREF c = GetPixel(mem, ox + x, oy + y);
      if (c == CLR_INVALID)
        continue;
      const int R = GetRValue(c), G = GetGValue(c), B = GetBValue(c);
      sr += R; sg += G; sb += B; ++n;
      const int L = (R * 299 + G * 587 + B * 114) / 1000;
      sl += L;
      if (L < minL) minL = L;
      if (L > maxL) maxL = L;
      bool dup = false;
      for (int i = 0; i < distinct; ++i)
        if (seen[i] == c) { dup = true; break; }
      if (!dup && distinct < 512)
        seen[distinct++] = c;
    }
  if (n > 0)
    // `rgb=` and `luma=` are separate fields ON PURPOSE: the script compares the runs by number, and parsing a
    // hex string in awk is a second implementation of something the probe already knows.
    Log("band: n=%d mean=#%02X%02X%02X rgb=%ld,%ld,%ld luma=%.1f min=%d max=%d distinct=%d", n, sr / n, sg / n,
        sb / n, sr / n, sg / n, sb / n, sl / n, minL, maxL, distinct);
  else
    Log("band: NO SAMPLES -- the window is smaller than the sample grid (%d..%d x %d..%d)", kBandX0, kBandX1,
        kBandY0, kBandY1);

  // ⚠️ A COUNT, NOT A CLAIM. "The window painted nothing" is what makes the material visible, and the only
  // honest evidence is how many times Windows asked this process to paint and what it did about it. Both
  // handlers below return without touching a brush; these numbers are what says so.
  Log("painted nothing: WM_ERASEBKGND=%d WM_PAINT=%d", g_erases, g_paints);

  // THE PRICE OF THE PICTURE ABOVE. CPU time spent while the window was up and active, here and in dwm.exe.
  if (g_holdStart)
  {
    const DWORD wall = GetTickCount() - g_holdStart;
    const HANDLE d = DwmProcess();
    const unsigned long self = (unsigned long)(ProcCpuMs(GetCurrentProcess()) - g_cpuSelf0);
    const unsigned long dwm = d ? (unsigned long)(ProcCpuMs(d) - g_cpuDwm0) : 0;
    Log("perf: over %lums  this process cpu=%lums  dwm.exe cpu=%lums", (unsigned long)wall, self, dwm);
  }
  else
    Log("perf: the hold never started, so no CPU numbers (see the watchdog line)");

  if (g_outBmp[0])
    Log("bmp: %s %s", g_outBmp, WriteBmp(g_outBmp, bmp, mem, w, h) ? "written" : "FAILED");

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  Log("probe done");
}

// ---- the two COM callbacks every WebView2 host needs ----------------------------------------------------
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

class NavHandler : public ICoreWebView2NavigationCompletedEventHandler
{
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (!ppv)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown))
      *ppv = static_cast<ICoreWebView2NavigationCompletedEventHandler *>(this);
    else if (IsEqualIID(riid, IID_ICoreWebView2NavigationCompletedEventHandler))
      *ppv = static_cast<ICoreWebView2NavigationCompletedEventHandler *>(this);
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
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args)
      override
  {
    BOOL ok = FALSE;
    args->get_IsSuccess(&ok);
    Log("page navigation: %s", ok ? "done" : "FAILED");
    g_navDone = true;
    g_navTick = GetTickCount();
    return S_OK;
  }
};

HRESULT EnvHandler::Invoke(HRESULT result, ICoreWebView2Environment *env)
{
  if (FAILED(result) || !env)
  {
    Log("WebView2 environment FAILED hr=0x%08lx", (unsigned long)result);
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
    Log("WebView2 controller FAILED hr=0x%08lx", (unsigned long)result);
    return S_OK;
  }
  g_controller = controller;
  RECT rc;
  GetClientRect(g_wnd, &rc);
  controller->put_Bounds(rc);

  ComPtr<ICoreWebView2> web;
  controller->get_CoreWebView2(&web);

  // ⚠️ THE WHOLE QUESTION IN ONE CALL. The controller paints WHITE until the document has painted, so a
  // transparent WebView2 is what lets the material be seen at all -- and the runtime only accepts alpha 0 or
  // 255 (a semi-transparent value fails with E_INVALIDARG, per the SDK's own documentation). A REFUSED call
  // would leave the white default in place, which on screen is indistinguishable from "Mica does not work".
  {
    ICoreWebView2Controller2 *raw = nullptr;
    // Queried by IID rather than with IID_PPV_ARGS: MinGW cannot resolve __uuidof for this SDK's headers (see
    // apex/ui_webview.cpp, where the same link error was met for the same reason).
    if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2, (void **)&raw)) && raw)
    {
      ComPtr<ICoreWebView2Controller2> c2;
      c2.Attach(raw);
      COREWEBVIEW2_COLOR col;
      col.A = 0; // 0 = transparent: "WebView renders the hosting app content as the background"
      col.R = 0;
      col.G = 0;
      col.B = 0;
      const HRESULT hr = c2->put_DefaultBackgroundColor(col);
      Log("transparent background: A=0 hr=0x%08lx %s", (unsigned long)hr,
          SUCCEEDED(hr) ? "(accepted -- the material can be seen)" : "(REFUSED -- the page will be white)");
    }
    else
      Log("transparent background: ICoreWebView2Controller2 is unavailable on this runtime");
  }

  if (ComPtr<ICoreWebView2Settings> s; SUCCEEDED(web->get_Settings(&s)) && s)
  {
    s->put_AreDefaultContextMenusEnabled(FALSE);
    s->put_IsZoomControlEnabled(FALSE);
  }

  if (web)
  {
    EventRegistrationToken tok;
    web->add_NavigationCompleted(new NavHandler(), &tok);
    char html[8192];
    BuildPage(html, sizeof(html));
    wchar_t wide[8192];
    const int n = MultiByteToWideChar(CP_UTF8, 0, html, -1, wide, 8191);
    if (n > 0)
      web->NavigateToString(wide);
    else
      Log("the page could not be converted (error %lu)", GetLastError());
  }

  controller->put_IsVisible(TRUE);
  return S_OK;
}

// ---- the window ----------------------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  // ⚠️ THE HALF OF MICA THAT IS NOT A CALL. DWM draws the material behind the window, so anything this window
  // paints sits ON TOP of it. The panel erases to PageBgColor on every WM_ERASEBKGND and would hide the
  // material completely. Here the erase is swallowed (return 1 = handled, nothing drawn) and WM_PAINT is
  // validated without a brush -- and both are counted, because "the window painted nothing" is the premise the
  // whole measurement rests on.
  case WM_ERASEBKGND:
    ++g_erases;
    // With a colour key the client area must be FILLED with the key colour -- that is what makes the hole. It
    // is still "painting nothing" in the sense that matters: the key is not a colour anybody sees, it is the
    // absence of the window's surface.
    if (g_colorkey)
    {
      RECT rc;
      GetClientRect(h, &rc);
      HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
      FillRect((HDC)wp, &rc, br);
      DeleteObject(br);
    }
    return 1;
  case WM_PAINT:
  {
    ++g_paints;
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    // The same hole, for the regions a paint covers rather than the erase.
    if (g_colorkey)
    {
      RECT rc;
      GetClientRect(h, &rc);
      HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(dc, &rc, br);
      DeleteObject(br);
    }
    // ⚠️ ONLY WHEN ASKED, AND ONLY A BLOCK: this is the capture's control point (see --gdi). It is not painted
    // in a normal run, because a window that paints is a window whose surface covers the material -- which is
    // the whole question being measured.
    if (g_gdiMark)
    {
      RECT m = {kMarkX, kMarkY, kMarkX + kMarkSize, kMarkY + kMarkSize};
      HBRUSH br = CreateSolidBrush(RGB(0xFF, 0x00, 0xFF));
      FillRect(dc, &m, br);
      DeleteObject(br);
    }
    EndPaint(h, &ps);
    return 0;
  }
  case WM_SIZE:
    if (g_controller)
    {
      RECT rc;
      GetClientRect(h, &rc);
      g_controller->put_Bounds(rc);
    }
    return 0;
  case WM_TIMER:
    // Two timers, two jobs, one id each -- WM_TIMER carries nothing else, so they must not share a number.
    if (wp == 1)
    {
      Log("watchdog: the page never finished, giving up (the material still applies)");
      ForceForeground(h);
      if (!g_holdStart)
        StartHold();
      Capture(); // capture anyway: an answer about the material is still an answer
      DestroyWindow(h);
      return 0;
    }
    if (wp == 2 && g_navDone && !g_captured)
    {
      const DWORD t = GetTickCount();
      // Phase 1: let the first paint settle, then start the clock and take the CPU baselines.
      if (!g_holdStart && t - g_navTick >= (DWORD)g_settleMs)
        StartHold();
      // Phase 2: the hold is over. Force the window active ONE MORE TIME before the capture: this is the state
      // in which the material is drawn at all, and it is the only state the numbers above are about.
      if (g_holdStart && t - g_holdStart >= g_holdMs)
      {
        ForceForeground(h);
        Capture();
        DestroyWindow(h);
      }
    }
    return 0;
  case WM_DESTROY:
    if (g_controller)
    {
      g_controller->Close();
      g_controller.ReleaseAndGetAddressOf();
    }
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
  char buf[64] = {0};
  ArgValue("--backdrop=", g_backdrop, sizeof(g_backdrop));
  if (ArgValue("--dark=", buf, sizeof(buf)))
    g_dark = atoi(buf) ? 1 : 0;
  if (ArgValue("--settle=", buf, sizeof(buf)))
    g_settleMs = atoi(buf);
  if (ArgValue("--hold=", buf, sizeof(buf)))
    g_holdMs = (DWORD)atoi(buf);
  if (ArgValue("--extend=", buf, sizeof(buf)))
    g_extend = atoi(buf) ? 1 : 0;
  if (ArgValue("--gdi=", buf, sizeof(buf)))
    g_gdiMark = atoi(buf) ? 1 : 0;
  if (ArgValue("--colorkey=", buf, sizeof(buf)))
    g_colorkey = atoi(buf) ? 1 : 0;
  ArgValue("--out=", g_outBmp, sizeof(g_outBmp));
  char logPath[512] = {0};
  ArgValue("--log=", logPath, sizeof(logPath));
  if (logPath[0])
    g_logPath = logPath;
  DeleteFileA(g_logPath); // one run, one log: a leftover line would be read as this run's answer

  // Physical pixels for GetWindowRect and for the screen capture. The real panel does not need this; a
  // measurement does, because a scaled capture would sample the wrong points.
  SetProcessDPIAware();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  Log("probe start: backdrop=%s dark=%d extend=%d colorkey=%d gdi=%d settle=%dms hold=%lums", g_backdrop, g_dark,
      g_extend, g_colorkey, g_gdiMark, g_settleMs, (unsigned long)g_holdMs);
  Log("(this window takes the foreground for about two seconds and puts itself on top)");

  WNDCLASSW wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kWndClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // ⚠️ NO hbrBackground, ON PURPOSE: a class brush is a second way for Windows to paint over the material
  // (WM_ERASEBKGND is only one of them), and the panel does not set one either.
  RegisterClassW(&wc);

  // Centred in the work area, like the panel, and CLAMPED so it can never open off-screen -- and the same
  // place on every run, because a comparison between two runs only means something if the wallpaper behind the
  // window is the same region of the desktop in both.
  const int w = 900, h = 640;
  RECT work = {0, 0, 0, 0};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || work.right <= work.left)
  {
    work.left = 0;
    work.top = 0;
    work.right = GetSystemMetrics(SM_CXSCREEN);
    work.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  int x = work.left + ((work.right - work.left) - w) / 2;
  int y = work.top + ((work.bottom - work.top) - h) / 2;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;

  g_wnd = CreateWindowExW(WS_EX_TOPMOST, kWndClass, L"Apex mica probe", WS_OVERLAPPEDWINDOW, x, y, w, h,
                          nullptr, nullptr, inst, nullptr);
  if (!g_wnd)
  {
    Log("CreateWindowEx failed (%lu)", (unsigned long)GetLastError());
    return 1;
  }
  ShowWindow(g_wnd, SW_SHOW);
  // ⚠️ ACTIVE, NOT JUST VISIBLE -- and it is forced rather than requested, because a refused
  // SetForegroundWindow silently turns the whole measurement into a measurement of Mica's solid FALLBACK colour
  // (documented: Mica falls back when the window deactivates). That is exactly what the first run measured.
  ForceForeground(g_wnd);
  ApplyChrome(g_wnd); // after the window exists: the material is a property of a window

  // The watchdog only has to outlast a healthy run, and a healthy run now includes the hold.
  SetTimer(g_wnd, 1, g_holdMs + 15000, nullptr); // the watchdog (id 1)
  SetTimer(g_wnd, 2, 100, nullptr);   // "has the page settled" (id 2), a condition rather than a sleep

  // The browser's own data folder, beside the exe -- the portable rule, and the same subfolder name the panel
  // uses. Only this probe's own folder, never build/apex.
  wchar_t udf[MAX_PATH] = {0};
  {
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t *slash = wcsrchr(exe, L'\\'))
      *(slash + 1) = 0;
    _snwprintf(udf, MAX_PATH, L"%sWebView2", exe);
  }
  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf, nullptr, new EnvHandler());
  Log("CreateCoreWebView2EnvironmentWithOptions hr=0x%08lx", (unsigned long)hr);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CoUninitialize();
  return 0;
}
