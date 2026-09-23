// SEND ONE SETTING EDIT AS THE PANEL WOULD, so the persistence gate can watch the file appear.
//
// It speaks the real protocol (settings_ipc.h: IpcRequest over WM_COPYDATA) to the host window of the copy
// identified by --dir. Writing a second implementation of the message format would be a copy that can
// disagree with the panel's, so it uses the SAME header the two processes use.
//
// ⚠️ IT ONLY EVER TALKS TO THE COPY IT IS TOLD TO. The host window is found by class AND by the folder the
// owning process runs from -- the same rule the product uses (FindOwnWindow), because another copy of Apex on
// this machine registers the same window class.
//
// usage: apex_edit_probe.exe <install-dir> <key> <value>
//        apex_edit_probe.exe <install-dir> <key> @<file>    <- the value is that file's BYTES
//   e.g. apex_edit_probe.exe "D:/.../build/_persist_run" theme dark
//
// ⚠️⚠️ WHY THE `@file` FORM EXISTS, AND IT IS NOT CONVENIENCE: A VALUE CAN CARRY A FEATURE'S OWN WORDS, AND
// ARGV CANNOT. This probe is a MinGW console program, so the C runtime decodes its command line as ANSI -- a
// UTF-8 key like `MediaControl|亮度` arrives with every Chinese character replaced (measured: the host saved
// `MediaControl|???` and the ordering test could not match anything, which looked exactly like "the order is
// not applied"). The real panel never goes near argv: it posts JSON, the panel process converts that to UTF-8
// bytes and the host reads them as bytes. So when a gate needs to send what the panel would send, it writes
// those bytes to a file and points this probe at it -- byte for byte the same value.
#include "settings_ipc.h"

#include <stdio.h>
#include <string.h>

using namespace apex;

int main(int argc, char **argv)
{
  if (argc < 4)
  {
    printf("usage: apex_edit_probe.exe <install-dir> <key> <value>\n");
    return 2;
  }

  // Find the host window in THAT folder. Enumerating and comparing directories is what FindOwnWindow does for
  // this process; here the reference directory is the argument rather than this exe's own path, so the search
  // is spelled out. (The probe lives in build/, the host in build/_persist_run/ -- they are not the same
  // install, and the probe must not assume they are.)
  struct S
  {
    const char *dir;
    HWND found;
  } s = {argv[1], nullptr};

  struct Helper
  {
    static BOOL CALLBACK Proc(HWND h, LPARAM lp)
    {
      S *st = (S *)lp;
      char cls[128] = {0};
      GetClassNameA(h, cls, sizeof(cls));
      if (strcmp(cls, APEX_HOST_WND_CLASS) != 0)
        return TRUE;
      DWORD pid = 0;
      GetWindowThreadProcessId(h, &pid);
      char path[MAX_PATH] = {0};
      HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!p)
        return TRUE;
      DWORD n = MAX_PATH;
      const BOOL ok = QueryFullProcessImageNameA(p, 0, path, &n);
      CloseHandle(p);
      if (!ok)
        return TRUE;
      // Compare directories, ignoring case, with '/' and '\' treated alike (the argument comes from a shell).
      for (char *c = path; *c; ++c)
        if (*c == '\\')
          *c = '/';
      if (_strnicmp(path, st->dir, strlen(st->dir)) != 0)
        return TRUE;
      st->found = h;
      return FALSE;
    }
  };
  EnumWindows(Helper::Proc, (LPARAM)&s);
  if (!s.found)
  {
    printf("no host window found for \"%s\"\n", argv[1]);
    return 1;
  }

  // THE SAME REQUEST THE PANEL BUILDS: a "key=value" line, sent as a copy-data block. The panel's own window
  // is the sender in the real thing; this probe sends its own handle, which the host only uses to learn where
  // to post the activity notification.
  //
  // ⚠️ `@file` READS THE VALUE AS BYTES (see the note at the top of this file): a key can carry a feature's own
  // words, and this program's argv cannot.
  static char fromFile[2048];
  const char *value = argv[3];
  if (argv[3][0] == '@')
  {
    FILE *fp = fopen(argv[3] + 1, "rb");
    if (!fp)
    {
      printf("cannot read the value file: %s\n", argv[3] + 1);
      return 2;
    }
    size_t got = fread(fromFile, 1, sizeof(fromFile) - 1, fp);
    fclose(fp);
    fromFile[got] = 0;
    // A trailing newline is how a text file ends; the value is one line, so it is not part of it.
    while (got && (fromFile[got - 1] == '\n' || fromFile[got - 1] == '\r'))
      fromFile[--got] = 0;
    value = fromFile;
  }

  char text[2300] = {0};
  _snprintf(text, sizeof(text), "key=%s\nvalue=%s\n", argv[2], value);

  IpcRequest req;
  ZeroMemory(&req, sizeof(req));
  req.cmd = kIpcSetHost;
  _snprintf(req.text, sizeof(req.text), "%s", text);

  COPYDATASTRUCT cds;
  cds.dwData = kIpcSetHost;
  cds.cbData = sizeof(req);
  cds.lpData = &req;

  HWND self = GetConsoleWindow();
  const LRESULT rc = SendMessageA(s.found, WM_COPYDATA, (WPARAM)self, (LPARAM)&cds);
  printf("sent setHost %s=%s -> reply %ld\n", argv[2], argv[3], (long)rc);
  return rc == (LRESULT)kIpcOk ? 0 : 1;
}
