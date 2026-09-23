// ---------------------------------------------------------------------------
// apex_window_watch -- WATCH for windows appearing while something else runs, so "did a test window pop up
// on my screen?" can be answered by enumerating windows instead of by squinting at screenshots.
//
// WHY IT EXISTS. Running the gate suite used to put a white rectangle and a black console on the user's
// screen. Both were found by reading the code, but "we fixed it" needs evidence, and the honest evidence is
// the window list: a window that is never created cannot be shown.
//
// ⚠️ IT POLLS, IT DOES NOT DIFF TWO SAMPLES. The first version enumerated once at the start and once at the
// end -- which cannot see the thing being tested, because the receiver lives about two seconds in the
// MIDDLE of a two-minute suite. A window that appears and disappears between the two samples is invisible to
// a diff, and "appeared=0" would have been a clean-looking result that proved nothing. This samples
// repeatedly and reports every sighting.
//
// Build: gcc -O2 -o build/apex_window_watch.exe _diag/apex_window_watch.c -luser32
// Run:   apex_window_watch.exe <seconds> [name,name,...]
//
// `name` matches the START of an exe name or a class name (case-insensitive). Every sighting is printed, and
// the summary counts them, so a transient window is caught rather than missed.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAXNAMES 16
#define MAXSEEN 64

static const char *g_names[MAXNAMES];
static int g_nameCount = 0;

// A window we have already reported, so a window that stays up is counted once rather than once per poll.
typedef struct
{
  HWND h;
  char cls[128];
  char exe[128];
} Seen;

static Seen g_seen[MAXSEEN];
static int g_seenN = 0;
static int g_sightings = 0;

static BOOL NameMatches(const char *cls, const char *exe)
{
  for (int i = 0; i < g_nameCount; ++i)
  {
    const size_t n = strlen(g_names[i]);
    if (n == 0)
      continue;
    if (_strnicmp(cls, g_names[i], n) == 0 || _strnicmp(exe, g_names[i], n) == 0)
      return TRUE;
  }
  return FALSE;
}

static BOOL CALLBACK Visit(HWND h, LPARAM)
{
  if (!IsWindowVisible(h))
    return TRUE;

  char cls[128] = {0};
  GetClassNameA(h, cls, sizeof(cls) - 1);

  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  char exe[128] = {0};
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (p)
  {
    char path[MAX_PATH] = {0};
    DWORD nn = sizeof(path);
    if (QueryFullProcessImageNameA(p, 0, path, &nn))
    {
      const char *base = strrchr(path, '\\');
      _snprintf(exe, sizeof(exe), "%s", base ? base + 1 : path);
    }
    CloseHandle(p);
  }

  if (!NameMatches(cls, exe))
    return TRUE;

  for (int i = 0; i < g_seenN; ++i)
    if (g_seen[i].h == h)
      return TRUE; // already reported; it is still up, which is not news

  if (g_seenN < MAXSEEN)
  {
    g_seen[g_seenN].h = h;
    _snprintf(g_seen[g_seenN].cls, sizeof(g_seen[0].cls), "%s", cls);
    _snprintf(g_seen[g_seenN].exe, sizeof(g_seen[0].exe), "%s", exe);
    ++g_seenN;
  }
  ++g_sightings;
  printf("  SEEN      %-24s %-28s pid=%lu\n", exe, cls, (unsigned long)pid);
  fflush(stdout);
  return TRUE;
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("usage: apex_window_watch.exe <seconds> [name,name,...]\n");
    return 2;
  }
  const int seconds = atoi(argv[1]);
  if (argc >= 3)
  {
    static char buf[512];
    _snprintf(buf, sizeof(buf), "%s", argv[2]);
    for (char *tok = strtok(buf, ","); tok && g_nameCount < MAXNAMES; tok = strtok(nullptr, ","))
    {
      while (*tok == ' ')
        ++tok;
      if (*tok)
        g_names[g_nameCount++] = tok;
    }
  }
  if (g_nameCount == 0)
  {
    printf("nothing to watch for: give at least one name\n");
    return 2;
  }

  printf("watching for %d s: ", seconds);
  for (int i = 0; i < g_nameCount; ++i)
    printf("%s%s", i ? ", " : "", g_names[i]);
  printf("\n");
  fflush(stdout);

  // Poll fast enough to catch a window that lives a couple of seconds: at 200 ms a two-second window is
  // sampled about ten times, so it cannot be missed between two samples.
  const int polls = seconds * 5;
  for (int i = 0; i < polls; ++i)
  {
    EnumWindows(Visit, 0);
    Sleep(200);
  }

  printf("\ndistinct windows seen: %d\n", g_seenN);
  if (g_seenN == 0)
    printf("OK: none of the named windows appeared\n");
  else
    printf("FOUND: %d named window(s) appeared during the run\n", g_seenN);
  return g_seenN == 0 ? 0 : 1;
}
