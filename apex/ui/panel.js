"use strict";

// ---------------------------------------------------------------------------
// THE PANEL'S JOB, IN ONE LINE: ask the host for the state, draw it, and tell the host when the user
// changes something.
//
// THE PAGE IS DUMB ON PURPOSE. It knows about sliders, switches and lists -- general shapes -- and nothing
// about smoothing, or about any particular feature. The host sends a description of the controls
// (settingsJson), and this renders whatever it is given. That is why a second feature needs no UI work: it
// describes its own controls and they appear.
//
// WHICH MEANS: no feature name, control name, or unit may be hard-coded here. Everything visible comes
// from the host.
// ---------------------------------------------------------------------------

var TEXT = {
  zh: {
    slogan: "端，最前者也。",
    general: "通用",
    generalTitle: "通用设置",
    language: "语言",
    theme: "外观",
    autostart: "开机启动",
    quickPanel: "快速面板",
    quickCompact: "插件总开关",
    quickOwn: "局部功能开关",
    quickOrder: "快速面板里的功能",
    quickOrderHint: "拖动调整顺序",
    quickOrderEmpty: "还没有功能映射到快速面板（在插件页里打开它自己的「快速面板」开关）",    auto: "跟随系统",
    light: "浅色",
    dark: "深色",
    add: "添加",
    remove: "移除",
    empty: "（空）",
    settingsFile: "打开设置文件",
    noHost: "Apex 主程序没有在运行，设置无法生效。",
    loading: "正在载入设置…",
    features: "功能",
    off: "已停用",
    failed: "加载失败",
    curveNote: "参数变化时，运动曲线会实时更新。",
    curveTime: "时间 →",
    curveSpeed: "速度",
    enabledOff: "该功能已停用",
    edit: "编辑",
    save: "保存",
    saved: "已保存。",
    saveFailed: "保存没有生效，请再点一次「保存」。",
    nothingToSave: "没有修改。",
    captureArmed: "已进入捕获状态：点击你要捕获的控件。",
    captureNeedsEdit: "先点「编辑」进入可编辑状态，再点捕获。",
    captureKeyNoRule: "先在左侧选中一条规则，再按 Ctrl+Alt+Q。",
    hotkeyRecord: "点击后按下新的快捷键",
    hotkeyRecording: "按下组合键…（Esc 取消）",
    hotkeyTaken: "这个快捷键已经被「点击捕获」占用，换一个。",
    hotkeyUnsupported: "这个键不能用（方向键、Home 之类本功能不支持），换一个。",
    hotkeyCleared: "已清除。",
  },
  en: {
    slogan: "Apex. The first point.",
    general: "General",
    generalTitle: "General",
    language: "Language",
    theme: "Appearance",
    autostart: "Start with Windows",
    quickPanel: "Quick panel",
    quickCompact: "Feature switches",
    quickOwn: "Feature controls",
    quickOrder: "Features in the quick panel",
    quickOrderHint: "Drag to reorder",
    quickOrderEmpty: "No feature is mapped into the quick panel yet (open its own \"Quick panel\" switch on its page)",
    auto: "Follow system",
    light: "Light",
    dark: "Dark",
    add: "Add",
    remove: "Remove",
    empty: "(empty)",
    settingsFile: "Open the settings file",
    noHost: "The Apex host is not running, so changes cannot take effect.",
    loading: "Loading settings…",
    features: "Features",
    off: "disabled",
    failed: "failed to load",
    curveNote: "The motion curve follows the parameters as you move them.",
    curveTime: "time →",
    curveSpeed: "speed",
    enabledOff: "this feature is disabled",
    edit: "Edit",
    save: "Save",
    saved: "Saved.",
    saveFailed: "The save did not take effect -- press Save again.",
    nothingToSave: "Nothing was changed.",
    captureArmed: "Capture armed: click the control you want.",
    captureNeedsEdit: "Press Edit first, then capture.",
    captureKeyNoRule: "Select a rule on the left first, then press Ctrl+Alt+Q.",
    hotkeyRecord: "Click, then press the keys",
    hotkeyRecording: "Press the combination… (Esc cancels)",
    hotkeyTaken: "Capture already uses that shortcut -- try another.",
    hotkeyUnsupported: "That key cannot be used here -- try another.",
    hotkeyCleared: "Cleared.",
  }
};

var S = {
  lang: "en",
  theme: "auto",
  view: "general",  // "general" or a feature slot
  snap: null,
  controls: null,
  // ⚠️ THREE STATES. `null` = the first snapshot has not come back yet (nobody knows), `false` = the host is
  // not there, `true` = it answered. It used to be a plain `false` at boot, which made "not answered yet" and
  // "the host is dead" the same thing -- so every launch began with a red "the host is not running" for as long
  // as the first round trip took. See renderConn.
  connected: null
};

function t(k) { return (TEXT[S.lang] && TEXT[S.lang][k]) || TEXT.en[k] || k; }

// The page's only way to say something went wrong. It cannot log anywhere the host can read, so it sends
// the text over the same channel a settings change uses, and the host puts it in its log.
function reportError(what) {
  try { window.chrome.webview.postMessage({ cmd: "pageError", what: String(what) }); } catch (e) {}
}

function $(id) {
  var e = document.getElementById(id);
  if (!e) reportError("missing element #" + id);
  return e;
}
function el(tag, cls, txt) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (txt !== undefined && txt !== null) e.textContent = txt;
  return e;
}

// ---- talking to the host -----------------------------------------------------
//
// THE PANEL IS A CLIENT OF A SEPARATE PROCESS. Every call is a window message; when the host is not
// running the call simply fails, which is a state the panel has to show rather than crash on -- the host
// can be closed while the panel is open.
//
// `window.chrome.webview.postMessage` is the one-way channel to the host's C++ (used to send a request),
// and the host calls `window.__apexReceive(json)` below for every answer. That split is what WebView2
// gives: a message out, and a script call in.
function hostCall(cmd, fields) {
  var req = { cmd: cmd };
  if (fields) for (var k in fields) req[k] = fields[k];
  try { window.chrome.webview.postMessage(req); }
  catch (e) { return false; }
  return true;
}

// Called FROM C++ for every document the host sends.
//
// A PARSE ERROR IS REPORTED, NOT SWALLOWED. The first version caught and returned, which turned a broken
// document into a panel that simply stayed empty -- every layer above reported success and there was
// nothing anywhere saying why. The host's log is the only place that can be looked at from outside, so the
// error goes there.
window.__apexSnapshot = function (json) {
  try { S.snap = JSON.parse(json); }
  catch (e) { reportError("snapshot: " + e.message); return; }
  S.connected = true;
  applyHostToUi();
  // ⚠️ THE BANNER HAS TO BE TOLD TOO. Setting `connected` is not enough: the warning was shown at boot
  // (when the panel really was not connected yet), and nothing hid it again -- so a host that was running
  // perfectly well sat behind a red "the Apex host is not running", which is worse than no banner at all.
  //
  // EVERY WRITE TO `connected` IS FOLLOWED BY renderConn(), and there are three: here, __apexHostGone and
  // __apexHostBack. (renderAll calls renderConn as well, but it does not change the flag -- it is a redraw,
  // not a change of state, and an earlier version of this comment claimed otherwise.)
  renderConn();
  renderNav();
  if (S.view === "general") renderGeneral();
  else loadFeature(S.view);
};

window.__apexControls = function (json) {
  // ⚠️ WHICH SLOT THIS ANSWER IS FOR: the oldest question still outstanding (see `pendingDescribes`). An answer
  // with no question behind it cannot be placed, and placing it by guesswork is the bug this replaces.
  var q = pendingDescribes.length ? pendingDescribes.shift() : { slot: String(S.view), curveOnly: false };
  var forSlot = q.slot;
  // ⚠️ AN UNCHANGED ANSWER WHILE A WAIT IS RUNNING IS NOT NEWS, AND REDRAWING IT WOULD BE A BUG. The wait polls
  // `describe` four times a second and every delivery rebuilds the page (`renderFeature` wipes `#body`), which
  // would take the keyboard focus out of whatever the user is typing, four times a second, for as long as the
  // wait lasts. So the comparison is on the RAW TEXT, before anything is replaced.
  //
  // ⚠️⚠️ AND THIS IS ALL IT MEANS NOW. It used to ALSO decide that the wait was over ("the document changed,
  // so the capture landed") -- and that is how a captured rule came to stay invisible until something else
  // re-read the page: the host answers every `listOp` with a snapshot, the page re-reads after it, that
  // delivery differs from the one taken when the wait began, and the watch was declared finished before the
  // user had even clicked. The user's report: "捕获事件进行时，鼠标点击后结果要马上给到参数页，目前没有，要等到
  // 点击新建的规则条才会出现". What ends a wait is now the FEATURE saying it is over -- the `waiting` field in
  // abi.h, read below -- so this guard is back to being only about not redrawing.
  if (captureWatch && forSlot === String(captureWatch.slot) && json === captureWatch.last) return;
  try { S.controls = JSON.parse(json); }
  catch (e) { reportError("controls: " + e.message); return; }
  // ⚠️ AND AN ANSWER FOR A PAGE THE USER HAS LEFT IS DROPPED, not drawn. `S.controls` has already been set --
  // it belongs to that other slot and the next visit asks again -- but the view on screen is not it.
  if (forSlot !== String(S.view)) return;
  // ⚠️ A CURVE-ONLY ANSWER TOUCHES THE CHART AND NOTHING ELSE -- no `renderFeature`, no rebuilt controls, so
  // the slider the user is dragging keeps its element, its focus and its position. Everything the chart needs
  // was stored above; the chart is redrawn from it now.
  if (q.curveOnly) {
    redrawCurveInPlace();
    return;
  }
  // ⚠️ AND A LIVE GROUP'S OWN RE-READ, WHEN NOTHING HAS CHANGED, IS NOT DRAWN AT ALL (see `askControls` for why
  // the reason matters): it asks once a second for as long as the page is open, and rebuilding the page on every
  // identical answer would take the focus out of whatever the user is typing -- once a second -- for nothing.
  // ⚠️ THE COMPARISON IS AGAINST WHAT WAS LAST DRAWN (`S.drawnJson`), not against a flag set when the question
  // was asked: only the drawing side knows what the page is really holding.
  if (q.quiet && S.controls && String(S.drawnSlot) === String(forSlot) && json === S.drawnJson) return;
  // ⚠️ ⚠️ THE WAIT IS SETTLED BEFORE THE DRAW, AND THE ORDER IS THE FIX FOR A BUG I WROTE INTO THIS FILE.
  //
  // The draw is where an arm can happen by itself: `groupCard` ends with `ArmCapture` for a rule the feature has
  // just opened (see `armOnFollow`), which STARTS a wait. So a settle that ran after the draw judged that brand
  // new wait against the document that created it -- a document that cannot say `waiting` yet, because the feature
  // has not been asked -- and stopped it on the spot. The user's symptom would have been exactly the one this
  // whole change is about, arriving only for a NEW rule: press 添加, the capture arms, nothing ever comes back.
  // Before the draw, a wait created by this delivery is simply not there to be judged.
  syncWaitWatch(forSlot, json);
  // ... AND THE LIVE GROUPS, on the same delivery and for the same reason: what has to be watched is read from
  // the document that was just drawn, not from a flag the page set for itself (see syncLiveGroups).
  syncLiveGroups(forSlot);
  if (S.view !== "general") {
    // What the page is holding now, so a silent re-read can tell "nothing changed" from "the page is empty"
    // (see the quiet guard above; `S.controls` is nulled when the user leaves a page, and a document that
    // matches the last one must still be DRAWN in that case -- that blank-page bug has been here once already).
    S.drawnSlot = forSlot;
    S.drawnJson = json;
    renderFeature(S.view);
  }
};

// The name of the rule the feature has open in the page's current document -- what a completed capture just
// filled in. Empty when there is no such row, so the caller falls back to a plain notice.
function capturedRowName(slot) {
  var params = (S.controls && S.controls.params) || [];
  for (var i = 0; i < params.length; i++) {
    var p = params[i];
    if (p.type !== "group") continue;
    var at = (typeof p.editing === "number") ? p.editing : -1;
    if (at < 0 || !p.items || !p.items[at]) continue;
    var v = p.items[at].values || {};
    if (v.name) return String(v.name);
  }
  return "";
}

window.__apexHostGone = function () {
  S.connected = false;
  renderConn();
  // (The panel PROCESS logs the transition -- see the liveness poll in ui_webview.cpp. Nothing is logged from
  // here: this page has one channel out, `pageError`, and it means "something threw". A host going away is
  // not an error, it is a state.)
};

// ⚠️ AND BACK, WHICH THE FIRST VERSION DID NOT HAVE. Without it a host that was restarted left the warning up
// forever -- the page had no way to notice, and the only cure was closing and reopening the panel. The panel
// process polls (see ui_webview.cpp) and calls this when the host window reappears.
window.__apexHostBack = function () {
  S.connected = true;
  renderConn();
  // Ask for everything again: the host that came back is not necessarily holding the state this page last
  // saw, and it may have been a different build.
  hostCall("snapshot");
};

// Anything the page itself throws is reported the same way: a script error inside the web view is
// otherwise completely invisible from outside it.
window.addEventListener("error", function (e) {
  reportError("js: " + e.message + " @" + e.lineno);
});

// ---- shared widgets ---------------------------------------------------------

// THE SWITCH ITSELF, WITHOUT ITS LABEL -- so a row that puts one beside something else (a condition's checkbox,
// a list row's enable switch) is the same control as a `bool` row, not a second drawing of one.
function switchBox(checked, onToggle, disabled, title) {
  var sw = el("div", "sw");
  if (title) sw.title = title;
  var inp = el("input");
  inp.type = "checkbox";
  inp.checked = !!checked;
  inp.disabled = !!disabled;
  // ⚠️ THE COMPARISON IS AGAINST THE LAST VALUE WE KNOW ABOUT, NOT THE ONE AT BUILD TIME. Comparing against
  // the `checked` ARGUMENT looked like the echo guard the selects use, but an argument is frozen at build
  // time: after one click the box is the opposite of `checked`, so the SECOND click matched the stale value
  // and was silently dropped -- no message, no redraw, nothing. Reported from the outside as "the switch only
  // works once", and it is worst for a feature's `bool` parameter, because `setControl` is deliberately NOT
  // answered with a snapshot (see ui_webview.cpp): nothing comes back to correct the display, so the panel
  // and the host disagree until something else refreshes the page.
  var last = !!checked;
  inp.addEventListener("change", function () {
    if (inp.checked === last)
      return; // a rebuild that left the box matching what we last told the host: not an edit
    last = inp.checked;
    onToggle(inp.checked);
  });
  sw.appendChild(inp);
  sw.appendChild(el("div", "tr"));
  sw.appendChild(el("div", "kn"));
  return sw;
}

function switchRow(label, checked, onToggle, disabled) {
  var row = el("div", "row");
  row.appendChild(el("label", null, label));
  var ctl = el("div", "ctl");
  ctl.appendChild(switchBox(checked, onToggle, disabled));
  row.appendChild(ctl);
  return row;
}

// TWO SWITCHES ON ONE LINE, UNDER ONE LABEL -- for a pair that is ONE thing seen from two sides rather than two
// settings that happen to be adjacent. The user's words for this pair: "它是一个单独的功能，一行两个开关".
//
// ⚠️ WHY THAT IS THE RIGHT SHAPE HERE. `autostart`, `language` and `theme` are three separate settings, so they
// are three lines. The quick panel is one feature with two halves, and drawing them as two lines says they are
// two features -- which is exactly what the user corrected. Each switch keeps its own short label, because the
// two halves are not obvious from the switches alone.
//
// ⚠️ IT REUSES `.swcell` / `.swtxt`, WHICH A LIST ROW ALSO USES (see `rowToggle` in abi.h): a small label beside
// a switch. One shape, one rule -- the alternative was a second pair of classes that would drift from the first.
function switchRowPair(label, specs) {
  var row = el("div", "row");
  row.appendChild(el("label", null, label));
  var ctl = el("div", "ctl");
  specs.forEach(function (s) {
    var cell = el("span", "swcell");
    cell.appendChild(el("span", "swtxt", s.label));
    cell.appendChild(switchBox(s.checked, s.onToggle));
    ctl.appendChild(cell);
  });
  row.appendChild(ctl);
  return row;
}

// A `<select>`, from the host's options and its current value.
//
// ⚠️ A CHANGE EVENT FIRES WHILE THE CONTROL IS BEING BUILT, AND THAT CAUSED A LIVE BUG.
//
// Filling a select fires `change` as the options go in (the browser auto-selects the first one, then re-selects
// as the matching option arrives), so the handler ran with no user involved. Each of those calls sent setHost,
// the host answered with a fresh snapshot, the snapshot re-rendered the select, and the cycle repeated --
// measured in the host's log as settings walking through their own values, unprompted:
//
//     setHost lang=zh   ->  setHost lang=auto  ->  setHost lang=en  ->  setHost theme=dark  ->  ...
//
// The guard is a comparison, not a flag or a timer: a change event whose value EQUALS the value the host just
// reported is not an edit -- nothing needs to change, so nothing is sent. A real click always differs (that is
// what makes it a different choice), so the user's own input is unaffected.
//
// This is also why it is fixed here rather than in the host: the host cannot tell a spurious echo from a real
// edit, because they arrive identically. Only the side that knows whether a person touched the control can.
function selectRow(label, options, value, onChange) {
  var row = el("div", "row");
  row.appendChild(el("label", null, label));
  var ctl = el("div", "ctl");
  var sel = el("select");
  options.forEach(function (o) {
    var op = el("option", null, o.label);
    op.value = o.value;
    if (o.value === value) op.selected = true;
    sel.appendChild(op);
  });
  // The last value we know the host holds -- see switchRow for why the ARGUMENT must not be used as the
  // guard. (A select is rebuilt after every setHost because the host answers with a snapshot, so this is
  // belt-and-braces here; it is the same three lines, and two rules that are one rule should be written once.)
  var last = value;
  sel.addEventListener("change", function () {
    if (sel.value === last)
      return; // the echo described above, not an edit
    last = sel.value;
    onChange(sel.value);
  });
  ctl.appendChild(sel);
  row.appendChild(ctl);
  return row;
}

