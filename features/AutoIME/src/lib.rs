//! AUTOI ME AS AN APEX FEATURE.
//!
//! ⚠️ WHY THIS IS A RUST cdylib AND NOT C++, IN ONE PARAGRAPH. The upstream project (D:\Projects\Code\Auto IME)
//! is Rust, ~3100 lines, and it is not a pile of API calls: its FAILED_APPROACHES.md records FOUR approaches
//! that were tried and found BROKEN -- cross-process `ImmGetContext` reads, `ImmSetOpenStatus` for switching,
//! per-process state tables, and `GetAsyncKeyState` polling for manual changes -- with the correct method
//! written beside each. Those corrections are load-bearing and already in this code. A C++ rewrite would be a
//! rewrite of the CORRECTIONS, with every one of them a chance to reintroduce a bug that took a debugging
//! session to find. Apex's host is C++ and this is Rust, and the boundary between them is one C struct.
//!
//! WHAT IS PORTED: the rule model and its matching engine, the IME state read/write, the focus/click capture,
//! the UI Automation control identification, and the per-thread state machine.
//! WHAT IS NOT: the settings window (egui), the tray icon, the about box, window-position persistence and the
//! single-instance mutex -- Apex owns all of those generically, and dropping eframe takes the binary from a
//! 24 MB exe to a DLL measured in hundreds of KB.

// ⚠️⚠️ DEAD CODE IS AN ERROR HERE, NOT A WARNING, AND THIS CRATE IS WHY. Almost everything in it is PORTED,
// and the port came from a standalone program that owned things Apex now owns generically: a window class, a
// message loop, a tray, a hotkey, a single-instance mutex, window-position repair. The ported code was kept
// `pub` "in case" -- so the crate carried a self-check nobody ran, a window-position helper nobody called, a
// mutex nobody created, and four enums' worth of labels for a settings window that does not exist. Measured
// before this was added: 93 warnings, and the dead-code ones alone named about fifty items.
//
// A warning is not enough for this, because a warning is invisible in a build that succeeds. What the compiler
// can prove about reachability is the ONE thing here that no comment can: a comment saying "this is used by X"
// stops being true the moment X is deleted, and nothing says so.
//
// ⚠️ THE FIX FOR A NEW WARNING IS DELETE OR GATE, NEVER `#[allow(dead_code)]`: an item that only the tests use
// goes behind `#[cfg(test)]` (which says so), and an item nothing uses goes. The single exception is an ABI
// constant that exists to mirror `apex/abi.h` -- see abi.rs.
#![deny(dead_code)]

// ⚠️ ONE WARNING IS EXPECTED HERE AND IS DELIBERATELY LEFT ALONE: `crate AutoIME should have a snake case
// name`. That name is the DLL's file name (`AutoIME.dll`, in `Plugins/AutoIME/`), which the host's loader and
// the folder layout require -- renaming the crate would rename the artifact. Rust has no way to allow that lint
// for the crate NAME alone, and allowing `non_snake_case` crate-wide to silence one lint would switch it off in
// `model.rs` and `settings.rs` too, where the names are ours and the rule should hold.
// (The Windows-sized names in `win32.rs` and `abi.rs` are allowed where THEY are, with their own reason.)

mod abi;
mod model;
mod settings;
mod win32;
#[cfg(test)]
mod tests;
#[cfg(test)]
mod settings_tests;

use abi::*;
use std::os::raw::{c_char, c_double, c_int, c_uint};
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};

/// The host, for the calls that need it. Set by `init`, cleared by `shutdown`.
///
/// ⚠️ IT IS AN ATOMIC POINTER, NOT A `static mut`, AND THAT IS NOT PEDANTRY: the monitor thread reads it while
/// the host's own thread writes it on init/shutdown, and the compiler is entitled to assume a plain `static
/// mut` does not change under it.
static HOST: AtomicPtr<ApexHost> = AtomicPtr::new(std::ptr::null_mut());

pub(crate) fn host() -> Option<&'static ApexHost> {
    let p = HOST.load(Ordering::Acquire);
    if p.is_null() {
        None
    } else {
        Some(unsafe { &*p })
    }
}

