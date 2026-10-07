# Apex

> 端，体之无序而最前者也。 — 《墨经》
>
> **Apex. The first point.**

*[中文](README.md)*

**端** (duān) — the foremost point of a body, where it can no longer be divided. The smallest unit, and the
starting point.

---

## What this is

A Windows tool set in the shape of PowerToys: each feature does one thing.

| Feature | What it does |
|---|---|
| **Silky Scroll**<br>丝滑滚动 | Takes over the mouse wheel system-wide — scrolling anywhere gains acceleration and a smooth landing |
| **Auto IME**<br>自动输入法 | Switches the input method by the program or control that has focus; rules are reordered by dragging and recorded by pressing a key |
| **Keep Awake**<br>保持唤醒 | A list of programs, two switches per row — "keep awake" and "keep the screen on"; plus one unremovable "system-wide" row |
| **Media Control**<br>媒体控制 | One brightness slider and one screen-off button per monitor; one volume slider and one mute button per application |

Every feature **minds its own business**: its own switch, its own settings file, its own *Exclude* list.

## The two projects that bring their own engine

**This smoothing engine has been embedded into a few applications individually**, and Apex steps aside for those
— **whether or not that engine is switched on**:

| Project | What it is |
|---|---|
| **[SmoothWheelScroll](https://github.com/bobo198504/SmoothWheelScroll)** | The smooth-scrolling plugin for REAPER |
| **[Lertaro](https://github.com/bobo198504/Lertaro)** | A standalone Windows tool (**the fork**) that ports the same smoothing model |

## Using it

1. Run `apex.exe`; an icon appears in the tray
2. **Click** the tray icon → the **quick panel** (it opens above the icon, holding each feature's switches, sliders and buttons)
3. **Double-click** it, or right-click → **Settings**, to open the settings panel
4. To keep one program out of **one feature's** way, add its name to that **feature page's "Exclude"** (`*` and `?` work)

## Portable

Copy the whole folder; the settings come with it. No registry, no `%APPDATA%`.

```
apex.exe              the host
apex.ini              its settings
apex-settings.exe     the settings panel
Plugins\<feature>\    each feature, with its own settings
```

"Start with Windows" is off by default (it is the only thing written outside this folder). Opening the settings
panel creates a `WebView2\` cache directory — this machine's own, don't pass it on.

## Building

```bash
bash apex/build.sh                # build; the output lands in build/apex/
bash test/run_all.sh              # build-layer gates: pure logic and artefacts, no processes, no input devices
bash test/run_all.sh --assembly   # adds the assembly layer, which starts real programs; run before a release
bash apex/package.sh              # the portable package: build/Apex-<version>-portable.zip
bash apex/deploy.sh               # copy it over the installation on this machine
```

You need **MinGW-w64** (`g++` / `windres`); the project is developed with
[w64devkit](https://github.com/skeeto/w64devkit), and the scripts name a default toolchain path near the top. No
REAPER SDK, no CMake; the WebView2 SDK is included (`third_party/`).

## Development

Structure and rules are in **[AGENTS.md](AGENTS.md)** (per-topic chapters in `docs/rules/`); what changed in each
release is in **[CHANGELOG.md](CHANGELOG.md)**.

- **A feature is one folder** (`features\<id>\`) implementing the C interface in `apex/abi.h`; the UI is not written per feature
- **Settings and the hook are two processes**: a stuck panel cannot stall scrolling
- **The model in `shared/`** is a copy of the SmoothWheelScroll plugin's `src/`; the two are kept in step by hand (`test/check_model_sync.sh`)

## Licence

**GPL-3.0** — the full text is in [LICENSE](LICENSE).
