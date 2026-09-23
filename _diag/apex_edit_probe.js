// A standalone reproduction of the edit/save flow against the real page, without the probe's other 80 checks
// in the way. If this passes, the page is right and the probe's sequencing is wrong; if it fails, the page is.
const fs = require("fs");
const path = require("path");

const html = fs.readFileSync(path.join("build", "panel.built.html"), "utf8");
const m = html.match(/<script>([\s\S]*?)<\/script>\s*<\/body>/);
if (!m) { console.error("no script block"); process.exit(1); }

// ---- the smallest DOM stub that can run this page ----
const sent = [];
const elements = {};
function mkEl(id) {
  const e = {
    id, style: { setProperty() {}, removeProperty() {} }, className: "", textContent: "",
    value: "", disabled: false, children: [], _on: {}, _attr: {},
    // ⚠️ `innerHTML = ""` MUST ACTUALLY CLEAR THE CHILDREN -- see the note in apex_panel_probe.js, which
    // worked around this by hand while these probes silently kept the old page's elements alongside the new
    // ones. A check that looks for a button after a redraw then finds the DETACHED one from the draw before,
    // so the page reads as broken while it is working.
    set innerHTML(v) { this._html = v; if (v === "") this.children.length = 0; },
    get innerHTML() { return this._html || ""; },
    // ⚠️ `appendChild` MOVES AN EXISTING CHILD AND `insertBefore` EXISTS, because the page is entitled to
    // re-parent an element. (It did for the live read-out until that was removed; the two lines stay because a
    // stub that is missing a DOM call the page might use turns a working page into a crash -- which is exactly
    // what happened when `insertBefore` was first used.)
    parentNode: null,
    appendChild(c) {
      if (c.parentNode && c.parentNode !== this) {
        const at = c.parentNode.children.indexOf(c);
        if (at >= 0) c.parentNode.children.splice(at, 1);
      }
      c.parentNode = this;
      this.children.push(c);
      return c;
    },
    insertBefore(n, ref) {
      if (n.parentNode && n.parentNode !== this) {
        const was = n.parentNode.children.indexOf(n);
        if (was >= 0) n.parentNode.children.splice(was, 1);
      }
      n.parentNode = this;
      const at = this.children.indexOf(ref);
      if (at < 0) this.children.push(n);
      else this.children.splice(at, 0, n);
      return n;
    },
    addEventListener(t, f) { (this._on[t] = this._on[t] || []).push(f); },
    fire(t, d) { (this._on[t] || []).forEach((f) => f(Object.assign({ type: t, preventDefault() {}, stopPropagation() {} }, d))); },
    setAttribute(k, v) { this._attr[k] = v; }, removeAttribute(k) { delete this._attr[k]; },
    getAttribute(k) { return this._attr[k] || null; }, querySelector() { return null; },
    getContext() { return { save() {}, restore() {}, clearRect() {}, beginPath() {}, moveTo() {}, lineTo() {},
      stroke() {}, setLineDash() {}, fill() {}, fillText() {}, arc() {}, closePath() {} }; },
    get clientWidth() { return 600; }, get clientHeight() { return 130; },
    set src(v) {}, get offsetWidth() { return 600; },
  };
  return e;
}
// ⚠️ THE DOCUMENT KEEPS ITS LISTENERS HERE, AND IT MATTERS: THE HOTKEY RECORDER LISTENS ON THE DOCUMENT as
// well as on its own box, because Ctrl+Space can take the keyboard focus away and the box alone never sees the
// press (that is the user's "Ctrl+Space 录不上"). A stub whose document.addEventListener() throws the listener
// away cannot tell a working recorder from a broken one -- it would pass either way.
const docListeners = {};
global.document = {
  getElementById(id) { return elements[id] || (elements[id] = mkEl(id)); },
  createElement(t) { return mkEl("#" + t.toLowerCase()); },
  documentElement: mkEl("html"),
  addEventListener(t, f, capture) { (docListeners[t] = docListeners[t] || []).push(f); },
  removeEventListener(t, f) {
    const list = docListeners[t] || [];
    const at = list.indexOf(f);
    if (at >= 0) list.splice(at, 1);
  },
};
// One press, delivered the way the browser would: to the document (capture) AND to the element that has the
// focus. The page is expected to handle it once -- see `__apexHotkeySeen`.
function pressDoc(type, d) {
  (docListeners[type] || []).slice().forEach((f) =>
    f(Object.assign({ type: type, target: null, preventDefault() {}, stopPropagation() {} }, d)));
}
global.__pressDoc = pressDoc;
global.window = {
  chrome: { webview: { postMessage(x) { sent.push(x); } } },
  matchMedia() { return { matches: true, addEventListener() {} }; },
  devicePixelRatio: 1, addEventListener() {},
};
global.requestAnimationFrame = () => 0; global.cancelAnimationFrame = () => {};
global.setInterval = () => 0; global.clearInterval = () => {};
global.navigator = {};

