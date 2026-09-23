// THE EDIT/SAVE SEQUENCE, IN THE ORDER THE REAL PANEL SEES IT.
//
// ⚠️⚠️ THIS PROBE EXISTS BECAUSE THE THREE PROBES BEFORE IT ALL PASSED WHILE THE FEATURE DID NOT WORK.
//
// They all made the same mistake: they drove the page through ONE step and looked. The real panel receives a
// SEQUENCE -- and the fault was in the sequence, not in any step:
//
//   1. the user clicks 编辑;
//   2. the page sends `listOp begin-edit` and records which rule is being edited;
//   3. THE HOST REPLIES WITH A SNAPSHOT (ui_webview.cpp answers every command it does not exclude, and listOp
//      is not excluded);
//   4. the snapshot's handler calls `loadFeature`, which called `cancelEditOnLeave` -- so the page immediately
//      sent `cancel-edit` and cleared its own edit flag.
//
// The edit was destroyed by its own side effect one message after it began. A probe that stops after step 2
// sees a working edit; the user, who lives at step 4, sees nothing happen.
//
// usage: node _diag/apex_edit_sequence_probe.js
const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");
const REAL = path.join(ROOT, "build", "_real_doc.json");
if (!fs.existsSync(REAL)) {
  console.error("FAIL: " + REAL + " is missing (extract it from apex-settings.log -- see apex_real_doc_probe.js)");
  process.exit(1);
}
const realDoc = fs.readFileSync(REAL, "utf8");

// ⚠️ THE DOCUMENT'S `editing` FIELD IS PART OF THE SEQUENCE NOW, AND LEAVING IT OUT IS WHAT WOULD MAKE THIS
// PROBE WRONG. The feature owns the draft, so the feature is the only party that knows which row is open; it
// publishes that as `editing` (a display index, or -1) and the page draws it. The real document extracted from
// the log was captured when nothing was being edited, so the probe sets the field for the step it is playing --
// exactly as the feature would.
function withEditing(docText, index) {
  const d = JSON.parse(docText);
  d.params.forEach((p) => { if (p.id === "rules") p.editing = index; });
  return JSON.stringify(d);
}

const snapshot = JSON.stringify({
  host: { lang: "zh", theme: "light", systemLight: 1, systemIsChinese: 1 },
  features: [{ slot: 0, ok: 1, id: "AutoIME", nameZh: "自动输入法", nameEn: "Auto IME", version: "1.0.0", off: 0, enabled: 1 }],
});

// ---- the minimal DOM stub (same shape as the other probes) ----
const html = fs.readFileSync(path.join(ROOT, "build", "panel.built.html"), "utf8");
const m = html.match(/<script>([\s\S]*?)<\/script>\s*<\/body>/);
if (!m) { console.error("no script block"); process.exit(1); }

const sent = [];
const elements = {};
function mkEl(id) {
  return {
    id, style: { setProperty() {}, removeProperty() {} }, className: "", textContent: "",
    value: "", disabled: false, children: [], _on: {}, _attr: {},
    // ⚠️ `innerHTML = ""` MUST ACTUALLY CLEAR THE CHILDREN -- see the note in apex_panel_probe.js, which
    // worked around this by hand while these probes silently kept the old page's elements alongside the new
    // ones. A check that looks for a button after a redraw then finds the DETACHED one from the draw before,
    // so the page reads as broken while it is working.
    set innerHTML(v) {
      this._html = v;
      if (v === "") {
        this.children.length = 0;
        // An empty scroll container cannot be scrolled -- this is the browser behaviour that made the bug.
        this.scrollTop = 0;
      }
    },
    get innerHTML() { return this._html || ""; }, scrollTop: 0,
    // ⚠️ `appendChild` MOVES AN EXISTING CHILD AND `insertBefore` EXISTS, because the page is entitled to
    // re-parent an element (it did for the live read-out until that was removed). This stub declared neither,
    // and the page died on the second message of the sequence below -- which is exactly what these stubs are FOR
    // (the same class as the missing `preventDefault` and the missing canvas methods: the browser always has it,
    // so a stub without it turns a working page into a crash).
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
  };
}
global.document = {
  getElementById(id) { return elements[id] || (elements[id] = mkEl(id)); },
  createElement(t) { return mkEl("#" + t.toLowerCase()); },
  documentElement: mkEl("html"), addEventListener() {},
};
global.window = {
  chrome: { webview: { postMessage(x) { sent.push(x); } } },
  matchMedia() { return { matches: true, addEventListener() {} }; },
  devicePixelRatio: 1, addEventListener() {},
};
global.requestAnimationFrame = () => 0;
global.cancelAnimationFrame = () => {};
global.setInterval = () => 0;
global.clearInterval = () => {};
global.navigator = {};

eval(m[1] + ";globalThis.__p = {S:S, controls:window.__apexControls, snapshot:window.__apexSnapshot, renderFeature:renderFeature, t:t, loadFeature:loadFeature};");
const P = globalThis.__p;

