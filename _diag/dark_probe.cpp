// ---------------------------------------------------------------------------
// dark_probe -- CAN A MENU BE MADE DARK, AND A WINDOW'S OWN BACKGROUND, WITHOUT INJECTING ANYBODY?
//
// WHY IT EXISTS. The user's target is "menus and the main panel's background", and those are NOT the same
// problem, so this probe measures them separately:
//
//   1. THE MENU. Dark menus in Windows 10 1809+ are a UXTHEME feature: a process that asks for it gets its
//      menus (and scroll bars, and the common dialogs) DRAWN DARK BY THE SYSTEM. That matters enormously for
//      cost -- "let the system draw it dark" is a different order of work from "repaint the controls
//      ourselves". The levers are a family of UNDOCUMENTED uxtheme exports:
//
//          ordinal 133  AllowDarkModeForWindow(HWND, BOOL)   -- per WINDOW
//          ordinal 135  SetPreferredAppMode(int)             -- per PROCESS
//          ordinal 136  FlushMenuThemes(void)                -- make it take effect now
//
//      ⚠️⚠️ THE QUESTION THIS PROBE IS REALLY ASKING IS ABOUT ordinal 133. A feature that cannot inject anybody
//      can never call 135 (process state, must run inside the target). But 133 takes a WINDOW HANDLE, and this
//      project has already MEASURED that a window handle crosses processes for the same family of calls
//      (_diag/ui_probe.cpp: SetWindowTheme on another process's window worked, it simply had no effect on the
//      menu bar). SO: IF 133 IS EFFECTIVE ACROSS PROCESSES, DARK MENUS NEED NO INJECTION AT ALL.
//
//      ⚠️ AND THE ORDINALS ARE GUESSED, so every call here happens inside this probe's own process and on this
//      probe's own windows: if a guess is wrong, the crash lands here and nothing else on the machine notices.
//
//   2. ⚠️⚠️ VISUAL STYLES ARE THE PREMISE, AND THE FIRST RUN OF THIS PROBE GOT THAT WRONG. uxtheme only draws
//      what uses THEMES. A process without a comctl32 v6 manifest -- which is what every old program is, and
//      what the first version of this probe built -- has its menus drawn by USER32 the classic way, and asking
//      uxtheme for a dark one can only do nothing. THAT WOULD HAVE BEEN READ AS "DARK MENUS ARE IMPOSSIBLE"
//      WHEN IT ONLY MEANT "NOT FOR THIS KIND OF WINDOW". So there are TWO windows now: one built without the
//      activation context (the classic kind, i.e. the old programs the user is complaining about) and one with
//      it (the themed kind). One `SetPreferredAppMode` call then tells the two apart.
//
//   3. THE WINDOW'S OWN BACKGROUND. Old programs paint their client area themselves, so a DWM attribute cannot
//      reach it -- but many do not paint a COLOUR, they paint a SYSTEM colour (COLOR_3DFACE for a dialog,
//      COLOR_WINDOW for an edit). SetSysColors rewrites that table for the whole session. The change is
//      machine-wide, so it lives behind its own switch (`--syscolors`) and is put back immediately.
//
// HOW IT MEASURES. It photographs the COMPOSITED SCREEN over its own windows and writes a numbered BMP per
// step, and it samples the MENU BAR strip and reports the mean colour as three numbers -- "it looks darker to
// me" is not evidence, and the first version of this file proved it by producing ten byte-identical pictures.
//
// ⚠️ NOT A GATE, and not in test/run_all.sh. It puts its own windows on screen, it briefly opens ITS OWN popup
// menu (ended again within a fraction of a second: an open menu is modal and eats mouse input, and this
// project forbids a test from touching the user's input devices), and it touches NO window that belongs to
// anybody else. `--syscolors` also changes one system colour for about a second and restores it on every exit
// path, including the crash path.
//
// Build + run: _diag/dark_probe.sh
// ---------------------------------------------------------------------------

#include <windows.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const wchar_t *kClassClassic = L"ApexDarkProbeClassic";
const wchar_t *kClassThemed = L"ApexDarkProbeThemed";

// ---- DwmFlush, on demand -------------------------------------------------------------------------------
typedef HRESULT(WINAPI *DwmFlushFn)(void);

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

// ---- the chrome attributes, for a MENU this time -------------------------------------------------------
typedef HRESULT(WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);

void DwmSetAttr(HWND h, DWORD attr, const void *val, DWORD size)
{
  static DwmSetAttrFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE d = LoadLibraryA("dwmapi.dll"))
      fn = (DwmSetAttrFn)(void *)GetProcAddress(d, "DwmSetWindowAttribute");
  }
  if (fn)
    fn(h, attr, val, size);
}

#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWA_BORDER_COLOR 34
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMWCP_DEFAULT 0
#define DWMWCP_ROUND 2
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3 // Desktop Acrylic

// ---- the OTHER blur path: the undocumented accent on user32 -------------------------------------------
//
// ⚠️ THIS IS THE ONE apex's OWN QUICK PANEL TRIED AND REJECTED, and the reason is worth carrying over here:
// the accent paints THE WHOLE WINDOW RECTANGLE, so for a window whose visible shape is smaller than its
// rectangle (the flyout, with transparent gaps between its panes) it covers the gaps. A MENU IS A RECTANGLE,
// so that objection does not apply to it -- which is exactly why it is worth measuring here rather than
// inheriting the earlier verdict.
enum
{
  ACCENT_DISABLED = 0,
  ACCENT_ENABLE_BLURBEHIND = 3,
  ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};
struct AccentPolicy
{
  int nAccentState;
  int nFlags;
  int nColor; // ABGR
  int nAnimationId;
};
struct WinCompAttrData
{
  int nAttribute;
  void *pData;
  SIZE_T ulDataSize;
};
#define WCA_ACCENT_POLICY 19

typedef BOOL(WINAPI *SetWindowCompositionAttributeFn)(HWND, WinCompAttrData *);

void SetAccent(HWND h, int state, unsigned color)
{
  static SetWindowCompositionAttributeFn fn = nullptr;
  static bool tried = false;
  if (!tried)
  {
    tried = true;
    if (HMODULE u = GetModuleHandleA("user32.dll"))
      fn = (SetWindowCompositionAttributeFn)(void *)GetProcAddress(u, "SetWindowCompositionAttribute");
  }
  if (!fn)
    return;
  AccentPolicy p = {0};
  p.nAccentState = state;
  p.nFlags = 2;
  p.nColor = color;
  WinCompAttrData d = {WCA_ACCENT_POLICY, &p, sizeof(p)};
  fn(h, &d);
}

// ---- the undocumented uxtheme exports -------------------------------------------------------------------
typedef BOOL(WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL); // 133
typedef int(WINAPI *SetPreferredAppModeFn)(int);            // 135
typedef void(WINAPI *FlushMenuThemesFn)(void);              // 136

AllowDarkModeForWindowFn g_allowDark = nullptr;
SetPreferredAppModeFn g_preferMode = nullptr;
FlushMenuThemesFn g_flushThemes = nullptr;

void LoadUxTheme()
{
  HMODULE u = LoadLibraryA("uxtheme.dll");
  if (!u)
    return;
  // ⚠️ BY ORDINAL: none of the three has an exported name.
  g_allowDark = (AllowDarkModeForWindowFn)(void *)GetProcAddress(u, MAKEINTRESOURCEA(133));
  g_preferMode = (SetPreferredAppModeFn)(void *)GetProcAddress(u, MAKEINTRESOURCEA(135));
  g_flushThemes = (FlushMenuThemesFn)(void *)GetProcAddress(u, MAKEINTRESOURCEA(136));
}