// A slider that reports EVERY move, which is the "parameters change in real time" requirement: the host
// applies the value on the spot and only writes the file when the window closes.
//
// DOUBLE-CLICK RESTORES THE DEFAULT. The value is `def`, sent by the feature with the control -- so this
// does not need a reset entry point on the ABI, a round trip, or the panel knowing what the parameter
// means. A feature that omits `def` simply has a slider that does not reset.
function rangeRow(p, slot, disabled, pathOverride) {
  // ⚠️ THE PATH IS A PARAMETER BECAUSE A `range` CAN LIVE INSIDE A GROUP ROW (see groupField). A field of an
  // item is addressed as `displays[2].brightness`, never as `brightness`, and this function used to write `p.id`
  // into every message it sent -- so a slider drawn inside a row would have set a control that does not exist,
  // and the drag would have looked like a slider that does not work.
  var path = pathOverride || p.id;
  var row = el("div", "row");
  // ⚠️ AN EMPTY LABEL DRAWS NOTHING, like textRow's (see there): a feature that puts a fader into a row whose
  // own left-hand column already names the thing has no second name for it, and an empty `<label>` would take
  // that width anyway.
  var lbl = S.lang === "zh" ? p.labelZh : p.labelEn;
  if (lbl) row.appendChild(el("label", null, lbl));
  var ctl = el("div", "ctl");
  var inp = el("input");
  inp.type = "range";
  inp.min = p.min; inp.max = p.max; inp.step = p.step || 1;
  inp.value = p.value;
  inp.disabled = !!disabled;
  // THE ROW'S COLOUR, applied to the thumb (see the CSS). Same function the chart uses for the segment this
  // parameter owns (channelColor) -- so the control and its piece of curve are one colour, not two that happen
  // to match.
  var thumb = channelColor(p.hue);
  if (thumb) inp.style.setProperty("--thumb", thumb);
  var val = el("div", "val");
  function show() { val.textContent = fmt(inp.value, p); }
  //
  // ⚠️ NOT through sendControl: that one re-reads the controls afterwards, which is right for a field whose
  // value the feature may refuse and WRONG for a drag -- the round trip would redraw the page and delete the
  // slider from under the cursor. A drag is a stream of values the feature is expected to take, so it is sent
  // without asking (the host does not answer a control set with a snapshot, for this same reason).
  inp.addEventListener("input", function () {
    show();
    hostCall("setControl", { slot: slot, path: path, value: inp.value });
    // ⚠️ AND THE CHART FOLLOWS THE SLIDER WHILE IT MOVES. `queueCurve` asks for the feature's own numbers and
    // the answer redraws the chart IN PLACE -- it does not rebuild the page, so the slider under the cursor
    // stays where it is. It is throttled (80 ms), not debounced: a debounce would wait for the drag to pause,
    // which is exactly the "只有改完值后才动一下" the user reported.
    //
    // Only asked for when there is a chart: a feature without one must not pay a round trip per drag.
    if (S.controls && S.controls.curve && S.controls.curve.shape) queueCurve(slot);
  });
  // THE CURVE IS REDRAWN FROM THE HOST'S OWN NUMBERS, not from a local guess: ask for the controls again and
  // the feature rebuilds its chart with the new value.
  //
  // ⚠️ `change` IS THE DRAG'S END, AND IT IS THE MOMENT A FULL RE-READ IS SAFE AND WORTH DOING. The live
  // redraws above keep the chart current, but they deliberately do not rebuild the controls -- so a value the
  // feature NORMALISES or REFUSES (rounds to a step, clamps) would sit in the slider as the user left it. Here
  // the drag is over, nothing is being held, and re-reading can put the feature's own answer on screen.
  inp.addEventListener("change", function () {
    if (S.controls && S.controls.curve) refreshControls(slot);
  });
  if (typeof p.def === "number") {
    inp.addEventListener("dblclick", function () {
      inp.value = p.def;
      show();
      hostCall("setControl", { slot: slot, path: path, value: String(p.def) });
      if (S.controls && S.controls.curve) queueCurve(slot);
    });
  }
  // ---- THE WHEEL OVER A SLIDER CHANGES ITS VALUE ----
  //
  // THE USER ASKED FOR THIS ("插件拉杆、旋钮（以后会有）支持滚轮直接改值"), and the reason it is not already true is
  // worth stating, because a range input looks like it should be: a focused `<input type=range>` DOES respond
  // to a wheel in this browser, but only once the user has clicked it, and only by whatever the browser
  // decides. What is wanted is a control that takes the wheel without being clicked first (a settings panel
  // is a thing you point at), and that moves by the control's OWN step so the value stays on the grid the
  // feature described.
  //
  // ⚠️ NOTHING HERE HAS TO FIGHT THE HOST FOR THE EVENT, because the host PASSES wheels over this panel.
  // That is a rule in apex/decision.h, and it is load-bearing for this whole feature: if the host smoothed
  // this wheel, one notch of the user's hand would arrive as dozens of tiny wheel messages and each would
  // step the value again.
  //
  // THE STEP IS EXACTLY ONE `step`, in the direction of the scroll, and the resulting value goes through the
  // SAME path a drag takes (`input` then the commit) rather than a private one -- so the number shown, the
  // number sent, and the curve redraw cannot disagree about what happened.
  //
  // A trackpad's fine-grained wheel is handled by ACCUMULATING instead of stepping per event: a device that
  // reports 3 deltas at a time would otherwise need forty flicks to move one step. The accumulator is in
  // units of 120, Windows' own notch, so a notched mouse (which sends exactly 120) advances exactly one step
  // per click, and a trackpad advances at the same RATE rather than the same number of events.
  var accum = 0;
  inp.addEventListener("wheel", function (e) {
    if (inp.disabled) return;
    e.preventDefault();
    var step = Number(p.step) > 0 ? Number(p.step) : 1;
    var lo = Number(p.min), hi = Number(p.max);
    // DeltaMode: 0 = pixels, 1 = lines, 2 = pages. Only pixels and lines are worth accumulating; a page-mode
    // event is a huge jump and is treated as a notch.
    var unit = e.deltaMode === 1 ? 40 : (e.deltaMode === 2 ? 120 : 1);
    accum += (-e.deltaY) * unit;
    if (Math.abs(accum) < 120) return;
    var notches = Math.trunc(accum / 120);
    accum -= notches * 120;
    var v = Number(inp.value) + notches * step;
    // Clamp to the control's OWN range, then re-snap to the grid: floating point steps (1.5, 0.05) drift, and
    // a value that is not on the grid makes the browser round it somewhere else on the next render.
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    v = Math.round((v - lo) / step) * step + lo;
    v = Number(v.toFixed(6));
    if (Number(inp.value) === v) return; // already at the end: do not report a change that did not happen
    inp.value = v;
    show();
    hostCall("setControl", { slot: slot, path: path, value: String(v) });
    if (S.controls && S.controls.curve) queueCurve(slot);
  }, { passive: false });

  show();
  ctl.appendChild(inp);
  row.appendChild(ctl);
  row.appendChild(val);
  return row;
}

// A DROPDOWN -- one of NAMED choices (apex/abi.h, `select`).
//
// ⚠️ THE VALUE SENT IS THE OPTION'S `value` STRING, NOT ITS POSITION. An index would be shorter and would
// break the first time a feature reordered its own options; worse, it would put the feature's vocabulary
// ("ime" / "simulate") into the page, which is the coupling the whole controls document exists to avoid.
function selectRowCtl(p, slot, disabled, path, sink) {
  var row = el("div", "row");
  row.appendChild(el("label", null, S.lang === "zh" ? p.labelZh : p.labelEn));
  var ctl = el("div", "ctl");
  var sel = el("select");
  (p.options || []).forEach(function (o) {
    var op = el("option", null, S.lang === "zh" ? (o.labelZh || o.value) : (o.labelEn || o.value));
    op.value = o.value;
    if (o.value === p.value) op.selected = true;
    sel.appendChild(op);
  });
  sel.disabled = !!disabled;
  // The echo guard, same shape as everywhere else: compare against the value we last told the host, never
  // against the argument (a frozen argument swallows every edit after the first -- see switchRow).
  var last = p.value;
  sel.addEventListener("change", function () {
    if (sel.value === last) return;
    last = sel.value;
    if (sink) sink(sel.value); else sendControl(slot, path, sel.value);
  });
  ctl.appendChild(sel);
  row.appendChild(ctl);
  return row;
}

// A SINGLE LINE OF TEXT (apex/abi.h, `text`).
//
// ⚠️ IT REPORTS ON `change`, NOT ON `input`, AND THAT IS THE OPPOSITE OF THE SLIDERS. A slider is a stream of
// positions and the host wants each one; a text field is a word being typed, and sending a value per keystroke
// would (a) write a settings file per keystroke once the debounce fires and (b) REFUSE half-typed values --
// a pattern with no closing bracket is invalid, so the feature would reject it and the page would replace what
// the user was typing with the old value. `change` fires when the field is committed (blur, or Enter).
//
// Enter is wired explicitly: without it a one-field form needs a click elsewhere to commit, which reads as
// "the change did not take".
function textRow(p, slot, disabled, path, sink, lead) {
  var row = el("div", "row");
  // ⚠️ AN EMPTY LABEL DRAWS NOTHING. A feature may put a field where a label would go and give it no name of its
  // own -- MediaControl's per-row "your name for this device" box sits exactly there (the user's arrangement:
  // "把它放在每个设备的「亮度」「音量」位置就很合适，那个「名字」提示的也可以去掉"), and an empty `<label>` would
  // take its space anyway and push the row wider for a word that is not there.
  var lbl = S.lang === "zh" ? p.labelZh : p.labelEn;
  if (lbl) row.appendChild(el("label", null, lbl));
  var ctl = el("div", "ctl");
  // ⚠️ `lead` IS THE CHECKBOX OF A CONDITION (see the `toggle` note in apex/abi.h): the value and "use this
  // value" are one decision, so they are one row -- otherwise every rule would be twice as tall.
  if (lead) ctl.appendChild(lead);
  var inp = el("input");
  inp.type = "text";
  inp.value = p.value === undefined || p.value === null ? "" : String(p.value);
  inp.placeholder = S.lang === "zh" ? (p.placeholderZh || "") : (p.placeholderEn || "");
  inp.disabled = !!disabled;
  var last = inp.value;
  function commit() {
    if (inp.value === last) return;
    last = inp.value;
    if (sink) sink(inp.value); else sendControl(slot, path, inp.value);
  }
  inp.addEventListener("change", commit);
  inp.addEventListener("keydown", function (e) {
    if (e.key === "Enter") { commit(); inp.blur(); }
  });
  ctl.appendChild(inp);
  row.appendChild(ctl);
  return row;
}

// ---- ASKING A FEATURE FOR ITS CONTROLS, AND KNOWING WHICH ANSWER IS WHICH ----
//
// ⚠️⚠️ A `describe` ANSWER CARRIES NO SLOT, SO THE PAGE HAS TO REMEMBER WHAT IT ASKED FOR -- AND IT MUST.
//
// The host answers `{"params":[...]}` and nothing else: the document does not say which feature it belongs to.
// The panel asks about one slot at a time and the host answers IN THE ORDER IT RECEIVED THE REQUESTS (one
// message loop, one answer each), so the queue below is what pairs them up.
//
// ⚠️ WITHOUT IT, A STALE ANSWER LANDS ON THE PAGE THE USER IS LOOKING AT. That is not hypothetical: the capture
// watch polls the controls every 400 ms, so arming a capture and then moving on left a stream of answers for the
// OLD slot arriving while another page was on screen -- and each one replaced `S.controls` and redrew the page
// with the wrong feature's controls. Reported as "插件面板消失了" (the rules page was replaced by whatever the
// old answer described). A quick double navigation had the same race; the capture watch made it routine.
// ⚠️ WHAT EACH QUESTION WAS FOR IS PART OF THE QUESTION. An entry is `{slot, curveOnly}`:
//
//   * `curveOnly: false` -- the answer REPLACES the page (a list row the feature may have normalised, a page
//     being opened, a capture that just landed);
//   * `curveOnly: true`  -- the answer is wanted for its CURVE alone, while the user is holding a slider. The
//     document is still stored (S.controls), but only the chart is redrawn: rebuilding the page mid-drag is
//     what deletes the slider from under the cursor (and it is why the curve used to update only when the drag
//     ended -- see the slider's own handlers).
var pendingDescribes = [];

/// Ask for one slot's controls, remembering the question so the answer can be recognised.
///
/// ⚠️ `quiet` SAYS WHY THE QUESTION WAS ASKED, AND IT CHANGES WHAT AN UNCHANGED ANSWER MEANS. Two very different
/// things ask for a document:
///
///   * a WRITE or a NAVIGATION (`refreshControls` after `setControl`, `loadFeature`) -- quiet = false. An answer
///     here must ALWAYS be drawn, even when its text is identical to what is on screen, because that is how a
///     feature that REFUSED a value puts the control back: the page clicked "on", the feature kept "off", and the
///     document it sends is the "off" the page was already showing. Skipping that redraw would leave the switch
///     showing a state the feature does not have;
///   * a LIVE GROUP's own re-read (apex/abi.h `live`), once a second, forever -- quiet = true. There the identical
///     answer is the COMMON case, and drawing it would rebuild the page (and take the focus out of whatever the
///     user is typing) once a second for nothing.
function askControls(slot, quiet) {
  pendingDescribes.push({ slot: String(slot), curveOnly: false, quiet: !!quiet });
  hostCall("describe", { slot: slot });
}

/// Ask for the same document, but say that only its curve is wanted (see `pendingDescribes`).
function askCurve(slot) {
  pendingDescribes.push({ slot: String(slot), curveOnly: true });
  hostCall("describe", { slot: slot });
}

// ---- THE HOTKEY CONTROL: A KEY COMBINATION THE USER RECORDS (apex/abi.h, `hotkey`) ----
//
// ⚠️ WHY NOT A TEXT BOX. Nobody can spell "Ctrl+Space" reliably, and a typed combination can be one this program
// cannot send -- which the user discovers as "the shortcut does nothing". Recording makes the keyboard the input,
// so nothing unspellable can be entered.

/// The combination the user just pressed, or "" when it is not one this feature can be given.
///
/// ⚠️ THE NAME HAS TO BE ONE THE FEATURE'S GRAMMAR KNOWS (see its `key_to_vk`): a word for the named keys, the
/// character itself for everything else. A key outside that vocabulary (an arrow, Home, PageUp...) returns ""
/// and the caller says so -- sending it would be sending something the feature can only refuse, and the user
/// would have no idea why.
function comboFromKey(e) {
  var mods = [];
  if (e.ctrlKey) mods.push("Ctrl");
  if (e.altKey) mods.push("Alt");
  if (e.shiftKey) mods.push("Shift");
  if (e.metaKey) mods.push("Win");
  var k = e.key || "";
  var key = "";
  if (k === " ") key = "Space";
  else if (k.length === 1) key = k.toUpperCase();
  else if (k === "Escape") key = "Esc";
  else if (k === "Enter" || k === "Tab" || k === "Backspace") key = k;
  else if (/^F([1-9]|1[0-9]|2[0-4])$/.test(k)) key = k;
  // A modifier alone is not a combination: it means "still waiting".
  if (mods.length === 0 || key === "") return "";
  return mods.concat([key]).join("+");
}

/// Every shortcut this PAGE is already using -- the feature's declared actions (`actions[].key`), which the panel
/// draws and the host registers. Two controls must not share one, and the page is the only side that can see them
/// all at once.
function actionShortcuts() {
  var keys = [];
  ((S.controls && S.controls.params) || []).forEach(function (q) {
    (q.actions || []).forEach(function (a) { if (a.key) keys.push(String(a.key).toLowerCase()); });
  });
  return keys;
}

function hotkeyRow(p, slot, disabled, pathOverride) {
  // ⚠️ SAME REASON AS `rangeRow`: a recorded combination can be a field INSIDE a group row -- MediaControl
  // keeps one screen-off shortcut per monitor -- and its address is `displays[1].hotkey`, not `hotkey`.
  var path = pathOverride || p.id;
  var row = el("div", "row");
  // ⚠️ AN EMPTY LABEL DRAWS NOTHING, THE SAME RULE THE NAME BOX AND THE FADER ALREADY FOLLOW (see textRow and
  // rangeRow). This function used to append the `<label>` unconditionally, and inside a rows list that is not a
  // cosmetic difference: `.row label` is a FIXED 132 px column, so the box was pushed a whole label's width away
  // from whatever came before it. That was invisible while the shortcut was the only control on its own line (the
  // gap read as "right-aligned"), and it is exactly what the user is looking at when the box has to follow the
  // screen-off switch: "快捷键录入框跟在「熄屏」开关右边".
  var lbl = S.lang === "zh" ? p.labelZh : p.labelEn;
  if (lbl) row.appendChild(el("label", null, lbl));
  var ctl = el("div", "ctl");

  var stored = (p.value === undefined || p.value === null) ? "" : String(p.value);
  var box = el("div", "hotkey");
  box.tabIndex = 0;               // so a click can put the keyboard focus here (`keydown` needs it)
  box.title = S.lang === "zh"
    ? "点击这一格，然后按下要用的快捷键"
    : "Click this box, then press the combination you want";
  ctl.appendChild(box);

  var recording = false;
  function showRecording() {
    box.className = "hotkey rec";
    box.textContent = t("hotkeyRecord");
  }
  function showStored() {
    box.className = "hotkey" + (stored ? "" : " empty");
    box.textContent = stored || (S.lang === "zh" ? (p.placeholderZh || "") : (p.placeholderEn || ""));
  }
  function stop() {
    recording = false;
    detachKeys();
    showStored();
  }
  showStored();

  /// ⚠️⚠️ THE KEYS ARE LISTENED FOR ON THE DOCUMENT, NOT ONLY ON THIS BOX -- AND THAT IS THE FIX FOR
  /// "Ctrl+Space 录不上".
  ///
  /// The first version listened on the box alone, which relies on the browser keeping the focus there while the
  /// user presses the combination. Ctrl+Space is the SYSTEM's own input-method switch: pressing it can take the
  /// focus away (or be delivered before the box ever sees it), and the box's `blur` handler then CANCELLED the
  /// recording -- so the page either never got the key or threw the state away a moment later, and the user saw
  /// "nothing happens". A capture-phase listener on the document sees every key in the page regardless of focus,
  /// which is what "press the keys now" actually means.
  ///
  /// The box's own listener is kept as a second path (a page embedded somewhere the document listener does not
  /// reach), and `__apexHotkeySeen` stops the two from handling one press twice -- the box IS inside the
  /// document, so both fire when the focus is on it.
  function onKey(e) {
    if (!recording) return;
    if (e.__apexHotkeySeen) return;
    e.__apexHotkeySeen = true;
    // ⚠️ THE PAGE KEEPS THE KEYSTROKE, the page being the thing that recorded it: without this the browser would
    // also act on it (Ctrl+Space toggles a menu, Space scrolls the page, Escape closes things).
    if (e.preventDefault) e.preventDefault();
    if (e.stopPropagation) e.stopPropagation();
    // Escape means "forget it" -- the one way out that does not need the mouse.
    if (e.key === "Escape") { stop(); return; }
    // ⚠️ DEL AND BACKSPACE CLEAR THE COMBINATION (the user's request: "设置快捷键时，按Del或Backspace能清掉快捷键").
    // ⚠️ ONLY WITHOUT A MODIFIER: Ctrl+Backspace is a combination like any other and must stay recordable -- and
    // the feature is the one that decides whether it accepts one, not this page.
    if ((e.key === "Delete" || e.key === "Backspace") && !e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey) {
      stop();
      stored = "";
      showStored();
      // The same path every other edit takes, so the feature's own answer is what is finally on screen (an empty
      // combination is accepted by MediaControl, and a feature that refused it would simply show its old value).
      sendControl(slot, path, "");
      setCaptureHint(t("hotkeyCleared"));
      return;
    }
    // A modifier on its own is not a combination yet: keep waiting.
    if (e.key === "Control" || e.key === "Shift" || e.key === "Alt" || e.key === "Meta") {
      box.textContent = t("hotkeyRecording");
      return;
    }
    var combo = comboFromKey(e);
    if (!combo) {
      setCaptureHint(t("hotkeyUnsupported"));
      return;                    // still recording -- the user can press another key
    }
    var taken = actionShortcuts();
    if (taken.indexOf(combo.toLowerCase()) >= 0) {
      // ⚠️ REFUSED HERE RATHER THAN BY THE FEATURE, because the conflict is between two controls on THIS page --
      // the feature cannot see the shortcuts its own actions declare in the panel.
      //
      // ⚠️ AND IT STAYS IN RECORDING, like the unsupported-key case below: the user's next act is to try another
      // combination, and dropping out of the state would make them click the box again for every attempt.
      setCaptureHint(t("hotkeyTaken"));
      return;
    }
    stop();
    stored = combo;              // shown at once; the re-read below is what confirms it
    showStored();
    // ⚠️ `sendControl` ASKS THE FEATURE FOR ITS CONTROLS AGAIN BY ITSELF (see its note: the panel cannot know
    // what is valid, so the truth after a send is always read back). That is what makes a REFUSED combination
    // snap back to the one in force -- and it is why there is no second `refreshControls` here.
    sendControl(slot, path, combo);
    setCaptureHint(t("saved"));
  }

  function attachKeys() {
    if (typeof document !== "undefined" && document.addEventListener) {
      document.addEventListener("keydown", onKey, true);
      // ⚠️ AND A CLICK ANYWHERE ELSE ENDS IT, which is what the box's own `blur` used to do -- and must not do
      // here, since the whole point is that recording survives losing the focus. This listener is added DURING
      // the click that armed the recording, in the BUBBLE phase, so the capture-phase listener does not see that
      // same click (its capture pass is already over) -- otherwise every recording would cancel itself on the
      // click that started it.
      document.addEventListener("click", outsideClick, true);
    }
    box.addEventListener("keydown", onKey);
  }
  function outsideClick(e) {
    if (e && e.target === box) return;
    stop();
  }
  function detachKeys() {
    if (typeof document !== "undefined" && document.removeEventListener) {
      document.removeEventListener("keydown", onKey, true);
      document.removeEventListener("click", outsideClick, true);
    }
    if (box.removeEventListener) box.removeEventListener("keydown", onKey);
  }

  box.addEventListener("click", function () {
    if (disabled) return;
    if (recording) return;
    recording = true;
    showRecording();
    attachKeys();
    if (box.focus) box.focus();
  });

  row.appendChild(ctl);
  return row;
}

// ---- THE FIELDS A GROUP DRAWS IN ITS ROWS ------------------------------------------------------
//
// `rowToggle` is ONE field id OR AN ARRAY of them (apex/abi.h): each is drawn as a switch in the row rather
// than among the fields, so an item can be switched without being opened. An array is what a list of two-sided
// choices needs -- KeepAwake asks every listed program "keep the machine awake?" and "keep the screen on?"
// on the same line, because they are one decision seen from two sides.
//
// ⚠️ NORMALISED IN ONE PLACE. Three call sites need this list (the master list's rows, the stacked boxes'
// headers, the rows layout), and a call site that forgot the array case would draw NO switches at all -- a
// silent missing control, which is the failure this page has had most often.
function rowToggleIds(p) {
  var r = p.rowToggle;
  if (!r) return [];
  return (Object.prototype.toString.call(r) === "[object Array]") ? r.slice() : [r];
}

/// The schema of one field of a group, by id -- for the switch's own label, which the feature sends with the
/// field and not with the row.
function fieldOf(p, id) {
  var out = null;
  (p.fields || []).forEach(function (f) { if (f.id === id) out = f; });
  return out;
}