// ⚠️ `watch` IS A GETTER, NOT A COPY: the page reassigns `captureWatch` (arming, then stopping), and a snapshot
// of the variable taken here would be stuck on whatever it held at boot -- a check that could neither see a wait
// start nor see it end.
eval(m[1] + ";globalThis.__p = {S:S, controls:window.__apexControls, snapshot:window.__apexSnapshot, renderFeature:renderFeature, t:t, watch:function(){return captureWatch;}};");
const P = globalThis.__p;

// ⚠️ `editing` IS PART OF EVERY DOCUMENT NOW, AND THAT IS WHAT THESE CHECKS TURN ON.
//
// The feature owns the draft, so the feature is the only party that knows which row is open -- it publishes
// that as `editing` (a display index, or -1) and the page DRAWS it. A document without the field means "nothing
// is being edited", which is what `-1` says explicitly. The probe plays the feature's part by supplying it, so
// every "the page shows edit mode" check below is now a check that the page followed the DOCUMENT rather than a
// flag of its own -- which is the whole change.
function doc(layout, n, editing, waiting) {
  const fields = [
    { id: "name", type: "text", labelZh: "名称", labelEn: "Name" },
    { id: "enabled", type: "bool", labelZh: "启用", labelEn: "Enabled" },
  ];
  const items = [];
  for (let i = 0; i < n; i++) items.push({ title: "规则 " + i, values: { name: "规则 " + i, enabled: 1 } });
  const g = { id: "rules", type: "group", labelZh: "规则", labelEn: "Rules", fields, items,
    editing: (editing === undefined ? -1 : editing),
    // ⚠️ `waiting` IS PART OF THE CONTRACT NOW (apex/abi.h, ABI 10 -> 11): a feature that has an action which
    // cannot answer yet says so here, and the page polls until it stops being said. Absent/null means "nothing is
    // being waited for", which is what the feature sends when it is idle -- so the probe sends it the same way.
    waiting: (waiting === undefined ? null : waiting),
    actions: [{ op: "capture", labelZh: "点击捕获", labelEn: "Capture" }] };
  if (layout) g.layout = layout;
  return JSON.stringify({ params: [g] });
}
function walk(node, fn, depth) {
  if (!node || (depth || 0) > 12) return;
  fn(node);
  (node.children || []).forEach((c) => walk(c, fn, (depth || 0) + 1));
}
function first(node, pred) { let hit = null; walk(node, (n) => { if (!hit && pred(n)) hit = n; }); return hit; }
function body() { return elements["body"]; }
// How many `.row` elements the body currently holds -- "is the page drawn at all", cheaply. Used by the check
// that a snapshot (which empties the body for one render) is followed by a page that comes BACK.
function countByClass(node, cls) {
  let n = 0;
  walk(node, (x) => { if ((x.className || "").split(" ").indexOf(cls) >= 0) n++; });
  return n;
}

// ⚠️ WHERE A MESSAGE IS SUPPOSED TO APPEAR: THE QUIET BOX AT THE RIGHT-HAND END OF THE RULES BAR, BESIDE
// 「点击捕获」 -- "消息提示统一移到「点击捕获」按钮右侧，UI采用上方不高亮的提示." The probe finds it by its own class,
// so it is checking the box the user was pointing at rather than a variable the page happens to keep.
function hintBoxText() {
  const n = first(body(), (x) => (x.className || "").split(" ").indexOf("hintbox") >= 0);
  return n ? (n.textContent || "") : null;
}
// ⚠️ THERE IS NO SECOND BOX ANY MORE. Messages used to land either in that box or in the feature's read-out at
// the top of the page, and this probe checked BOTH -- that the message was in the right one and not in the other.
// The read-out is gone from the program (apex/abi.h, ABI 9 -> 10: the user tried it, then said it was not
// needed), so there is one place for a message to be and every check is "is it in the box beside the button".

// The labels of the buttons in the edit/save bar -- ONE button, whose word is the mode indicator ("点完编辑按钮时，
// 它要变成'保存'"). Defined here rather than further down because the first checks already need it.
function editBarLabels() {
  const bar = first(body(), (n) => (n.className || "").split(" ").indexOf("editbar") >= 0);
  if (!bar) return [];
  return (bar.children || []).filter((c) => c.id === "#button").map((c) => c.textContent);
}

// The page needs a snapshot before it will draw a feature page.
P.snapshot(JSON.stringify({ host: { lang: "en", theme: "light", systemLight: 1, systemIsChinese: 0 },
  features: [{ slot: 0, ok: 1, id: "F", nameZh: "F", nameEn: "F", version: "1", off: 0, enabled: 1 }] }));
P.S.view = 0;
P.controls(doc("master", 3));

let failures = 0;
function check(label, ok, detail) {
  console.log((ok ? "  ok   " : "  FAIL ") + label + (detail ? "  (" + detail + ")" : ""));
  if (!ok) failures++;
}

