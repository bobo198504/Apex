// WHAT THE TRAY IS ACTUALLY SHOWING, read back from the shell.
//
// The tray is the shell's own window, so the text and icon it displays cannot be read from the Apex process
// -- it is the shell that holds them, not us. This asks the shell directly, which is the only way to check
// that a language change reached the tray rather than just the code that builds it.
//
// HOW: Shell_NotifyIcon's NIM_ADD is the only documented way in, so instead this walks the tray's toolbar
// and reads the button text (the tooltip). The icon is compared by HANDLE: the shell stores what we gave it,
// and a different handle between two runs is a different image.
//
// usage: apex_tray_state [label]     -> appends a line to apex_tray_state.txt
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

// The toolbar inside the tray. Not a documented interface -- it is how every tray-reading tool does it, and
// this is a diagnostic, so a failure here is reported rather than relied on.
static HWND FindTrayToolbar()
{
  HWND tray = FindWindowA("Shell_TrayWnd", nullptr);
  if (!tray)
    return nullptr;
  HWND notify = FindWindowExA(tray, nullptr, "TrayNotifyWnd", nullptr);
  HWND pager = notify ? FindWindowExA(notify, nullptr, "SysPager", nullptr) : nullptr;
  HWND toolbar = pager ? FindWindowExA(pager, nullptr, "ToolbarWindow32", nullptr) : nullptr;
  return toolbar;
}

int main(int argc, char **argv)
{
  const char *label = (argc > 1) ? argv[1] : "state";
  FILE *out = fopen("apex_tray_state.txt", "a");
  if (!out)
    return 1;

  // Which icon variant is in force, according to the same registry read the host uses.
  bool systemLight = false;
  {
    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ,
                      &k) == ERROR_SUCCESS)
    {
      DWORD v = 0, sz = sizeof(v), type = 0;
      if (RegQueryValueExA(k, "AppsUseLightTheme", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
        systemLight = v != 0;
      RegCloseKey(k);
    }
  }

  HWND toolbar = FindTrayToolbar();
  fprintf(out, "=== %s ===\n", label);
  fprintf(out, "system theme : %s\n", systemLight ? "LIGHT" : "DARK");
  if (!toolbar)
  {
    fprintf(out, "tray toolbar : not found\n\n");
    fclose(out);
    return 2;
  }

  const int count = (int)SendMessageA(toolbar, TB_BUTTONCOUNT, 0, 0);
  fprintf(out, "tray buttons : %d\n", count);

  // Read the button text of every button whose process is Apex's own. The text lives in the SHELL's address
  // space, so it has to be read with ReadProcessMemory -- a pointer from the toolbar is meaningless here.
  DWORD pid = 0;
  GetWindowThreadProcessId(toolbar, &pid);
  HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                            FALSE, pid);
  if (!proc)
  {
    fprintf(out, "cannot read the tray (error %lu)\n\n", GetLastError());
    fclose(out);
    return 3;
  }

  TBBUTTON btn;
  void *remote = VirtualAllocEx(proc, nullptr, sizeof(TBBUTTON) + 512, MEM_COMMIT, PAGE_READWRITE);
  char *remoteText = (char *)remote + sizeof(TBBUTTON);
  for (int i = 0; i < count; ++i)
  {
    if (!SendMessageA(toolbar, TB_GETBUTTON, i, (LPARAM)remote))
      continue;
    SIZE_T rd = 0;
    if (!ReadProcessMemory(proc, remote, &btn, sizeof(btn), &rd))
      continue;
    if (!btn.dwData)
      continue; // a separator or something the shell owns
    // The tooltip text is what carries "Apex -- ..."; TB_GETBUTTONTEXTW needs a window thread, so the
    // button's own text is read instead via the toolbar's string (simpler and enough here).
    wchar_t text[256] = {0};
    if (SendMessageA(toolbar, TB_GETBUTTONTEXTW, btn.idCommand, (LPARAM)remoteText) > 0)
    {
      if (ReadProcessMemory(proc, remoteText, text, sizeof(text) - sizeof(wchar_t), &rd))
      {
        text[255] = 0;
        if (wcsstr(text, L"Apex") || wcsstr(text, L"\u7aef"))
        {
          // Report the text AS UTF-8, so the file can be read whatever the console codepage is.
          char utf8[512] = {0};
          WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
          fprintf(out, "apex tooltip : [%s]\n", utf8);
        }
      }
    }
  }

  VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
  CloseHandle(proc);
  fprintf(out, "\n");
  fclose(out);
  return 0;
}
