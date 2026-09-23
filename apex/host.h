#ifndef APEX_HOST_H
#define APEX_HOST_H

// ⚠️ A FEATURE MAY NOT INCLUDE THIS FILE -- the guard below is what makes that a CONTRACT rather than a
// convention. A feature is compiled with the same -I paths as the host (see apex/build.sh) and exports a C
// ABI, so nothing else stops it reaching in here and calling host internals the ABI never promised to keep
// stable. Everything a feature is entitled to is in abi.h; this file is the host's own interface to its platform layer.
#ifndef APEX_BUILDING_HOST
#error "host.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif

// ---------------------------------------------------------------------------
// THE HOST'S OWN VERSION, in one place.
//
// ⚠️ IT IS THE PROGRAM'S VERSION, NOT A FEATURE'S. A feature carries its own (AutoIME 1.2.1, SmoothWheel 1.7.1 --
// the versions of the standalone programs they came from), and the panel shows each one on its own page. This is
// the number for Apex itself, which the panel puts beside the slogan in the sidebar ("Apex主程序版本号1.0 ... 居右").
// One definition, because the snapshot and anything else that reports it must not be able to disagree.
// ---------------------------------------------------------------------------
#define APEX_HOST_VERSION "1.3"

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

// Is this project's REAPER plugin running ANYWHERE on this machine? A MACHINE-wide question, unlike the one
// above (which is about the program under the cursor) -- see ApexHost::reaperPluginRunning for why both exist
// and why this one must not be used to decide anything.
//
// Expensive (enumerates processes and reads a module list), so it is for a settings page, not a hot path.
// false when it cannot tell.
int ReaperPluginIsRunning();

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
// Declared here because MORE THAN ONE translation unit needs them: settings_host.cpp answers the settings
// panel with them, quickpanel_win.cpp builds the flyout out of them, and main.cpp defines them. They are the
// host's own objects, not a copy -- so neither surface can reach anything the tray could not.
HostConfig *SettingsConfig();
Loader *SettingsLoader();
void SettingsLog(const char *fmt, ...);
void SettingsSaveAll();

// IS THE INTERFACE TO BE CHINESE? `auto` resolves by asking Windows for the user's preferred UI language, so
// the tray, the flyout and the settings page cannot disagree about it -- which is the whole reason this is one
// function rather than three (see the note where the tray strings live, in main.cpp).
bool UiIsChinese();

// SOMETHING THE SETTINGS PAGE DRAWS HAS CHANGED -- tell it NOW rather than at the next tick of the REAPER
// watcher. Used by the quick panel, which is a second view of the same state and can be operated while the page
// is open behind it (the user's rule: "快速面板的开关要能实时同步到设置面板上"). `what` is one of the
// kState* values in settings_ipc.h. Cheap and safe to call with no panel open.
void PanelStateChanged(unsigned what);

// ONE BLOCK OF THE QUICK PANEL: WHAT THE FLYOUT DRAWS AS A SINGLE PANE.
//
// ⚠️⚠️ THE UNIT IS THE BLOCK, NOT THE FEATURE, AND THE USER SAID SO: "快速面板的局部功能分组是按单个开关算的，
// 不是按插件算，所以这边排序要注意". A feature's controls are not collected under its name -- each control (or each
// NAMED GROUP of them: a monitor's faders, a program list) is a pane of its own (see the grouping rule in
// apex/quickpanel.h) -- so a list of FEATURES could not show what the flyout shows, and could not order 亮度
// against 音量 even though they are two panes.
struct QuickBlock
{
  int slot = -1;       // the feature that owns it (Loader::At), so a caller can fetch that feature's items
  char key[112] = {0}; // "<feature id>|<group name>", or "<feature id>|<item id>" for an item that names no group
  char nameZh[64] = {0}; // what the flyout calls the pane: the group's name, or the item's own label
  char nameEn[64] = {0};
};

// THE KEY OF THE BLOCK AN ITEM BELONGS TO. ⚠️ IT IS `inline` IN quickpanel.h RATHER THAN DECLARED HERE, for the same
// reason the layout arithmetic lives there: it is a pure function of a feature id and an item, so the probe that has
// no host, no window and no GDI+ can exercise it -- and it is a TRANSPORT string as well as an identity (the order
// travels to the page and back), which is exactly the kind of rule that has to be checkable. See the note on it.

static const int kMaxQuickBlocks = 64; // the same ceiling the flyout's own item list has