// ---- SAVE, CANCEL AND CAPTURE ARE MESSAGES TO THE FEATURE (it owns the data) ----
//
// ⚠️ WHAT IS CHECKED IS THE MESSAGE, NOT A LOCAL COPY. The draft lives in the feature (see DRAFT in its
// settings.rs), so the page's job is to send the right op for the right rule -- and a test that looked at page
// state would pass while the ops were never sent, which is the failure the user actually hit.
function opsSent(from) {
  return sent.slice(from).filter(function (m) { return m.cmd === "listOp"; });
}
function clickButton(node, text) {
  const b = first(node, (n) => n.id === "#button" && n.textContent === text);
  if (b) b.fire("click");
  return !!b;
}

// EDIT sends begin-edit for the SELECTED rule...
elements["body"].children.length = 0;
P.S.view = 0;
P.S.grpEditing = {};
P.renderFeature(0);
const beforeBegin = sent.length;
check("an Edit button is drawn", clickButton(body(), "Edit"));
const began = opsSent(beforeBegin);
check("  and it asks the feature to begin editing that rule",
      began.some((m) => m.op === "begin-edit" && String(m.index) === "0"), JSON.stringify(began));
// ⚠️ ...AND THE MODE COMES BACK FROM THE FEATURE, NOT FROM THE CLICK. The probe plays the feature's part: the
// document says row 0 is open. Until that arrives the page must NOT show edit mode -- that is the contract, and
// it is exactly the assumption the old page made on its own (and got wrong for `add`).
check("  and the page is still in view mode until the feature says otherwise",
      editBarLabels().length === 1 && editBarLabels()[0] === "Edit", JSON.stringify(editBarLabels()));
P.controls(doc("master", 3, 0));
check("  once the feature says row 0 is open, the page shows edit mode",
      editBarLabels().length === 1 && editBarLabels()[0] === "Save", JSON.stringify(editBarLabels()));
check("  and the page records that as the row the feature has open",
      P.S.grpEditing.rules === 0, JSON.stringify(P.S.grpEditing));

// SAVE sends commit-edit, and the page only claims success when the answer says the row closed.
const beforeSave = sent.length;
check("a Save button appears while editing", clickButton(body(), "Save"));
const committed = opsSent(beforeSave);
check("  and it commits the edit for that rule",
      committed.some((m) => m.op === "commit-edit" && String(m.index) === "0"), JSON.stringify(committed));
// The feature's answer to a commit: the draft is gone, so `editing` is -1 again.
P.controls(doc("master", 3, -1));
check("  and the page goes back to view mode when the feature says the commit landed",
      editBarLabels().length === 1 && editBarLabels()[0] === "Edit", JSON.stringify(editBarLabels()));
check("    and says so, in the box beside 「点击捕获」",
      hintBoxText() === P.t("saved"), JSON.stringify(hintBoxText()));

// ⚠️⚠️ AND A SAVE THAT DID NOT TAKE IS NOT REPORTED AS ONE THAT DID -- the failure the user could not see.
//
// The feature answers `commit-edit` with 0 when it holds no draft for that row, and the OLD page cleared its own
// flag and printed "已保存" regardless: a refused save and a real one looked identical. The document is the
// check, and the page waits for two documents before concluding a failure (a `describe` sent just before the
// commit can still be in flight, and its answer legitimately shows the row open).
P.controls(doc("master", 3, 0));
clickButton(body(), "Save");
P.controls(doc("master", 3, 0)); // the stale answer: the row is still open
P.controls(doc("master", 3, 0)); // and the fresh one agrees -- nothing was committed
check("  a commit the feature refused is NOT reported as saved",
      hintBoxText() === P.t("saveFailed"),
      JSON.stringify(hintBoxText()));

// ---- LEAVING IS THE CANCEL: there is no Cancel button any more ----
//
// "不需要取消按钮，有点保存就保存，没点保存，切走，或关了，就是取消." Two ways to walk away, and both must drop the
// edit: selecting another rule in the list, and leaving the feature's page entirely.
elements["body"].children.length = 0;
P.S.view = 0;
P.S.grpEditing = {};
P.controls(doc("master", 3, -1));
check("there is no Cancel button", !clickButton(body(), "Cancel"));

// (a) selecting another rule drops the edit for the one being left.
P.S.grpSel = { rules: 0 };
elements["body"].children.length = 0;
P.S.view = 0;
P.controls(doc("master", 3, 0)); // the feature has row 0 open
const beforeLeaveRow = sent.length;
const secondRow = (function findRow(node, depth) {
  let seen = 0, hit = null;
  (function walk(n, d) {
    if (!n || d > 12 || hit) return;
    if ((n.className || "").split(" ").indexOf("item") >= 0) {
      if (seen === 1 && n._on && n._on.click) hit = n;
      seen++;
    }
    (n.children || []).forEach((c) => walk(c, d + 1));
  })(node, depth || 0);
  return hit;
})(body(), 0);
if (secondRow) secondRow.fire("click");
const leftRow = opsSent(beforeLeaveRow);
check("  selecting another rule cancels the edit that was open",
      leftRow.some((m) => m.op === "cancel-edit" && String(m.index) === "0"), JSON.stringify(leftRow));
