# Apex

> 端，体之无序而最前者也。 — 《墨经》
>
> **Apex. The first point.**

*[中文](README.md)*

**端** (duān) — the foremost point of a body, where it can no longer be divided. The smallest unit, and the
starting point.

---

## What this is

A Windows tool set in the shape of PowerToys: **each feature does one thing, and does it properly.**

There are four of them today:

| Feature | What it does |
|---|---|
| **Silky Scroll**<br>丝滑滚动 | Takes over the mouse wheel system-wide — scrolling anywhere gains acceleration and a smooth landing |
| **Auto IME**<br>自动输入法 | Switches the input method by the program or control that has focus; rules are reordered by dragging, and recorded by pressing a key |
| **Keep Awake**<br>保持唤醒 | A list of programs, two switches per row — "keep awake" and "keep the screen on"; plus one unremovable "system-wide" row |
| **Media Control**<br>媒体控制 | One brightness slider and one screen-off button per monitor; one volume slider and one mute button per application |

Every feature **minds its own business**: its own switch, its own settings file, its own *Exclude* list.

## The two projects that bring their own engine

**Silky Scroll** takes the wheel over *globally*: it turns wheel messages into smooth motion everywhere.
But some programs **already ship a smoothing engine of their own**, and a second one on top of it only fights
it — so Apex steps aside:

| Project | What it is | What Apex does |
|---|---|---|
| **[SmoothWheelScroll](https://github.com/bobo198504/SmoothWheelScroll)** | The smooth-scrolling plugin for REAPER, and where this project's smoothing model comes from (`shared/` is a copy of its `src/`) | Never takes REAPER's wheel, **whether or not the plugin is loaded** |
| **[Lertaro](https://github.com/Lertaro/Lertaro)** | A standalone Windows tool that ports the same model and publishes a named event while its smoothing is on | Never takes Lertaro's wheel, **whether or not its smoothing is switched on** |

The test is **the program itself**, not "is its engine on right now" — otherwise the feel would flip between two
kinds of smoothing the moment you enabled the plugin.

On the **Silky Scroll** page, a grey line beside *Exclude* names the engines that are actually running
(`REAPER、Lertaro专用引擎已运行`, earliest-started first). That line only **tells you**; stepping aside does not
depend on it, and never did.

When another program turns up with its own engine, adding it is a single place (`common/engines.h`, one table) —
and both the note and the step-aside follow from it.

## Using it

1. Run `apex.exe`; an icon appears in the tray
2. **Click** the tray icon once → the **quick panel** opens above it
3. **Double-click** it, or right-click → **Settings**, to open the settings panel
4. The wheel is smooth in every program
5. To keep one program out of **one feature's** way, add its name to that **feature page's "Exclude"**
   (`game.exe`; `*` and `?` work)

To switch a feature off, open its page and turn its own switch off.
There is no master switch — **each feature owns its own**.

> ⚠️ **On a fresh install every feature starts switched off**; turn on what you want. An existing settings file
> is left exactly as it is.

## The quick panel

The small panel that appears when you **click** the tray icon. It is not the settings window: it is a
self-drawn window inside the host process, and it opens right above the tray icon — because a click on the tray
promises something *now*, and the settings panel is a browser process that needs half a second to start.

- Each feature puts its own controls here: switches, sliders, buttons
- Which ones is **the feature's** decision; some stay out of the panel until you turn them on in its page
- A name too long for its row scrolls back and forth while the pointer rests on it

## Portable

Copy the whole folder; the settings come with it. No registry, no `%APPDATA%`.

```
apex.exe              the host
apex.ini              its settings
apex-settings.exe     the settings panel
Plugins\<feature>\    each feature, with its own settings
```

One exception: **"Start with Windows"** (off by default) writes a startup entry — the only thing in the whole
program that is written outside its own folder, which is why it is off.

Opening the settings panel for the first time also creates a `WebView2\` folder: that is the browser's own cache
directory. It belongs to this machine — don't pass it on.

## Settings

Bilingual, follows the system's light/dark setting, and **a change takes effect immediately** (no restart).

**General** sits at the top of the left-hand list (the 端 / Apex entry); the features follow.

- **General**: language (follow the system / 中文 / English), appearance (follow the system / light / dark),
  start with Windows, and the two halves of the quick panel
- **A feature's page**: one switch, and that is its own enable; a slider **returns to its default on a
  double-click**, and the wheel changes its value while the pointer rests on it
- **The "Exclude" list** lives on a feature's page: a program listed there is left completely alone by *that*
  feature. ⚠️ It belongs to the feature — whether another feature has such a list, and what it is called, is up
  to that feature

What changed, and in which release, is in **[CHANGELOG.md](CHANGELOG.md)**.

## Building

```bash
bash apex/build.sh                # build; the output lands in build/apex/
bash test/run_all.sh              # build-layer gates: pure logic and artefacts, no processes, no input devices
bash test/run_all.sh --assembly   # adds the assembly layer, which starts real programs; run before a release
bash apex/package.sh              # the portable package: build/Apex-<version>-portable.zip
bash apex/deploy.sh               # copy it over the installation on this machine
```

You need **MinGW-w64** (`g++` / `windres`). The project is developed with
[w64devkit](https://github.com/skeeto/w64devkit); the scripts name a default toolchain path near the top —
change it, or put your toolchain on `PATH`. No REAPER SDK, no CMake; the WebView2 SDK is included
(`third_party/`).

`apex/package.sh` puts only the program files into the package (exe/dll) and then reads the archive back to check
itself — `build/apex/` is also a folder the author has *run* the program in, and packing it as-is would ship his
settings and logs.

## Development

The structure and the rules are in **[AGENTS.md](AGENTS.md)** (the core, read daily; the per-topic chapters are in
`docs/rules/`). In short:

- **A feature is one folder** (`features\<id>\`) implementing the C interface in `apex/abi.h`
- **No UI is written per feature**: a feature describes its own controls, the panel draws them
- **Settings and the hook are two processes**: a stuck panel cannot stall scrolling
- **The model in `shared/`** is a copy of the SmoothWheelScroll plugin's `src/`; the two are kept in step by hand
  (`test/check_model_sync.sh` compares them)

## Licence

**GPL-3.0** — the full text is in [LICENSE](LICENSE).
