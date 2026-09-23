
const fs = require("fs");
// ⚠️ argv[2] IS THE PAGE, NOT argv[1]. When node runs a SCRIPT FILE, argv[1] is the script's own path and the
// first argument is argv[2] -- whereas inside `node -e '...'` the first argument really is argv[1]. This probe
// was inline in its gate first, and moving it into a file changed that without changing the index: it then
// read its own source, found no `--bg` in it, and reported every palette as UNREADABLE rather than as "I am
// looking at the wrong file". (Same shape as a DOM stub that is missing a method the page calls: the tool
// failing in a way that reads as the product failing.)
const pagePath = process.argv[2];
if (!pagePath) {
  console.log("usage: node apex_palette_probe.js <panel.html>");
  process.exit(2);
}
const src = fs.readFileSync(pagePath, "utf8");
let fail = 0;
const say = (ok, what, extra) => {
  console.log("  " + (ok ? "ok  " : "FAIL") + " " + what + (extra ? "   " + extra : ""));
  if (!ok) fail++;
};

// ---- read the three blocks ---------------------------------------------------
// Anchored on the selectors the file actually uses, then that block only. A CSS parser would be a second
// implementation of CSS; finding three known blocks is not.
function block(header, until) {
  const i = src.indexOf(header);
  if (i < 0) return null;
  const j = until ? src.indexOf(until, i) : src.indexOf("}", i);
  if (j < 0) return null;
  return src.slice(i, j);
}
function vars(text) {
  const out = {};
  if (!text) return out;
  const re = /--([a-z-]+)\s*:\s*(#[0-9a-fA-F]{3,8})/g;
  let m;
  while ((m = re.exec(text))) out[m[1]] = m[2].toLowerCase();
  return out;
}

const rootAll   = block(":root {", "@media");
const pinLight  = block(":root[data-theme=\"light\"] {");
const pinDark   = block(":root[data-theme=\"dark\"] {");
// The media block holds the dark palette; take it as the content between the @media line and the next rule.
const mediaStart = src.indexOf("@media (prefers-color-scheme: dark)");
const mediaDark  = mediaStart < 0 ? null : vars(src.slice(mediaStart, src.indexOf(":root[data-theme", mediaStart)));

const L  = vars(rootAll);      // the light theme, and the auto fallback
const LH = vars(pinLight);     // pinned light
const D  = mediaDark || {};    // dark, following the system
const DH = vars(pinDark);      // pinned dark

for (const [n, v] of [["light (:root)", L], ["light (pinned)", LH], ["dark (media)", D], ["dark (pinned)", DH]]) {
  if (!v.bg || !v.panel || !v.fg || !v.sub) { console.log("FAIL: could not read the " + n + " palette"); fail++; }
}
if (fail) { console.log("\nFAILED: the palettes could not be read"); process.exit(1); }

// ---- colour maths -------------------------------------------------------------
const rgb = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
const lin = (c) => { c /= 255; return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
const lum = (h) => { const [r, g, b] = rgb(h); return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b); };
const ratio = (a, b) => { const x = lum(a), y = lum(b); const hi = Math.max(x, y), lo = Math.min(x, y);
                          return (hi + 0.05) / (lo + 0.05); };
const sat = (h) => { // HSL saturation, in %
  const [r0, g0, b0] = rgb(h).map((v) => v / 255);
  const mx = Math.max(r0, g0, b0), mn = Math.min(r0, g0, b0), l = (mx + mn) / 2, d = mx - mn;
  if (d === 0) return 0;
  return Math.round(100 * (l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn)));
};
const lightL = (h) => { const [r0, g0, b0] = rgb(h).map((v) => v / 255);
                        return Math.round(100 * ((Math.max(r0, g0, b0) + Math.min(r0, g0, b0)) / 2)); };

