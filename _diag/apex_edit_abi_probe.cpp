// Drive the REAL feature in the REAL host through the REAL IPC, and print what comes back.
//
// ⚠️ THIS EXISTS BECAUSE I COULD NOT GET THE ANSWER BY READING. The user reported twice that edit/save did not
// work; the page's own probe passed twice, because a DOM stub does not have the one participant that matters --
// the host, the feature and the messages between them. This probe is that missing participant's side: it speaks
// the protocol by hand and prints the documents, so the answer comes from the running program rather than from
// my reading of it.
//
// ⚠️ IT DOES NOT CLICK ANYTHING. Every request here is one the panel already sends on the user's behalf, and no
// input device is touched. The capture op is deliberately NOT driven (it waits for a real mouse click).
//
// ⚠️⚠️ IT REFUSES TO RUN AGAINST A REAL INSTALLATION, AND THAT GUARD IS HERE BECAUSE IT WAS NEEDED.
//
// This probe writes: it asks a feature to edit a rule and commit it, which is the only way to prove the save
// reaches the FILE. The first version was pointed at the user's live folder, and it left damage twice over:
//
//   * its "restore" wrote the old name back with `_snprintf("%s")`, which is ANSI -- so a rule called
//     "Lertaro.App.exe / 2026/1/2 Fri 03:04" came back with the Chinese replaced by garbage;
//   * and because the HOST WAS STILL RUNNING with the old rules in memory, the commit wrote ITS copy of the
//     config back over the file -- so a restore made in another window was undone by the program itself.
//
// The lesson is the project's own, written down after the deploy did the same thing: A DIAGNOSTIC RUNS FROM A
// COPY. The path must name a scratch folder (the "_abi_run" convention every other probe here uses), and the
// caller is expected to make one -- see test/check_feature_edit.sh, which does.
//
// usage: apex_edit_abi_probe.exe <scratch-install-dir> <slot> [original-name-to-restore]
#include "settings_ipc.h"

#include <stdio.h>
#include <string.h>

using namespace apex;

static char g_answer[kIpcMaxDocument];
static int g_answerLen = 0;

static LRESULT CALLBACK ProbeWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_COPYDATA)
  {
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)l;
    if (cds && cds->dwData == kIpcAnswer && cds->lpData)
    {
      const IpcDocument *d = (const IpcDocument *)cds->lpData;
      const int n = (cds->cbData >= sizeof(IpcDocument))
                        ? d->length
                        : (int)cds->cbData - (int)offsetof(IpcDocument, text);
      if (n > 0 && n < (int)sizeof(g_answer))
      {
        memcpy(g_answer, d->text, (size_t)n);
        g_answer[n] = 0;
        g_answerLen = n;
      }
    }
    return 1;
  }
  return DefWindowProcA(h, m, w, l);
}

// One request; waits up to 3 s for the answer and returns its length (0 = none).
static int Ask(IpcClient &ipc, unsigned long cmd, const char *text)
{
  g_answerLen = 0;
  g_answer[0] = 0;
  const unsigned long rc = ipc.Send(cmd, text);
  for (int i = 0; i < 300 && g_answerLen == 0; ++i)
  {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    Sleep(10);
  }
  (void)rc;
  return g_answerLen;
}

// One request whose answer is the host's STATUS CODE, not a document.
//
// ⚠️⚠️ WHY THIS EXISTS, AND WHAT IT COSTS TO FORGET: the host answers a `listOp`, a `setControl` or a `setHost`
// with a return code and sends NO document -- only a snapshot or a describe carries one. `Ask` above waits up to
// 3 s for a document, so every one of those calls burned its whole timeout: nineteen round trips, about a dozen
// of them document-less, and the gate that drives them took 47.6 s of which ~36 s was this wait. The trace said
// it plainly (`answer: 0 bytes` on exactly those calls) -- a probe that is slow for a reason nobody looked at.
//
// ⚠️ THE STATUS IS WORTH PRINTING TOO: `kIpcOk` is 0, and a refusal (a bad request, a locked row, a missing
// draft) comes back as a non-zero code here rather than as silence. `Tell` returns it so a caller that cares can
// say so.
static unsigned long Tell(IpcClient &ipc, unsigned long cmd, const char *text)
{
  return ipc.Send(cmd, text);
}

