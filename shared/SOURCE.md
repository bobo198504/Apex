# The model headers in this folder are a COPY

## Where they came from

Copied on **2026-09-18** from the REAPER plugin project:

```
D:\Projects\Code\SmoothWheelScroll for reaper\src\
```

At that moment the plugin was at version **1.7.1** (`ext_name` = `Smooth Wheel Scroll 1.7.1`).

## Why they are a copy and not a shared file

Until the split, this project lived inside the plugin's repository as `app/`, so "the same header" meant the
same bytes. Two separate projects cannot do that, and the alternatives were both worse:

* **a git submodule or a shared directory outside both trees** -- adds a third thing to clone, and the
  toolchain here is deliberately "copy the folder and it builds" (no REAPER SDK, no package manager);
* **re-implementing the model here** -- the one thing that is definitely wrong: the sibling WPF project did
  exactly that, stopped at 1.1.0, and the two no longer feel alike.

So: a copy, with the fingerprints written down. The user's own reasoning for accepting the cost is that the
model is finished ("模型已经近乎完美，不太需要更新，有的就是接递层的事").

## The fingerprints, so drift is checkable

| file | bytes | md5 |
|---|---|---|
| `model.h` | 178 lines | `ff6fecfe0105c0f338ada982412866c5` |
| `anim3_core.h` | 226 lines | `8d2231cef1e3d70860ea30290198d40e` |
| `anim161_core.h` | 1123 lines | `d785d9371295448461a033bb1efd179e` |

`device.h` was **not** copied: it is the plugin's own device/touchpad-tracking header and the feature here
does not use it (the Apex host gets modifier state from the OS hook instead).

To compare against the plugin at any time:

```bash
cd "D:/Projects/Code/SmoothWheelScroll for reaper"
md5sum src/model.h src/anim3_core.h src/anim161_core.h
md5sum "D:/Projects/Code/Apex/shared/model.h" \
       "D:/Projects/Code/Apex/shared/anim3_core.h" \
       "D:/Projects/Code/Apex/shared/anim161_core.h"
```

Identical output means the two projects still share one model. **They must be kept identical.**

## What to do if the model changes on either side

1. Make the change in the PLUGIN first (`src/`), because that is the side with the gates: run the plugin's
   nine gates (`test/check_anim3.sh`, `check_conservation.sh`, `check_travel.sh`, and the rest).
2. Copy the changed header(s) into this folder, overwriting.
3. Update the table above (bytes, md5) -- **a fingerprint that is not updated is worse than none**, because it
   says "checked" when nobody did.
4. Run this project's gates, which include the model checks: `test/check_app_core.sh` asserts conservation
   (what goes in comes out, exactly) and the travel/attenuation rules.
5. Run `test/check_apex_endtoend.sh` -- it drives a real wheel through the real host and counts what arrives,
   which is the only check that would notice a model change that broke the assembly rather than the maths.

## What is NOT here on purpose

`common/` (the settings and the core) is Apex's own code, not shared: it was never part of the plugin. The
split moved it out of `app/` unchanged. Only `shared/` is a copy of another project's files.