check("  and no field edit was sent", !leftRow.some((m) => m.cmd === "setControl"), JSON.stringify(leftRow));

// (b) leaving the feature's page drops it too -- through the brand entry, which is the click a user makes.
P.S.grpSel = { rules: 0 };
elements["body"].children.length = 0;
P.S.view = 0;
P.controls(doc("master", 3, 0)); // the feature has row 0 open
const beforeLeavePage = sent.length;
const brand2 = elements["brand"];
if (brand2 && brand2._on && brand2._on.click) brand2._on.click.forEach((f) => f({}));
const leftPage = opsSent(beforeLeavePage);
check("  leaving the feature's page cancels the edit too",
      leftPage.some((m) => m.op === "cancel-edit"), JSON.stringify(leftPage));
check("  and the page forgets the edit", !P.S.grpEditing || P.S.grpEditing.rules === undefined,
      JSON.stringify(P.S.grpEditing));

// ---- ⚠️ THE ONE THE USER REPORTED: A CAPTURE MAY ONLY LAND ON THE RULE BEING EDITED ----
//
// "点击捕获下一个点击的目标，功能不对 ... 在新建规则时默认启用，或编辑规则时启用，其它情况下不启用." A capture
// OVERWRITES every pattern of the rule it lands on, so arming it while looking at a rule that is NOT being
// edited would rewrite a working rule from one stray click.
function armOp(node) {
  const b = first(node, (n) => n.id === "#button" && /捕获|Capture/.test(n.textContent || ""));
  if (b) b.fire("click");
  return !!b;
}
const captureOps = (from) => sent.slice(from).filter((m) => m.op === "capture");

// (a) looking at a rule that is not being edited: nothing may be armed.
elements["body"].children.length = 0;
P.S.view = 0;
P.S.grpEditing = {};
P.controls(doc("master", 3, -1));
const beforeArm1 = sent.length;
const hasArmBtn = armOp(body());
check("the capture button is drawn", hasArmBtn);
check("  but nothing is armed while no rule is being edited",
      captureOps(beforeArm1).length === 0, JSON.stringify(captureOps(beforeArm1)));

// (b) while editing, it arms -- for THAT rule.
P.S.grpSel = { rules: 0 };
elements["body"].children.length = 0;
P.S.view = 0;
P.controls(doc("master", 3, 0)); // the feature has row 0 open
const beforeArm2 = sent.length;
armOp(body());
const armed = captureOps(beforeArm2);
check("  and it arms once the rule is being edited", armed.length === 1, JSON.stringify(armed));
check("    aiming at the rule on screen", armed.length === 1 && String(armed[0].index) === "0",
      JSON.stringify(armed));

// (c) ⚠️ AND SWITCHING TO ANOTHER RULE STOPS IT. This is the per-rule property the user reported missing:
// "每条的保存/取消，是独立的，切换规则后不保存". An edit belongs to one rule, so selecting another must leave
// BOTH rules alone until the user acts on the one they are looking at. The document still names row 0 (the
// feature is not told about the selection), so the page must refuse to arm for row 1 on its own.
P.S.grpSel = { rules: 1 };
elements["body"].children.length = 0;
P.S.view = 0;
P.controls(doc("master", 3, 0));
const beforeArm3 = sent.length;
armOp(body());
check("  and selecting a different rule stops the capture",
      captureOps(beforeArm3).length === 0, JSON.stringify(captureOps(beforeArm3)));
check("  and the page does not show that rule as being edited",
      P.S.grpEditing.rules !== 1, JSON.stringify(P.S.grpEditing));

// ---- ADD: THE PAGE FOLLOWS THE ROW THE FEATURE OPENED, WHEREVER IT LANDED ----
//
// ⚠️⚠️ THIS IS THE BUG THE USER REPORTED AS "规则添加 ... 不完善", PINNED.
//
// `add` appends to the FILE, and the page shows the rules SORTED by priority then name -- so a new rule (priority
// 100) lands wherever it sorts, NOT necessarily last. The old page selected `items.length - 1` (the last row)
// and asked the feature to edit it, which opened SOMEBODY ELSE'S RULE: the user typed into the new rule's editor
// and pressed Save, and the fields went onto a rule they never meant to touch.
//
// So the feature opens the draft itself and publishes the row in the document, and this check puts the new rule
// FIRST -- the arrangement in which "last" and "right" are different rows.
P.S.grpSel = { rules: 0 };
elements["body"].children.length = 0;
P.S.view = 0;
P.S.grpEditing = {};
P.controls(doc("master", 3, -1));
const beforeAdd = sent.length;
check("the Add button is in the list bar", clickButton(body(), "Add") || clickButton(body(), "添加"));
check("  and it asks the feature to add a rule",
      opsSent(beforeAdd).some((m) => m.op === "add"), JSON.stringify(opsSent(beforeAdd)));