/// ASKED BY THE MONITOR LOOP, EVERY 20 ms: is the user still keeping this feature on?
///
/// ⚠️ IT GOES THROUGH THE HOST EVERY TIME, ON PURPOSE. The host's switch is the only authority -- caching the
/// answer here would be a second copy of a value the user can change at any moment, and the copy would be the
/// one that is wrong. It is a list scan on the host's side (see HostFeatureEnabled), which is why asking 50
/// times a second is fine and asking per frame would not be.
///
/// ⚠️ IF THERE IS NO HOST, THE ANSWER IS NO. Reaching this without a host means init has not run or shutdown
/// has -- either way this feature has no business doing anything, and "no" is the quiet direction.
pub(crate) fn feature_is_enabled() -> bool {
    match host() {
        Some(h) => match h.featureEnabled {
            Some(f) => {
                let id = c"AutoIME";
                unsafe { f(id.as_ptr()) != 0 }
            }
            None => false, // an older host with no such call: treat as off rather than guess
        },
        None => false,
    }
}

/// A flag rather than a kill: the loop polls it and returns on its own, because it may be in the middle of a
/// `SendMessage` into another process's IME window and tearing a thread out of that is how a program deadlocks
/// another program.
pub(crate) static SHUTDOWN: AtomicBool = AtomicBool::new(false);

pub(crate) fn shutdown_flag() -> &'static AtomicBool {
    &SHUTDOWN
}

/// Set by the monitor thread as its very last act, so `shutdown` can wait for it instead of guessing with a
/// fixed sleep. Without it, unloading the DLL while the thread was mid-`SendMessage` would leave that call
/// returning into freed code -- the classic crash-on-exit that only happens sometimes.
pub(crate) static LEFT_THE_LOOP: AtomicBool = AtomicBool::new(false);

/// THE NAME TO IGNORE WHEN LOOKING AT FOCUSED WINDOWS: Apex itself.
///
/// ⚠️ NOT "our own exe". Upstream is a standalone program and filters its own process so that focusing its
/// settings window does not switch the IME. This code now lives INSIDE apex.exe, so "our own exe" would name
/// the host and would also refuse to act in any other program called apex.exe. What must be ignored is Apex:
/// its own windows are the last place this feature should be flipping the input method.
pub(crate) fn own_exe_name() -> String {
    // GetCurrentProcessId, not "the module this DLL was loaded from": the source of a focus event is a WINDOW
    // in ANOTHER process, so the comparison is against the process the whole feature is running inside.
    win32::own_process_name()
}

