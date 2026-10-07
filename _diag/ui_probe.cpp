// ---------------------------------------------------------------------------
// ui_probe -- RESTYLING SOMEBODY ELSE'S WINDOW: WHAT ACTUALLY WORKS, AND WHERE THE MENU BAR IS.
//
// WHY IT EXISTS. The idea is a feature that makes old Win32 windows look like the rest of Windows 11: a dark
// title bar, rounded corners, the 1px frame gone. Three things about that cannot be settled by reading
// documentation, and each of them decides whether the feature is worth writing at all:
//
//   1. CAN A DIFFERENT PROCESS DO IT? DwmSetWindowAttribute takes a window handle, and a window handle is valid
//      across processes -- but "valid" is not "permitted". The feature would run inside apex.exe and would by
//      definition be a different process from every window it wants to touch. If this fails, the whole
//      no-injection plan is dead and the answer to the user is "it needs injection".
//   2. WHERE DOES THE MENU BAR LIVE? A popup menu is a window of class #32768, so something can be said about
//      it. A window's MENU BAR is NOT a window at all: USER32 draws it as part of the non-client area, so no
//      DWM attribute can reach it. SetWindowTheme is the only documented cross-process lever that might, and
//      the user asked for menus by name -- so this is measured rather than assumed.
//   3. WHAT DOES IT TAKE TO MAKE AN ALREADY-VISIBLE WINDOW CHANGE? A window that is already on screen may keep
//      its old non-client area until something makes it recalculate. The two documented ways to force that are
//      RedrawWindow(RDW_FRAME) and SetWindowPos(SWP_FRAMECHANGED) -- and they are NOT interchangeable:
//      SWP_FRAMECHANGED sends WM_NCCALCSIZE into the TARGET program, which is a stranger's window and may
//      relayout or flicker on it. "Which one works" and "which one is safe on somebody else's window" are two
//      questions, and this probe photographs the result of each so they can be told apart.
//
// HOW IT MEASURES. It draws its OWN windows (never the user's, never another program's), photographs the
// COMPOSITED SCREEN over each one, and writes a numbered BMP per step. The screen is the only instrument that
// can see a DWM attribute: PrintWindow asks the WINDOW to draw itself, while the title bar and the corners are
// drawn by DWM behind and around it (see _diag/mica_probe.cpp, which learned this the hard way).
//
// ⚠️ CROSS-PROCESS IS MEASURED FOR REAL, BY A SECOND PROCESS. `--apply=<hwnd> --dark=<0|1>` is the same
// executable run again, doing nothing but that one call and exiting. Same-process success would prove nothing
// about the feature, which is always cross-process.
//
// ⚠️ THE TEST WINDOW IS A CLASSIC ONE. This probe is built without a comctl32 v6 manifest, so its controls and
// its menu bar are drawn in the pre-XP style -- which is exactly the "old UI" being asked about, and is why no
// control needs to be placed in it: the non-client area is the whole subject.
//
// ⚠️ THIS IS NOT A GATE AND IT IS NOT IN test/run_all.sh. It takes the foreground for a few seconds, because a
// DWM material is only drawn while its window is active (measured: _diag/mica_probe.cpp). It uses its own exe
// name and its own folder, it does not start, stop or disturb the user's Apex, and it touches no window that
// belongs to anybody else.
//
// Build + run: _diag/ui_probe.sh
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const wchar_t *kWndClass = L"ApexUiProbeWnd";

// ---- the attributes, written out here rather than included --------------------------------------------
//
// MinGW's dwmapi.h on this toolchain predates all of these (see _diag/mica_probe.cpp, which declares them for
// the same reason). The numbers differ between Windows builds for the caption one, so both spellings are kept.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWA_BORDER_COLOR 34
#define DWMWA_CAPTION_COLOR 35
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMWA_COLOR_NONE 0xFFFFFFFE

#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3

#define DWMSBT_AUTO 0
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3
#define DWMSBT_TABBEDWINDOW 4

