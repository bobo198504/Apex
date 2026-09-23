// The app's settings handling, exercised outside Windows.
//
// It includes the REAL app/config.h -- the same header the app compiles -- so this tests the shipped
// parse/format/clamp/skip-list rather than a copy of it (the project's rule for testable logic).
//
// Why it matters here: the settings file is hand-editable and is the one place a user or a bug report
// can put a nonsense number. Every value must be clamped onto its range, an unknown key must be
// ignored, and the skip list must match on an exe NAME (lower-case, no path) no matter how it was
// written -- a path, a full path, or a name in mixed case.
//
// g++ -std=c++17 -O2 -I. -I../src _diag/app_config_probe.cpp -o /tmp/acp && /tmp/acp
#include <cstdio>
#include <cstring>
#include "config.h"

static int failures = 0;
static void Check(bool ok, const char *what)
{
  printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++failures;
}

int main()
{
  printf("app settings: defaults, clamping, unknown keys, skip list\n\n");

  // ---- the app's OWN defaults, inside their ranges ----
  {
    app::Config c;
    // ⚠️ THE DEFAULTS ARE THE PLUGIN'S -- ALL FOUR -- AND THIS CHECK HAS STOOD ON BOTH SIDES OF THAT.
    // Ramp-up was 500 here for a while: first on a feel argument of mine, then because a remark of the user's
    // about the SLIDER's default was misread as a request for this value. They have since said it plainly --
    // "加速基准，默认应该是1000" -- and the rule that keeps it settled matters more than the number: THE SHARED
    // MODEL COMES WITH SHARED NUMBERS. The headers in shared/ are the plugin's byte for byte, so the two
    // products should also start from the same tuning; a deliberate difference needs the user asking for it,
    // not a maintainer's judgement about feel.
    Check(c.glideMs == 200.0, "default Glide length is 200 ms (the plugin's value)");
    Check(c.slowStep == 5.0, "default Slow step is 5 (the plugin's value)");
    Check(c.rampUp == 1000.0, "default Ramp-up is 1000 (the plugin's value too -- see common/config.h)");
    Check(c.topSpeed == 1.5, "default Top speed is 1.5x (the plugin's value)");

    // ⚠️ NO `enabled` CHECK ANY MORE. This feature has no master switch of its own: enabling is the generic
    // control the host keeps for every feature (see common/config.h for the reasoning). What that field's
    // coverage leaves behind is the OLD-FILE case below -- a settings file written by an earlier version
    // still carries `enabled=0`, and it has to LOAD, ignoring the line rather than failing on it.
    c.Clamp();
    Check(c.glideMs == 200.0 && c.slowStep == 5.0 && c.rampUp == 1000.0 && c.topSpeed == 1.5,
          "clamping the defaults changes nothing");
  }

  // ---- the window: the Glide value, clamped to the model's envelope ----
  printf("\n-- the window --\n");
  {
    app::Config c;
    Check(std::fabs(c.WindowMsFor() - c.glideMs) < 1e-9, "the window is the Glide length");
    // The envelope is the MODEL's (100..400), and the Glide slider's range (100..300) is narrower, so at the
    // shipped defaults the clamp never bites. Both ends are still asserted, because a value loaded from a file
    // is not obliged to respect the slider.
    app::Config big = c;
    big.glideMs = 900.0;
    big.Clamp();
    Check(big.WindowMsFor() <= app::Config::WindowHiMs() + 1e-9, "a huge Glide is clamped to the envelope");
    app::Config small = c;
    small.glideMs = 10.0;
    small.Clamp();
    Check(small.WindowMsFor() >= app::Config::WindowLoMs() - 1e-9, "a tiny Glide is clamped to the envelope");
  }


  // ---- every parameter is clamped onto its range, both ends ----
  {
    app::Config hi;
    hi.glideMs = 1e9; hi.slowStep = 1e9; hi.rampUp = 1e9; hi.topSpeed = 1e9;
    hi.Clamp();
    Check(hi.glideMs == app::Config::GlideHi(), "glide clamped to its maximum");
    Check(hi.slowStep == app::Config::SlowHi(), "slow step clamped to its maximum");
    Check(hi.rampUp == app::Config::RampHi(), "ramp-up clamped to its maximum");
    Check(hi.topSpeed == app::Config::TopHi(), "top speed clamped to its maximum");

    app::Config lo;
    lo.glideMs = -1e9; lo.slowStep = -1e9; lo.rampUp = -1e9; lo.topSpeed = -1e9;
    lo.Clamp();
    Check(lo.glideMs == app::Config::GlideLo(), "glide clamped to its minimum");
    Check(lo.slowStep == app::Config::SlowLo(), "slow step clamped to its minimum");
    Check(lo.rampUp == app::Config::RampLo(), "ramp-up clamped to its minimum");
    Check(lo.topSpeed == app::Config::TopLo(), "top speed clamped to its minimum");
  }

  // ---- parse: the shipped format round-trips, and a bad file cannot hurt ----
  {
    const char *text =
        "# a comment\n"
        "\n"
        "glide=250\n"
        "slow=7.5\n"
        "ramp=1500\n"
        "top=1.25\n"
        "tail=2.5\n"          // AN OLD FILE: that parameter is gone, so this line must be ignored
        "enabled=0\n"          // an OLD file: the line must be ignored, not choke the load
        "nosuchkey=42\n"        // must be ignored
        "ramp=oops\n";          // unparseable -> the setting is LEFT ALONE (it stays 1500)
    app::Config c;
    app::ParseConfigText(text, c);
    Check(c.glideMs == 250.0, "glide read back");
    Check(c.slowStep == 7.5, "slow step read back");
    // A TYPO MUST NOT MOVE THE SETTING. atof("oops") is 0.0, and 0.0 after clamping is the FLOOR -- so
    // reading it as a number would silently send this parameter to the end of its range. The whole token
    // has to parse; otherwise the value already read (here, from the line above) stands.
    Check(c.rampUp == 1500.0, "an unparseable number is ignored, leaving the value alone");
    Check(c.topSpeed == 1.25, "top speed read back");
    // ⚠️ tail= IS IGNORED NOW, and that is the point: an old settings file (the app wrote the line for
    // several versions) still has to LOAD. Unknown keys are ignored by design, and a removed parameter is
    // exactly the case that design exists for.
    Check(c.glideMs == 250.0 && c.slowStep == 7.5, "a file from a version with tail= still loads");
    Check(c.glideMs > 0 && c.rampUp > 0, "a file written by an older version still loads (its dead "
                                         "enabled= line is ignored)");
  }

  // ---- a number with trailing junk is not a number: "150ms" must not read as 150 ----
  {
    app::Config c;
    app::ParseConfigText("glide=150ms\n", c);
    Check(c.glideMs == 200.0, "a value with trailing junk keeps the default");

    app::Config d;
    app::ParseConfigText("glide=175\n", d);
    Check(d.glideMs == 175.0, "a clean value still parses");

    app::Config e;
    app::ParseConfigText("glide=175\n", e);
    Check(e.glideMs == 175.0, "a clean value still parses");
  }

  // ---- round trip: format then parse gives the same thing ----
  {
    app::Config a;
    a.glideMs = 275.0;
    a.slowStep = 3.5;
    a.rampUp = 800.0;
    a.topSpeed = 1.75;
    a.SkipAdd("Notepad.EXE");
    a.SkipAdd("C:\\Games\\some game.exe");
    char text[8192];
    app::FormatConfigText(a, text, (int)sizeof(text));

    app::Config b;
    app::ParseConfigText(text, b);
    Check(b.glideMs == a.glideMs && b.slowStep == a.slowStep, "round trip: the numeric values survive");
    Check(b.rampUp == a.rampUp && b.topSpeed == a.topSpeed, "round trip: the remaining values survive");
    Check(b.skipN == 2, "round trip: both skip entries survive");

    // ⚠️ AND THE FILE IS WRITTEN IN THE NEW SPELLING. The control is called `exclude` and the file used to
    // say `skip`; after the rename, a saved file must carry the new key -- otherwise the label and the id and
    // the file are three names for one thing, which is how the next reader gets lost.
    Check(strstr(text, "exclude=") != nullptr, "  and a saved file writes the list as `exclude=`");
    Check(strstr(text, "\nskip=") == nullptr, "  and no longer writes the old `skip=` spelling");

    // ⚠️ AND AN OLD FILE STILL LOADS ITS LIST IN FULL. This is the case that would be SILENT DATA LOSS: the
    // key was renamed, and if only the new spelling were parsed the user's list would come back EMPTY -- and
    // then the next save would write the empty list back over the file. Loading is not enough on its own
    // either, so the entry is checked: a parse that accepted the line and dropped the value would pass a
    // "the file loaded" test and still lose the list.
    {
      app::Config old;
      const char *legacy = "glide=200\nslow=5.0\nramp=1000\ntop=1.50\nskip=notepad++*\nskip=game.exe\n";
      app::ParseConfigText(legacy, old);
      Check(old.skipN == 2, "a file written before the rename keeps BOTH of its list entries");
      Check(old.SkipHas("notepad++*") && old.SkipHas("game.exe"),
            "  and they are the entries it had, patterns intact");
      // ... and re-saving that config writes the new spelling, which is how a file converts by itself.
      char out2[2048] = {0};
      app::FormatConfigText(old, out2, (int)sizeof(out2));
      Check(strstr(out2, "exclude=notepad++*") != nullptr,
            "  and saving it converts the file to the new spelling");
    }
  }

  // ---- the skip list matches on the NAME, however it was written ----
  {
    app::Config c;
    Check(c.SkipAdd("Notepad.EXE"), "a mixed-case name with an extension is accepted");
    Check(c.SkipHas("notepad.exe"), "it matches the lower-case name");
    Check(c.SkipHas("NOTEPAD.EXE"), "and an upper-case query");
    Check(!c.SkipHas("notepad2.exe"), "a different name does not match");
    Check(!c.SkipAdd("notepad.exe"), "adding the same name twice is refused");
    Check(c.skipN == 1, "so the list still holds one entry");

    Check(c.SkipAdd("C:\\Program Files\\Some App\\thing.exe"), "a full path is accepted");
    Check(c.SkipHas("thing.exe"), "and reduces to the bare name");
    Check(!c.SkipHas("C:\\Program Files\\Some App\\thing.exe"), "while the path form does not match");

    c.SkipRemove(0);
    Check(!c.SkipHas("notepad.exe"), "removing an entry works");
    Check(c.SkipHas("thing.exe"), "and leaves the rest");
    Check(c.skipN == 1, "with the count updated");

    c.SkipRemove(0);
    Check(c.skipN == 0, "the list can be emptied");
    c.SkipRemove(0); // must not misbehave on an empty list
    Check(c.skipN == 0, "removing from an empty list is harmless");
  }

  // ---- PATTERNS: the fuzzy matching, and above all its ANCHORING ----
  //
  // ⚠️ THE ANCHORING IS THE WHOLE TEST. "Fuzzy" has an easy wrong version -- match anywhere in the name --
  // and its failure mode is silent and enormous: an entry of `e` would claim every program with an e in it,
  // so the wheel quietly stops smoothing in places the user never listed and never hears about it. Every
  // entry here is written to fail if the match is not anchored at both ends.
  printf("\n-- the skip list as PATTERNS --\n");
  {
    app::Config c;
    Check(c.SkipAdd("game*"), "'game*' is accepted as a pattern");
    Check(c.SkipHas("game.exe"), "  and matches game.exe");
    Check(c.SkipHas("game-launcher.exe"), "  and a longer name that starts with it");
    // ⚠️ `game*` DOES MATCH `gameplay` -- anything after the literal is what the star is FOR. (An earlier
    // version of this check asserted the opposite and failed: the assertion was wrong, not the matcher. The
    // anchoring is what the line below proves, and it is the property that matters.)
    Check(c.SkipHas("gameplay"), "  including a name with no extension at all");
    Check(!c.SkipHas("mygame.exe"), "  but NOT a name that merely CONTAINS it (anchored at the START)");

    app::Config q;
    Check(q.SkipAdd("tool?"), "'tool?' is accepted");
    Check(q.SkipHas("tool1"), "  and matches one character");
    Check(q.SkipHas("tools"), "  including a letter");
    Check(!q.SkipHas("tool"), "  but not zero characters ('?' is exactly one)");
    Check(!q.SkipHas("tool12"), "  and not two");

    app::Config both;
    Check(both.SkipAdd("*tool.exe"), "a LEADING star is accepted");
    Check(both.SkipHas("mytool.exe"), "  and matches a suffix");
    Check(!both.SkipHas("mytool.dll"), "  only the suffix it names");
    Check(both.SkipAdd("a*b*c"), "two stars and a literal in between");
    Check(both.SkipHas("abc"), "  match with nothing in the middle");
    Check(both.SkipHas("axxbyyc"), "  and with text in both gaps");
    Check(!both.SkipHas("axxcbyy"), "  but not when the letters are in the wrong order");

    // A BARE NAME IS STILL EXACT -- the old behaviour, kept because every existing entry relies on it.
    app::Config exact;
    exact.SkipAdd("notepad.exe");
    Check(exact.SkipHas("notepad.exe"), "a name with no wildcards still matches itself");
    Check(!exact.SkipHas("notepad"), "and does NOT match a prefix of it");
    Check(!exact.SkipHas("notepad.exe.bak"), "nor a name it is a prefix of");

    // ⚠️ A LONE STAR MATCHES EVERYTHING, which is the one pattern that can silence the feature entirely.
    // It is allowed (the user typed it, and the panel shows it back) but it must be OBEYED rather than
    // half-matched: "everything is blacklisted" is a state the user can see and undo, where a star that
    // matched nothing would look like the list was broken.
    app::Config star;
    star.SkipAdd("*");
    Check(star.SkipHas("anything.exe"), "'*' matches every name (so the list is honouring what it shows)");

    // CASE: patterns are lower-cased on the way in and the query is lower-cased on the way through, so a
    // user who types capitals gets the match they expect.
    app::Config caps;
    caps.SkipAdd("Game*.EXE");
    Check(caps.SkipHas("GAME1.exe"), "a pattern typed in capitals matches in any case");

    // DUPLICATES, and the comparison that is deliberately NOT a match test: `game*` and `game.exe` are
    // different entries with different reach, so entering one after the other must be allowed.
    Check(!caps.SkipAdd("game*.exe"), "the same pattern twice is refused");
    Check(caps.SkipAdd("game.exe"), "but 'game.exe' beside 'game*.exe' is a different entry and is allowed");

    // ⚠️ AND A PATH IS STILL REDUCED TO ITS FILE NAME -- the normalisation the exact match already had. A
    // pasted path with wildcards in the FOLDER would otherwise become a pattern that can never match.
    app::Config path;
    path.SkipAdd("C:\\Some Dir\\thing*.exe");
    Check(path.SkipHas("thing1.exe"), "a pasted path reduces to its file name, wildcards intact");
  }

  printf("\n%s\n", failures ? "FAIL" : "OK: defaults, clamping, parsing and the skip list all hold");
  return failures ? 1 : 0;
}
