// PNG -> multi-size ICO, for the tray and window icons.
//
// WHY A TOOL AND NOT A CHECKED-IN .ico: the source art is the two 900x900 PNGs the project was given,
// and they are the thing that gets edited. Regenerating the .ico from them has to be a repeatable step
// rather than a hand export somebody has to remember to redo, so it is a build-time tool in the same
// toolchain as everything else.
//
// The sizes written are the ones Windows actually asks for: the tray at 16 and 20/24 (DPI scaling), the
// title bar and taskbar at 32, alt-tab and the shell at 48/64, and one large size for the file's own
// icon. Windows picks the closest; supplying them all is what keeps a small icon crisp instead of a
// downscaled blur.
//
// A .ico is a directory plus one packed DIB per size, and the DIB is the old 32bpp-by-hand format:
// a BITMAPINFOHEADER whose height is DOUBLE the real height (colour rows, then a 1bpp AND mask),
// colour rows stored bottom-up in BGRA, and an AND mask that must exist even when alpha already
// describes the shape (all-zero mask = "use the alpha").
//
// g++ -std=c++17 -O2 tools/png2ico.cpp -o build/png2ico.exe -lgdiplus
// usage: png2ico <in.png> <out.ico> <size> [size...]
#include <windows.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace Gdiplus;

static bool WriteIco(const wchar_t *inPath, const wchar_t *outPath, const std::vector<int> &sizes)
{
  Bitmap src(inPath, FALSE);
  if (src.GetLastStatus() != Ok)
  {
    wprintf(L"cannot load %ls\n", inPath);
    return false;
  }

  struct Img
  {
    int w, h;
    std::vector<unsigned char> dib;
  };
  std::vector<Img> images;

  for (int s : sizes)
  {
    Bitmap dst(s, s, PixelFormat32bppARGB);
    Graphics g(&dst);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));
    g.DrawImage(&src, Rect(0, 0, s, s), 0, 0, (int)src.GetWidth(), (int)src.GetHeight(), UnitPixel);
    g.Flush();

    // Read the pixels out as top-down BGRA, which is what LockBits hands over for a 32bpp ARGB bitmap.
    BitmapData bd;
    Rect rc(0, 0, s, s);
    if (dst.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
    {
      wprintf(L"cannot read %dx%d\n", s, s);
      return false;
    }

    Img im;
    im.w = im.h = s;
    const int andStride = ((s + 31) / 32) * 4; // 1bpp rows, 4-byte aligned
    const int xorSize = s * s * 4;
    const int andSize = andStride * s;
    im.dib.resize(40 + xorSize + andSize, 0);

    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)im.dib.data();
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = s;
    bi->biHeight = s * 2; // colour rows + mask rows
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = xorSize + andSize;

    unsigned char *xorDst = im.dib.data() + 40;
    const unsigned char *row = (const unsigned char *)bd.Scan0;
    for (int y = 0; y < s; ++y)
    {
      // The DIB is bottom-up, so the last source row is written first.
      const unsigned char *srcRow = row + (size_t)(s - 1 - y) * bd.Stride;
      memcpy(xorDst + (size_t)y * s * 4, srcRow, (size_t)s * 4);
    }
    dst.UnlockBits(&bd);
    // The AND mask is left all zero: with a 32bpp image the alpha channel defines the shape, and a
    // non-zero mask would only carve extra holes out of it.
    images.push_back(std::move(im));
  }

  FILE *f = _wfopen(outPath, L"wb");
  if (!f)
  {
    wprintf(L"cannot write %ls\n", outPath);
    return false;
  }

  // ICONDIR
  const unsigned short count = (unsigned short)images.size();
  unsigned short hdr[3] = {0, 1, count};
  fwrite(hdr, sizeof(unsigned short), 3, f);

  DWORD offset = 6 + 16 * (DWORD)count;
  for (const Img &im : images)
  {
    unsigned char e[16];
    e[0] = (unsigned char)(im.w >= 256 ? 0 : im.w); // 256 is encoded as 0
    e[1] = (unsigned char)(im.h >= 256 ? 0 : im.h);
    e[2] = 0; // palette colours
    e[3] = 0; // reserved
    unsigned short planes = 1, bits = 32;
    memcpy(e + 4, &planes, 2);
    memcpy(e + 6, &bits, 2);
    const DWORD sz = (DWORD)im.dib.size();
    memcpy(e + 8, &sz, 4);
    memcpy(e + 12, &offset, 4);
    fwrite(e, 1, 16, f);
    offset += sz;
  }
  for (const Img &im : images)
    fwrite(im.dib.data(), 1, im.dib.size(), f);
  fclose(f);

  wprintf(L"wrote %ls: %d sizes (", outPath, (int)images.size());
  for (size_t i = 0; i < sizes.size(); ++i)
    wprintf(L"%s%d", i ? L", " : L"", sizes[i]);
  wprintf(L")\n");
  return true;
}

int wmain(int argc, wchar_t **argv)
{
  if (argc < 4)
  {
    wprintf(L"usage: png2ico <in.png> <out.ico> <size> [size...]\n");
    return 2;
  }

  GdiplusStartupInput si;
  ULONG_PTR tok = 0;
  if (GdiplusStartup(&tok, &si, nullptr) != Ok)
  {
    wprintf(L"GDI+ could not start\n");
    return 1;
  }

  std::vector<int> sizes;
  for (int i = 3; i < argc; ++i)
    sizes.push_back(_wtoi(argv[i]));

  const bool ok = WriteIco(argv[1], argv[2], sizes);
  GdiplusShutdown(tok);
  return ok ? 0 : 1;
}
