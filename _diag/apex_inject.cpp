// WHEEL-ONLY: this injects MOUSEEVENTF_WHEEL and nothing else, and it does not touch the cursor.
// (See test/check_apex_modular.sh section 9 for what this marker means and why it is required.)
// SEND EXACTLY N NOTCHES, and say so.
//
// Written because the first attempt at this measurement mixed two tools that sent different amounts, and
// the receiver's total could not be attributed to either. Both sides of an A/B have to use the SAME
// input or the comparison means nothing -- this is that same input, in one small program.
//
// ⚠️ IT DOES NOT MOVE THE MOUSE. That took two steps to get right, and the second one was a user report.
//
// This program started with `SetCursorPos(x, y)`: the receiver originally made the test come to IT by dragging
// the pointer to a point it had chosen. The receiver was later fixed to build its window AROUND the cursor
// instead (the pointer is read, never written), and the gate reads its aim point out of the receiver's own
// report -- so by the time this runs the cursor is already where it needs to be, and the SetCursorPos had
// become both unnecessary and harmful. It yanked the pointer back to a position read a fraction of a second
// earlier, so a user who moved their mouse mid-run had it teleported back under them: a TEST stealing the
// user's input device. ("该项目每次部署都在抢用户鼠标")
//
// And it did not buy reliability either. The wheel goes to whatever window is under the cursor when SendInput
// runs; if the user moves the pointer mid-run the wheels land on THEIR window, which is exactly what the
// receiver's own `valid: no` verdict exists to catch. Pinning the cursor first only hid that case from the
// validity check. Leaving the cursor alone measures the arrangement the product really runs in.
//
// usage: apex_inject <x> <y> <notches> [gapMs]
//   notches: positive = wheel up, negative = wheel down
//   x, y:    where the receiver IS (from its own report) -- read, never written
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
  if (argc < 4)
  {
    printf("usage: apex_inject <x> <y> <notches> [gapMs]\n");
    return 2;
  }
  const int x = atoi(argv[1]);
  const int y = atoi(argv[2]);
  const int notches = atoi(argv[3]);
  const int gap = (argc > 4) ? atoi(argv[4]) : 100;
  const int n = (notches < 0) ? -notches : notches;
  const int delta = (notches < 0) ? -WHEEL_DELTA : WHEEL_DELTA;

  int sent = 0;
  for (int i = 0; i < n; ++i)
  {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)delta;
    if (SendInput(1, &in, sizeof(in)) == 1)
      ++sent;
    if (gap > 0)
      Sleep(gap);
  }
  printf("injected %d of %d notches (%d deltas each) aimed at %d,%d, %d ms apart\n", sent, n, delta, x, y, gap);
  return 0;
}