// A REPEATABLE BLOCK -- a rule, a profile, a shortcut (apex/abi.h, `group`).
//
// `p.fields` is the schema of ONE item and `p.items` are the values, paired by field id. The page never
// interprets either: a field's `type` decides which row to draw, and the row's path is `<group id>[<index>]`.
//
// ⚠️ STRUCTURAL EDITS GO THROUGH listOp, FIELD EDITS THROUGH setControl -- and the two must not be confused.
// Adding an item is a change to the LIST, and the shape of a new item is the FEATURE's decision (it supplies
// the defaults); setting a field inside an item is a change to a VALUE. A page that built a new item itself
// would be inventing a feature's defaults, which is a second definition of that feature's settings.
//
// ⚠️ ORDER IS MEANING: for a rule list the engine takes the FIRST match, so which rule comes first is part of
// what the user configured. That is why the order is editable at all -- and it is edited by DRAGGING a row onto
// another (see `makeDraggable`), which sends ONE message for a move that may be several places. The ▲/▼ buttons
// that used to do this are gone, at the user's request: one way to reorder, not two.
function groupCard(p, slot, disabled) {
  var card = el("div", "card");

  var items = p.items || [];
  // Set while rendering the rule a just-pressed 「新建」 produced; acted on once the card is complete (see the
  // follow block in the master layout and the arming at the end of this function).
  var armOnFollow = false;

  // ⚠️ THE BUTTONS SIT ABOVE THE LIST, AND THAT IS THE USER'S OWN ARRANGEMENT: the standalone program
  // puts 新建规则 / 删除规则 at the top of the rules panel, and the request was to match it -- "添加、删除 规则
  // 按钮放列表上面". The reason it reads better is the same in both: the buttons act on the LIST, so they
  // belong beside its heading rather than after its last row, where they are only reached by scrolling.
  // ⚠️ AND THE HEADING IS PLACED IN THE LIST COLUMN, NOT AT THE TOP OF THE CARD, because the user asked for it
  // there: "表头「规则」以及规则统计数要在规则列表里的顶端，不是在参数面板顶端." As the card's first child it sat above
  // BOTH columns and read as a title for the parameter pane beside it.
  var head = el("div", "listhead");
  head.appendChild(el("label", null, S.lang === "zh" ? p.labelZh : p.labelEn));
  // ⚠️ THE COUNT IS IN THE HEADER because it is the one thing a master list cannot show at a glance once it
  // scrolls: how many there are in all. (The sidebar does the same for features, for the same reason.)
  //
  // ⚠️ AND IT COUNTS THE ROWS THE USER MADE, NOT THE LIST'S OWN. An item can be `locked` -- the feature's own row,
  // which the user may configure but not add or remove (see apex/abi.h) -- and a locked row is not one of the
  // user's entries: KeepAwake has exactly one and it is always there. Counting it made the number say 1 before the
  // user had added anything at all, which reads as "you already have a program in the list". The user's words:
  // "保持唤醒，程序数量中，系统全局不用计入其中."
  //
  // ⚠️⚠️ AND ONLY A LIST THE USER CAN ADD TO HAS A COUNT AT ALL (2026-09-23). The number answers "how many entries
  // have I made", which is a question about a list you maintain -- and MediaControl's three groups are entirely
  // the MACHINE's rows (`noAdd: true`, and every row `locked`), so the count was a "0" that could never be anything
  // else. The user's report of it: "「亮度」「熄屏快捷键」「音量」对应的小标题右侧有个数字，是用来计数的？那个不需要，可以
  // 去掉." KeepAwake's program list and AutoIME's rules keep theirs -- for those the number is the answer to a real
  // question ("have I added anything yet").
  if (!p.noAdd) {
    var mine = 0;
    items.forEach(function (it) { if (!it.locked) ++mine; });
    head.appendChild(el("div", "rownote", String(mine)));
  }

  var toggles = rowToggleIds(p);
  // IS THIS A LIST OF NAMES RATHER THAN A LIST OF RULES? (`layout:"rows"`, see apex/abi.h.) Everything below
  // that branches on it does so because the two kinds of item differ in what they HAVE, not in how they look:
  // a row has no draft to edit, no name field to open, and no order to rearrange.
  var byRows = (p.layout === "rows");
  // ... and a rows group whose items are IDENTIFIED BY THEIR NAME asks for a text box to add one with
  // (`addHintZh`/`addHintEn`): the feature cannot invent a program name, so the plain "append an item" button
  // would create a row that matches nothing and cannot be fixed.
  var addHint = byRows ? (S.lang === "zh" ? (p.addHintZh || p.addHintEn) : (p.addHintEn || p.addHintZh)) : "";

  var bar = el("div", "grpbar");
  // ⚠️ THE BAR'S OWN ITEM COUNT, KEPT HERE RATHER THAN ASKED OF THE DOM. `bar.childNodes.length` is what this
  // wants to say, but the panel is also run against a DOM stub (see _diag/apex_panel_probe.js), and the stub
  // models what the page USES -- it threw `Cannot read properties of undefined` and the whole page failed to
  // render in the probe while being perfectly fine in a browser. A counter is one line, cannot disagree with
  // reality, and works in both.
  var barItems = 0;
  function barButton(label, title, fn, off, cls) {
    // ⚠️ `cls` IS HOW A FEATURE'S OWN ACTION IS STILL IDENTIFIABLE. The buttons all sit in one bar now -- that
    // is the user's arrangement -- but a feature's action is not the same thing as Add or Delete: the panel
    // probe counts them (`action`), and a future page may style them differently. Losing the class when they
    // moved was a real regression, and the probe caught it.
    var b = el("button", cls || null, label);
    b.title = title || "";
    b.disabled = !!off;
    b.addEventListener("click", fn);
    bar.appendChild(b);
    ++barItems;
  }
  if (addHint) {
    // THE LIST-STYLE ADD CONTROL, as the `list` control has (see listRow): a text box and a button. The typed
    // text travels in `listOp`'s `value`, and the FEATURE still decides what is acceptable -- it lower-cases,
    // strips a path, refuses a duplicate, or refuses outright, and the page re-reads afterwards either way.
    var addbox = el("div", "addrow");
    var addInp = el("input");
    addInp.type = "text";
    addInp.placeholder = addHint;
    addInp.disabled = !!disabled;
    var addBtn = el("button", "addbtn", t("add"));
    addBtn.disabled = !!disabled;
    function doAdd() {
      var v = addInp.value.trim();
      if (!v) return;
      addInp.value = "";
      hostCall("listOp", { slot: slot, id: p.id, op: "add", value: v });
      refreshControls(slot);
    }
    addBtn.addEventListener("click", doAdd);
    addInp.addEventListener("keydown", function (e) { if (e.key === "Enter") doAdd(); });
    addbox.appendChild(addInp);
    addbox.appendChild(addBtn);
    bar.appendChild(addbox);
    ++barItems;
    // ⚠️ AND THE BUTTON IS AN `addbtn`, NOT A `pri`: "添加按钮不要做撞色" (see the CSS note) -- this one sits in a
    // bar with Delete and the feature's own actions, so it says "this appends" in the same quiet way the other
    // Add buttons do, rather than wearing the accent chip.
  } else if (!p.noAdd) {
    // ⚠️⚠️ `noAdd` IS WHY THIS BRANCH CAN NOW BE SKIPPED ENTIRELY (apex/abi.h, ABI 13 -> 14). The page used to
    // assume every group is growable, so it drew an Add button above EVERY rows group -- including the ones whose
    // rows come from the machine. MediaControl's page had three of them (monitors, screen-off shortcuts,
    // applications), and not one of those can be created by the user: the button could only ask the feature for
    // a monitor that does not exist. The feature refuses (a page is not a gatekeeper), so the button did nothing
    // at all -- which is the worst kind of control, because it looks like it should work.
    barButton(t("add"), S.lang === "zh" ? "新建一条" : "Add one", function () {
      // ⚠️⚠️ THE PAGE DOES NOT DECIDE WHERE THE NEW RULE WENT -- THE FEATURE TELLS IT.
      //
      // The old version selected `items.length - 1` (the last row) and sent `begin-edit` for it, on the
      // assumption that a new rule is appended to the END OF THE LIST. It is appended to the FILE, while the list
      // is SORTED by priority and then name -- so the new rule lands wherever it sorts, and when that is not last
      // the page opened a DIFFERENT rule for editing and a Save would have written the new rule's fields over it.
      //
      // Now `add` opens the draft on this side of the process boundary (see the feature's list_op) and the
      // document carries `editing = <the row that really is open>`. The page only has to FOLLOW it, which is what
      // `grpFollowEdit` asks the next render to do -- once, because the user's own clicks choose the selection
      // from then on.
      hostCall("listOp", { slot: slot, id: p.id, op: "add", value: "" });
      S.grpFollowEdit = p.id;
      refreshControls(slot);
    }, disabled, "addbtn");
  }
  // ⚠️ NO "DELETE THE SELECTED ONE" IN A ROWS LIST, because there is no selection there: each line carries its
  // own remove button, which is also the only way to remove a row that the user is not looking at. Drawing a
  // button that acted on a row the page never highlighted would be the worst kind of control -- one that
  // deletes something the user cannot see.
  if (!byRows) {
    barButton(t("remove"), S.lang === "zh" ? "删除选中的一条" : "Delete the selected one", function () {
      var sel = (S.grpSel && S.grpSel[p.id]) || 0;
      hostCall("listOp", { slot: slot, id: p.id, op: "remove", index: sel });
      // ⚠️ KEEP A SENSIBLE SELECTION. Dropping the last item should leave the NEW last one selected rather
      // than falling back to the first -- which would silently jump the user to an unrelated rule.
      if (S.grpSel) S.grpSel[p.id] = Math.max(0, Math.min(sel, items.length - 2));
      // (The feature drops any draft of its own: a rule that just went cannot be the one being edited.)
      refreshControls(slot);
    }, disabled || !items.length);
  }
  // ⚠️ THE FEATURE'S OWN ACTIONS GO IN THIS BAR TOO, so everything that acts on the list is in one place.
  // They were beside Delete before; the user's arrangement puts the list's buttons together at the top.
  (p.actions || []).forEach(function (a) {
    // ⚠️ THE SHORTCUT IS SHOWN ON THE BUTTON, not written into its label. "「击点捕获」这个按钮后面的（下一个
    // 点击目标）没意义，去掉，改成快捷键." A key in parentheses is a sentence; a key in the hint under the button is
    // something the eye can use. The feature sends it (`key` on the action) so the panel does not have to know
    // that THIS feature happens to have a hotkey.
    var label = S.lang === "zh" ? (a.labelZh || a.op) : (a.labelEn || a.op);
    var b = el("button", "action", label);
    b.disabled = !!disabled || !items.length;
    // ⚠️ THE SHORTCUT IS INSIDE THE BUTTON, because the user said the key and the button are one
    // thing: "它跟「点击捕获」是同一个功能，做成同一个按钮就好." A keycap drawn in the button says "this and
    // Ctrl+Alt+Q are the same act" without a second control to find. The feature sends the key (`key` on the
    // action), so the panel never has to know which feature happens to have a hotkey.
    if (a.key) b.appendChild(el("span", "keycap", a.key));
    b.addEventListener("click", function () { ArmCapture(slot, p, p.id); });
    bar.appendChild(b);
    // ⚠️ AND AN ACTION COUNTS AS A REASON FOR THE BAR TO EXIST, WHICH IT DID NOT (found 2026-09-23 while moving
    // the quick-panel switch). `barItems` is what decides whether a rows group's bar is drawn at all (see the end
    // of this function: "an empty bar is not appended"), and the loop above never incremented it -- so a rows
    // group whose rows come from the machine AND which declares an action lost its whole bar: MediaControl's
    // 「刷新应用列表」 button has been on no screen since it was written, and nothing said so, because the only
    // fixture with actions is a master-layout group (where the bar is appended either way). A control that is
    // built and then thrown away is the failure this file's own notes are about.
    ++barItems;
  });
  // THE ONE MESSAGE BOX, right-aligned in the bar (see `hintbox` in the CSS). Created once per card and
  // registered as THE box for capture notices, so the button and Ctrl+Alt+Q write to the same place.
  //
  // ⚠️ AND WHATEVER THE PAGE LAST HAD TO SAY IS PUT BACK IN IT. This element is brand new on every render; the
  // message is not (see `hintText`), or a re-read would silently swallow it -- and the completion of a capture
  // always causes a re-read.
  captureHintBox = el("span", "hintbox", "");
  bar.appendChild(captureHintBox);
  setCaptureHint(hintText);

  // ---- THE GROUP'S OWN QUICK-PANEL SWITCH (apex/abi.h, `quick`, ABI 16 -> 17) ----
  //
  // ONE SWITCH FOR THE WHOLE GROUP, and it is not a row among the parameters: it is drawn at the right-hand end
  // of the line that already carries this group's own controls. WHICH line that is follows from what the group
  // already has, and each case is the user's own words:
  //
  //   * a group whose rows the user ADDS BY HAND has an add box, and the switch goes beside it -- "放在「添加」
  //     按钮右侧，居右";
  //   * a group whose rows come from the MACHINE has no add box, so its heading is the line -- "位置移到亮度和
  //     音量各自小标题的右侧，居右";
  //   * a group that is neither (a stack of boxes, or a master list with a detail pane) has no heading of its own
  //     at all -- `listhead` is only built for a rows list -- so the bar is the only line it has.
  //
  // ⚠️ IT REUSES `.swcell`/`.swtxt`, WHICH A LIST ROW'S OWN SWITCHES ALREADY USE: a short label beside a switch
  // is one shape and therefore one rule (the alternative is a second pair of classes that drift). `.quicksw`
  // adds only the two things this place needs -- pushed to the right, and not a flexible item.
  //
  // ⚠️ AND IT IS AN ORDINARY CONTROL: the id is a `setControl` path like any other, `sendControl` reads the
  // group back afterwards, and a feature that clamps or refuses therefore wins on the next render. The page does
  // not know what any of these switches mean, and it does not need to -- it draws the one the group described.
  if (p.quick && p.quick.id) {
    var qcell = el("span", "swcell quicksw");
    qcell.appendChild(el("span", "swtxt", S.lang === "zh" ? p.quick.labelZh : p.quick.labelEn));
    qcell.appendChild(switchBox(!!Number(p.quick.value), function (v) {
      sendControl(slot, p.quick.id, v ? "1" : "0");
    }, disabled));
    (addHint ? bar : (byRows ? head : bar)).appendChild(qcell);
  }
  // ⚠️ WHERE THE BAR GOES DEPENDS ON THE LAYOUT, AND THAT IS NOT A STYLE CHOICE -- the user asked for both
  // arrangements, for the two different kinds of list:
  //
  //   * a list of RULES keeps its buttons INSIDE the card, above the list ("添加、删除 规则按钮放列表上面");
  //   * a list of NAMES puts the add row OUTSIDE the list entirely -- above the card, so the box belongs to the
  //     page and the card is purely the list. The user's words, after seeing it inside: "添加这行，要在列表外."
  //
  // The second one is drawn by wrapping the bar and the card in one block (see the end of this function), because
  // the card is what the caller appends to the page.
  if (byRows)
    card.appendChild(head);
  else
    card.appendChild(bar);

  if (!items.length) {
    card.appendChild(el("div", "note", t("empty")));
  } else if (p.layout === "master") {
    // ---- MASTER / DETAIL: a list on the left, the selected item's fields on the right ----
    //
    // WHY A FEATURE ASKS FOR THIS (see "layout" in apex/abi.h): a group of twenty stacked boxes means scrolling
    // past every one of them to reach the last, and "pick a rule, change it" becomes two separate journeys.
    // The user's words: "现在这样做是变好看了，但是不方便，规则一多，要不断往下翻".
    //
    // ⚠️ S.grpSel IS PAGE STATE: which item is being looked at. It is never sent anywhere (the feature has no
    // opinion about it) and it is kept PER GROUP, so two groups on one page do not fight over it. A redraw
    // keeps it, which is what stops "edit a field -> the controls are re-read -> the page jumps back to item 0".
    if (!S.grpSel) S.grpSel = {};
    var sel = S.grpSel[p.id];
    if (typeof sel !== "number" || sel < 0 || sel >= items.length) { sel = 0; S.grpSel[p.id] = 0; }

    // ---- WHICH ROW IS BEING EDITED: THE FEATURE'S ANSWER, NOT THE PAGE'S GUESS ----
    //
    // ⚠️⚠️ THIS IS THE FIX, AND IT REPLACES A GUESS THAT COST THE USER THREE SEPARATE REPORTS.
    //
    // The draft lives in the feature (see DRAFT in its settings.rs), so the feature is the only party that can
    // say which rule is open. The page used to keep its own flag (`S.grpEdit`) and had to work out for itself
    // where a new rule had landed, when a capture had filled which row, and whether a re-read had cancelled the
    // edit -- three questions it could only answer by assumption. The document now carries `editing` (a display
    // index, or -1), and this page DRAWS it: the mode indicator, the editable fields, and everything that says
    // "this rule is being edited" all come from that one number.
    //
    // ⚠️ AND THE PAGE STILL REMEMBERS IT, AS AN OBSERVATION RATHER THAN A DECISION: `S.grpEditing` is what the
    // last document said. Two things need it between renders -- `cancelEditOnLeave` (which must not send a
    // cancel for nothing) and `ArmCapture` (a capture fills ONE rule) -- and both are about what the FEATURE
    // holds, so the cached answer is refreshed here, on every render, from the source.
    if (!S.grpEditing) S.grpEditing = {};
    var editingAt = (typeof p.editing === "number") ? p.editing : -1;
    S.grpEditing[p.id] = editingAt;

    // ⚠️ THE ONE-SHOT FOLLOW, USED ONLY BY Add. The user's own clicks set the selection directly, and adopting
    // `editing` on every render would fight them: clicking another rule cancels the edit (asynchronously), so
    // for one render the document still names the row the user just left -- and the selection would snap back.
    // Add is the one case where the page has no opinion at all and the feature's answer is the only one.
    if (S.grpFollowEdit === p.id && editingAt >= 0) {
      S.grpFollowEdit = null;
      sel = editingAt;
      S.grpSel[p.id] = sel;
      // ⚠️ AND THE NEW RULE GOES STRAIGHT INTO CAPTURE MODE: "新规则，默认进入捕获状态."
      //
      // A new rule is an empty shell -- no patterns, switched off -- so the FIRST thing its author wants is the
      // control it is about, and the only way to get that is to point at it. Making them find 「点击捕获」 as a
      // second step would be asking for a click that has exactly one sensible next move. (It is also what the
      // program this feature replaces did: "新建规则时默认启用捕获".)
      //
      // ⚠️ ARMED AFTER THE CARD IS BUILT, not here: arming writes the instruction into this card's own message
      // box, which does not exist until the render reaches the end of this function.
      armOnFollow = true;
    }

    var editing = (editingAt === sel);

    // ⚠️ DID THE SAVE TAKE? The page asked for a commit; a document is the answer.
    //
    // The feature answers `commit-edit` with 0 when there was nothing to commit (its draft was gone -- the row
    // was switched, or the click landed on a different rule), and the page used to clear its own flag and say
    // "已保存" regardless: a save that did not happen looked exactly like one that did. The document is the
    // check -- the feature clears `editing` when a commit really happened.
    //
    // ⚠️ AND IT WAITS FOR A SECOND DOCUMENT BEFORE SAYING OTHERWISE, which is not caution but arithmetic: a
    // `describe` sent just before the commit can still be in flight, and ITS answer shows the row as open
    // (correctly -- it was drawn before the commit). A message about that document would be a lie in the other
    // direction. So "still open" is only reported once a second document agrees, and the wait is one render.
    if (S.grpCommitFor === p.id) {
      if (!S.grpCommitWait) S.grpCommitWait = {};
      if (editingAt < 0) {
        S.grpCommitFor = null;
        S.grpCommitWait[p.id] = false;
        ShowLive(t("saved"));
      } else if (S.grpCommitWait[p.id]) {
        S.grpCommitFor = null;
        S.grpCommitWait[p.id] = false;
        ShowLive(t("saveFailed"));
      } else {
        S.grpCommitWait[p.id] = true;
      }
    }

    // ⚠️ EDITING IS PER SELECTION, AND THAT IS THE POINT: "每条的保存/取消，是独立的，切换规则后不保存，现在看起来
    // 它是所有规则一起用的." The feature holds ONE draft, so only the row it names is editable -- selecting
    // another rule shows that rule's fields read-only, rather than the first rule's edit.
    // (That is `editing`, computed above from the document's own `editing` field.)

    // ⚠️ THE DATA STILL COMES FROM THE FEATURE, AND SO DOES THE MODE NOW. The fields show whatever
    // `settingsJson` sent -- the DRAFT while one is open, the saved rule otherwise -- and `editing` (which row
    // that draft belongs to) comes from the same document. So a change made by a CAPTURE, produced inside the
    // feature, appears here on the next read and so does the mode it belongs to; nothing on this page has to be
    // told what the feature did behind its back.
    var split = el("div", "split");
    var list = el("div", "splitlist");
    // The heading names the column it counts -- see where `head` is built, above.
    list.appendChild(head);
    items.forEach(function (item, i) {
      var row = el("div", "item" + (i === sel ? " sel" : "") + (itemIsOff(p, item) ? " dead" : ""));
      row.appendChild(el("div", "nm", itemTitle(p, item, i)));
      // ⚠️ THE SWITCHES THAT BELONG IN THE ROW (apex/abi.h, `rowToggle`): "规则荐的'启用'移到规则列表的各项右侧."
      // Each writes the SAME field the detail pane would otherwise show (`rules[i].enabled`), so there is one
      // value and one path to it -- but it is where the user can reach it without opening the rule, which is the
      // point. They are drawn in the order the feature declared, and labelled with their own field's label (the
      // feature knows what each one means; the page is not asked to).
      //
      // ⚠️ AND THEY DO NOT NEED EDIT MODE. The rule a switch belongs to is not necessarily the one being edited,
      // and a list of switches that were dead until you opened each rule would be worse than no switches.
      toggles.forEach(function (fid) {
        var tf = fieldOf(p, fid);
        var on = !!Number(itemValue(p, item, fid));
        var box = switchBox(on, function (v) {
          sendControl(slot, p.id + "[" + i + "]." + fid, v ? "1" : "0");
        }, disabled, tf ? (S.lang === "zh" ? tf.labelZh : tf.labelEn) : fid);
        // The click must not also select the row (and so change what the detail pane shows): the switch is its
        // own control, and `switchRow`'s warning about decoration divs applies here too -- `stopPropagation`.
        box.addEventListener("click", function (e) { if (e.stopPropagation) e.stopPropagation(); });
        row.appendChild(box);
      });
      // ⚠️ ORDER IS CHANGED BY DRAGGING THE ROW: "规则顺序可以直接拖动调整顺序，上下按钮去掉."
      //
      // The arrows that used to be here are gone -- one way to reorder, not two. The drag sends ONE message
      // (the feature's `move`, see abi.h): the page knows where the row was and where it was dropped, and a
      // chain of move-up/move-down would be one message and one full re-read of the page per place, so the list
      // would visibly walk to its destination.
      makeDraggable(row, i, function (from, to) { moveItem(p, slot, from, to); });
      row.addEventListener("click", function () {
        // ⚠️ SELECTING ANOTHER RULE ENDS ANY EDIT -- that is what makes each rule's edit its own.
        //
        // The user's report: "编辑第一条，点第2条，再回第1条，内容应该变成编前的数据，且要重新点编辑才能编辑."
        // The DRAFT lives in the feature, so the PAGE has to say when the user moved on -- nothing else knows
        // that the click was a decision to stop editing. (Whether an edit is open is the DOCUMENT's answer, not
        // the page's -- see the `editing` note above.)
        if (editing && i !== sel) {
          hostCall("listOp", { slot: slot, id: p.id, op: "cancel-edit", index: sel });
        }
        // ⚠️ A LOCAL REDRAW, NOT A ROUND TRIP. Selecting is not an edit -- the data has not changed -- so
        // asking the feature for its controls again would cost a message per click and could reorder the list
        // under the user's hand. (When an edit WAS open the message above has already been sent, and the redraw
        // below is the one that shows the saved values again.)
        S.grpSel[p.id] = i;
        refreshControls(slot);
      });
      list.appendChild(row);
    });
    split.appendChild(list);

    var detail = el("div", "splitdetail");
    // ---- the edit / save / cancel bar, and the fields that follow it ----
    //
    // ⚠️ FIELDS ARE READ-ONLY UNTIL Edit IS PRESSED, as in the standalone program. The lock is what makes the
    // draft meaningful: there is no way to type into a rule and then wonder whether it was saved.
    //
    // ⚠️ AND EACH RULE'S EDIT IS ITS OWN -- pressing Edit on one rule and then selecting another must not carry
    // the first one's edit across. The user's report: "每条的保存/取消，是独立的，切换规则后不保存，现在看起来它
    // 是所有规则一起用的." The page's flag is per group and per SELECTION, and the feature refuses to hold a draft
    // for two rules at once: beginning an edit on another rule replaces the draft (see "begin-edit" in its
    // settings.rs), so an edit that is walked away from is gone, not silently applied to the next rule.
    var editBar = el("div", "editbar");
    // ⚠️ ONE BUTTON, AND ITS LABEL IS THE STATE INDICATOR.
    //
    // The user's request: "点完编辑按钮时，它要变成'保存'，这样才知道是不是编辑状态." The first version swapped between
    // two DIFFERENT buttons -- an Edit built in one branch, a Save in the other -- which is the same behaviour
    // read from the code and NOT the same thing to look at: a control that stays put and changes its word says
    // "you are in a different mode now", and a control that is replaced by another says nothing in particular.
    //
    // So the element is built once and only its label and emphasis change. `pri` is added while editing because
    // that is the action the user is expected to take next; without it the button would look equally like "go
    // back" when it is in fact "commit".
    var editBtn = el("button", editing ? "pri" : null, editing ? t("save") : t("edit"));
    editBtn.disabled = !!disabled;
    editBtn.title = editing
      ? (S.lang === "zh" ? "保存这条规则的修改" : "Save this rule's changes")
      : (S.lang === "zh" ? "编辑这条规则" : "Edit this rule");
    editBtn.addEventListener("click", function () {
      if (editing) {
        // ⚠️ ONE MESSAGE, AND THE FEATURE DOES THE WRITING. The fields were already sent as they were typed
        // (into the feature's draft); pressing the button now publishes that draft into the rule and the file.
        // The page does not know the file format and must not -- see `commit-edit` in the feature's settings.rs.
        //
        // ⚠️ AND THE PAGE DOES NOT ANNOUNCE THE SAVE ITSELF. It asks, and the next document answers: the
        // feature clears `editing` when the commit really happened, and `grpCommitFor` is what turns that into
        // the message (see the `editing` note above). Claiming "已保存" here would be a claim about the feature's
        // state made by the party that does not hold it -- which is how a refused save used to look like a
        // successful one.
        hostCall("listOp", { slot: slot, id: p.id, op: "commit-edit", index: sel });
        S.grpCommitFor = p.id;
        refreshControls(slot);
      } else {
        // The FEATURE copies the rule into its draft -- it owns the data, so it is the one that can.
        hostCall("listOp", { slot: slot, id: p.id, op: "begin-edit", index: sel });
        refreshControls(slot);
      }
    });
    editBar.appendChild(editBtn);
    // ⚠️ NO CANCEL BUTTON -- the user removed it: "不需要取消按钮，有点保存就保存，没点保存，切走，或关了，就是取消."
    // Leaving the rule, leaving the page, or closing the panel IS the cancel -- see cancelEditOnLeave, which
    // every one of those paths calls.
    detail.appendChild(editBar);

    // The fields show whatever the feature sent -- which is its DRAFT while one is open, and the saved rule
    // otherwise. A field edit goes straight to setControl either way; the feature decides where it lands, which
    // is the whole point of moving the draft there.
    (p.fields || []).forEach(function (fld) {
      // ⚠️ EXCEPT THE ONES THE GROUP PUT IN THE ROW (see `rowToggle`): each is drawn once, at the right of the
      // row, and drawing it here as well would be two controls for one value -- the user changes one and the
      // other says something else until the next read.
      if (toggles.indexOf(fld.id) >= 0) return;
      detail.appendChild(groupField(p, fld, items[sel], sel, slot, disabled || !editing, null));
    });
    split.appendChild(detail);
    card.appendChild(split);
  } else if (byRows) {
    // ---- ROWS: one line per item, every field live (see "rows" in apex/abi.h) ----
    //
    // WHAT THIS LAYOUT IS FOR, in the user's own description of the page they wanted: "统一做成列表式，每个进程有
    // 「保持唤醒」和「阻止熄屏」两项，和一个移除按钮". A list of names with two switches each. So a line is:
    // the item's title, any field that is not a row switch, the switches, and a remove button.
    //
    // ⚠️ NO EDIT MODE AND NO DRAFT, WHICH IS THE WHOLE DIFFERENCE FROM "stack" AND "master". There is nothing on
    // this kind of item worth drafting -- no pattern to get half-typed -- and a page that made the user press
    // Edit to flip a checkbox would be a form standing in the way of a switch. Every control here writes through
    // `sendControl` immediately, and the feature answers by re-reading (which is also how it corrects a pair of
    // switches it does not accept as a pair).
    //
    // ⚠️ AND NO DRAG: the order of a list like this is not meaning (see the layout note in abi.h).
    var rowBox = el("div", "rowlist");
    items.forEach(function (item, i) {
      // ⚠️ A LOCKED ITEM IS THE FEATURE'S OWN ROW: it cannot be removed, but it CAN be configured -- KeepAwake's
      // "系统全局" line is exactly that (the two master switches live in a row the user may not delete). So this
      // is not "disabled": it removes the remove button and nothing else.
      var locked = !!item.locked;
      // The row's own bottom border is dropped on the LAST one, exactly as the list control does it -- a
      // selector would have to guess, and the guess breaks the moment anything else lands in the card.
      var r = el("div", "skiprow" + (i === items.length - 1 ? " last" : "") + (locked ? " locked" : ""));
      r.appendChild(el("div", "nm", itemTitle(p, item, i)));
      (p.fields || []).forEach(function (fld) {
        if (toggles.indexOf(fld.id) >= 0) return; // drawn as a switch further along this same line
        r.appendChild(groupField(p, fld, item, i, slot, disabled, null));
      });
      toggles.forEach(function (fid) {
        var tf = fieldOf(p, fid);
        var on = !!Number(itemValue(p, item, fid));
        var cell = el("div", "swcell");
        // The label is the FIELD's, sent by the feature with the field -- the page still does not know what
        // either switch means, it only knows what to call it (see the CSS note on why it is here at all).
        if (tf) cell.appendChild(el("span", "swtxt", S.lang === "zh" ? tf.labelZh : tf.labelEn));
        var box = switchBox(on, function (v) {
          sendControl(slot, p.id + "[" + i + "]." + fid, v ? "1" : "0");
        }, disabled, tf ? (S.lang === "zh" ? tf.labelZh : tf.labelEn) : fid);
        cell.appendChild(box);
        r.appendChild(cell);
      });
      if (!locked) {
        var rm = el("button", null, t("remove"));
        rm.title = S.lang === "zh" ? "把这一行删掉" : "Remove this line";
        rm.disabled = !!disabled;
        rm.addEventListener("click", function () {
          hostCall("listOp", { slot: slot, id: p.id, op: "remove", index: i });
          refreshControls(slot);
        });
        r.appendChild(rm);
      } else {
        // ⚠️ THE SPACE A REMOVE BUTTON WOULD TAKE IS RESERVED, AND THAT IS THE WHOLE POINT OF THIS ELEMENT.
        // A locked row has no button, so without this its switches sit one button-width to the right of every
        // other row's -- which is what the user saw: "系统全局后面两个开关，可以跟其它程度开关对齐". The spacer is a
        // REAL BUTTON with the same label, made invisible, rather than a fixed pixel width: a width written in
        // pixels is right in Chinese and wrong in English ("删除" is two characters, "Remove" is six), and the
        // next language would be wrong again.
        var ghost = el("button", "ghost", t("remove"));
        ghost.setAttribute("aria-hidden", "true");
        ghost.tabIndex = -1;
        r.appendChild(ghost);
      }
      rowBox.appendChild(r);
    });
    card.appendChild(rowBox);
  } else {
    // ---- STACKED (the default): one box per item ----
    items.forEach(function (item, i) {
      var box = el("div", "grp");
      var gh = el("div", "grphead");
      gh.appendChild(el("div", "nm", itemTitle(p, item, i)));
      var tools = el("div", "grptools");
      // ⚠️ THE SWITCHES THE GROUP PUT IN THE ROW ARE DRAWN HERE, IN THE BOX'S OWN HEADER -- and until this line
      // existed they were drawn NOWHERE. The body skips them (a switch is not a field row) and the header did not
      // draw them, so a stacked group with a `rowToggle` had a switch that simply was not on the page: the item's
      // own on/off control, missing, with the page looking complete. (Nothing that ships used stacked together
      // with a rowToggle -- AutoIME, the only feature with one, asks for "master" -- so this was a trap for the
      // next feature rather than a live fault. It is fixed here because this is the file it would have sprung.)
      toggles.forEach(function (fid) {
        var tf = fieldOf(p, fid);
        var on = !!Number(itemValue(p, item, fid));
        var boxSw = switchBox(on, function (v) {
          sendControl(slot, p.id + "[" + i + "]." + fid, v ? "1" : "0");
        }, disabled, tf ? (S.lang === "zh" ? tf.labelZh : tf.labelEn) : fid);
        gh.appendChild(boxSw);
      });
      // ⚠️ THE ARROWS ARE GONE HERE TOO, AND THE DRAG IS HERE FOR THE SAME REASON: a layout is not a good place
      // to offer a worse way of doing the same thing. A stack of twenty items still reorders by dragging the box.
      makeDraggable(box, i, function (from, to) { moveItem(p, slot, from, to); });
      function tool(label, title, fn, isDisabled) {
        var b = el("button", null, label);
        b.title = title;
        b.disabled = !!isDisabled;
        b.addEventListener("click", function () { fn(); });
        tools.appendChild(b);
      }
      tool(t("remove"), S.lang === "zh" ? "删除这一项" : "Delete this item",
           function () { hostCall("listOp", { slot: slot, id: p.id, op: "remove", index: i }); refreshControls(slot); });
      gh.appendChild(tools);
      box.appendChild(gh);
      // ⚠️ THE SAME DRAFT SINK AS THE MASTER LAYOUT, and it is not decoration: a feature that chose the
      // stacked layout still edits its items through the page, and a stack with editable fields and no
      // Save button would be a form that writes the file as you type. A stack has no Edit button by design
      // -- "pick one, then change it" is what the layout is FOR -- so its fields stay read-only here, which
      // is what they were before this rework too.
      (p.fields || []).forEach(function (fld) {
        if (toggles.indexOf(fld.id) >= 0) return;   // drawn in the box's own header instead
        box.appendChild(groupField(p, fld, item, i, slot, disabled || true, null));
      });
      // (The feature's own action buttons are in the bar at the top of the card now, with Add and Delete --
      // they act on the list, so they belong with the list's other buttons.)
      card.appendChild(box);
    });
  }

  // ⚠️ THE ARMED-AFTER-ADD, DONE LAST BECAUSE IT WRITES INTO THIS CARD. See the follow block: a new rule is
  // opened for editing by the feature, and the page takes the user straight into capturing the control it is
  // about. `ArmCapture` puts its instruction in the message box built above, so it has to run after it exists.
  if (armOnFollow) ArmCapture(slot, p, p.id);

  // ⚠️ AND A "rows" LIST HANDS BACK A BLOCK: its bar ABOVE the card, not inside it. The caller appends one node
  // to the page, so the two have to travel together -- and the bar has to be the FIRST child, because "the add
  // row stands outside the list" means above it (the user's answer to being asked exactly this: "添加这行，要在
  // 列表外" -> the card above). Everything else about the card is unchanged: its heading, its lines and its
  // hairlines are still one box, which is what makes it read as the list.
  if (byRows) {
    var block = el("div", "rowsblock");
    // ⚠️ AN EMPTY BAR IS NOT APPENDED. A rows group with no Add control and no actions of its own (MediaControl's
    // screen-off shortcuts, since ABI 14) would otherwise leave `.grpbar`'s bottom margin standing as a gap
    // above the card -- "there is nothing here" drawn as a space, which reads as a layout mistake.
    if (barItems) block.appendChild(bar);
    block.appendChild(card);
    return block;
  }
  return card;
}

