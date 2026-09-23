// A MINIMAL WHEEL RECEIVER -- a window that records every wheel message it is given.
//
// WHY THIS EXISTS INSTEAD OF USING NOTEPAD (or any real program): the question being asked is "what
// actually reaches the program under the cursor", and a real program cannot answer it -- Notepad scrolls,
// and reading back how far it scrolled is a proxy with its own errors. This window does nothing but count
// and sum what arrives, so the number it reports IS the answer.
//
// ⚠️ IT USED TO DISTURB THE USER'S WORK, and the sentence that used to stand here claimed it could not.
// Three complaints, all from this file, and the fixes are marked in the code below:
//
//   * A WHITE RECTANGLE in the top-left of the screen     -> it does not paint (layered, alpha 1)
//   * A BLACK CONSOLE appearing beside it                 -> built with -mwindows; see the build line in
//     test/check_apex_endtoend.sh (the report goes to a file, which is what the gate reads anyway, so the
//     console was never carrying information to anyone)
//   * THE MOUSE CURSOR BEING PULLED AWAY AND NOT PUT BACK -> IT NEVER MOVES THE CURSOR NOW. The window is
//     placed AROUND the cursor instead: wherever the pointer already is, the receiver covers it.
//
// The third one was worse than it looks. Moving the cursor made the test depend on the user not touching the
// mouse for about a second -- and when they did, the injected wheels went to whatever window they had moved
// to, the receiver counted zero, and the gate reported a failure of the CODE UNDER TEST. That happened, for
// real: a run with the user working at the machine showed one of three notches reaching the receiver and the
// rest delivered to other windows. So this is not only a comfort fix; it removes a way for the gate to lie.
//
// usage: apex_receiver [seconds] [x y]
//   Prints a running total and writes it to apex_receiver.txt, then exits.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int g_wheels = 0;
static int g_sum = 0;
static FILE *g_out = nullptr;
static const char *kWndClass = "ApexReceiverWnd";

static void Report(const char *why)
{
  if (!g_out)
    return;
  fprintf(g_out, "%s: %d wheel messages, %+d deltas total\n", why, g_wheels, g_sum);
  fflush(g_out);
}

// When the last wheel arrived, so the run can end once the burst is over instead of after a fixed ten
// seconds. The window is invisible but it is still UNDER THE USER'S CURSOR, so it swallows their clicks for
// as long as it lives -- keeping that to ~1 second instead of 10 is the difference between a test they never
// notice and one they have to wait out. (The old fixed timeout is kept as a hard cap.)
static DWORD g_lastWheelTick = 0;

// ---------------------------------------------------------------------------
// THE ARRIVAL TRACE: every wheel message with the moment it arrived, so the DELIVERED RATE can be measured
// rather than inferred.
//
// WHY IT EXISTS: this program's throughput IS these timestamps -- "how much is handed over per frame" is the
// speed the user sees, so a jump is a step in this series and nothing else. The earlier probes measured a
// MODEL REPLICA instead, and the model is not where the timing lives: between its output and this window sit
// the host's engine period, its per-frame carry, its injection queue, and the OS's dispatch. If the model is
// smooth and the delivered stream is not, only a trace from here can say so.
//
// ⚠️ QueryPerformanceCounter, NOT GetTickCount. The latter's granularity on this machine was MEASURED at
// 15 ms (a busy-spin loop saw 20 changes in 300 ms), which is coarser than the frame period it would be
// measuring: every interval would quantise to 0 or 15 and a 4 ms frame could not be seen at all. That same
// coarse clock is what the FEATURE reads for its message gaps, which is why this had to be checked rather
// than assumed -- see the note where the feature reads it.
//
// Off unless APEX_RECV_TRACE names a file, so a normal gate run writes its one summary line and nothing else.
// ---------------------------------------------------------------------------
static FILE *g_trace = nullptr;
static LARGE_INTEGER g_qpf = {{0}};
static LARGE_INTEGER g_qp0 = {{0}};

