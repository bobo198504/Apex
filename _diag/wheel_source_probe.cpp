// ---------------------------------------------------------------------------
// WHAT IS THIS WHEEL? -- a PASSIVE listener on WH_MOUSE_LL that prints every WM_MOUSEWHEEL exactly as the
// OS delivered it: the delta, the flags (LLMHF_INJECTED / LLMHF_LOWER_IL_INJECTED) and dwExtraInfo.
//
// WHY IT EXISTS. Apex has exactly ONE input filter, and it is the injected flag:
//
//   apex/host_win.cpp:  const bool synthetic = (ms->flags & LLMHF_INJECTED) != 0;
//                       if (!synthetic || (acceptSynthetic && !ourOutput)) { ...ask the features... }
//
// so a wheel that carries the flag is PASSED THROUGH untouched, and a wheel that does not is a candidate for
// smoothing. Nothing anywhere in Apex looks at the DEVICE (there is no touchpad test, no 0xFF515700
// promoted-pointer test -- grep for it). That means the source can answer "what does Apex filter" but NOT
// "is a touchpad wheel filtered here": the second question is about what Windows puts in `flags` for THIS
// machine's touchpad, and only the machine can answer it.
//
// This probe is that answer. It is also the reason it does NOT report a verdict about Apex: it prints facts
// (flag counts, delta shapes, extra-info signatures) and the summary says what the flags MEAN for Apex.
//
// IT DOES NOT TOUCH THE USER'S INPUT DEVICES. It installs a low-level hook and prints. It injects nothing,
// moves nothing, opens no window and (unlike _diag/apex_inject.cpp once did) never calls SetCursorPos.
//
// USAGE
//   wheel_source_probe.exe [seconds]        (default 120; the countdown is in the log)
//   -> writes wheel_source.txt NEXT TO THE EXE, and the same text to stdout.
//
// HOW TO USE IT, which is the whole trick: roll the TOUCHPAD a few times FIRST, then the MOUSE wheel a few
// times (or the other way round). The two are told apart by their delta: a notched mouse sends multiples of
// 120, a precision touchpad sends fine-grained values. The summary separates them by that shape.
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED 0x00000001
#endif
#ifndef LLMHF_LOWER_IL_INJECTED
#define LLMHF_LOWER_IL_INJECTED 0x00000002
#endif

