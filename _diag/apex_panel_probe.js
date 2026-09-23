// A HEADLESS RUN OF THE PANEL'S OWN SCRIPT.
//
// Why this exists: the panel is a page inside a web view, and a bug in it is invisible from the host's side
// -- the first version reported success at every layer while the panel showed nothing, and finding that
// took a screenshot, a log trace and two rebuilds. This runs the same script against a DOM stub, so the
// page's logic (does a snapshot mark it connected, does a click select a feature, do the controls render)
// can be checked in one second and no window at all.
//
// It is NOT a substitute for looking at the panel -- it cannot tell whether anything LOOKS right. It
// catches the class of failure that is otherwise silent: a function that does not exist, a stale value, a
// render that throws.
//
// node _diag/apex_panel_probe.js
//
// ⚠️ THE PATH IS apex/ui/panel.html. This probed `app/apex/ui/panel.html` -- the layout from before the split
// out of the plugin project -- so it could not run at all, which is worse than having no probe: AGENTS.md
// lists it as a diagnostic and nothing said it had stopped working.
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..");
const html = fs.readFileSync(path.join(root, "build", "panel.built.html"), "utf8");

const scriptStart = html.indexOf("<script>");
const scriptEnd = html.indexOf("</script>");
if (scriptStart < 0 || scriptEnd < 0) {
  console.log("FAIL: no script block in panel.html");
  process.exit(1);
}
const js = html.slice(scriptStart + "<script>".length, scriptEnd);

let failures = 0;
function check(what, pass, detail) {
  console.log("  " + what.padEnd(62) + (pass ? "ok" : "FAIL") + (detail ? "  " + detail : ""));
  if (!pass) ++failures;
}

// ---- a DOM stub, only as complete as the page actually uses ----
// THE CHART FIXTURE, IN THE FORMAT THE FEATURE ACTUALLY SENDS (see ApexFeature::settingsJson).
// ⚠️ IT HAS BEEN WRONG TWICE, AND BOTH TIMES THIS PROBE STAYED GREEN while the real card was invisible:
// it fed a bare points array, then a ported chart object, each after the feature had moved on. A probe that
// feeds a format the feature no longer sends is a green light over a broken page -- so this is written from
// the feature's own output, and the keys here are the ones settingsJson writes.
function chartFixture() {
  return {
    spanMs: 400, yTopDeltas: 1080, notch: 120, notches: 6,
    nativeY: 0.6667, stepX: 50, stepY: 200,
    shape: [[0,0,0],[0.1,0.03,0],[0.2,0.085,0],[0.35,0.36,1],[0.5,0.7,1],[0.6,0.94,1],[0.7,1,2],[1,1,2]],
    native: [[0,0],[0.17,0.1111],[0.34,0.2222],[0.5,0.3333],[0.67,0.4444],[0.84,0.5556],[1,0.6667]],
  };
}

const elements = {};
// HOW MANY TIMES THE CHART WAS ACTUALLY PAINTED. A stub canvas method that does nothing is enough to prove the
// page does not THROW, but not enough to prove the chart was REDRAWN -- which is what "the curve follows the
// sliders" means. `clearRect` is the first thing drawCurve does on every pass, so counting it counts draws.
let canvasClears = 0;
function mkEl(id) {
  return {
    // ⚠️ `style` NEEDS setProperty, for the same reason the canvas stub needs its methods and the event stub
    // needs preventDefault: the page CALLS it (a slider paints its thumb from the feature's `hue`), the real
    // browser always provides it, and a stub without it turns a working page into a thrown exception. It went
    // unnoticed until a feature with a `hue` was driven through this probe -- the first fixture had none.
    id, style: { setProperty() {}, removeProperty() {} },
    className: "", textContent: "", innerHTML: "", value: "", children: [],
    _attr: {},
    // ⚠️ `appendChild` MOVES AN EXISTING CHILD, exactly as the DOM does, and the stub did not. The page is
    // entitled to re-parent an element (it did for the live read-out until that was removed), and a stub that let
    // it sit in two parents at once would report a page that cannot exist while hiding the one fault that matters
    // here: an element that was MOVED but left behind somewhere else.
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
    // ⚠️ HANDLERS ARE KEPT, NOT SWALLOWED. This was `addEventListener() {}` -- a stub that accepts a listener
    // and forgets it, which means the probe could not tell a wired-up page from one with no listeners at all.
    // The General entry lived for a while with its text and its selected class being set on every render and
    // NO click handler ever attached, so once a feature was open there was no way back; the user found it, and
    // this stub is why the probe did not. Clicking is now something the probe can do.
    _on: {},
    addEventListener(type, fn) { (this._on[type] = this._on[type] || []).push(fn); },
    // `detail` is merged into the event object, so a handler that reads `e.deltaY` (the wheel over a slider)
    // can be driven with a real notch rather than a bare `{type}`. Existing calls -- fire("click") -- pass
    // nothing and are unaffected.
    //
    // ⚠️ THE EVENT NEEDS preventDefault, and its absence is the same class of stub gap as the missing canvas
    // methods below: the wheel handler calls it (a wheel over a slider must not also scroll the page), the
    // real browser always provides it, and a stub that does not makes a WORKING page look broken.
    fire(type, detail) {
      (this._on[type] || []).forEach((fn) =>
        fn(Object.assign({ type, preventDefault() {}, stopPropagation() {} }, detail)));
    },
    setAttribute(k, v) { this._attr[k] = v; },
    removeAttribute(k) { delete this._attr[k]; },
    getAttribute(k) { return this._attr[k] || null; },
    querySelector() { return null; },
    // ⚠️ THE CANVAS STUB MUST BE AS COMPLETE AS THE DRAWING, and it was not: the chart now sets a font and
    // writes tick numbers, so fillText/font/textAlign/textBaseline/save/restore/setLineDash/arc/fill are all
    // called. A stub missing one of them throws INSIDE drawCurve, and the probe then reports the page as
    // broken -- a probe failure that is entirely the probe's fault, which is the most expensive kind.
    getContext() {
      const noop = function () {};
      return {
        setTransform: noop, clearRect() { canvasClears++; }, beginPath: noop, moveTo: noop, lineTo: noop,
        stroke: noop,
        fillRect: noop, drawImage: noop, fillText: noop, arc: noop, fill: noop, save: noop,
        restore: noop, setLineDash: noop, measureText: function () { return { width: 10 }; },
        set strokeStyle(v) {}, set fillStyle(v) {}, set lineWidth(v) {}, set lineJoin(v) {},
        set lineCap(v) {}, set font(v) {}, set textAlign(v) {}, set textBaseline(v) {}
      };
    },
    get clientWidth() { return 600; },
    get clientHeight() { return 130; },
  };
}

global.document = {
  getElementById(id) { if (!elements[id]) elements[id] = mkEl(id); return elements[id]; },
  createElement(t) { return mkEl("#" + t); },
  documentElement: mkEl("html"),
  addEventListener() {},
};
const sent = [];
global.window = {
  chrome: { webview: { postMessage(m) { sent.push(m); } } },
  matchMedia() { return { matches: true, addEventListener() {} }; },
  devicePixelRatio: 1,
  addEventListener() {},
};
// ⚠️ requestAnimationFrame MUST NOT CALL THE CALLBACK, and it used to. `(f) => f()` reads like "run it now",
// and it worked while the only animation was a single draw. The motion chart changed that: its frame
// function RE-ARMS ITSELF (`curveAnim = requestAnimationFrame(step)` at the end of each frame), so calling it
// eagerly recurses until the stack dies -- and a probe that crashes reports everything after it as missing.
// The stub now hands back an id and lets the caller drive frames explicitly; cancelAnimationFrame is a no-op
// for the same reason (there is no real queue behind it).
let rafId = 0;
const rafQueue = new Map();
global.requestAnimationFrame = (f) => { const id = ++rafId; rafQueue.set(id, f); return id; };
global.cancelAnimationFrame = (id) => { rafQueue.delete(id); };
global.getComputedStyle = () => ({ getPropertyValue: () => "#123456" });
// ⚠️ TIMERS ARE RECORDED, NOT SWALLOWED, and this is the same class of gap as the listener stub above: it was
// `setTimeout = () => 0`, a stub that accepts a callback and drops it -- so anything the page DEFERS became
// invisible here, and "it schedules a redraw" looked exactly like "it never redraws at all". The page defers two
// things that matter: the chart refresh while a slider is dragged (queueCurve) and the startup timers.
//
// They are queued and run only when a check asks (`runTimers`), so every check written before this behaved and
// still behaves the same way -- nothing fires behind their backs.
let timerSeq = 0;
const timers = [];
global.setTimeout = (fn, ms) => {
  const id = ++timerSeq;
  timers.push({ id, fn, at: Date.now() + (ms || 0) });
  return id;
};
global.clearTimeout = (id) => {
  const at = timers.findIndex((t) => t.id === id);
  if (at >= 0) timers.splice(at, 1);
};
// ⚠️ AND INTERVALS ARE RECORDED TOO, WHICH THEY WERE NOT: this was `setInterval = () => 0`, a callback the page
// hands over and never sees again -- the same gap the note above describes for `setTimeout`, and it hid the two
// watches that ask the host again by THEMSELVES: the capture wait (200 ms) and a live group (apex/abi.h `live`,
// ABI 17 -> 18, 1000 ms). Nothing fires unless a check asks, and `clearInterval` really removes one, so "the page
// started watching" and "the page stopped" are both visible from here.
let intervalSeq = 0;
const intervals = new Map();
global.setInterval = (fn, ms) => { const id = ++intervalSeq; intervals.set(id, { fn, ms }); return id; };
global.clearInterval = (id) => { intervals.delete(id); };
function intervalsOf(ms) {
  return Array.from(intervals.values()).filter((t) => t.ms === ms);
}

// ---- run the page's script ----
let booted = true;
let bootError = "";
try {
  // The script is evaluated with the stub in scope, and its globals are captured for the checks below.
  //
  // ⚠️ THIS LIST MUST NAME ONLY THINGS THAT EXIST. A name that does not evaluates to `undefined`, the probe
  // still boots, and then it throws `X is not a function` in the middle of the checks -- which reads as "the
  // page is broken" rather than "the probe is stale". That has happened here twice, in both directions: once
  // when the animation was removed while this list still drove it, and once when it came back. If you delete an
  // entry point from the page, delete it here in the same change.
  eval(js + ";globalThis.__p = {S:S, renderFeature:renderFeature, renderGeneral:renderGeneral, " +
         "renderNav:renderNav, loadFeature:loadFeature, " +
         "snapshot:window.__apexSnapshot, controls:window.__apexControls, t:t, " +
         "applyTheme:applyTheme, syncNativeTheme:syncNativeTheme, " +
         "activity:window.__apexActivity, stateChanged:window.__apexStateChanged, " +
         "balls:function(){return curveBorn;}};");
} catch (e) {
  booted = false;
  bootError = e.message;
}

console.log("panel script");
check("the script runs at all", booted, bootError);
if (!booted) {
  console.log("\nFAILED (nothing below can be trusted)");
  process.exit(1);
}
const P = globalThis.__p;

