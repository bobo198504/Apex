// ---------------------------------------------------------------------------
// THE SETTINGS PANEL -- its own process.
//
// WHY IT IS NOT PART OF apex.exe: the hook lives in the OS input path. A window that renders a browser,
// waits on the user, and can be killed from Task Manager must not be able to affect it. Two processes mean
// the panel can be hung, crash-restarted, or simply closed while smoothing keeps running untouched.
//
// WHAT THIS FILE IS: the entry point, and the host's launcher. Everything else is in ui_webview.cpp (the
// window and the browser) and settings_ipc.h (how the two processes talk).
//
// IT CAN ALSO BE RUN ON ITS OWN -- double-clicking apex-settings.exe opens the panel, which then reports
// that the host is not running rather than silently doing nothing. That is the useful half of the split:
// the panel is testable without the hook, and the hook has no dependency on the panel existing.
// ---------------------------------------------------------------------------

#include "settings_ipc.h"

#include <windows.h>
#include <stdio.h>

namespace apex {
namespace ui {
int RunPanel(HINSTANCE inst, bool startHidden);
}
} // namespace apex

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
  // A second copy is not a second panel: it is two windows fighting over the same settings, so an existing
  // one is brought forward instead. (The host does the same before launching, but a user can double-click
  // the exe directly, and that path has to behave too.)
  //
  // ⚠️ FindOwnWindow, NOT FindWindowA. The window class is registered by EVERY copy of this portable program,
  // so the plain lookup finds whichever copy happens to be first on the machine -- and bringing another
  // installation's panel to the front while exiting silently leaves this copy with no panel and the user
  // looking at somebody else's settings. See the note in settings_ipc.h.
  if (HWND existing = apex::FindOwnWindow(APEX_SETTINGS_WND_CLASS))
  {
    ShowWindow(existing, SW_SHOW);
    SetForegroundWindow(existing);
    return 0;
  }
  return apex::ui::RunPanel(inst, false);
}