// ---- REORDERING A LIST BY DRAGGING ----
//
// ⚠️ ONE IMPLEMENTATION FOR BOTH LAYOUTS. The master list and the stacked boxes answer the same question ("move
// this item there") and only differ in what the row looks like; two copies of a drag handler would be two places
// where "which index did the user grab" can be got wrong.
//
// ⚠️ AND IT SENDS ONE MESSAGE. The feature has `move` (see abi.h): the page knows where the row was (`index`,
// which it closed over when it built the row) and where it was dropped (`to`). A chain of move-up/move-down
// would be one message and one full re-read of the page per place, so a rule dragged six places down would make
// the list visibly walk there.
var dragFrom = null;      // the display index the drag started on, or null
var dragOver = null;      // the node currently marked as the drop target

function clearDropMark() {
  if (dragOver) dragOver.className = dragOver.className.replace(/\s*dropover/g, "");
  dragOver = null;
}

function makeDraggable(node, index, onDrop) {
  node.draggable = true;
  node.addEventListener("dragstart", function (e) {
    dragFrom = index;
    node.className += " dragging";
    // ⚠️ THE DATA IS NOT WHAT CARRIES THE MOVE -- `dragFrom` is, because a drop on the SAME list is the only
    // case here. `setData` is what some browsers require for a drag to start at all, so it is set and unused.
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = "move";
      try { e.dataTransfer.setData("text/plain", String(index)); } catch (x) { /* a stub without setData */ }
    }
  });
  node.addEventListener("dragend", function () {
    dragFrom = null;
    node.className = node.className.replace(/\s*dragging/g, "");
    clearDropMark();
  });
  node.addEventListener("dragover", function (e) {
    if (dragFrom === null || dragFrom === index) return;
    // ⚠️ THIS IS THE LINE THAT MAKES THE DROP POSSIBLE: without `preventDefault` the browser treats the element
    // as "not a drop target" and never fires `drop` -- so the whole feature would silently do nothing.
    if (e.preventDefault) e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = "move";
    if (dragOver !== node) { clearDropMark(); node.className += " dropover"; dragOver = node; }
  });
  node.addEventListener("drop", function (e) {
    if (e.preventDefault) e.preventDefault();
    if (dragFrom === null || dragFrom === index) { clearDropMark(); return; }
    // ⚠️ THE TWO NUMBERS ARE DIFFERENT AND MIXING THEM UP IS SILENT: `from` is the row the user GRABBED (the
    // module-level `dragFrom`), `to` is the row it was dropped on (this node's own index, closed over here).
    // The first version passed only this node's index to the callback, so every drag read as "move row N onto
    // itself" -- the feature was asked to move the row that did not move. Caught by the panel probe, not by hand.
    var from = dragFrom;
    clearDropMark();
    onDrop(from, index);
  });
}

/// Move the item the user dragged, and look at it afterwards.
function moveItem(p, slot, from, to) {
  hostCall("listOp", { slot: slot, id: p.id, op: "move", value: String(to), index: from });
  // ⚠️ THE SELECTION FOLLOWS THE RULE THE USER MOVED. Which row is SELECTED is page state (the feature has no
  // opinion about it), so the page has to move it -- otherwise the user drags rule 7 to the top and then finds
  // the detail pane showing whatever now happens to sit at row 7.
  if (S.grpSel) S.grpSel[p.id] = to;
  refreshControls(slot);
}

// ONE FIELD OF ONE ITEM, in whichever layout asked for it, so a field is built the same way in both.
function groupField(p, fld, item, i, slot, disabled, sink) {
  // ⚠️ A MISSING FIELD IS EMPTY, NOT AN ERROR. The feature may have added a field since the item was stored,
  // and a page that refused to draw the item would leave the user unable to reach the other fields to fix it.
  // Drawing it empty and letting the feature decide what an empty value means is the recoverable direction.
  var one = itemValue(p, item, fld.id);
  var path = p.id + "[" + i + "]." + fld.id;

  // ⚠️ `sink` IS HOW THE EDIT/SAVE WORKFLOW WORKS WITHOUT EVERY ROW KNOWING ABOUT IT. While a rule is being
  // EDITED the page keeps a DRAFT -- the same shape the standalone program uses -- so a change is held here
  // and written only when the user presses Save. Without a sink the row talks to the host as before, which is
  // what every other feature on this page still does: the behaviour is opt-in and nothing else changes.
  //
  // ⚠️ AND SAVING EVERY CHANGE IS NOT MERELY CHATTIER. Each `setControl` makes the host persist the file
  // (SettingsTouch) and re-read the controls, so editing a rule would be a disk write per field -- and Cancel
  // would have nothing left to cancel, because the change would already be in config.json.
  function put(text) {
    if (sink) { sink(i, fld.id, text); return; }
    sendControl(slot, path, text);
  }
  // ⚠️ AND THE CHECKBOX THAT GOVERNS IT (apex/abi.h, `toggle`): "各参数原先是有个复选框，打钩了才生效." It is a
  // SEPARATE FIELD of the item (`use_process` beside `process_pattern`) and it is written like any other
  // control -- but drawn here, at the left of the value it governs, because the two are one decision.
  var lead = null;
  if (fld.toggle) {
    var on = !!Number(itemValue(p, item, fld.toggle));
    lead = switchBox(on, function (v) {
      if (sink) { sink(i, fld.toggle, v ? "1" : "0"); return; }
      sendControl(slot, p.id + "[" + i + "]." + fld.toggle, v ? "1" : "0");
    }, disabled, S.lang === "zh" ? "这一项参与判断" : "test this value");
    lead.className += " cond";
  }

  if (fld.type === "bool") {
    return switchRow(S.lang === "zh" ? fld.labelZh : fld.labelEn, !!Number(one),
                     function (v) { put(v ? "1" : "0"); }, disabled);
  }
  if (fld.type === "select") {
    var sp = shallowCopy(fld); sp.value = one;
    return selectRowCtl(sp, slot, disabled, path, function (v) { put(v); });
  }
  // ⚠️⚠️ EVERY OTHER TYPE USED TO FALL THROUGH TO A TEXT BOX, AND THAT WAS WRONG FOR TWO OF THEM.
  //
  // The comment here said "everything else in a group is TEXT -- the patterns are the whole point of this
  // feature", which was true of the feature it was written for (AutoIME's rules: process patterns, window
  // titles, class names) and false about the ABI, which lets a group's fields be ANY control type. So a
  // MediaControl page with a `range` inside a row drew a NUMBER INPUT, and a `hotkey` inside a row drew a plain
  // text box that could not record anything -- the user's report was "亮度和音量在设置面板里的调整，也要用推子" and
  // "熄屏快捷键不能录入和生效". The host had been sending the right document both times; the page was drawing
  // two of the five control types as the sixth.
  //
  // ⚠️ AND THE THREE DISPATCHES BELOW ARE THE SAME FUNCTIONS THE TOP-LEVEL CONTROLS USE -- not lookalikes.
  // `rangeRow` and `hotkeyRow` now take the field's own path (`offkeys[1].hotkey`) instead of assuming the
  // control is a top-level one, which is the only change they needed to work in both places.
  if (fld.type === "range") {
    var rp = shallowCopy(fld); rp.value = one;
    var rangeRowEl = rangeRow(rp, slot, disabled, path);
    // ⚠️⚠️ THE FADER IS THE ONE THING IN A ROW THAT TAKES THE SPARE WIDTH (see the CSS note on `.grow`). A row can
    // hold three or four controls now -- a name box, a fader, a switch, a shortcut -- and if every one of them grew
    // equally the fader paid for all of them: with two growing fields in a row it kept half the leftover, with
    // three it kept a third, and the control the user reads the row by got narrower every time the feature added
    // something to it. The page knows which field is which because the FEATURE said so (`type`), so this is not a
    // guess about MediaControl: it is "a slider is the control that wants room", written once.
    rangeRowEl.className += " grow";
    return rangeRowEl;
  }
  if (fld.type === "hotkey") {
    var hp = shallowCopy(fld); hp.value = one;
    return hotkeyRow(hp, slot, disabled, path);
  }
  // Everything else in a group is TEXT.
  var tp = shallowCopy(fld); tp.value = one;
  return textRow(tp, slot, disabled, path, function (v) { put(v); }, lead);
}

