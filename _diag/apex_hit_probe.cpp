// WHAT DOES THE HOST SEE UNDER A POINT INSIDE THE SETTINGS PANEL?
//
// This answers one question and touches nothing: the host's target identification is `WindowFromPoint` plus
// the owning process's exe name, and the panel is a WebView2 -- a browser hosted in ANOTHER process. So the
// window at a point over the panel's page may belong to apex-settings.exe or to msedgewebview2.exe, and the
// two answers lead to different designs. GUESSING IS NOT GOOD ENOUGH HERE, because the fix (if a fix is
// needed) is a rule in the hot path.
//
// It is READ-ONLY: no SetCursorPos, no clicks, no injection. It finds the panel's window, walks the window
// at several points inside it, and prints the chain -- class names, pids and exe names, exactly as the host's
// own BareExeName would report them -- plus the GA_ROOT ancestor, which is the thing a "is this our own UI?"
// rule would have to test.
//
// usage: apex_hit_probe.exe [class-name]        (default: the settings window's class)
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void BareName(DWORD pid, char *out, int n)
{
  out[0] = 0;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
  {
    _snprintf(out, n, "(cannot open)");
    return;
  }
  char full[MAX_PATH] = {0};
  DWORD sz = sizeof(full);
  if (QueryFullProcessImageNameA(h, 0, full, &sz))
  {
    const char *base = strrchr(full, '\\');
    _snprintf(out, n, "%s", base ? base + 1 : full);
  }
  CloseHandle(h);
}

static BOOL CALLBACK EnumChild(HWND h, LPARAM lp)
{
  int *depth = (int *)lp;
  char cls[128] = {0};
  GetClassNameA(h, cls, sizeof(cls));
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  char exe[128] = {0};
  BareName(pid, exe, sizeof(exe));
  printf("      child[%d] hwnd=%p class=%-28s pid=%-6lu exe=%s\n", *depth, (void *)h, cls, pid, exe);
  ++*depth;
  return TRUE;
}

int main(int argc, char **argv)
{
  const char *cls = (argc > 1) ? argv[1] : "ApexSettingsWnd";
  HWND w = FindWindowA(cls, nullptr);
  if (!w)
  {
    printf("no window of class \"%s\" is up -- start the panel first\n", cls);
    return 1;
  }

  RECT rc = {0};
  GetWindowRect(w, &rc);
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  char exe[128] = {0};
  BareName(pid, exe, sizeof(exe));
  printf("the panel window: hwnd=%p class=%s pid=%lu exe=%s\n", (void *)w, cls, pid, exe);
  printf("  rect %ld,%ld .. %ld,%ld   visible=%s\n", rc.left, rc.top, rc.right, rc.bottom,
         IsWindowVisible(w) ? "yes" : "no");

  printf("\n  its children:\n");
  int depth = 0;
  EnumChildWindows(w, EnumChild, (LPARAM)&depth);

  // A point in the middle of the client area: that is page content, which is where a slider would be.
  RECT cr = {0};
  GetClientRect(w, &cr);
  POINT c = {(cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2};
  ClientToScreen(w, &c);

  printf("\n  what WindowFromPoint returns inside the panel:\n");
  const int dx[] = {0, -120, 120};
  const int dy[] = {0, -80, 80};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
    {
      POINT p = {c.x + dx[i], c.y + dy[j]};
      HWND hit = WindowFromPoint(p);
      char hcls[128] = {0};
      if (hit)
        GetClassNameA(hit, hcls, sizeof(hcls));
      DWORD hpid = 0;
      if (hit)
        GetWindowThreadProcessId(hit, &hpid);
      char hexe[128] = {0};
      if (hit)
        BareName(hpid, hexe, sizeof(hexe));

      HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
      char rootcls[128] = {0};
      if (root)
        GetClassNameA(root, rootcls, sizeof(rootcls));

      const bool same = (root == w);
      printf("    (%ld,%ld) hit=%-28s exe=%-24s GA_ROOT=%-28s %s\n", p.x, p.y, hit ? hcls : "(null)",
             hexe, root ? rootcls : "(null)", same ? "<-- IS THE PANEL WINDOW" : "");
    }

  printf("\n  VERDICT INPUT: ");
  {
    // The rule a design would use: does the window under the point resolve to the panel as its root?
    POINT p = c;
    HWND hit = WindowFromPoint(p);
    HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    if (root == w)
      printf("a wheel over the page resolves to the PANEL window -- one ancestor test is enough\n");
    else
      printf("a wheel over the page does NOT resolve to the panel window (root=%s) -- the exe name or the "
             "process is what identifies it\n",
             root ? "another window" : "none");
  }
  return 0;
}
