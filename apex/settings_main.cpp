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
#include <string.h>

namespace apex {
namespace ui {
int RunPanel(HINSTANCE inst, bool warm);
}
} // namespace apex

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int)
{
  // ⚠️ `--warm` MEANS "START, LOAD THE PAGE, AND WAIT -- DO NOT APPEAR". The host starts this process when the
  // user right-clicks the tray icon, because the panel cannot show anything until WebView2 has started
  // (~0.55 s, measured) and that wait is spent while the user is still choosing from the menu. A warm-up that
  // showed itself would be a window nobody asked for, so it waits to be told (see PrewarmPanel in main.cpp and
  // the two messages in settings_ipc.h).
  const bool warm = (cmdLine != nullptr && strstr(cmdLine, APEX_PANEL_WARM_ARG) != nullptr);

  // A second copy is not a second panel: it is two windows fighting over the same settings, so an existing
  // one is brought forward instead. (The host does the same before launching, but a user can double-click
  // the exe directly, and that path has to behave too.)
  //
  // ⚠️ FindApexPanel, NOT FindWindowA. The window class is registered by EVERY Apex process on the machine,
  // so the plain lookup finds whichever one happens to be first -- and bringing a panel of a host that this
  // copy is not talking to gives the user a window whose edits go somewhere else. The lookup identifies the
  // process by its executable name, which is the machine-wide identity the host's own single-instance check
  // uses. Since there is exactly one Apex running at a time (that is the rule), it is also the panel of the
  // installation that is up. See the note in settings_ipc.h.
  if (HWND existing = apex::FindApexPanel())
  {
    // ⚠️ A WARM-UP THAT FINDS A PANEL ALREADY THERE HAS NOTHING TO DO -- AND MUST NOT SHOW IT. The user
    // right-clicked the tray; they have not asked for the settings yet, and a warm-up that revealed a window
    // would be worse than the pause it exists to remove.
    if (!warm)
    {
      ShowWindow(existing, SW_SHOW);
      SetForegroundWindow(existing);
    }
    return 0;
  }
  // ⚠️ THE WINDOW THIS OPENS IS NOT SHOWN IMMEDIATELY, AND THAT IS DELIBERATE -- see ShowPanelOnce in
  // ui_webview.cpp. It stays hidden until the page has drawn, because until then there is nothing in it but
  // background colour.
  return apex::ui::RunPanel(inst, warm);
}