/// One line in the host's log (never called before `init`).
pub(crate) fn log(msg: &str) {
    if let Some(h) = host() {
        if let Some(f) = h.logLine {
            if let Ok(c) = std::ffi::CString::new(msg) {
                unsafe { f(c.as_ptr()) };
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The entry points the host calls. Every one of them is a thin shell over a function that knows nothing about
// the ABI -- the same split the C++ features use, so the ported logic stays testable on its own.
// ---------------------------------------------------------------------------
unsafe extern "C" fn fe_init(host: *const ApexHost) -> c_int {
    if host.is_null() {
        return 1; // refuse: no host, nothing works
    }
    let h = &*host;

    // ⚠️ THE HOST'S ABI VERSION IS CHECKED HERE EVEN THOUGH THE HOST ALSO CHECKS OURS. The loader refuses a
    // feature built against a different ABI (it compares this struct's fields before reading them), but the
    // check only works in one direction from the host's side: this side needs to know that `featureEnabled`
    // and the rest of the struct past the fields it was compiled against actually exist. A mismatch here would
    // be a call through a null function pointer, which is a crash rather than a message.
    if h.abiVersion != APEX_ABI_VERSION || (h.structSize as usize) < std::mem::size_of::<ApexHost>() {
        log("AutoIME: REFUSED -- the host ABI does not match (rebuild both)");
        return 1;
    }
    HOST.store(host as *mut ApexHost, Ordering::Release);
    SHUTDOWN.store(false, Ordering::Release);

    // WHERE THIS FEATURE'S FILES LIVE. Asked NOW because the host only knows the answer during its own calls
    // into the feature, and this is one of them -- a worker thread asking later would get an empty answer.
    let dir = {
        let mut buf = [0 as c_char; 512];
        match h.featureDir {
            Some(f) if f(buf.as_mut_ptr(), buf.len() as c_int) != 0 => {
                let cstr = std::ffi::CStr::from_ptr(buf.as_ptr());
                match cstr.to_str() {
                    Ok(s) => Some(std::path::PathBuf::from(s)),
                    Err(_) => None,
                }
            }
            _ => None,
        }
    };
    match dir {
        Some(d) => {
            // ⚠️ TWO THINGS ARE SET UP BEFORE ANYTHING CAN BE REPORTED, and both come from this one folder:
            // where the feature keeps its own files, and WHICH INSTALLATION IT IS PART OF -- two levels up,
            // because the ABI documents this folder as `Plugins/<id>/` (apex/abi.h, `featureDir`). The second is
            // how the monitor recognises Apex's own windows, including the settings panel's; see
            // `model::is_apex_itself`.
            if let Some(installation) = model::installation_of(&d) {
                model::set_install_root(installation);
            }
            model::set_config_dir(d.clone());
            // A FRESH DIAGNOSTIC LOG PER RUN, opened before the first thing worth recording. See the long note
            // on DIAG_FILE: it used to be append-only, in the host's folder, untimestamped and written 50 times
            // a second.
            win32::start_diagnostics();
            // ⚠️ THE INSTALLATION ROOT GETS A LINE OF ITS OWN, because the guard that keeps the rules out of
            // Apex's own windows is built from it: "(unknown)" means that guard has fallen back to comparing the
            // host's exe name -- which cannot see the settings panel. A state that would otherwise be invisible.
            match model::install_root() {
                Some(root) => win32::diag(&format!("install root {}", root.display())),
                None => win32::diag(
                    "install root UNKNOWN -- Apex's own windows are recognised by exe name only",
                ),
            }
            log(&format!("AutoIME: folder {}", d.display()));
            // THE USER'S RULES, loaded once here so a bad file is reported at start-up rather than silently
            // producing a feature that does nothing.
            let cfg = model::load_config();
            log(&format!("AutoIME: {} rule(s) loaded", cfg.rules.len()));
            win32::diag(&format!("{} rule(s) loaded", cfg.rules.len()));
            let shared = std::sync::Arc::new(std::sync::Mutex::new(cfg));
            // ⚠️ THE SETTINGS PAGE NEEDS THE SAME CONFIG, so a second handle to it is kept here. It is the
            // SAME Arc, not a copy: the monitor thread reads the rules and the panel writes them, and two
            // copies would mean an edited rule taking effect only after a restart.
            settings::set_config(shared.clone());
            // ⚠️ ON ITS OWN THREAD, AND THAT IS NOT A PREFERENCE. The work is `SendMessage` into another
            // process's IME window plus UI Automation walks -- tens of milliseconds, or seconds if the target
            // is busy. The host's `tick` is called 250 times a second on the thread that feeds the wheel
            // injector, so doing this there would stall scrolling for every program on the machine.
            std::thread::Builder::new()
                .name("AutoIME-monitor".into())
                .spawn(move || win32::run_monitor(shared))
                .map(|_| log("AutoIME: monitor thread running"))
                .unwrap_or_else(|e| log(&format!("AutoIME: could not start the monitor thread: {e}")));
        }
        None => {
            // No folder means no rules file, and a feature that quietly monitors with no rules is worse than
            // one that says it cannot start.
            log("AutoIME: REFUSED -- the host did not give us a folder for our settings");
            HOST.store(std::ptr::null_mut(), Ordering::Release);
            return 1;
        }
    }
    0
}

unsafe extern "C" fn fe_shutdown() {
    // ASKED TO STOP, THEN WAITED FOR: the loop may be inside a `SendMessage` to another program's IME window,
    // and the DLL must not be unloaded while that is in flight. The wait is bounded because a hung target
    // process must not be able to hang Apex's exit -- the thread is detached either way, and the flag means it
    // will not do any further work even if it outlives the unload (there is nothing left to call into: every
    // path out of it checks the flag or the host pointer first).
    SHUTDOWN.store(true, Ordering::Release);
    for _ in 0..50 {
        if LEFT_THE_LOOP.load(Ordering::Acquire) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    log("AutoIME: stopped");
    win32::diag("---- stopped ----");
    HOST.store(std::ptr::null_mut(), Ordering::Release);
}

unsafe extern "C" fn fe_settings_json(out: *mut c_char, out_size: c_int) -> c_int {
    settings::settings_json(out, out_size)
}

unsafe extern "C" fn fe_set_control(path: *const c_char, value: *const c_char) -> c_int {
    settings::set_control(path, value)
}

unsafe extern "C" fn fe_list_op(id: *const c_char, op: *const c_char, value: *const c_char,
                                index: c_int) -> c_int {
    settings::list_op(id, op, value, index)
}

unsafe extern "C" fn fe_save() -> c_int {
    settings::save_settings()
}

unsafe extern "C" fn fe_reload() -> c_int {
    settings::reload_settings()
}

// ⚠️ THIS FEATURE PUBLISHES NO READ-OUT, AND `liveText` NO LONGER EXISTS TO PUBLISH THROUGH. AutoIME was the
// first feature to have one, and the user removed it ("自动输入法页，最上方的状态提示去了"); KeepAwake then had one and
// the user removed that too ("其实这个提示可以完全去掉。并不需要"), so the field went out of the ABI entirely (9 -> 10,
// see abi.rs). The page's signal that a CAPTURE finished never travelled through that string anyway -- it watches
// the controls document (see `watchForCapture`).
unsafe extern "C" fn fe_flags() -> c_uint {
    APEX_FEATURE_ENABLED
}

/// ⚠️ THIS FEATURE TAKES NO WHEELS. It is here because the host requires the field to be present (it is not
/// optional in the struct), and returning 0 is the honest answer: nothing about switching input methods has
/// anything to do with scrolling.
unsafe extern "C" fn fe_on_wheel(_ev: *const ApexWheelEvent) -> c_int {
    0
}

/// ⚠️ NO WORK HERE, AND THAT IS A RULE RATHER THAN A PREFERENCE. This is called at 250 Hz on the host's engine
/// thread, and this feature's work is `SendMessage` to another process's IME window and UI Automation walks --
/// both of which can block for tens of milliseconds or more. Doing that here would stall the host's wheel
/// pipeline. The work lives on this feature's own thread (see `win32::spawn_monitor`).
unsafe extern "C" fn fe_tick(_dt: c_double) -> c_double {
    0.0
}

static K_FEATURE: ApexFeature = ApexFeature {
    abiVersion: APEX_ABI_VERSION,
    structSize: std::mem::size_of::<ApexFeature>() as c_uint,
    id: c"AutoIME".as_ptr(),
    nameZh: c"\u{81ea}\u{52a8}\u{8f93}\u{5165}\u{6cd5}".as_ptr(), // 自动输入法
    nameEn: c"Auto IME".as_ptr(),
    version: c"1.2.1".as_ptr(),
    init: Some(fe_init),
    shutdown: Some(fe_shutdown),
    reloadSettings: Some(fe_reload),
    settingsJson: Some(fe_settings_json),
    setControl: Some(fe_set_control),
    listOp: Some(fe_list_op),
    // ⚠️ NO QUICK-PANEL CONTROLS, AND THAT IS AN ANSWER RATHER THAN AN OMISSION. This feature's settings are
    // nine rules of up to nine conditions each, edited in `config.json` -- its page is honestly empty (see
    // settings.rs). There is no single number or switch in it that a 320-pixel flyout could carry, and the one
    // thing a user would reach for in a hurry -- "turn auto-IME off for now" -- is already there: it is this
    // feature's entry in the host's own switch list, which the quick panel always shows (see hostconfig.h).
    quickItems: None,
    saveSettings: Some(fe_save),
    onWheel: Some(fe_on_wheel),
    tick: Some(fe_tick),
    flags: Some(fe_flags),
};

#[no_mangle]
pub extern "C" fn ApexFeatureEntry() -> *const ApexFeature {
    &K_FEATURE
}

/// NOT PART OF THE ABI -- a self-check the gates call: the sizes this DLL was built with.
///
/// ⚠️ THIS EXISTS BECAUSE THE ABI IS DESCRIBED TWICE, once in C and once in Rust (see abi.rs), and neither
/// compiler can see the other's copy. The host checks the sizes at load time and refuses a mismatch, so a
/// wrong transcription cannot corrupt memory -- but it CAN silently make this feature unloadable, and a test
/// that prints both numbers turns "the DLL does not load" into "the struct is 8 bytes out".
#[no_mangle]
pub extern "C" fn ApexFeatureSizes(host_size: *mut c_uint, feature_size: *mut c_uint) {
    unsafe {
        if !host_size.is_null() {
            *host_size = std::mem::size_of::<ApexHost>() as c_uint;
        }
        if !feature_size.is_null() {
            *feature_size = std::mem::size_of::<ApexFeature>() as c_uint;
        }
    }
}
