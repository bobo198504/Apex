#ifndef APEX_HOST_H
#define APEX_HOST_H

// ---------------------------------------------------------------------------
// THE HOST'S OWN INTERFACE: everything apex.exe needs from host_win.cpp, plus the places where the
// settings window and the hook talk to each other.
//
// The ABI header (abi.h) is what a FEATURE sees and must stay small and stable. This file is private to
// the host and may change freely -- keeping the two apart is what stops a host refactor from becoming an
// ABI break for every feature.
// ---------------------------------------------------------------------------

#include "abi.h"
#include "hostconfig.h" // HostConfig, named by the settings accessors below
#include "loader.h"     // Loader, likewise

namespace apex {
namespace host {

// A wheel event arrives here. Return true to swallow the original message.
typedef bool (*WheelSink)(const ApexWheelEvent &ev, void *user);

// ---- capture ----
bool CaptureStart(WheelSink sink, void *user);
void CaptureStop();
bool CaptureAcceptsInjected();

// ---- target identity ----
// The bare lower-case exe name of the program at a screen point. false when nothing is known.
//
// ⚠️ `rootOut` IS THE TOP-LEVEL WINDOW THE POINT LANDS IN, WHICH IS NOT THE SAME AS THE WINDOW
// `WindowFromPoint` RETURNS. A WebView2 page is a child window owned by a DIFFERENT PROCESS
// (msedgewebview2.exe) nested inside its host's window, so "which process owns what the cursor is over" says
// "a browser" -- shared infrastructure that could belong to any program -- while the top-level window says
// which APPLICATION the user is actually pointing at. Measured with _diag/apex_hit_probe.cpp: over the
// settings page, `WindowFromPoint` gives Chrome_RenderWidgetHostHWND (msedgewebview2.exe) and its root is
// ApexSettingsWnd (apex-settings.exe).
//
// The caller owns any policy about whose window that is; this layer only reports what the OS says.
bool TargetUnderCursor(int x, int y, char *exeOut, int exeSize, unsigned long *pidOut, void **rootOut);
// See host_win.cpp: PRESENT means "a better handler is loaded there", UNKNOWN means "could not tell",
// and both mean "do not act".
int ExternalHandlerState(const char *exeName, unsigned long pid, char *detailOut, int detailSize);

// ---- injection ----
void InjectQueuePush(int delta);
bool InjectThreadStart();
void InjectThreadStop();
long LastInjectMicros();

// ---- the clock ----
typedef void (*EngineFn)(void *user);
bool EngineStart(EngineFn fn, void *user, double periodMs);
void EngineStop();

// ---- what the panel asks the system ----
bool SystemIsLightTheme();
bool PreferredUiLanguage(char *out, int outSize);
bool LanguageTagIsChinese(const char *tag);

double NowSecondsPublic();

// ---- the settings protocol's view of the host's state ----
//
// Declared here because TWO translation units need them: settings_host.cpp answers the panel with them, and
// main.cpp defines them. They are the host's own objects, not a copy -- so the panel cannot reach anything
// the tray could not.
HostConfig *SettingsConfig();
Loader *SettingsLoader();
void SettingsLog(const char *fmt, ...);
void SettingsSaveAll();

// SOMETHING WORTH PERSISTING CHANGED -- schedule the debounced write (see APEXWM_SAVE_TIMER in main.cpp).
// Every IPC command that alters a setting calls this, so none of them has to think about WHEN the file is
// written; and because the decision lives in the host, a feature gets it without implementing anything.
void SettingsTouch();
// (SettingsRefreshTarget and SettingsOpenFileForSlot were declared here. Both are gone -- see the note where
// they were defined in main.cpp: the first served a host-side blacklist that no longer exists, and the second
// was a second implementation of opening a feature's file that disagreed with the one the panel uses.)

// ---- the tray's own refresh ----
//
// Called when a change made in the PANEL has to show up in the tray (the language and the theme both do: the
// tray's text and icon are drawn from them). Declared here rather than kept static in main.cpp because the
// panel's request arrives in settings_host.cpp, and the tray lives in main.cpp -- routing it through the
// header is what keeps the dependency one-way.
void TrayRefreshFromSettings();

// ---- the feature whose call is in progress ----
//
// Set around every call INTO a feature, so that a feature asking the host where its own folder is gets the
// right answer without having to pass its id back on every call (see ApexHost::featureDir).
//
// IT MUST BE SET BEFORE init(). That is not obvious and was got wrong once: a feature reads its settings
// in init(), so if the id is not yet set, the very first question -- "where are my settings" -- is
// answered "(unknown)" and the feature cannot start correctly. The loader sets it; so does the wheel and
// frame dispatch in main.cpp.
void SetCurrentFeature(const char *id);
const char *CurrentFeature();

} // namespace host
} // namespace apex

#endif // APEX_HOST_H