// ---- the one log ---------------------------------------------------------------------------------------
FILE *g_log = nullptr;
const char *g_logPath = "dark_probe.log";
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
  fflush(g_log); // ⚠️ EVERY LINE: this probe may be killed, and a buffered line is a line that never existed
}

// ---- arguments -----------------------------------------------------------------------------------------
//
// ⚠️ TWO FUNCTIONS, AND THE FIRST VERSION HAD ONLY ONE -- WHICH IS WHY `--syscolors` RAN AS THE MENU MODE.
// `ArgValue` is for `--flag=value`; it walks from the end of the flag to the next space, so a flag with NO
// value yields an empty string and reports "not present". A valueless switch needs `HasFlag`.
bool HasFlag(const char *flag)
{
  const char *cl = GetCommandLineA();
  return cl && strstr(cl, flag) != nullptr;
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

// ---- BMP writer, copied in shape from _diag/ui_probe.cpp -----------------------------------------------
bool WriteBmp(const char *path, HBITMAP bmp, HDC mem, int w, int h)
{
  BITMAPINFOHEADER bih = {0};
  bih.biSize = sizeof(bih);
  bih.biWidth = w;
  bih.biHeight = h;
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
  fh.bfType = 0x4D42;
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

// ---- PUMP, DO NOT SLEEP -------------------------------------------------------------------------------
// A window's client area is drawn in WM_PAINT, and WM_PAINT only reaches a thread READING ITS MESSAGE QUEUE.
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
    DWORD slice = end - now;
    if (slice > 10)
      slice = 10;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
  }
}

int g_step = 0;

void BringToFront(HWND h)
{
  SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ---- ONE STEP: photograph one window, and read the two numbers that matter ----------------------------
//
// `pumpMs` is 0 when this is called from INSIDE a menu's modal loop: that loop is already pumping this
// thread's messages, and pumping them again from here would feed the menu its own input a second time.
void StepOf(HWND h, const char *tag, DWORD pumpMs = 250)
{
  if (!h)
    return;
  char path[600];
  _snprintf(path, sizeof(path), "%s\\%02d-%s.bmp", g_bmpDir, g_step++, tag);

  if (pumpMs)
    PumpFor(pumpMs);
  DwmFlushNow();

  RECT r;
  GetWindowRect(h, &r);
  const int w = r.right - r.left, ht = r.bottom - r.top;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, ht);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, ht, screen, r.left, r.top, SRCCOPY);
  ReleaseDC(nullptr, screen);

  POINT o = {0, 0};
  ClientToScreen(h, &o);
  const int ox = o.x - r.left, oy = o.y - r.top;

  // The capture's own control: a known colour this process painted.
  const COLORREF mark = GetPixel(mem, ox + 45, oy + 45);

  // THE MENU BAR STRIP, just inside the top of the client area. These three numbers are the evidence; the
  // first version of this probe produced ten byte-identical pictures and would have been read as "nothing
  // works" when in fact its own menu never appeared.
  long mr = 0, mg = 0, mb = 0;
  int n = 0;
  for (int x = 60; x < 300; x += 10)
  {
    const COLORREF c = GetPixel(mem, ox + x, oy + 4);
    if (c == CLR_INVALID)
      continue;
    mr += GetRValue(c);
    mg += GetGValue(c);
    mb += GetBValue(c);
    ++n;
  }

  // And the client area, well below the menu bar: this is the "main panel background" the user asked about.
  const COLORREF body = GetPixel(mem, ox + 400, oy + 300);

  const bool written = WriteBmp(path, bmp, mem, w, ht);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);

  Log("step %02d %-30s bmp=%s mark=%s menubar=%ld,%ld,%ld body=#%02X%02X%02X", g_step - 1, tag,
      written ? "written" : "FAILED",
      (GetRValue(mark) == 0xFF && GetGValue(mark) == 0x00 && GetBValue(mark) == 0xFF) ? "as drawn" : "MISSING",
      n ? mr / n : -1, n ? mg / n : -1, n ? mb / n : -1, GetRValue(body), GetGValue(body), GetBValue(body));
}

// ---- the popup menu -----------------------------------------------------------------------------------
//
// ⚠️⚠️ IT IS SHOWN FROM THE THREAD THAT OWNS THE WINDOW, AND IT IS CLOSED AGAIN IN A FRACTION OF A SECOND.
//
// The second half is a RULE rather than tidiness: an open menu is MODAL and takes the mouse, so a user who
// clicks anywhere while it is up would be clicking this probe's menu instead of their own window -- and this
// project forbids a test from touching the user's input devices.
//
// ⚠️ THE FIRST HALF IS A BUG THAT WAS FOUND BY MEASURING. The first version called TrackPopupMenu from a
// worker thread and recorded NOTHING about the result; the menu never appeared, and the ten byte-identical
// pictures it produced would have been read as "dark menus do nothing". Now it runs on the owning thread, the
// timer fires inside the menu's own modal loop (which is the only moment the menu is really on screen), the
// photograph is taken there, and EndMenu closes it. The return value and GetLastError go in the log.
volatile LONG g_menuShown = 0; // set from WM_INITMENUPOPUP: the menu is really being displayed
const char *g_menuTag = nullptr;
int g_menuTweak = 0; // 1 = round corners, 2 = Acrylic backdrop, 4 = acrylic accent, 8 = blur accent

// ---- THE MENU WINDOW ITSELF ---------------------------------------------------------------------------
//
// ⚠️ WHAT THIS IS FOR. A menu is a real top-level window of class "#32768", so every chrome call that works
// on an ordinary window can be aimed at it. Whether any of them DO anything is the question: a corner is
// drawn by DWM (so it might), while a material is drawn BEHIND the window and a menu paints its own opaque
// background over its whole rectangle (so it probably cannot be seen). Both are worth a picture, because
// "the material is hidden behind the menu's own background" is a claim about the world, and this is the
// instrument that can support it -- or refute it.
//
// ⚠️ AND IT RESETS FIRST. The menu window is REUSED between popups, so an earlier test's corner or material
// would still be on it; a measurement that inherits the previous measurement measures nothing.
// ---- A WINDOW-EVENT WATCHER, FOR THE QUESTION "DO MENUS EVEN ANNOUNCE THEMSELVES?" --------------------
//
// ⚠️ WHY THIS IS HERE. This watcher was built to answer a question about the UnifiedUI feature, which used
// SetWinEventHook(EVENT_OBJECT_SHOW) to find menus and filtered on `idObject == OBJID_WINDOW`. ⚠️ THAT FEATURE
// IS GONE (removed 2026-10-06 -- see docs/rules/features.md §3.14; its source is in
// versions/_removed_UnifiedUI_2026-10-06/). The watcher stays because the QUESTION is still answerable and
// still interesting to anyone who reopens this road. What it found: a passive run produced NO menu lines at
// all --
// neither a success nor a failure -- which means its callback never saw a menu, while the same hook did see
// ordinary windows. The suspicion is that filter: a MENU is not OBJID_WINDOW, it is very likely OBJID_MENU
// (0xFFFFFFFD). This prints what actually arrives instead of leaving it to reasoning.
//
// (OBJID_WINDOW = 0, OBJID_CLIENT = 0xFFFFFFFC, OBJID_MENU = 0xFFFFFFFD.)
void CALLBACK WatchProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
  static int n = 0;
  if (n > 60)
    return;
  wchar_t cls[64] = {0};
  if (hwnd)
    GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));
  if (wcscmp(cls, L"#32768") == 0 || idObject == (LONG)0xFFFFFFFD)
  {
    ++n;
    Log("watch: event=0x%04lX hwnd=%p idObject=0x%08lX idChild=%ld class=%ls", (unsigned long)event,
        (void *)hwnd, (unsigned long)idObject, (long)idChild, cls);
  }
}