let failures = 0;
function check(label, ok, detail) {
  console.log((ok ? "  ok   " : "  FAIL ") + label + (detail ? "  (" + detail + ")" : ""));
  if (!ok) failures++;
}
function walk(node, fn, depth) {
  if (!node || (depth || 0) > 14) return;
  fn(node);
  (node.children || []).forEach((c) => walk(c, fn, (depth || 0) + 1));
}
function first(node, pred) { let hit = null; walk(node, (n) => { if (!hit && pred(n)) hit = n; }); return hit; }
const body = () => elements["body"];
function editButton() {
  const bar = first(body(), (n) => (n.className || "").split(" ").indexOf("editbar") >= 0);
  if (!bar) return null;
  return (bar.children || []).filter((c) => c.id === "#button")[0] || null;
}
function ops(from) { return sent.slice(from).filter((x) => x.cmd === "listOp"); }
function mark() { return sent.length; }

console.log("== the sequence the real panel sees ==");

// 1. the page connects and a feature page is opened.
const viewDoc = withEditing(realDoc, -1);
const editDoc = withEditing(realDoc, 0);
P.snapshot(snapshot);
P.S.grpSel = { rules: 0 };
// ⚠️ THE VIEW IS SET THE WAY THE PAGE SETS IT: through the nav row, which is what a user clicks. Setting
// S.view directly would skip loadFeature's own work -- and loadFeature is where the bug lived, so skipping it
// would make this probe pass for the wrong reason.
if (P.loadFeature) P.loadFeature(0, true);
else P.S.view = 0;
P.controls(viewDoc);
check("the feature page is open, with an edit button", !!editButton(), editButton() ? editButton().textContent : "none");

// 2. THE CLICK.
let at = mark();
const btn = editButton();
if (btn) btn.fire("click");
const began = ops(at);
check("clicking Edit asks the feature to begin", began.some((x) => x.op === "begin-edit"), JSON.stringify(began));

// 3. THE HOST'S REPLY -- a snapshot, because ui_webview.cpp answers every command it does not exclude.
//    ⚠️ THIS IS THE STEP THE EARLIER PROBES SKIPPED, AND THE ONE THE BUG LIVED IN.
at = mark();
P.snapshot(snapshot);
const afterSnapshot = ops(at);
check("the snapshot that follows does NOT cancel the edit",
      !afterSnapshot.some((x) => x.op === "cancel-edit"), JSON.stringify(afterSnapshot));

// 4. and the page's own re-read (what refreshControls asked for) -- with the feature's answer: row 0 is open.
P.controls(editDoc);
check("the button still says 保存 after the re-read",
      !!editButton() && /保存|Save/.test(editButton().textContent || ""),
      editButton() ? JSON.stringify(editButton().textContent) : "no button");

// 5. TYPING. The fields are the page's; the value goes to the feature as setControl.
const detail = first(body(), (n) => (n.className || "").split(" ").indexOf("splitdetail") >= 0);
const input = detail ? first(detail, (n) => n.id === "#input" && n.type === "text") : null;
check("the fields are editable", !!input && !input.disabled, input ? "disabled=" + input.disabled : "none");
at = mark();
if (input) { input.value = "typed-by-probe"; input.fire("change"); }
const typed = sent.slice(at).filter((x) => x.cmd === "setControl");
check("  and typing sends the field to the feature",
      typed.length === 1 && typed[0].path === "rules[0].name", JSON.stringify(typed));

// 6. SAVING.
at = mark();
const saveBtn = editButton();
if (saveBtn) saveBtn.fire("click");
const saved = ops(at);
check("clicking Save commits the edit", saved.some((x) => x.op === "commit-edit"), JSON.stringify(saved));

// 7. and the page goes back to view mode -- the feature's answer to a commit is "no row is open".
P.snapshot(snapshot);
P.controls(viewDoc);
check("  and the button says 编辑 again",
      !!editButton() && /编辑|Edit/.test(editButton().textContent || ""),
      editButton() ? JSON.stringify(editButton().textContent) : "no button");

// ---- THE SCROLL MUST SURVIVE A RE-RENDER ----
//
// "点'编辑'时，参数界面会焦点会跳到顶端，这点规避掉，不要跳。保持原样."
//
// The page rebuilds `#body` on every re-read, and clearing innerHTML resets the scroll of the scrolling element.
// A user reading a long rule's fields presses Edit and is thrown back to the top of the page.
at = mark();
elements["body"].scrollTop = 420;
P.controls(viewDoc);
check("a re-read of the SAME page keeps the scroll position",
      elements["body"].scrollTop === 420, "scrollTop=" + elements["body"].scrollTop);

console.log(failures ? "\nFAILED (" + failures + ")" : "\nOK: the edit survives the message sequence");
process.exit(failures ? 1 : 0);
