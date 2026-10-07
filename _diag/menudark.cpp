// ⚠️ THE SMALLEST POSSIBLE PAYLOAD, FOR ONE QUESTION: does SetPreferredAppMode(ForceDark) actually darken an
// old program's popup menus?
//
// WHY IT IS A DLL RATHER THAN A CALL IN THE PROBE: the point is to find out whether the answer survives being
// delivered FROM OUTSIDE the process, which is what a real feature would have to do.
//
// ⚠️⚠️ AND IT DELIBERATELY USES NO C RUNTIME AT ALL -- NOT EVEN ONE CALL. This is not minimalism for its own
// sake, it is the difference between loading and not loading in somebody else's process.
//
// What happened without this rule: the payload was written against stdio, and the 32-bit toolchain available
// here is a UCRT build, so the DLL came out depending on api-ms-win-crt-stdio/-string/-private and friends.
// In a process that has the UCRT (anything modern) that is fine. In a process that does not -- ReNamer, for
// instance -- LoadLibraryA inside that process FAILED with a null return, and the injection looked like it had
// been refused when in fact it had been delivered and rejected by the loader. One line in the target's module
// list is the difference.
//
// So every string, every integer and every log line below is written by hand against kernel32 and user32 only.
// The dependency list this produces is the point of the file: KERNEL32, USER32, and nothing else.
//
// ⚠️ NO GLOBAL OBJECTS EITHER, for the same reason: a C++ static initialiser would drag the CRT back in.
//
// ⚠️ ONE SOURCE, TWO PAYLOADS. "Make menus dark" and "put them back" must be the same code path with one number
// changed, because the failure this guards against is asymmetric: if the undo payload differs from the apply
// payload in any way that matters, the thing being tested is the difference rather than the undo.
//
//   g++ -std=c++17 -O2 -shared -DAPEX_PREFERRED_MODE=2 -o menudark.dll   menudark.cpp
//   g++ -std=c++17 -O2 -shared -DAPEX_PREFERRED_MODE=0 -o menuundark.dll menudark.cpp
//
// 0 = Default, 1 = AllowDark, 2 = ForceDark, 3 = ForceLight.
#ifndef APEX_PREFERRED_MODE
#define APEX_PREFERRED_MODE 2
#endif

#include <windows.h>

typedef int(WINAPI *SetPreferredAppModeFn)(int);
typedef void(WINAPI *FlushMenuThemesFn)(void);

static char g_log[MAX_PATH] = {0};

// ---- the handful of primitives this needs, written out so the CRT is never linked -------------------------

static int StrLen(const char *s)
{
  int n = 0;
  while (s[n])
    ++n;
  return n;
}

static char *AppendStr(char *p, const char *s)
{
  while (*s)
    *p++ = *s++;
  return p;
}

static char *AppendInt(char *p, int v)
{
  char tmp[16];
  int n = 0;
  unsigned int u;
  if (v < 0)
  {
    *p++ = '-';
    u = (unsigned int)(-v);
  }
  else
    u = (unsigned int)v;
  do
  {
    tmp[n++] = (char)('0' + (u % 10));
    u /= 10;
  } while (u);
  while (n)
    *p++ = tmp[--n];
  return p;
}

static char *AppendHex(char *p, unsigned long v)
{
  const char *digits = "0123456789ABCDEF";
  for (int shift = 28; shift >= 0; shift -= 4)
    *p++ = digits[(v >> shift) & 0xF];
  return p;
}

// ⚠️ THE LOG IS OPENED, WRITTEN AND CLOSED EVERY TIME, WITH NO BUFFERING TO FLUSH. The case this log exists for
// is a process that gets killed rather than one that exits cleanly, so anything buffered is anything lost.
static void Note(const char *text)
{
  if (!g_log[0])
    return;
  HANDLE f = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return;
  DWORD wrote = 0;
  WriteFile(f, text, (DWORD)StrLen(text), &wrote, nullptr);
  WriteFile(f, "\r\n", 2, &wrote, nullptr);
  CloseHandle(f);
}

// ⚠️ THIS EXPORT EXISTS ONLY SO SetWindowsHookEx HAS SOMETHING TO POINT AT, AND IT DELIBERATELY DOES NOTHING.
// The work happens in DllMain, the moment the system loads this DLL into the target -- which is why the hook
// route suits this payload: the callback never has to run for the job to be done.
extern "C" __declspec(dllexport) LRESULT CALLBACK ApexHookProc(int code, WPARAM wp, LPARAM lp)
{
  return CallNextHookEx(nullptr, code, wp, lp);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;
  DisableThreadLibraryCalls(inst);

  char self[MAX_PATH] = {0};
  GetModuleFileNameA(inst, self, MAX_PATH);
  // keep only the directory
  for (int i = StrLen(self) - 1; i >= 0; --i)
  {
    if (self[i] == '\\')
    {
      self[i + 1] = 0;
      break;
    }
  }
  char *p = AppendStr(g_log, self);
  AppendStr(p, "menudark.log");

  char line[512];
  p = AppendStr(line, "menudark: attached to pid ");
  p = AppendInt(p, (int)GetCurrentProcessId());
  p = AppendStr(p, " in ");
  p = AppendStr(p, self);
  *p = 0;
  Note(line);

  // ⚠️ uxtheme IS ALREADY LOADED IN ALMOST ANY PROCESS WITH A WINDOW, and looking it up rather than loading it
  // keeps this payload from adding a dependency that was not already there.
  HMODULE ux = GetModuleHandleA("uxtheme.dll");
  if (!ux)
  {
    Note("menudark: uxtheme.dll is not loaded in this process -- nothing done");
    return TRUE;
  }

  SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
  FlushMenuThemesFn flush = (FlushMenuThemesFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));

  p = AppendStr(line, "menudark: SetPreferredAppMode=0x");
  p = AppendHex(p, (unsigned long)(ULONG_PTR)setMode);
  p = AppendStr(p, " FlushMenuThemes=0x");
  p = AppendHex(p, (unsigned long)(ULONG_PTR)flush);
  *p = 0;
  Note(line);

  if (setMode)
  {
    const int prev = setMode(APEX_PREFERRED_MODE);
    p = AppendStr(line, "menudark: SetPreferredAppMode(");
    p = AppendInt(p, (int)APEX_PREFERRED_MODE);
    p = AppendStr(p, ") -> the previous mode was ");
    p = AppendInt(p, prev);
    *p = 0;
    Note(line);
  }
  if (flush)
    flush();

  return TRUE;
}