typedef HRESULT(WINAPI *DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT(WINAPI *DwmGetFn)(HWND, DWORD, LPVOID, DWORD);
typedef HRESULT(WINAPI *DwmFlushFn)(void);
typedef HRESULT(WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);

// The client area's own colour and the control block inside it. TWO CONSTANTS OF THE SAME COLOUR ON PURPOSE:
// what is drawn and what must be read back are one fact written twice, and that is the assertion that the
// capture is looking at THIS window and not at a blank desktop.
const int kClientR = 0xF0, kClientG = 0xF0, kClientB = 0xF0;
const int kMarkR = 0xFF, kMarkG = 0x00, kMarkB = 0xFF; // magenta, used by nothing else here
const int kMarkX = 40, kMarkY = 120, kMarkSize = 48;

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

DwmGetFn DwmGet()
{
  static DwmGetFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE d = LoadLibraryA("dwmapi.dll"))
      fn = (DwmGetFn)(void *)GetProcAddress(d, "DwmGetWindowAttribute");
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

// ⚠️ NAMED EXPORT, AND LOADED BY NAME. SetWindowTheme is a documented export of uxtheme.dll, so unlike the
// dark-mode ordinals (133/135/136, which this probe deliberately does NOT touch -- an ordinal that means
// something else on a newer build is a call into an unknown function) it can be reached without guessing.
SetWindowThemeFn SetTheme()
{
  static SetWindowThemeFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE u = LoadLibraryA("uxtheme.dll"))
      fn = (SetWindowThemeFn)(void *)GetProcAddress(u, "SetWindowTheme");
  }
  return fn;
}

// ---- the one log --------------------------------------------------------------------------------------
FILE *g_log = nullptr;
const char *g_logPath = "ui_probe.log";
char g_bmpDir[512] = ".";

void Log(const char *fmt, ...)
{
  if (!g_log)
  {
    g_log = fopen(g_logPath, "w");
    if (!g_log)
      return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log); // a buffered line is indistinguishable from a probe that never got there
}

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

// ---- 32-bit BMP, no image library ---------------------------------------------------------------------
// Copied in shape from _diag/mica_probe.cpp, which learned the hard way that Gdiplus::Bitmap::Save segfaults
// on this toolchain AFTER a successful capture -- making a working capture look like a failure.
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

HWND g_wnd = nullptr;

// ---- BRING IT TO THE FRONT, WITHOUT TAKING THE FOREGROUND ----------------------------------------------
//
// ⚠️⚠️ THIS IS A DELIBERATE DEPARTURE FROM _diag/mica_probe.cpp, WHICH FORCES THE FOREGROUND WITH
// AttachThreadInput. That probe had to: a DWM MATERIAL is only drawn while its window is active, and measuring
// the material was its entire subject. This probe is about the TITLE BAR, the CORNERS and the BORDER, and all
// three are drawn for an inactive window as well -- the one thing that is not was already measured over there.
//
// What the forcing cost is visible in the first run of this file: AttachThreadInput joins this thread's input
// queue to whatever process happens to own the foreground, and the run died at exactly that point (the log
// stopped one line short of it, and the process was gone). It is a known-delicate call and it is not needed.
//
// ⚠️ AND NOT SEIZING THE FOREGROUND IS THE POLITER CHOICE TOO. This probe runs while the user is working; a
// test that takes their keyboard focus for five seconds is a test that interrupts them. SWP_NOACTIVATE puts
// the window on top and leaves the user's focus where it was -- so the pictures show the INACTIVE title bar,
// which is the state most windows on a desktop are actually in.
void BringToFront(HWND h)
{
  SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ---- PUMP, DO NOT SLEEP --------------------------------------------------------------------------------
//
// ⚠️⚠️ THIS IS WHAT MAKES THE PICTURES MEAN ANYTHING, AND ITS ABSENCE IS NOT A COSMETIC BUG. A window's own
// client area is drawn in WM_PAINT, and WM_PAINT is only delivered to a thread that is READING ITS MESSAGE
// QUEUE. This probe has no GetMessage loop -- every step is a synchronous call -- so without this the window
// never draws itself, the captures show an unpainted rectangle, and the runs that "worked" would have been
// photographs of nothing. (Measured the hard way: the first version slept instead, never wrote a log line,
// and had to be killed from outside.)
void PumpFor(DWORD ms)
{
  const DWORD end = GetTickCount() + ms;
  for (;;)
  {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    const DWORD now = GetTickCount();
    if ((int)(end - now) <= 0)
      break;
    // Wait for a message OR the next slice, whichever comes first: a plain Sleep would not let the paint in.
    DWORD slice = end - now;
    if (slice > 10)
      slice = 10;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
  }
}

// ---- ONE STEP: force the frame to be recomposed, photograph the screen, read the control block ---------
int g_step = 0;

void Step(const char *tag)
{
  char path[600];
  _snprintf(path, sizeof(path), "%s\\%02d-%s.bmp", g_bmpDir, g_step++, tag);

  // Let whatever was just asked for reach the screen: the pump covers the window's own repaint, and DwmFlush
  // waits for the frame in flight to be composed (a different clock). A probe may wait like this; a gate may
  // not (see AGENTS.md).
  PumpFor(250);
  DwmFlushNow();

  RECT r;
  GetWindowRect(g_wnd, &r);
  const int w = r.right - r.left, h = r.bottom - r.top;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
  ReleaseDC(nullptr, screen);

  POINT o = {0, 0};
  ClientToScreen(g_wnd, &o);
  const int ox = o.x - r.left, oy = o.y - r.top;
  const COLORREF mark = GetPixel(mem, ox + kMarkX + 5, oy + kMarkY + 5);
  const bool markOk =
      GetRValue(mark) == kMarkR && GetGValue(mark) == kMarkG && GetBValue(mark) == kMarkB;

  RECT cr;
  GetClientRect(g_wnd, &cr);
  const COLORREF client = GetPixel(mem, ox + 5, oy + 5);
  const bool clientOk = GetRValue(client) == kClientR && GetGValue(client) == kClientG &&
                        GetBValue(client) == kClientB;

  const bool written = WriteBmp(path, bmp, mem, w, h);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);

  Log("step %02d %-22s bmp=%s foreground=%d mark=%s client=%s", g_step - 1, tag,
      written ? path : "FAILED", GetForegroundWindow() == g_wnd ? 1 : 0,
      markOk ? "as drawn" : "MISSING -- the capture is not seeing this window",
      clientOk ? "as drawn" : "CHANGED (something is drawing over the client area)");
}