// The value of one field of the Nth item in the document, so the probe can print a rule rather than a wall.
// Crude on purpose: this is a diagnostic, and a JSON parser here would be a second implementation of the
// document format (see the note about that in apex/abi.h).
static const char *FindItemStart(const char *doc, int index)
{
  const char *p = strstr(doc, "\"items\":[");
  if (!p)
    return nullptr;
  p = strchr(p, '{');
  for (int i = 0; i < index && p; ++i)
  {
    p = strchr(p + 1, '{');
    // skip nested objects: find the item boundaries by depth
    int depth = 1;
    const char *q = p;
    while (q && depth > 0)
    {
      ++q;
      if (*q == '{') ++depth;
      else if (*q == '}') --depth;
    }
    p = q ? strchr(q, '{') : nullptr;
  }
  return p;
}

// HOW MANY ITEMS THE DOCUMENT HOLDS. The group document has exactly one `"title"` per item (see rule_item in
// the feature), so counting the key counts the rows. Crude, and deliberately so -- see FindItemStart.
static int CountItems(const char *doc)
{
  int n = 0;
  for (const char *p = doc; (p = strstr(p, "\"title\":\"")) != nullptr; ++p)
    ++n;
  return n;
}

// The display index of the LAST item with this title, or -1. This is the question the PAGE had to answer when
// it decided which row a new rule landed on -- and it used to answer "the last one" (see the Add button in
// panel.html). Comparing that guess with what the FEATURE says is what turns it into something measurable.
static int ItemIndexByTitle(const char *doc, const char *title)
{
  char needle[160] = {0};
  _snprintf(needle, sizeof(needle), "\"title\":\"%s\"", title);
  const char *items = strstr(doc, "\"items\":[");
  if (!items)
    return -1;
  const char *last = nullptr;
  for (const char *p = items; (p = strstr(p, needle)) != nullptr; ++p)
    last = p;
  if (!last)
    return -1;
  int index = 0;
  for (const char *q = strstr(items, "\"title\":\""); q && q < last; q = strstr(q + 1, "\"title\":\""))
    ++index;
  return index;
}

// THE ROW THE FEATURE SAYS IS OPEN FOR EDITING (`editing` in the group document), or -2 when the document does
// not carry the field at all -- which is worth telling apart from -1, because "nothing is open" and "this
// feature cannot say" want different conclusions.
static int EditingRow(const char *doc)
{
  const char *p = strstr(doc, "\"editing\":");
  if (!p)
    return -2;
  return atoi(p + strlen("\"editing\":"));
}

// A file read into a caller-supplied buffer (NUL-terminated). Returns the length, or -1.
static int ReadTextFile(const char *path, char *out, int cap)
{
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return -1;
  const size_t n = fread(out, 1, (size_t)cap - 1, fp);
  fclose(fp);
  out[n] = 0;
  return (int)n;
}

// How many RULES the config file holds: one `match_target` per rule and nowhere else in the file.
static int CountFileRules(const char *path)
{
  static char body[65536];
  if (ReadTextFile(path, body, (int)sizeof(body)) < 0)
    return -1;
  int n = 0;
  for (const char *p = body; (p = strstr(p, "\"match_target\"")) != nullptr; ++p)
    ++n;
  return n;
}

// Wait until the config file's rule count reaches/exceeds (or drops to/below) a number, or give up.
//
// ⚠️ A CONDITION, NOT A SLEEP -- the project's rule, and this is where it cost the most: the host debounces a
// write by one second (main.cpp, APEXWM_SAVE_TIMER), and the probe used to `Sleep(2500)` twice for "the write has
// landed by now". That is 5 s of every run spent hoping, and on a busy machine it is still only a hope. Polling
// returns as soon as the file says so.
static bool WaitForFileRules(const char *path, int want, bool atLeast, int seconds)
{
  for (int i = 0; i < seconds * 10; ++i)
  {
    const int n = CountFileRules(path);
    if (atLeast ? (n >= want) : (n <= want))
      return true;
    Sleep(100);
  }
  return false;
}

static void PrintField(const char *item, const char *field)
{
  char needle[64] = {0};
  _snprintf(needle, sizeof(needle), "\"%s\":\"", field);
  const char *p = strstr(item, needle);
  if (!p)
  {
    printf("      %-22s (not present)", field);
    return;
  }
  p += strlen(needle);
  const char *e = strchr(p, '"');
  if (!e)
    return;
  printf("      %-22s %.*s", field, (int)(e - p), p);
}