// The feature's answer: four rules, the new one FIRST, opened for editing. It answers `add` by opening the draft
// (see its list_op) -- the probe plays the document it produces.
const withNew = JSON.parse(doc("master", 3, 0));
withNew.params[0].items.unshift({ title: "新规则", values: { name: "", enabled: 0 } });
P.controls(JSON.stringify(withNew));
check("  the page selects the row the FEATURE opened, not the last one",
      P.S.grpSel.rules === 0, "selected=" + P.S.grpSel.rules + " (last row would be 3)");
check("  and that row is in edit mode", JSON.stringify(editBarLabels()) === '["Save"]',
      JSON.stringify(editBarLabels()));
const newFields = first(body(), (n) => n.id === "#input" && n.type === "text");
check("    with its fields editable right away", !!newFields && !newFields.disabled,
      newFields ? "disabled=" + newFields.disabled : "none");

// ⚠️⚠️ AND IT GOES STRAIGHT INTO CAPTURE MODE: "新规则，默认进入捕获状态."
//
// A new rule is an empty shell, so the first thing its author wants is the control it is about -- and the only
// way to give them that is to arm the capture. The check is the op the feature receives (the arming lives in the
// feature, see its list_op) plus the instruction in the message box, which is what tells the user what to do
// next: a capture that is armed silently would make the next click anywhere a surprise.
const armedByAdd = captureOps(beforeAdd);
check("  and a new rule arms the capture without being asked",
      armedByAdd.some((m) => m.op === "capture" && String(m.index) === "0"), JSON.stringify(armedByAdd));
check("    saying so in the message box beside the button",
      hintBoxText() === P.t("captureArmed"), JSON.stringify(hintBoxText()));
// ⚠️ AND THE WAIT THAT ARM JUST STARTED MUST SURVIVE THE DELIVERY THAT STARTED IT. The arm happens INSIDE the
// draw (`groupCard` ends with `ArmCapture` for a rule the feature just opened), so anything that judges the wait
// at the end of the same delivery judges it against the document that caused the arm -- a document that cannot
// say `waiting` yet, because the feature has not been asked. It would stop the wait on the spot, and only for a
// NEW rule: press 添加, the capture arms, and nothing ever comes back. (This is a bug I wrote and this check is
// what caught it.)
check("    and the wait it started survives the delivery that started it",
      !!P.watch(), JSON.stringify(P.watch()));

// ⚠️⚠️ AND THE PAGE MUST SURVIVE THE SNAPSHOT THAT FOLLOWS EVERY `listOp` -- THE BLACK PANE.
//
// The user's report: "新建规则后，插件面板还是会变黑，直到鼠标点击完成捕获后它才显示。经测试，就算是改其它规则，只要
// 进入捕获状态，面板就消失."
//
// ⚠️ THE CAUSE TURNED OUT TO BE ONE LINE, and it is the reason this section asserts on the SNAPSHOT ALONE.
// `loadFeature` cleared `S.controls` whenever it was asked to re-read (`force`), and `__apexSnapshot` re-reads the
// page it is already on after EVERY `listOp`. Clearing the document means drawing once with nothing: an empty
// `#body`, which in the dark theme is a #141414 rectangle. So the panel went black on the arm and stayed black
// until the capture landed. The first fix for it was a guard on the capture watch ("an unchanged document is not
// news -- except when nothing is drawn"), which drew the page back but ALSO ended the wait (see the next section),
// and that is why the result then only appeared when the user clicked a row.
//
// Now a re-read of the page you are on keeps what is drawn, so there is no empty render to recover from: the
// snapshot alone must leave the page exactly as it was.
{
  const rows = () => countByClass(body(), "row");
  const drawnBefore = rows();
  check("  the page is drawn while the capture is armed", drawnBefore > 0, "rows=" + drawnBefore);
  P.snapshot(JSON.stringify({ host: { lang: "en", theme: "light", systemLight: 1, systemIsChinese: 0 },
    features: [{ slot: 0, ok: 1, id: "F", nameZh: "F", nameEn: "F", version: "1", off: 0, enabled: 1 }] }));
  const drawnAfterSnapshot = rows();
  check("    and the snapshot that follows every listOp does not blank it",
        drawnAfterSnapshot === drawnBefore, "rows=" + drawnBefore + " -> " + drawnAfterSnapshot);
  P.controls(JSON.stringify(withNew));   // the SAME document: nothing has happened yet
  check("    and the unchanged answer that follows leaves it drawn",
        rows() === drawnBefore, "rows=" + drawnBefore + " -> " + rows());
}

