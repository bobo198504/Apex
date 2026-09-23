// ---------------------------------------------------------------------------
// THE SETTINGS PROTOCOL, HOST SIDE.
//
// Answers the panel's requests and applies its changes. Everything it touches -- the host config, the
// loader, the features -- is reached through the same functions the rest of the host uses, so the panel
// cannot do anything the tray could not.
//
// THE ONE CONSTRAINT: nothing here may block. It runs on the host's message loop, which is also where the
// tray lives, so a slow answer shows up as an unresponsive tray icon. It can never cost a wheel: the hook
// and the engine are on their own threads and never wait for this.
// ---------------------------------------------------------------------------

#include "settings_ipc.h"
#include "host.h"
#include "hostconfig.h"
#include "loader.h"
#include "paths.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

namespace apex {
namespace host {

// The state and the actions below come from main.cpp; they are declared once in host.h, which both files
// include, so there is no second list to keep in step.

namespace {

// ---------------------------------------------------------------------------
// A FILE NAME FROM THE PAGE, TURNED INTO A PATH INSIDE THE FEATURE'S OWN FOLDER -- OR NOTHING.
//
// ⚠️ WHY A FEATURE GETS TO NAME ITS SETTINGS FILE AT ALL: the ABI fixes the FOLDER a feature owns
// (`Plugins/<id>/`) and says nothing about a file name, so the host cannot guess. Its fallback is the
// `<id>.ini` convention, which is right for a feature that ships one -- and for AutoIME, whose settings are
// `config.json`, it opened nothing at all. So the feature says the name (apex/abi.h, `settingsFile`) and the
// page passes it back with the request.
//
// ⚠️ AND THE NAME IS CHECKED, BECAUSE IT ARRIVES FROM A WEB PAGE. It must be a NAME: a separator, a drive
// letter or a ".." would let a document reach out of the folder it owns, and the host -- not the page -- is
// what decides where a feature's files may be.
// ---------------------------------------------------------------------------
bool FileInFeatureFolder(const char *id, const char *name, char *out, int outSize)
{
  if (!name || !*name || strlen(name) > 120)
    return false;
  for (const char *p = name; *p; ++p)
    if (*p == '\\' || *p == '/' || *p == ':')
      return false;
  if (strstr(name, ".."))
    return false;
  char dir[512] = {0};
  if (!FeatureDir(id, dir, (int)sizeof(dir)))
    return false;
  return JoinPath(dir, name, out, outSize);
}

// ---------------------------------------------------------------------------
// JSON, BUILT BY HAND
//
// A settings panel is where arbitrary text arrives: a blacklist entry is whatever a user pastes. If an exe
// name containing a quote or a backslash could break the document, the panel would fail to parse and go
// blank -- so every string that comes from outside goes through StrBytes, which escapes what JSON requires
// (quote, backslash, control characters) and passes everything else through byte for byte.
//
// UTF-8 IS NOT TOUCHED. The document is served to a web page and both sides are UTF-8 already; converting
// it here is how 端 becomes mojibake.
// ---------------------------------------------------------------------------
struct Json
{
  char *buf;
  int size;
  int off = 0;
  bool truncated = false;

  void Raw(const char *s)
  {
    if (!s)
      return;
    const int n = (int)strlen(s);
    if (off + n >= size)
    {
      truncated = true;
      return;
    }
    memcpy(buf + off, s, (size_t)n);
    off += n;
    buf[off] = 0;
  }

  void Fmt(const char *fmt, ...)
  {
    if (off >= size - 1)
    {
      truncated = true;
      return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + off, (size_t)(size - off), fmt, ap);
    va_end(ap);
    if (n > 0)
    {
      if (off + n >= size)
      {
        truncated = true;
        off = size - 1;
      }
      else
        off += n;
    }
  }

