// ---------------------------------------------------------------------------
// apex_hostconfig_probe -- the HOST's own settings file, exercised without Windows.
//
// WHY IT EXISTS: apex.ini is hand-editable (the header in it says so, and it is the file a user opens when they
// want to know what Apex is doing), so what it MEANS has to survive being read, written and read again -- and the
// order the quick panel's features are drawn in is now one of the things it stores.
//
// ⚠️ THE FOCUS IS THE NEW PART AND THE ROUND TRIP, which is where a hand-edited file can hurt: a key that is
// written but not read back, or read back in a different order, is a setting the user changes twice.
//
// ⚠️ AND THE ORDER'S RULE IS THE ONE THAT DESERVES A CHECK: the panel sends the ids it is SHOWING (the features
// that map something right now), and `QuickOrderSetFromKeys` must merge that with what is already stored -- a
// feature that is temporarily unmapped must not lose the place it had (see the note on `quickOrder` in
// hostconfig.h). "The order I am looking at" and "the order that gets saved" are two different lists, and this is
// where the difference is written down.
//
// Build: g++ -std=c++17 -O2 -DAPEX_BUILDING_HOST -I apex -o build/_hostconfig_probe.exe _diag/apex_hostconfig_probe.cpp
// Run:   build/_hostconfig_probe.exe
// ---------------------------------------------------------------------------

#define APEX_BUILDING_HOST 1 // the header refuses to compile without it, exactly as for a host source file

#include "hostconfig.h"

#include <stdio.h>
#include <string.h>

using namespace apex;

static int failures = 0;
static int checks = 0;

static void Check(bool ok, const char *what, const char *detail = "")
{
  ++checks;
  printf("  %-66s %s%s%s\n", what, ok ? "ok" : "FAIL", detail[0] ? "  " : "", detail);
  if (!ok)
    ++failures;
}

// The saved order, as one string, so a check can say what it expects in one piece.
static void OrderText(const HostConfig &c, char *out, int outSize)
{
  out[0] = 0;
  int off = 0;
  for (int i = 0; i < c.quickOrderN && off < outSize - 2; ++i)
    off += _snprintf(out + off, outSize - off, "%s%s", i ? "," : "", c.quickOrder[i]);
}

// Does the text contain this needle? (strstr with the argument order a sentence reads in.)
static bool HasText(const char *hay, const char *needle) { return hay && needle && strstr(hay, needle) != nullptr; }