// ---- 1. the two light paths agree ---------------------------------------------
console.log("1. one palette per appearance (the bug that made it \"too yellow\")");
{
  const keys = ["bg", "panel", "fg", "sub", "line", "track", "accent", "accent-fg"];
  let same = true, diff = [];
  for (const k of keys) if (L[k] !== LH[k]) { same = false; diff.push(k + ": " + L[k] + " vs " + LH[k]); }
  say(same, "the auto light path and the pinned light path declare the same values",
      same ? "" : diff.join(", "));
  let sameD = true, diffD = [];
  for (const k of keys) if (D[k] !== DH[k]) { sameD = false; diffD.push(k + ": " + D[k] + " vs " + DH[k]); }
  say(sameD, "and the same is true of dark", sameD ? "" : diffD.join(", "));
}

// ---- 2. the surfaces are ordered ----------------------------------------------
console.log("\n2. the surfaces are ordered the way each theme is read");
say(lightL(L.panel) < lightL(L.bg),
    "light: the card is DARKER than the page (a recessed area)",
    "panel L" + lightL(L.panel) + "% < bg L" + lightL(L.bg) + "%");
say(lightL(D.panel) > lightL(D.bg),
    "dark: the card is LIGHTER than the page (that is what makes it visible)",
    "panel L" + lightL(D.panel) + "% > bg L" + lightL(D.bg) + "%");
say(lightL(L.line) < lightL(L.panel), "light: the hairline is darker than the card it divides",
    "line L" + lightL(L.line) + "% < panel L" + lightL(L.panel) + "%");
say(lightL(L.track) < lightL(L.panel), "light: a slider's track is visible inside a card",
    "track L" + lightL(L.track) + "% < panel L" + lightL(L.panel) + "%");

// ---- 3. the text still reads ---------------------------------------------------
// 4.5:1 is WCAG AA for small text, and the panel's body text is 14px.
console.log("\n3. contrast (WCAG AA for small text is 4.5:1)");
const pairs = [["fg on bg", L.fg, L.bg], ["fg on panel", L.fg, L.panel],
               ["sub on bg", L.sub, L.bg], ["sub on panel", L.sub, L.panel],
               ["warn on bg", L.warn, L.bg],
               ["dark: fg on bg", D.fg, D.bg], ["dark: sub on panel", D.sub, D.panel]];
let worst = 99;
for (const [name, a, b] of pairs) {
  if (!a || !b) { say(false, "missing colour for " + name); continue; }
  const r = ratio(a, b);
  if (r < worst) worst = r;
  say(r >= 4.5, name, r.toFixed(2) + ":1");
}
say(ratio(L["accent-fg"] || L.bg, L.accent) >= 4.5, "the selected row's own text",
    ratio(L["accent-fg"] || L.bg, L.accent).toFixed(2) + ":1");

// ---- 4. it is actually less saturated ------------------------------------------
console.log("\n4. the light surfaces are paler than the ones the user rejected");
// THE VALUES THAT WERE REJECTED, from the commit that introduced the change. They are written down HERE
// rather than read from git so the gate has no dependency on history -- and so that "how much paler" is a
// number in the test rather than a memory.
const OLD = { bg: "#fdf8e8", panel: "#f4eac6", line: "#e3d9b8", track: "#e4d9b2" };
for (const k of ["bg", "panel", "line", "track"]) {
  say(sat(L[k]) < sat(OLD[k]), "light --" + k + " is less saturated",
      sat(OLD[k]) + "% -> " + sat(L[k]) + "%");
}
say(sat(L.bg) <= 25, "the page background is close to neutral (it was the yellow one)",
    "saturation " + sat(L.bg) + "%");