// ---- ⚠️⚠️ A CAPTURE'S RESULT MUST REACH THE PAGE WITHOUT THE USER TOUCHING ANYTHING ELSE ----
//
// "捕获事件进行时，鼠标点击后结果要马上给到参数页，目前没有，要等到点击新建的规则条才会出现."
//
// ⚠️ WHAT WENT WRONG: the wait used to END when the document CHANGED. But the document also changes for reasons
// that have nothing to do with the capture -- arming one sends `listOp`, the host answers every `listOp` with a
// snapshot, the page re-reads after it, and that answer differs from the one taken when the wait began (it now
// carries `waiting`). So the watch died the moment it started, and the user had to poke the row to make the page
// read again. The mirror image is just as broken: a capture that fills in exactly what was already there changes
// nothing, and that version would have polled for ever.
//
// ⚠️ WHAT REPLACED IT: the FEATURE says whether it is still waiting (apex/abi.h, `waiting`), and that is the only
// thing that keeps the poll alive. The section below plays those deliveries in the order the feature produces
// them, and checks both the result that appears and the wait that ends.
{
  const w = () => P.watch();
  // ⚠️ `withNew` IS ALREADY PARSED (the Add section built it as an object and re-stringified it for the page), so
  // a copy of it is `JSON.parse(JSON.stringify(...))`. Handing the object itself to `P.controls` -- which takes
  // the text the host would send -- is what makes this probe the page's caller and not the host.
  const copy = (o) => JSON.parse(JSON.stringify(o));
  const named = (name) => {
    const d = copy(withNew);
    d.params[0].items[0].values.name = name;
    return d;
  };
  // 1. THE FEATURE'S ANSWER TO THE ARM: the same rows, plus "I am still waiting for the click".
  const armedDoc = copy(withNew);
  armedDoc.params[0].waiting = "capture";
  P.controls(JSON.stringify(armedDoc));
  check("a document that says the feature is still waiting keeps the page watching",
        !!w(), JSON.stringify(w()));
  check("  and it is NOT announced as a finished capture",
        hintBoxText() === P.t("captureArmed"), JSON.stringify(hintBoxText()));

  // 2. THE CLICK LANDS: the feature stops saying `waiting` and the row now holds what was captured. This
  // delivery has to be enough -- no click on the page, no navigation, nothing else.
  const filled = named("explorer.exe / Address bar");
  P.controls(JSON.stringify(filled));
  check("  when the feature stops waiting, the captured values are on the page right away",
        !!first(body(), (n) => n.id === "#input" && n.value === "explorer.exe / Address bar"),
        (() => { const i = first(body(), (n) => n.id === "#input"); return i ? JSON.stringify(i.value) : "no input"; })());
  check("    and the wait is over", !w(), JSON.stringify(w()));
  check("    and it says so in the box beside the button",
        hintBoxText() === (P.S.lang === "zh" ? "已捕获：" : "Captured: ") + "explorer.exe / Address bar",
        JSON.stringify(hintBoxText()));

  // 3. ⚠️ AND A CAPTURE THAT CHANGES NOTHING BUT THE FLAG STILL ENDS THE WAIT. This is the case the old
  // "did the document change" rule could never see: capturing the very control a rule already describes fills in
  // the same values, so the ONLY difference is `waiting` going away -- and if that did not end the wait, the page
  // would ask the host for the controls for ever.
  P.S.grpSel = { rules: 0 };
  elements["body"].children.length = 0;
  P.S.view = 0;
  P.controls(JSON.stringify(filled));      // the document the previous wait ended on
  const beforeRearm = sent.length;
  armOp(body());
  check("  (re-armed for the second half)", captureOps(beforeRearm).length === 1,
        JSON.stringify(captureOps(beforeRearm)));
  P.controls(JSON.stringify(armedDoc));    // the feature is waiting again
  check("    waiting again", !!w());
  const sameValues = JSON.parse(JSON.stringify(filled));   // identical values, no `waiting`
  P.controls(JSON.stringify(sameValues));
  check("    a capture whose values are identical still ends the wait", !w(), JSON.stringify(w()));
}
// ---- REORDERING IS BY DRAGGING; THE ARROWS ARE GONE ----
//
// "规则顺序可以直接拖动调整顺序，上下按钮去掉." Both halves are checked, and the second matters: a list that KEPT
// its arrows while gaining a drag would be two ways to do one thing, and the arrows are the ones the user asked
// to have removed.
elements["body"].children.length = 0;
P.S.view = 0;
P.S.grpSel = { rules: 0 };
P.controls(doc("master", 4, -1));
check("the list has no ▲ / ▼ buttons any more",
      !first(body(), (n) => n.textContent === "▲" || n.textContent === "▼"), "found one");
function nthRow(n) {
  let seen = 0, hit = null;
  walk(body(), (x) => {
    if (hit) return;
    if ((x.className || "").split(" ").indexOf("item") >= 0) {
      if (seen === n) hit = x;
      seen++;
    }
  });
  return hit;
}
const row0 = nthRow(0);
const row2 = nthRow(2);
check("the rows are draggable",
      !!row0 && row0.draggable === true, row0 ? "draggable=" + row0.draggable : "no row");