// ---- ARE OTHER PROGRAMS' WINDOWS ALREADY ROUND? -------------------------------------------------------
//
// ⚠️ WHY THIS EXISTS. Windows 11 rounds the corners of "standard" top-level windows by itself, so giving every
// window DWMWA_WINDOW_CORNER_PREFERENCE may be a switch that changes nothing visible -- and this project has
// just removed one of those. The corner-preference ATTRIBUTE cannot answer the question: it reports what a
// program ASKED for, not what was drawn. The PIXELS can. With a rounded corner, the pixel at the window
// rectangle's own (0,0) belongs to whatever is BEHIND the window; with a square corner it is the window's own
// border. So this samples a few pixels just outside the window and a few just inside it, and compares.
//
// ⚠️ AND IT PRINTS COLOURS RATHER THAN SAVING PICTURES. These are other programs' windows and their title bars
// can carry file names; a handful of hex values says everything this needs to say, and nothing more.
//
// ⚠️ THE PROBE'S OWN WINDOW IS NOT EVIDENCE, WHICH IS THE MISTAKE THIS AVOIDS: it carries WS_EX_TOOLWINDOW
// (measured: 0x00080189), and Win11 deliberately does not round tool windows. A real program's window does not
// have that bit, so the question has to be asked of real programs.
struct CornerProbe
{
  int examined;
  int rounded;
  int square;
};

bool g_restoreOnly = false; // --restore-corners: write DEFAULT and measure nothing

BOOL CALLBACK CornerProc(HWND h, LPARAM param)
{
  CornerProbe *cp = (CornerProbe *)param;
  if (!IsWindowVisible(h) || IsIconic(h))
    return TRUE;
  if (GetWindow(h, GW_OWNER) != nullptr)
    return TRUE; // owned windows (dialogs, palettes) are not what one sees as "a program"
  if ((GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)
    return TRUE;

  RECT r;
  if (!GetWindowRect(h, &r))
    return TRUE;
  if (r.right - r.left < 200 || r.bottom - r.top < 120)
    return TRUE;
  if (r.left < 6 || r.top < 6)
    return TRUE; // its own corner is off-screen; there is nothing outside it to compare with

  HDC screen = GetDC(nullptr);
  if (!screen)
    return TRUE;
  const COLORREF bg = GetPixel(screen, r.left - 4, r.top - 4);
  const COLORREF c00 = GetPixel(screen, r.left, r.top);
  const COLORREF c22 = GetPixel(screen, r.left + 2, r.top + 2);
  const COLORREF c88 = GetPixel(screen, r.left + 8, r.top + 8);
  ReleaseDC(nullptr, screen);

  const bool round = (c00 == bg);
  const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  wchar_t cls[64] = {0};
  GetClassNameW(h, cls, (int)(sizeof(cls) / sizeof(cls[0])));
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);

  // ⚠️ ITS OWN COPY, NOT A SHARED ONE: this probe is a standalone instrument that is compiled on its own and
  // must not depend on anything inside Apex (and Apex's own ProcessName lives in a feature's private file).
  char exe[64] = {0};
  HANDLE pr = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (pr)
  {
    DWORD n = (DWORD)sizeof(exe);
    if (!QueryFullProcessImageNameA(pr, 0, exe, &n))
      exe[0] = 0;
    CloseHandle(pr);
  }
  if (char *slash = strrchr(exe, '\\'))
    memmove(exe, slash + 1, strlen(slash) + 1);
  if (!exe[0])
    _snprintf(exe, sizeof(exe), "pid %lu", (unsigned long)pid);

  if (cp->examined < 40)
    Log("corner: %-20s ex=0x%08lX %-7s outside=#%02X%02X%02X (0,0)=#%02X%02X%02X (2,2)=#%02X%02X%02X "
        "(8,8)=#%02X%02X%02X class=%ls",
        exe, (unsigned long)ex, round ? "ROUNDED" : "square", GetRValue(bg), GetGValue(bg), GetBValue(bg),
        GetRValue(c00), GetGValue(c00), GetBValue(c00), GetRValue(c22), GetGValue(c22), GetBValue(c22),
        GetRValue(c88), GetGValue(c88), GetBValue(c88), cls);

  ++cp->examined;
  if (round)
    ++cp->rounded;
  else
    ++cp->square;
  return TRUE;
}

// ⚠️⚠️ THE JUDGEMENT ABOVE (CornerProc) WAS NOT GOOD ENOUGH AND ITS FIRST RESULT WAS WRONG: it asked whether
// the corner pixel EQUALS the pixel just outside, and every real window landed 1-2 levels away -- because of
// the DWM shadow and the anti-aliased edge of the corner arc. Six windows "measured" square that way, and the
// number meant nothing.
//
// SO ASK THE QUESTION THE OTHER WAY ROUND, WHICH HAS NO TOLERANCE TO GET WRONG: change the preference to ROUND
// and see whether the pixels MOVE. If Windows was already rounding the window, asking for it again changes
// nothing; if it was not, the corner changes. That is an A/B on the same window, so it needs no threshold and
// no theory about what colour a corner "should" be.
//
// ⚠️ IT ONLY EVER ASKS FOR THE ROUNDED STATE AND PUTS THE PREFERENCE BACK, so nothing the user is looking at is
// left altered -- asking for a square corner would be visible and is not needed to answer the question.
void SampleCorner(HWND h, COLORREF *out, int side)
{
  RECT r;
  GetWindowRect(h, &r);
  HDC screen = GetDC(nullptr);
  int n = 0;
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x)
      out[n++] = GetPixel(screen, r.left + x, r.top + y);
  ReleaseDC(nullptr, screen);
}