// ⚠️⚠️ ARGC IS CHECKED BEFORE argv[4] IS TOUCHED -- AND THAT GUARD IS HERE BECAUSE ITS ABSENCE CRASHED.
//
// The probe grew a fourth argument (the feature id to look for) after the first three, and `argv[4]` was read
// with only a check for argc >= 3. Run with three arguments it read past the end of the array and died with
// status 139 printing NOTHING -- so the gate that runs it reported "the probe could not drive the program",
// with no output to say why. The feature id now has a default (the one this probe is for), so three arguments
// is a complete invocation.
static const char *FeatureId(int argc, char **argv)
{
  return (argc > 4 && argv[4] && argv[4][0]) ? argv[4] : "AutoIME";
}

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    printf("usage: apex_edit_abi_probe.exe <scratch-install-dir> <slot> [feature-id]\n");
    return 2;
  }
  const char *dir = argv[1];
  const int askedSlot = atoi(argv[2]);
  int wantSlot = askedSlot;
  const char *wantId = FeatureId(argc, argv);

  // ⚠️ THE GUARD. A scratch folder is what makes this safe to run at all: the caller copies the built program
  // into one and points this at that copy, so the damage and the restore both stay inside a folder nobody
  // uses. See the long note at the top for what happened when it was not required.
  if (!strstr(dir, "_abi_run"))
  {
    printf("REFUSING: %s does not look like a scratch copy (expect a path containing \"_abi_run\").\n", dir);
    printf("          This probe WRITES to the installation it is pointed at -- see the note at the top of\n");
    printf("          %s. Copy the built folder somewhere temporary and point this at that.\n", __FILE__);
    return 2;
  }

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = ProbeWndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "ApexEditAbiProbeWnd";
  RegisterClassA(&wc);
  HWND self = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance,
                              nullptr);
  if (!self)
  {
    printf("could not create a window\n");
    return 1;
  }

  // The host of THAT folder, found the way the panel finds it (see settings_ipc.h): by class AND by the
  // executable that owns it.
  IpcClient ipc;
  ipc.Attach(self);
  (void)dir;

  char req[256] = {0};

  // ⚠️⚠️ THE SLOT IS FOUND, NOT ASSUMED -- and assuming it is exactly what this probe did first.
  //
  // Features are numbered by FOLDER NAME, so the slot depends on what else is loaded. A release build carries
  // AutoIME and SmoothWheel; a development build also carries AuditStub, which sorts first and pushes AutoIME to
  // slot 1. Asking slot 0 then talks to AuditStub: the probe got a 222-byte document (one bool) back, could not
  // find an item in it, and reported nothing at all -- which reads as "the host did not answer".
  //
  // The project already has this rule written down for the gate that needed it first (see check_reaper_note.sh,
  // "THE SLOT IS FOUND, NOT ASSUMED"). It is found by asking for the snapshot, which lists every feature with
  // its id AND its slot.
  printf("== 0. which slot is the feature in? ==\n");
  if (Ask(ipc, kIpcSnapshot, nullptr) == 0)
  {
    printf("   NO ANSWER -- the host is not running from that folder\n");
    return 1;
  }
  {
    // The snapshot carries `{"slot":N,...,"id":"..."}`. Crude on purpose: this is a diagnostic, and a JSON
    // parser here would be a second implementation of the document format.
    char needle[64] = {0};
    _snprintf(needle, sizeof(needle), "\"id\":\"%s\"", wantId);
    const char *p = strstr(g_answer, needle);
    if (!p)
    {
      printf("   no feature with that id in the snapshot: %.200s\n", g_answer);
      return 1;
    }
    // Walk back to the nearest "slot": before it.
    const char *s = p;
    while (s > g_answer && strncmp(s, "\"slot\":", 7) != 0)
      --s;
    if (strncmp(s, "\"slot\":", 7) == 0)
    {
      const int found = atoi(s + 7);
      printf("   %s is in slot %d (asked for %d)\n", wantId, found, askedSlot);
      wantSlot = found;
    }
    else
    {
      printf("   found the id but not its slot: %.200s\n", g_answer);
      return 1;
    }
  }

  printf("== 1. the document the page starts from ==\n");
  _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
  if (Ask(ipc, kIpcDescribe, req) == 0)
  {
    printf("   NO ANSWER -- the host is not running from that folder, or the feature is not loaded\n");
    return 1;
  }
  const char *item0 = FindItemStart(g_answer, 0);
  if (item0)
  {
    printf("   item 0:\n");
    PrintField(item0, "name");
    PrintField(item0, "process_pattern");
  }

  // ⚠️⚠️ AND WHETHER AN ACTION IS STILL WAITING IS PART OF THAT DOCUMENT (`waiting`, apex/abi.h; ABI 10 -> 11).
  //
  // The page polls while it is set and stops when it is gone -- that is the ONLY thing that makes a capture's
  // result appear on the page without the user touching the row. The page used to guess it from "the document
  // changed", and every `listOp` is followed by a snapshot whose re-read changed the document, so the wait ended
  // at arming time: "捕获事件进行时，鼠标点击后结果要马上给到参数页，目前没有，要等到点击新建的规则条才会出现."
  //
  // ⚠️ THIS ONLY READS. Arming a capture from here is deliberately NOT done: the feature's armed flag is a NAMED,
  // SESSION-WIDE event (win32.rs), so a probe that armed one would quietly stop click matching inside whatever
  // Apex the user happens to be running. The flag's other state is covered by the feature's own tests, which set
  // the state directly instead of through the event.
  printf("   THE WAIT FLAG: in the document: %s\n",
         strstr(g_answer, "\"waiting\"") ? "YES" : "NO");

  printf("\n== 2. begin-edit on display index 0 ==\n");
  _snprintf(req, sizeof(req), "slot=%d\nid=rules\nop=begin-edit\nvalue=\nindex=0\n", wantSlot);
  const unsigned long n2 = Tell(ipc, kIpcListOp, req);
  printf("   answer: %lu bytes: %.120s\n", n2, g_answer);

  printf("\n== 3. the document while editing (does the draft show?) ==\n");
  _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
  Ask(ipc, kIpcDescribe, req);
  const char *itemE = FindItemStart(g_answer, 0);
  if (itemE)
  {
    printf("   item 0:\n");
    PrintField(itemE, "name");
    PrintField(itemE, "process_pattern");
  }

  printf("\n== 4. write a field through setControl ==\n");
  // ⚠️ THE PATH GOES IN THE FIELD CALLED `id` -- that is the wire name the host reads (see ApplySetControl and
  // the panel's own sender in ui_webview.cpp: `path` on the page becomes `id=` in the payload). Sending the
  // value without it, or naming the field `path` here, produces a request the host ignores -- which looks
  // exactly like a feature that did not take the edit.
  _snprintf(req, sizeof(req), "slot=%d\nid=rules[0].name\nvalue=PROBE-EDITED\n", wantSlot);
  const unsigned long n4 = Tell(ipc, kIpcSetControl, req);
  printf("   answer: %lu bytes: %.120s\n", n4, g_answer);

  printf("\n== 5. read it back ==\n");
  _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
  Ask(ipc, kIpcDescribe, req);
  const char *itemW = FindItemStart(g_answer, 0);
  if (itemW)
  {
    printf("   item 0:\n");
    PrintField(itemW, "name");
  }

  printf("\n== 6. commit-edit, then read again (the edit must be IN THE RULE) ==\n");
  _snprintf(req, sizeof(req), "slot=%d\nid=rules\nop=commit-edit\nvalue=\nindex=0\n", wantSlot);
  const unsigned long n6 = Tell(ipc, kIpcListOp, req);
  printf("   answer: %lu bytes\n", n6);
  _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
  Ask(ipc, kIpcDescribe, req);
  const char *itemK = FindItemStart(g_answer, 0);
  if (itemK)
  {
    printf("   item 0 after commit:\n");
    PrintField(itemK, "name");
  }

  printf("\n== 7. and the FILE (the half the panel cannot see) ==\n");
  {
    char path[512] = {0};
    _snprintf(path, sizeof(path), "%s\\Plugins\\AutoIME\\config.json", dir);
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
      printf("   could not open %s\n", path);
    }
    else
    {
      static char body[65536];
      const size_t n = fread(body, 1, sizeof(body) - 1, fp);
      body[n] = 0;
      fclose(fp);
      const char *hit = strstr(body, "PROBE-EDITED");
      printf("   %s contains PROBE-EDITED: %s\n", path, hit ? "YES" : "NO");
      if (!hit)
        printf("   >>> THE SAVE DID NOT REACH THE FILE\n");
    }
  }

  // ---------------------------------------------------------------------------
  // 8+. ADD AND REMOVE -- THE TWO OPS THE USER SAID DO NOT WORK, MEASURED.
  //
  // ⚠️ WHY THIS SECTION EXISTS: "the rules page's add does nothing" is a report that can be true in three
  // different places -- the row never appears, the row appears but is not saved, or it IS saved but on the wrong
  // row -- and those want three different fixes. Reading the code cannot tell them apart, because all three look
  // like "the page sent a message". So each one is measured here, in the order the user meets them.
  //
  // ⚠️ AND IT WAITS FOR THE HOST'S DEBOUNCE. A list op is persisted by `SettingsTouch` -- a timer, not a write
  // (main.cpp) -- so a check that reads the file immediately after the op is checking the wrong moment. The
  // wait is longer than the debounce on purpose.
  // ---------------------------------------------------------------------------
  {
    char cfgPath[512] = {0};
    _snprintf(cfgPath, sizeof(cfgPath), "%s\\Plugins\\AutoIME\\config.json", dir);

    printf("\n== 8. what the file holds before the add ==\n");
    const int fileBefore = CountFileRules(cfgPath);
    printf("   the file has %d rule(s)\n", fileBefore);

    _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
    Ask(ipc, kIpcDescribe, req);
    const int memBefore = CountItems(g_answer);
    printf("   the document shows %d item(s)\n", memBefore);
    // ⚠️ WHAT THE PAGE BELIEVED, STATED OUT LOUD -- because it is the defect this section exists for. The Add
    // button selected the LAST row of the NEW list (`items.length - 1`), on the assumption that the feature
    // appended there. It does append -- to the FILE -- and the page draws the rules SORTED, so the last row is
    // only the new rule by luck.
    printf("   the page would select index %d for the new rule (the last row of the list)\n", memBefore);

    printf("\n== 9. add (the panel's own request), then wait for the debounced write ==\n");
    // ⚠️ OBSERVING AN ABSENCE NEEDS A WITNESS, OR THE CHECK PROVES NOTHING. The assertion below is "the unsaved
    // rule is NOT in the file" -- and that is only meaningful once the write the debounce was going to make has
    // actually happened. The witness is the file's own modification time: record it, then wait for it to move.
    // (The old `Sleep(2500)` was a guess in BOTH directions: 2.5x the debounce on a fast machine, and on a slow
    // one still too early -- and "too early" here reads as a PASS, which is the worst kind of wrong.)
    FILETIME wroteBefore = {0};
    {
      WIN32_FILE_ATTRIBUTE_DATA fad;
      if (GetFileAttributesExA(cfgPath, GetFileExInfoStandard, &fad))
        wroteBefore = fad.ftLastWriteTime;
    }
    _snprintf(req, sizeof(req), "slot=%d\nid=rules\nop=add\nvalue=\nindex=-1\n", wantSlot);
    const unsigned long nAdd = Tell(ipc, kIpcListOp, req);
    printf("   answer: %lu bytes\n", nAdd);
    for (int i = 0; i < 40; ++i)   // up to 4 s, and it returns as soon as the debounce fires
    {
      WIN32_FILE_ATTRIBUTE_DATA fad;
      if (GetFileAttributesExA(cfgPath, GetFileExInfoStandard, &fad) &&
          CompareFileTime(&fad.ftLastWriteTime, &wroteBefore) != 0)
        break;
      Sleep(100);
    }

    _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
    Ask(ipc, kIpcDescribe, req);
    const int memAfter = CountItems(g_answer);
    // ⚠️ AND IT CAN BE KEPT, FOR LOOKING AT THE PAGE IT BELONGS TO. This probe is the only thing in the project
    // that drives the REAL host and the REAL feature into "a new rule is open for editing", and that state cannot
    // be produced any other way -- the page's own layout cannot be judged from a DOM stub (no layout, no painting),
    // and a hand-written document is how a page gets checked against something the feature never sends.
    //   APEX_DUMP_DOC=<path>  writes this document there; _diag/panel_preview.js renders it.
    if (const char *dump = getenv("APEX_DUMP_DOC"))
    {
      if (FILE *fp = fopen(dump, "wb"))
      {
        fwrite(g_answer, 1, (size_t)g_answerLen, fp);
        fclose(fp);
        printf("   [document written to %s, for panel_preview.js]\n", dump);
      }
    }
    const int newAt = ItemIndexByTitle(g_answer, "新规则");
    const int guess = memAfter - 1;
    const int editing = EditingRow(g_answer);
    printf("   the document now shows %d item(s)\n", memAfter);
    printf("   the added row is at index %d; the page's guess would have been %d\n", newAt, guess);
    printf("   the feature says the row being edited is %d\n", editing);
    printf("   ADD: in memory: %s\n", memAfter > memBefore ? "YES" : "NO");
    // ⚠️ THE CHECK THAT MATTERS: the row opened for editing is the row that was created, whatever position it
    // landed in. Before the feature published this, the page had to guess, and a wrong guess meant the user's
    // typing went into a rule they never chose.
    printf("   ADD: the editor is on the new row: %s\n", (editing == newAt && newAt >= 0) ? "YES" : "NO");
    if (editing != newAt)
      printf("   >>> THE EDITOR IS ON THE WRONG ROW (feature says %d, the new row is %d)\n", editing, newAt);
    if (guess != newAt)
      printf("   (the page's old guess of %d would have opened a different rule -- this is what the fix is for)\n",
             guess);

    const int fileAfterAdd = CountFileRules(cfgPath);
    printf("   the file now has %d rule(s)\n", fileAfterAdd);
    // ⚠️⚠️ "NO" IS THE EXPECTED ANSWER HERE, AND IT IS THE USER'S OWN RULE: "自动输入法插件有个交互优化下：新建规则
    // 后，没点保存，切到其它规则后不保留" -- answered with "没点保存就切走 = 等于没建（不保留）". A rule that `add`
    // has just created is IN MEMORY ONLY until Save, so that walking away from it costs nothing and leaves no
    // empty 新规则 behind (`cancel-edit` rolls it back, and the file never held it).
    //
    // ⚠️ THE OPPOSITE WAS ASSERTED BEFORE THIS, and the reversal is worth stating rather than quietly flipping:
    // the old check demanded that the row reach config.json within the debounce, and it caught a real defect at
    // the time (a new rule that vanished on restart because nothing ever wrote it). What changed is the MODEL, not
    // the bug: the row is still created the moment the user asks for it -- it is just not PUBLISHED until Save,
    // which is what §9b below now checks.
    printf("   ADD: reached the FILE: %s (the model: not until Save)\n", fileAfterAdd > fileBefore ? "YES" : "NO");
    if (fileAfterAdd > fileBefore)
      printf("   >>> THE UNSAVED RULE WAS WRITTEN -- abandoning it would leave a row in config.json\n");
    int fileAfterSave = fileAfterAdd;

    // ⚠️ AND SAVE IS THE THING THAT WRITES IT. Without this step the file check below would be meaningless (a
    // rule that was never written cannot be shown to have been deleted from the file either).
    if (newAt >= 0)
    {
      printf("\n== 9b. commit-edit (Save) -- the ONLY thing that puts a new rule in the file ==\n");
      _snprintf(req, sizeof(req), "slot=%d\nid=rules\nop=commit-edit\nvalue=\nindex=%d\n", wantSlot, newAt);
      const unsigned long nCommit = Tell(ipc, kIpcListOp, req);
      printf("   answer: %lu bytes\n", nCommit);
      // Wait for the WRITE, not past the debounce: Save is what publishes a new rule, and the file says when.
      WaitForFileRules(cfgPath, fileAfterAdd + 1, true, 6);
      fileAfterSave = CountFileRules(cfgPath);
      printf("   the file now has %d rule(s)\n", fileAfterSave);
      printf("   SAVE: reached the FILE: %s\n", fileAfterSave > fileAfterAdd ? "YES" : "NO");
      if (fileAfterSave <= fileAfterAdd)
        printf("   >>> THE SAVED RULE IS NOT IN THE FILE -- Save does not persist a new rule\n");

      // ⚠️ AND THE EDIT IS RE-OPENED, BECAUSE COMMITTING CLOSED IT. The move below checks that an OPEN EDIT follows
      // the row it belongs to; with the draft gone (that is what Save does) there would be nothing to follow and
      // the check would pass for the wrong reason. Re-opening it is also what a user does -- Save, then Edit again.
      _snprintf(req, sizeof(req), "slot=%d\nid=rules\nop=begin-edit\nvalue=\nindex=%d\n", wantSlot, newAt);
      Tell(ipc, kIpcListOp, req);
      _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
      Ask(ipc, kIpcDescribe, req);
      printf("   the editor is open again on row %d: %s\n",
             EditingRow(g_answer), EditingRow(g_answer) == newAt ? "YES" : "NO");
    }

    printf("\n== 10. move: what a drag sends (one message, from -> to) ==\n");
    // ⚠️ THE ROW IS AT THE TOP (the feature puts a new rule first -- see `add`), so dragging it to the BOTTOM is
    // a move the arrows could only have done one place at a time. `value` carries the destination because the op
    // needs two numbers and the call has one numeric slot; both are checked here, because a page that swapped
    // them would ask the feature to move the wrong row and nothing would report it.
    int rowAt = newAt;
    if (rowAt >= 0 && memAfter > 1)
    {
      const int to = memAfter - 1;
      _snprintf(req, sizeof(req), "slot=%d\nid=rules\nop=move\nvalue=%d\nindex=%d\n", wantSlot, to, rowAt);
      const unsigned long nMove = Tell(ipc, kIpcListOp, req);
      printf("   asked to move row %d to row %d (answer %lu bytes)\n", rowAt, to, nMove);
      // ⚠️ NO SLEEP HERE, AND THAT IS THE POINT: `move` is applied to the live config by the op itself (the
      // debounce only decides when the FILE is written), and this section asserts on the DOCUMENT. The answer to
      // the call is the state after it, so the describe below is already looking at the result. The old
      // `Sleep(2500)` here was 2.5 s of nothing.
      _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
      Ask(ipc, kIpcDescribe, req);
      const int nowAt = ItemIndexByTitle(g_answer, "新规则");
      const int stillEditing = EditingRow(g_answer);
      printf("   it is now at index %d (was %d), and the editor says %d\n", nowAt, rowAt, stillEditing);
      printf("   MOVE: the row went where it was dropped: %s\n", nowAt == to ? "YES" : "NO");
      // ⚠️ AND THE OPEN EDIT FOLLOWED IT, which is the half that makes dragging during an edit safe: the draft is
      // keyed by the rule's position in the FILE, and a move only renumbers priorities.
      printf("   MOVE: the editor followed the row: %s\n", stillEditing == nowAt ? "YES" : "NO");
      if (nowAt >= 0)
        rowAt = nowAt;
    }

    printf("\n== 11. remove that row, then wait again ==\n");
    if (rowAt >= 0)
    {
      _snprintf(req, sizeof(req), "slot=%d\nid=rules\nop=remove\nvalue=\nindex=%d\n", wantSlot, rowAt);
      Tell(ipc, kIpcListOp, req);
      // Wait for the file to LOSE the rule (the other direction, same reason as the commit above).
      WaitForFileRules(cfgPath, fileAfterSave - 1, false, 6);
      _snprintf(req, sizeof(req), "slot=%d\n", wantSlot);
      Ask(ipc, kIpcDescribe, req);
      const int memRemoved = CountItems(g_answer);
      const int fileAfterRemove = CountFileRules(cfgPath);
      printf("   the document now shows %d item(s), the file %d\n", memRemoved, fileAfterRemove);
      printf("   REMOVE: in memory: %s\n", memRemoved < memAfter ? "YES" : "NO");
      // The baseline is the file AFTER the save in 9b, not after `add`: the rule was not in the file until it was
      // saved, so comparing against the post-add count would compare two numbers that are equal by design.
      printf("   REMOVE: reached the FILE: %s\n",
             (fileAfterRemove >= 0 && fileAfterRemove < fileAfterSave) ? "YES" : "NO");
      // And the draft went with the row: reading a document for a rule that no longer exists must not claim an
      // editor is open on it.
      printf("   REMOVE: nothing is left open for editing: %s\n", EditingRow(g_answer) == -1 ? "YES" : "NO");
    }
    else
    {
      printf("   no row titled 新规则 was found, so there is nothing to remove\n");
    }
  }

  // ⚠️ THERE IS NO "RESTORE" STEP, AND THAT IS DELIBERATE. The first version restored the name it had changed,
  // and the restore was worse than the change: it wrote the old value back with `_snprintf("%s")` -- ANSI -- so
  // a name containing Chinese came back as garbage. A probe that has to undo its own writes is a probe that can
  // break the thing it was measuring. Callers copy the install into a scratch folder instead (see the guard at
  // the top), and the copy is thrown away with it.

  DestroyWindow(self);
  return 0;
}