// ---- driving what the page DEFERS --------------------------------------------------------------
// The two helpers the checks below need, and nothing else in this file uses them: frames (the page's
// `requestAnimationFrame`) and timers (its `setTimeout`).
function flushFrames(rounds) {
  for (let n = 0; n < (rounds || 4) && rafQueue.size; ++n) {
    const due = Array.from(rafQueue.entries());
    rafQueue.clear();
    due.forEach(([, fn]) => fn());
  }
}
function runTimers() {
  const due = timers.splice(0, timers.length);
  due.forEach((t) => t.fn());
}
// Walk the stub's tree for the first node with this id / of this control type. Needed because the stub's
// `getElementById` cache is only filled for ids the PAGE asked for by id -- a card the page built with
// createElement and then named is not in it (see the note in the chart block below).
function firstById(node, want, depth) {
  if (!node || (depth || 0) > 8) return null;
  if (node.id === want) return node;
  const kids = node.children || [];
  for (let i = 0; i < kids.length; i++) {
    const hit = firstById(kids[i], want, (depth || 0) + 1);
    if (hit) return hit;
  }
  return null;
}
function firstRange(node, depth) {
  if (!node || (depth || 0) > 8) return null;
  if (node.type === "range") return node;
  const kids = node.children || [];
  for (let i = 0; i < kids.length; i++) {
    const hit = firstRange(kids[i], (depth || 0) + 1);
    if (hit) return hit;
  }
  return null;
}

// ---- it started disconnected, and asked the host for the state ----
check("it asks the host for a snapshot on start",
      sent.some((m) => m.cmd === "snapshot"));
// ⚠️ "NOT ANSWERED YET" IS ITS OWN STATE, NOT "DISCONNECTED". This used to assert `connected === false`, which
// is what the panel used to do -- start out accusing the host of not running, in red, for as long as the first
// round trip took. `null` is the unknown state and it has to read as loading (see renderConn).
check("it starts out not-yet-answered, rather than disconnected", P.S.connected === null,
      "connected=" + JSON.stringify(P.S.connected));
check("  and says it is loading, not that the host is missing",
      elements["nohost"].style.display === "block" &&
      elements["nohost"].className === "wait" &&
      elements["nohost"].textContent !== "",
      "text=" + JSON.stringify(elements["nohost"].textContent) +
      " class=" + JSON.stringify(elements["nohost"].className));

// ---- the panel process is told the page is on screen, so it can show its window ----
// ⚠️ THIS SIGNAL IS WHY THE WINDOW IS NOT SHOWN TOO EARLY. The window is created hidden (see ShowPanelOnce in
// ui_webview.cpp) because before the page is drawn it is nothing but background colour -- "设置刚打开时，会有一小
// 段时间面板是空的". If this message stops being sent, the panel falls back to a 900 ms timer: usable, but the
// user is back to watching a blank rectangle.
sent.length = 0;
flushFrames();
check("it tells the panel process that it has drawn (after the first frame)",
      sent.some((m) => m.cmd === "ready"), JSON.stringify(sent.map((m) => m.cmd)));

// ---- and the loading state expires by itself when no host ever answers ----
runTimers();
check("an unanswered panel gives up and says the host is not running",
      P.S.connected === false && elements["nohost"].className === "",
      "connected=" + JSON.stringify(P.S.connected) +
      " class=" + JSON.stringify(elements["nohost"].className));

// ---- a snapshot arrives ----
sent.length = 0;
P.snapshot(JSON.stringify({
  // ⚠️ NO "enabled" IN host: the host has no master switch any more (a feature owns its own). The page would
  // not crash on one -- it simply has no control for it -- but a probe that sends a field the host no longer
  // produces is a probe describing a page that no longer exists. The feature's own `off`/`enabled` below are a
  // different thing and are still sent.
  host: { lang: "zh", theme: "dark", skip: ["game.exe"], systemLight: 0,
          systemLang: "zh-CN", systemIsChinese: 1 },
  features: [{ slot: 0, ok: 1, id: "SmoothWheel", nameZh: "滑动滚轮", nameEn: "Smooth Wheel Scroll",
               version: "1.0.0", off: 0, enabled: 1 }],
}));
check("a snapshot marks it connected", P.S.connected === true);
check("  and the banner is hidden, not left over from boot",
      elements["nohost"].style.display === "none", "display=" + elements["nohost"].style.display);
check("  and it resolved the language to Chinese", P.S.lang === "zh", P.S.lang);
check("  and it applied the saved theme", P.S.theme === "dark", P.S.theme);
check("  and it listed the feature in the sidebar", elements["nav"].children.length === 1);

// ---- THE STATE DOT: always there, at the right, one of three colours ----
//
// ⚠️ THIS IS CHECKED BECAUSE THE DOT HAS BEEN WRONG IN TWO DIFFERENT WAYS, and both were invisible from the
// outside. It used to be an ERROR badge -- appended only when a feature was off, so "off" and "still loading"
// looked identical, which is the one thing a status indicator must not do. And its colour was restyled for a
// SELECTED row into the foreground colour, so clicking a feature changed what the dot SAID.
//
// The three states are named as classes and the colours come from CSS, so what the probe can hold is the
// thing the page decides: which state a row is in, and that a dot is always present.
{
  const items = elements["nav"].children;
  function dotOf(it) {
    const kids = (it && it.children) || [];
    for (const c of kids) if (c.className && c.className.indexOf("dot") === 0) return c;
    return null;
  }
  const d0 = dotOf(items[0]);
  check("  and the row carries a state dot", !!d0, d0 ? "class=" + d0.className : "no .dot child");
  // The snapshot above is the normal case: a feature that is loaded and on.
  check("    which reads ON for a feature that is running",
        d0 && d0.className.indexOf("dot-on") >= 0, d0 ? d0.className : "-");

  // ... and the other two states, driven from the snapshot the way the host would send them.
  //
  // ⚠️ A REBUILT OBJECT, NOT A PATCHED ONE. `Object.assign` onto the snapshot's own entry would leave the
  // next check reading the state this one set -- the stub keeps the object it was handed, so a test that
  // mutated it would be measuring itself.
  const base = { slot: 0, ok: 1, id: "SmoothWheel", nameZh: "滚轮", nameEn: "Wheel", version: "1.0.0",
                 off: 0, enabled: 1 };
  function renderWith(patch) {
    const f = {};
    for (const k of Object.keys(base)) f[k] = base[k];
    for (const k of Object.keys(patch)) f[k] = patch[k];
    P.S.snap = { host: P.S.snap.host, features: [f] };
    P.S.view = "general";
    elements["nav"].children.length = 0;
    P.renderNav();
    return dotOf(elements["nav"].children[0]);
  }
  const dOff = renderWith({ off: 1 });
  check("    and OFF for one the user disabled",
        dOff && dOff.className.indexOf("dot-off") >= 0, dOff ? dOff.className : "-");
  const dDead = renderWith({ ok: 0 });
  check("    and DISABLED for one that would not load",
        dDead && dDead.className.indexOf("dot-disabled") >= 0, dDead ? dDead.className : "-");
  // The three must be DISTINCT classes, or two states would draw the same colour and the dot would be lying.
  const set = [d0.className, dOff.className, dDead.className];
  check("    and the three states are three different classes",
        new Set(set).size === 3, set.join(" | "));

  // ---- THE DOT IS A SWITCH NOW, AND IT IS CHECKED AS ONE ----
  //
  // The user asked for the feature's on/off to live on the dot ("插件的启动开关直接做到原来插件列表的'绿点'位置").
  // The colours above are half of it; the other half is that clicking SENDS THE CHANGE, and that a feature which
  // failed to load does NOT pretend to be switchable.
  function clickDot(patch) {
    const before = sent.length;
    const d = renderWith(patch);
    if (d) d.fire("click");
    return sent.slice(before);
  }
  const onSent = clickDot({ off: 0 });
  check("    clicking a running feature's switch disables it",
        onSent.some((m) => m.cmd === "featureOff" && String(m.value) === "1" && String(m.slot) === "0"),
        JSON.stringify(onSent));
  const offSent = clickDot({ off: 1 });
  check("    clicking a disabled feature's switch enables it",
        offSent.some((m) => m.cmd === "featureOff" && String(m.value) === "0"),
        JSON.stringify(offSent));
  // ⚠️ AND A FEATURE THAT FAILED TO LOAD HAS NOTHING TO SWITCH. There is no `featureOff` the host could honour
  // for it, so a click must send nothing at all -- a switch that looks pressable and does nothing is worse than
  // no switch, because the user's honest conclusion is that the program is broken.
  const deadSent = clickDot({ ok: 0 });
  check("    and a failed feature's marker is NOT a switch",
        !deadSent.some((m) => m.cmd === "featureOff"), JSON.stringify(deadSent));
  // The click must not also switch pages: the switch is a control ON the row, not the row.
  check("    and the switch does not navigate into the feature",
        !deadSent.some((m) => m.cmd === "describe") && !onSent.some((m) => m.cmd === "describe"),
        "describe in " + JSON.stringify(onSent.concat(deadSent)));

  // Put the normal snapshot back for whatever runs after this.
  P.S.snap = { host: P.S.snap.host, features: [base] };
  P.renderNav();
}
check("  and the language was reported to the host for the caption",
      sent.some((m) => m.cmd === "nativeTheme"));

// ---- WHAT THE HOST IS TOLD ABOUT THE THEME, pinned or following ----
//
// `pinned` is not decoration. The page re-reports on a system theme change ONLY while it is following the
// system; a pinned theme stays silent, because the change cannot affect it. So the host (which sets the
// window's caption AND its icon, neither of which the page can draw) has to know which of the two it is
// holding: told nothing, it re-reads the system on the next theme change and quietly undoes the user's pinned
// theme in the two places they cannot see it fixed.
function lastNativeTheme() {
  for (let i = sent.length - 1; i >= 0; i--) if (sent[i].cmd === "nativeTheme") return sent[i];
  return null;
}
sent.length = 0;
P.applyTheme("auto");
check("  theme=auto reports itself as NOT pinned", lastNativeTheme() && lastNativeTheme().pinned === 0,
      lastNativeTheme() ? "pinned=" + lastNativeTheme().pinned : "no message");
sent.length = 0;
P.applyTheme("light");
check("  theme=light reports pinned AND light",
      lastNativeTheme() && lastNativeTheme().pinned === 1 && lastNativeTheme().dark === 0,
      lastNativeTheme() ? "pinned=" + lastNativeTheme().pinned + " dark=" + lastNativeTheme().dark : "none");
sent.length = 0;
P.applyTheme("dark");
check("  theme=dark reports pinned AND dark",
      lastNativeTheme() && lastNativeTheme().pinned === 1 && lastNativeTheme().dark === 1,
      lastNativeTheme() ? "pinned=" + lastNativeTheme().pinned + " dark=" + lastNativeTheme().dark : "none");
P.applyTheme("auto"); // leave the page as the rest of the checks expect it