BOOL CALLBACK CornerEffectProc(HWND h, LPARAM param)
{
  int *done = (int *)param;
  if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER) != nullptr)
    return TRUE;
  if ((GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)
    return TRUE;
  RECT r;
  if (!GetWindowRect(h, &r) || r.right - r.left < 200 || r.bottom - r.top < 120)
    return TRUE;
  if (r.left < 30 || r.top < 30)
    return TRUE;

  // ⚠️ THE RESTORE PASS EXISTS BECAUSE THE A/B PASS CAN BE CUT SHORT (it was, by the DwmFlush hang): a probe
  // that dies mid-test leaves somebody else's window carrying a preference this probe set, and cleaning that up
  // is the probe's job, not the user's. It walks EVERY window rather than stopping at three.
  if (g_restoreOnly)
  {
    const int def = DWMWCP_DEFAULT;
    DwmSetAttr(h, DWMWA_WINDOW_CORNER_PREFERENCE, &def, sizeof(def));
    ++*done;
    return TRUE;
  }
  if (*done >= 3)
    return FALSE;

  const int side = 20;
  COLORREF a[400], b[400];
  // ⚠️ NO DwmFlush HERE, AND THAT IS THE FIX FOR A REAL HANG. The first version called it before each sample,
  // and the probe then sat for minutes inside the first window: DwmFlush waits for a composition to complete,
  // and asking for that from a process which does not own the window, right after its corner preference was
  // changed, is not a wait that is guaranteed to end. A plain sleep is what this actually needs -- time for
  // DWM to redraw -- and it cannot hang.
  SampleCorner(h, a, side);
  const int round = DWMWCP_ROUND;
  DwmSetAttr(h, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
  Sleep(350);
  SampleCorner(h, b, side);
  const int def = DWMWCP_DEFAULT;
  DwmSetAttr(h, DWMWA_WINDOW_CORNER_PREFERENCE, &def, sizeof(def));
  Sleep(150);

  int diff = 0;
  for (int i = 0; i < side * side; ++i)
    if (a[i] != b[i])
      ++diff;

  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  char exe[64] = {0};
  HANDLE pr = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (pr)
  {
    DWORD n = (DWORD)sizeof(exe);
    if (!QueryFullProcessImageNameA(pr, 0, exe, &n))
      exe[0] = 0;
    CloseHandle(pr);
  }
  if (char *slash = strrchr(exe, '\\'))
    memmove(exe, slash + 1, strlen(slash) + 1);
  if (!exe[0])
    _snprintf(exe, sizeof(exe), "pid %lu", (unsigned long)pid);

  Log("corner-effect: %-20s %d of %d corner pixels changed when ROUND was requested -> %s", exe, diff,
      side * side,
      diff > 0 ? "ALREADY SQUARE (the preference does something)"
               : "already rounded (the preference changes NOTHING -- do not add it)");
  ++*done;
  return TRUE;
}

void TweakMenuNow()
{
  // ⚠️ A NEGATIVE tweak means HANDS OFF: pop the menu and change nothing about it. That mode exists so this
  // probe can be a PASSIVE SUBJECT for something else. It was built for the UnifiedUI feature running alongside
  // it -- the only way to test "a feature that watches for menus" end to end without asking the user to
  // right-click anything. ⚠️ THAT FEATURE IS GONE (removed 2026-10-06, docs/rules/features.md §3.14); the mode
  // stays because any future menu-watching experiment needs the same passive subject. If this probe reset the
  // menu's alpha here (as it does for its own measurements), it would erase exactly what the other program is
  // being tested for.
  if (g_menuTweak < 0)
    return;

  // The menu window belongs to this process (this process called TrackPopupMenu), but the desktop can hold
  // other menus; the pid check keeps this from aiming at somebody else's.
  HWND menu = nullptr;
  for (HWND w = FindWindowA("#32768", nullptr); w; w = FindWindowExA(nullptr, w, "#32768", nullptr))
  {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId())
    {
      menu = w;
      break;
    }
  }
  if (!menu)
  {
    Log("  menu: no #32768 window of this process was found -- nothing to aim at");
    return;
  }

  // Whatever the previous test left, go back to the defaults first.
  const int cornerOff = DWMWCP_DEFAULT;
  const int backdropOff = DWMSBT_NONE;
  DwmSetAttr(menu, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerOff, sizeof(cornerOff));
  DwmSetAttr(menu, DWMWA_SYSTEMBACKDROP_TYPE, &backdropOff, sizeof(backdropOff));
  SetAccent(menu, ACCENT_DISABLED, 0);
  // ⚠️ AND TAKE ANY TRANSPARENCY OFF, or the next test's picture would be showing the previous test's
  // transparency. 255 IS "fully opaque", which is what an un-layered window looks like, so this is enough to
  // make the effect go away. ⚠️ The WS_EX_LAYERED BIT ITSELF IS DELIBERATELY LEFT ALONE: clearing a bit the
  // window already had before this probe ran would be this probe changing something it did not set.
  if (GetWindowLongPtrW(menu, GWL_EXSTYLE) & WS_EX_LAYERED)
    SetLayeredWindowAttributes(menu, 0, 255, LWA_ALPHA);

  const int corner = (g_menuTweak & 1) ? DWMWCP_ROUND : DWMWCP_DEFAULT;
  const int backdrop = (g_menuTweak & 2) ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
  DwmSetAttr(menu, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  DwmSetAttr(menu, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
  if (g_menuTweak & 4)
    SetAccent(menu, ACCENT_ENABLE_ACRYLICBLURBEHIND, 0xCC202020);
  if (g_menuTweak & 8)
    SetAccent(menu, ACCENT_ENABLE_BLURBEHIND, 0xCC202020);

  Log("  menu: hwnd=%p tweak=%d -> corner=%d backdrop=%d accent=%s", (void *)menu, g_menuTweak, corner,
      backdrop, (g_menuTweak & 4) ? "acrylic" : ((g_menuTweak & 8) ? "blur" : "off"));

  // ---- TRANSPARENCY, WHICH IS NOT THE SAME THING AS A MATERIAL -----------------------------------------
  //
  // ⚠️ AND IT IS THE ONE THAT MIGHT ACTUALLY WORK ON A MENU, for the reason the material cannot: an alpha
  // applied through WS_EX_LAYERED is applied TO THE WINDOW ITSELF, not drawn behind it, so a menu painting its
  // own opaque background over its whole rectangle does not hide it. What it does hide is TEXT CONTRAST --
  // the background and the text fade together, and whatever is behind the menu shows through both. That is the
  // trade the user is asking about ("一点点就好"), and it is a picture rather than an argument.
  //
  // ⚠️ IT ALSO HAS TO BE APPLIED IN A WAY THAT SURVIVES BEING SOMEBODY ELSE'S WINDOW, which is what the
  // cross-process pass below is for: SetWindowLongPtr on GWL_EXSTYLE is permitted on another process's window
  // (only GWLP_WNDPROC is not), but "permitted" is not "works" and this is the measurement.
  if (g_menuTweak & (16 | 32 | 64))
  {
    const LONG_PTR ex = GetWindowLongPtrW(menu, GWL_EXSTYLE);
    const bool alreadyLayered = (ex & WS_EX_LAYERED) != 0;
    BYTE oldAlpha = 255;
    DWORD oldFlags = 0;
    COLORREF oldKey = 0;
    if (alreadyLayered)
      GetLayeredWindowAttributes(menu, &oldKey, &oldAlpha, &oldFlags);
    Log("  menu: GWL_EXSTYLE=0x%08lX  WS_EX_LAYERED=%d  (its own alpha=%lu flags=0x%lX)", (unsigned long)ex,
        alreadyLayered ? 1 : 0, (unsigned long)oldAlpha, (unsigned long)oldFlags);

    const int alpha = (g_menuTweak & 64) ? 210 : 242;
    if (g_menuTweak & 32)
    {
      char exe[MAX_PATH] = {0};
      GetModuleFileNameA(nullptr, exe, MAX_PATH);
      char cmd[2048], nText[32], otherLog[600];
      _snprintf(nText, sizeof(nText), "--alpha-menu=%d", alpha);
      _snprintf(otherLog, sizeof(otherLog), "--log=%s\\alpha-other.log", g_bmpDir);
      _snprintf(cmd, sizeof(cmd), "\"%s\" %s %s", exe, nText, otherLog);
      STARTUPINFOA si = {0};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi = {0};
      if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, g_bmpDir, &si, &pi))
      {
        // ⚠️⚠️ WAIT FOR IT **WHILE PUMPING THIS THREAD'S MESSAGES**, AND THE FIRST VERSION DID NOT -- which
        // made this probe report a failure that was its own fault. The other process changes the menu's
        // extended style; for that to become a really-layered window, THE OWNING THREAD has to process the
        // resulting WM_STYLECHANGED. That thread is this one, and the first version had it parked in
        // WaitForSingleObject, doing nothing -- so the other process's SetLayeredWindowAttributes came back
        // with ERROR_INVALID_WINDOW_HANDLE (1400) against a window the system did not consider layered yet.
        // A measurement must not report its own race as a property of the API.
        for (;;)
        {
          const DWORD w = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 5000, QS_ALLINPUT);
          if (w == WAIT_OBJECT_0)
            break;
          if (w == WAIT_OBJECT_0 + 1)
          {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
              TranslateMessage(&msg);
              DispatchMessageW(&msg);
            }
            continue;
          }
          Log("  menu: the other process did not finish (wait returned %lu)", (unsigned long)w);
          break;
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Log("  menu: the other process has finished (see alpha-other.log)");
      }
      else
        Log("  menu: CreateProcess failed (%lu) -- cross-process NOT measured", (unsigned long)GetLastError());
    }
    else
    {
      if (!alreadyLayered)
        SetWindowLongPtrW(menu, GWL_EXSTYLE, ex | WS_EX_LAYERED);
      SetLastError(0);
      const BOOL ok = SetLayeredWindowAttributes(menu, 0, (BYTE)alpha, LWA_ALPHA);
      Log("  menu: SetLayeredWindowAttributes(alpha=%d) in THIS process -> %d, GetLastError=%lu", alpha,
          (int)ok, (unsigned long)GetLastError());
    }
    Sleep(250);
  }

  // ⚠️ A PLAIN SLEEP, DELIBERATELY NOT A PUMP: this runs inside the menu's modal loop, which is already
  // pumping this thread's messages, and pumping them again from here would hand the menu its own input twice.
  Sleep(220);
}