// Is this item switched off? Read from its own `enabled` field when it has one, so a master list can grey it
// the way the sidebar greys a disabled feature. A group with no such field simply never greys anything.
function itemIsOff(p, item) {
  var has = (p.fields || []).some(function (f) { return f.id === "enabled"; });
  return has && !Number(itemValue(p, item, "enabled"));
}

// Small helpers the group editor needs. Both exist so the group code reads as pairing, not as indexing.
function shallowCopy(o) {
  var c = {};
  Object.keys(o || {}).forEach(function (k) { c[k] = o[k]; });
  return c;
}
function itemValue(p, item, fieldId) {
  // The host sends an item's fields under `values`, keyed by field id. A group whose field is missing gets
  // "" rather than undefined so the input shows empty instead of the string "undefined".
  //
  // ⚠️ AND A DRAFT ITEM IS FLAT (`{name:"..",enabled:1}`), WHICH IS WHY THIS ACCEPTS BOTH SHAPES.
  //
  // The two shapes are not a wart to be tidied away: the HOST's item is `{title, values}` because it carries
  // presentation (`title`) alongside data, while the DRAFT is nothing but data. What must not happen is one
  // shape being read by code that expects the other -- which is exactly what happened: the fields were handed
  // the FLAT draft, this looked for `.values`, found nothing, and returned "". Every field of an edited rule
  // rendered EMPTY while the draft held the right values, and Save still worked (it reads the draft directly) --
  // which is what made it look like a drawing bug instead of a shape mismatch.
  if (!item) return "";
  var bag = item.values ? item.values : item;
  return Object.prototype.hasOwnProperty.call(bag, fieldId) ? bag[fieldId] : "";
}
function itemTitle(p, item, index) {
  // An item may carry a `title` for the header (a rule's name). Falling back to "#3" keeps the buttons
  // reachable for an item that has none -- an unlabelled box still needs its move and delete buttons.
  //
  // ⚠️ AND A TITLE MAY COME IN TWO LANGUAGES (see `titleZh`/`titleEn` in apex/abi.h). The user's own data has one
  // spelling and no translation -- a rule is called what the user called it -- but a row the FEATURE built for
  // itself is a label like every other one on this page, and the feature cannot know which language is on screen.
  // KeepAwake's undeletable "系统全局" line stayed Chinese on an English page until this existed; the user found
  // it: "系统全局，这几个字，要随 中/英文切换".
  var named = (S.lang === "zh") ? (item && item.titleZh) : (item && (item.titleEn || item.titleZh));
  if (named) return named;
  var t2 = item && item.title;
  if (t2) return t2;
  return (S.lang === "zh" ? "第 " : "#") + (index + 1);
}

// ONE PLACE THAT SENDS A CONTROL VALUE, so the four callers cannot disagree about the message shape.
function sendControl(slot, path, value) {
  hostCall("setControl", { slot: slot, path: path, value: value });
  // ⚠️ AND THEN ASK AGAIN, BECAUSE THE FEATURE MAY HAVE REFUSED. The panel cannot know what is valid -- it
  // does not know what any of these fields MEAN -- so the only way to show the truth after a send is to read
  // it back. (The alternative, trusting the value we just sent, is how a page comes to display a value the
  // feature rejected: the user sees their typo accepted, and the settings file disagrees with the screen.)
  refreshControls(slot);
}

function fmt(v, p) {
  var n = Number(v);
  var s = (Math.abs(n) >= 100 || n === Math.trunc(n)) ? String(Math.round(n * 100) / 100) : n.toFixed(2);
  return p.unit ? s + " " + p.unit : s;
}

// ---- the curve ---------------------------------------------------------------
//
// A normalised polyline from the feature, drawn as a speed-over-time shape. The panel does not compute it:
// a feature that wants to show its motion describes it, and one that does not simply sends no curve.
var curveTimer = 0;      // a chart refresh is already scheduled
var curveNextAt = 0;     // the earliest moment the next one may be asked for

// ASK FOR THE CURVE AGAIN WHILE THE USER DRAGS. Called from the slider's `input` handler -- i.e. once per pixel
// of a drag -- and never rebuilds the page: the answer redraws the chart in place (see `pendingDescribes` and
// redrawCurveInPlace). The chart follows the sliders as they move, which is what it is for: the shape, the
// numbers on both axes, and each segment's colour are all read from the values the feature now holds.
//
// ⚠️ THROTTLED, NOT DEBOUNCED, AND THE DIFFERENCE IS THE WHOLE BUG. The first version waited 60 ms and restarted
// the wait on every event -- so a drag that never paused never redrew anything, and the chart moved exactly once,
// when the user let go. ("现在只是改完值后才动一下".) A throttle fires on the leading edge and then at a fixed
// rate for as long as the events keep coming: one round trip per 80 ms, which is more than the eye can use and
// far less than the per-pixel `setControl` stream beside it.
//
// ⚠️ AND THE ANSWER IS ONLY WANTED IF THERE IS A CHART TO DRAW IT ON -- a feature without a curve must not pay
// a round trip per drag. The caller checks that; nothing here knows what a curve is.
function queueCurve(slot) {
  if (curveTimer) return; // one is already on its way, and it will read the newest value
  var wait = curveNextAt - Date.now();
  if (wait < 0) wait = 0;
  curveTimer = setTimeout(function () {
    curveTimer = 0;
    curveNextAt = Date.now() + 80;
    askCurve(slot);
  }, wait);
}

// ASK FOR THIS FEATURE'S CONTROLS AGAIN. Used after an edit whose RESULT the page cannot predict -- a list
// row the feature may have normalised or refused (see ApexFeature::listOp). The answer replaces S.controls
// and redraws; asking is cheaper than guessing wrong and being contradicted on the next refresh.
function refreshControls(slot) {
  askControls(slot);
}

// THE MOTION CURVE: the shape of the motion, plus a ball that runs along it when a wheel turns.
//
// The feature sends a normalised polyline (x and y both 0..1, see ApexFeature::settingsJson) and this draws it.
// That is the whole contract: the panel knows nothing about wheels, windows or speed, and a feature that sends
// no curve gets no curve card.
//
// ⚠️ THE ANIMATION IS DRIVEN BY REAL WHEEL ACTIVITY, NEVER BY A TIMER. This is the user's requirement in their
// own words -- "是有鼠标滚轮事件才有，没有就没有" -- and it is the difference between a picture and a decoration:
//   * on a timer it animates while the user is idle and keeps animating while they scroll, so it says the same
//     thing in every state and means nothing;
//   * on real traffic it says exactly one thing, and it says it truthfully: a ball appears when a wheel was
//     TAKEN by this feature, and there is no motion when nothing is happening.
//
// The count arrives from the host through __apexActivity (the feature reports every wheel it takes through
// ApexHost::activity; the host coalesces and posts). The panel process forwards it here.
//
// ⚠️ AND IT WAS REMOVED ONCE, BY ME, DURING A REVIEW -- on the argument that it "spends the input path's budget
// on decoration". That was wrong on the facts (the input path spends one interlocked increment; the messaging
// is the host's own thread, off the input path) and wrong about the requirement, which had been asked for
// explicitly. The feature is back; the reasoning is recorded so it is not deleted a third time.
//
// A ball is never recycled mid-flight -- one that vanished halfway reads as a glitch -- so when every slot is
// busy the new one is simply not sent. The ones already out keep arriving, so a roll still reads as continuous
// motion.
var CURVE_BALLS = 6;
var curveAnim = 0;      // the requestAnimationFrame handle, while something is moving
var curveBorn = [];     // when each ball started, in ms, or 0 for a free slot
var curveCanvas = null; // the canvas the current curve is drawn on
var curveObj = null;    // the chart object it was drawn from
var curveSpanMs = 400;  // how long a ball takes to cross it (from the feature's own window)

function StopCurve() {
  if (curveAnim) { cancelAnimationFrame(curveAnim); curveAnim = 0; }
  curveBorn = [];
  curveCanvas = null;
  curveObj = null;
}

// TAKE OVER THE CANVAS, DRAW THE CURVE, AND WAIT. Nothing moves until a wheel happens -- but the SHAPE appears
// at once, because a blank card is not "a chart waiting for input", it is a card that looks broken.
function StartCurve(canvas, curve) {
  StopCurve();
  curveCanvas = canvas;
  curveObj = curve;
  // HOW LONG A BALL TAKES TO CROSS: the x axis IS time, so its length is the animation's length. Taken from
  // the feature rather than invented here, so the ball runs at the speed of the motion it is drawing.
  curveSpanMs = (curve && curve.spanMs > 0) ? curve.spanMs : 400;
  drawCurve(canvas, curve);
}

// REDRAW THE CHART ON THE CANVAS THAT IS ALREADY THERE, keeping whatever is flying across it.
//
// ⚠️ THIS IS NOT `StartCurve`, AND THE DIFFERENCE IS THE BALLS. `StartCurve` stops the animation and clears the
// in-flight list (`StopCurve`), which is right when a card is created and wrong here: this runs every 80 ms
// while a slider is dragged, and any wheel taken during that drag would have its ball deleted mid-flight --
// which reads as a glitch, and the note on SpawnBall says a ball is never recycled mid-flight for that reason.
// Only the numbers change here; `curveBorn` is left alone.
function redrawCurveInPlace() {
  var canvas = document.getElementById("curve");
  var curve = S.controls && S.controls.curve;
  if (!canvas || !curve || !curve.shape || curve.shape.length < 2) return;
  if (curveCanvas !== canvas) {
    // The page was rebuilt since the last draw (the user navigated, or a full refresh landed): this canvas is
    // new and has nothing on it, so it gets the full start rather than an in-place update.
    StartCurve(canvas, curve);
    return;
  }
  curveObj = curve;
  curveSpanMs = (curve.spanMs > 0) ? curve.spanMs : 400;
  drawCurve(canvas, curve);
}

// One ball per wheel the feature took. `n` is how many happened since the last look; they are staggered a
// little so a burst reads as a stream rather than a clump at the same point.
//
// ⚠️ ASSIGNED TO `window`, NOT DECLARED AS A FUNCTION. The panel process calls it as an expression --
// `window.__apexActivity(n)` -- so it has to be a PROPERTY of the page's window. A bare
// `function __apexActivity(){}` is not: it is a binding in the script's scope, the call from C++ would find
// nothing, and ExecuteScript returns S_OK for a script that throws -- so the notification would arrive and be
// silently dropped.
window.__apexActivity = function (n) {
  if (!curveCanvas || !curveObj) return;
  var count = Math.min(n | 0, 12); // a burst is still a burst; more than this in one hop is not readable
  var now = Date.now();
  for (var i = 0; i < count; i++) SpawnBall(now + i * 45);
  if (!curveAnim) curveAnim = requestAnimationFrame(CurveFrame);
};

// SOMETHING THE PANEL DRAWS HAS CHANGED IN THE HOST (see APEX_STATE_MSG_NAME). Same `window.` rule as above.
//
// ⚠️ WHY THIS EXISTS: the controls are only re-read when a feature is SELECTED, so anything that changes on
// its own -- the REAPER note is the first -- stayed stale until the user switched pages and came back. That is
// not detecting, it is "refreshing if you happen to ask again", and the user said so.
//
// ⚠️⚠️ AND IT ASKS FOR A SNAPSHOT RATHER THAN RE-READING ONE PAGE, WHICH IS A FIX TO THIS FUNCTION RATHER THAN A
// PREFERENCE. It used to call `loadFeature(S.view)` -- and it returned early on the general page -- which was
// HALF A REFRESH, and the missing half was the half the user was looking at. What changes on the host's side is
// its own list of which features are on, and that list is drawn in two places `loadFeature` never touches:
//
//   * the SIDEBAR's status dots (green/grey/red -- see renderNav);
//   * the feature page's own "disabled" state, which is part of the SNAPSHOT, not of that feature's controls.
//
// So flipping a switch in the quick panel left the page showing the old answer. The user's report, verbatim:
// "在设置打开的时候，点快速面板开关时，设置的开关要同步跟着实时变化".
//
// A snapshot re-reads everything the host owns, and `__apexSnapshot` then re-reads the open feature page's own
// controls itself (see its last line) -- so one message covers both halves.
//
// ⚠️ IT STILL KNOWS NOTHING ABOUT WHICH CONTROL CHANGED, and that is deliberate: the host's `what` is opaque
// here, because teaching the page to interpret it would mean the page knowing the features' ids -- the exact
// coupling the controls document exists to avoid.
window.__apexStateChanged = function (what) {
  if (!S.snap) return; // not connected yet; the first snapshot will carry the current answer
  hostCall("snapshot");
};

// CTRL+ALT+Q WAS PRESSED (see kCaptureHotkeyId in ui_webview.cpp): arm a capture on the page.
//
// ⚠️ THE KEY DOES WHAT THE BUTTON DOES, AND IT IS THE SAME CODE PATH. The standalone program's key armed a
// capture; the panel's button arms a capture; both must land on the feature's `capture` op for the rule the
// user is looking at. So this finds the group that declared a capture action and presses its logical
// equivalent -- rather than duplicating the call, which is how the two would drift apart.
//
// ⚠️ IT SAYS SO WHEN IT CANNOT. Pressing the key on the general page, or on a feature page with no capture
// action, must not be silent: the user pressed a key and expects something, and "nothing happened" is the
// report that wastes their time. The page has no message bar, so the readout box at the top is used -- it is
// already the place this page talks back.
window.__apexCaptureKey = function () {
  if (typeof S.view === "number" && S.controls && S.controls.params) {
    for (var i = 0; i < S.controls.params.length; i++) {
      var p = S.controls.params[i];
      if (p.type === "group" && p.actions) {
        for (var j = 0; j < p.actions.length; j++) {
          if (p.actions[j].op === "capture") {
            ArmCapture(S.view, p, p.id);
            return;
          }
        }
      }
    }
  }
  // Nothing to arm: say which page would work, rather than doing nothing.
  ShowLive(t("captureKeyNoRule"));
};

// The one place a capture is armed -- the button in the group and the hotkey both come here.
//
// ⚠️ IT ONLY ARMS WHILE THE RULE IS BEING EDITED, WHICH IS THE STANDALONE PROGRAM'S OWN RULE:
//
//     fn arm_click_capture(&mut self, ...) {
//         if !self.editing {
//             self.status = "请先点“编辑”进入可编辑状态，再点击捕获。";
//             return;
//         }
//
// The reason is the same one that put the fields behind Edit: a capture OVERWRITES the rule it lands on
// (every pattern, plus the name), so arming it outside an edit would rewrite a working rule from a single
// stray click -- with no Save, and nothing to undo. The button and Ctrl+Alt+Q both come through here, so the
// rule is enforced once and neither of them can get ahead of it.
//
// ⚠️ AND THE MESSAGE SAYS WHAT TO DO, because the alternative is a button that looks broken. The user's
// report of the previous behaviour was exactly that the button did not do what it said -- "点击捕获下一个
// 点击的目标，功能不对" -- so a refusal now explains itself in the page's own readout.
// THE ONE MESSAGE BOX FOR THIS PAGE'S OWN NOTICES, at the right-hand end of the button bar.
//
// ⚠️ IT IS ONE ELEMENT FOR EVERY PATH, WHICH IS THE POINT OF THE USER'S REQUEST: "各种提示做在统一
// 提示框，统一放在「点击捕获」按钮右边，居右." The first version handed the element to the BUTTON's click handler, so
// Ctrl+Alt+Q -- which has no card to look in -- wrote its notice nowhere at all, and the user saw the refusal
// appear in the readout at the top of the page instead: "有提示，但没生效." A module-level handle is what
// makes every path land in the same place.
// ---- THE ONE MESSAGE BOX ----
//
// ⚠️ THERE IS EXACTLY ONE PLACE A MESSAGE APPEARS, AND IT IS THE QUIET HINT AT THE RIGHT-HAND END OF THE RULES
// BAR, BESIDE 「点击捕获」. The user's instruction: "消息提示统一移到「点击捕获」按钮右侧，UI采用上方不高亮的提示."
//
// WHAT IT REPLACED: the page used to write its messages into the READOUT at the top of the page (the feature's
// live panel) while the capture messages went here -- so "先点编辑再捕获" and "已保存" appeared in two different
// parts of the window depending on which code path produced them. Both of those other places are gone: the
// read-out entirely (ABI 9 -> 10), so this box is the page's one voice.
var captureHintBox = null;

/// WHAT THE BOX SHOULD SAY, KEPT OUTSIDE THE ELEMENT.
///
/// ⚠️ ONE LINE, AND IT IS WHAT MAKES A MESSAGE SURVIVE. The page rebuilds the whole card on every re-read
/// (`innerHTML = ""`), so the box the message was written into is thrown away a moment later -- and a completed
/// capture ALWAYS re-reads the controls, which means the message that says "已捕获: ..." would have been wiped by
/// the very refresh it caused. So the text lives here and every render re-applies it: the message is page state,
/// and the element is only how it is shown.
var hintText = "";

/// Show a message in that box -- and when the card it belongs to is not on screen, nowhere.
///
/// ⚠️ THERE USED TO BE A FALLBACK INTO THE FEATURE'S READ-OUT, AND IT IS GONE WITH THE READ-OUT ITSELF (see the
/// note where its CSS was: ABI 9 -> 10, "其实这个提示可以完全去掉。并不需要"). The message box is created per card, so a
/// message can only be shown while the card it belongs to is drawn -- which is exactly when it is about something
/// the user can act on. (Every message this page shows is about a rule: edit, save, capture, the Ctrl+Alt+Q hint
/// when no rule is selected.)
function setCaptureHint(text) {
  hintText = text || "";
  if (!captureHintBox)
    return;
  captureHintBox.textContent = hintText;
  // `on` is only "has something to say" -- the styling is deliberately quiet (see the CSS): this is a hint, not
  // an alarm.
  captureHintBox.className = "hintbox" + (hintText ? " on" : "");
}

function ArmCapture(slot, p, groupId) {
  // ⚠️ THE RIGHT INDEX, NOT JUST "SOMETHING IS BEING EDITED". A capture lands on ONE rule, so the page has to
  // be editing the one the user is looking at -- otherwise Ctrl+Alt+Q pressed after clicking to another rule
  // would fill the rule that was left behind (see the per-selection note in groupCard).
  var sel = (S.grpSel && S.grpSel[groupId]);
  if (typeof sel !== "number") sel = 0;
  // ⚠️ `S.grpEditing` IS WHAT THE FEATURE LAST SAID, refreshed on every render from the document (see the
  // `editing` note in groupCard). The page asks THAT rather than its own opinion: a capture fills the rule the
  // FEATURE has open, so an answer the page made up could point at a rule with no draft -- and the capture would
  // be dropped by the feature (its `capture` op refuses without one).
  if (!S.grpEditing || S.grpEditing[groupId] !== sel) {
    ShowLive(t("captureNeedsEdit"));
    setCaptureHint(t("captureNeedsEdit"));
    return;
  }
  hostCall("listOp", { slot: slot, id: groupId, op: "capture", index: sel });
  ShowLive(t("captureArmed"));
  setCaptureHint(t("captureArmed"));
  // AND THE PAGE STARTS LISTENING for the capture to come back -- see watchForCapture. Without this the
  // filled-in fields would not appear until something else re-read the controls.
  watchForCapture(slot);
}

// Show a one-off line. One implementation, in one place -- see the note on `setCaptureHint`, which this is
// simply another name for. (It used to write into the readout at the top of the page, which is the other box.)
function ShowLive(text) {
  setCaptureHint(text);
}

