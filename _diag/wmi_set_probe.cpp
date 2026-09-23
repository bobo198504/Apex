// ---------------------------------------------------------------------------
// wmi_set_probe -- WHICH FORM OF THE OBJECT PATH ACTUALLY MAKES `WmiSetBrightness` SUCCEED?
//
// WHY THIS EXISTS. MediaControl reports `WmiSetBrightness FAILED (hr=0x80041008)` (WBEM_E_INVALID_PARAMETER) on
// a machine where the class, the instance and the match are all correct -- so something about the CALL is wrong
// and the error names neither what nor why. `ExecMethod` is given an object path, and there are four spellings
// of it in circulation (the bare key value, the instance's `__RELPATH`, its `__PATH`, and a hand-written
// `Class.Key="value"`), plus two plausible VARIANT types for the byte parameter. This tries them all.
//
// ⚠️ EVERY WRITE IS THE VALUE THE PANEL IS ALREADY AT. It reads `CurrentBrightness` first and asks for exactly
// that number, so a successful call changes nothing on screen -- which is what makes it safe to run this on the
// user's machine at all. It never asks for a different brightness.
//
// Build: g++ -std=c++17 -O2 -mconsole _diag/wmi_set_probe.cpp -o build/wmi_set_probe.exe -lole32 -loleaut32 -luuid -lwbemuuid
// Run:   build/wmi_set_probe.exe
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wbemidl.h>
#include <oleauto.h>

#include <cstdio>
#include <cstring>

static IWbemServices *g_svc = nullptr;

static void Wide(const wchar_t *w, char *out, int outSize)
{
  out[0] = 0;
  if (w)
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outSize - 1, nullptr, nullptr);
}

static bool OpenWmi()
{
  CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                       nullptr, EOAC_NONE, nullptr);
  IWbemLocator *loc = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                              reinterpret_cast<void **>(&loc))) ||
      !loc)
  {
    printf("no locator\n");
    return false;
  }
  BSTR ns = SysAllocString(L"ROOT\\WMI");
  const HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_svc);
  SysFreeString(ns);
  loc->Release();
  if (FAILED(hr) || !g_svc)
  {
    printf("ConnectServer hr=0x%08lx\n", (unsigned long)hr);
    return false;
  }
  CoSetProxyBlanket(g_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  return true;
}

// Read one string property of the first instance of a class, plus (optionally) one integer property.
struct Inst
{
  char keyValue[512] = {0}; // InstanceName -- the raw key value
  char relPath[512] = {0};  // __RELPATH
  char path[1024] = {0};    // __PATH
  long number = -1;         // the requested integer property, if any
  int count = 0;
};

static bool FirstInstance(const wchar_t *cls, const wchar_t *numberProp, Inst *out)
{
  wchar_t wql[256] = {0};
  _snwprintf(wql, 255, L"SELECT * FROM %s", cls);
  IEnumWbemClassObject *en = nullptr;
  BSTR lang = SysAllocString(L"WQL");
  BSTR q = SysAllocString(wql);
  const HRESULT hr = g_svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                                      &en);
  SysFreeString(lang);
  SysFreeString(q);
  if (FAILED(hr) || !en)
  {
    printf("  query %ls failed hr=0x%08lx\n", cls, (unsigned long)hr);
    return false;
  }
  for (;;)
  {
    IWbemClassObject *obj = nullptr;
    ULONG got = 0;
    if (en->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0 || !obj)
      break;
    ++out->count;
    VARIANT v;
    const wchar_t *props[3] = {L"InstanceName", L"__RELPATH", L"__PATH"};
    char *dsts[3] = {out->keyValue, out->relPath, out->path};
    const int caps[3] = {(int)sizeof(out->keyValue), (int)sizeof(out->relPath), (int)sizeof(out->path)};
    for (int i = 0; i < 3; ++i)
    {
      VariantInit(&v);
      if (obj->Get(props[i], 0, &v, nullptr, nullptr) == S_OK && v.vt == VT_BSTR)
        Wide(v.bstrVal, dsts[i], caps[i]);
      VariantClear(&v);
    }
    if (numberProp)
    {
      VariantInit(&v);
      if (obj->Get(numberProp, 0, &v, nullptr, nullptr) == S_OK)
        out->number = (long)v.lVal;
      VariantClear(&v);
    }
    obj->Release();
  }
  en->Release();
  return out->count > 0;
}