void StepWithMenuOf(HWND h, const char *tag, int tweak = 0)
{
  if (!h)
    return;
  HMENU bar = GetMenu(h);
  HMENU sub = bar ? GetSubMenu(bar, 0) : nullptr;
  if (!sub)
  {
    Log("  menu: GetMenu/GetSubMenu returned nothing -- there is no menu to show");
    return;
  }

  InterlockedExchange(&g_menuShown, 0);
  g_menuTag = tag;
  g_menuTweak = tweak;
  RECT r;
  GetWindowRect(h, &r);

  // ⚠️ A TIMER, NOT A SECOND THREAD: the menu's modal loop dispatches WM_TIMER, so this is where the
  // photograph is taken -- while the menu is genuinely open -- and where EndMenu ends it.
  SetTimer(h, 1, 450, nullptr);
  SetLastError(0);
  const BOOL ok = TrackPopupMenu(sub, TPM_LEFTALIGN | TPM_TOPALIGN, r.left + 60, r.top + 70, 0, h, nullptr);
  const DWORD err = GetLastError();
  Log("  menu: TrackPopupMenu returned %d, GetLastError=%lu, WM_INITMENUPOPUP was %s", (int)ok,
      (unsigned long)err, InterlockedCompareExchange(&g_menuShown, 0, 0) ? "seen" : "NEVER SEEN");
  PumpFor(150);
}

// ---- the windows --------------------------------------------------------------------------------------
//
// ⚠️ THE CLIENT AREA IS PAINTED WITH A SYSTEM COLOUR, ON PURPOSE. That is what an old dialog does, and it is
// the one kind of client-area background that is not the program's own idea -- it comes from the single system
// table SetSysColors writes to. A program that painted RGB(240,240,240) directly could not be moved by
// anything, which is itself worth knowing.
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  case WM_INITMENUPOPUP:
    InterlockedExchange(&g_menuShown, 1); // the menu is REALLY on screen now
    return 0;
  // ⚠️ THE PHOTOGRAPH HAPPENS HERE, INSIDE THE MENU'S OWN MODAL LOOP -- the only moment the menu is genuinely
  // on screen and this thread can still run code of its own. See StepWithMenuOf.
  case WM_TIMER:
    if (wp == 1)
    {
      KillTimer(h, 1);
      TweakMenuNow(); // a no-op unless this run asked for something through g_menuTweak
      StepOf(h, g_menuTag ? g_menuTag : "popup", 0);
      EndMenu();
    }
    return 0;
  case WM_ERASEBKGND:
  case WM_PAINT:
  {
    PAINTSTRUCT ps;
    HDC dc = (msg == WM_PAINT) ? BeginPaint(h, &ps) : (HDC)wp;
    RECT rc;
    GetClientRect(h, &rc);
    // ⚠️ A FRESH BRUSH FROM THE CURRENT VALUE, NOT GetSysColorBrush. The cached brush is the documented way
    // and what a real old dialog uses, but whether that cache follows a SetSysColors is exactly one of the
    // things being asked here -- so the window is drawn from GetSysColor() directly, which cannot hide a
    // change. What a program holding a CACHED brush does is a separate question and is noted in the report.
    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    RECT m = {40, 40, 90, 90};
    HBRUSH mb = CreateSolidBrush(RGB(0xFF, 0x00, 0xFF));
    FillRect(dc, &m, mb);
    DeleteObject(mb);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    const wchar_t *line = L"client area = COLOR_3DFACE (what an old dialog paints)";
    TextOutW(dc, 40, 110, line, (int)wcslen(line));
    if (msg == WM_PAINT)
      EndPaint(h, &ps);
    return msg == WM_ERASEBKGND ? 1 : 0;
  }
  case WM_SYSCOLORCHANGE:
    InvalidateRect(h, nullptr, TRUE);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// A comctl32 v6 manifest, written out at run time so the probe can build BOTH kinds of window in one process.
// The activation context only affects windows created while it is active, which is the whole point.
void WriteManifest(const char *path)
{
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  fputs("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "  <dependentAssembly>\n"
        "    <assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
        "processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>\n"
        "  </dependentAssembly>\n"
        "</assembly>\n",
        f);
  fclose(f);
}

HANDLE g_actCtx = INVALID_HANDLE_VALUE;

bool LoadThemedContext()
{
  char path[600];
  _snprintf(path, sizeof(path), "%s\\probe.manifest", g_bmpDir);
  WriteManifest(path);

  // ⚠️ SAY WHETHER THE FILE IS ACTUALLY THERE. CreateActCtx fails with a code that says nothing about the
  // real reason, and the first run of this probe reported only "FAILED" -- which hid whether the manifest had
  // not been written, or had been written and rejected.
  long size = -1;
  if (FILE *check = fopen(path, "rb"))
  {
    fseek(check, 0, SEEK_END);
    size = ftell(check);
    fclose(check);
  }
  Log("manifest at %s: %s", path, size >= 0 ? "written" : "NOT WRITTEN");
  if (size >= 0)
    Log("  its size is %ld bytes", size);

  ACTCTXA ctx = {0};
  ctx.cbSize = sizeof(ctx);
  ctx.lpSource = path;
  SetLastError(0);
  g_actCtx = CreateActCtxA(&ctx);
  const DWORD err = GetLastError();
  Log("CreateActCtxA -> %s (GetLastError=%lu)", g_actCtx != INVALID_HANDLE_VALUE ? "ok" : "FAILED",
      (unsigned long)err);
  return g_actCtx != INVALID_HANDLE_VALUE;
}