// ---- a feature's controls arrive ----
P.controls(JSON.stringify({
  params: [
    { id: "enabled", type: "bool", labelZh: "启用", labelEn: "Enable", value: 1 },
    { id: "glide", type: "range", labelZh: "滑动时长", labelEn: "Glide", min: 100, max: 300, step: 5,
      value: 200, unit: "ms" },
  ],
  curve: chartFixture(),
}));
check("the feature's controls were accepted", P.S.controls.params.length === 2);
check("  including its chart", P.S.controls.curve && P.S.controls.curve.shape.length === 8);

// ---- drawing a feature page and the general page ----
let rendered = true;
let renderError = "";
try {
  P.S.view = 0;
  P.renderFeature(0);
  P.renderGeneral();
} catch (e) {
  rendered = false;
  renderError = e.message;
}
check("both pages render without throwing", rendered, renderError);

// ---- SWITCHING BETWEEN THE LIST AND THE GENERAL PAGE ----
//
// ⚠️ THE REPORTED BUG WAS A MISSING CLICK HANDLER, and nothing here could have seen it: the stub accepted
// listeners and dropped them, so "is the general entry wired up at all" was not a question this probe could
// ask. It can now, and this is the check that would have caught it.
//
// The general entry is the brand block at the top of the sidebar (it replaced a footer button that was never
// wired -- see the markup in panel.html). Opening a feature and clicking back has to return to General.
{
  // Clicking a feature in the list selects that feature -- AND ASKS FOR ITS CONTROLS. That second half is
  // the "设置页面不全" bug: the row's handler used to set S.view and render, and only `loadFeature` ever sends
  // the `describe` request, so the page drew an empty card where the parameters belong. The check is on the
  // MESSAGE, not on the view, because the view was always right.
  sent.length = 0;
  const firstItem = elements["nav"].children[0];
  let ok = !!(firstItem && firstItem._on && firstItem._on.click);
  check("the sidebar's feature rows have a click handler", ok);
  if (ok) firstItem.fire("click");
  check("  clicking one selects it", String(P.S.view) === "0", String(P.S.view));
  check("  and asks the host for that feature's controls",
        sent.some((m) => m.cmd === "describe"), sent.map((m) => m.cmd).join(","));

  // And the general entry brings you back. This is the one that was missing.
  const brand = elements["brand"];
  ok = !!(brand._on && brand._on.click);
  check("the general entry (the brand block) has a click handler", ok);
  if (ok) brand.fire("click");
  check("  clicking it returns to the general page", P.S.view === "general", String(P.S.view));

  // The selected styling follows the view, in the same place the clickable area is -- a heading that is
  // clickable but never looks selected reads as decoration.
  P.S.view = 0;
  P.renderNav();
  check("  and the highlight is off the general entry while a feature is shown", elements["brand"].className === "");
  P.S.view = "general";
  P.renderNav();
  check("  and on it while the general page is shown", elements["brand"].className === "sel");
}

// ---- THE CONTROLS ON A FEATURE'S PAGE ----
//
// ⚠️ WHAT THIS CAN AND CANNOT SEE. The "开关不能用" bug was a CSS one: the switch's track and knob are
// absolutely positioned divs that come AFTER the input, so they painted on top of it and took the click --
// the checkbox itself was never hit, no `change` ever fired, and the handler was fine all along. A DOM stub
// has no layout and no hit-testing, so NO assertion here could have caught that; it was found by clicking the
// real window (_diag/apex_click.c) and reading the host's log for a message that never came.
//
// What IS checked here is the wiring on this side of the boundary: that a feature page is built from the
// host's description at all, and that the controls it produces report their changes.
{
  P.controls(JSON.stringify({
    params: [
      { id: "enabled", type: "bool", labelZh: "启用", labelEn: "Enabled", value: 1 },
      { id: "slow", type: "range", labelZh: "慢滚步长", labelEn: "Slow step", min: 1, max: 10, step: 0.1,
        value: 5, unit: "" },
    ],
    curve: null,
  }));
  P.S.controls = JSON.parse(JSON.stringify(P.S.controls));
  P.S.view = 0;
  // ⚠️ CLEAR THE STUB'S BODY FIRST. `b.innerHTML = ""` is how the page clears it, and the stub records
  // children in an array that innerHTML does not touch -- so the counts below picked up the elements from the
  // EARLIER "controls were accepted" block as well (5 boxes, 2 ranges instead of 1 and 1). The stub has to be
  // told, because the simplification that makes it small is exactly the thing that makes it lie here.
  elements["body"].children.length = 0;
  P.renderFeature(0);

  // ⚠️ WALK THE TREE, DO NOT GUESS THE PATH. The first version of this reached for
  // `body.children[1].children[1].children[0]` and crashed -- the row/card nesting is the page's business and
  // guessing it makes the probe a copy of the layout, which is the thing most likely to change. Collecting
  // the inputs first and asserting on what they ARE keeps this about behaviour.
  function collect(cl, out) {
    (cl || []).forEach(function (c) {
      if (c && c.type === "range" && c._on) out.ranges.push(c);
      if (c && c.type === "checkbox" && c._on) out.boxes.push(c);
      if (c && c.children) collect(c.children, out);
    });
    return out;
  }
  const found = collect(elements["body"].children, { ranges: [], boxes: [] });
  // ONE RANGE AND ONE CHECKBOX, and both numbers are meaningful:
  //   * the range is the `slow` parameter from the description above;
  //   * the checkbox is the `enabled` parameter the feature describes.
  //
  // ⚠️ THE SWITCH THE PANEL USED TO DRAW HERE IS GONE -- it is on the feature LIST now (the switch that
  // replaced the state dot). This count was 2 for that reason and is 1 without it; the assertion keeps the
  // number visible so the next person can see which chrome the page is expected to add by itself.
  check("a feature page is built from the described parameters",
        found.ranges.length === 1 && found.boxes.length === 1,
        "ranges=" + found.ranges.length + " boxes=" + found.boxes.length);

  const slider = found.ranges[0];
  if (slider && slider._on.input) {
    sent.length = 0;
    slider.value = 7.5;
    slider.fire("input");
    const set = sent.filter((m) => m.cmd === "setControl");
    check("  and moving it reports the new value to the host", set.length === 1 && set[0].path === "slow",
          JSON.stringify(set[0] || null));
    check("  and moving it does NOT ask for a snapshot (which would rebuild the page under the drag)",
          !sent.some((m) => m.cmd === "snapshot"), sent.map((m) => m.cmd).join(","));
  } else {
    check("  and the slider reports changes", false, "no input handler");
  }

  // ---- THE WHEEL OVER A SLIDER ----
  //
  // The user asked for this ("插件拉杆、旋钮（以后会有）支持滚轮直接改值"), and the shape of the check is
  // deliberately about the ARITHMETIC rather than about "a handler exists":
  //
  //   * one notch of 120 must move the value by exactly ONE step (a slider that moved a pixel's worth per
  //     wheel message would need forty flicks per notch on a trackpad, and one that moved a fixed unit would
  //     ignore the step the feature described);
  //   * the direction must follow the sign of deltaY;
  //   * it must stop at the control's own bounds and not report a value that did not change;
  //   * and it must go through the SAME `setFeature` path a drag uses, with the graph redrawn -- the point is
  //     a control the user can drive, not a private number.
  //
  // The stub's `step`/`min`/`max` come from the description the probe feeds in, so these numbers are the
  // feature's own contract rather than ones written here.
  if (slider && slider._on.wheel) {
    const step = Number(slider.step) || 1;
    const lo = Number(slider.min), hi = Number(slider.max);

    slider.value = 5;
    sent.length = 0;
    slider.fire("wheel", { deltaY: -120, deltaMode: 0 });
    let set = sent.filter((m) => m.cmd === "setControl");
    check("  the wheel up moves it exactly one step", set.length === 1 && Number(set[0].value) === 5 + step,
          "value=" + (set[0] ? set[0].value : "none") + " (5 + " + step + " expected)");

    slider.value = 5;
    sent.length = 0;
    slider.fire("wheel", { deltaY: 120, deltaMode: 0 });
    set = sent.filter((m) => m.cmd === "setControl");
    check("  the wheel down moves it one step the other way",
          set.length === 1 && Number(set[0].value) === 5 - step,
          "value=" + (set[0] ? set[0].value : "none"));

    // A TRACKPAD SENDS MANY SMALL DELTAS, and the accumulator is what makes that the same RATE as a notched
    // mouse rather than the same number of events. 120 deltas in total must equal one notch, however it is
    // split.
    slider.value = 5;
    sent.length = 0;
    for (let i = 0; i < 8; i++) slider.fire("wheel", { deltaY: -15, deltaMode: 0 });
    set = sent.filter((m) => m.cmd === "setControl");
    check("  a trackpad's 120 deltas in 8 pieces still equals ONE step",
          set.length === 1 && Number(set[0].value) === 5 + step,
          "reports=" + set.length + " value=" + (set[0] ? set[0].value : "none"));

    // ... and a partial notch must do nothing at all, or a stray touchpad twitch would edit the settings.
    slider.value = 5;
    sent.length = 0;
    slider.fire("wheel", { deltaY: -20, deltaMode: 0 });
    check("  a partial notch changes nothing", sent.filter((m) => m.cmd === "setControl").length === 0,
          "reports=" + sent.filter((m) => m.cmd === "setControl").length);

    // AT THE END OF THE RANGE: clamped, and silently -- a value that did not change must not be reported, or
    // every further notch would write the file for nothing.
    slider.value = hi;
    sent.length = 0;
    slider.fire("wheel", { deltaY: -120, deltaMode: 0 });
    check("  and at the top of the range it stops, without reporting a change",
          sent.filter((m) => m.cmd === "setControl").length === 0 &&
              Number(slider.value) === hi,
          "value=" + slider.value + " reports=" + sent.filter((m) => m.cmd === "setControl").length);

    slider.value = lo;
    sent.length = 0;
    slider.fire("wheel", { deltaY: 120, deltaMode: 0 });
    check("  and at the bottom it stops too",
          sent.filter((m) => m.cmd === "setControl").length === 0 && Number(slider.value) === lo,
          "value=" + slider.value);

    // AND THE VALUE STAYS ON THE GRID. A step of 0.1 (or 0.05) does not divide its range evenly in binary
    // floating point, so repeated steps drift off the control's own grid -- at which point the browser snaps
    // the value somewhere of its own choosing and the number shown is not the number sent.
    slider.value = Number(lo);
    sent.length = 0;
    for (let i = 0; i < 12; i++) slider.fire("wheel", { deltaY: -120, deltaMode: 0 });
    set = sent.filter((m) => m.cmd === "setControl");
    const offGrid = set.filter((m) => {
      const k = (Number(m.value) - lo) / step;
      return Math.abs(k - Math.round(k)) > 1e-6;
    });
    check("  repeated steps stay exactly on the control's own grid", offGrid.length === 0,
          offGrid.length + " of " + set.length + " off-grid, e.g. " +
              (offGrid[0] ? offGrid[0].value : "-"));
  } else {
    check("  the wheel over a slider changes its value", false, "no wheel handler");
  }
}