// EVERY BLOCK THE QUICK PANEL WOULD DRAW, IN THE ORDER IT WOULD DRAW THEM.
//
// ⚠️ ONE IMPLEMENTATION FOR TWO READERS, WHICH IS THE WHOLE REASON IT IS HERE. The flyout builds its panes in
// this order, and the General page draws its reorder list from the same answer (settings_host.cpp puts it in the
// snapshot as `quickOrder`). Two implementations of "which blocks, in what order" could disagree, and the user
// would then be looking at a list whose order is not the order they are looking at.
//
// The order is: the keys in `cfg->quickOrder` first (in that order), then every other block in the order the
// features and their items come in.
//
// ⚠️ A FEATURE THE USER HAS SWITCHED OFF CONTRIBUTES NOTHING AT ALL. The flyout used to draw its rows greyed out,
// and the user's rule is the opposite: "当插件总开关关闭后…其插件的局部快速面板功能要隐藏，打开后再按各局部功能快速
// 面板功能开关状态还原" -- hidden, and the per-control mapping switches are untouched, so switching the feature
// back on brings its rows back exactly as they were. Nothing has to be remembered for that: this function simply
// does not see a switched-off feature, and the saved order keeps its key (see QuickOrderSetFromKeys) so it also
// comes back in its own place.
//
// ⚠️ IT ASKS EACH FEATURE `quickItems`, which is why it is a settings-page call and not a frame call: a feature
// that enumerates hardware does it on its own throttle (see MediaControl).
int QuickBlocks(QuickBlock *out, int max);

// PUT THE MACHINE IN STEP WITH THE `autostart` SETTING (main.cpp).
//
// ⚠️⚠️ THIS IS THE ONE WRITE APEX MAKES OUTSIDE ITS OWN FOLDER, and it exists because the user asked for the
// feature ("通用设置增加选项'开机启动'，默认不启用"): Windows has no way to start a program at logon from inside
// that program's directory, so it is the per-user Run key or nothing. Called at start-up (so the entry is
// re-pointed at wherever this copy lives now -- the folder is portable and can be moved) and whenever the switch
// is flipped. With the setting off it DELETES the value, so a user who turns it off is not left with a dead
// entry pointing at a folder they may have moved or removed.
void AutostartApply();

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

// WHERE THE TRAY ICON IS, so the quick panel can appear above it. False when the shell will not say -- an icon
// in the overflow flyout, or a tray that has just restarted -- which the caller answers by falling back to the
// cursor. It lives here rather than in quickpanel_win.cpp because the icon's identity (the host window and its
// uID) is main.cpp's, and a second copy of "which icon is ours" is exactly the kind of duplication that puts a
// flyout under somebody else's icon.
bool TrayIconScreenRect(int *x, int *y, int *w, int *h);

// ---- THE QUICK PANEL -- the flyout the tray shows on a single click (quickpanel_win.cpp) ----
//
// It is the host's own window, created once and kept hidden, because the settings panel is a browser in another
// process and takes ~0.55 s to appear (see apex/quickpanel_win.cpp for the whole argument).
void QuickPanelInit();                  // create the window (called at start-up, so the first click is instant)
void QuickPanelShow();                  // open it, or bring it back if it is fading out
void QuickPanelToggle();                // what a single tray click means: show it, or put it away
void QuickPanelHide(const char *why);   // put it away (the reason is logged)
bool QuickPanelVisible();               // is it up and not on its way out
void QuickPanelRefresh();               // the language or the theme changed under it

// ⚠️ THE FLYOUT'S WINDOW CLASS, ASKED FOR BY NAME SO THERE IS ONLY ONE COPY OF IT. The window is created in
// quickpanel_win.cpp, and main.cpp needs the same string for a different question: is the wheel under the cursor
// over OUR OWN UI? A wheel there is not a scroll to be smoothed, it is the user's hand on a control -- and since
// the flyout now has faders that take the wheel (see quickpanel.h), smoothing one would deliver a single notch as
// dozens of small messages and the value would run away. That is the same rule the settings panel is on (see
// decision.h rule 2), and a second literal there would be a second answer to "where is the panel".
const char *QuickPanelWndClass();

// TEST-ONLY: APEX_QUICKPANEL_ONCE is set, so the flyout is shown once at start-up and hidden again. The gate
// that uses it (test/check_apex_flyout.sh) is the only thing that sets it, and the flyout must NOT take the
// focus while it is set -- a test may not interrupt whoever is at the machine. See the definition in main.cpp
// for why the product carries a switch for this at all.
bool QuickPanelSelfTest();

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
