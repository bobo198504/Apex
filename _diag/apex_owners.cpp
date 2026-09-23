// WHO IS RUNNING APEX? -- by the one thing only Apex can claim: its own window classes.
//
// ⚠️ THIS EXISTS BECAUSE THE OBVIOUS ANSWERS ARE ALL UNSAFE.
//
// Apex takes over the wheel for the whole machine, so a second host is not a second window -- it is a second
// global hook, and the user's rule is now "进程只能有一个Apex.exe", whichever folder it came from. That means
// the gates and the deploy script have to CLEAR THE FIELD before starting a host, and clearing it wrong is
// the mistake this project has already made twice:
//
//   * `taskkill /F /IM apex.exe` kills every apex.exe on the machine, including the user's -- which is now
//     authorised, but it also kills anything ELSE that happens to be called apex.exe. The name is generic.
//   * `Get-Process | Where-Object { $_.Path -like '*...*' }` with no name filter killed a whole folder of the
//     user's portable tools (see AGENTS §五).
//
// So the identity is neither the name nor the path: it is THE WINDOW CLASS. `ApexHostWnd` and
// `ApexSettingsWnd` are registered by this program and nothing else, and a process that owns one of those
// windows is running Apex -- wherever its exe lives, and whatever it is called.
//
// ⚠️ AND THIS IS THE SAME IDENTITY THE PRODUCT USES. The host's single-instance check (main.cpp) and the two
// processes' lookups of each other (settings_ipc.h) are all window-class based, so "clear the field" and
// "refuse a second instance" agree by construction rather than by both being kept in step by hand.
//
// Prints one pid per line, duplicates removed, and nothing else. Exit status 0 even when nothing was found:
// "no Apex is running" is an answer, not a failure.
//
// usage: apex_owners.exe

#include <windows.h>
#include <stdio.h>
#include <string.h>

struct ClassPids
{
  const char *cls;
  DWORD pids[512];
  int count;
};

static BOOL CALLBACK CollectProc(HWND h, LPARAM lp)
{
  ClassPids *s = (ClassPids *)lp;
  char cls[128] = {0};
  if (!GetClassNameA(h, cls, sizeof(cls)))
    return TRUE;
  if (strcmp(cls, s->cls) != 0)
    return TRUE;

  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (!pid)
    return TRUE;
  for (int i = 0; i < s->count; i++)
    if (s->pids[i] == pid)
      return TRUE; // already have it
  if (s->count < (int)(sizeof(s->pids) / sizeof(s->pids[0])))
    s->pids[s->count++] = pid;
  return TRUE;
}

int main()
{
  // The two classes from apex/settings_ipc.h. A copy of the names lives here because this tool must be
  // runnable by a shell script without the headers -- and the modular gate reads that header to confirm the
  // two still agree, so the duplication cannot drift silently.
  static const char *classes[] = {"ApexHostWnd", "ApexSettingsWnd"};

  DWORD all[1024];
  int allCount = 0;
  for (int c = 0; c < 2; c++)
  {
    ClassPids s;
    ZeroMemory(&s, sizeof(s));
    s.cls = classes[c];
    s.count = 0;
    EnumWindows(CollectProc, (LPARAM)&s);
    for (int i = 0; i < s.count; i++)
      if (allCount < (int)(sizeof(all) / sizeof(all[0])))
        all[allCount++] = s.pids[i];
  }

  for (int i = 0; i < allCount; i++)
  {
    bool dup = false;
    for (int j = 0; j < i; j++)
      if (all[j] == all[i])
        dup = true;
    if (!dup)
      printf("%lu\n", (unsigned long)all[i]);
  }
  return 0;
}