// ---- THE CHART ANIMATES ON WHEEL ACTIVITY, NOT ON A TIMER ----
//
// ---- THE MOTION CURVE: DRAWN AT ONCE, ANIMATED ONLY BY REAL WHEEL ACTIVITY ----
//
// ⚠️ TWO THINGS ARE CHECKED, AND BOTH WERE REAL BUGS.
//
//   1. THE CARD MUST BE BUILT. The guard that decides whether to build it once asked for `curve.shape` -- the
//      shape of the data BEFORE the curve was simplified to a plain array of points. The feature sent the
//      array, the guard was false for every feature, and an unmet condition in a render function is
//      indistinguishable from a feature that sends no curve: the curve was invisible and nothing said a word.
//      NOTE WHAT THIS PROBE WAS FEEDING AT THE TIME: the old object format. A probe that feeds a format the
//      feature no longer sends is a green light over a broken page.
//
//   2. NOTHING MOVES UNTIL A WHEEL TURNS. The curve is drawn when the page is rendered, and then it is still:
//      no requestAnimationFrame is scheduled. `__apexActivity(n)` -- what the panel process calls when the host
//      reports that the feature took a wheel -- is the ONLY thing that starts motion, and the count it is given
//      is how many balls appear. (An earlier version animated on a timer, which is a picture of nothing: the
//      user's words were "是有鼠标滚轮事件才有，没有就没有".)
{
  P.controls(JSON.stringify({
    params: [{ id: "glide", type: "range", labelZh: "滑动时长", labelEn: "Glide", min: 100, max: 300,
               step: 5, value: 200, def: 200, unit: "ms" }],
    curve: chartFixture(),
  }));
  P.S.controls = JSON.parse(JSON.stringify(P.S.controls));
  elements["body"].children.length = 0;
  P.S.view = 0;
  rafQueue.clear();
  P.renderFeature(0);

  // The card is found by WALKING THE TREE, not by an id lookup: the stub's getElementById cache is only filled
  // when the page ASKS for an element by id, and the card is created with createElement and given its id
  // afterwards -- so `elements["curveCard"]` stays undefined even when the card is right there. (A probe that
  // looks in the wrong place reports "not built" for a page that built it perfectly.)
  function findById(node, want, depth) {
    if (!node || (depth || 0) > 8) return null;
    if (node.id === want) return node;
    const kids = node.children || [];
    for (let i = 0; i < kids.length; i++) {
      const hit = findById(kids[i], want, (depth || 0) + 1);
      if (hit) return hit;
    }
    return null;
  }
  const card = findById(elements["body"], "curveCard", 0);
  check("a feature that sends a curve gets a curve card", !!card);
  check("  and drawing it schedules no animation", rafQueue.size === 0,
        "pending frames=" + rafQueue.size);

  // A feature with NO curve must not get the card -- the same guard from the other side, so this cannot pass by
  // the condition being inverted.
  P.controls(JSON.stringify({ params: [], curve: null }));
  P.S.controls = JSON.parse(JSON.stringify(P.S.controls));
  elements["body"].children.length = 0;
  P.renderFeature(0);
  check("  and no curve means no card", !findById(elements["body"], "curveCard", 0));

  // AND NOW THE WHEEL. The page is on a curve card again; one wheel must produce one ball and one frame loop.
  P.controls(JSON.stringify({
    params: [],
    curve: chartFixture(),
  }));
  P.S.controls = JSON.parse(JSON.stringify(P.S.controls));
  elements["body"].children.length = 0;
  P.S.view = 0;
  rafQueue.clear();
  P.renderFeature(0);
  P.activity(2);
  check("wheel activity moves the curve", P.balls().filter(Boolean).length === 2,
        "balls=" + P.balls().filter(Boolean).length);
  check("  and starts the frame loop", rafQueue.size === 1, "pending frames=" + rafQueue.size);
  P.activity(0);
  check("  and zero activity adds nothing", P.balls().filter(Boolean).length === 2);
}

// ---- THE CHART FOLLOWS THE SLIDERS WHILE THEY ARE BEING DRAGGED ----
//
// ⚠️ THE USER'S OWN REPORT, WORD FOR WORD: "滑动滚轮 插件下方的曲线动画，原先是可以随着上面四个参数值实时变化的，
// 现在只是改完值后才动一下". The chart is drawn from the FEATURE's numbers, so following the sliders means asking
// the feature again DURING the drag. It used to do that only on `change` -- because the answer to a `describe`
// rebuilds the page, and rebuilding it mid-drag deletes the slider from under the user's finger (that is the bug
// the `change`-only refresh was written to avoid; see AGENTS §3.5.7).
//
// The two halves of the fix, and both are asserted here:
//   * while dragging, ask again (throttled, NOT debounced -- a debounce waits for the drag to pause, which is
//     exactly the "只动一下" the user saw);
//   * and the answer for that question redraws the CHART ALONE, leaving the rest of the page standing.
{
  const doc = () => JSON.stringify({
    params: [{ id: "glide", type: "range", labelZh: "滑动时长", labelEn: "Glide", min: 100, max: 300,
               step: 5, value: 200, def: 200, unit: "ms" }],
    curve: chartFixture(),
  });
  P.controls(doc());
  P.S.view = 0;
  elements["body"].children.length = 0;
  rafQueue.clear();
  P.renderFeature(0);

  const rng = firstRange(elements["body"], 0);
  check("  a page with a chart has a slider to drag", !!rng);
  const nodesBefore = elements["body"].children.length;
  const drawsBefore = canvasClears;

  if (rng) {
    sent.length = 0;
    rng.value = 210;
    rng.fire("input");
    check("  dragging a slider reports the value first",
          sent.filter((m) => m.cmd === "setControl").length === 1, JSON.stringify(sent.map((m) => m.cmd)));
    // ⚠️ NOT PER PIXEL: the refresh is on a timer, so nothing has gone out yet. (A `describe` per `input` event
    // would be a round trip per pixel of the drag, on top of the value that is already being sent.)
    check("  and asks nothing yet (the chart refresh is throttled, not per pixel)",
          sent.filter((m) => m.cmd === "describe").length === 0, JSON.stringify(sent.map((m) => m.cmd)));

    // ⚠️ THE DEBOUNCE WOULD FAIL HERE. Every event in a burst must cost ONE round trip in total -- not one each,
    // and not "none until the user stops" (which is the bug: the chart moved only when the drag ended).
    rng.value = 220;
    rng.fire("input");
    runTimers();
    check("  a burst of events costs exactly one round trip",
          sent.filter((m) => m.cmd === "describe").length === 1,
          "describes=" + sent.filter((m) => m.cmd === "describe").length);

    // ... and a further event, after the throttle's window, does go out: the chart keeps up for the whole drag.
    rng.value = 230;
    rng.fire("input");
    runTimers();
    check("  and the next one goes out when the throttle allows it",
          sent.filter((m) => m.cmd === "describe").length === 2,
          "describes=" + sent.filter((m) => m.cmd === "describe").length);

    // THE ANSWER: the chart is repainted, and the page is NOT rebuilt around it.
    // (In this stub a rebuild shows up as nodes APPENDED to #body -- the stub's `innerHTML = ""` is a plain
    // property and does not clear the children array, which is why every other block clears it by hand.)
    P.controls(doc()); // answers the first question
    P.controls(doc()); // answers the second
    check("  the answer repaints the chart", canvasClears > drawsBefore,
          "draws=" + (canvasClears - drawsBefore));
    check("  and does NOT rebuild the page under the drag",
          elements["body"].children.length === nodesBefore,
          "nodes=" + elements["body"].children.length + " before=" + nodesBefore);
  }
  // Everything the page deferred is run out here, so the blocks below start from a quiet state -- and the
  // timers are dropped rather than run: a request sent from here would leave a question with no answer behind
  // it, and the NEXT block's answer would be paired with that stale question.
  timers.length = 0;
  rafQueue.clear();
  elements["body"].children.length = 0;
}

// ---- THE BANNER MUST SURVIVE A REDRAW ----
//
// This is the bug the real panel had, and it is worth a check of its own because the symptom did not point
// at the cause: the two renderers empty #body before drawing into it (b.innerHTML = ""), and the "host is
// not running" banner used to live INSIDE #body -- so the first render deleted the banner element, and the
// next render (any click) threw "missing element #nohost" and left the page half-drawn. It looked like a
// click bug.
//
// The stub keeps elements in a map, so an element that gets removed by a redraw is not simulated by
// innerHTML = "" alone. What IS checked here is the rule that prevents it: that the banner is not a
// descendant of the container the renderers empty. That relationship is in the markup, so this reads it
// straight from the HTML text -- a structural assertion rather than a behavioural one, which is the right
// strength for a nesting rule.
{
  const bodyStart = html.indexOf('<div id="body">');
  const bannerStart = html.indexOf('<div id="banner">');
  check("the banner exists in the markup", bannerStart >= 0);

  // The banner must come BEFORE #body and not be nested inside it. Since the markup is a flat sequence with
  // balanced divs, "before #body" plus "not inside #body" is enough to assert here.
  check("  and it is not inside #body (which the renderers empty)", bannerStart > 0 && bannerStart < bodyStart);

  // And the renderers must not be able to reach it: they may only empty #body.
  const clearsBody = /b\.innerHTML\s*=\s*""/.test(js);
  check("  and the renderers do empty #body (so the rule above is load-bearing)", clearsBody);
}

// ---- A CHANGE THE HOST ANNOUNCES MUST REACH THE PAGE, AND MUST NOT BE A POLL ----
//
// The user's question about this: "「排除」标签右边那行灰字，做不到动态加载，还要拖动切换插件标签才能显示吗？"
// The answer is this callback. The host posts ONE message when a fact it watches changes (never on a timer
// tick), the panel process forwards the name, and the page re-reads what it draws.
//
// ⚠️ IT ASKS FOR A SNAPSHOT RATHER THAN RE-READING ONE PAGE, AND THIS CHECK USED TO ASSERT THE OLD BEHAVIOUR.
// The old version called `loadFeature(S.view)` and returned early on the general page -- which refreshed the
// feature page's CONTROLS and nothing else. But what the host announces is often about state it owns (which
// features are on), and that is drawn in two places the old call never touched: the sidebar's status dots, and
// the feature page's own "disabled" state -- which is part of the SNAPSHOT, not of that feature's controls. So
// flipping a switch in the quick panel left the page showing the old answer, and the user reported exactly that:
// "在设置打开的时候，点快速面板开关时，设置的开关要同步跟着实时变化".
//
// ⚠️ AND THE GENERAL PAGE IS NOT EXEMPT ANY MORE -- which is the second assertion below, reversed. "No feature
// is open" was read as "nothing to refresh", and that was wrong: the SIDEBAR is on screen whatever page is
// showing, and it is the thing with the status dots on it.
{
  // On a FEATURE page: it must ask the host for a fresh snapshot (which re-reads that page's own controls too --
  // see `__apexSnapshot` in panel.js).
  P.S.view = 0;
  P.S.snap = { host: P.S.snap.host, features: [{ slot: 0, ok: 1, id: "SmoothWheel", nameZh: "滚轮",
                                               nameEn: "Wheel", version: "1.1.0", off: 0, enabled: 1 }] };
  sent.length = 0;
  check("the page has a callback for a change the host announces", typeof P.stateChanged === "function");
  if (typeof P.stateChanged === "function") {
    P.stateChanged(1);
    const asked = sent.filter((m) => m.cmd === "snapshot");
    check("  and it asks for a fresh snapshot when told", asked.length === 1,
          JSON.stringify(sent.map((m) => m.cmd)));

    // On the GENERAL page it must STILL ask: the sidebar and its status dots are drawn from the snapshot, and
    // they are on screen whichever page is open.
    P.S.view = "general";
    sent.length = 0;
    P.stateChanged(1);
    const onGeneral = sent.filter((m) => m.cmd === "snapshot");
    check("  and it refreshes on the general page too (the sidebar lives there)", onGeneral.length === 1,
          sent.map((m) => m.cmd).join(","));

    // Before the first snapshot there is nothing to refresh, and no connection to ask through.
    P.S.view = 0;
    const keep = P.S.snap;
    P.S.snap = null;
    sent.length = 0;
    P.stateChanged(1);
    check("  and it stays quiet before the first snapshot arrives", sent.length === 0,
          sent.map((m) => m.cmd).join(","));
    P.S.snap = keep;
  }
  // Leave the page as this block found it.
  P.S.view = "general";
}

