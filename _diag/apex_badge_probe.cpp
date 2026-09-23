// ---------------------------------------------------------------------------
// LOOK AT THE "WORKING" TRAY MARK -- ALL THREE STATES OF IT. This probe exists because "the mark never changed",
// "the icon failed to compose" and "the bar changed but nobody can see it" are three different problems with
// three different fixes, and NO LOG LINE CAN TELL THEM APART. (That is not a theory: the first version of this
// feature logged `load=ok` while the shell was being handed nothing it could draw, and the user's report was
// simply "the dot is not there".)
//
// ⚠️ AND THERE ARE THREE STATES NOW, NOT TWO: the user asked for the tray to say which of the two things
// KeepAwake is holding ("保持唤醒（绿色），保持唤醒+防止熄屏（红色）"), so "the bar is the other ink" is a claim
// that has to be checked per ink -- a second colour that quietly composed to the same pixels as the first would
// leave the whole distinction invisible, with every layer reporting success.
//
// WHAT IT DOES: loads the two marks out of a built apex.exe exactly as the tray does (LoadImageW by resource id,
// at the tray's own size), composes the plain, the ordinary-hold and the strong-hold version of each, draws them
// SCALED UP into a BMP the gate turns into a PNG, and counts the pixels of each ink.
//
// Build: g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I apex -o build/_badge_probe.exe _diag/apex_badge_probe.cpp -lgdi32 -luser32
// Run:   build/_badge_probe.exe build/apex/apex.exe build/_badge.bmp
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "traymark.h"

static const int kScale = 8; // each icon is drawn at 8x so the eye can judge it

static bool WriteBmp(const char *path, const void *pixels, int w, int h)
{
  BITMAPFILEHEADER fh;
  memset(&fh, 0, sizeof(fh));
  BITMAPINFOHEADER ih;
  memset(&ih, 0, sizeof(ih));
  ih.biSize = sizeof(ih);
  ih.biWidth = w;
  ih.biHeight = -h; // negative = top-down, which is how GetDIBits handed the pixels over
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  const DWORD bytes = (DWORD)w * 4 * h;
  fh.bfType = 0x4D42; // "BM"
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + bytes;
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  fwrite(&fh, sizeof(fh), 1, f);
  fwrite(&ih, sizeof(ih), 1, f);
  fwrite(pixels, 1, bytes, f);
  fclose(f);
  return true;
}

// How many pixels are (close to) one of the hold inks, inside ONE CELL of the image? The mechanical answer to
// "did the bar change, and to which colour".
//
// ⚠️ THE STRIDE IS THE WHOLE PICTURE'S WIDTH, NOT THE CELL'S. The first version walked a cell-sized run of
// consecutive pixels, which crosses the other cells and the grey background diagonally: it found zero of
// everything, on an image the eye could see was correct. (A counting bug that agrees with "nothing happened"
// is the most expensive kind -- it would have sent me looking at the composition, which was fine.)
static int CountInk(const unsigned char *px, int stride, int x0, int cell, int h, COLORREF want)
{
  const int wantR = GetRValue(want), wantG = GetGValue(want), wantB = GetBValue(want);
  int n = 0;
  for (int y = 0; y < h; ++y)
  {
    for (int x = x0; x < x0 + cell; ++x)
    {
      const unsigned char *p = px + ((size_t)y * stride + x) * 4;
      if (abs((int)p[2] - wantR) <= 16 && abs((int)p[1] - wantG) <= 16 && abs((int)p[0] - wantB) <= 16)
        ++n;
    }
  }
  return n;
}

