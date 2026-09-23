// Run the REAL controls document (captured from the running host) through the page, and drive the edit button.
//
// ⚠️ WHY THIS IS A SEPARATE PROBE FROM apex_edit_probe.js: that one uses a two-field synthetic fixture, which is
// what a probe SHOULD do for the layout. But the user reported the feature still not working while that fixture
// passed every time -- so the difference has to be in the real data, and the way to find it is to feed the page
// the document the host actually sends. build/_real_doc.json is that document, taken from the panel's own log.
//
// usage: node _diag/apex_real_doc_probe.js
const fs = require("fs");
const path = require("path");

const REAL = path.join(__dirname, "..", "build", "_real_doc.json");
if (!fs.existsSync(REAL)) {
  console.error("FAIL: " + REAL + " is missing -- run the panel once and re-extract it from apex-settings.log");
  process.exit(1);
}
const realDoc = fs.readFileSync(REAL, "utf8");

// ---- the same minimal DOM stub the other probes use ----
const html = fs.readFileSync(path.join(__dirname, "..", "build", "panel.built.html"), "utf8");
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
    set innerHTML(v) { this._html = v; if (v === "") this.children.length = 0; },
    get innerHTML() { return this._html || ""; },
    // ⚠️ `appendChild` MOVES AN EXISTING CHILD AND `insertBefore` EXISTS -- see the note in apex_edit_probe.js:
    // the page re-parents its live readout into the list bar it belongs in, and this stub declared neither.
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

eval(m[1] + ";globalThis.__p = {S:S, controls:window.__apexControls, snapshot:window.__apexSnapshot, renderFeature:renderFeature, t:t};");
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
function buttons() {
  const out = [];
  walk(body(), (n) => { if (n.id === "#button") out.push(n); });
  return out;
}
function buttonSaying(text) { return first(body(), (n) => n.id === "#button" && n.textContent === text); }
function ops(from) { return sent.slice(from).filter((x) => x.cmd === "listOp"); }

console.log("== the real document, through the page ==");
P.snapshot(JSON.stringify({ host: { lang: "zh", theme: "light", systemLight: 1, systemIsChinese: 1 },
  features: [{ slot: 0, ok: 1, id: "AutoIME", nameZh: "自动输入法", nameEn: "Auto IME", version: "1.0.0", off: 0, enabled: 1 }] }));
P.S.view = 0;
P.controls(realDoc);

const all = buttons().map((b) => b.textContent);
console.log("   buttons on the page: " + JSON.stringify(all));

// The rule list should be there, one row per rule, with the capture button in the bar.
const rows = [];
walk(body(), (n) => { if ((n.className || "").split(" ").indexOf("item") >= 0) rows.push(n); });
const ruleCount = JSON.parse(realDoc).params.find((p) => p.id === "rules").items.length;
check("the list shows every rule in the document", rows.length === ruleCount,
      "rows=" + rows.length + " rules=" + ruleCount);

const captureBtn = first(body(), (n) => n.id === "#button" && /捕获/.test(n.textContent || ""));
check("the capture button is drawn with its keycap",
      !!captureBtn && /Ctrl\+Alt\+Q/.test(captureBtn.textContent || ""),
      captureBtn ? JSON.stringify(captureBtn.textContent) : "missing");

// ---- the edit button ----
const before = sent.length;
const editBtn = buttonSaying("编辑") || buttonSaying("Edit");
check("an Edit button is drawn", !!editBtn, JSON.stringify(all));
if (editBtn) editBtn.fire("click");
const began = ops(before);
check("  and it asks the feature to begin editing", began.some((x) => x.op === "begin-edit"),
      JSON.stringify(began));

// The host answers a begin-edit by... nothing (it returns a code). The page re-reads with describe, and the
// FEATURE's next settingsJson will carry the draft. The probe plays that part by re-feeding the SAME document
// -- which is what a real page gets when nothing has changed in the data yet.
P.controls(realDoc);
const afterClick = buttons().map((b) => b.textContent);
check("  and the button now says 保存 (the edit state is visible)",
      afterClick.some((x) => x === "保存" || x === "Save"), JSON.stringify(afterClick));

const fieldInputs = [];
walk(body(), (n) => { if (n.id === "#input") fieldInputs.push(n); });
check("  and the fields are editable", fieldInputs.length > 0 && !fieldInputs[0].disabled,
      fieldInputs.length ? "inputs=" + fieldInputs.length + " first disabled=" + fieldInputs[0].disabled : "no inputs");

console.log(failures ? "\nFAILED (" + failures + ")" : "\nOK: the page drives the real document");
process.exit(failures ? 1 : 0);