// ---- A GROUP THAT ASKS FOR MASTER LAYOUT GETS A LIST, NOT A STACK ----
//
// The user's report of the stacked version: "现在这样做是变好看了，但是不方便，规则一多，要不断往下翻". The
// feature asks for the other layout (see "layout" in apex/abi.h) and the page must honour it -- and must
// keep honouring "stack" for a group that does not ask, because a group of two items is fine and better.
{
  function rulesGroupDoc(layout, n, actions, rowToggle) {
    const fields = [
      { id: "name", type: "text", labelZh: "名称", labelEn: "Name" },
      { id: "enabled", type: "bool", labelZh: "启用", labelEn: "Enabled" },
    ];
    const items = [];
    for (let i = 0; i < n; i++) {
      items.push({ title: "规则 " + i, values: { name: "规则 " + i, enabled: 1 } });
    }
    const g = { id: "rules", type: "group", labelZh: "规则", labelEn: "Rules", fields: fields, items: items };
    if (layout) g.layout = layout;
    if (actions) g.actions = actions;
    if (rowToggle) g.rowToggle = rowToggle;
    return JSON.stringify({ params: [g] });
  }
  function countByClass(node, cls, depth) {
    if (!node || (depth || 0) > 10) return 0;
    let n = (node.className || "").split(" ").indexOf(cls) >= 0 ? 1 : 0;
    (node.children || []).forEach((c) => { n += countByClass(c, cls, (depth || 0) + 1); });
    return n;
  }
  // ⚠️ TWO DIFFERENT JOBS, SO TWO FUNCTIONS. `renderGroup` starts a CASE -- fresh controls, fresh page
  // state -- and is what every layout check wants. But a check that follows a user ACROSS a redraw (press Edit,
  // then look at what the page drew) must not reset the state it is in the middle of testing; the first
  // version reused renderGroup for both, so pressing Edit and then re-rendering threw the edit away and the
  // checks below it were asking about a page that had been reset.
  function redrawGroup() {
    elements["body"].children.length = 0;
    P.S.view = 0;
    P.renderFeature(0);
    return elements["body"];
  }
  function renderGroup(doc) {
    P.S.grpSel = {}; // selection is page state; start every case clean
    P.S.grpEdit = {};
    P.S.grpDraft = {};
    P.controls(doc);
    P.S.controls = JSON.parse(JSON.stringify(P.S.controls));
    return redrawGroup();
  }

  // master: a list container and a detail pane, with N rows in the list and only ONE item's fields built.
  const bodyMaster = renderGroup(rulesGroupDoc("master", 5));
  const rows = countByClass(bodyMaster, "item", 0);
  const details = countByClass(bodyMaster, "splitdetail", 0);
  check("a group asking for layout=master gets a list of its items", rows === 5, "rows=" + rows);
  check("  and one detail pane", details === 1, "panes=" + details);
  // ⚠️ THE POINT OF THE LAYOUT: only the SELECTED item's fields exist. If all five were built, the list would
  // be cosmetic and the page would still be five screens tall.
  const nameFields = countByClass(bodyMaster, "row", 0);
  check("  and only the selected item is built (so the page is one screen, not five)",
        nameFields > 0 && nameFields < 12, "rows in the page=" + nameFields);

  // stack: the default, for a group that does not ask -- every item built, no list.
  const bodyStack = renderGroup(rulesGroupDoc(null, 3));
  const stackRows = countByClass(bodyStack, "item", 0);
  const stackBoxes = countByClass(bodyStack, "grp", 0);
  check("a group with no layout request is stacked, as before", stackRows === 0 && stackBoxes === 3,
        "items=" + stackRows + " boxes=" + stackBoxes);

  // ⚠️ THE STRING FORM OF `rowToggle` IS THE SHIPPED CONTRACT (AutoIME's rules use it), and it is checked here
  // because the array form below is what the next feature needs: a change that handled only the array would
  // silently draw NO switch for the feature that is already in the field.
  //
  // ⚠️ AND IT IS CHECKED ON ITS OWN RENDER, IMMEDIATELY. `elements["body"]` IS ONE NODE REUSED BY EVERY RENDER
  // (getElementById caches it and every render clears its children), so `bodyMaster` above is not a snapshot of
  // the master layout -- it is a handle to whatever was drawn LAST. A check that reads it two renders later is
  // reading the wrong page. (This is not hypothetical: the first version of this check was written after the
  // stacked case and counted the STACK's switches -- 3, from the three read-only `enabled` rows -- and would
  // have read as "the string form is broken" while the page was perfect.)
  {
    const bodyToggle = renderGroup(rulesGroupDoc("master", 5, null, "enabled"));
    const sw = countByClass(bodyToggle, "sw", 0);
    check("  and one field id as `rowToggle` still draws one switch per row", sw === 5, "switches=" + sw);
  }

  // ---- ROWS: A LIST OF NAMES WITH ITS SWITCHES ON EVERY LINE ----
  //
  // The user's description of the page they wanted: "统一做成列表式，每个进程有「保持唤醒」和「阻止熄屏」两项，和
  // 一个移除按钮". It is a THIRD kind of item -- no draft to edit, no selection, no name to type -- so the page has
  // to draw it differently from both layouts above, and the three things that make it that kind are what this
  // case checks: every line carries ALL the switches (not just the first), the feature's own LOCKED row has no
  // remove button while the others have one, and adding a name goes through a text box.
  function collectByClass(node, cls, out, depth) {
    out = out || [];
    if (!node || (depth || 0) > 12) return out;
    if ((node.className || "").split(" ").indexOf(cls) >= 0) out.push(node);
    (node.children || []).forEach((c) => collectByClass(c, cls, out, (depth || 0) + 1));
    return out;
  }
  function findBy(node, pred, depth) {
    if (!node || (depth || 0) > 12) return null;
    if (pred(node)) return node;
    for (const c of (node.children || [])) { const r = findBy(c, pred, (depth || 0) + 1); if (r) return r; }
    return null;
  }
  // The buttons the page builds directly inside a row (the stub names an element by its tag, see mkEl), and
  // every button anywhere inside a container -- the bar puts its add button INSIDE a `.addrow` box, so a count
  // of direct children would read zero and "no buttons at all" is not what is being checked.
  //
  // ⚠️ A `ghost` IS NOT COUNTED: it is the invisible button that holds a locked row's column width so the
  // switches line up (see the note where it is built). It is present, it is invisible, and it has no handler.
  function buttonsIn(node) {
    return ((node && node.children) || []).filter((c) => c.id === "#button" && !isGhost(c)).length;
  }
  function isGhost(n) { return (n.className || "").split(" ").indexOf("ghost") >= 0; }
  function countButtons(node, depth) {
    if (!node || (depth || 0) > 12) return 0;
    let n = node.id === "#button" && !isGhost(node) ? 1 : 0;
    (node.children || []).forEach((c) => { n += countButtons(c, (depth || 0) + 1); });
    return n;
  }
  function rowsGroupDoc() {
    return JSON.stringify({ params: [{
      id: "rules", type: "group", labelZh: "程序名单", labelEn: "Programs",
      layout: "rows", rowToggle: ["awake", "display"],
      addHintZh: "例如 chrome.exe", addHintEn: "e.g. chrome.exe",
      fields: [ { id: "awake", type: "bool", labelZh: "保持唤醒", labelEn: "Keep awake" },
                { id: "display", type: "bool", labelZh: "阻止熄屏", labelEn: "Keep the screen on" } ],
      items: [ { titleZh: "系统全局", titleEn: "System-wide", locked: true, values: { awake: 0, display: 0 } },
               { title: "chrome.exe", values: { awake: 1, display: 0 } },
               { title: "vlc.exe", values: { awake: 1, display: 1 } } ],
    }]});
  }
  const bodyRows = renderGroup(rowsGroupDoc());
  check("a group asking for layout=rows draws one line per item",
        countByClass(bodyRows, "skiprow", 0) === 3, "lines=" + countByClass(bodyRows, "skiprow", 0));
  check("  and neither boxes nor a detail pane (it is a third kind of page)",
        countByClass(bodyRows, "grp", 0) === 0 && countByClass(bodyRows, "splitdetail", 0) === 0,
        "boxes=" + countByClass(bodyRows, "grp", 0));
  // ⚠️ BOTH SWITCHES ON EVERY LINE. `rowToggle` is an id OR an array (apex/abi.h); a page that handled only the
  // single-id form would draw ONE switch here and silently drop the other -- a missing control with the page
  // looking complete, which is the failure this probe exists for.
  check("  every line carries BOTH switches",
        countByClass(bodyRows, "sw", 0) === 6, "switches=" + countByClass(bodyRows, "sw", 0));
  // ⚠️ AND EACH ONE SAYS WHAT IT IS. Two unlabelled switches side by side are a control the user has to guess at,
  // and a tooltip is not an answer to "which one is which" for a list read at a glance.
  check("  and each switch carries its own visible label",
        countByClass(bodyRows, "swtxt", 0) === 6, "labels=" + countByClass(bodyRows, "swtxt", 0));

  const rowList = findBy(bodyRows, (n) => (n.className || "").split(" ").indexOf("rowlist") >= 0);
  check("  the lines live in their own list", !!rowList && rowList.children.length === 3, "");
  // ⚠️ AND THE COUNT IN THE HEADING IS OF THE USER'S ROWS, NOT OF THE LIST'S OWN. One of the three items here is
  // `locked` (the feature's built-in row) and cannot be added or removed by the user, so the heading must say 2 --
  // counting it made KeepAwake's page claim the user already had a program before they had added any. The user
  // reported it: "保持唤醒，程序数量中，系统全局不用计入其中."
  {
    const head = findBy(bodyRows, (n) => (n.className || "").split(" ").indexOf("listhead") >= 0);
    const note = head ? (head.children || []).filter((c) => (c.className || "") === "rownote")[0] : null;
    check("  and the heading counts the user's rows, not the feature's locked one",
          !!note && String(note.textContent) === "2",
          "count=" + (note ? note.textContent : "?") + " of 3 items");
  }
  const lockedRow = rowList && rowList.children[0];
  const plainRow = rowList && rowList.children[1];
  // ⚠️ THE LOCKED ROW IS THE FEATURE'S OWN (KeepAwake's 系统全局): it keeps its switches and loses its remove
  // button. "It cannot be deleted" is only true if the page does not offer the button -- the feature refuses the
  // op as well, and both halves are checked, but this is the half the user sees.
  check("  the feature's LOCKED row has its switches and NO usable remove button",
        !!lockedRow && countByClass(lockedRow, "sw", 0) === 2 && buttonsIn(lockedRow) === 0,
        "buttons=" + (lockedRow ? buttonsIn(lockedRow) : "?"));
  // ⚠️ AND WHAT TOOK THE BUTTON'S PLACE IS NOT A BUTTON THE USER CAN PRESS. The ghost exists to hold the column
  // width (so every row's switches line up); if it were ever wired to the remove op, the locked row would be
  // deletable from the page while every count of "usable buttons" still said zero.
  {
    const ghost = lockedRow ? (lockedRow.children || []).filter(isGhost)[0] : null;
    const wired = ghost ? ((ghost._on || {}).click || []).length : -1;
    check("    the spacer that took its place is invisible and has no handler",
          !!ghost && ghost._attr["aria-hidden"] === "true" && wired === 0,
          "ghost=" + (ghost ? "yes, handlers=" + wired : "MISSING -- the switches cannot line up"));
  }
  check("  and a normal row has exactly one remove button",
        !!plainRow && countByClass(plainRow, "sw", 0) === 2 && buttonsIn(plainRow) === 1,
        "buttons=" + (plainRow ? buttonsIn(plainRow) : "?"));

  // ⚠️ THE ADD BOX STANDS OUTSIDE THE CARD, ABOVE IT, AND THAT IS THE USER'S ARRANGEMENT: asked which of three
  // places the add row should go, they chose "列表卡片的上方（卡片外）" -- so the card is purely the list and the
  // box belongs to the page. The order inside the card is checked too, because the two mistakes this has already
  // been through were both ORDER mistakes: the add box used to sit above the list's own heading, and the heading
  // has to stay inside the card.
  {
    const block = findBy(bodyRows, (n) => (n.className || "").split(" ").indexOf("rowsblock") >= 0);
    const kids = (block && block.children) || [];
    const order = kids.map((c) => (c.className || "").split(" ")[0]);
    check("  the add box stands ABOVE the card, outside it",
          order[0] === "grpbar" && order[1] === "card", "block children: " + order.join(" "));
    const card = kids[1];
    const inCard = ((card && card.children) || []).map((c) => (c.className || "").split(" ")[0]);
    check("    and the card is only the list: its heading, then the lines",
          inCard[0] === "listhead" && inCard[1] === "rowlist", "card children: " + inCard.join(" "));
  }
  // (The feature's live read-out used to be checked here -- that it was moved into this bar rather than left
  // across the top of the page. Both the read-out and the box are gone from the program (apex/abi.h, ABI 9 -> 10:
  // the user tried it, then twice said it was not needed), so there is nothing left to place.)

  // ⚠️ A ROW THE FEATURE BUILT FOR ITSELF CARRIES ITS TITLE IN BOTH LANGUAGES, and the page must draw the
  // reader's one. An item's `title` is usually the USER's data (a rule's name) and has no translation -- but the
  // locked row is the feature's own word for something, and it cannot know which language is on screen. KeepAwake's
  // fixed "系统全局" stayed Chinese on an English page until this existed; the user reported it: "系统全局，这几个
  // 字，要随 中/英文切换".
  {
    function firstLineTitle(lang) {
      P.S.lang = lang;
      const b = renderGroup(rowsGroupDoc());
      const nm = findBy(b, (n) => (n.className || "") === "nm");
      return nm ? String(nm.textContent) : null;
    }
    const zh = firstLineTitle("zh");
    const en = firstLineTitle("en");
    P.S.lang = "zh";
    check("  a row titled in both languages follows the page's language",
          zh === "系统全局" && en === "System-wide", "zh=" + zh + " en=" + en);
    // ... and a row that has only the user's own single-spelling title is left alone.
    const plain = findBy(renderGroup(rowsGroupDoc()), (n) => (n.className || "") === "nm" &&
      String(n.textContent) === "chrome.exe");
    check("    while a row with the user's own name keeps it in either language", !!plain, "");
  }

  // ⚠️⚠️ AND A LIST THE USER CANNOT ADD TO HAS NO COUNT AT ALL (2026-09-23). The number in the heading answers "how
  // many entries have I made", which is a question about a list you maintain -- and MediaControl's three groups are
  // entirely the MACHINE's rows (`noAdd: true`, every row `locked`), so the count was a "0" that could never be
  // anything else. The user's report of it: "「亮度」「熄屏快捷键」「音量」对应的小标题右侧有个数字，是用来计数的？那个
  // 不需要，可以去掉." The other half is checked above: a list you DO maintain keeps its number.
  {
    const doc = JSON.stringify({ params: [{
      id: "displays", type: "group", labelZh: "亮度", labelEn: "Brightness",
      layout: "rows", noAdd: true, rowToggle: ["off"],
      fields: [ { id: "off", type: "bool", labelZh: "熄屏", labelEn: "Screen off" } ],
      items: [ { title: "SDC4190 2880x1800", locked: true, values: { off: 0 } },
               { title: "HKC0000 3840x2160", locked: true, values: { off: 0 } } ],
    }]});
    const b = renderGroup(doc);
    const head = findBy(b, (n) => (n.className || "").split(" ").indexOf("listhead") >= 0);
    const note = head ? (head.children || []).filter((c) => (c.className || "") === "rownote")[0] : null;
    check("  a group whose rows are the MACHINE's has no count in its heading",
          !!head && !note, note ? "count=" + note.textContent : "");
    // ⚠️ AND THE PAGE IS PUT BACK. `renderGroup` draws into the SAME container every check in this section looks
    // at (`bodyRows` IS that container, and it is cleared in place), so leaving this document standing would make
    // every check after it ask about a page that is not there -- which is exactly what happened the first time
    // this was written: the add box and the bar's buttons "disappeared".
    renderGroup(rowsGroupDoc());
  }

  // Flipping the SECOND switch must write the second field's path -- not the first one's, which is what a page
  // that assumed "the row has one switch" would do.
  {
    const sw2 = plainRow ? collectByClass(plainRow, "sw")[1] : null;
    const inp = sw2 && sw2.children[0];
    sent.length = 0;
    if (inp) { inp.checked = true; inp.fire("change"); }
    const m = sent.filter((x) => x.cmd === "setControl")[0];
    check("  flipping 阻止熄屏 writes rules[1].display, not the other field",
          !!m && m.path === "rules[1].display" && m.value === "1",
          m ? m.path + "=" + m.value : "nothing was sent");
  }
  // ⚠️ THE ADD BOX IS HOW A NAME GETS INTO A LIST OF NAMES. The feature cannot invent a program name and there
  // is no field to type one into afterwards, so `addHint*` is what asks for this control (apex/abi.h) -- and a
  // page that ignored it would leave the user with a list they cannot add to.
  {
    const addInp = findBy(bodyRows, (n) => n.id === "#input" && n.type === "text");
    const addBtn = findBy(bodyRows, (n) => n.id === "#button" && n.className === "addbtn");
    check("  it asks for the add box and shows the feature's own hint",
          !!addInp && addInp.placeholder === "例如 chrome.exe",
          addInp ? String(addInp.placeholder) : "no text box");
    sent.length = 0;
    if (addInp && addBtn) { addInp.value = "  chrome.exe  "; addBtn.fire("click"); }
    const m = sent.filter((x) => x.cmd === "listOp")[0];
    check("  and pressing Add sends the typed name (trimmed) to the feature",
          !!m && m.op === "add" && m.value === "chrome.exe",
          m ? m.op + "=" + JSON.stringify(m.value) : "nothing was sent");
  }
  // ⚠️ AND NO "DELETE THE SELECTED ONE" IN THIS LAYOUT: a rows list has no selection, so that button would act
  // on a row the page never highlighted -- a control that deletes something the user cannot see.
  {
    const bar = findBy(bodyRows, (n) => (n.className || "").split(" ").indexOf("grpbar") >= 0);
    check("  the bar holds the add box and nothing that acts on a selection",
          !!bar && countButtons(bar, 0) === 1, "bar buttons=" + (bar ? countButtons(bar, 0) : "?"));
  }

  // ---- THE GROUP'S OWN QUICK-PANEL SWITCH (apex/abi.h, `quick`, ABI 16 -> 17) ----
  //
  // The user asked for this in three places in one breath, and the three are ONE rule: the switch that maps a
  // group into the flyout is drawn at the right-hand end of the line that already carries that group's own
  // controls. WHICH line follows from what the group already has --
  //
  //   * "保持唤醒插件的快速面板给一个开关，放在「添加」按钮右侧，居右" -- the group whose rows the user adds has an
  //     add row, so the switch goes with it;
  //   * "媒体控制插件的「快速面板」开关，位置移到亮度和音量各自小标题的右侧，居右" -- a group whose rows come from
  //     the machine has no add row, so its heading is its control line;
  //   * a STACK or a MASTER group has no heading of its own at all (`listhead` is built for a rows list only), so
  //     the bar is the only line left.
  //
  // ⚠️ AND IT IS NOT A ROW AMONG THE PARAMETERS, which is what it used to be for two of these features: a stack
  // of switches at the bottom of the page, away from the group each one was about.
  function quickGroupDoc(opts) {
    const o = opts || {};
    const g = {
      id: "displays", type: "group", labelZh: "亮度", labelEn: "Brightness", layout: "rows", noAdd: true,
      quick: { id: "quick_brightness", labelZh: "快速面板", labelEn: "Quick panel", value: 0 },
      rowToggle: ["off"],
      fields: [ { id: "off", type: "bool", labelZh: "熄屏", labelEn: "Screen off" } ],
      items: [ { title: "SDC4190 2880x1800", locked: true, values: { off: 0 } } ],
    };
    if (o.addHint) {
      g.addHintZh = "例如 chrome.exe"; g.addHintEn = "e.g. chrome.exe"; g.noAdd = false;
    }
    if (o.actions) g.actions = [{ op: "refresh", labelZh: "刷新应用列表", labelEn: "Refresh applications" }];
    if (o.noQuick) delete g.quick;
    if (o.master) {
      delete g.layout; delete g.noAdd; delete g.rowToggle;
      g.fields = [{ id: "name", type: "text", labelZh: "名称", labelEn: "Name" }];
      g.items = [{ title: "规则 0", values: { name: "规则 0" } }];
    }
    return JSON.stringify({ params: [g] });
  }
  const byClass = (node, cls) =>
    (node && node.children ? node.children : []).filter((c) => (c.className || "").split(" ").indexOf(cls) >= 0);
  const grpbarIn = (b) => findBy(b, (n) => (n.className || "").split(" ").indexOf("grpbar") >= 0);
  const headIn = (b) => findBy(b, (n) => (n.className || "").split(" ").indexOf("listhead") >= 0);

  {
    const b = renderGroup(quickGroupDoc());
    const head = headIn(b);
    const q = head ? byClass(head, "quicksw")[0] : null;
    check("a machine's own list draws its quick-panel switch on its heading line", !!q,
          "in heading=" + !!q);
    // ⚠️ AND THE LABEL IS THE FEATURE'S, IN THE READER'S LANGUAGE -- the page does not carry a translation for a
    // control it knows nothing about (the same rule as every other label here).
    const txt = q ? byClass(q, "swtxt")[0] : null;
    check("  carrying the label the feature sent, in the page's language",
          !!txt && String(txt.textContent) === "快速面板", "label=" + (txt ? txt.textContent : "?"));
    // ⚠️ AND IT WRITES THE PATH THE FEATURE NAMED. The id is a `setControl` path like any other; a page that
    // invented one (the group's own id, say) would map nothing while looking perfectly wired.
    const inp = q ? byClass(q, "sw")[0] : null;
    const box = inp ? inp.children[0] : null;
    sent.length = 0;
    if (box) { box.checked = true; box.fire("change"); }
    const m = sent.filter((x) => x.cmd === "setControl")[0];
    check("  and flipping it sets the control the GROUP named",
          !!m && m.path === "quick_brightness" && m.value === "1",
          m ? m.path + "=" + m.value : "nothing was sent");
    // ... and it does not conjure a bar for a group that has nothing to put in one.
    check("    without inventing a bar the group does not have", !grpbarIn(b), "");
  }
  {
    const b = renderGroup(quickGroupDoc({ addHint: true }));
    const bar = grpbarIn(b);
    const q = bar ? byClass(bar, "quicksw")[0] : null;
    check("a list the user ADDS to draws the switch in the add row instead", !!q, "in bar=" + !!q);
    check("  and last in that row, so it sits to the right of Add",
          !!q && (bar.children || [])[bar.children.length - 1] === q, "");
  }
  {
    // ⚠️⚠️ AND A ROWS GROUP THAT DECLARES AN ACTION KEEPS ITS BAR -- which it did not until this was written.
    // `barItems` is what decides whether a rows group's bar is drawn, and the actions loop never counted itself,
    // so MediaControl's 「刷新应用列表」 button was built and then thrown away. Nothing caught it: the only fixture
    // with actions was a master-layout group, where the bar is appended regardless.
    const b = renderGroup(quickGroupDoc({ actions: true }));
    const bar = grpbarIn(b);
    check("a rows group that declares an action still draws its bar", !!bar, "bar=" + !!bar);
    check("  with the action's own button in it (it used to be discarded with the bar)",
          !!bar && countByClass(bar, "action", 0) === 1, "actions=" + (bar ? countByClass(bar, "action", 0) : "?"));
    // ... and the switch still belongs to the HEADING: the user asked for it beside 音量, which is a heading, not
    // beside the refresh button.
    const head = headIn(b);
    check("  and the quick-panel switch is still on the heading, not in that bar",
          !!head && byClass(head, "quicksw").length === 1 && byClass(bar, "quicksw").length === 0, "");
  }
  {
    // A group with no heading of its own (a stack or a master list) has only its bar to put the switch on.
    const b = renderGroup(quickGroupDoc({ master: true }));
    const bar = grpbarIn(b);
    check("a group with no heading of its own draws the switch in its bar",
          !!bar && countByClass(bar, "quicksw", 0) === 1 && !headIn(b), "");
  }
  {
    const b = renderGroup(quickGroupDoc({ noQuick: true }));
    check("a group that maps nothing gets no such switch", countByClass(b, "quicksw", 0) === 0, "");
  }

  // ---- A LIVE GROUP: THE PAGE RE-READS IT BY ITSELF (apex/abi.h, `live`, ABI 17 -> 18) ----
  //
  // ⚠️ IT REPLACED A BUTTON, IN THE USER'S OWN WORDS: "媒体控制列表的「刷新应用列表」按钮去掉，这个做成实时自动刷新".
  // What has to be true on this side is that the page starts asking by itself while such a group is on screen, at
  // the rate it promised, that the asking stops when the page is left, and that a group which is NOT live does not
  // cost anything -- because "the rows are a picture of something outside the page" is a fact only the feature
  // knows (KeepAwake's program list is a list the USER maintains, and re-reading that for ever would be work
  // nobody asked for).
  function liveGroupDoc(live) {
    const g = {
      id: "sessions", type: "group", labelZh: "音量", labelEn: "Volume", layout: "rows", noAdd: true,
      rowToggle: ["mute"],
      fields: [ { id: "volume", type: "range", min: 0, max: 100, step: 1, labelZh: "", labelEn: "", value: 50 },
                { id: "mute", type: "bool", labelZh: "静音", labelEn: "Mute" } ],
      items: [ { title: "chrome.exe", locked: true, values: { volume: 50, mute: 0 } } ],
    };
    if (live) g.live = true;
    return JSON.stringify({ params: [g] });
  }
  {
    renderGroup(liveGroupDoc(true));
    check("a group that says its rows are LIVE makes the page re-read it by itself",
          intervalsOf(1000).length === 1, "live timers=" + intervalsOf(1000).length);
    // ⚠️ SLOWER THAN THE CAPTURE WAIT ON PURPOSE: that one is chasing a click the user just made and ends by
    // itself; this one runs for as long as the page is open.
    check("  at one second, not at the capture wait's 200 ms",
          intervalsOf(1000).length === 1 && intervalsOf(200).length === 0, "");

    // ... and an answer that has not changed asks for nothing else: one tick sends one `describe`, and the page
    // does not redraw from it (that guard is elsewhere -- see `__apexControls`).
    sent.length = 0;
    intervalsOf(1000)[0].fn();
    // (Its answer, delivered here so the queue below is balanced: every tick below is followed by the answer to
    //  THAT tick, and the last check asks the question the ordinary way.)
    P.controls(liveGroupDoc(true));
    check("  a tick asks the host for the page again",
          sent.filter((x) => x.cmd === "describe").length === 1, JSON.stringify(sent));

    // ⚠️⚠️ AND AN UNCHANGED ANSWER TO THAT TICK IS NOT DRAWN. This is the half that makes a once-a-second re-read
    // affordable at all: `renderFeature` rebuilds `#body` from scratch, so drawing it once a second would take the
    // focus out of whatever the user is typing -- a second after they started typing, for ever.
    //
    // ⚠️ A REBUILD IS COUNTED, NOT SOUGHT BY IDENTITY: in this stub `innerHTML = ""` is a plain property write, so
    // it does not touch `children` and a redraw shows up as nodes APPENDED to #body (see the note at line 773).
    {
      const before = elements["body"].children.length;
      intervalsOf(1000)[0].fn();                 // the quiet ask
      P.controls(liveGroupDoc(true));            // ... and exactly the same document comes back
      check("  an unchanged answer to a live check does not rebuild the page",
            elements["body"].children.length === before,
            "nodes appended=" + (elements["body"].children.length - before));
    }

    // ⚠️⚠️ AND THE OTHER HALF, WHICH IS THE ONE THAT COULD HAVE BEEN BROKEN BY IT: an answer to a WRITE is drawn
    // even when its text is identical, because that is how a feature that REFUSED a value puts the control back.
    // The page clicked "on", the feature kept "off", and the document it sends is the "off" the page already
    // had -- skip that redraw and the switch shows a state the feature does not have.
    {
      const before = elements["body"].children.length;
      P.controls(liveGroupDoc(true));            // the same text, asked for by refreshControls (not quiet)
      check("  but a control write's answer is drawn even when it is identical",
            elements["body"].children.length > before,
            "nodes appended=" + (elements["body"].children.length - before));
    }

    // ⚠️ AND AN ABANDONED PAGE STOPS ASKING. The timer may still be armed for a moment, so the guard is on the
    // ASK, not on the tick: leaving a page whose group is live must not leave a message a second going out for it
    // for the rest of the session.
    sent.length = 0;
    P.S.view = 7;
    intervalsOf(1000)[0].fn();
    check("  and a page the user has left asks nothing at all",
          sent.filter((x) => x.cmd === "describe").length === 0, JSON.stringify(sent));
    P.S.view = 0;

    // ⚠️ AND THE TIMER ITSELF GOES WHEN THE PAGE DOES (`loadFeature` is what navigates -- the same line that stops
    // the capture watch). Left armed, it would wake up once a second for ever.
    P.S.view = 9;
    P.loadFeature(0);
    check("  and navigating away clears the timer, so nothing wakes up once a second for ever",
          intervalsOf(1000).length === 0, "live timers=" + intervalsOf(1000).length);

    // A group that does NOT say it is live costs no timer at all.
    renderGroup(liveGroupDoc(false));
    check("a group that does not say so is not re-read at all", intervalsOf(1000).length === 0, "");

    // Put the page back the way the rest of the probe expects it (see the note on the shared container above).
    renderGroup(rowsGroupDoc());
  }

  // ---- A FEATURE'S OWN ACTION BUTTONS ----
  //
  // The feature declares them and the page draws them (see `actions` in apex/abi.h). This is the half of
  // "capture" that lives in the page: the op is implemented in Rust and the button is what reaches it, so a
  // page that ignored `actions` would leave the user with no way to press it and NOTHING would report a fault
  // -- the same silent-missing-control failure the note at the top of this file is about.
  const acts = [{ op: "capture", labelZh: "点击捕获", labelEn: "Capture by click" }];
  const bodyActions = renderGroup(rulesGroupDoc("master", 3, acts));
  const buttons = countByClass(bodyActions, "action", 0);
  check("a group that declares actions gets one button each", buttons === 1, "buttons=" + buttons);
  // ⚠️ AND IT SITS IN THE LIST'S BAR, WITH Add AND Delete. The buttons that act on the LIST belong together
  // above it -- the user's own arrangement, taken from the standalone program ("添加、删除 规则按钮放列表上面").
  // This assertion used to require the detail pane, which was wrong once the buttons moved; what it must keep
  // checking is that the button EXISTS and is reachable, not which corner of the card it is parked in.
  const barHasAction = (function findAction(node, depth) {
    if (!node || (depth || 0) > 10) return false;
    if ((node.className || "").split(" ").indexOf("grpbar") >= 0 &&
        countByClass(node, "action", 0) > 0) return true;
    return (node.children || []).some(function (c) { return findAction(c, (depth || 0) + 1); });
  })(bodyActions, 0);
  check("  and it sits in the list's bar, with Add and Delete", barHasAction === true, "in bar=" + barHasAction);
  // And Add/Delete are there too, above the list rather than after its last row.
  const barButtons = (function countBar(node, depth) {
    if (!node || (depth || 0) > 10) return 0;
    if ((node.className || "").split(" ").indexOf("grpbar") >= 0) return (node.children || []).length;
    return (node.children || []).reduce(function (n, c) { return n + countBar(c, (depth || 0) + 1); }, 0);
  })(bodyActions, 0);
  check("  and Add/Delete are in that bar too", barButtons >= 3, "bar buttons=" + barButtons);
  // A group that declares none gets none -- otherwise every group would sprout buttons.
  const bodyNoActions = renderGroup(rulesGroupDoc("master", 3));
  check("a group with no actions gets no buttons", countByClass(bodyNoActions, "action", 0) === 0,
        "buttons=" + countByClass(bodyNoActions, "action", 0));

  // Put the page back the way the rest of the probe expects it.
  P.S.view = "general";
}