// ---- the calls, one per function so the log says exactly what was asked ---------------------------------
// ---- WHAT A WINDOW'S CHROME CURRENTLY IS ----------------------------------------------------------------
//
// ⚠️ THIS IS THE MEASUREMENT THE USER'S REQUIREMENT TURNS ON. In the user's words: "有些Win32也会有明暗主题,
// 如果原生有, 就不接管明暗, 只做材质接管, 如果没有, 再做明暗. 明暗随系统设置." Deciding that from OUTSIDE the
// program needs exactly one thing to be readable across processes: the window's current dark-mode flag.
//
//   * If DwmGetWindowAttribute answers for a window this process does NOT own, a feature can compare "what the
//     window says" with "what the system says" and touch only the windows that disagree -- which is exactly
//     "a program that already has its own light/dark is left alone".
//   * If it does not answer, then "does this program follow the system?" cannot be asked at all from here, and
//     the honest design is a user-curated list rather than a guess dressed up as detection.
//
// It reports the HRESULT of every read, so "the read failed" cannot be mistaken for "the value is 0" -- those
// two mean opposite things here, and only one of them would justify overwriting the window.
void ReadChromeOf(HWND h, const char *where)
{
  DwmGetFn get = DwmGet();
  if (!get)
  {
    Log("  read(%s): dwmapi unavailable", where);
    return;
  }
  // -1 rather than 0 as the starting value ON PURPOSE: if every call fails, the log must not read as "this
  // window is light", which is a claim about the window rather than about the instrument.
  BOOL dark = (BOOL)-1;
  HRESULT hd = get(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (FAILED(hd))
    hd = get(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &dark, sizeof(dark));

  int corner = -1;
  const HRESULT hc = get(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

  int backdrop = -1;
  const HRESULT hb = get(h, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

  COLORREF border = 0;
  const HRESULT hbd = get(h, DWMWA_BORDER_COLOR, &border, sizeof(border));

  Log("  read(%s): dark=%d hr=0x%08lx | corner=%d hr=0x%08lx | backdrop=%d hr=0x%08lx | border=#%08lX hr=0x%08lx",
      where, (int)dark, (unsigned long)hd, corner, (unsigned long)hc, backdrop, (unsigned long)hb,
      (unsigned long)border, (unsigned long)hbd);
}

void ReadChrome(const char *where) { ReadChromeOf(g_wnd, where); }

void SetDark(BOOL dark)
{
  DwmSetFn fn = DwmSet();
  if (!fn)
  {
    Log("  dark=%d: dwmapi unavailable", (int)dark);
    return;
  }
  HRESULT hr = fn(g_wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (FAILED(hr))
    hr = fn(g_wnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &dark, sizeof(dark));
  Log("  DwmSetWindowAttribute(DARK_MODE, %d) hr=0x%08lx", (int)dark, (unsigned long)hr);
}

void SetCorner(int pref)
{
  DwmSetFn fn = DwmSet();
  if (!fn)
    return;
  const HRESULT hr = fn(g_wnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
  Log("  DwmSetWindowAttribute(CORNER_PREFERENCE, %d) hr=0x%08lx", pref, (unsigned long)hr);
}

void SetCaptionColor(COLORREF c)
{
  DwmSetFn fn = DwmSet();
  if (!fn)
    return;
  // ⚠️ DWMWA_CAPTION_COLOR takes a COLORREF, and DWMWA_COLOR_DEFAULT is 0xFFFFFFFF. The value is written
  // here as an int, which is what the API reads.
  const int v = (int)c;
  const HRESULT hr = fn(g_wnd, DWMWA_CAPTION_COLOR, &v, sizeof(v));
  Log("  DwmSetWindowAttribute(CAPTION_COLOR, #%02X%02X%02X) hr=0x%08lx", GetRValue(c), GetGValue(c),
      GetBValue(c), (unsigned long)hr);
}

void SetBorderNone(void)
{
  DwmSetFn fn = DwmSet();
  if (!fn)
    return;
  const COLORREF none = (COLORREF)DWMWA_COLOR_NONE;
  const HRESULT hr = fn(g_wnd, DWMWA_BORDER_COLOR, &none, sizeof(none));
  Log("  DwmSetWindowAttribute(BORDER_COLOR, NONE) hr=0x%08lx", (unsigned long)hr);
}

void SetBackdrop(int backdrop)
{
  DwmSetFn fn = DwmSet();
  if (!fn)
    return;
  const HRESULT hr = fn(g_wnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
  Log("  DwmSetWindowAttribute(SYSTEMBACKDROP_TYPE, %d) hr=0x%08lx", backdrop, (unsigned long)hr);
}

void RedrawFrame(void)
{
  const BOOL ok = RedrawWindow(g_wnd, nullptr, nullptr,
                               RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
  Log("  RedrawWindow(RDW_FRAME|RDW_INVALIDATE|RDW_UPDATENOW) -> %d", (int)ok);
}

// ⚠️ THE BLUNT ONE, AND THE ONE THE FEATURE MIGHT HAVE TO AVOID: SWP_FRAMECHANGED sends WM_NCCALCSIZE into
// the window's own process. On this probe's own window that is harmless; on a stranger's window it is a
// message the program did not ask for, and a program is free to relayout, flicker or misbehave on it.
void SetPosFrameChanged(void)
{
  const BOOL ok = SetWindowPos(g_wnd, nullptr, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  Log("  SetWindowPos(SWP_FRAMECHANGED) -> %d", (int)ok);
}

void SetMenuTheme(void)
{
  SetWindowThemeFn fn = SetTheme();
  if (!fn)
  {
    Log("  SetWindowTheme: uxtheme.dll or the export is unavailable");
    return;
  }
  const HRESULT hr = fn(g_wnd, L"DarkMode_Explorer", nullptr);
  Log("  SetWindowTheme(hwnd, L\"DarkMode_Explorer\", nullptr) hr=0x%08lx", (unsigned long)hr);
}

// ---- running this same exe as the OTHER process --------------------------------------------------------
//
// Returns true if it ran in that mode and is done. `--apply=<hwnd> --dark=<0|1>` makes exactly one call and
// exits: that is a genuinely different process from the window's owner, which is the only way to measure what
// the feature will actually be doing.
bool RunAsApplier()
{
  char hwndText[64] = {0}, darkText[64] = {0};
  if (!ArgValue("--apply=", hwndText, sizeof(hwndText)))
    return false;

  const HWND target = (HWND)(ULONG_PTR)_strtoui64(hwndText, nullptr, 10);
  const BOOL dark = ArgValue("--dark=", darkText, sizeof(darkText)) && atoi(darkText) ? TRUE : FALSE;

  char logPath[512] = {0};
  ArgValue("--log=", logPath, sizeof(logPath));
  if (logPath[0])
    g_logPath = logPath;

  // ⚠️ READ FIRST, THEN WRITE, THEN READ AGAIN -- all three from THIS process, which does not own the window.
  //
  //   * Before answers "can a stranger see what this window's light/dark currently is?", which is the only
  //     basis there could be for leaving a program's own choice alone.
  //   * After answers "did the write land?" A write that returns S_OK while the value is unchanged would be a
  //     setting that silently does nothing -- and it would look exactly like success in the log.
  ReadChromeOf(target, "applier, before");

  DwmSetFn fn = DwmSet();
  if (!fn)
  {
    Log("applier: dwmapi unavailable");
    return true;
  }
  HRESULT hr = fn(target, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  const char *which = "DARK_MODE(20)";
  if (FAILED(hr))
  {
    hr = fn(target, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &dark, sizeof(dark));
    which = "DARK_MODE(19)";
  }
  Log("applier: a DIFFERENT process set %s=%d on hwnd=%p hr=0x%08lx", which, (int)dark, (void *)target,
      (unsigned long)hr);
  ReadChromeOf(target, "applier, after");
  Log("applier done");
  return true;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  // THE CLIENT AREA IS PAINTED, AND THAT IS THE POINT: it stands for an ordinary old program, whose client
  // area is opaque and full. Anything DWM draws behind the window is therefore invisible here, which is a
  // fact about the plan rather than a flaw in the probe -- Mica can only be seen where the window does not
  // paint, and an ordinary program paints everywhere.
  case WM_ERASEBKGND:
  {
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH br = CreateSolidBrush(RGB(kClientR, kClientG, kClientB));
    FillRect((HDC)wp, &rc, br);
    DeleteObject(br);
    return 1;
  }
  case WM_PAINT:
  {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH br = CreateSolidBrush(RGB(kClientR, kClientG, kClientB));
    FillRect(dc, &rc, br);
    DeleteObject(br);
    // The capture's own control block: if this does not read back, nothing else the capture says counts.
    RECT m = {kMarkX, kMarkY, kMarkX + kMarkSize, kMarkY + kMarkSize};
    HBRUSH mb = CreateSolidBrush(RGB(kMarkR, kMarkG, kMarkB));
    FillRect(dc, &m, mb);
    DeleteObject(mb);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x20, 0x20, 0x20));
    const wchar_t *line = L"client area -- stands for an ordinary old program, painted everywhere";
    TextOutW(dc, kMarkX, kMarkY + kMarkSize + 12, line, (int)wcslen(line));
    const wchar_t *line2 = L"the menu bar above it is drawn by USER32 as part of the non-client area";
    TextOutW(dc, kMarkX, kMarkY + kMarkSize + 36, line2, (int)wcslen(line2));
    EndPaint(h, &ps);
    return 0;
  }
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
  char buf[64] = {0};

  if (RunAsApplier())
    return 0;

  ArgValue("--dir=", g_bmpDir, sizeof(g_bmpDir));
  char logPath[512] = {0};
  ArgValue("--log=", logPath, sizeof(logPath));
  if (logPath[0])
    g_logPath = logPath;
  DeleteFileA(g_logPath); // one run, one log

  // ⚠️ THE LOG IS OPENED BEFORE ANYTHING ELSE CAN HANG. It used to be opened lazily by the first line, and
  // that line was written AFTER the window had been shown and forced to the foreground -- so a hang in there
  // left no file at all, and "no log" was indistinguishable from "never ran". Diagnosing it cost a killed run.
  Log("ui_probe: starting");

  // Physical pixels: a scaled capture would sample the wrong points.
  SetProcessDPIAware();

  WNDCLASSW wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kWndClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // NO hbrBackground, on purpose: the class brush is a second way for Windows to paint the client area, and
  // WM_ERASEBKGND above is the one this probe measures through.
  RegisterClassW(&wc);

  const int w = 860, h = 520;
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

  // ⚠️ A MENU BAR GOES ON IT, because a menu bar is half of what the user asked about and it only exists on a
  // window that has one. Two items are enough: their COLOURS and their frame are the subject, not their
  // contents.
  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, 1001, L"Open");
  AppendMenuW(file, MF_STRING, 1002, L"Save");
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"File");
  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, 2001, L"About");
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L"Help");

  // ⚠️ TOPMOST, like _diag/mica_probe.cpp: a screenshot of a partly covered window measures the covering
  // window. It is shown for a few seconds and destroys itself.
  // ⚠️ A LINE BEFORE AND A LINE AFTER EVERY STEP THAT COULD FAIL. The first run of this probe wrote its
  // opening line and then nothing at all, and "there is no second line" was the only clue that it had died in
  // the window setup rather than in one of the calls being measured. A log that records only successes cannot
  // say where it stopped.
  Log("  about to create the window (class registered, menu built, %dx%d at %d,%d)", w, h, x, y);
  g_wnd = CreateWindowExW(WS_EX_TOPMOST, kWndClass, L"Apex ui probe -- restyling a stranger's chrome",
                          WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, menu, inst, nullptr);
  if (!g_wnd)
  {
    Log("  CreateWindowEx failed (%lu)", (unsigned long)GetLastError());
    return 1;
  }
  Log("  window created: %p", (void *)g_wnd);
  ShowWindow(g_wnd, SW_SHOW);
  BringToFront(g_wnd);
  Log("  shown and put on top -- WITHOUT taking the foreground from the user");
  PumpFor(300); // let it actually appear and draw before the first photograph

  Log("probe start: a classic (no comctl32 v6 manifest) window with a menu bar, at %d,%d %dx%d", x, y, w, h);
  Log("(it takes the foreground for a few seconds and puts itself on top)");
  Log("");

  // ---- 1. the baseline, and the dark flag applied WITHOUT forcing a redraw -------------------------------
  //
  // ⚠️ THE READS GO BEFORE THE FIRST WRITE. `dark` here is what a window NOTHING has touched says, and that
  // number is half the answer to the question the feature's whole reason to exist rests on: does Windows
  // already darken an old program's title bar by itself (in which case there is little to do), or must every
  // program ask (in which case an old program that never asks is exactly the window the user is complaining
  // about)? The other half is the baseline PICTURE, which shows what that number looks like on screen.
  ReadChrome("baseline, nothing applied yet");
  Step("baseline");
  SetDark(TRUE);
  Step("dark-no-refresh");

  // ---- 2. the two ways to force the frame to be recomposed ----------------------------------------------
  RedrawFrame();
  Step("dark-redraw-frame");

  SetDark(FALSE);
  Step("light-redraw-frame");
  SetDark(TRUE);
  SetPosFrameChanged();
  Step("dark-setpos-framechanged");

  // ---- 3. the rest of the chrome ------------------------------------------------------------------------
  SetCorner(DWMWCP_ROUND);
  Step("corner-round");

  SetCaptionColor(RGB(0x2F, 0x54, 0x8A));
  Step("caption-color");

  SetBorderNone();
  Step("border-none");

  // ---- 4. the menu bar, which is not a window -----------------------------------------------------------
  SetMenuTheme();
  Step("menu-theme-darkmode-explorer");

  // ---- 5. a material on a window that paints everywhere -------------------------------------------------
  SetBackdrop(DWMSBT_MAINWINDOW);
  Step("backdrop-mica");

  // ---- 6. THE SAME CALLS, MADE BY A DIFFERENT PROCESS ---------------------------------------------------
  //
  // This is the measurement the whole plan rests on. The child does one call and exits; the parent WAITS for
  // it (a condition, not a sleep) and photographs. `--dark=0` first, so a change is visible in either
  // direction rather than only one.
  {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char hwndText[64], logText[600];
    _snprintf(hwndText, sizeof(hwndText), "--apply=%llu", (unsigned long long)(ULONG_PTR)g_wnd);
    _snprintf(logText, sizeof(logText), "--log=%s\\applier.log", g_bmpDir);

    const char *modes[2] = {"--dark=0", "--dark=1"};
    const char *tags[2] = {"crossproc-light", "crossproc-dark"};
    for (int i = 0; i < 2; ++i)
    {
      char cmd[2048];
      _snprintf(cmd, sizeof(cmd), "\"%s\" %s %s %s", exe, hwndText, modes[i], logText);
      STARTUPINFOA si = {0};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi = {0};
      if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, g_bmpDir, &si, &pi))
      {
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD rc = 0;
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Log("  a second process (%s) exited with %lu", modes[i], (unsigned long)rc);
      }
      else
        Log("  CreateProcess failed (%lu) -- cross-process NOT measured", (unsigned long)GetLastError());
      BringToFront(g_wnd);
      Step(tags[i]);
    }
  }

  // ---- 7. what the window's own style says --------------------------------------------------------------
  {
    const LONG_PTR style = GetWindowLongPtrW(g_wnd, GWL_STYLE);
    Log("");
    Log("the window's style: WS_CAPTION=%d WS_THICKFRAME=%d WS_SYSMENU=%d WS_VISIBLE=%d", 
        (style & WS_CAPTION) ? 1 : 0, (style & WS_THICKFRAME) ? 1 : 0, (style & WS_SYSMENU) ? 1 : 0,
        (style & WS_VISIBLE) ? 1 : 0);
    Log("screen: %d x %d, work area %ld,%ld %ldx%ld", GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN), work.left, work.top, work.right - work.left, work.bottom - work.top);
  }

  Log("probe done");
  DestroyWindow(g_wnd);
  return 0;
}
