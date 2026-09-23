// ---------------------------------------------------------------------------
// THE TWO PRODUCT MARKS, AND THE IDS THE WHOLE PROJECT AGREES ON.
//
// WHY THIS IS A HEADER AND NOT A `#define` IN main.cpp -- this is a bug that shipped, and it was
// invisible from every side at once:
//
//   * apex.rc wrote `IDI_APEX_LIGHT ICON "..."`, and main.cpp `#define`d that name to 1. The resource
//     compiler NEVER SEES A C++ DEFINITION, so windres registered the artwork under the STRING NAME
//     "IDI_APEX_LIGHT" instead of under the number 1. (A name in a .rc that no one has #defined is not an
//     error -- it is a resource with that name, and `windres` says nothing.)
//   * The code asks MAKEINTRESOURCE(2) on a light system. No such resource existed, so LoadImage returned
//     NULL, the tray was handed no icon, and the shell kept showing the mark it already had.
//   * The log line reported the mark that was CHOSEN, not the one that loaded, so it claimed the switch
//     had happened. test/check_apex_language.sh asserted the rule and never looked at a resource.
//
// The result was an icon that "never follows the theme" while every layer reported success.
//
// Both .rc files and both .cpp files now read the numbers from HERE: the resource compiler through its own
// preprocessor, the C++ through its. They cannot drift. test/check_apex_icons.sh asserts the built exes.
//
// ---------------------------------------------------------------------------
// WHICH MARK GOES WITH WHICH APPEARANCE -- read this before "fixing" the mapping.
//
// The mark is PLATE + GLYPH, and the plate is meant to MATCH THE BACKGROUND it sits on while the glyph
// carries the contrast:
//
//   icon/apex-light.ico   a NEAR-WHITE plate (#FFFDEF) with the glyph in black     -> a LIGHT appearance
//   icon/apex-dark.ico    a NEAR-BLACK plate (#10130C) with the glyph in cream   -> a DARK appearance
//
// So the file name says WHICH APPEARANCE IT BELONGS TO, and the mapping is the direct one:
//
//   a light appearance -> IDI_APEX_LIGHT      a dark appearance -> IDI_APEX_DARK
//
// ⚠️ THIS WAS INVERTED ONCE, by exactly the reasoning that looks obviously right: "a light taskbar needs a
// dark icon for contrast". That is true of a BARE glyph, and these are not bare glyphs -- the plate would
// then be a bright blob, or a black square, sitting on a taskbar it does not match. Measuring the mean luma
// made it worse rather than better: the mean reports the PLATE, so "apex-light.ico is the bright one" was
// read as "it is the one for a dark desktop". Looking at the artwork is what settles it, and the names
// were right from the start.
// ---------------------------------------------------------------------------
#ifndef APEX_ICONS_H
#define APEX_ICONS_H

// ⚠️ A FEATURE MAY NOT READ THIS. The two ids are shared between the .rc files and the host's C++, and a
// feature has no business knowing either -- it reports motion, it does not draw Apex. See the note in abi.h.
// (Skipped when windres is preprocessing a .rc: the resource compiler defines RC_INVOKED and never defines
// APEX_BUILDING_HOST, and it is entitled to these numbers.)
#ifndef RC_INVOKED
#ifndef APEX_BUILDING_HOST
#error "icons.h is the HOST's -- a feature includes only abi.h (see the note at the top of abi.h)"
#endif
#endif

// The CREAM-plated mark (icon/apex-light.ico) -- for a LIGHT appearance. It doubles as the application's
// own file icon: id 1 is what Explorer and a shortcut show, and there is exactly one of those, which is
// why the .rc files carry no second copy of the same artwork.
#define IDI_APEX_LIGHT 1

// The DARK-plated mark (icon/apex-dark.ico) -- for a DARK appearance.
#define IDI_APEX_DARK 2