// ---- the QUICK PANEL'S OWN SMALL PANEL: which PANES are in it, and in what order ----
//
// ⚠️ THE USER ASKED FOR THREE THINGS IN ONE SENTENCE AND ALL THREE ARE CHECKED HERE: "把快速面板功能单独给一个小面板，
// 在两个总开关下面实时给出当前有映射在快速面板的功能，并能实现上下排序。总开关不参与排序，固定最上面." And then a
// correction that changed the UNIT of the list: "快速面板的局部功能分组是按单个开关算的，不是按插件算，所以这边排序
// 要注意" -- so a row is a PANE (one control, or one named group), with the name the flyout prints as its heading.
//
// What the page is responsible for is narrow on purpose: it DRAWS the list the host hands it (`quickOrder` in the
// snapshot), and it hands the order BACK after a drag. It never works out which pane exists, and it never sorts --
// both are the host's answer, because the flyout draws from the same one.
{
  const base = { host: P.S.snap.host, features: P.S.snap.features };
  function findByClass(node, cls, depth) {
    if (!node || (depth || 0) > 8) return null;
    if ((node.className || "").split(" ").indexOf(cls) >= 0) return node;
    for (const c of (node.children || [])) { const r = findByClass(c, cls, (depth || 0) + 1); if (r) return r; }
    return null;
  }
  function generalWith(order) {
    const snap = { host: base.host, features: base.features };
    if (order) snap.quickOrder = order;
    P.S.snap = snap;
    P.S.view = "general";
    elements["body"].children.length = 0;
    P.renderGeneral();
    return elements["body"];
  }
  const rowsIn = (body) => {
    const list = findByClass(body, "qolist");
    return list ? list.children.slice() : [];
  };

  // The panes the host would report, in ITS order: two from ONE feature (which is the case a per-feature list
  // could not express) and one from another.
  const panes = [{ key: "MediaControl|音量", nameZh: "音量", nameEn: "Volume" },
                 { key: "KeepAwake|保持唤醒", nameZh: "保持唤醒", nameEn: "Keep awake" },
                 { key: "MediaControl|亮度", nameZh: "亮度", nameEn: "Brightness" }];
  {
    const body = generalWith(panes);
    const rows = rowsIn(body);
    check("the general page lists the panes the host says are in the flyout",
          rows.length === 3, "rows=" + rows.length);
    // ⚠️ IN THE ORDER THE HOST SENT -- the flyout's order, not the sidebar's and not a sorted one. Two panes of
    // ONE feature appear separately and in the order given, which is the whole point of the unit being a pane.
    const names = rows.map((r) => {
      const nm = (r.children || []).filter((c) => (c.className || "") === "nm")[0];
      return nm ? String(nm.textContent) : "?";
    });
    check("  in the host's own order (the order the flyout draws), pane by pane",
          names.join("|") === "音量|保持唤醒|亮度", names.join(" | "));
    // ⚠️ AND THE TWO MASTER SWITCHES ARE NOT IN IT: they are not panes and have no order ("总开关不参与排序，
    // 固定最上面"). They stay in the row above, where they have always been.
    const switchRows = rows.filter((r) =>
      (r.children || []).some((c) => (c.className || "").split(" ").indexOf("sw") >= 0)).length;
    check("  and the two master switches are not part of the list", switchRows === 0,
          "switch rows in the list=" + switchRows);

    // A DRAG SENDS THE WHOLE ORDER, once, as the keys in their new sequence.
    sent.length = 0;
    rows[0].fire("dragstart");
    rows[2].fire("drop");
    const m = sent.filter((x) => x.cmd === "setHost" && x.key === "quickorder")[0];
    check("  dragging a row sends the whole new order to the host",
          !!m && m.value === "KeepAwake|保持唤醒,MediaControl|亮度,MediaControl|音量",
          m ? JSON.stringify(m.value) : "nothing was sent");
    check("    as ONE message, not one per row it passed",
          sent.filter((x) => x.cmd === "setHost").length === 1,
          JSON.stringify(sent.filter((x) => x.cmd === "setHost")));
    // ⚠️⚠️ COMMAS, AND NEVER A NEWLINE -- THIS IS THE BUG THE USER FOUND ("快速面板分组不能调顺序"): the request
    // protocol is `key=value` LINES, so a value carrying a newline arrives TRUNCATED AT ITS FIRST LINE. The host
    // saw one key, and a drag could move at most one row. A comma is safe as the separator because the key builder
    // (QuickBlockKey) keeps commas out of the keys themselves; a newline is not a separator at all, it is a cut.
    check("    and the keys are comma-separated, with NO newline anywhere in the value",
          !!m && m.value.indexOf("\n") < 0 && m.value.indexOf("\r") < 0 && m.value.split(",").length === 3,
          m ? JSON.stringify(m.value) : "");
  }
  {
    // Nothing in the flyout: the card says so in words rather than showing an empty box.
    const body = generalWith([]);
    check("a page with nothing mapped says so instead of drawing an empty card",
          rowsIn(body).length === 0 && countByClass(body, "note", 0) >= 1, "");
    // ... and an older host that sends no `quickOrder` at all is the same case, not a crash.
    const body2 = generalWith(null);
    check("  and a snapshot with no such key is that same case", rowsIn(body2).length === 0, "");
  }
  // Put the page back the way the rest of the probe expects it.
  P.S.snap = { host: base.host, features: base.features };
  P.S.view = "general";
}