// ---- WAITING FOR A CAPTURE TO COME BACK ----
//
// ⚠️⚠️ IT WATCHES THE CONTROLS, NOT A READOUT, AND THAT IS A CHANGE WITH A HISTORY.
//
// A capture is produced by the FEATURE (its own thread waits for the user's click), so the page cannot have an
// answer when the op returns -- it has to find out later. It used to find out by watching the feature's readout
// text change, which meant the page's refresh depended on an invisible string and the box that displayed it was
// the only reason the string existed. The user removed the readout ("自动输入法页，最上方的状态提示去了"), so the
// signal had to become something the page actually wants: THE CONTROLS. What it polls is what it redraws.
//
// ⚠️⚠️ AND THE FEATURE IS WHAT SAYS WHEN THE WAIT IS OVER. The first version of that poll compared documents:
// the wait ended at the first delivery that differed from the one at arming time. That is a guess, and it was
// wrong in both directions -- it ends early on a change that has nothing to do with the capture (the host
// answers every `listOp` with a snapshot and the page re-reads after it, which is what the user hit: the result
// did not appear until they clicked the rule row), and it never ends at all when a capture fills in exactly what
// was already there. So the feature now publishes `waiting` (apex/abi.h) for as long as it is waiting, and this
// is the only thing that keeps the poll alive.
//
// ⚠️ IT POLLS WHILE THE FEATURE SAYS IT IS WAITING, AND THEN STOPS. While a capture is armed the user is
// pointing at a control RIGHT NOW, so the poll is 400 ms; it stops on the first document that no longer says
// `waiting`, and there is no other poll in this page any more (the one-second read-out poll went with the
// read-out: ABI 9 -> 10). An idle panel therefore sends NOTHING -- which is what "the panel is a separate
// process and the host does not pay for it" has to mean in practice.
//
// ⚠️ `last` IS ONLY ABOUT NOT REDRAWING (see the guard in `__apexControls`): it is the text of the last answer
// this wait drew, so the four-a-second poll does not rebuild the card -- and take the focus -- while nothing
// has happened. `sawWaiting` is the hint's honesty: a wait that never started (the feature refusing the op,
// say) must not be announced as a finished capture.
var captureWatch = null;
var liveTimer = 0;

// Start asking because the page just armed a capture -- the feature only answers "I am waiting" to a question,
// so without this first ask the page would never learn it had to keep asking.
function watchForCapture(slot) {
  startWaitWatch(slot, JSON.stringify(S.controls || null), false);
}

function startWaitWatch(slot, last, sawWaiting) {
  captureWatch = { slot: String(slot), last: last, sawWaiting: !!sawWaiting };
  if (liveTimer) clearInterval(liveTimer);
  // ⚠️ 400 -> 200 ms, AND IT IS THE SAME NUMBER AS THE FEATURE'S OWN POLL (kPollHardwareFastMs). A slider that
  // follows the machine's brightness keys is only as smooth as the slower of the two clocks, and the user's
  // report was about exactly that: "SDC4190的亮度控制跟系统控制同步有延迟，没法做到实时同步". A capture is not
  // asked for more often than it was -- this only changes how soon an answer that has ALREADY changed is seen.
  liveTimer = setInterval(pollCapture, 200);
}

function stopWatchingForCapture() {
  captureWatch = null;
  // ⚠️ AND THE TIMER STOPS WITH IT. It used to fall back to a one-second read-out poll (the feature's live line
  // above its page); that read-out is gone from both ends -- see the note where its CSS was -- so a timer left
  // running here would be a message a second between the panel and the host for nothing at all.
  if (liveTimer) clearInterval(liveTimer);
  liveTimer = 0;
}

// Ask for the controls again while a capture is armed. (The answer is handled in `__apexControls`, which is the
// only place that knows the document arrived.)
function pollCapture() {
  if (!captureWatch) return;
  if (!S.snap || !S.connected) return;
  askControls(captureWatch.slot);
}

/// THE OP A FEATURE SAYS IT IS STILL WAITING FOR, or "" -- the `waiting` field of any group in the document that
/// arrived (apex/abi.h). Empty means nothing is being waited for.
///
/// ⚠️ IT IS READ FROM THE DOCUMENT RATHER THAN FROM A FLAG THE PAGE SET when it armed the action, in both
/// directions: a wait that has ENDED must be noticed even though the page has no way to know, and a wait the
/// page did not start -- the panel was closed and reopened in the middle of one -- is still a wait. The feature
/// is the only side that knows, so the feature is what the page asks.
///
/// ⚠️ ANY GROUP, NOT THE ONE THAT ARMED IT: only one thing can be waited for at a time (the feature has one such
/// state), and the page has no way to tell which group an op belongs to without knowing the feature. What it
/// does with the answer does not depend on which group it came from.
function waitingOp(doc) {
  var params = (doc && doc.params) || [];
  for (var i = 0; i < params.length; i++) {
    var p = params[i];
    if (p && p.type === "group" && typeof p.waiting === "string" && p.waiting) return p.waiting;
  }
  return "";
}

/// FOLLOW THE WAIT FROM THE ANSWER THAT WAS JUST DRAWN. Called at the end of `__apexControls` -- after the
/// document replaced `S.controls` and the page was drawn -- because both things it does are about that answer:
/// keep asking, or stop and say so.
function syncWaitWatch(slot, json) {
  var op = waitingOp(S.controls);
  if (captureWatch && String(captureWatch.slot) === String(slot)) {
    if (op) {
      // STILL WAITING: remember what was drawn (so an unchanged next answer is not redrawn) and that this wait
      // was real (so the completion below may announce itself).
      captureWatch.last = json;
      captureWatch.sawWaiting = true;
      return;
    }
    // NOT WAITING ANY MORE, and the answer is already in the document that was just drawn -- which is the whole
    // point of the flag: the values and the news that they arrived are the same delivery.
    var announced = captureWatch.sawWaiting;
    stopWatchingForCapture();
    if (announced) {
      // ⚠️ AND THE MESSAGE NAMES WHAT WAS CAPTURED, READ FROM THE ROW ITSELF. The feature used to publish a line
      // ("已捕获: 进程 / 窗口 / 控件类"); with no readout, the page reads the rule's own name, which a capture sets
      // to exactly that -- so the sentence survives without a second channel to carry it.
      //
      // ⚠️⚠️ AND WHEN THERE IS NO SUCH ROW, NOTHING IS SAID AT ALL. `waiting` is not only for captures any more:
      // MediaControl publishes it while a screen is dark (so this poll is what makes the screen-off switch follow
      // a click on the dark window). That wait ends with no captured row, and the old code announced "已保存" for
      // it -- a message about a save that never happened, on a page that had just been corrected.
      var named = capturedRowName(slot);
      if (named) setCaptureHint((S.lang === "zh" ? "已捕获：" : "Captured: ") + named);
    }
    return;
  }
  // ⚠️ NOT WATCHING, BUT THE FEATURE SAYS IT IS WAITING: this page instance did not arm it -- it was opened (or
  // re-read) in the middle of somebody else's wait. Start asking, or the answer would never reach the page.
  if (op) startWaitWatch(slot, json, true);
}

// ---------------------------------------------------------------------------
// A GROUP WHOSE ROWS ARE A LIVE PICTURE OF SOMETHING OUTSIDE THE PAGE (apex/abi.h, `live`, ABI 17 -> 18)
//
// ⚠️ WHAT IT REPLACED IS A BUTTON, AND THE USER SAID SO IN ONE SENTENCE: "媒体控制列表的「刷新应用列表」按钮去掉，这个
// 做成实时自动刷新". The volume group listed the programs that are making sound and carried an action that re-read
// them; a list like that is not a document with a version, it is a fact about the machine that changes while the
// user is looking at it -- so a button that re-reads it is a button that asks them to keep pressing it.
//
// ⚠️ THE FEATURE SAYS *WHAT*, THE PAGE DECIDES *HOW OFTEN*, and the split is the same one the wait has: "these
// rows are live" is something only the feature can know (the page cannot tell a list that changes by itself from
// one the user typed -- KeepAwake's program list is the second kind), while the rate is a property of the panel.
// It is ONE SECOND, deliberately slower than the 200 ms capture wait: that one is chasing a click the user just
// made and ends by itself, and this one runs for as long as the page is open. The feature is expected to make a
// re-read cheap -- MediaControl caches its enumeration for 800 ms for exactly this reason.
//
// ⚠️ AND IT COSTS ALMOST NOTHING WHEN NOTHING CHANGES: an answer identical to the last one is not redrawn (the
// guard at the top of `__apexControls`), so an idle page on a live group is one small round trip a second.
var kLiveGroupMs = 1000;
var liveGroupTimer = 0;

function stopLiveGroups() {
  if (liveGroupTimer) clearInterval(liveGroupTimer);
  liveGroupTimer = 0;
}

/// Start watching the page's live groups, or stop when it has none. Called with every document that is drawn.
function syncLiveGroups(slot) {
  var params = (S.controls && S.controls.params) || [];
  var live = false;
  for (var i = 0; i < params.length; i++) {
    var p = params[i];
    if (p && p.type === "group" && p.live) { live = true; break; }
  }
  if (!live) { stopLiveGroups(); return; }
  if (liveGroupTimer) return; // already watching this page; the answer that arrived is what started it
  liveGroupTimer = setInterval(function () {
    // ⚠️ THE SAME TWO GUARDS THE WAIT WATCH USES, for the same two reasons: there is nothing to ask through
    // without a connection, and nothing to ask ABOUT once the user has left the page.
    if (!S.snap || !S.connected) return;
    if (String(S.view) !== String(slot)) return;
    // QUIET: an identical answer is not drawn (see `askControls`) -- this question is asked once a second.
    askControls(slot, true);
  }, kLiveGroupMs);
}

function CurveFrame() {
  curveAnim = 0;
  if (!curveCanvas || !curveObj) return;
  drawCurve(curveCanvas, curveObj);
  // Keep the loop alive only while a ball is still travelling: an idle panel does no work at all, the same
  // rule the host's engine follows.
  var now = Date.now(), running = false;
  for (var i = 0; i < curveBorn.length; i++) {
    if (curveBorn[i] && now - curveBorn[i] < curveSpanMs) { running = true; }
  }
  if (running) curveAnim = requestAnimationFrame(CurveFrame);
}

function SpawnBall(atMs) {
  for (var i = 0; i < CURVE_BALLS; i++) {
    // A FREE SLOT, or one whose ball has already arrived. Never one that is still travelling: a ball that
    // vanished mid-flight reads as a glitch.
    if (!curveBorn[i] || atMs - curveBorn[i] >= curveSpanMs) { curveBorn[i] = atMs; return; }
  }
  // Every slot genuinely busy: this one is not sent. The balls already out keep arriving, so the roll still
  // reads as continuous motion.
}

// THE CHART. Everything about how it looks lives here; the feature only supplies numbers (see the `curve` block
// in apex/abi.h), which is what keeps this a generic renderer rather than a picture of wheels.
//
// WHAT IS DRAWN, and each part has a job:
//
//   * the GRID, from the feature's own tick steps, with the numbers printed. The axes are the two scales the
//     parameters actually move -- x is real milliseconds, y is real wheel deltas -- so a slider that rescales
//     the picture moves the NUMBERS too. A fixed 1/6 grid would sit in the same place whatever the sliders
//     said, which makes it decoration.
//   * the NATIVE LINE, where the raw wheel's own travel tops out.
//   * the WHEEL's path as a dotted staircase: one hop per notch, the raw input this picture is an answer to.
//   * the SHAPED path, IN ONE COLOUR PER SEGMENT -- see below.
//   * the BALLS, one per wheel the feature took, riding the shaped path (see __apexActivity).
//
// ⚠️ THE SEGMENT COLOURS COME FROM THE SLIDERS THEMSELVES. The feature tags each point with which parameter
// owns it (`seg`), and this looks the colour up in the CONTROL LIST it was given -- not in a table of its own.
// So the rise is Slow step's colour, the climb is Ramp-up's, the flat top is Top speed's, and they cannot come
// out different from the sliders: there is one place the colour is written down, and it is the slider.
//
// The colour is BLENDED toward the card's background by the slider's own position, which is how the plugin does
// it: a parameter at 30% is a faint version of its hue, one at 100% is the full colour, and the picture says at
// a glance how far up each control is. (That blend is why this needs the CSS variables rather than the hues
// alone.)
function drawCurve(canvas, curve) {
  var dpr = window.devicePixelRatio || 1;
  var w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  var g = canvas.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);

  var css = getComputedStyle(document.documentElement);
  var grid = css.getPropertyValue("--line").trim() || "#2c2c2c";
  var sub = css.getPropertyValue("--sub").trim() || "#888";
  var panelBg = css.getPropertyValue("--panel").trim() || "#1a1a1a";
  var fg = css.getPropertyValue("--fg").trim() || "#fff";

  if (!curve || !curve.shape || curve.shape.length < 2) {
    // No chart (a feature that sends none): a baseline, so the card still reads as a chart area rather than as
    // a box that failed to load.
    g.strokeStyle = grid;
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(0, h - 0.5);
    g.lineTo(w, h - 0.5);
    g.stroke();
    return;
  }

  // THE PLOT BOX, inset so the tick numbers have room: the y labels sit down the left edge and the x labels
  // under the floor, both inside the canvas so nothing is clipped.
  var padL = 34, padR = 8, padT = 8, padB = 18;
  var x0 = padL, x1 = w - padR, y0 = h - padB, y1 = padT;
  var wIn = x1 - x0, hIn = y0 - y1;
  if (wIn < 20 || hIn < 20) return; // a card too small to draw in: nothing readable fits
  var toX = function (u) { return x0 + u * wIn; };
  var toY = function (v) { return y0 - v * hIn; };

  // The axis lines.
  g.strokeStyle = grid;
  g.lineWidth = 1;
  g.beginPath();
  g.moveTo(x0, y0); g.lineTo(x1, y0);
  g.moveTo(x0, y1); g.lineTo(x0, y0);
  g.stroke();

  // THE GRID AND ITS NUMBERS. The box is filled by the curve (see the feature: it is sized so the motion reaches
  // its top and right edge), and the ticks are labelled in REAL units -- milliseconds along x, wheel deltas up y
  // -- from the two ranges the feature sends. Those ranges MOVE with the settings, so a slider that changes one
  // changes these numbers; a fixed 1/6 grid would sit in the same place whatever the sliders said.
  var spanMs = curve.spanMs > 0 ? curve.spanMs : 1;
  var yTop = curve.yTopDeltas > 0 ? curve.yTopDeltas : 1;
  g.fillStyle = sub;
  g.font = "10px system-ui, sans-serif";
  g.textBaseline = "top";
  g.textAlign = "center";
  if (curve.stepX > 0) {
    for (var vx = curve.stepX; vx < spanMs - 1e-9; vx += curve.stepX) {
      var px = toX(vx / spanMs);
      g.strokeStyle = grid;
      g.beginPath(); g.moveTo(px, y1); g.lineTo(px, y0); g.stroke();
      g.fillText(String(Math.round(vx)), px, y0 + 3);
    }
  }
  g.textAlign = "right";
  g.textBaseline = "middle";
  if (curve.stepY > 0) {
    for (var vy = curve.stepY; vy < yTop - 1e-9; vy += curve.stepY) {
      var py = toY(vy / yTop);
      g.strokeStyle = grid;
      g.beginPath(); g.moveTo(x0, py); g.lineTo(x1, py); g.stroke();
      g.fillText(String(Math.round(vy)), x0 - 4, py);
    }
  }

  // THE NATIVE LINE: where the raw wheel's own travel tops out. It is the reference the whole picture is
  // measured against, so it is drawn under everything else and in the muted colour.
  if (curve.nativeY > 0 && curve.nativeY <= 1) {
    g.strokeStyle = sub;
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(x0, toY(curve.nativeY));
    g.lineTo(x1, toY(curve.nativeY));
    g.stroke();
  }

  // THE WHEEL'S OWN PATH: a dotted staircase, one hop per notch.
  if (curve.native && curve.native.length >= 2) {
    g.save();
    g.setLineDash([2, 3]);
    g.strokeStyle = sub;
    g.lineWidth = 1;
    g.beginPath();
    for (var i = 0; i < curve.native.length; i++) {
      var nx = toX(curve.native[i][0]), ny = toY(curve.native[i][1]);
      if (i === 0) g.moveTo(nx, ny); else g.lineTo(nx, ny);
    }
    g.stroke();
    g.restore();
  }

  // THE SHAPED PATH, broken into runs by segment and stroked in each segment's own colour. A run is started with
  // moveTo at the colour change rather than drawn through it, so no line is drawn across the boundary -- the
  // plugin's own note: "a fresh run: no line drawn across the colour change".
  // WHICH PARAMETER OWNS WHICH SEGMENT -- and the panel ASKS, it does not know.
  //
  // ⚠️ THIS USED TO BE A HARD-CODED LIST OF ONE FEATURE'S SLIDER IDS (`["slow","ramp","top"]`), and that is
  // exactly the kind of thing that makes "a feature is a folder" untrue: the page would have worked perfectly
  // for SmoothWheel and drawn every later feature's chart in a single colour, silently, with no error
  // anywhere. (Found by auditing the claim rather than by a failure -- see AGENTS.md §3.10.)
  //
  // Now each `range` says which segment it colours (`seg`, beside its `hue`), so the mapping arrives with the
  // data. The panel's job is only to pair them up: for each segment number, the hue of the slider that
  // claimed it. A segment no slider claims is drawn in the foreground colour -- visible, and honest about
  // being unclaimed. A feature may therefore ship fewer sliders than segments, or none at all.
  //
  // ⚠️ IT READS THE CONTROLS THROUGH `S.controls`, NOT A LOCAL NAME, and that is not laziness: drawCurve is
  // called from renderFeature, which has its own local `params`, and referring to that here threw "params is
  // not defined" -- different scopes, only one visible at this point. Reading the object where it lives cannot
  // be wrong about scope (the probe caught that one).
  var ctlParams = (S.controls && S.controls.params) || [];
  function segColor(seg) {
    var p = null;
    ctlParams.forEach(function (x) { if (x.seg === seg && p === null) p = x; });
    return (p ? channelColor(p.hue) : null) || fg;
  }

  var LINE = 3; // heavier than a hairline: it is the subject of the picture
  var prevSeg = -1, started = false;
  g.lineWidth = LINE;
  g.lineJoin = "round";
  g.lineCap = "round";
  for (var k = 0; k < curve.shape.length; k++) {
    var p = curve.shape[k];
    var cx = toX(p[0]), cy = toY(p[1]);
    var seg = p.length > 2 ? p[2] : 0;
    if (seg !== prevSeg) {
      if (started) g.stroke();
      g.beginPath();
      g.strokeStyle = segColor(seg);
      g.moveTo(cx, cy);
      started = true;
      prevSeg = seg;
    } else {
      g.lineTo(cx, cy);
    }
  }
  if (started) g.stroke();

  // THE BALLS, on top of the line they are riding -- one per wheel the feature took (see __apexActivity). A
  // ball's place is a fraction of the elapsed time and its height is the curve's own value there, so it is ON
  // the line at every moment: it shows WHERE in the motion that wheel currently is.
  if (!curveBorn.length) return;
  var now = Date.now();
  for (var b = 0; b < curveBorn.length; b++) {
    if (!curveBorn[b]) continue;
    var u = (now - curveBorn[b]) / curveSpanMs;
    if (u >= 1) { curveBorn[b] = 0; continue; }
    var by = curveValueAt(curve.shape, Math.max(0, Math.min(1, u)));
    g.beginPath();
    g.arc(toX(Math.max(0, Math.min(1, u))), toY(by), 4, 0, Math.PI * 2);
    g.fillStyle = fg;
    g.fill();
  }
}

// A point of the shaped path with segment tags: [x, y, seg]. Interpolating must IGNORE the tag -- averaging a
// segment number would invent a segment that does not exist.
function curveValueAt(shape, u) {
  if (!shape || shape.length < 2) return 0;
  if (u <= shape[0][0]) return shape[0][1];
  if (u >= shape[shape.length - 1][0]) return shape[shape.length - 1][1];
  for (var i = 1; i < shape.length; i++) {
    if (u <= shape[i][0]) {
      var a = shape[i - 1], b = shape[i];
      var f = (b[0] > a[0]) ? ((u - a[0]) / (b[0] - a[0])) : 0;
      return a[1] + (b[1] - a[1]) * f;
    }
  }
  return shape[shape.length - 1][1];
}

// `#rrggbb` blended toward another: `a` is how much of `hex` comes through (0 = all `toward`, 1 = all `hex`).
function mixHex(toward, hex, a) {
  function part(s, i) { return parseInt(s.substr(i, 2), 16); }
  var t = toward.charAt(0) === "#" ? toward.substr(1) : toward;
  var c = hex.charAt(0) === "#" ? hex.substr(1) : hex;
  if (t.length !== 6 || c.length !== 6) return hex;
  var r = Math.round(part(t, 0) + (part(c, 0) - part(t, 0)) * a);
  var gg = Math.round(part(t, 2) + (part(c, 2) - part(t, 2)) * a);
  var bl = Math.round(part(t, 4) + (part(c, 4) - part(t, 4)) * a);
  return "rgb(" + r + "," + gg + "," + bl + ")";
}