const beforeDrag = sent.length;
if (row0 && row2) {
  row0.fire("dragstart");
  row2.fire("dragover");
  row2.fire("drop");
}
const dropped = opsSent(beforeDrag);
check("  and dropping a row asks the feature to move it, in ONE message",
      dropped.length === 1 && dropped[0].op === "move" && String(dropped[0].index) === "0" &&
      String(dropped[0].value) === "2",
      JSON.stringify(dropped));
check("  and the selection follows the rule that was moved",
      P.S.grpSel.rules === 2, "selected=" + P.S.grpSel.rules);
check("  and the drop mark is cleared afterwards",
      !/\bdropover\b/.test(row2 ? row2.className : ""), row2 ? row2.className : "no row");

// ---- THE HOTKEY CONTROL: RECORDED, NOT TYPED ----
//
// "切换热键点击后是记录新的快捷键，捕获到有效快捷键后，确定无冲突，自动保存." The three properties, in the order the
// user meets them: clicking starts recording, a press is turned into the feature's own text and sent, and a
// press that is not usable (a modifier alone, a key the feature cannot send, another control's shortcut) is
// refused HERE rather than being stored and silently doing nothing.
{
  const hotkeyDoc = JSON.stringify({ params: [
    { id: "ime_toggle_hotkey", type: "hotkey", labelZh: "切换热键", labelEn: "Toggle hotkey",
      value: "Ctrl+Space", placeholderZh: "点击后按下新的快捷键", placeholderEn: "Click, then press the keys" },
    { id: "rules", type: "group", labelZh: "规则", labelEn: "Rules", editing: -1,
      fields: [{ id: "name", type: "text", labelZh: "名称", labelEn: "Name" }],
      items: [{ title: "r0", values: { name: "r0" } }],
      actions: [{ op: "capture", key: "Ctrl+Alt+Q", labelZh: "点击捕获", labelEn: "Capture" }] },
  ] });
  elements["body"].children.length = 0;
  P.S.view = 0;
  P.S.grpSel = { rules: 0 };
  P.controls(hotkeyDoc);
  const hk = first(body(), (n) => (n.className || "").split(" ").indexOf("hotkey") >= 0);
  check("the hotkey control is drawn as something you click, not type",
        !!hk && hk.textContent === "Ctrl+Space", hk ? JSON.stringify(hk.textContent) : "none");
  const key = (k, mods) => Object.assign({ key: k, preventDefault() {}, stopPropagation() {} }, mods || {});
  if (hk) hk.fire("click");
  check("  clicking it asks for the keys", !!hk && /press|按下/.test(hk.textContent || ""),
        hk ? JSON.stringify(hk.textContent) : "none");
  // A modifier on its own is not a combination: it must keep waiting, and send nothing.
  let at = sent.length;
  if (hk) hk.fire("keydown", key("Control", { ctrlKey: true }));
  check("  a modifier alone is not a shortcut",
        sent.slice(at).filter((x) => x.cmd === "setControl").length === 0,
        JSON.stringify(sent.slice(at)));
  // A key this feature cannot send: refused, with a reason, and still recording.
  at = sent.length;
  if (hk) hk.fire("keydown", key("ArrowUp", { ctrlKey: true }));
  check("  a key the feature cannot send is refused, and says so",
        sent.slice(at).filter((x) => x.cmd === "setControl").length === 0 && /不能用|cannot be used/.test(hintBoxText() || ""),
        JSON.stringify(hintBoxText()));
  // Another control's shortcut: refused here, because the feature cannot see the panel's own action keys.
  at = sent.length;
  if (hk) hk.fire("keydown", key("q", { ctrlKey: true, altKey: true }));
  check("  another control's shortcut is refused here",
        sent.slice(at).filter((x) => x.cmd === "setControl").length === 0 && /占用|already uses/.test(hintBoxText() || ""),
        JSON.stringify(hintBoxText()));
  // And a usable combination: sent to the feature in ITS own text, and the box shows it at once.
  at = sent.length;
  if (hk) hk.fire("keydown", key(" ", { ctrlKey: true, shiftKey: true }));
  const sentCombo = sent.slice(at).filter((x) => x.cmd === "setControl");
  check("  a usable combination is sent to the feature in its own text",
        sentCombo.length === 1 && sentCombo[0].path === "ime_toggle_hotkey" &&
        sentCombo[0].value === "Ctrl+Shift+Space",
        JSON.stringify(sentCombo));
  check("  and the box shows it, saying it was saved",
        !!hk && hk.textContent === "Ctrl+Shift+Space" && /Saved|已保存/.test(hintBoxText() || ""),
        hk ? JSON.stringify(hk.textContent) + " / " + JSON.stringify(hintBoxText()) : "none");
  // Escape gets out of recording without sending anything.
  at = sent.length;
  if (hk) hk.fire("click");
  if (hk) hk.fire("keydown", key("Escape"));
  check("  Escape leaves recording without sending anything",
        sent.slice(at).filter((x) => x.cmd === "setControl").length === 0 && !!hk &&
        hk.textContent === "Ctrl+Shift+Space",
        hk ? JSON.stringify(hk.textContent) : "none");

  // ⚠️⚠️ AND Ctrl+Space IS RECORDED EVEN IF THE BOX NEVER SEES THE PRESS -- THE USER'S OWN REPORT.
  //
  // "切换热键可以支持捕获 Ctrl+Space，现在可能是因为系统默认是这个." The system owns Ctrl+Space (it switches the
  // input method), so pressing it can move the focus before the box gets a keydown; the recorder listens on the
  // DOCUMENT for that reason, and this check fires the press ONLY at the document -- the box's own listener is
  // never called, which is exactly the situation the user was in.
  at = sent.length;
  if (hk) hk.fire("click");
  global.__pressDoc("keydown", key(" ", { ctrlKey: true }));
  const spaceSent = sent.slice(at).filter((x) => x.cmd === "setControl");
  check("  Ctrl+Space is recorded even when only the DOCUMENT sees the press",
        spaceSent.length === 1 && spaceSent[0].value === "Ctrl+Space", JSON.stringify(spaceSent));
  // ...and the listener is taken off again, or the next keystroke anywhere on the page would be recorded.
  at = sent.length;
  global.__pressDoc("keydown", key(" ", { ctrlKey: true }));
  check("  and the recorder stops listening once it has an answer",
        sent.slice(at).filter((x) => x.cmd === "setControl").length === 0, "still listening");
}