static void TraceOpen()
{
  const char *path = getenv("APEX_RECV_TRACE");
  if (!path || !*path)
    return;
  g_trace = fopen(path, "wb");
  if (!g_trace)
    return;
  QueryPerformanceFrequency(&g_qpf);
  QueryPerformanceCounter(&g_qp0);
  fprintf(g_trace, "# t_us delta\n");
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_MOUSEWHEEL)
  {
    const int delta = (int)(short)HIWORD(w);
    ++g_wheels;
    g_sum += delta;
    g_lastWheelTick = GetTickCount();
    if (g_trace && g_qpf.QuadPart)
    {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      const double us = 1e6 * (double)(now.QuadPart - g_qp0.QuadPart) / (double)g_qpf.QuadPart;
      // ⚠️ NO fflush HERE. It was there first, and it makes the instrument part of the measurement: a flush
      // is a write to disk, on the thread that is receiving the stream being timed. A stall in this window
      // pump delays the delivery of the very messages being measured. The file is closed on the way out,
      // which flushes whatever is buffered.
      fprintf(g_trace, "%.1f %d\n", us, delta);
    }
    return 0; // handled: pretend we scrolled
  }
  if (m == WM_DESTROY)
  {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

// Is the receiver the window the cursor is over RIGHT NOW? The measurement is only meaningful while it is,
// and this is asked at BOTH ends of the run -- see the report written at exit.
static bool CursorIsOverUs(HWND h, char *clsOut, int clsSize)
{
  POINT p;
  GetCursorPos(&p);
  HWND under = WindowFromPoint(p);
  if (clsOut && clsSize > 0)
  {
    clsOut[0] = 0;
    GetClassNameA(under, clsOut, clsSize - 1);
  }
  return (under == h) || (under && IsChild(h, under));
}

// WHETHER THIS RUN IS TRUSTWORTHY, checked while the window still exists.
//
// ⚠️ IT HAS TO RUN BEFORE DestroyWindow, and getting that wrong is easy: the check was first written after
// the message loop, by which point the window is gone and WindowFromPoint can only ever answer "something
// else". Every run then declared itself invalid, which is a test that cannot pass rather than a test that can
// fail -- the opposite failure, and just as useless.
static bool g_oursAtStart = false;

static void FinishRun(HWND h, const char *why)
{
  char cls[128] = {0};
  const bool oursEnd = CursorIsOverUs(h, cls, sizeof(cls));
  if (g_out)
  {
    fprintf(g_out, "cursor at end: %s\n",
            oursEnd ? "still over this receiver" : "MOVED AWAY (the run below is not about this window)");
    // The setter is `check_apex_endtoend.sh`: it refuses to read the counts unless this says yes.
    fprintf(g_out, "valid: %s\n", (g_oursAtStart && oursEnd) ? "yes" : "no");
    fflush(g_out);
  }
  Report(why);
  DestroyWindow(h);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int)
{
  int seconds = 30;
  int px = -1, py = -1; // the point to place under the cursor, if given
  if (cmd && *cmd)
  {
    char buf[256] = {0};
    _snprintf(buf, sizeof(buf), "%s", cmd);
    char *tok = strtok(buf, " ");
    if (tok)
      seconds = atoi(tok);
    tok = strtok(nullptr, " ");
    if (tok)
      px = atoi(tok);
    tok = strtok(nullptr, " ");
    if (tok)
      py = atoi(tok);
  }

  g_out = fopen("apex_receiver.txt", "w");
  TraceOpen(); // before the window exists, so the first message is already timed
  Report("start");

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kWndClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // NO BACKGROUND BRUSH. It used to be WHITE_BRUSH, which is what put a white rectangle in the corner of the
  // screen for the length of every end-to-end run. (NULL is not "no paint" on its own -- see the WS_EX_LAYERED
  // note below, which is what actually makes the window invisible while keeping it hittable.)
  wc.hbrBackground = nullptr;
  RegisterClassA(&wc);

  // A plain, small window. WS_POPUP would not take focus and some input paths treat it differently, so
  // this is an ordinary overlapped window like any program's.
  //
  // ⚠️ TOPMOST, AND THIS IS THE FIX THAT ACTUALLY WORKED.
  //
  // Three attempts came before it, and the log of each is worth keeping because it is a trap:
  //
  //   1. A fixed point (300,220) with the window where CreateWindow put it: something on the desktop covered
  //      it and the injected wheels went to THAT window. The test then read zero for Apex and for the
  //      baseline alike -- a believable looking regression caused entirely by the test.
  //   2. Bringing it to the front (SetForegroundWindow/BringWindowToTop/AllowSetForegroundWindow): all
  //      silently ignored. A process started detached has no right to the foreground, and this one has no
  //      user to grant it.
  //   3. Moving the window to a corner of the monitor: still covered, because what covered it was a
  //      maximised window and this machine's desktop is 3840x2160.
  //
  // A test window is exactly the case where WS_EX_TOPMOST is correct: it exists to receive input and nothing
  // else, it is small, it is on screen for ten seconds, and it must not depend on what else the desktop is
  // showing. The alternative -- requiring the user to clear their desktop before running a gate -- is not a
  // test, it is a ceremony.
  //
  // ⚠️ AND TOPMOST IS NOT THE SAME AS VISIBLE, which is what took a user's complaint to realise: a window
  // can be hit-tested perfectly while showing nothing. WS_EX_LAYERED + SetLayeredWindowAttributes keeps it a
  // real window for WindowFromPoint -- which the measurement rests on -- and paints nothing, so the run stops
  // being a white rectangle the user has to work around.
  //
  // ⚠️ THE ALPHA IS 1, NOT 0, AND THAT IS A CORRECTNESS REQUIREMENT. A layered window's mouse hit-testing
  // follows its transparency, and areas at alpha ZERO LET MOUSE MESSAGES THROUGH to whatever is underneath.
  // Measured: with alpha 0 the receiver was invisible AND the cursor's window came back as SysListView32 --
  // "the point is covered". The wheels would have gone to the desktop, and this gate would have reported a
  // believable failure of the code under test. Alpha 1 is not zero, so the window stays hittable, and at
  // 1/255 it is indistinguishable from nothing.
  //
  // AND IT STILL REPORTS WHERE IT IS AND WHAT IS UNDER THE CURSOR, because "topmost" is a request and not a
  // guarantee; if the measurement is ever surprising, the log has to say so rather than leave a zero to be
  // interpreted.
  // The window's size. It is only ever seen by the hit-test and by whatever the user happens to be doing
  // underneath it, so it is sized to be comfortably larger than any wheel target and nothing more.
  const int ww = 520, wh = 360;

  // ⚠️ THE WINDOW IS PLACED AROUND THE CURSOR, NOT THE CURSOR AROUND THE WINDOW. This is the fix for the
  // complaint that mattered most: the old version called SetCursorPos to move the pointer onto a fixed point,
  // which pulled the mouse out from under whatever the user was doing and -- when they moved it back, or
  // simply kept working -- sent the injected wheels to THEIR window while this one counted zero.
  //
  // So the pointer is READ and never written, and the receiver is centred on it. The user can keep working:
  // their cursor does not move, and whatever is under it for the next ten seconds is an invisible,
  // deliberately-hittable window. (px,py still override, for the manual case.)
  POINT target;
  int wx, wy;
  if (px >= 0 && py >= 0)
  {
    target.x = px;
    target.y = py;
    wx = px - ww / 2;
    wy = py - wh / 2;
  }
  else
  {
    GetCursorPos(&target);
    wx = target.x - ww / 2;
    wy = target.y - wh / 2;
  }

  // Clamped so the window is fully on the virtual desktop: a cursor near a screen edge must not put half the
  // window off-screen (the topmost window would then be hittable only where it exists, and the target point
  // could land outside it).
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (wx < vx) wx = vx;
  if (wy < vy) wy = vy;
  if (vw > 0 && wx + ww > vx + vw) wx = vx + vw - ww;
  if (vh > 0 && wy + wh > vy + vh) wy = vy + vh - wh;

  HWND h = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kWndClass, "Apex receiver",
                           WS_OVERLAPPED | WS_VISIBLE, wx, wy, ww, wh, nullptr, nullptr, inst, nullptr);
  if (!h)
  {
    Report("no window");
    return 1;
  }
  if (!SetLayeredWindowAttributes(h, 0, 1, LWA_ALPHA))
  {
    // Not fatal: without it the window is merely visible, which is the old behaviour. Said out loud because
    // a visible test window is exactly the thing that made running a gate unpleasant.
    fprintf(g_out, "note: the receiver window could not be made invisible (error %lu)\n", GetLastError());
    fflush(g_out);
  }

  RECT r;
  GetWindowRect(h, &r);

  // WHERE THE WHEEL WILL BE AIMED: the point the CURSOR IS ALREADY AT, which the window was just built
  // around. It is read again (not assumed to be the centre) because the clamp above can shift the window, and
  // the measurement must be about the cursor, not about the window's geometry.
  GetCursorPos(&target);

  if (g_out)
  {
    fprintf(g_out, "window %ld,%ld %ldx%ld  target point %ld,%ld\n", r.left, r.top, r.right - r.left,
            r.bottom - r.top, target.x, target.y);
    fflush(g_out);
  }
  Sleep(200); // let the window settle before the first event can arrive

  // Say whether the cursor really is over us. If it is not, the measurement below is meaningless and it is
  // better to know that here than to read a zero and guess.
  //
  // ⚠️ ASKED AGAIN AT THE END, because the answer can change while the receiver is alive -- the user moves the
  // mouse and the point being measured is suddenly their window. That is not hypothetical: it is how a run
  // came to report zero wheel messages for the code under test while the host's log showed the wheel being
  // routed to other programs. A count of zero is only evidence of a regression if the cursor stayed here, so
  // the report has to say whether it did, and `check_apex_endtoend.sh` refuses to draw a conclusion when it
  // did not.
  char clsStart[128] = {0};
  g_oursAtStart = CursorIsOverUs(h, clsStart, sizeof(clsStart));
  if (g_out)
  {
    fprintf(g_out, "under cursor : %s (%s)\n", clsStart,
            g_oursAtStart ? "this receiver" : "SOMETHING ELSE");
    if (!g_oursAtStart)
      fprintf(g_out, "NOTE: the point is covered -- any wheel counts below measure the wrong window\n");
    fflush(g_out);
  }

  // The wheels arrive as a burst. Once they stop, wait a short grace period and finish -- do not sit under the
  // user's cursor for the whole timeout. (The timeout stays as a hard cap for the case where nothing arrives.)
  SetTimer(h, 1, (UINT)seconds * 1000, nullptr); // the cap
  SetTimer(h, 2, 250, nullptr);                  // the "has the burst stopped?" poll
  MSG msg;
  while (GetMessageA(&msg, nullptr, 0, 0) > 0)
  {
    // The timers are matched on their window AND id: WM_TIMER carries no other identity, and a bare check
    // would also catch timers this window never set (a mistake the webview spike made once).
    if (msg.message == WM_TIMER && msg.hwnd == h)
    {
      if (msg.wParam == 1)
      {
        FinishRun(h, "timeout");
        continue;
      }
      if (msg.wParam == 2 && g_lastWheelTick != 0 &&
          GetTickCount() - g_lastWheelTick > 1200)
      {
        FinishRun(h, "done");
        continue;
      }
    }
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }

  if (g_trace)
  {
    fclose(g_trace);
    g_trace = nullptr;
  }
  if (g_out)
    fclose(g_out);
  return 0;
}
