// ---------------------------------------------------------------------------
// RENDER THE SETTINGS PAGE IN A REAL BROWSER AND TAKE A PICTURE OF IT.
//
// ⚠️ WHY THIS EXISTS, AND WHY IT IS NOT A GATE. `_diag/apex_panel_probe.js` drives the page's SCRIPT against a
// DOM stub -- it can say "this card was built, that handler is wired, this message was sent" -- and it cannot
// say a single thing about how the page LOOKS, because a stub has no layout, no box model and no hit testing.
// The project has paid for that gap twice: the switch whose decoration divs ate every click, and the field
// that was drawn 200px wide in a 220px column. Both looked perfect to the probe.
//
// So this is the other half: it feeds a feature's REAL document to the real page, hands the whole thing to
// Edge, and writes a PNG. It is a DIAGNOSTIC, not a gate -- there is nothing here to assert against (a
// screenshot cannot fail), and a human has to look at the picture. Its job is to make "I think it is aligned"
// into something that can be checked, and to stop CSS changes from being guesses.
//
// ⚠️ THE DOCUMENT MUST BE ONE THE FEATURE REALLY SENDS. `feature_keepawake_probe.cpp` can write its controls
// document to a file for exactly this (its optional third argument); feeding a hand-written "looks about
// right" document is how the chart probe once stayed green over a card that could not be drawn at all.
//
// usage: node _diag/panel_preview.js <controls.json> <out.png> [theme=light|dark] [width] [height]
// ---------------------------------------------------------------------------
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const ROOT = path.resolve(__dirname, "..");
// ⚠️ ABSOLUTE PATHS FOR EVERYTHING EDGE IS GIVEN. A relative `--screenshot=` is resolved against Edge's own
// working directory, not the shell's, and the failure is a plain "cannot find the path" that reads like the
// browser refusing to start.
const docPath = path.resolve(process.argv[2] || "");
const outPath = path.resolve(process.argv[3] || path.join(ROOT, "build", "_panel_preview.png"));
const theme = process.argv[4] || "light";
const W = process.argv[5] || "1100";
const H = process.argv[6] || "760";
// ⚠️ THE LANGUAGE IS AN ARGUMENT BECAUSE IT CHANGES THE LAYOUT. A column that is reserved with a fixed pixel
// width is right in one language and wrong in the other (删除 is two characters, Remove is six), so the picture
// that proves an alignment has to be taken in the language where the control is widest.
const lang = process.argv[7] || "zh";
// ⚠️ AND SO ARE THE FEATURE'S NAME AND VERSION, for a different reason: the page prints them in the header and the
// sidebar, and they come from the HOST's snapshot rather than from the controls document. Hard-coding them was how
// the first SmoothWheel render came out headed "保持唤醒 v2.0.0" over SmoothWheel's own controls -- a picture that
// lies about which feature it shows, which is worse than no picture at all.
const featNameZh = process.argv[8] || "保持唤醒";
const featNameEn = process.argv[9] || "Keep Awake";
const featVersion = process.argv[10] || "2.0.0";
// ⚠️ AND ONE FLAG: --press=<text> clicks the first button whose label contains that text, once the page is
// drawn. It is how a FLOW is reproduced rather than just a state -- see the note in the shim.
const pressArg = process.argv.find((a) => a.indexOf("--press=") === 0);
const pressLabel = pressArg ? pressArg.slice("--press=".length) : "";
// ... and --after=<controls.json> is the document to serve from then on. That is what makes the run a FLOW
// rather than a state: the page starts where it really starts, the button is pressed, and the document that
// arrives afterwards is the one the feature would send (e.g. "a new rule, open for editing").
const afterArg = process.argv.find((a) => a.indexOf("--after=") === 0);
const afterDoc = afterArg ? fs.readFileSync(path.resolve(afterArg.slice("--after=".length)), "utf8") : null;

if (!docPath) {
  console.log("usage: node _diag/panel_preview.js <controls.json> <out.png> [theme] [width] [height] [lang] " +
              "[nameZh] [nameEn] [version]");
  process.exit(2);
}
const doc = fs.readFileSync(docPath, "utf8");

// ⚠️ `--general` RENDERS THE GENERAL PAGE INSTEAD OF A FEATURE'S. The panel boots on General, so the shim's
// usual click on a sidebar row is what the feature pictures need -- and it is exactly what hides the page this
// flag exists for (the quick panel's own settings and its reorder list live there).
const general = process.argv.includes("--general");

