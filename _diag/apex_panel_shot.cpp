// ---------------------------------------------------------------------------
// apex_panel_shot -- take a screenshot of the settings panel's window, whatever is on screen.
//
// WHY THIS IS NOT `CopyFromScreen`. That was tried first for the caption measurement and it captured the
// wrong window: the shell's idea of "the main window" is not necessarily the one being measured, and on a
// multi-monitor desktop it is often not even the same screen (see _diag/DARK_TITLEBAR.md, where this cost a
// false conclusion). So the window is FOUND by process and class, brought to the front, and captured from
// ITS OWN rectangle with PrintWindow -- which draws the window even where it is not the topmost thing.
//
// It also PRINTS THE COLOURS IT SAMPLED, so the answer does not depend on anyone looking at a PNG: a
// background and a card colour are read from two points and reported as hex.
//
// Build: g++ -std=c++17 -O2 -mconsole -o build/apex_panel_shot.exe _diag/apex_panel_shot.cpp -luser32 -lgdi32
// Run:   apex_panel_shot.exe <exe-name> <class-name> <out.png> [x1,y1 x2,y2 ...]
//
// The sample points are CLIENT coordinates; each is printed as hex. The PNG is written with GDI+ so no image
// library is needed. Exit 0 on success.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_exe = nullptr;
static const char *g_cls = nullptr;
static HWND g_found = nullptr;

// A 32-bit BMP out of a DC's pixels. No library: a BITMAPFILEHEADER, a BITMAPINFOHEADER, the rows bottom-up.
static bool WriteBmp(const char *path, HBITMAP bmp, HDC mem, int w, int h)
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

static BOOL CALLBACK Visit(HWND h, LPARAM)
{
  if (g_found)
    return FALSE;
  if (!IsWindowVisible(h))
    return TRUE;
  char cls[128] = {0};
  GetClassNameA(h, cls, sizeof(cls) - 1);
  if (_stricmp(cls, g_cls) != 0)
    return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return TRUE;
  char path[MAX_PATH] = {0};
  DWORD n = sizeof(path);
  const BOOL ok = QueryFullProcessImageNameA(p, 0, path, &n);
  CloseHandle(p);
  if (!ok)
    return TRUE;
  const char *base = strrchr(path, '\\');
  base = base ? base + 1 : path;
  if (_stricmp(base, g_exe) == 0)
    g_found = h;
  return TRUE;
}

int main(int argc, char **argv)
{
  if (argc < 4)
  {
    printf("usage: apex_panel_shot.exe <exe-name> <class-name> <out.png> [x,y ...]\n");
    return 2;
  }
  g_exe = argv[1];
  g_cls = argv[2];
  const char *outBmp = argv[3];

  for (int i = 0; i < 40 && !g_found; ++i)
  {
    EnumWindows(Visit, 0);
    if (!g_found)
      Sleep(250);
  }
  if (!g_found)
  {
    printf("no window: %s / %s\n", g_exe, g_cls);
    return 1;
  }

  // ⚠️ THE PID IS PART OF THE ANSWER, and it was learned the hard way: a capture taken while an OLDER build
  // of the panel was still running showed the old page, and there was nothing in the output to say which
  // process had been photographed. The same exe name from a different folder is a different program.
  DWORD foundPid = 0;
  GetWindowThreadProcessId(g_found, &foundPid);
  char exePath[MAX_PATH] = {0};
  {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, foundPid);
    if (p)
    {
      DWORD n = sizeof(exePath);
      QueryFullProcessImageNameA(p, 0, exePath, &n);
      CloseHandle(p);
    }
  }
  printf("capturing %s pid=%lu from %s\n", g_cls, (unsigned long)foundPid, exePath);
  fflush(stdout);

  // Bring it up so what is captured is what the user sees. (Not required for PrintWindow, but the panel
  // redraws itself when it is activated -- and a stale back buffer is exactly what would be captured.)
  ShowWindow(g_found, SW_RESTORE);
  SetForegroundWindow(g_found);
  Sleep(400);

  RECT r;
  GetWindowRect(g_found, &r);
  const int w = r.right - r.left, h = r.bottom - r.top;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  // PW_RENDERFULLCONTENT is the flag that makes this work for a window whose content is composited by
  // another process -- without it, a WebView2 host captures as an empty rectangle.
  const BOOL painted = PrintWindow(g_found, mem, PW_RENDERFULLCONTENT);
  ReleaseDC(nullptr, screen);

  printf("window %dx%d at %ld,%ld  PrintWindow=%s\n", w, h, r.left, r.top, painted ? "ok" : "FAILED");
  if (!painted)
  {
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return 1;
  }

  // The sample points, in CLIENT coordinates, reported as hex so a colour question has a numeric answer.
  if (argc > 4)
  {
    POINT origin = {0, 0};
    ClientToScreen(g_found, &origin);
    for (int i = 4; i < argc; ++i)
    {
      int x = 0, y = 0;
      if (sscanf(argv[i], "%d,%d", &x, &y) != 2)
        continue;
      // Client -> window coordinates for the capture.
      const int wx = origin.x - r.left + x, wy = origin.y - r.top + y;
      COLORREF c = GetPixel(mem, wx, wy);
      if (c == CLR_INVALID)
      {
        printf("  (%d,%d) -> out of the capture\n", x, y);
        continue;
      }
      printf("  client(%d,%d) -> #%02X%02X%02X\n", x, y, GetRValue(c), GetGValue(c), GetBValue(c));
    }
  }

  // ---- write a BMP ----
  //
  // ⚠️ PLAIN GDI, NOT GDI+. The first version used Gdiplus::Bitmap::Save and SEGFAULTED on this toolchain
  // (MinGW), after the capture had already succeeded -- so the failure looked like "the window could not be
  // captured" when the picture was sitting in memory the whole time. A 32-bit BMP header is twenty lines and
  // has no library behind it; the caller converts to PNG if it wants one.
  const int rc = WriteBmp(outBmp, bmp, mem, w, h) ? 0 : 1;
  printf("bmp -> %s  %s\n", outBmp, rc == 0 ? "written" : "FAILED");
  printf("done\n");
  fflush(stdout);

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  return rc;
}
