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
//   icon/apex-light.ico   a CREAM plate (#F4EAC6) with the glyph in near-black  -> a LIGHT appearance
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
#endif

#endif