// ---------------------------------------------------------------------------
// THE MAPPING ITSELF, WRITTEN ONCE.
//
// This is the interface the rest of the project uses: `appearance -> mark id`. The host's tray and the
// panel's window both call it, and NEITHER writes the choice out again.
//
// WHY IT IS A FUNCTION AND NOT TWO TERNARIES: it was two ternaries -- one in main.cpp, one in ui_webview.cpp
// -- and that is the shape that keeps producing this bug. Two copies of a mapping cannot be kept in step by
// discipline; they can only be kept in step by there being one copy. When this mapping was found inverted,
// both copies had been written by the same wrong reasoning, and either one could have been "fixed" alone,
// leaving the tray and the window disagreeing.
//
// The guard: the .rc files include this header too, and the RESOURCE COMPILER has no business seeing a C++
// function. `RC_INVOKED` is defined by windres (and by rc.exe) while it preprocesses a .rc, so the .rc side
// gets the two numbers above and nothing else.
// ---------------------------------------------------------------------------
#ifndef RC_INVOKED
static inline int ApexMarkForAppearance(bool lightAppearance)
{
  return lightAppearance ? IDI_APEX_LIGHT : IDI_APEX_DARK;
}

// ---- THE MARK WHILE A FEATURE IS HOLDING SOMETHING: the middle bar changes colour ----------------
//
// The mark is a PLATE plus ONE VERTICAL BAR down the middle (look at icon/*.png): cream plate with a near-black
// bar for a light appearance, near-black plate with a cream bar for a dark one. When a feature is holding
// something on the user's behalf, that bar is drawn in one of the two HOLD INKS instead. Nothing is added to
// the artwork and nothing is drawn beside it: the same shape, saying "I am working -- and how hard".
//
// ⚠️ THERE ARE TWO DEGREES, AND THEY ARE THE USER'S OWN ARRANGEMENT. KeepAwake can hold one of two things --
// "the machine will not sleep" (invisible: nothing on screen changes) and "the screen will not turn off" (very
// much visible: it is either wanted or infuriating). The user asked for the two to be told apart at a glance,
// in the tray: "这个插件只有三个状态：什么也不管，保持唤醒，保持唤醒+防止熄屏。所以托盘图标再加一种状态：什么也不管
// （原样），保持唤醒（绿色），保持唤醒+防止熄屏（红色）". So:
//
//     nothing held      ->  the plain artwork, its own bar untouched
//     an ordinary hold  ->  the bar in the GREEN below
//     the stronger hold ->  the bar in the RED below
//
// HOW IT GOT HERE, because two of these steps were mine and wrong: first a green dot in the corner (tried, and
// invisible -- the composition was broken in two separate ways, see traymark.h), then the bar in orange, then
// red with the bar grown a pixel on each side -- which the user rejected as too fat and asked for COLOUR ONLY:
// "应该是视觉错觉了。改回去吧，比较好看。用红色的，这样也比较显示。大红。不管是浅还是暗主题，用一样的颜色".
// So: no growth, one colour for both appearances. The second ink came later and is the same recipe -- a colour,
// and nothing at all about the shape.
//
// ⚠️ ONE COLOUR FOR BOTH PLATES IS A DELIBERATE EXCEPTION to the rule this artwork follows (its bar is
// near-black on cream and cream on near-black, because one value cannot carry contrast on both). A SATURATED
// colour is the case where it works: dark enough to stand out on the cream plate, bright enough to stand out
// on the near-black one. That claim is not taken on trust -- `test/check_apex_palette.sh` measures BOTH inks
// against BOTH plate colours (WCAG's non-text minimum, 3:1), which is why the plates are written down below.
//
// The order is COLORREF's: 0x00BBGGRR.
#define APEX_HOLD_INK 0x00327D2E // #2E7D32 -- the ordinary hold: the GREEN the panel already uses for "on"

// ⚠️ AND THAT GREEN IS NOT A NEW COLOUR: it is the panel's own `--dot-on`, the "this feature is switched on"
// dot in the feature list. That dot had to survive being drawn on the page, on a card, and on a SELECTED row
// (near-black in the light theme, near-off-white in the dark one), so it was chosen to hold its contrast on
// both -- the argument is in §3.7.4 of AGENTS.md. Using it again here means the tray's "working" green and the
// panel's "enabled" green are ONE green, and `test/check_apex_palette.sh` asserts that they are the same
// NUMBER -- two copies of a hex value are two values that will eventually disagree.
#define APEX_HOLD_HARD_INK 0x001200E6 // #E60012, 大红 -- the stronger hold: the same red on both plates

// THE ARTWORK'S TWO PLATE COLOURS, as drawn in icon/*.ico. They are here so the measurements above have
// something to measure against, and so that a redrawn mark has an obvious number to update.
#define APEX_PLATE_LIGHT 0x00EFFDFF // #FFFDEF (the redrawn artwork: a near-white plate, sampled)
#define APEX_PLATE_DARK 0x000C1310  // #10130C
#endif
#endif