// The host's snapshot, in the shape settings_host.cpp sends: the host settings, and the feature list with this
// feature in it. The feature is "on" -- a page whose controls are greyed out would hide the very layout this is
// meant to show.
const snapshot = {
  host: { lang: lang, theme: theme, skip: [], systemLight: theme === "light" ? 1 : 0,
          systemLang: "zh-CN", systemIsChinese: lang === "zh" ? 1 : 0, autostart: 0,
          quickCompact: 1, quickOwn: 1 },
  // ⚠️ THE MAP IS SENT THE WAY THE HOST SENDS IT: the features that have something in the flyout, in the order
  // it will draw them -- already resolved, because the page never works that out for itself.
  quickOrder: [{ id: "Preview", nameZh: featNameZh, nameEn: featNameEn },
               { id: "KeepAwake", nameZh: "保持唤醒", nameEn: "Keep Awake" },
               { id: "MediaControl", nameZh: "媒体控制", nameEn: "Media Control" }],
  features: [{ slot: 0, ok: 1, id: "Preview", nameZh: featNameZh, nameEn: featNameEn,
               version: featVersion, off: 0, enabled: 1 }],
};
// The read-out the feature used to publish is gone from the program (ABI 9 -> 10), so the shim answers only the
// two documents the page can still ask for.

// ⚠️ THE SHIM STANDS IN FOR `chrome.webview`, WHICH IS THE ONLY THING A REAL BROWSER DOES NOT HAVE. The page
// posts a message and expects an answer back as a global call; that is the whole of the host protocol from the
// page's side (see settings_ipc.h), so a synchronous-ish answer is a faithful stand-in -- not a mock of the
// host, just of the transport.
const shim = `
(function () {
  var SNAP = ${JSON.stringify(snapshot)};
  var DOC = ${JSON.stringify(doc)};
  var PRESS = ${JSON.stringify(pressLabel)};
  var AFTER = ${JSON.stringify(afterDoc)};
  window.__previewGeneral = ${general ? "true" : "false"};
  window.__previewSent = [];
  window.__previewErrors = [];
  function answer(fn, text) { setTimeout(function () { if (window[fn]) window[fn](text); }, 0); }
  window.chrome = { webview: { postMessage: function (m) {
    window.__previewSent.push(m);
    if (m && m.cmd === "pageError") window.__previewErrors.push("page: " + (m.text || m.message || m.msg || ""));
    if (m.cmd === "snapshot") answer("__apexSnapshot", JSON.stringify(SNAP));
    else if (m.cmd === "describe") answer("__apexControls", (AFTER && window.__previewPressed) ? AFTER : DOC);
    // setControl / listOp / setHost: no answer, exactly as the host behaves (a control write is not a refresh).
  } } };
  // ⚠️ THE PAGE'S OWN ERRORS ARE COLLECTED, BECAUSE AN EXCEPTION THROWN MID-RENDER DOES NOT LOOK LIKE ONE: the
  // page is left half drawn, and what the user sees is a region that never appeared -- reported to me as
  // "新建时，参数面板是黑的" (in the dark theme the page background is #141414, so a pane that was never painted IS
  // a black rectangle). A DOM stub cannot see this at all: its elements have no paint, and the page reports its
  // own errors through the same message channel this shim stands in for.
  window.addEventListener("error", function (e) {
    window.__previewErrors.push("js: " + e.message + " @" + e.lineno + ":" + e.colno);
  });
  // OPEN THE FEATURE'S PAGE, because the panel boots on the general page and the page under test is the
  // feature's. One click on its sidebar row is what a user does; doing it here is what makes the picture the
  // picture of THAT page. (The --general flag skips it: that IS the page under test then.)
  setTimeout(function () {
    if (window.__previewGeneral) return;
    var it = document.querySelector("#nav .item");
    if (it) it.click();
  }, 200);
  // ⚠️ OPTIONAL: PRESS A BUTTON AND SEE WHAT THE PAGE DOES WITH IT. Rendering a document says nothing about the
  // FLOW that produces it, and the flow is where the page's own state is exercised -- which row is selected,
  // which edit is open, what it does when an item it did not expect arrives. The busiest such flow here is
  // AutoIME's 新建: the feature creates the rule AND opens it for editing, and the page has to follow.
  setTimeout(function () {
    if (!PRESS) return;
    var all = document.querySelectorAll("button"), hit = null;
    for (var i = 0; i < all.length; i++)
      if ((all[i].textContent || "").indexOf(PRESS) >= 0) { hit = all[i]; break; }
    if (!hit) { window.__previewPress = "no button matching " + PRESS; return; }
    hit.click();
    window.__previewPressed = true;
    window.__previewPress = "clicked " + (hit.textContent || "");
  }, 700);
  // ⚠️ AND THE MEASUREMENTS GO INTO THE TITLE, because a picture tells you THAT two things are out of line and
  // never by how much. --dump-dom prints the document, the title is in it, and so a run of this harness can be
  // read by a script instead of by an eye. (The selectors are the ones a page's own layout is made of.)
  // (No backticks in this template literal: one inside it ends the string and the file stops being JavaScript.)
  setTimeout(function () {
    var what = [
      ["card", ".card"], ["bar", ".grpbar"], ["addbox", ".grpbar .addrow"],
      ["input", ".grpbar input[type=text]"], ["addbtn", ".grpbar button.addbtn"],
      ["head", ".listhead"], ["rowlist", ".rowlist"], ["row0", ".rowlist .skiprow"],
      ["name0", ".rowlist .skiprow .nm"], ["name1", ".rowlist .skiprow:nth-child(2) .nm"],
      ["sw0a", ".rowlist .skiprow .swcell"],
      ["sw1a", ".rowlist .skiprow:nth-child(2) .swcell"],
      ["ghost", ".rowlist .skiprow .ghost"], ["btn1", ".rowlist .skiprow:nth-child(2) button"],
      // THE GROUP'S OWN QUICK-PANEL SWITCH (apex/abi.h, the "quick" key): where it lands is the whole of "居右",
      // so it is measured against the card and the bar rather than eyeballed (see docs/rules/panel.md).
      ["qs", ".quicksw"], ["qsin", ".listhead .quicksw"], ["barqs", ".grpbar .quicksw"],
    ];
    var out = [];
    what.forEach(function (w) {
      var el = document.querySelector(w[1]);
      if (!el) { out.push(w[0] + "=-"); return; }
      var r = el.getBoundingClientRect();
      out.push(w[0] + "=" + [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)].join(","));
    });
    document.title = "GEOM " + out.join(" ") +
      " theme=" + (document.documentElement.getAttribute("data-theme") || "none") +
      " bodybg=" + getComputedStyle(document.body).backgroundColor +
      " press=" + (window.__previewPress || "-") +
      " errors=" + window.__previewErrors.join(" | ");
  }, 2500);
})();
`;