  // A JSON string literal, escaped byte by byte. The only way outside text enters the document.
  void Str(const char *s)
  {
    Raw("\"");
    if (s)
    {
      for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      {
        const unsigned char c = *p;
        if (c == '"')
          Raw("\\\"");
        else if (c == '\\')
          Raw("\\\\");
        else if (c < 0x20)
          Fmt("\\u%04x", c);
        else if (off < size - 1)
        {
          buf[off++] = (char)c;
          buf[off] = 0;
        }
        else
          truncated = true;
      }
    }
    Raw("\"");
  }
};

// "key=value" lines. The panel generates them and this reads them: no JSON parser in the host, which is
// the one place a malformed document must not be possible (it lives beside the input path).
struct Fields
{
  char slot[16] = {0};
  char id[64] = {0};
  char key[32] = {0};
  // ⚠️ 512 -> 1024 FOR THE SAME REASON the panel's own `value` buffer is that size: this field carries a LIST now
  // (the quick panel's order is a comma-separated run of pane keys), and a buffer sized for "dark" would cut it
  // short. A truncated list is not an error anywhere -- it is an order that half worked, which is what the user
  // reported as "快速面板分组不能调顺序" (and see the note where the keys are built: the newline the page used to
  // join them with did the same thing for a different reason).
  char value[1024] = {0};
  // For a list edit (`kIpcListOp`): which operation, and which row for "remove". `value` carries the text
  // for "add" -- the same field the other commands use, because it IS the payload either way.
  char op[16] = {0};
  char index[16] = {0};
};

void CopyToken(const char *from, char *to, int outSize)
{
  if (!from || !to || outSize <= 0)
    return;
  int n = 0;
  for (; *from && *from != '\n' && *from != '\r' && n < outSize - 1; ++from)
    to[n++] = *from;
  to[n] = 0;
}

Fields ParseFields(const char *text)
{
  Fields f;
  if (!text)
    return f;
  const char *p = text;
  while (*p)
  {
    const char *eol = p;
    while (*eol && *eol != '\n')
      ++eol;
    char line[600];
    int n = (int)(eol - p);
    if (n > (int)sizeof(line) - 1)
      n = (int)sizeof(line) - 1;
    memcpy(line, p, (size_t)n);
    line[n] = 0;
    p = *eol ? eol + 1 : eol;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *k = line;
    char *v = eq + 1;
    char *ke = k + strlen(k);
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\r'))
      *--ke = 0;
    while (*v == ' ' || *v == '\r')
      ++v;
    char *ve = v + strlen(v);
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\r'))
      *--ve = 0;

    if (strcmp(k, "slot") == 0)
      CopyToken(v, f.slot, sizeof(f.slot));
    else if (strcmp(k, "id") == 0)
      CopyToken(v, f.id, sizeof(f.id));
    else if (strcmp(k, "key") == 0)
      CopyToken(v, f.key, sizeof(f.key));
    else if (strcmp(k, "value") == 0)
      CopyToken(v, f.value, sizeof(f.value));
    else if (strcmp(k, "op") == 0)
      CopyToken(v, f.op, sizeof(f.op));
    else if (strcmp(k, "index") == 0)
      CopyToken(v, f.index, sizeof(f.index));
  }
  return f;
}

double FieldValue(const Fields &f, double fallback = 0.0)
{
  double d = fallback;
  if (ParseNumber(f.value, &d))
    return d;
  // A switch may be written as a word by hand-editing code paths; accept those too.
  bool b = false;
  if (ParseBool(f.value, &b))
    return b ? 1.0 : 0.0;
  return fallback;
}

// Send a document back to the panel. THE ANSWER TRAVELS THE SAME WAY THE REQUEST DID (see settings_ipc.h):
// the panel's pointer means nothing in this process, so the only correct route is another copied message.
bool SendDocument(HWND panel, unsigned long what, const char *text, int length)
{
  if (!panel || !text || length <= 0)
    return false;

  static IpcDocument doc; // static: 64 KB on the stack is not a good idea on the message thread
  doc.cmd = what;
  if (length > (int)sizeof(doc.text) - 1)
    length = (int)sizeof(doc.text) - 1;
  memcpy(doc.text, text, (size_t)length);
  doc.text[length] = 0;
  doc.length = length;

  COPYDATASTRUCT cds;
  // ⚠️⚠️ `what`, NOT A HARDCODED `kIpcAnswer` -- AND HARDCODING IT HERE IS WHAT ONCE KILLED THE LIVE READOUT.
  //
  // The panel's window procedure tells its replies apart by this code: a document (snapshot or controls) is
  // JSON the page parses, while the read-out was plain prose the page printed. This line ignored the parameter
  // it was handed and stamped EVERY reply as a document, so the read-out arrived looking like JSON: the panel's
  // shape test found no `"host"` in it, called it `controls`, and handed Chinese prose to `JSON.parse` -- which
  // threw, and the read-out was never drawn. That was "点完没有刷新到编辑框内" (the page never learns a capture
  // finished) with a dead read-out panel behind it. `doc.cmd` carried the right value all along; nothing read it.
  //
  // (The read-out itself is gone now -- ABI 9 -> 10 -- but this line stays correct for the two codes that
  // remain, and the lesson is the reason it is spelled out: a reply's TYPE is not decoration.)
  cds.dwData = what;
  cds.cbData = sizeof(IpcDocument);
  cds.lpData = &doc;
  SettingsLog("ipc: replying to %p with %d bytes (code %lu)", (void *)panel, length, what);
  // Sent SYNCHRONOUSLY while the panel is still inside its own SendMessage to us. That is safe and is the
  // reason the panel can treat the exchange as one call: a thread blocked in SendMessage still dispatches
  // messages that are SENT to it, so the panel's window procedure runs, stores the document, and returns
  // before our caller's SendMessage does.
  SendMessageA(panel, WM_COPYDATA, 0, (LPARAM)&cds);
  return true;
}

void ApplySetHost(const Fields &f)
{
  // Logged because a silently-ignored setting is indistinguishable from a delivered one from the panel's
  // side: the panel sends, the host returns OK, and nothing anywhere says the value was not understood.
  SettingsLog("setHost: key=\"%s\" value=\"%s\"", f.key, f.value);
  HostConfig *cfg = SettingsConfig();
  bool trayAffecting = false;
  if (strcmp(f.key, "theme") == 0)
  {
    Theme t;
    if (ParseTheme(f.value, &t))
    {
      cfg->theme = t;
      trayAffecting = true; // the tray icon is chosen by the theme
    }
    else
      SettingsLog("  theme value not understood");
  }
  else if (strcmp(f.key, "lang") == 0)
  {
    Lang l;
    if (ParseLang(f.value, &l))
    {
      cfg->lang = l;
      trayAffecting = true; // so is the tray's own text
    }
    else
      SettingsLog("  lang value not understood");
  }
  else if (strcmp(f.key, "enabled") == 0)
  {
    // GONE, and answered rather than ignored so that an older panel build says something in the log instead
    // of silently doing nothing. There is no host-wide enable any more: a feature owns its own switch, and
    // the panel exposes that on the feature's own page (see `off` in the snapshot). Kept as an explicit
    // branch -- not folded into "unknown host key" -- because the difference between "this panel is old" and
    // "this panel is sending nonsense" is worth having in a log.
    SettingsLog("  \"enabled\" is no longer a host setting (a feature owns its own switch) -- ignored");
  }
  else if (strcmp(f.key, "autostart") == 0)
  {
    bool b;
    if (ParseBool(f.value, &b))
    {
      cfg->autostart = b;
      // ⚠️ APPLIED AT ONCE, WHILE THE SWITCH IS BEING FLIPPED, because it is the one setting whose effect is
      // OUTSIDE this program: a user who turns it on and then kills Apex would otherwise be left with the
      // setting saved and Windows not knowing about it. (The tray does not depend on it, so this is not a
      // `trayAffecting` key -- but it must be on disk, hence the touch.)
      AutostartApply();
      SettingsTouch();
    }
    else
      SettingsLog("  autostart value not understood");
  }
  // THE QUICK PANEL'S TWO SWITCHES. Neither touches the tray, so neither sets `trayAffecting` -- they are read
  // when the flyout is built, which is the next time the user clicks the tray icon.
  else if (strcmp(f.key, "quickcompact") == 0)
  {
    bool b;
    if (ParseBool(f.value, &b))
    {
      cfg->quickCompact = b;
      SettingsTouch();
    }
    else
      SettingsLog("  quickcompact value not understood");
  }
  else if (strcmp(f.key, "quickown") == 0)
  {
    bool b;
    if (ParseBool(f.value, &b))
    {
      cfg->quickOwn = b;
      SettingsTouch();
    }
    else
      SettingsLog("  quickown value not understood");
  }
  // THE ORDER OF THE PANES IN THE QUICK PANEL (see `quickOrder` in hostconfig.h). The page sends the keys it is
  // SHOWING, in their new order, newline-separated -- a drag is one message, not one per row it passed.
  // ⚠️ AN EMPTY VALUE IS NOT "CLEAR THE ORDER": it names nothing, so every saved key is kept, in the order it
  // already had (QuickOrderSetFromKeys). That is the honest reading of "the list I am showing is empty", and it
  // means a reorder can never wipe a pane's place by accident -- which matters here, because a pane is ABSENT
  // from that list whenever its feature is switched off or its own mapping switch is off.
  else if (strcmp(f.key, "quickorder") == 0)
  {
    cfg->QuickOrderSetFromKeys(f.value);
    SettingsLog("  quick panel order: %s", f.value[0] ? f.value : "(unchanged -- nothing was named)");
    SettingsTouch();
  }
  else
    SettingsLog("  unknown host key");

  // THE LANGUAGE AND THE THEME ARE THE SETTINGS A USER CHANGES AND THEN FORGETS ABOUT, so they are exactly the
  // ones that must not be lost to an unclean exit. `trayAffecting` is true for precisely the two keys that took
  // a real value, so it doubles as "something changed here".
  if (trayAffecting)
    SettingsTouch();

  // THE TRAY HAS TO FOLLOW THE PANEL. Both surfaces show the same three settings, and a user who changes the
  // language in the panel and then looks at the tray must see the new one -- without a restart and without
  // having to know that they are two separate implementations of the same idea.
  if (trayAffecting)
    TrayRefreshFromSettings();
}

int ApplySetControl(const Fields &f)
{
  Loader *ld = SettingsLoader();
  const int slot = f.slot[0] ? atoi(f.slot) : -1;
  if (!ld || slot < 0 || slot >= ld->CountAll())
    return kIpcNoSuchSlot;
  const LoadedFeature &lf = ld->Seen(slot);
  if (!lf.ok || !lf.api || !lf.api->setControl)
    return kIpcRejected;

  // ⚠️ THE VALUE GOES THROUGH AS THE TEXT THE PAGE SENT. It used to be converted to a number here
  // (`FieldValue(f)`), which is where a pattern, a name or a chosen option was LOST -- the transport had
  // carried it fine, and this line threw it away before the feature ever saw it. Parsing is the feature's
  // job now: it knows whether a control is a number, and it already parses its own settings file.
  //
  // The ID is bracketed so the feature can reach its own folder if it needs to; the value is applied in
  // memory only -- the file is written by kIpcSave, not here. Writing a file per slider move would be a
  // write for every mouse position.
  SetCurrentFeature(lf.api->id);
  const int took = lf.api->setControl(f.id, f.value);
  SetCurrentFeature(nullptr);
  // TOUCHED, NOT SAVED: a slider sends a value per pixel, so the write is scheduled rather than done here --
  // see SettingsTouch. Called even when the feature REJECTED the value, because "rejected" only means the
  // feature did not take it; it says nothing about whether the file is up to date, and a spurious write costs
  // one file per second at worst.
  if (took)
    SettingsTouch();
  return took ? kIpcOk : kIpcRejected;
}

} // namespace

// ---------------------------------------------------------------------------
// THE SNAPSHOT: everything the panel needs, as one document.
//
// ONE DOCUMENT RATHER THAN MANY CALLS: the panel redraws as a unit, and five separate questions would mean
// five round trips and a window that fills in piece by piece.
// ---------------------------------------------------------------------------
int SettingsBuildSnapshot(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  out[0] = 0;
  Json j;
  j.buf = out;
  j.size = outSize;

  HostConfig *cfg = SettingsConfig();
  Loader *ld = SettingsLoader();
  SetCurrentFeature(nullptr);

  j.Raw("{\"host\":{");
  j.Fmt("\"lang\":\"%s\",", LangName(cfg->lang));
  // ⚠️ NO TRAILING COMMA. The theme used to be followed by "enabled" and then a "skip" array; both are
  // gone, and the two commas they left behind produced `"theme":"auto",,...` -- which JSON.parse rejects
  // outright. The page then reported "the Apex host is not running" while the host answered every message:
  // the document was well-formed enough to be sent and not well-formed enough to be read, which is the one
  // failure the panel cannot tell from a dead host. Caught by opening the page, not by a gate -- there is no
  // gate that parses the snapshot, which is worth remembering.
  j.Fmt("\"theme\":\"%s\"", ThemeName(cfg->theme));
  // ⚠️ AND THE PROGRAM'S OWN VERSION, which the panel shows beside the slogan in the sidebar. It is the HOST's
  // number: each feature reports its own in the list below (they are the versions of the standalone programs
  // those features came from), and mixing the two up would put a plugin's version on Apex itself.
  j.Fmt(",\"version\":\"%s\"", APEX_HOST_VERSION);
  // ⚠️ AND THE SWITCHES THE GENERAL PAGE DRAWS. They live in apex.ini (the host's own settings) and the page
  // learns them the only way it learns anything: from the document. Adding a key here without adding it to the
  // page would be a setting nobody can see; the page having a switch for a key that is not here would be a
  // control that does nothing.
  j.Fmt(",\"autostart\":%d", cfg->autostart ? 1 : 0);
  // ⚠️ AND THE TWO SWITCHES THAT DECIDE WHAT THE QUICK PANEL SHOWS. They are host settings like the rest of
  // this page (the flyout is the host's own window, not a feature's), and the page is the ONLY place they can be
  // changed: the panel itself deliberately has no "settings" affordance, because a flyout that could grow a
  // second page would be the settings window with the sidebar taken away.
  j.Fmt(",\"quickCompact\":%d", cfg->quickCompact ? 1 : 0);
  j.Fmt(",\"quickOwn\":%d", cfg->quickOwn ? 1 : 0);
  // No "enabled": the host has no master switch any more. A feature's own on/off travels per feature, in
  // the list below, where the panel already draws it on that feature's page.
  //
  // No "skip" either: the blacklist is the FEATURE's now (see ApexFeature::listOp in abi.h). It arrives
  // through that feature's own `describe`, with every other control it owns -- which is also why the panel
  // did not need to learn anything new to edit it.

  // What the SYSTEM says, so the panel can show which way "follow the system" resolved rather than leaving
  // the user to guess. (The page's own prefers-color-scheme already follows it for rendering; this is for
  // the label.)
  j.Fmt(",\"systemLight\":%d,", SystemIsLightTheme() ? 1 : 0);
  {
    char tag[64] = {0};
    PreferredUiLanguage(tag, (int)sizeof(tag));
    j.Raw("\"systemLang\":");
    j.Str(tag);
    j.Fmt(",\"systemIsChinese\":%d", LanguageTagIsChinese(tag) ? 1 : 0);
  }
  j.Raw("},");

  // ⚠️⚠️ WHAT THE FLYOUT WILL DRAW, IN THE ORDER IT WILL DRAW IT, AND IT IS SENT READY TO DRAW: the General page's
  // small panel lists exactly this -- one row per PANE, with the pane's own heading -- and it does not have to work
  // the order out for itself. That is deliberate: the alternative is two implementations of "which panes, in what
  // order", and the one the user is looking at would then be able to disagree with the flyout it describes.
  //
  // ⚠️ THE UNIT IS THE PANE, NOT THE FEATURE. The user's own correction: "快速面板的局部功能分组是按单个开关算的，
  // 不是按插件算，所以这边排序要注意" -- a feature's controls are one pane per control (or per named group), so a
  // list of features could not show what the flyout shows, and could not put 亮度 before 音量.
  //
  // ⚠️ AND IT IS COMPUTED FROM THE LIVE FEATURES (host.h `QuickBlocks`), so a pane in here has already been asked
  // whether it exists -- a feature the user has switched off, or one that maps nothing, is simply not in the list.
  // `key` is what the page sends back when the user drags; the two names are the pane's heading so a row can never
  // be nameless and the page never has to join ids.
  j.Raw("\"quickOrder\":[");
  {
    static QuickBlock blocks[kMaxQuickBlocks];
    const int panes = QuickBlocks(blocks, kMaxQuickBlocks);
    for (int k = 0; k < panes; ++k)
    {
      if (k)
        j.Raw(",");
      j.Raw("{\"key\":");
      j.Str(blocks[k].key);
      j.Raw(",\"nameZh\":");
      j.Str(blocks[k].nameZh);
      j.Raw(",\"nameEn\":");
      j.Str(blocks[k].nameEn);
      j.Raw("}");
    }
  }
  // ⚠️ THE ARRAY'S OWN CLOSER, AND IT IS NOT DECORATION: it was missing for exactly one build, and the document
  // that came out had `"quickOrder":[{...},"features":[` in it -- which `JSON.parse` rejects, so the panel showed
  // "the Apex host is not running" while the host answered every message. That is the failure this file's own
  // note at the top of the snapshot warns about ("there is no gate that parses the snapshot"), and it was found
  // by asking the real host for a snapshot and parsing it by hand. check_apex_persist.sh now does that in earnest.
  j.Raw("],");

  j.Raw("\"features\":[");
  const int n = ld ? ld->CountAll() : 0;
  for (int i = 0; i < n; ++i)
  {
    const LoadedFeature &f = ld->Seen(i);
    if (i)
      j.Raw(",");
    j.Fmt("{\"slot\":%d,\"ok\":%d,", i, f.ok ? 1 : 0);
    if (f.ok && f.api)
    {
      j.Raw("\"id\":");
      j.Str(f.api->id ? f.api->id : "");
      j.Raw(",\"nameZh\":");
      j.Str(f.api->nameZh ? f.api->nameZh : "");
      j.Raw(",\"nameEn\":");
      j.Str(f.api->nameEn ? f.api->nameEn : "");
      j.Raw(",\"version\":");
      j.Str(f.api->version ? f.api->version : "");
      // `off` is the user's choice, `enabled` is the feature's own switch: two different things, and the
      // panel shows both because "I turned it off" and "it turned itself off" are not the same state.
      j.Fmt(",\"off\":%d", cfg->FeatureOff(f.api->id) ? 1 : 0);
      j.Fmt(",\"enabled\":%d",
            (f.api->flags && (f.api->flags() & APEX_FEATURE_ENABLED)) ? 1 : 0);
    }
    else
    {
      // A FAILED feature is still listed: a user who sees their feature missing has no way to tell "not
      // installed" from "would not load", and a row with the reason is that difference.
      j.Raw("\"id\":\"\",\"nameZh\":\"\",\"nameEn\":\"\",\"version\":\"\",\"off\":0,\"enabled\":0,\"why\":");
      j.Str(f.why);
      j.Raw(",\"folder\":");
      j.Str(f.dir);
    }
    j.Raw("}");
  }
  j.Raw("]}");

  if (j.truncated)
    SettingsLog("settings: the snapshot was truncated at %d bytes -- raise kIpcMaxDocument", j.off);
  return j.off;
}

int SettingsBuildDescribe(int slot, char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  out[0] = 0;
  Loader *ld = SettingsLoader();
  if (!ld || slot < 0 || slot >= ld->CountAll())
    return 0;
  const LoadedFeature &f = ld->Seen(slot);
  if (!f.ok || !f.api || !f.api->settingsJson)
    return 0;

  SetCurrentFeature(f.api->id);
  const int n = f.api->settingsJson(out, outSize);
  SetCurrentFeature(nullptr);
  return n;
}

// ---------------------------------------------------------------------------
// THE HANDLER
// ---------------------------------------------------------------------------
LRESULT SettingsIpc(HWND sender, const COPYDATASTRUCT *cds)
{
  // WHO THE PANEL IS, learned for free: every request carries the sender's own window (that is where the reply
  // goes). The host needs it for the activity relay, which must not go looking for a window -- see the note on
  // g_panelWnd in main.cpp.
  SettingsPanelSeen(sender);

  if (!cds || !cds->lpData || cds->cbData < sizeof(IpcRequest))
    return kIpcBadRequest;

  const IpcRequest *req = (const IpcRequest *)cds->lpData;
  const Fields f = ParseFields(req->text);
  Loader *ld = SettingsLoader();
  const int slot = f.slot[0] ? atoi(f.slot) : -1;

  switch (req->cmd)
  {
  case kIpcSnapshot:
  {
    static char doc[kIpcMaxDocument];
    const int n = SettingsBuildSnapshot(doc, (int)sizeof(doc));
    SendDocument(sender, kIpcAnswer, doc, n);
    return kIpcOk;
  }
  case kIpcDescribe:
  {
    static char doc[kIpcMaxDocument];
    const int n = SettingsBuildDescribe(slot, doc, (int)sizeof(doc));
    if (n <= 0)
      return kIpcNoSuchSlot;
    SendDocument(sender, kIpcAnswer, doc, n);
    return kIpcOk;
  }
  case kIpcSetControl:
    return ApplySetControl(f);

  case kIpcSetHost:
    ApplySetHost(f);
    return kIpcOk;

  // A LIST A FEATURE OWNS, edited by the panel's generic list control. THE HOST DOES NOT LOOK INSIDE: it
  // finds the feature, hands the request over, and returns what the feature said. That is what keeps a
  // blacklist (or a hotkey list, or a whitelist, or any other list a future feature has) out of the host --
  // and it is why this one case replaces the two blacklist-specific ones that used to live here.
  case kIpcListOp:
  {
    if (!ld || slot < 0 || slot >= ld->CountAll())
      return kIpcNoSuchSlot;
    const LoadedFeature &lf = ld->Seen(slot);
    if (!lf.ok || !lf.api || !lf.api->listOp)
      return kIpcRejected;
    const int index = f.index[0] ? atoi(f.index) : -1;
    SetCurrentFeature(lf.api->id); // the feature may want its own folder (see FeatureScope in main.cpp)
    const int changed = lf.api->listOp(f.id, f.op, f.value, index);
    SetCurrentFeature(nullptr);
    // A list row is worth persisting for the same reason a slider is -- and a list is edited one row at a
    // time, so this is the case where losing the write would lose a whole entry rather than a nudge.
    if (changed)
      SettingsTouch();
    // The feature may want to act on the change immediately (its own equivalent of "do not smooth the
    // program I just added"), and it owns that decision -- the host only reports whether it took.
    return changed ? kIpcOk : kIpcRejected;
  }

  // ⚠️ `kIpcLive` USED TO BE HANDLED HERE -- the command behind a feature's one-line read-out. Both are gone
  // (apex/abi.h, ABI 9 -> 10: the user tried the read-out, then twice said it was not needed). The command
  // number is retired rather than reused, and a request for it now falls to the default branch below and is
  // answered "bad request" -- which is the right answer to a request that has stopped existing. A panel from an
  // older build would show nothing where its read-out was, instead of something else.

  case kIpcFeatureOff:
  {
    if (!ld || slot < 0 || slot >= ld->CountAll())
      return kIpcNoSuchSlot;
    const LoadedFeature &lf = ld->Seen(slot);
    if (!lf.ok || !lf.api || !lf.api->id)
      return kIpcRejected;
    SettingsConfig()->FeatureSetOff(lf.api->id, FieldValue(f, 1.0) != 0.0);
    SettingsTouch(); // the feature's switch is the only "enabled" there is, so it must survive a kill
    // ⚠️ AND THE FLYOUT FOLLOWS AT ONCE IF IT IS UP. The user's rule is that switching a feature off HIDES its
    // local quick-panel controls ("当插件总开关关闭后…其插件的局部快速面板功能要隐藏"), and the flyout rebuilds
    // itself from the config every time it is shown -- so this line is only for the case where it is ALREADY up
    // (the switch can be flipped from the panel while the flyout is on screen: the page is behind it, and the
    // user is looking at both).
    if (QuickPanelVisible())
      QuickPanelRefresh();
    return kIpcOk;
  }
  case kIpcSave:
    SettingsSaveAll();
    return kIpcOk;

  case kIpcOpenFile:
  {
    if (!ld || slot < 0 || slot >= ld->CountAll())
      return kIpcNoSuchSlot;
    const LoadedFeature &lf = ld->Seen(slot);
    if (!lf.ok || !lf.api)
      return kIpcRejected;
    // Written first, so the menu item always opens something to edit even on a first run: until the panel
    // saves, the file may not exist yet.
    if (lf.api->saveSettings)
      lf.api->saveSettings();
    char path[600] = {0};
    // The feature's own name for its settings file if the page sent one (apex/abi.h, `settingsFile`), and the
    // `<id>.ini` convention otherwise -- which is what a feature that ships an ini and names nothing gets.
    const bool named = FileInFeatureFolder(lf.api->id, f.value, path, (int)sizeof(path));
    if (!named)
    {
      if (!FeatureConfigPath(lf.api->id, path, (int)sizeof(path)))
        return kIpcBadRequest;
    }
    // ⚠️ AND THE OUTCOME IS LOGGED, because a button whose whole job is to make another program appear has no
    // other way to report that it did not: the page has no error channel, and ShellExecuteA failing (no
    // association, a file that is not there) returns a number nobody looks at. "I clicked it and nothing
    // happened" is exactly the report this line turns into a diagnosis.
    const HINSTANCE opened = ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
    SettingsLog("openFile: %s -> %s (%s)", named ? f.value : "(the <id>.ini convention)", path,
                ((INT_PTR)opened > 32) ? "launched" : "FAILED (no association, or the file is not there)");
    return kIpcOk;
  }
  case kIpcQuitHost:
    // The panel asks the host to close, which is how "Quit" in the panel's own UI works without the panel
    // having to know how the host is structured.
    PostMessageA(FindApexHost(), WM_CLOSE, 0, 0);
    return kIpcOk;

  default:
    return kIpcBadRequest;
  }
}

} // namespace host
} // namespace apex