// ---- THE EDIT BUTTON AND THE SAVE BUTTON ARE THE SAME BUTTON ----
//
// "点完编辑按钮时，它要变成'保存'，这样才知道是不是编辑状态." The first version built two DIFFERENT buttons, one in each
// branch -- which is the same behaviour in the code and says nothing on screen: a control that is REPLACED looks
// like a different control, while one that stays put and changes its word is a state the user can read.
//
// The assertions are "exactly one button, and its label differs by state", which is what "the same button"
// means where it matters. A second button appearing beside the first would fail the count.
elements["body"].children.length = 0;
P.S.view = 0;
P.S.grpEditing = {};
// ⚠️ THE SELECTION IS PART OF THE SET-UP, because the page only shows edit mode for the row the FEATURE names
// AND only when that is the row being looked at. A section that leaves the selection elsewhere makes this one
// fail for a reason that has nothing to do with what it is checking -- which is exactly what happened when the
// drag section above left the selection on row 2.
P.S.grpSel = { rules: 0 };
P.controls(doc("master", 3, -1));
const viewButtons = editBarLabels();
check("view mode: one button, and it says Edit",
      viewButtons.length === 1 && viewButtons[0] === "Edit", JSON.stringify(viewButtons));
clickButton(body(), "Edit");
P.controls(doc("master", 3, 0));
const editButtons = editBarLabels();
check("edit mode: still one button, and it now says Save",
      editButtons.length === 1 && editButtons[0] === "Save", JSON.stringify(editButtons));
// ...and pressing THAT button saves -- the toggle is not decoration.
const beforeToggleSave = sent.length;
clickButton(body(), "Save");
check("  and pressing it saves the rule",
      opsSent(beforeToggleSave).some((m) => m.op === "commit-edit"), JSON.stringify(opsSent(beforeToggleSave)));
P.controls(doc("master", 3, -1));
check("  and it goes back to saying Edit", JSON.stringify(editBarLabels()) === '["Edit"]',
      JSON.stringify(editBarLabels()));

// ---- EVERY t("...") KEY THE SCRIPT USES MUST EXIST ----
//
// ⚠️ THIS ONE IS HERE BECAUSE IT HAPPENED. The save path called `t("saved")` and no such key was ever defined, so
// `t` fell through to its last resort -- the key itself -- and the message read "saved" in English and in
// Chinese. Nothing else would have noticed: the readout is a string either way, and a missing key is invisible
// in the page's source (it looks exactly like a key that IS defined, one line away).
{
  const html = fs.readFileSync(path.join("build", "panel.built.html"), "utf8");
  const script = html.match(/<script>([\s\S]*?)<\/script>\s*<\/body>/)[1];
  const used = new Set();
  const re = /\bt\("([A-Za-z0-9_]+)"\)/g;
  let m;
  while ((m = re.exec(script)) !== null) used.add(m[1]);
  const missing = [];
  used.forEach((k) => {
    if (!P.t(k) || P.t(k) === k) missing.push(k); // t() returns the key itself when it has nothing
  });
  check("every t() key the script uses is defined", missing.length === 0,
        missing.length ? "missing: " + missing.join(", ") : used.size + " keys used");
}

console.log(failures ? "\nFAILED (" + failures + ")" : "\nOK: edit/save/cancel/capture, per rule");
process.exit(failures ? 1 : 0);