int main(int argc, char **argv)
{
  const char *exe = (argc >= 2) ? argv[1] : "build/apex/apex.exe";
  const char *out = (argc >= 3) ? argv[2] : "build/_badge.bmp";

  // ⚠️ THE EXE IS OPENED AS A DATA MODULE: the marks are resources inside a program that may be running
  // elsewhere, and this probe must not start a second copy of it.
  HMODULE mod = LoadLibraryExA(exe, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
  if (!mod)
  {
    printf("cannot open %s (err %lu)\n", exe, GetLastError());
    return 2;
  }

  const int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
  printf("the tray's icon size on this machine: %dx%d (drawn at %dx for the image)\n", cx, cy, kScale);

  const int ids[2] = {IDI_APEX_LIGHT, IDI_APEX_DARK};
  const char *names[2] = {"light", "dark"};
  // ⚠️ THE THREE STATES, IN THE ORDER THE MARK SHOWS THEM: plain, an ordinary hold (green), the strong one
  // (red). Level 0 is the plain artwork -- there is no composition for it at all, which is the point.
  const int levels[3] = {0, 1, 2};
  const char *stateNames[3] = {"plain", "hold", "hold-hard"};

  const int cell = cx * kScale;
  const int W = cell * 6, H = cy * kScale; // two appearances x three states
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP canvas = CreateCompatibleBitmap(screen, W, H);
  if (!canvas)
  {
    printf("no bitmap\n");
    return 1;
  }
  HGDIOBJ old = SelectObject(mem, canvas);
  RECT all = {0, 0, W, H};
  // ⚠️ A MID GREY BACKGROUND for the image, because the marks fail differently: an ink that vanishes on the
  // cream plate and one that vanishes on the near-black plate are two different bugs.
  HBRUSH bg = CreateSolidBrush(RGB(128, 128, 128));
  FillRect(mem, &all, bg);
  DeleteObject(bg);

  int failures = 0;
  for (int i = 0; i < 2; ++i)
  {
    const bool light = (i == 0);
    HICON base = (HICON)LoadImageW(mod, MAKEINTRESOURCEW(ids[i]), IMAGE_ICON, cx, cy, 0);
    printf("%s mark (%s appearance): %s\n", names[i], light ? "light" : "dark",
           base ? "loaded" : "NOT LOADED -- the resource is missing");
    if (!base)
    {
      ++failures;
      continue;
    }
    for (int l = 0; l < 3; ++l)
    {
      const int slot = i * 3 + l;
      HICON drawn = base;
      HICON composed = nullptr;
      if (levels[l] > 0)
      {
        composed = apex::ComposeActiveMark(base, cx, cy, levels[l]);
        drawn = composed;
        printf("  %-10s version: %s\n", stateNames[l],
               composed ? "composed" : "COMPOSITION FAILED (the tray would keep the plain mark)");
        if (!composed)
          ++failures;
      }
      DrawIconEx(mem, cell * slot + 4, 4, drawn, cell - 8, H - 8, 0, nullptr, DI_NORMAL);
      if (composed)
        DestroyIcon(composed);
    }
    DestroyIcon(base);
  }
  SelectObject(mem, old);

  BITMAPINFO bi;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = W;
  bi.bmiHeader.biHeight = -H; // top-down read
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  unsigned char *px = (unsigned char *)malloc((size_t)W * 4 * H);
  const int got = GetDIBits(mem, canvas, 0, H, px, &bi, DIB_RGB_COLORS);
  if (got && px)
  {
    const COLORREF inks[2] = {apex::HoldInk(1), apex::HoldInk(2)};
    printf("\npixels of each ink in the 8x image (the bar is a large part of the mark):\n");
    int cellIndex = 0;
    for (int i = 0; i < 2; ++i)
    {
      for (int l = 0; l < 3; ++l, ++cellIndex)
      {
        const int green = CountInk(px, W, cell * cellIndex, cell, H, inks[0]);
        const int red = CountInk(px, W, cell * cellIndex, cell, H, inks[1]);
        printf("  %-5s %-10s green %5d   red %5d\n", names[i], stateNames[l], green, red);
        // ⚠️ THE CHECK THAT MATTERS IS THE ONE ON THE WRONG INK: a plain mark must carry NEITHER, an ordinary
        // hold the green and NOT the red, a strong hold the red and NOT the green. Without the second half, a
        // composition that painted every state the same colour would pass on the pixels of one of them.
        const int wantGreen = (l == 1), wantRed = (l == 2);
        if ((wantGreen && green < 500) || (wantRed && red < 500))
        {
          printf("    FAIL: state %s did not take its own ink\n", stateNames[l]);
          ++failures;
        }
        if ((!wantGreen && green > 50) || (!wantRed && red > 50))
        {
          printf("    FAIL: state %s carries the OTHER ink (the three states are not told apart)\n",
                 stateNames[l]);
          ++failures;
        }
      }
    }
    if (!WriteBmp(out, px, W, H))
    {
      printf("could not write %s\n", out);
      ++failures;
    }
    else
    {
      printf("\nimage: %s\n", out);
    }
    free(px);
  }
  else
  {
    printf("could not read the pixels back\n");
    ++failures;
  }

  DeleteObject(canvas);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  printf("\n%s\n", failures ? "FAILED"
                            : "OK: the marks load, and each of the three states carries its own ink and no other");
  FreeLibrary(mod);
  return failures ? 1 : 0;
}