// ---- 5. the two hold inks are legible on BOTH plates ------------------------------------------
//
// ⚠️ A COLOUR WRITTEN IN TWO PLACES IS A COLOUR THAT DRIFTS, and these are in C++ because the HOST draws them:
// the tray mark's MIDDLE BAR takes one of two inks while a feature holds something the user cannot see
// (APEX_FEATURE_USER_VISIBLE / APEX_FEATURE_HOLD_HARD in abi.h; APEX_HOLD_INK / APEX_HOLD_HARD_INK in icons.h).
//
// ⚠️ AND EACH IS ONE COLOUR FOR BOTH APPEARANCES, which is an exception to the rule the artwork itself follows
// (its bar is near-black on the cream plate and cream on the near-black one). The exception is only safe if the
// colour really can be seen on both, so this MEASURES it: WCAG's non-text minimum is 3:1, and the two plates are
// the colours the .ico files carry (written down in icons.h for exactly this). An ink that failed here would be
// a mark that vanishes in one of the two themes -- and "the mark never changed" is the report that started all
// of this.
//
// ⚠️ AND THE GREEN IS ASSERTED TO BE THE PANEL'S OWN `--dot-on`, not merely a colour that happens to be close to
// it. It IS that green (see icons.h), and the tie is what makes "the tray's working green and the list's
// enabled green are one green" a fact rather than a comment: the alternative is two hex values that agree today.
console.log("\n5. the two hold inks are legible on both plates");
{
  // ⚠️⚠️ icons.h IS A SECOND FILE THIS PROBE MUST AGREE WITH, AND THE CALLER NAMES IT (argv[3]).
  //
  // It used to be DERIVED from the page's own path -- `.../ui/panel.html`.replace(...) -> `.../icons.h` -- and that
  // broke the moment the page moved into `build/panel.built.html` (the split into shell + css + js): the replace
  // matched nothing, so this probe read THE PAGE ITSELF as if it were icons.h, found no inks, and failed seven
  // checks with "not found" -- while the page it was reading was perfectly correct. A path guessed from another
  // path is a second place that has to be kept in step; naming it is one place that cannot drift.
  const iconsPath = process.argv[3] || path.join(path.dirname(pagePath), "..", "apex", "icons.h");
  let icons = "";
  try {
    icons = fs.readFileSync(iconsPath, "utf8");
  } catch (e) {
    say(false, "apex/icons.h can be read (the hold inks live there)", iconsPath);
  }
  const col = (name) => {
    const m = icons.match(new RegExp(name + "\\s+(0x[0-9a-fA-F]{8})"));
    if (!m) return null;
    const v = parseInt(m[1], 16);
    // COLORREF is 0x00BBGGRR: the RED is the LOW byte, which is why the components come out in this order.
    return "#" + [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff]
      .map((b) => b.toString(16).padStart(2, "0")).join("");
  };
  const inks = [["the ordinary hold (green)", col("APEX_HOLD_INK")],
                ["the strong hold (red)", col("APEX_HOLD_HARD_INK")]];
  const plates = [["the cream plate (light appearance)", col("APEX_PLATE_LIGHT")],
                  ["the near-black plate (dark appearance)", col("APEX_PLATE_DARK")]];
  for (const [what, ink] of inks) {
    say(!!ink, what + " is declared in icons.h", ink || "not found");
  }
  for (const [what, ink] of inks) {
    for (const [plateWhat, plate] of plates) {
      if (!ink || !plate) {
        say(false, "  and " + plateWhat + " is declared too", "");
        continue;
      }
      const r = ratio(ink, plate);
      say(r >= 3, "  " + what + " reads on " + plateWhat, r.toFixed(2) + ":1 (the non-text minimum is 3:1)");
    }
  }
  // The tray's ordinary hold and the feature list's "on" dot are ONE colour, so they are asserted to be one
  // value rather than two values that happen to match (see the note above).
  const dotOn = L["dot-on"];
  const green = inks[0][1];
  say(!!dotOn && !!green && String(dotOn).trim().toLowerCase() === String(green).trim().toLowerCase(),
      "the tray's ordinary hold IS the panel's --dot-on green (one colour, not two)",
      (dotOn || "?") + " vs " + (green || "?"));
}

console.log();
if (fail) { console.log("FAILED: " + fail + " check(s)"); process.exit(1); }
console.log("OK: one light palette on both paths, surfaces ordered, text readable, and paler than before");