// THE COLOUR OF ONE PARAMETER, AS THE USER SEES IT -- and there is exactly ONE of this function on purpose.
//
// It is used twice: for the slider's own thumb, and for the piece of the motion chart that parameter owns. If
// each place computed it, they would be two implementations of "what colour is Ramp-up" and would eventually
// disagree -- which is the failure the plugin's table avoids by keeping the hue in the control row, and which
// this avoids by keeping the BLEND here.
//
// ⚠️ THE COLOUR DOES NOT DEPEND ON THE VALUE, AND THAT WAS THE USER'S POINT: a slider has ONE colour, always
// the same one, so "the green one" is a way to refer to Ramp-up whether it is at 60 or 2000. The first version
// faded the colour with the value -- the plugin does that, and it makes the row and the curve agree moment to
// moment, but it also means the control changes identity as you drag it and the four colours are unreadable at
// the bottom of their ranges. Fixed is what was asked for.
function channelColor(hue) {
  if (!hue) return null;
  var bg = getComputedStyle(document.documentElement).getPropertyValue("--panel").trim();
  // Blended only enough to sit on the card rather than glare against it; the hue is fully present.
  return mixHex(bg && bg.charAt(0) === "#" ? bg : "#1a1a1a", hue, 0.85);
}
function renderConn() {
  var n = $("nohost");
  if (S.connected) { n.style.display = "none"; return; }
  // ⚠️ "NOT ANSWERED YET" IS NOT "THE HOST IS NOT RUNNING", and the panel used to say the second one while the
  // first was true -- on every launch, in red. `null` is the unknown state (see the note on S.connected); it is
  // resolved by the first snapshot, by the one-shot timer in boot(), or by __apexHostGone.
  var waiting = (S.connected === null);
  n.textContent = waiting ? t("loading") : t("noHost");
  n.className = waiting ? "wait" : "";
  n.style.display = "block";
}

// TAKE THE HOST'S OWN VALUES AS THE STARTING POINT, before anything is drawn.
//
// This exists because of a real bug: __apexSnapshot called it and the function did not exist. The call threw
// a ReferenceError, which killed the rest of the handler -- so `connected` was set, the drawing never
// happened, and the panel showed its "the host is not running" banner while everything behind it had
// worked. Two lessons are baked in here: a page that dies mid-handler looks exactly like a page that never
// got the message, and __apexSnapshot must not call anything that is not defined above it.
//
// What it does: resolves `auto` into the language actually being used, and applies the saved theme, so the
// first frame is already correct instead of flashing the defaults and then correcting itself.
function applyHostToUi() {
  var h = S.snap.host;

  S.lang = (h.lang === "auto") ? (h.systemIsChinese ? "zh" : "en") : h.lang;

  S.theme = h.theme;
  applyTheme(h.theme);

  // ⚠️ TWO ELEMENTS, NOT ONE: the slogan text and the host's version share a line, and `textContent = ...` on
  // their parent would delete the version along with the old words ("端/Apex下方的...右侧，字号相比小一号，居右").
  document.getElementById("slogantext").textContent = t("slogan");
  document.getElementById("hostver").textContent = h.version ? ("v" + h.version) : "";
  document.documentElement.lang = (S.lang === "zh") ? "zh" : "en";
}

function renderNav() {
  var nav = $("nav");
  nav.innerHTML = "";
  // The general entry: selected-ness only. The CLICK is attached once, at boot -- attaching it here would
  // stack one listener per render, and the symptom of that is a view that redraws several times per click.
  $("brand").className = (S.view === "general") ? "sel" : "";
  if (!S.snap) return;
  S.snap.features.forEach(function (f) {
    var it = el("div", "item" + (String(f.slot) === String(S.view) ? " sel" : "") + (f.ok ? "" : " dead"));
    var nm = el("div", "nm", S.lang === "zh" ? (f.nameZh || f.nameEn) : (f.nameEn || f.nameZh));
    if (!f.ok) nm.textContent = (S.lang === "zh" ? "加载失败" : "failed to load");
    it.appendChild(nm);
    // THE SWITCH IS ALWAYS APPENDED -- see the CSS. Three states, and the distinction between the last two is
    // the point: "I turned this off" and "it failed to load" both stop it working and want different
    // reactions from the user -- and only one of them can be undone by clicking.
    var state = !f.ok ? "disabled" : (f.off ? "off" : "on");
    var sw = el("div", "dot dot-" + state);
    sw.title = state === "disabled"
      ? (S.lang === "zh" ? "加载失败，无法启用" : "failed to load")
      : (f.off ? (S.lang === "zh" ? "已停用，点一下启用" : "off -- click to enable")
               : (S.lang === "zh" ? "运行中，点一下停用" : "on -- click to disable"));
    // ⚠️ A FEATURE THAT FAILED TO LOAD HAS NO SWITCH. `featureOff` is an IPC call INTO the host, and the host
    // cannot switch on what it could not load -- so the click would be sent, refused, and the row would redraw
    // identically. Leaving it inert is the honest rendering of "there is nothing to turn on here".
    if (state !== "disabled") {
      sw.addEventListener("click", function (e) {
        // ⚠️ THE CLICK MUST NOT REACH THE ROW. Clicking a switch is not "open this feature": without this the
        // same click would also navigate to the page, and the user would be looking at a feature they were
        // only trying to switch off.
        e.stopPropagation();
        hostCall("featureOff", { slot: f.slot, value: f.off ? 0 : 1 });
        // Optimistic, like the page's own switch: the host may be slow to answer and a switch that waits for
        // a round trip feels broken. The next snapshot corrects it if the host disagreed.
        f.off = !f.off;
        renderNav();
        // The feature page draws its controls dimmed while the feature is off, so if that page is open it has
        // to be told too -- otherwise the switch and the page disagree about the same feature.
        if (String(S.view) === String(f.slot)) renderFeature(f.slot);
      });
    }
    it.appendChild(sw);
    it.addEventListener("click", function () {
      // ⚠️ THROUGH loadFeature, NOT `S.view = f.slot; renderAll()`. The controls are not part of the
      // snapshot: the page has to ASK for them, and only loadFeature sends that request. Selecting a feature
      // with renderAll alone drew its page from `S.controls`, which was still null -- so the feature appeared
      // with its switch and an EMPTY CARD where its parameters belong. That is "设置页面不全", and it was
      // not a missing render: the request was never made.
      //
      // ⚠️ AND THE PREVIOUS FEATURE'S PARAMETERS GO WITH THE PAGE CHANGE. `loadFeature` clears `S.controls` when
      // the SLOT changes (and only then), which is exactly this case: without it the old feature's controls would
      // be drawn under the new one's name for as long as the answer takes to arrive.
      loadFeature(f.slot);
    });
    nav.appendChild(it);
  });
}

// `hasFile` is false for a feature that failed to load: its page is an explanation, and the settings-file
// button would ask the host to open a file that does not exist.
function renderHead(name, ver, hasFile, desc) {
  $("title").textContent = name;
  $("ver").textContent = ver || "";
  // The feature's own one-line description, in the reader's language (see `summaryZh`/`summaryEn` in abi.h).
  $("desc").textContent = desc || "";
  var show = (S.view !== "general") && (hasFile !== false);
  $("btnFile").style.display = show ? "" : "none";
  $("btnFile").textContent = t("settingsFile");
}

function renderGeneral() {
  renderHead(t("generalTitle"), "");
  var b = $("body");
  var keep = b.scrollTop;
  b.innerHTML = "";

  var c2 = el("div", "card gen");
  // ⚠️ START WITH WINDOWS COMES FIRST: "开机启动移到第一项." It is the one setting here that changes what the
  // machine does rather than how the panel looks, so it is the one a user comes to this page for.
  //
  // ⚠️ AND IT IS OFF UNLESS THE USER TURNS IT ON. Its effect is outside this program (see `autostart` in
  // hostconfig.h: a value under HKCU\...\Run). The switch is the same control as every other one, at the same
  // size as the feature list's -- "滑动按钮也保持一致".
  c2.appendChild(switchRow(t("autostart"), !!Number(S.snap.host.autostart), function (v) {
    hostCall("setHost", { key: "autostart", value: v ? "1" : "0" });
    // Optimistic, like the feature switches: the host applies it immediately, and the next snapshot corrects the
    // display if it refused. (It can refuse -- an administrator policy can deny the write -- and the log says so.)
    S.snap.host.autostart = v ? 1 : 0;
  }));
  c2.appendChild(selectRow(t("language"), [
    { value: "auto", label: t("auto") + " (" + S.snap.host.systemLang + ")" },
    { value: "zh", label: "中文" },
    { value: "en", label: "English" }
  ], S.snap.host.lang, function (v) {
    hostCall("setHost", { key: "lang", value: v });
    S.snap.host.lang = v;
    S.lang = (v === "auto") ? (S.snap.host.systemIsChinese ? "zh" : "en") : v;
    renderAll();
  }));
  c2.appendChild(selectRow(t("theme"), [
    { value: "auto", label: t("auto") + (S.snap.host.systemLight ? " (light)" : " (dark)") },
    { value: "light", label: t("light") },
    { value: "dark", label: t("dark") }
  ], S.snap.host.theme, function (v) {
    hostCall("setHost", { key: "theme", value: v });
    S.snap.host.theme = v;
    applyTheme(v);
  }));
  // ⚠️ WHAT THE QUICK PANEL SHOWS -- the two switches the user asked for, ON ONE LINE because they are ONE
  // feature seen from two sides ("它是一个单独的功能，一行两个开关"): the host's own list of every feature's on/off
  // switch, and the controls the features asked for through `quickItems`. Both default to on, so the first
  // click on the tray icon shows something.
  //
  // ⚠️ AND THE PAGE DOES NOT NEED TO TELL ANYONE. The flyout is in the HOST's process and rebuilds itself from
  // these settings every time it opens (see Rebuild in quickpanel_win.cpp), so the next click already shows the
  // new answer -- there is no live window to refresh, and this page is the only place they can be changed.
  c2.appendChild(switchRowPair(t("quickPanel"), [
    {
      label: t("quickCompact"), checked: Number(S.snap.host.quickCompact) !== 0,
      onToggle: function (v) {
        hostCall("setHost", { key: "quickcompact", value: v ? "1" : "0" });
        S.snap.host.quickCompact = v ? 1 : 0;
      }
    },
    {
      label: t("quickOwn"), checked: Number(S.snap.host.quickOwn) !== 0,
      onToggle: function (v) {
        hostCall("setHost", { key: "quickown", value: v ? "1" : "0" });
        S.snap.host.quickOwn = v ? 1 : 0;
      }
    }
  ]));
  b.appendChild(c2);

  // ---- ⚠️⚠️ AND WHAT IS *ACTUALLY* IN IT, IN THE ORDER IT IS DRAWN ------------------------------------------
  //
  // The user's request: "设置通用里，把快速面板功能单独给一个小面板，在两个总开关下面实时给出当前有映射在快速面板的
  // 功能，并能实现上下排序。总开关不参与排序，固定最上面."
  //
  // So: a card of its own, under the two switches -- which stay where they are and are NOT in the list, because
  // they are not features and nothing about them is an order. What the card lists is the features that currently
  // put something into the flyout, and the host answers that question with the very list it is about to draw
  // (`quickOrder` in the snapshot), so this page never has to work out which feature maps what.
  //
  // ⚠️ THE ORDER IS THE HOST'S ANSWER TOO, and the page only ever hands it BACK (see moveQuickOrder). A page that
  // sorted the rows itself would be a second implementation of the flyout's order, and the two could disagree --
  // which is the one failure this card must not have, since its whole job is to show the user what the flyout
  // will do.
  b.appendChild(quickOrderCard());

  // ⚠️ THE BLACKLIST CARD USED TO BE HERE, AND IT MOVED TO THE FEATURE THAT OWNS IT. "Do not smooth in
  // this program" is a statement about smoothing, not about Apex: a future feature with nothing to do with
  // wheels has no reason to inherit a wheel-shaped list. It is now one of SmoothWheel's own controls
  // (`type: "list"`), edited by the generic listRow below and stored in SmoothWheel.ini -- and the host
  // never sees it. The panel got no blacklist-specific code out of the move; it lost some.

  b.scrollTop = keep; // see the note in renderFeature -- a redraw must not move the page
}

// THE QUICK PANEL'S OWN SMALL PANEL: WHICH PANES ARE IN IT, AND IN WHAT ORDER.
//
// ⚠️⚠️ A ROW IS A PANE, NOT A PLUGIN, AND THAT IS THE USER'S OWN CORRECTION: "快速面板的局部功能分组是按单个开关算的，
// 不是按插件算，所以这边排序要注意". The flyout draws one pane per control (or per named group of them -- MediaControl's
// 亮度 and 音量, KeepAwake's list, SmoothWheel's four faders), so this list shows those panes, with the names the
// flyout prints as their headings. A list of FEATURES would have shown a different thing from what the user is
// looking at, and could not have put 亮度 before 音量 at all.
//
// ⚠️ IT LISTS WHAT IS *THERE*, NOT WHAT IS INSTALLED: a feature reaches the flyout only when the user has switched
// a mapping on inside that feature's own page (the permission rule, see apex/abi.h), and a feature the user has
// switched OFF contributes no panes at all (its mapping switches are untouched, so switching it back on restores
// them -- the host does that, not this page). A pane that is not there has no order to be part of.
//
// ⚠️ AND THE ROWS ARE REORDERED THE WAY EVERY OTHER LIST IN THIS PROGRAM IS -- by dragging (see `makeDraggable`,
// the SAME helper the rule list and the monitor list use). One way to reorder, not two.
function quickOrderCard() {
  var card = el("div", "card");
  var order = (S.snap && S.snap.quickOrder) || [];

  var head = el("div", "row");
  head.appendChild(el("label", null, t("quickOrder")));
  if (order.length > 1) head.appendChild(el("div", "rownote", t("quickOrderHint")));
  card.appendChild(head);

  // "Nothing is mapped" and "something went wrong" must not look the same -- the page says which.
  if (!order.length) {
    card.appendChild(el("div", "note", t("quickOrderEmpty")));
    return card;
  }

  var list = el("div", "qolist");
  order.forEach(function (pane, i) {
    var row = el("div", "qorow");
    // A GRIP, because a row that can be dragged has to LOOK draggable -- the same statement the rule list makes
    // with `cursor: grab` and a marker at the left of each row.
    row.appendChild(el("span", "grip", "\u2261"));
    row.appendChild(el("div", "nm", S.lang === "zh" ? (pane.nameZh || pane.key) : (pane.nameEn || pane.key)));
    // ONE MESSAGE PER DRAG, carrying the whole order: the host replaces what it has (see `quickorder` in
    // hostconfig.h), so a row moved six places is one write rather than six.
    makeDraggable(row, i, function (from, to) { moveQuickOrder(from, to); });
    list.appendChild(row);
  });
  card.appendChild(list);
  return card;
}

/// Put the row the user dragged where they dropped it, and tell the host the whole order.
function moveQuickOrder(from, to) {
  var order = ((S.snap && S.snap.quickOrder) || []).slice();
  if (from === to || from < 0 || to < 0 || from >= order.length || to >= order.length)
    return;
  var moved = order.splice(from, 1)[0];
  order.splice(to, 0, moved);
  // ⚠️ COMMA-SEPARATED, AND A KEY CANNOT CONTAIN ONE: the host builds the keys (QuickBlockKey in apex/quickpanel.h)
  // and replaces a comma or a newline in a NAME with a space. A newline would be worse than a comma -- the request
  // protocol is `key=value` LINES, so a value containing one arrives TRUNCATED AT ITS FIRST LINE. That is exactly
  // the bug this line used to have: the host received only the first key, so a drag could move at most one row.
  // The user's report: "快速面板分组不能调顺序".
  var keys = order.map(function (pane) { return pane.key; }).join(",");
  // NOT DRAWN HERE: the host answers every `setHost` with a snapshot, and `__apexSnapshot` re-renders this page
  // from the order the host really stored. Drawing our own guess first would be the second implementation of the
  // order that this card exists to avoid.
  hostCall("setHost", { key: "quickorder", value: keys });
}

// A LIST THE USER EDITS -- one of A FEATURE's controls, and the panel has no idea what is in it.
// This is the generic version of what used to be the blacklist card: a heading, the rows with a remove
// button each, and an add field. A `list` parameter from `describe` gets this, whatever it means -- a
// blacklist today, something else in the next feature.
//
// The feature validates (see ApexFeature::listOp): it may lower-case, normalise a path, refuse a duplicate
// or refuse outright. So after every edit the panel ASKS AGAIN rather than assuming the row appeared --
// otherwise a refused row would show as added and then vanish on the next refresh.
function listRow(p, slot) {
  var card = el("div", "card");
  // THE LABEL ROW, and the feature may put a NOTE in it -- smaller and greyer, to the right of the label.
  //
  // ⚠️ THE NOTE IS THE FEATURE'S SENTENCE, NOT THE PANEL'S. The page is handed the text and draws it; it does
  // not know what it means, which is the same rule as everywhere else here (the panel draws what it is given).
  // That matters for this one in particular: the text says the REAPER plugin is running, and "what counts as
  // the plugin running" is a fact about another program that only the host can check.
  //
  // ⚠️ AND IT IS ONLY SENT WHEN IT IS TRUE, so there is nothing to switch off here -- an absent note is an
  // absent field. Nothing in this row is about REAPER or about any other feature.
  var head = el("div", "row");
  head.appendChild(el("label", null, S.lang === "zh" ? p.labelZh : p.labelEn));
  var note = S.lang === "zh" ? p.noteZh : (p.noteEn || p.noteZh);
  if (note) head.appendChild(el("div", "rownote", note));
  card.appendChild(head);
  var values = p.values || [];
  if (!values.length) {
    card.appendChild(el("div", "note", t("empty")));
  } else {
    values.forEach(function (name, i) {
      // The last row is marked so it can drop its bottom border (see the CSS). The row that is last knows it;
      // a selector would have to guess, and the guess breaks when anything else lands in this card.
      var r = el("div", "skiprow" + (i === values.length - 1 ? " last" : ""));
      r.appendChild(el("div", "nm", name));
      var btn = el("button", null, t("remove"));
      btn.addEventListener("click", function () {
        hostCall("listOp", { slot: slot, id: p.id, op: "remove", index: i });
        refreshControls(slot);
      });
      r.appendChild(btn);
      card.appendChild(r);
    });
  }
  var add = el("div", "addrow");
  var inp = el("input");
  inp.type = "text";
  inp.placeholder = S.lang === "zh" ? (p.placeholderZh || "") : (p.placeholderEn || "");
  var ab = el("button", "addbtn", t("add"));
  function doAdd() {
    var v = inp.value.trim();
    if (!v) return;
    inp.value = "";
    hostCall("listOp", { slot: slot, id: p.id, op: "add", value: v });
    refreshControls(slot);
  }
  ab.addEventListener("click", doAdd);
  inp.addEventListener("keydown", function (e) { if (e.key === "Enter") doAdd(); });
  add.appendChild(inp);
  add.appendChild(ab);
  card.appendChild(add);
  return card;
}

// THE FEATURE'S ONE-LINE DESCRIPTION, in the reader's language -- out of the document it sent (apex/abi.h,
// `summaryZh`/`summaryEn`). Read here rather than kept in `S` because it belongs to whatever document is on
// screen: a stale copy would describe the previous feature on the new feature's page.
function featureSummary() {
  if (!S.controls) return "";
  var zh = S.controls.summaryZh, en = S.controls.summaryEn;
  return (S.lang === "zh") ? (zh || en || "") : (en || zh || "");
}

// ---- THE NAME COLUMN OF A "rows" LIST: AS SHORT AS ITS OWN NAMES, AND THE SAME IN EVERY ROW ----
//
// ⚠️⚠️ THE USER'S WORDS, AND THEY ASK FOR THREE THINGS AT ONCE: "设置里，设备名称宽度自适应缩到最短，给推子让空间，但每个
// 小面板各控制要保持对齐." Each of the three rules out one of the obvious answers:
//
//   * the column used to be the FLEXIBLE one (`flex: 1`), so it ate every spare pixel of the row and the fader sat
//     at a fixed 150 px in the middle of a 800 px card;
//   * `width: max-content` on each row would be as short as possible but NOT aligned -- three monitor names are
//     three different widths, so the faders would start at three different places;
//   * one fixed width for every card would align but waste whatever a card's short names leave over
//     (「系统声音」, "firefox").
//
// So the width is MEASURED, once per card: the widest name in THAT card, written onto every name cell in it. Two
// spans of the same font and the same text are the same width, so this is a layout question with an arithmetic
// answer rather than a guess -- and it is why the whole thing is one function with one caller.
//
// ⚠️ AND IT MUST SURVIVE AN ENVIRONMENT WITH NO LAYOUT AT ALL. This page also runs against a DOM stub
// (_diag/apex_panel_probe.js), where `offsetWidth` is not a number and `getComputedStyle` may be missing -- and a
// page that THREW here would not render at all in the probe (the same failure `barItems` was written to avoid,
// see the note in `groupCard`). So every step is guarded and "cannot measure" leaves the stylesheet's own width
// standing: the page then looks exactly like it did before this function existed, which is a good failure.
function fitNameColumn(card) {
  if (!card || !card.querySelectorAll) return;
  var cells = card.querySelectorAll(".skiprow > .nm");
  if (!cells || !cells.length) return;

  var widest = 0;
  for (var i = 0; i < cells.length; ++i) {
    var text = cells[i].textContent || "";
    if (!text) continue;
    var probe = document.createElement("span");
    probe.textContent = text;
    var cs = (window.getComputedStyle ? window.getComputedStyle(cells[i]) : null);
    if (cs) {
      probe.style.fontFamily = cs.fontFamily;
      probe.style.fontSize = cs.fontSize;
      probe.style.fontWeight = cs.fontWeight;
      probe.style.fontStyle = cs.fontStyle;
      probe.style.letterSpacing = cs.letterSpacing;
    }
    // Off the layout, but still laid out: `visibility: hidden` keeps the box measurable while nothing is drawn.
    probe.style.position = "absolute";
    probe.style.visibility = "hidden";
    probe.style.whiteSpace = "nowrap";
    probe.style.pointerEvents = "none";
    card.appendChild(probe);
    var w = probe.offsetWidth;
    card.removeChild(probe);
    if (typeof w !== "number" || !isFinite(w) || w <= 0) return; // no layout here: leave the CSS width alone
    if (w > widest) widest = w;
  }
  if (widest <= 0) return;
  // ⚠️ A FLOOR AND A CEILING, BOTH FOR THE SAME REASON: this is a NAME column beside a control, not the row's
  // content. Below the floor a name is unreadable noise; above the ceiling one absurd program name would push the
  // fader back to where the user just asked for it not to be. Both are the kind of number that should be visible
  // rather than hidden in a stylesheet, and they are also the two ends the probe checks.
  if (widest < 70) widest = 70;
  if (widest > 220) widest = 220;
  for (var j = 0; j < cells.length; ++j)
    cells[j].style.width = widest + "px";
}