// ---- a BAD document must be reported, not swallowed ----
// ---- NO CONTROL MAY SEND A CHANGE WHILE IT IS BEING BUILT ----
//
// The live bug the real panel had, and the one no other check here can see: the panel looked correct and the
// host looked correct, but the SETTINGS MOVED ON THEIR OWN. Filling a <select> fires `change` as the options
// go in, the handler sent setHost, the host answered with a snapshot, the snapshot re-rendered the select,
// and round it went -- the host's log showed lang and theme walking through their own values with nobody
// touching anything:
//
//     setHost lang=zh -> lang=auto -> lang=en -> theme=dark -> theme=light -> ...
//
// The fix is a guard in the page (see selectRow), and this is the check that keeps it: rendering the page
// repeatedly must produce no setHost at all. A rebuild is not an edit.
{
  const before = sent.length;
  P.renderGeneral();
  P.renderGeneral();
  P.renderGeneral();
  const spurious = sent.slice(before).filter((m) => m.cmd === "setHost");
  check("rendering the page repeatedly sends no setHost", spurious.length === 0,
        spurious.length ? spurious.length + " sent, e.g. " + JSON.stringify(spurious[0]) : "");
}

// ---- A SECOND FEATURE, WHOSE CONTROLS THE PAGE HAS NEVER SEEN ----
//
// ⚠️ THIS IS THE ONE THAT KEEPS "A FEATURE IS A FOLDER" TRUE. Everything else here exercises the page with
// SmoothWheel's own parameters, so the page could be secretly built around them and every check would still
// pass -- and the failure would only appear the day a second feature shipped. So the whole page is driven
// with a control set that shares NOTHING with the first one: different ids, a bool first, one range, no
// `curve`, and a `hue` the page has not been given before.
//
// The document below is the REAL one, taken from a throwaway second feature (features/AuditStub) that was
// built and loaded by the host for this audit -- not a document written to suit the page.
//
// What must hold: it renders, it reports changes under the ids IT chose, and it invents no complaint about
// the missing curve.
{
  const before2 = sent.length;
  P.controls(JSON.stringify({
    params: [
      { id: "on", type: "bool", labelZh: "开启", labelEn: "On", value: 1 },
      { id: "amount", type: "range", labelZh: "数量", labelEn: "Amount", min: 1, max: 10, step: 1,
        value: 3, def: 3, unit: "d", hue: "#78BEFF" },
    ],
  }));
  P.S.controls = JSON.parse(JSON.stringify(P.S.controls));
  elements["body"].children.length = 0;
  P.S.view = 0;
  P.renderFeature(0);

  check("a SECOND feature's controls render, with ids the page has never seen", true);
  // Its own walker: `findById` above is scoped to the block that defined it, and reaching across blocks is
  // what made a previous version of this probe throw "not a function" in the middle of the checks.
  function hasId2(node, want, depth) {
    if (!node || (depth || 0) > 8) return false;
    if (node.id === want) return true;
    return (node.children || []).some((c) => hasId2(c, want, (depth || 0) + 1));
  }
  check("  and it drew no curve card (the document has none)",
        !hasId2(elements["body"], "curveCard", 0), "");

  // The page must report under the SECOND feature's own id -- not a name it remembers from the first.
  function collect2(node, out) {
    if (!node) return out;
    if (node.type === "range" && node._on) out.ranges.push(node);
    if (node.type === "checkbox" && node._on) out.boxes.push(node);
    (node.children || []).forEach((c) => collect2(c, out));
    return out;
  }
  // ONE RANGE, AND EXACTLY THE FEATURE'S OWN SWITCH -- NO GENERIC ONE.
  //
  // ⚠️ THIS COUNT WAS 2 UNTIL THE ENABLE SWITCH MOVED TO THE FEATURE LIST ("插件页各自的启用要去了，留下插件
  // 列表那个开关即可"). The box count is asserted rather than ignored because it is what tells "the page drew the
  // feature's controls" apart from "the page drew its own chrome" -- and the chrome changed, so the number did.
  const f2 = collect2(elements["body"], { ranges: [], boxes: [] });
  check("  with its own controls (one range, and only the feature's own switch)",
        f2.ranges.length === 1 && f2.boxes.length === 1,
        "ranges=" + f2.ranges.length + " boxes=" + f2.boxes.length);
  if (f2.ranges.length) {
    sent.length = 0;
    f2.ranges[0].value = 7;
    f2.ranges[0].fire("input");
    const set = sent.filter((m) => m.cmd === "setControl");
    check("  and reports them under their OWN id", set.length === 1 && set[0].path === "amount",
          JSON.stringify(set[0] || null));
  }
  check("  and nothing about the second feature was refused or threw",
        sent.slice(before2).every((m) => m.cmd !== "pageError"),
        sent.slice(before2).map((m) => m.cmd).join(","));
  sent.length = before2; // leave the stream as this block found it
}

// ---- a BAD document must be reported, not swallowed ----
sent.length = 0;
P.snapshot("{not json");
check("a malformed snapshot is reported to the host",
      sent.some((m) => m.cmd === "pageError"));

console.log("");
if (failures === 0) {
  console.log("OK: the panel's script boots, connects, lists, and renders");
} else {
  console.log("FAILED: " + failures);
}
process.exit(failures === 0 ? 0 : 1);