const html = fs.readFileSync(path.join(ROOT, "build", "panel.built.html"), "utf8");
const at = html.indexOf("<script>");
if (at < 0) {
  console.log("panel.html has no <script> -- the shim cannot be injected");
  process.exit(1);
}
const out = html.slice(0, at) + "<script>" + shim + "</script>\n" + html.slice(at);
const preview = path.join(ROOT, "build", "_panel_preview.html");
fs.writeFileSync(preview, out, "utf8");

const edges = [
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
];
const edge = edges.find((p) => fs.existsSync(p));
if (!edge) {
  console.log("no Edge found -- cannot render the page");
  process.exit(1);
}

// ⚠️ A SEPARATE USER-DATA-DIRECTORY, so this never touches the profile the user's own browser is using (and so
// two previews cannot collide). `--virtual-time-budget` is what lets the page's timers (the readout poll, the
// click above) run before the picture is taken -- without it the shot is of a half-booted page.
const profile = path.join(ROOT, "build", "_preview_profile");
const args = [
  "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
  "--user-data-dir=" + profile,
  "--screenshot=" + outPath,
  "--window-size=" + W + "," + H,
  "--virtual-time-budget=4000",
  "--hide-scrollbars",
  "file:///" + preview.replace(/\\/g, "/"),
];
// stdio: 'inherit' -- this harness may refuse to capture a child's piped output, and the screenshot file IS the
// output anyway. For `--geom` the DOM has to be captured, so that run is the exception (see below).
const geom = process.argv.includes("--geom");
if (geom) args.splice(args.indexOf("--screenshot=" + outPath), 1, "--dump-dom");
const r = spawnSync(edge, args, { encoding: "utf8", stdio: geom ? ["ignore", "pipe", "ignore"] : "inherit" });
if (geom) {
  const m = /GEOM ([^<]*)</.exec(r.stdout || "");
  console.log(m ? m[1] : "no measurements (the page did not reach the harness's timer)");
  process.exit(m ? 0 : 1);
}
if (!fs.existsSync(outPath)) {
  console.log("Edge did not write " + outPath + " (exit " + r.status + ")");
  process.exit(1);
}
console.log("rendered " + docPath + " as " + theme + " -> " + outPath);
