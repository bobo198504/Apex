#ifndef APEX_WALLPAPER_WINRT_H
#define APEX_WALLPAPER_WINRT_H

// The Windows metadata has UserProfilePersonalizationSettings and StorageFile, but the MinGW SDK header shipped
// with this project omits those two generated interfaces. Only the methods Wallpaper calls are declared here;
// their IIDs and method order come from Windows.System.winmd and Windows.Storage.winmd (26100 contracts).

#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <asyncinfo.h>

namespace apex {
namespace wallpaper {
namespace winrt {

typedef unsigned char Boolean;

// Windows.Foundation.IAsyncAction. This is the return type of LockScreen::SetImageFileAsync; its method
// order is put_Completed, get_Completed, GetResults, followed by the IInspectable methods.
MIDL_INTERFACE("5a648006-843a-4da9-865b-9d26e5dfad7b")
IAsyncAction : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE put_Completed(void *handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(void **handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};

// IAsyncOperation<T> itself has these three methods above IInspectable. IAsyncInfo, where get_Status lives,
// is obtained with QueryInterface after the operation has been returned.
MIDL_INTERFACE("5e52f8ce-aced-5a42-95b4-f674dd84885e")
IStorageFileOperation : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE put_Completed(void *handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(void **handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(IUnknown **file) = 0;
};

MIDL_INTERFACE("c1d3d1a2-ae17-5a5f-b5a2-bdcc8844889a")
IBooleanOperation : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE put_Completed(void *handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(void **handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(Boolean *value) = 0;
};

MIDL_INTERFACE("5984c710-daf2-43c8-8bb4-a4d3eacfd03f")
IStorageFileStatics : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE GetFileFromPathAsync(HSTRING path,
                                                          IStorageFileOperation **operation) = 0;
};

struct IPersonalizationSettings;

MIDL_INTERFACE("91acb841-5037-454b-9883-bb772d08dd16")
IPersonalizationStatics : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE get_Current(IPersonalizationSettings **settings) = 0;
  virtual HRESULT STDMETHODCALLTYPE IsSupported(Boolean *supported) = 0;
};

MIDL_INTERFACE("8ceddab4-7998-46d5-8dd3-184f1c5f9ab9")
IPersonalizationSettings : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE TrySetLockScreenImageAsync(
      IUnknown *file, IBooleanOperation **operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE TrySetWallpaperImageAsync(
      IUnknown *file, IBooleanOperation **operation) = 0;
};

MIDL_INTERFACE("3ee9d3ad-b607-40ae-b426-7631d9821269")
ILockScreenStatics : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE get_OriginalImageFile(IUnknown **value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetImageStream(IUnknown **value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetImageFileAsync(IUnknown *file,
                                                       IAsyncAction **operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetImageStreamAsync(IUnknown *stream,
                                                        IAsyncAction **operation) = 0;
};

inline const WCHAR kStorageFileClass[] = L"Windows.Storage.StorageFile";
inline const WCHAR kPersonalizationClass[] =
    L"Windows.System.UserProfile.UserProfilePersonalizationSettings";
inline const WCHAR kLockScreenClass[] = L"Windows.System.UserProfile.LockScreen";
inline const IID kIIDStorageFileStatics = {
    0x5984c710, 0xdaf2, 0x43c8, {0x8b, 0xb4, 0xa4, 0xd3, 0xea, 0xcf, 0xd0, 0x3f}};
inline const IID kIIDPersonalizationStatics = {
    0x91acb841, 0x5037, 0x454b, {0x98, 0x83, 0xbb, 0x77, 0x2d, 0x08, 0xdd, 0x16}};
inline const IID kIIDLockScreenStatics = {
    0x3ee9d3ad, 0xb607, 0x40ae, {0xb4, 0x26, 0x76, 0x31, 0xd9, 0x82, 0x12, 0x69}};
inline const IID kIIDAsyncInfo = {
    0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// ⚠️ RESOLVED FROM combase.dll INSTEAD OF IMPORTED. The MinGW RuntimeObject/WindowsApp import libraries name
// API-set DLLs that this toolchain can link but this Windows installation refuses during process loading, before
// Wallpaper.dll can report anything. The functions have been in combase.dll since Windows 8, so resolving the
// exact exports at runtime keeps the feature's static import table to ordinary system DLLs.
struct CombaseApi
{
  using RoInitializeFn = HRESULT(WINAPI *)(RO_INIT_TYPE);
  using RoUninitializeFn = void(WINAPI *)();
  using RoGetActivationFactoryFn = HRESULT(WINAPI *)(HSTRING, REFIID, void **);
  using WindowsCreateStringFn = HRESULT(WINAPI *)(LPCWSTR, UINT32, HSTRING *);
  using WindowsDeleteStringFn = HRESULT(WINAPI *)(HSTRING);

  HMODULE module = nullptr;
  RoInitializeFn roInitialize = nullptr;
  RoUninitializeFn roUninitialize = nullptr;
  RoGetActivationFactoryFn roGetActivationFactory = nullptr;
  WindowsCreateStringFn windowsCreateString = nullptr;
  WindowsDeleteStringFn windowsDeleteString = nullptr;

  CombaseApi()
  {
    module = GetModuleHandleW(L"combase.dll");
    if (!module)
      module = LoadLibraryW(L"combase.dll");
    if (!module)
      return;
    roInitialize = reinterpret_cast<RoInitializeFn>(
        reinterpret_cast<void *>(GetProcAddress(module, "RoInitialize")));
    roUninitialize = reinterpret_cast<RoUninitializeFn>(
        reinterpret_cast<void *>(GetProcAddress(module, "RoUninitialize")));
    roGetActivationFactory = reinterpret_cast<RoGetActivationFactoryFn>(
        reinterpret_cast<void *>(GetProcAddress(module, "RoGetActivationFactory")));
    windowsCreateString = reinterpret_cast<WindowsCreateStringFn>(
        reinterpret_cast<void *>(GetProcAddress(module, "WindowsCreateString")));
    windowsDeleteString = reinterpret_cast<WindowsDeleteStringFn>(
        reinterpret_cast<void *>(GetProcAddress(module, "WindowsDeleteString")));
  }

  bool ok() const
  {
    return roInitialize && roUninitialize && roGetActivationFactory && windowsCreateString &&
           windowsDeleteString;
  }
};

inline const CombaseApi &GetCombaseApi()
{
  static const CombaseApi api;
  return api;
}

} // namespace winrt
} // namespace wallpaper
} // namespace apex

#endif // APEX_WALLPAPER_WINRT_H
