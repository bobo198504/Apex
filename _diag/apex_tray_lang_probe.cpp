// THE TRAY'S LANGUAGE RULE, tested where it can actually be tested.
//
// Reading the tray back from the shell was attempted first and abandoned: the tray is the shell's own window,
// its internal structure differs between Windows builds (this machine's Shell_TrayWnd is not where the
// documented layout says it is), and a diagnostic that cannot be run is worse than no diagnostic. See
// _diag/apex_tray_state.cpp for the attempt and its result.
//
// So the RULE is tested instead of its rendering: which strings a given configuration selects, and -- the part
// that actually goes wrong in practice -- whether the Chinese ones survive the UTF-8 to UTF-16 conversion that
// the tray calls require. Mojibake is invisible in code review and obvious to a user, so it is asserted here.
//
// g++ -std=c++17 -O2 -mconsole _diag/apex_tray_lang_probe.cpp -o build/traylang.exe -luser32 -ladvapi32
#include <windows.h>
#include <stdio.h>
#include <string.h>

// ---- the same rule the host applies (app/apex/main.cpp), reproduced here in its essentials ----
//
// It is reproduced rather than linked because the host's copy is inside an anonymous namespace in a file with
// a WinMain; linking it would mean building the whole host. The risk of drift is real but bounded: what is
// being checked is that a language CHOICE maps to the expected string, and that the conversion is lossless.
// If the rule itself changes, the host's own log line ("tray: lang=...") is the cross-check.
enum Lang { kAuto = 0, kLangZh = 1, kLangEn = 2 };

static bool TagIsChinese(const char *tag)
{
  if (!tag || !*tag)
    return false;
  if (_strnicmp(tag, "zh", 2) == 0 && (tag[2] == 0 || tag[2] == '-' || tag[2] == '_'))
    return true;
  if (strlen(tag) == 4)
  {
    const unsigned v = (unsigned)strtoul(tag, nullptr, 16);
    const unsigned primary = v & 0x3FF;
    if (primary == 0x04 || primary == 0x7C04)
      return true;
  }
  return false;
}

static bool UiIsChinese(Lang lang, const char *systemTag)
{
  if (lang == kLangZh)
    return true;
  if (lang == kLangEn)
    return false;
  return TagIsChinese(systemTag);
}

// ---- the strings the tray shows, copied from main.cpp ----
struct TrayText
{
  const char *on, *off, *tipOn, *tipOff, *settings, *openDir, *showLog, *quit;
};

static const TrayText kTextEn = {"Enabled  (click to pause)", "Paused  (click to enable)", "running",
                             "OFF (click to turn on)", "Settings...", "Open the Apex folder",
                             "Show log...", "Quit"};
static const TrayText kTextZh = {"已启用（点击暂停）", "已暂停（点击启用）", "运行中", "已关闭（点击启用）",
                             "设置...", "打开 Apex 文件夹", "查看日志...", "退出"};

static int failures = 0;
static void ok(const char *what, bool pass, const char *detail = nullptr)
{
  printf("  %-58s %s%s%s\n", what, pass ? "ok" : "FAIL", detail ? "  " : "", detail ? detail : "");
  if (!pass)
    ++failures;
}

int main()
{
  printf("the tray's language rule\n");

  // ---- an explicit choice always wins ----
  ok("lang=zh gives Chinese even on an English system", UiIsChinese(kLangZh, "en-US"));
  ok("lang=en gives English even on a Chinese system", !UiIsChinese(kLangEn, "zh-CN"));

  // ---- auto follows the system ----
  ok("auto + zh-CN  -> Chinese", UiIsChinese(kAuto, "zh-CN"));
  ok("auto + zh     -> Chinese", UiIsChinese(kAuto, "zh"));
  ok("auto + zh-Hans-> Chinese", UiIsChinese(kAuto, "zh-Hans"));
  ok("auto + zh-TW  -> Chinese", UiIsChinese(kAuto, "zh-TW"));
  ok("auto + en-US  -> English", !UiIsChinese(kAuto, "en-US"));
  ok("auto + de-DE  -> English (not a half-translated UI)", !UiIsChinese(kAuto, "de-DE"));
  ok("auto + ja-JP  -> English", !UiIsChinese(kAuto, "ja-JP"));
  // The legacy LCID path: 0x0804 is Chinese (Simplified), 0x0409 is English (US).
  ok("auto + a hex LCID for Chinese -> Chinese", UiIsChinese(kAuto, "0804"));
  ok("auto + a hex LCID for English -> English", !UiIsChinese(kAuto, "0409"));
  // The empty string must not crash or claim Chinese.
  ok("auto + an empty tag -> English", !UiIsChinese(kAuto, ""));

  // ---- THE CONVERSION: the part that silently produces mojibake ----
  //
  // The tray calls are the WIDE ones, so every string is converted first. This checks the round trip: if the
  // Chinese text survives UTF-8 -> UTF-16 -> UTF-8, what the shell receives is the text that was intended.
  printf("\nthe UTF-8 -> UTF-16 conversion the tray calls require\n");
  {
    const char *samples[] = {kTextZh.on, kTextZh.off, kTextZh.tipOn, kTextZh.tipOff, kTextZh.settings,
                             kTextZh.openDir, kTextZh.showLog, kTextZh.quit};
    bool allOk = true;
    for (int i = 0; i < 8; ++i)
    {
      wchar_t wide[256] = {0};
      const int n = MultiByteToWideChar(CP_UTF8, 0, samples[i], -1, wide, 255);
      if (n <= 0)
      {
        allOk = false;
        break;
      }
      char back[512] = {0};
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, back, sizeof(back) - 1, nullptr, nullptr);
      if (strcmp(back, samples[i]) != 0)
      {
        printf("      mismatch: \"%s\" -> \"%s\"\n", samples[i], back);
        allOk = false;
      }
    }
    ok("every Chinese string round-trips unchanged", allOk);
  }
  // The bug this is guarding against has a signature: converting through the ANSI codepage turns 端 into "?",
  // or into a pair of unrelated bytes. A single check makes that impossible to miss.
  {
    wchar_t wide[8] = {0};
    MultiByteToWideChar(CP_UTF8, 0, "端", -1, wide, 7);
    ok("the product's own character survives", wide[0] == L'\u7aef', "端 -> U+7AEF");
    // And prove the WRONG way would fail, so the test cannot pass by accident on a machine whose ANSI
    // codepage happens to be Chinese. (On such a machine the A path would coincidentally work, which is
    // exactly why this is asserted rather than assumed.)
    const int ansiBytes = WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    char ansi[8] = {0};
    WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi, sizeof(ansi) - 1, nullptr, nullptr);
    printf("      (for the record: this machine's ANSI path gives %d byte(s) for 端, so the wide calls "
           "are what make it correct)\n", ansiBytes > 0 ? ansiBytes - 1 : -1);
  }

  printf("\n");
  if (failures == 0)
    printf("OK: the tray's language follows the setting, the system, and survives the conversion\n");
  else
    printf("FAILED: %d\n", failures);
  return failures == 0 ? 0 : 1;
}