int main(void)
{
  printf("== the host's own settings (apex.ini) ==\n");

  // ---- THE FILE'S OWN SPELLING: one key per line, in order, and a repeated one is ignored ----
  {
    HostConfig c;
    Check(ParseHostConfig("quickorder=MediaControl|亮度\nquickorder=KeepAwake|保持唤醒\n"
                          "quickorder=mediacontrol|亮度\n", c),
          "a file with quickorder lines parses", "");
    char got[256] = {0};
    OrderText(c, got, (int)sizeof(got));
    // ⚠️ VERBATIM, AND THAT IS THE POINT OF THIS CHECK: a key carries the feature's own words for a pane
    // (「亮度」), so anything that case-folds it or strips a path out of it stops it matching the pane it names.
    // An earlier version ran the key through `NormaliseExe` (the exe-name normaliser) and did exactly that.
    Check(strcmp(got, "MediaControl|亮度,KeepAwake|保持唤醒") == 0,
          "  one key per line, in file order, EACH KEPT EXACTLY as written", got);
    Check(c.quickOrderN == 2, "  and the repeated key did not become a second entry", "");
  }

  // ---- WHAT THE PANEL SENDS: the whole order at once, one key per line ----
  {
    HostConfig c;
    c.QuickOrderAppend("SmoothWheel|滚动参数");
    c.QuickOrderAppend("KeepAwake|保持唤醒");
    c.QuickOrderAppend("MediaControl|亮度");
    c.QuickOrderAppend("MediaControl|音量");
    c.QuickOrderSetFromKeys("MediaControl|音量\nMediaControl|亮度");
    char got[256] = {0};
    OrderText(c, got, (int)sizeof(got));
    Check(strcmp(got, "MediaControl|音量,MediaControl|亮度,SmoothWheel|滚动参数,KeepAwake|保持唤醒") == 0,
          "a reorder moves the named panes and KEEPS the ones it did not name, after them", got);
  }
  {
    // ⚠️ THE CASE THAT MAKES THE MERGE WORTH HAVING, AND IT IS THE USER'S OWN RULE: a pane is ABSENT from the
    // list the page sends whenever its feature is switched off ("当插件总开关关闭后…其插件的局部快速面板功能要
    // 隐藏") or its own mapping switch is off. Its place must survive all of that, or switching a feature back on
    // would drop its panes to the end of the flyout.
    HostConfig c;
    c.QuickOrderAppend("KeepAwake|保持唤醒");
    c.QuickOrderAppend("MediaControl|亮度");
    c.QuickOrderAppend("SmoothWheel|滚动参数");
    c.QuickOrderSetFromKeys("SmoothWheel|滚动参数"); // the only pane on screen right now
    char got[256] = {0};
    OrderText(c, got, (int)sizeof(got));
    Check(strcmp(got, "SmoothWheel|滚动参数,KeepAwake|保持唤醒,MediaControl|亮度") == 0,
          "a page showing ONE pane does not throw the other places away", got);
  }
  {
    // ... and an empty value names nothing, so it is a no-op rather than a wipe. ("Nothing is on screen" must
    // not mean "forget everything".)
    HostConfig c;
    c.QuickOrderAppend("KeepAwake|保持唤醒");
    c.QuickOrderAppend("SmoothWheel|滚动参数");
    c.QuickOrderSetFromKeys("");
    char got[256] = {0};
    OrderText(c, got, (int)sizeof(got));
    Check(strcmp(got, "KeepAwake|保持唤醒,SmoothWheel|滚动参数") == 0, "an empty reorder changes nothing", got);
    c.QuickOrderSetFromKeys(nullptr);
    OrderText(c, got, (int)sizeof(got));
    Check(strcmp(got, "KeepAwake|保持唤醒,SmoothWheel|滚动参数") == 0, "  and neither does a missing one", got);
  }
  {
    // Spaces around a key, and blank lines between them: the page builds the list from keys, but a hand-written
    // line is a hand-written line, and a key with a space in front of it would silently not match.
    HostConfig c;
    c.QuickOrderSetFromKeys(" MediaControl|音量 \n\nKeepAwake|保持唤醒\n");
    char got[256] = {0};
    OrderText(c, got, (int)sizeof(got));
    Check(strcmp(got, "MediaControl|音量,KeepAwake|保持唤醒") == 0,
          "spaces around a key and blank lines are tolerated", got);
    // ⚠️ THE COMMA IS THE PANEL'S SEPARATOR, and it is only safe because of where the key is built: `QuickBlockKey`
    // (apex/quickpanel.h) turns a comma or a newline in a NAME into a space, so a key never contains either. This is
    // the shape the PANEL sends (one comma-separated value, one message per drag); the pure quick-panel probe checks
    // the other half of that pair -- that no name can put a comma into a key in the first place.
    HostConfig d;
    d.QuickOrderSetFromKeys("MediaControl|亮度,KeepAwake|保持唤醒");
    char got2[256] = {0};
    OrderText(d, got2, (int)sizeof(got2));
    Check(strcmp(got2, "MediaControl|亮度,KeepAwake|保持唤醒") == 0,
          "  and the panel's one-value, comma-separated order splits into the same keys", got2);
    // ⚠️ AND THE COUNT IS NOT DECORATION, IT IS THE HALF THAT CAN FAIL. `OrderText` JOINS WITH COMMAS, so a list
    // that was never split at all prints the very same string as a properly split one -- this check passed while the
    // comma had been (deliberately) removed from the separator test in `QuickOrderSetFromKeys`. One key that still
    // holds a comma is not two keys: it names NO pane, so the drag would do nothing in the panel. Any future
    // assertion about this list has to say how MANY keys it expects, or it cannot tell these two apart.
    Check(d.quickOrderN == 2, "  and it became TWO keys, not one long one that names no pane", "");
  }

  // ---- THE ROUND TRIP, which is the whole promise of a hand-editable file ----
  {
    HostConfig a;
    a.lang = Lang::kZh;
    a.theme = Theme::kDark;
    a.autostart = true;
    a.quickCompact = false;
    a.quickOwn = true;
    a.FeatureSetOff("AuditStub", true);
    a.QuickOrderAppend("KeepAwake|保持唤醒");
    a.QuickOrderAppend("MediaControl|亮度");

    char text[4096] = {0};
    FormatHostConfig(a, text, (int)sizeof(text));
    HostConfig b;
    b.lang = Lang::kEn; // a DIFFERENT start, so a value that fails to parse cannot pass by coincidence
    Check(ParseHostConfig(text, b), "what FormatHostConfig writes, ParseHostConfig reads", "");

    Check(b.lang == Lang::kZh && b.theme == Theme::kDark && b.autostart && !b.quickCompact && b.quickOwn,
          "  every setting survives the round trip", "");
    Check(b.FeatureOff("AuditStub") && !b.FeatureOff("SmoothWheel"),
          "  including which features are switched off", "");
    char got[256] = {0};
    OrderText(b, got, (int)sizeof(got));
    Check(strcmp(got, "KeepAwake|保持唤醒,MediaControl|亮度") == 0,
          "  and the quick panel's order, in its order", got);
    Check(HasText(text, "quickorder=KeepAwake|保持唤醒"),
          "  and it is written one key per line, verbatim, as documented", "");
  }

  // ---- UNKNOWN KEYS AND MANGLED VALUES STILL LEAVE THE DEFAULTS ALONE ----
  {
    HostConfig c;
    Check(ParseHostConfig("# a comment\nnothing=1\nquickown=maybe\nquickorder=\n", c),
          "a file with an unknown key and a mangled value still parses", "");
    Check(c.quickOwn && c.quickOrderN == 0, "  with the defaults untouched", "");
  }

  printf("\n");
  if (failures)
  {
    printf("FAILED: %d check(s)\n", failures);
    return 1;
  }
  printf("OK: the host's settings file reads, writes and reads back -- and the quick panel's order merges the\n"
         "    way the panel's reorder needs it to.\n");
  return 0;
}