// Every rows-card on the page. ⚠️ CALLED ONCE PER RENDER, AFTER THE PAGE IS IN THE DOCUMENT -- a detached card has
// no layout to measure (every `offsetWidth` is 0), so measuring while the card was being built would silently do
// nothing at all and leave the column as wide as the stylesheet says.
function fitNameColumns(root) {
  if (!root || !root.querySelectorAll) return;
  var blocks = root.querySelectorAll(".rowsblock");
  for (var i = 0; i < blocks.length; ++i) fitNameColumn(blocks[i]);
}

function renderFeature(slot) {
  var f = null;
  S.snap.features.forEach(function (x) { if (String(x.slot) === String(slot)) f = x; });
  if (!f) { S.view = "general"; renderAll(); return; }

  // ⚠️ A FAILED FEATURE HAS NO VERSION AND NO FILE, so both of those must be conditional. The version comes
  // back as an empty string for a row that would not load (see the snapshot in settings_host.cpp), and
  // "v" + "" is a bare "v" sitting in the title bar; and the settings-file button asks the host to open a
  // file that does not exist, which the host refuses -- so it is a button that does nothing when pressed.
  // A failed feature's page is an explanation, and it should not carry controls that cannot act.
  renderHead(S.lang === "zh" ? (f.nameZh || f.nameEn) : (f.nameEn || f.nameZh),
             f.ok && f.version ? "v" + f.version : "", f.ok,
             f.ok ? featureSummary() : "");

  var b = $("body");
  // ⚠️⚠️ A REDRAW MUST NOT MOVE THE PAGE -- AND THIS ONE DID, EVERY TIME THE USER PRESSED 编辑.
  //
  // "点'编辑'时，参数界面会焦点会跳到顶端，这点规避掉，不要跳。保持原样."
  //
  // The cause is the two lines below: `innerHTML = ""` empties the scrolling element (#body), and an empty
  // scroll container has nothing to scroll -- so its scrollTop is reset to 0 by the browser. The page is then
  // refilled, but the position is gone.
  //
  // ⚠️ AND PRESSING 编辑 IS EXACTLY WHEN THIS HAPPENS. The click sends `listOp`, and the host answers EVERY
  // command with a snapshot (see ui_webview.cpp) -- the snapshot re-renders the page, which clears #body, which
  // throws the user back to the top. On a rule with fourteen fields that means losing your place for pressing a
  // button that did not change which rule you are looking at.
  //
  // Saving and restoring the number is enough, because the page is rebuilt from the same document: the content
  // above the fold is the same height it was, so the same scrollTop shows the same thing. (A page that came back
  // SHORTER -- a rule removed -- would clamp to its new maximum, which is the right place to end up anyway.)
  var keep = b.scrollTop;
  b.innerHTML = "";
  if (!f.ok) {
    var e = el("div", "card");
    e.appendChild(el("div", "err", f.why || ""));
    b.appendChild(e);
    return;
  }

  // ⚠️ THERE IS NO "ENABLE" SWITCH ON THIS PAGE ANY MORE -- IT IS ON THE FEATURE LIST.
  //
  // The user moved it: "插件的启动开关直接做到原来插件列表的'绿点'位置 ... 插件页各自的启用要去了，留下插件列表那个
  // 开关即可." The list is the right place for it for the reason this card originally gave for wanting ONE switch:
  // a user sees every feature's state at once there, and a second control here would be a second place the same
  // setting lives.
  //
  // ⚠️ AND THE PAGE STILL READS THE STATE -- `disabled` below dims the controls of a feature that is switched
  // off. It comes from `f.off`, which the list's switch updates optimistically, so the two cannot disagree for
  // longer than one round trip.

  // The feature's own controls, exactly as it described them -- and the panel still knows nothing about
  // them. `bool` and `range` go in one card; `list` gets a card of its own (it is a block of rows, not a
  // labelled row); the chart is the last card.
  var cc = el("div", "card");
  var params = (S.controls && S.controls.params) || [];
  var disabled = !!f.off;
  var anyRow = false;
  params.forEach(function (p) {
    // WHICH ROW TO DRAW. `list` and `group` are blocks of their own (a run of rows, or a box per item) and
    // break the card in progress; the rest are single labelled rows that share a card.
    //
    // ⚠️ THE LAST BRANCH IS A SLIDER, AND IT IS THE DEFAULT. That was true when `range` was the only
    // unlabelled numeric control and it stays true because every control added since (`select`, `text`) has
    // its own branch above it. A feature sending an unknown type therefore gets a slider -- visible and wrong
    // rather than invisible, which is the honest failure: the panel cannot invent a control it was never told
    // about, and drawing nothing would make a typo in a feature's document look like a feature with fewer
    // settings.
    if (p.type === "list") {
      if (anyRow) { b.appendChild(cc); cc = el("div", "card"); anyRow = false; }
      b.appendChild(listRow(p, slot));
    } else if (p.type === "group") {
      if (anyRow) { b.appendChild(cc); cc = el("div", "card"); anyRow = false; }
      b.appendChild(groupCard(p, slot, disabled));
    } else if (p.type === "bool") {
      cc.appendChild(switchRow(S.lang === "zh" ? p.labelZh : p.labelEn, p.value,
                              function (v) {
                                sendControl(slot, p.id, v ? "1" : "0");
                                // Only if there is a chart to redraw -- see the note in rangeRow.
                                if (S.controls && S.controls.curve) queueCurve(slot);
                              }, disabled));
      anyRow = true;
    } else if (p.type === "select") {
      cc.appendChild(selectRowCtl(p, slot, disabled, p.id));
      anyRow = true;
    } else if (p.type === "text") {
      cc.appendChild(textRow(p, slot, disabled, p.id));
      anyRow = true;
    } else if (p.type === "hotkey") {
      cc.appendChild(hotkeyRow(p, slot, disabled));
      anyRow = true;
    } else {
      cc.appendChild(rangeRow(p, slot, disabled));
      anyRow = true;
    }
  });
  if (anyRow) b.appendChild(cc);

  // ⚠️ `curve` IS AN OBJECT WITH A `shape` ARRAY IN IT -- see ApexFeature::settingsJson. THIS GUARD HAS BEEN
  // WRONG IN BOTH DIRECTIONS, and both times the symptom was identical (an empty card, from a render function
  // whose condition simply never matched):
  //
  //   * it asked for `curve.shape` while the feature sent a bare points array;
  //   * it asked for an array (`.length >= 2`) while the feature sent this object, so `.length` was undefined
  //     and the comparison false.
  //
  // The shape of the data is the feature's to change and the panel's to follow, so the guard asks the question
  // the renderer actually needs: does it have a polyline to draw?
  if (S.controls && S.controls.curve && S.controls.curve.shape &&
      S.controls.curve.shape.length >= 2) {
    var cv = el("div", "card");
    cv.id = "curveCard";
    var canvas = el("canvas");
    canvas.id = "curve";
    cv.appendChild(canvas);
    // ⚠️ AN ID, MATCHING THE CSS (`#curveLegend`). This set a CLASS, so the rule never matched and the two
    // end labels sat next to each other instead of being pushed apart -- a bug that was invisible while the
    // whole card was (see the guard above).
    var lg = el("div", null);
    lg.id = "curveLegend";
    lg.appendChild(el("span", null, t("curveSpeed")));
    lg.appendChild(el("span", null, t("curveTime")));
    cv.appendChild(lg);
    var note = el("div", "note", t("curveNote"));
    note.style.marginTop = "8px";
    cv.appendChild(note);
    b.appendChild(cv);
    // The span comes from the feature (it is the window the curve was computed from), so the ball runs at the
    // real speed of the motion rather than at a speed this page invented.
    StartCurve(canvas, S.controls.curve);
  } else if (disabled) {
    b.appendChild(el("div", "note", t("enabledOff")));
  }

  // ⚠️ PUT THE PAGE BACK WHERE IT WAS -- see `keep` at the top of this function. This is the line that stops
  // pressing 编辑 from throwing the user back to the top of a long rule.
  b.scrollTop = keep;

  // ⚠️ AND THE NAME COLUMNS ARE MEASURED HERE, ONCE, WITH THE PAGE IN THE DOCUMENT (see fitNameColumns): the
  // cards are appended above but a detached one has no layout to measure, so this cannot live inside `groupCard`.
  fitNameColumns(b);
}


function loadFeature(slot) {
  // ⚠️⚠️ LEAVING A FEATURE'S PAGE CANCELS AN OPEN EDIT -- BUT ONLY WHEN THE VIEW ACTUALLY CHANGES.
  //
  // The rule is the user's: "不需要取消按钮，有点保存就保存，没点保存，切走，或关了，就是取消." Walking away ends the
  // edit, and with no Cancel button that is the only thing that makes "cancel" true.
  //
  // ⚠️ AND THE FIRST VERSION OF THIS LINE DESTROYED EVERY EDIT THE MOMENT IT BEGAN. It cancelled on EVERY call,
  // and `loadFeature` is not only called when the user navigates: `__apexSnapshot` calls it to re-read the page
  // it is already on, and a snapshot arrives after EVERY `listOp` (ui_webview.cpp answers every command it does
  // not exclude). So the sequence was:
  //
  //     click Edit -> listOp begin-edit -> the host's snapshot -> loadFeature -> cancel-edit
  //
  // The draft was thrown away one message after it was made, the button went back to 编辑, and Save could never
  // be reached. The user's report -- "编辑/保存还是没做好" -- was exactly this; and it is why the page probes
  // passed while the feature did not: none of them sent the snapshot that follows.
  //
  // The honest test for "leaving" is that the SLOT CHANGED. Re-reading the page you are on is not leaving.
  var leaving = String(slot) !== String(S.view);
  if (leaving) cancelEditOnLeave();
  // ⚠️ AND THE CAPTURE WATCH STOPS WITH IT: it polls a NAMED slot four times a second, so leaving the page while
  // a capture is armed would keep asking about a feature nobody is looking at -- and every answer is one more
  // chance for the wrong document to arrive at the wrong moment. (The capture itself stays armed in the feature:
  // the next click still fills the rule, and coming back re-reads it.)
  if (leaving) stopWatchingForCapture();
  // ... and the live-group watch, which has exactly the same problem: it asks about a NAMED slot once a second,
  // so leaving the page would keep re-reading a feature nobody is looking at (see syncLiveGroups).
  if (leaving) stopLiveGroups();
  S.view = slot;
  // ⚠️⚠️ ONLY A CHANGE OF PAGE THROWS THE DOCUMENT AWAY -- AND THIS LINE IS THE ROOT OF TWO BUGS THAT LOOKED
  // UNRELATED.
  //
  // It used to be `if (force || !S.controls)`, and every re-read of the page already on screen passed
  // `force = true`: once from `__apexSnapshot` (the host answers EVERY `listOp` with a snapshot) and once from
  // `__apexStateChanged`. So any `listOp` blanked the page for one render -- and the user, on a dark theme,
  // reported the result as "新建规则后，插件面板还是会变黑，直到鼠标点击完成捕获后它才显示".
  //
  // The other half of the pair was worse, because it made a CAPTURE look broken: the panel polls the controls
  // while a capture is armed, the snapshot's re-read arrives with the SAME document (the capture has not happened
  // yet), and that delivery was taken as "the wait is over" -- so the watch died at the moment of arming and the
  // captured fields stayed invisible until something else re-read the page. The user: "捕获事件进行时，鼠标点击
  // 后结果要马上给到参数页，目前没有，要等到点击新建的规则条才会出现".
  //
  // Both are this one line. A re-read of the page you are on is not a reason to undraw it: keep what is there
  // until the new document arrives, and the blank render never happens -- which is also why `force` is gone
  // (it had no other meaning left: `askControls` below is what actually refreshes).
  if (leaving) S.controls = null;
  askControls(slot);
  renderAll();
}

// Drop any open edit by telling the feature to. Silent when there is none.
//
// ⚠️ WHAT IT READS IS THE FEATURE'S LAST ANSWER (`S.grpEditing`, refreshed by every render from the document),
// NOT AN OPINION OF THE PAGE'S OWN. The old version kept a flag the page had set when the user pressed Edit --
// which is exactly the kind of second copy that goes wrong: a capture, a refused commit or a row that moved can
// all change what the feature holds without the page being told. The only reason the page needs the number at
// all is to avoid sending a cancel for nothing, and that is a question about the FEATURE's state.
function cancelEditOnLeave() {
  if (!S.grpEditing) return;
  for (var groupId in S.grpEditing) {
    var index = S.grpEditing[groupId];
    if (typeof index !== "number" || index < 0) continue;
    var slot = (typeof S.view === "number") ? S.view : null;
    if (slot !== null) hostCall("listOp", { slot: slot, id: groupId, op: "cancel-edit", index: index });
  }
  S.grpEditing = {};
}

function renderAll() {
  // ⚠️ THE ANIMATION STOPS ON EVERY REDRAW, before anything decides what to draw. The frame loop runs on
  // requestAnimationFrame, which does not care what the page is showing: left running it would keep repainting
  // a canvas the next render detached -- a leak that looks like nothing until the panel has been left open on
  // another page for a while. renderFeature starts it again if the page it draws has a curve.
  StopCurve();
  renderConn();
  renderNav();
  if (S.view === "general") renderGeneral();
  else renderFeature(S.view);
}

// ---- theme -------------------------------------------------------------------
//
// THE PAGE FOLLOWS THE SYSTEM BY ITSELF (prefers-color-scheme, see the note in the CSS). A pinned theme
// overrides it by setting data-theme on :root, which the CSS has rules for. The host applies the SAME
// decision to the window's caption, so the two cannot disagree.
function applyTheme(mode) {
  var root = document.documentElement;
  if (mode === "light" || mode === "dark") root.setAttribute("data-theme", mode);
  else root.removeAttribute("data-theme");
  syncNativeTheme();
}

// Tell the host which way the page resolved, so it can set both parts of the chrome the page cannot reach --
// the caption AND the icon beside it, which must contrast with what the page actually looks like. It also
// says whether the theme is PINNED: the page only re-reports on a system change when it is following the
// system, so without that the host could not tell "the theme follows the system, re-read it" from "the theme
// is pinned, and this change is none of its business".
function syncNativeTheme() {
  var pinned = document.documentElement.getAttribute("data-theme") !== null;
  var dark = document.documentElement.getAttribute("data-theme") === "dark" ||
             (!pinned && window.matchMedia("(prefers-color-scheme: dark)").matches);
  hostCall("nativeTheme", { dark: dark ? 1 : 0, pinned: pinned ? 1 : 0 });
}

// ---- wiring ------------------------------------------------------------------

// THE GENERAL ENTRY, attached ONCE. It is the brand block at the top of the sidebar (see the markup), and the
// handler lives here rather than in renderNav because renderNav runs on every refresh -- attaching there
// would add one more listener per render, and a click would then draw the view several times over.
$("brand").addEventListener("click", function () {
  if (S.view !== "general") {
    cancelEditOnLeave();
    S.view = "general";
    renderAll();
  }
  // ⚠️ AND IT ASKS THE HOST AGAIN, EVERY TIME. The general page draws things the page cannot know by itself --
  // which features are mapped into the quick panel, and in what order -- and the document that says so is only
  // sent when it is asked for. The user's rule is that this list follows the mapping switches: "要能实时根据面板
  // 开关而变动". A mapping switch lives on a FEATURE's page and a `setControl` deliberately produces no snapshot
  // (see sendControl), so without this line the list would still be showing the state of the last time something
  // else happened to refresh it. The answer re-renders the page (`__apexSnapshot`), so this is the whole fix.
  if (S.snap) hostCall("snapshot");
});

$("btnFile").addEventListener("click", function () {
  if (S.view === "general") return;
  // ⚠️ THE FILE IS NAMED BY THE FEATURE, AND THE PAGE IS ONLY THE MESSENGER (apex/abi.h, `settingsFile`).
  // The host cannot know it -- the ABI fixes the FOLDER a feature owns and says nothing about a file name -- so
  // the document carries the name and this passes it along. Without it the host falls back to `<id>.ini`, which
  // for this feature does not exist: the button opened nothing at all, which is what "实现它" was about.
  var name = (S.controls && S.controls.settingsFile) ? String(S.controls.settingsFile) : "";
  hostCall("openFile", { slot: S.view, path: name });
});

// A real system theme change: the media query moves and the page follows on its own, so only the caption
// needs telling.
try {
  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", function () {
    if (!document.documentElement.getAttribute("data-theme")) syncNativeTheme();
  });
} catch (e) { /* older engines: no live listener, which only costs a repaint on the next open */ }

function boot() {
  applyTheme("auto");
  hostCall("snapshot");
  // ⚠️ THE PAGE STARTS NO TIMERS. It used to start a one-second poll here for the feature read-out; that is gone
  // with the read-out (see the note where its CSS was). The only timer this page ever sets now is the 400 ms
  // capture wait, and it is set when a capture is armed and cleared when it lands -- so an open panel that nobody
  // is touching sends nothing at all.
  // ⚠️ NO POLL HERE, AND THE ONE THAT USED TO BE HERE COULD NOT HAVE WORKED. It was a setInterval whose body
  // checked `window.chrome.webview` and then returned in both branches -- empty. It also looked at the wrong
  // thing entirely: whether THIS page's webview exists says nothing about whether the HOST process does, and
  // a page cannot see another process's windows at all. The liveness check belongs to the panel process, which
  // owns `FindWindow`; it runs there and calls __apexHostGone / __apexHostBack (see ui_webview.cpp).
  //
  // Kept as a note because the shape of the old code is seductive: a poll that reads plausibly, sits in the
  // right place, and does nothing.
  //
  // ⚠️ AND THE UNKNOWN STATE HAS TO EXPIRE ON ITS OWN. The panel can be started on its own (double-clicking
  // apex-settings.exe), and then no snapshot is ever coming -- "loading" would sit there for good, which is a
  // quieter lie than the red warning but a lie all the same. One timer, once, equal to the panel process's own
  // fallback for the same situation (see RunPanel in ui_webview.cpp).
  setTimeout(function () {
    if (S.connected === null) {
      S.connected = false;
      renderConn();
    }
  }, 1200);
}

// The host sends the first snapshot in reply to `snapshot`; until then the panel shows the connecting
// state rather than an empty frame that looks broken.
renderConn();
boot();

// ---- TELL THE PANEL PROCESS THE PAGE IS ON SCREEN ------------------------------------------------
//
// ⚠️ THE WINDOW IS CREATED HIDDEN AND IS SHOWN BY THIS MESSAGE (see RunPanel in ui_webview.cpp). It used to be
// shown the moment it was created -- which is BEFORE WebView2 is even asked for -- so the user spent the whole
// browser startup looking at a flat rectangle of background colour. Reported as "设置刚打开时，会有一小段时间
// 面板是空的". Nothing can be drawn in that window until the browser exists, so the fix is not to draw earlier
// but to show LATER, when there is something to see.
//
// ⚠️ THE FIRST FRAME, NOT THE FIRST SCRIPT LINE. `requestAnimationFrame` runs BEFORE the paint it is attached
// to, so one of them only says "about to paint" -- the second one runs after that paint.
//
// ⚠️ AND THE `setTimeout` IS NOT A DUPLICATE, it is the way this cannot deadlock: a window that is not visible
// can have its animation frames throttled away entirely, and a page that never said "ready" would leave the
// panel invisible until the process's own fallback timer fired.
(function () {
  var sent = false;
  function readyOnce() {
    if (sent) return;
    sent = true;
    hostCall("ready");
  }
  setTimeout(readyOnce, 250);
  requestAnimationFrame(function () { requestAnimationFrame(readyOnce); });
})();