// ---- the records the hook writes and the main thread prints ---------------------
// WHY A QUEUE AND NOT fprintf IN THE HOOK: the hook runs inside the OS input path, where a slow callback is
// punished (the OS silently unhooks a low-level hook that overruns its timeout) and where a file write can
// block. The hook therefore only stamps a record; the message loop prints it. The ring is big enough that a
// 100 ms drain cannot overrun it at any scroll rate this needs to see.
namespace {
const int kMaxRec = 8192;

struct Rec
{
  unsigned long t;
  int delta;
  unsigned long flags;
  unsigned long extra;
  long x, y;
};

Rec g_rec[kMaxRec];
volatile LONG g_head = 0;
volatile LONG g_tail = 0;
volatile LONG g_overwritten = 0;

FILE *g_out = nullptr;
unsigned long g_start = 0;

// ---- tally ---------------------------------------------------------------------
int g_total = 0, g_injected = 0, g_raw = 0, g_lowerIl = 0, g_otherFlags = 0;
int g_extraNone = 0, g_extraTouch = 0, g_extraOther = 0;
int g_multTotal = 0, g_multInjected = 0;   // notched mouse shape: |delta| is a multiple of 120
int g_fineTotal = 0, g_fineInjected = 0;   // fine-grained shape: everything else
int g_zero = 0;
int g_deltaVal[64];
int g_deltaCnt[64];
int g_deltaN = 0;

void Out(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  if (g_out)
    vfprintf(g_out, fmt, ap2);
  vfprintf(stdout, fmt, ap);
  va_end(ap2);
  va_end(ap);
  if (g_out)
    fflush(g_out);
  fflush(stdout);
}

void FlagsText(unsigned long f, char *b, size_t n)
{
  if (f == 0)
  {
    snprintf(b, n, "raw (flags=0)");
    return;
  }
  char t[96] = {0};
  if (f & LLMHF_INJECTED)
    strncat(t, "INJECTED ", sizeof(t) - strlen(t) - 1);
  if (f & LLMHF_LOWER_IL_INJECTED)
    strncat(t, "LOWER_IL_INJECTED ", sizeof(t) - strlen(t) - 1);
  if (!t[0])
    snprintf(t, sizeof(t), "other ");
  snprintf(b, n, "%s(0x%08lX)", t, f);
}

// dwExtraInfo carries the "promoted pointer input" signature on Windows: 0xFF5157NN, where the low byte is a
// cursor id and bit 7 of it marks a pen rather than a finger. A mouse wheel has 0 here. It is printed because
// it is the OTHER way a touchpad can be told apart from a mouse, and it is worth knowing which of the two
// signals this machine actually sets.
void ExtraText(unsigned long e, char *b, size_t n)
{
  if (e == 0)
  {
    snprintf(b, n, "none");
    return;
  }
  if ((e & 0xFFFFFF00ul) == 0xFF515700ul)
  {
    const int pen = (int)(e & 0x80);
    const int id = (int)(e & 0x7F);
    snprintf(b, n, "%s cursor=%d", pen ? "PROMOTED-PEN" : "PROMOTED-TOUCH", id);
    return;
  }
  snprintf(b, n, "0x%08lX", e);
}

bool ExtraIsTouch(unsigned long e) { return (e & 0xFFFFFF00ul) == 0xFF515700ul; }

LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp)
{
  if (code == HC_ACTION && wp == WM_MOUSEWHEEL)
  {
    const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lp;
    const LONG i = InterlockedIncrement(&g_head) - 1;
    if (i - g_tail >= kMaxRec)
    {
      InterlockedIncrement(&g_overwritten);
    }
    else
    {
      Rec &r = g_rec[i % kMaxRec];
      r.t = GetTickCount();
      r.delta = (int)(short)HIWORD(ms->mouseData);
      r.flags = ms->flags;
      r.extra = (unsigned long)ms->dwExtraInfo;
      r.x = ms->pt.x;
      r.y = ms->pt.y;
    }
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

void CountDelta(int d)
{
  for (int i = 0; i < g_deltaN; ++i)
    if (g_deltaVal[i] == d)
    {
      ++g_deltaCnt[i];
      return;
    }
  if (g_deltaN < 64)
  {
    g_deltaVal[g_deltaN] = d;
    g_deltaCnt[g_deltaN] = 1;
    ++g_deltaN;
  }
}

void Drain()
{
  const LONG head = InterlockedCompareExchange(&g_head, 0, 0);
  if (head > kMaxRec)
  {
    const LONG oldest = head - kMaxRec;
    if (InterlockedCompareExchange(&g_tail, 0, 0) < oldest)
      g_tail = oldest;
  }

  while (g_tail < head)
  {
    const LONG i = g_tail++;
    const Rec &r = g_rec[i % kMaxRec];

    char fl[128], ex[128];
    FlagsText(r.flags, fl, sizeof(fl));
    ExtraText(r.extra, ex, sizeof(ex));
    Out("  t=%6lums  delta=%+6d  flags=%-30s extra=%-26s at %ld,%ld\n", r.t - g_start, r.delta, fl, ex,
        r.x, r.y);

    ++g_total;
    if (r.flags == 0)
      ++g_raw;
    else if (r.flags & LLMHF_INJECTED)
      ++g_injected;
    else if (r.flags & LLMHF_LOWER_IL_INJECTED)
      ++g_lowerIl;
    else
      ++g_otherFlags;
    if (r.extra == 0)
      ++g_extraNone;
    else if (ExtraIsTouch(r.extra))
      ++g_extraTouch;
    else
      ++g_extraOther;

    const int mag = (r.delta < 0) ? -r.delta : r.delta;
    const bool injected = (r.flags & LLMHF_INJECTED) != 0;
    if (mag == 0)
      ++g_zero;
    else if (mag % 120 == 0)
    {
      ++g_multTotal;
      if (injected)
        ++g_multInjected;
    }
    else
    {
      ++g_fineTotal;
      if (injected)
        ++g_fineInjected;
    }
    CountDelta(r.delta);
  }
}

void Summary()
{
  Out("\n---- SUMMARY ----\n");
  Out("messages seen:          %d", g_total);
  if (g_overwritten)
    Out("  (WARNING: %ld records were overwritten before they were drained)\n", (long)g_overwritten);
  else
    Out("\n");
  Out("  flags == 0 (raw):     %d\n", g_raw);
  Out("  LLMHF_INJECTED:       %d\n", g_injected);
  Out("  LOWER_IL_INJECTED:    %d\n", g_lowerIl);
  Out("  other non-zero:       %d\n", g_otherFlags);
  Out("dwExtraInfo:\n");
  Out("  none:                 %d\n", g_extraNone);
  Out("  promoted touch/pen:   %d\n", g_extraTouch);
  Out("  other:                %d\n", g_extraOther);
  Out("delta shape (this is what tells a touchpad from a notched mouse):\n");
  Out("  multiples of 120:     %d messages, %d of them flagged INJECTED\n", g_multTotal, g_multInjected);
  Out("  fine-grained:         %d messages, %d of them flagged INJECTED\n", g_fineTotal, g_fineInjected);
  if (g_zero)
    Out("  zero delta:           %d\n", g_zero);
  if (g_deltaN)
  {
    Out("  deltas seen:");
    for (int i = 0; i < g_deltaN; ++i)
      Out(" %+d x%d;", g_deltaVal[i], g_deltaCnt[i]);
    Out("\n");
  }

  // WHAT THE NUMBERS MEAN FOR APEX -- stated here so the log can be read without the source at hand.
  Out("\nWHAT APEX DOES WITH THESE (apex/host_win.cpp, the only input filter there is):\n");
  Out("  a wheel WITH LLMHF_INJECTED    -> PASSED THROUGH untouched: no feature ever sees it.\n");
  Out("  a wheel WITHOUT it            -> a candidate: it reaches the feature and can be smoothed.\n");
  if (g_fineTotal > 0)
  {
    if (g_fineInjected == g_fineTotal)
      Out("  THIS LOG: every fine-grained wheel was flagged INJECTED -> a touchpad here is NOT smoothed.\n");
    else if (g_fineInjected == 0)
      Out("  THIS LOG: no fine-grained wheel was flagged INJECTED -> a touchpad here IS smoothed.\n");
    else
      Out("  THIS LOG: fine-grained wheels came in BOTH ways (%d of %d injected) -> mixed; read the lines.\n",
          g_fineInjected, g_fineTotal);
  }
  else
  {
    Out("  THIS LOG: no fine-grained wheel was seen -- either only a notched mouse was rolled, or nothing\n");
    Out("            arrived at all (a wheel Apex swallows never reaches this probe; run it with Apex off\n");
    Out("            to tell those two apart).\n");
  }
}
} // namespace

int main(int argc, char **argv)
{
  int seconds = (argc > 1) ? atoi(argv[1]) : 120;
  if (seconds <= 0)
    seconds = 120;

  char path[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  char *slash = strrchr(path, '\\');
  if (slash)
    slash[1] = 0;
  else
    path[0] = 0;
  strncat(path, "wheel_source.txt", sizeof(path) - strlen(path) - 1);
  g_out = fopen(path, "w");
  if (!g_out)
    fprintf(stdout, "WARNING: could not open %s -- printing to stdout only\n", path);

  g_start = GetTickCount();
  Out("# wheel source probe -- passive WH_MOUSE_LL listener, %d s\n", seconds);
  Out("# log: %s\n", path);
  Out("# ROLL THE TOUCHPAD a few times FIRST, then the MOUSE WHEEL a few times.\n");
  Out("# One line per WM_MOUSEWHEEL, exactly as the OS delivered it.\n\n");

  HHOOK hook = SetWindowsHookExA(WH_MOUSE_LL, Proc, GetModuleHandleA(nullptr), 0);
  if (!hook)
  {
    Out("FATAL: SetWindowsHookEx failed (error %lu)\n", GetLastError());
    if (g_out)
      fclose(g_out);
    return 2;
  }

  const unsigned long deadline = g_start + (unsigned long)seconds * 1000ul;
  for (;;)
  {
    const unsigned long now = GetTickCount();
    if ((long)(now - deadline) >= 0)
      break;
    DWORD wait = deadline - now;
    if (wait > 100)
      wait = 100;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    Drain();
  }

  Drain();
  Summary();
  UnhookWindowsHookEx(hook);
  if (g_out)
  {
    fclose(g_out);
    fprintf(stdout, "\n(also written to %s)\n", path);
  }
  return 0;
}