HWND MakeWindow(HINSTANCE inst, const wchar_t *cls, const wchar_t *title, HMENU menu, int x, int y, int w,
                int h, bool themed)
{
  WNDCLASSW wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = cls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

  ULONG_PTR cookie = 0;
  bool active = false;
  if (themed && g_actCtx != INVALID_HANDLE_VALUE)
    active = ActivateActCtx(g_actCtx, &cookie) != FALSE;

  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, cls, title, WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, menu, inst,
                              nullptr);

  if (active)
    DeactivateActCtx(0, cookie);
  return hwnd;
}

// ---- the system-colour experiment, alone --------------------------------------------------------------
//
// ⚠️⚠️ THIS CHANGES SOMETHING MACHINE-WIDE, SO IT IS A MODE OF ITS OWN AND IS NOT RUN BY DEFAULT.
// SetSysColors rewrites the system colour table FOR THE SESSION: every program that paints with GetSysColor
// changes at once -- which is exactly what makes it interesting (no injection, no cooperation) and exactly
// what makes it dangerous (a probe killed mid-change leaves the user's desktop wearing the probe's colours).
// Three things keep it honest: the original is read first, it is restored on the normal path AND from an
// unhandled-exception filter, and the change lasts about a second. It is NOT PERSISTENT either -- a logoff or
// reboot restores Windows' own values regardless.
COLORREF g_sysBefore = 0;
bool g_sysChanged = false;

void RestoreSysColors()
{
  if (!g_sysChanged)
    return;
  const int idx[1] = {COLOR_3DFACE};
  SetSysColors(1, idx, &g_sysBefore);
  SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0, SMTO_ABORTIFHUNG, 500, nullptr);
  g_sysChanged = false;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS *)
{
  RestoreSysColors();
  return EXCEPTION_EXECUTE_HANDLER;
}

int RunSysColors(HWND h)
{
  g_sysBefore = GetSysColor(COLOR_3DFACE);
  Log("syscolors: COLOR_3DFACE is #%02X%02X%02X before (SetSysColors is a SESSION-wide change)",
      GetRValue(g_sysBefore), GetGValue(g_sysBefore), GetBValue(g_sysBefore));
  StepOf(h, "syscolors-before");

  const int idx[1] = {COLOR_3DFACE};
  const COLORREF dark = RGB(0x20, 0x20, 0x20);
  SetSysColors(1, idx, &dark);
  g_sysChanged = true;
  // ⚠️ READ IT BACK. "I called SetSysColors" and "the table now says dark" are two different facts, and the
  // first run of this mode reported the second one without ever checking it -- so "the window did not change"
  // could have meant either "the API did nothing" or "the window ignored it".
  const COLORREF now = GetSysColor(COLOR_3DFACE);
  Log("syscolors: SetSysColors(COLOR_3DFACE -> #202020) done; GetSysColor now says #%02X%02X%02X",
      GetRValue(now), GetGValue(now), GetBValue(now));

  // A real program is TOLD; without this the window would keep its old pixels and the measurement would say
  // "nothing happened" about a program that simply was not listening (this probe does listen -- see WndProc).
  SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0, SMTO_ABORTIFHUNG, 500, nullptr);
  StepOf(h, "syscolors-after");

  RestoreSysColors();
  Log("syscolors: restored");
  StepOf(h, "syscolors-restored");
  return 0;
}

// ---- the other process: the cross-process question ---------------------------------------------------
bool RunAsOther()
{
  char hwndText[64] = {0};
  if (!ArgValue("--allow-dark=", hwndText, sizeof(hwndText)))
    return false;

  char logPath[512] = {0};
  ArgValue("--log=", logPath, sizeof(logPath));
  if (logPath[0])
    g_logPath = logPath;

  HWND target = (HWND)(ULONG_PTR)_strtoui64(hwndText, nullptr, 10);
  LoadUxTheme();

  Log("other: target hwnd=%p", (void *)target);
  if (!g_allowDark)
  {
    Log("other: ordinal 133 is NOT AVAILABLE in uxtheme on this build");
    Log("other done");
    return true;
  }
  SetLastError(0);
  const BOOL r = g_allowDark(target, TRUE);
  Log("other: AllowDarkModeForWindow(a window from ANOTHER process, TRUE) returned %d, GetLastError=%lu",
      (int)r, (unsigned long)GetLastError());
  if (g_flushThemes)
    g_flushThemes();
  Log("other done");
  return true;
}