// One attempt: build the argument instance and call the method with `objectPath`.
static HRESULT TrySet(const wchar_t *objectPath, long brightness, bool byteType)
{
  IWbemClassObject *cls = nullptr;
  BSTR cn = SysAllocString(L"WmiMonitorBrightnessMethods");
  HRESULT hr = g_svc->GetObject(cn, 0, nullptr, &cls, nullptr);
  SysFreeString(cn);
  if (FAILED(hr) || !cls)
    return hr;

  IWbemClassObject *sig = nullptr;
  BSTR mn = SysAllocString(L"WmiSetBrightness");
  cls->GetMethod(mn, 0, &sig, nullptr);
  IWbemClassObject *in = nullptr;
  if (sig)
  {
    sig->SpawnInstance(0, &in);
    sig->Release();
  }
  if (!in)
  {
    cls->Release();
    SysFreeString(mn);
    return E_FAIL;
  }
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_I4;
  v.lVal = 0;
  BSTR pt = SysAllocString(L"Timeout");
  in->Put(pt, 0, &v, 0);
  SysFreeString(pt);
  VariantInit(&v);
  if (byteType)
  {
    v.vt = VT_UI1;
    v.bVal = (BYTE)brightness;
  }
  else
  {
    v.vt = VT_I4;
    v.lVal = brightness;
  }
  BSTR pb = SysAllocString(L"Brightness");
  in->Put(pb, 0, &v, 0);
  SysFreeString(pb);

  IWbemClassObject *outObj = nullptr;
  hr = g_svc->ExecMethod((LPWSTR)objectPath, mn, 0, nullptr, in, &outObj, nullptr);
  if (outObj)
    outObj->Release();
  in->Release();
  cls->Release();
  SysFreeString(mn);
  return hr;
}

int main()
{
  SetConsoleOutputCP(CP_UTF8);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!OpenWmi())
    return 1;

  Inst bright, methods;
  const bool haveBright = FirstInstance(L"WmiMonitorBrightness", L"CurrentBrightness", &bright);
  const bool haveMethods = FirstInstance(L"WmiMonitorBrightnessMethods", nullptr, &methods);
  printf("WmiMonitorBrightness: %d instance(s)\n", bright.count);
  printf("  InstanceName = %s\n", bright.keyValue);
  printf("  CurrentBrightness = %ld\n", bright.number);
  const long current = haveBright && bright.number >= 0 && bright.number <= 100 ? bright.number : 100;
  printf("WmiMonitorBrightnessMethods: %d instance(s)\n", methods.count);
  printf("  InstanceName = %s\n", methods.keyValue);
  printf("  __RELPATH    = %s\n", methods.relPath);
  printf("  __PATH       = %s\n", methods.path);
  if (!haveMethods)
  {
    printf("nothing to try\n");
    return 1;
  }

  // The four spellings, and the byte-typed variant of the winner-shaped one.
  char handWritten[1200] = {0};
  _snprintf(handWritten, sizeof(handWritten), "WmiMonitorBrightnessMethods.InstanceName=\"%s\"",
            methods.keyValue);
  wchar_t wKey[512] = {0}, wRel[512] = {0}, wPath[1024] = {0}, wHand[1200] = {0};
  MultiByteToWideChar(CP_UTF8, 0, methods.keyValue, -1, wKey, 511);
  MultiByteToWideChar(CP_UTF8, 0, methods.relPath, -1, wRel, 511);
  MultiByteToWideChar(CP_UTF8, 0, methods.path, -1, wPath, 1023);
  MultiByteToWideChar(CP_UTF8, 0, handWritten, -1, wHand, 1199);

  struct Attempt
  {
    const char *name;
    const wchar_t *path;
    bool byteType;
  };
  const Attempt attempts[] = {
      {"InstanceName value, VT_I4", wKey, false},
      {"InstanceName value, VT_UI1", wKey, true},
      {"__RELPATH, VT_I4", wRel, false},
      {"__PATH, VT_I4", wPath, false},
      {"__PATH, VT_UI1", wPath, true},
      {"Class.Key=\"value\", VT_I4", wHand, false},
  };

  printf("\n⚠️ every attempt asks for brightness %ld -- the value the panel is AT -- so the screen does not move\n",
         current);
  for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); ++i)
  {
    const HRESULT hr = TrySet(attempts[i].path, current, attempts[i].byteType);
    printf("  %-32s -> hr=0x%08lx  %s\n", attempts[i].name, (unsigned long)hr,
           SUCCEEDED(hr) ? "OK <-- the working form" : "");
  }

  g_svc->Release();
  CoUninitialize();
  printf("\ndone.\n");
  return 0;
}