// `--alpha-menu=<n>`: the cross-process transparency question. Finds the menu window -- which belongs to
// ANOTHER process from here -- and tries to make it translucent.
bool RunAsAlphaOther()
{
  char nText[32] = {0};
  if (!ArgValue("--alpha-menu=", nText, sizeof(nText)))
    return false;
  const int alpha = atoi(nText);

  char logPath[512] = {0};
  ArgValue("--log=", logPath, sizeof(logPath));
  if (logPath[0])
    g_logPath = logPath;

  HWND menu = nullptr;
  for (int i = 0; i < 250 && !menu; ++i)
  {
    menu = FindWindowA("#32768", nullptr);
    if (!menu)
      Sleep(20);
  }
  if (!menu)
  {
    Log("alpha-other: no #32768 window appeared");
    Log("other done");
    return true;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId(menu, &pid);
  Log("alpha-other: menu hwnd=%p is owned by pid=%lu; THIS process is %lu", (void *)menu, (unsigned long)pid,
      (unsigned long)GetCurrentProcessId());

  const LONG_PTR ex = GetWindowLongPtrW(menu, GWL_EXSTYLE);
  Log("alpha-other: its GWL_EXSTYLE=0x%08lX, WS_EX_LAYERED=%d", (unsigned long)ex,
      (ex & WS_EX_LAYERED) ? 1 : 0);

  if (!(ex & WS_EX_LAYERED))
  {
    SetLastError(0);
    const LONG_PTR r = SetWindowLongPtrW(menu, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    Log("alpha-other: SetWindowLongPtr(GWL_EXSTYLE |= WS_EX_LAYERED) on ANOTHER process's window -> %ld, "
        "GetLastError=%lu",
        (long)r, (unsigned long)GetLastError());
  }
  SetLastError(0);
  const BOOL ok = SetLayeredWindowAttributes(menu, 0, (BYTE)alpha, LWA_ALPHA);
  Log("alpha-other: SetLayeredWindowAttributes(alpha=%d) on ANOTHER process's window -> %d, GetLastError=%lu",
      alpha, (int)ok, (unsigned long)GetLastError());
  Log("other done");
  return true;
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
  char buf[64] = {0};

  if (RunAsOther())
    return 0;
  if (RunAsAlphaOther())
    return 0;

  const bool sysMode = HasFlag("--syscolors");
  ArgValue("--dir=", g_bmpDir, sizeof(g_bmpDir));
  char logPath[512] = {0};
  ArgValue("--log=", logPath, sizeof(logPath));
  if (logPath[0])
    g_logPath = logPath;
  DeleteFileA(g_logPath);

  SetProcessDPIAware();
  // ⚠️ THE PID GOES IN THE LOG SO AN INJECTOR CAN FIND THIS PROCESS WITHOUT Start-Process -PassThru, WHICH
  // HANGS UNDER THIS HARNESS (measured: the command never returned and the subject never actually started, so
  // the payload's log was never written and the run looked like "injection failed" when nothing had been tried).
  // One line here removes that dependency completely -- and the subject has to be launched through explorer.exe
  // anyway, to keep it out of the harness's job object.
  Log("dark_probe: starting (mode: %s, pid %lu)", sysMode ? "syscolors" : "menus",
      (unsigned long)GetCurrentProcessId());

  // ⚠️ THE ORDINALS ARE GUESSED, SO THEY ARE TRIED FROM THE SAFEST FIRST (136 takes nothing, the other two
  // take an argument). A wrong guess crashes HERE, in this probe, on this probe's own windows.
  LoadUxTheme();
  Log("uxtheme: ordinal 133 (AllowDarkModeForWindow) %s", g_allowDark ? "resolved" : "NOT AVAILABLE");
  Log("uxtheme: ordinal 135 (SetPreferredAppMode)  %s", g_preferMode ? "resolved" : "NOT AVAILABLE");
  Log("uxtheme: ordinal 136 (FlushMenuThemes)      %s", g_flushThemes ? "resolved" : "NOT AVAILABLE");
  Log("uxtheme: IsAppThemed() in this process = %d", IsAppThemed() ? 1 : 0);
  Log("uxtheme: IsThemeActive() = %d", IsThemeActive() ? 1 : 0);

  const bool themedReady = LoadThemedContext();
  Log("activation context for a v6 (themed) window: %s", themedReady ? "created" : "FAILED");
  if (sysMode)
    SetUnhandledExceptionFilter(CrashFilter);

  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, 1001, L"Open");
  AppendMenuW(file, MF_STRING, 1002, L"Save");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, 1003, L"Exit");
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"File");
  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, 2001, L"About");
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L"Help");

  const int w = 620, h = 400;
  RECT work = {0, 0, 0, 0};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || work.right <= work.left)
  {
    work.left = 0;
    work.top = 0;
    work.right = GetSystemMetrics(SM_CXSCREEN);
    work.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  const int x = work.left + ((work.right - work.left) - w * 2 - 20) / 2;
  const int y = work.top + ((work.bottom - work.top) - h) / 2;

  // ⚠️ TWO WINDOWS, ONE WITHOUT THE ACTIVATION CONTEXT AND ONE WITH IT. The classic one is what every old
  // program is; the themed one is what uxtheme's dark mode is even about. The first version of this probe
  // built only the classic kind and would have reported "dark menus do not work" about a window uxtheme was
  // never going to draw in the first place.
  HWND classic = MakeWindow(inst, kClassClassic, L"classic (no v6 manifest)", menu, x, y, w, h, false);
  HWND themed = MakeWindow(inst, kClassThemed, L"themed (comctl32 v6)", menu, x + w + 20, y, w, h, true);
  if (!classic || !themed)
  {
    Log("CreateWindowEx failed (%lu)", (unsigned long)GetLastError());
    return 1;
  }
  ShowWindow(classic, SW_SHOW);
  ShowWindow(themed, SW_SHOW);
  BringToFront(themed);
  PumpFor(400);

  // ⚠️ "--wait=<ms>": do nothing for a while before doing anything. That is the window an INJECTOR needs -- the
  // subject has to be alive and idle long enough for a separate process to find it and load a payload into it,
  // which is the one thing "the subject loads the dll itself" cannot test.
  {
    char waitText[32] = {0};
    if (ArgValue("--wait=", waitText, sizeof(waitText)))
    {
      const int ms = atoi(waitText);
      Log("wait: pausing %d ms before doing anything (room for an injector to arrive)", ms);
      Sleep(ms);
    }
  }

  // ⚠️ "--load=<dll>": load a payload into THIS process before anything is created. That is how the injected
  // code path gets exercised with no injection machinery involved at all -- the effect is judged first, and
  // only a real effect makes the delivery mechanism worth building. See _diag/menudark.cpp.
  {
    char payload[MAX_PATH] = {0};
    if (ArgValue("--load=", payload, sizeof(payload)))
    {
      HMODULE m = LoadLibraryA(payload);
      Log("load: \"%s\" -> %p (GetLastError=%lu)", payload, (void *)m, (unsigned long)GetLastError());
    }
  }

  // ⚠️ THE RESTORE PASS: put every window's corner preference back to DEFAULT. It is separate from the A/B above
  // because that one can be interrupted (it was, by a hang), and because "the probe tidies up after itself"
  // should not depend on the probe having finished successfully.
  if (HasFlag("--restore-corners"))
  {
    g_restoreOnly = true;
    int done = 0;
    EnumWindows(CornerEffectProc, (LPARAM)&done);
    Log("restore-corners: DWMWCP_DEFAULT was written to %d window(s)", done);
    Log("probe done");
    return 0;
  }

  // ⚠️ THE A/B THAT ANSWERS "IS THE LAST DWM ITEM WORTH DOING": ask for rounded corners on real windows and see
  // whether anything moves. See the note on CornerEffectProc for why the first, subtler judgement was wrong.
  if (HasFlag("--corner-effect"))
  {
    Log("corner-effect: asking three real windows for ROUND and comparing before/after");
    int done = 0;
    EnumWindows(CornerEffectProc, (LPARAM)&done);
    Log("corner-effect: %d window(s) tested (the preference was put back each time)", done);
    Log("probe done");
    return 0;
  }

  // ⚠️ THE ONE QUESTION THAT DECIDES WHETHER THE LAST DWM ITEM IS WORTH DOING AT ALL: are real programs'
  // windows already rounded? See the note on CornerProc. This needs no window of its own, so it runs before
  // anything is created and touches nothing.
  if (HasFlag("--window-corners"))
  {
    Log("window-corners: sampling the top-left corner of every ordinary top-level window on this desktop");
    Log("window-corners: (a corner pixel equal to what is just OUTSIDE the window means ROUNDED)");
    CornerProbe cp = {0, 0, 0};
    EnumWindows(CornerProc, (LPARAM)&cp);
    Log("window-corners: %d examined -- %d ROUNDED, %d square", cp.examined, cp.rounded, cp.square);
    Log("probe done");
    return 0;
  }

  // ⚠️ THE PASSIVE MODE: three menus, nothing touched. Run this with Apex running and read Apex's own log --
  // see the note in TweakMenuNow.
  if (HasFlag("--just-pop"))
  {
    // ⚠️ THE COUNT IS CONFIGURABLE BECAUSE THE UNDO TEST NEEDS THE SAME RUN TO OUTLIVE TWO INJECTIONS: pop,
    // inject the darkening payload, pop again, inject the undoing payload, pop again. One process, one session,
    // three states -- which is the only way to show that the second injection really reversed the first.
    int count = 3;
    char nText[16] = {0};
    if (ArgValue("--pop-count=", nText, sizeof(nText)))
      count = atoi(nText);
    if (count < 1)
      count = 1;
    if (count > 24)
      count = 24;
    Log("just-pop: popping %d menu(s) and changing nothing about them", count);
    for (int i = 0; i < count; ++i)
    {
      char tag[32];
      _snprintf(tag, sizeof(tag), "just-pop-%d", i);
      StepWithMenuOf(classic, tag, -1);
    }
    Log("probe done");
    DestroyWindow(themed);
    DestroyWindow(classic);
    return 0;
  }

  // ⚠️ WHAT ACTUALLY ARRIVES WHEN A MENU OPENS: three menus, watched, nothing touched.
  if (HasFlag("--watch-menus"))
  {
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, WatchProc, 0, 0,
                                         WINEVENT_OUTOFCONTEXT);
    Log("watch-menus: hook=%p, popping three menus", (void *)hook);
    for (int i = 0; i < 3; ++i)
    {
      char tag[32];
      _snprintf(tag, sizeof(tag), "watch-%d", i);
      StepWithMenuOf(classic, tag, -1);
    }
    if (hook)
      UnhookWinEvent(hook);
    Log("probe done");
    DestroyWindow(themed);
    DestroyWindow(classic);
    return 0;
  }

  if (sysMode)
  {
    // The background question is about one window; whether that window is themed does not matter for a system
    // colour, so only the classic one (which is what an old dialog is) is used.
    const int rc = RunSysColors(classic);
    Log("probe done");
    DestroyWindow(themed);
    DestroyWindow(classic);
    return rc;
  }

  Log("");
  Log("--- 1. what the two kinds look like with no dark-mode call at all ---");
  StepOf(classic, "classic-baseline");
  StepOf(themed, "themed-baseline");
  StepWithMenuOf(classic, "classic-popup-baseline");
  StepWithMenuOf(themed, "themed-popup-baseline");

  // ---- 1b. THE MENU WINDOW ITSELF: an ordinary top-level window of class #32768 ----------------------
  //
  // ⚠️ THE CHEAPEST QUESTION IN THIS WHOLE FILE, AND THE ONLY ONE THAT MIGHT COST NOTHING. A corner is drawn
  // by DWM around whatever window asks for it, and this costs one call on a window another process owns --
  // which this project has already measured to be permitted. A MATERIAL is a different story (it is drawn
  // behind the window and the menu paints its own opaque background over the whole rectangle), and so is the
  // accent. All three are photographed here so the difference is on the record as pictures, not as reasoning.
  Log("");
  Log("--- 1b. the MENU WINDOW itself (each one resets it first: the window is reused between popups) ---");
  StepWithMenuOf(classic, "menu-plain", 0);
  StepWithMenuOf(classic, "menu-corner-round", 1);
  StepWithMenuOf(classic, "menu-backdrop-acrylic", 2);
  StepWithMenuOf(classic, "menu-accent-acrylicblur", 4);
  StepWithMenuOf(classic, "menu-accent-blur", 8);
  StepWithMenuOf(classic, "menu-corner-and-material", 1 | 2);

  // ---- 1c. TRANSPARENCY, the other way to let the desktop show through --------------------------------
  //
  // ⚠️ THE CROSS-PROCESS PASS IS THE ONE THAT MATTERS FOR A REAL FEATURE, and it is here rather than left for
  // later because the answer changes what is worth building: if another process cannot make somebody else's
  // menu translucent, then this whole idea is not available to a feature that does not inject.
  Log("");
  Log("--- 1c. TRANSPARENCY through WS_EX_LAYERED (alpha), in this process and from another one ---");
  StepWithMenuOf(classic, "menu-alpha242-thisproc", 16);
  StepWithMenuOf(classic, "menu-alpha242-crossproc", 32);
  StepWithMenuOf(classic, "menu-alpha210-thisproc", 64);

  // ---- 2. the PROCESS-wide lever, which a feature that does not inject can never call ----------------
  //
  // ⚠️⚠️ ALLOWDARK AND FORCEDARK ARE NOT THE SAME, AND THIS IS WORTH A PICTURE EACH. AllowDark leaves the
  // choice to the program ("you may be dark"); ForceDark overrides it ("you are dark"). The first version of
  // this probe only tried AllowDark -- and ReaperDarkMode, another project on this machine that darkens a
  // whole application's dialogs and menus, uses ForceDark. If the two differ in what they reach, that is the
  // difference between "the menu bar is dark when the program agrees" and "the menu bar is dark".
  if (g_preferMode)
  {
    Log("");
    Log("--- 2a. SetPreferredAppMode(AllowDark = 1) ---");
    Log("  returned %d", g_preferMode(1));
    if (g_flushThemes)
      g_flushThemes();
    InvalidateRect(classic, nullptr, TRUE);
    InvalidateRect(themed, nullptr, TRUE);
    StepOf(classic, "allowdark-classic");
    StepOf(themed, "allowdark-themed");
    StepWithMenuOf(themed, "allowdark-themed-popup");

    Log("");
    Log("--- 2b. SetPreferredAppMode(ForceDark = 2) ---");
    Log("  returned %d", g_preferMode(2));
    if (g_flushThemes)
      g_flushThemes();
    InvalidateRect(classic, nullptr, TRUE);
    InvalidateRect(themed, nullptr, TRUE);
    StepOf(classic, "forcedark-classic");
    StepOf(themed, "forcedark-themed");
    StepWithMenuOf(classic, "forcedark-classic-popup");
    StepWithMenuOf(themed, "forcedark-themed-popup");
  }

  // ---- 3. the WINDOW-scoped lever, called from THIS process, on BOTH kinds -----------------------------
  if (g_allowDark)
  {
    Log("");
    Log("--- 3. AllowDarkModeForWindow(own window, TRUE), both kinds ---");
    const BOOL rc1 = g_allowDark(classic, TRUE);
    const BOOL rc2 = g_allowDark(themed, TRUE);
    Log("  classic returned %d, themed returned %d", (int)rc1, (int)rc2);
    if (g_flushThemes)
      g_flushThemes();
    InvalidateRect(classic, nullptr, TRUE);
    InvalidateRect(themed, nullptr, TRUE);
    StepOf(classic, "allow-dark-classic");
    StepOf(themed, "allow-dark-themed");
  }

  // ---- 4. THE QUESTION: the same call from a DIFFERENT process ---------------------------------------
  //
  // Put this process back to "no opinion" first, so that a change can only have come from the other one.
  Log("");
  Log("--- 4. the same call from a DIFFERENT process ---");
  if (g_preferMode)
    g_preferMode(0);
  if (g_allowDark)
  {
    g_allowDark(classic, FALSE);
    g_allowDark(themed, FALSE);
  }
  if (g_flushThemes)
    g_flushThemes();
  InvalidateRect(classic, nullptr, TRUE);
  InvalidateRect(themed, nullptr, TRUE);
  StepOf(classic, "reset-classic");
  StepOf(themed, "reset-themed");

  {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    for (int pass = 0; pass < 2; ++pass)
    {
      HWND target = pass ? themed : classic;
      const char *which = pass ? "themed" : "classic";
      char cmd[2048], hwndText[64], otherLog[600];
      _snprintf(hwndText, sizeof(hwndText), "--allow-dark=%llu", (unsigned long long)(ULONG_PTR)target);
      _snprintf(otherLog, sizeof(otherLog), "--log=%s\\other-%s.log", g_bmpDir, which);
      _snprintf(cmd, sizeof(cmd), "\"%s\" %s %s", exe, hwndText, otherLog);

      STARTUPINFOA si = {0};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi = {0};
      if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, g_bmpDir, &si, &pi))
      {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
      }
      else
        Log("  CreateProcess failed (%lu)", (unsigned long)GetLastError());
      PumpFor(250);
      InvalidateRect(target, nullptr, TRUE);
      StepOf(target, pass ? "crossproc-themed" : "crossproc-classic");
    }
  }

  Log("probe done");
  DestroyWindow(themed);
  DestroyWindow(classic);
  return 0;
}
