//! THE HOST'S C ABI, IN RUST.
//!
//! ⚠️ THIS FILE IS A TRANSCRIPTION OF `apex/abi.h`, AND THE TWO MUST AGREE FIELD FOR FIELD, IN ORDER.
//! They are two languages' descriptions of one struct, so nothing here can be checked by the compiler on the
//! C side or by cargo on the Rust side -- only by the loader, which refuses a mismatch at run time (it
//! compares `abiVersion` and `structSize` before reading anything, which is why a stale DLL is an error rather
//! than an out-of-bounds read). `_diag/abi_size_probe` prints both sizes and a gate compares them.
//!
//! ⚠️ `repr(C)` IS NOT DECORATION. Rust is free to reorder a struct's fields; the host is reading raw offsets.
#![allow(non_snake_case, non_camel_case_types)]

use std::os::raw::{c_char, c_double, c_int, c_uint, c_void};

/// The host, as a feature sees it. Field order and types are `ApexHost` in apex/abi.h.
#[repr(C)]
pub struct ApexHost {
    pub abiVersion: c_uint,
    pub structSize: c_uint,
    pub targetAt: Option<unsafe extern "C" fn(c_int, c_int, *mut ApexTarget) -> c_int>,
    pub injectDeltas: Option<unsafe extern "C" fn(c_double)>,
    pub logLine: Option<unsafe extern "C" fn(*const c_char)>,
    pub featureDir: Option<unsafe extern "C" fn(*mut c_char, c_int) -> c_int>,
    pub activity: Option<unsafe extern "C" fn()>,
    /// WHICH EXTERNAL SMOOTHING ENGINES ARE RUNNING, EARLIEST-STARTED FIRST (abi.h: `activeEngines`). It replaced a
    /// call that asked about REAPER alone: the note the user asked for names every program that brings its own
    /// smoothing -- "REAPER、Lertaro专用引擎已运行". `out` receives one `ApexEngine` per running engine and the return
    /// value is how many were written. ⚠️ The NAME travels with each one, so a program added to the host's table
    /// appears without any feature being edited.
    pub activeEngines: Option<unsafe extern "C" fn(*mut ApexEngine, c_int) -> c_int>,
    pub featureEnabled: Option<unsafe extern "C" fn(*const c_char) -> c_int>,
    pub hostUser: *mut c_void,
}

/// `APEX_ENGINE_*` from apex/abi.h -- which external program has its own smoothing running, for
/// `ApexHost::activeEngines`. Transcribed for the same reason the shapes below are: this file is a complete copy
/// of the C contract, whether or not this crate reads it.
#[allow(dead_code)]
pub const APEX_ENGINE_REAPER: c_int = 1;
#[allow(dead_code)]
pub const APEX_ENGINE_LERTARO: c_int = 2;

/// One running engine (apex/abi.h: `ApexEngine`). ⚠️ `name` IS THE PROGRAM'S OWN SPELLING, NOT A TRANSLATION --
/// "REAPER" and "Lertaro" read the same in every language, and the sentence around them belongs to the feature that
/// writes it. The name travels with the answer so that adding a program is a change in ONE place on the host side.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ApexEngine {
    pub kind: c_int,
    pub name: [c_char; 32],
}

/// ApexTarget in apex/abi.h: pid, a bare lower-case exe name, and who handles wheels there.
#[repr(C)]
pub struct ApexTarget {
    pub pid: u32,
    pub exe: [c_char; 64],
    pub handlerState: c_int,
}

/// A wheel event (apex/abi.h: ApexWheelEvent).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ApexWheelEvent {
    pub delta: c_int,
    pub x: c_int,
    pub y: c_int,
    pub key: c_uint,
    pub injected: c_int,
    /// The message's extra-info word as the hook read it. Windows tags touch/pen input with 0xFF515700
    /// (see common/device.h); the host never interprets it.
    pub extraInfo: u64,
}

/// One control this feature puts in the host's QUICK PANEL (apex/abi.h: ApexQuickItem).
///
/// ⚠️ THE STRINGS ARE FIXED-SIZE BUFFERS HERE, WHERE EVERY OTHER STRUCT IN THIS FILE CARRIES POINTERS. That is
/// not an inconsistency in the transcription: the C side is written the same way, because the host KEEPS these
/// items for as long as its flyout is up and redraws them every frame -- a pointer would have to stay valid
/// across calls, which the ABI never promises. See the note on ApexQuickItem in apex/abi.h.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ApexQuickItem {
    pub id: [c_char; 64],
    pub labelZh: [c_char; 64],
    pub labelEn: [c_char; 64],
    pub unit: [c_char; 24],
    /// The block this control belongs in (abi.h: `groupZh`/`groupEn`, ABI 12 -> 13). EMPTY MEANS "A BLOCK OF MY
    /// OWN", which is what every control used to get -- the user's rule was "以一个开关为一组", so a pane was the
    /// grouping and a lone control needed no heading. A non-empty name collects several controls into one pane
    /// with that name as its heading ("分组名称为「亮度」").
    pub groupZh: [c_char; 32],
    pub groupEn: [c_char; 32],
    pub type_: c_int,
    pub min: c_double,
    pub max: c_double,
    pub step: c_double,
    pub value: c_double,
    pub hue: c_uint,
    /// A companion switch drawn at the right of this row (abi.h: `toggleId`/`toggleOn`, ABI 14 -> 15) -- the mute
    /// button beside a volume fader ("音量在推子右边增加静音按钮"). EMPTY MEANS "THIS ROW HAS NONE", and it is a
    /// normal control path: the panel sends "1"/"0" through `setControl` and rebuilds from what the feature says.
    pub toggleId: [c_char; 64],
    pub toggleOn: c_int,
    /// What that companion switch MEANS, so the panel can pick its icon (abi.h: `toggleIcon`, ABI 15 -> 16): a
    /// mute button and a screen-off button are both two-state switches and must not look alike. One of
    /// `APEX_QUICK_ICON_*` below; an unknown value is drawn as the plain one.
    pub toggleIcon: c_int,
    /// The short word beside the row's OWN switch and beside its companion (abi.h: `switchLabel*`/`toggleLabel*`,
    /// ABI 19 -> 20) -- what tells two identically drawn switches apart, which is KeepAwake's "keep the machine
    /// awake" / "keep the screen on" pair ("注明哪个是防睡，哪个是防熄"). The panel owns the drawing but cannot
    /// invent the words, so they come from the feature; empty means "no label", and the column is not reserved.
    pub switchLabelZh: [c_char; 24],
    pub switchLabelEn: [c_char; 24],
    pub toggleLabelZh: [c_char; 24],
    pub toggleLabelEn: [c_char; 24],
}

/// `APEX_QUICK_ICON_PLAIN` / `_MUTE` / `_DISPLAY` from apex/abi.h -- what a companion switch is, for the purpose of
/// choosing the icon the panel draws. Same note as the shapes below: no reader in this crate, transcribed because
/// this file is a complete copy of the C contract.
#[allow(dead_code)]
pub const APEX_QUICK_ICON_PLAIN: c_int = 0;
#[allow(dead_code)]
pub const APEX_QUICK_ICON_MUTE: c_int = 1;
#[allow(dead_code)]
pub const APEX_QUICK_ICON_DISPLAY: c_int = 2;

/// `APEX_QUICK_TOGGLE` / `_SLIDER` / `_KNOB` from apex/abi.h -- the three shapes the panel can draw.
///
/// ⚠️ THESE HAVE NO READER IN THIS CRATE AND THEY STAY ANYWAY, for the same reason `APEX_FEATURE_ACTIVE` does
/// (see the note further down): this file's job is to be a COMPLETE transcription of the C header, and the set
/// of shapes a control may be is part of that description. This feature puts nothing in the quick panel (see
/// `quickItems: None` in lib.rs), which is a fact about the feature rather than about the ABI.
#[allow(dead_code)]
pub const APEX_QUICK_TOGGLE: c_int = 0;
#[allow(dead_code)]
pub const APEX_QUICK_SLIDER: c_int = 1;
#[allow(dead_code)]
pub const APEX_QUICK_KNOB: c_int = 2;

/// The feature, as the host sees it. Field order and types are `ApexFeature` in apex/abi.h.
#[repr(C)]
pub struct ApexFeature {
    pub abiVersion: c_uint,
    pub structSize: c_uint,
    pub id: *const c_char,
    pub nameZh: *const c_char,
    pub nameEn: *const c_char,
    pub version: *const c_char,
    pub init: Option<unsafe extern "C" fn(*const ApexHost) -> c_int>,
    pub shutdown: Option<unsafe extern "C" fn()>,
    pub reloadSettings: Option<unsafe extern "C" fn() -> c_int>,
    pub settingsJson: Option<unsafe extern "C" fn(*mut c_char, c_int) -> c_int>,
    pub setControl: Option<unsafe extern "C" fn(*const c_char, *const c_char) -> c_int>,
    pub listOp: Option<unsafe extern "C" fn(*const c_char, *const c_char, *const c_char, c_int) -> c_int>,
    pub quickItems: Option<unsafe extern "C" fn(*mut ApexQuickItem, c_int) -> c_int>,
    pub saveSettings: Option<unsafe extern "C" fn() -> c_int>,
    pub onWheel: Option<unsafe extern "C" fn(*const ApexWheelEvent) -> c_int>,
    pub tick: Option<unsafe extern "C" fn(c_double) -> c_double>,
    pub flags: Option<unsafe extern "C" fn() -> c_uint>,
}

/// `APEX_ABI_VERSION` from apex/abi.h. ⚠️ BUMP THIS WHENEVER THE C SIDE DOES.
///
/// It went to 8 with the two `flags()` bits for "a feature is holding something the user cannot see" and
/// "tell the user about it, once" (abi.h), to 9 when the second of those was replaced by
/// `APEX_FEATURE_HOLD_HARD` (the same hold, one degree stronger, which the tray draws in its stronger ink) and
/// `group` gained the four things a list of SWITCHES needs (`layout:"rows"`, a `rowToggle` that may name
/// several fields, `items[].locked`, `addHint*`), to 10 when `liveText` -- the feature's one-line read-out --
/// was REMOVED (the user tried it, then twice said it was not needed; see abi.h), to 11 when `group` gained
/// `waiting` -- the feature saying "I am still waiting for the click, keep asking", which replaced the panel's
/// guess that the wait was over whenever the document changed (see the note on `waiting` in abi.h: the guess
/// failed in both directions, and the user's report -- a capture's result not reaching the page -- was one of
/// them), and to 12 when `quickItems` was added -- the feature putting a few of its own controls into the
/// host's QUICK PANEL, the small flyout the tray shows on a single click. That last one moved a struct FIELD,
/// which is exactly the change this constant exists for: read
/// `ApexFeature` here and in abi.h together
/// whenever either is touched. To 13 it went with `ApexQuickItem` gaining `groupZh`/`groupEn`: the name of the
/// BLOCK a control belongs in, so that a feature's per-monitor brightness faders land in one pane called
/// "亮度" instead of one floating pane each. Also a struct field, and also a change an older DLL would have
/// read as whatever followed it. To 14 it went with `group` gaining `noAdd` -- a group whose rows the user
/// cannot create, which is a KEY a page acts on rather than a field in a struct (the same shape as `waiting`,
/// which is why it is bumped at all). To 15 it went with `ApexQuickItem` gaining `toggleId`/`toggleOn`: a
/// companion switch at the right of a row, which is the mute button on a volume fader. To 16 it went with
/// `toggleIcon` -- which picture that companion is drawn with, because a mute button and a screen-off button are
/// both two-state and must not look the same. To 17 it went with `group` gaining `quick` (ONE switch that maps a
/// whole group into the flyout, drawn beside the group's own heading or its Add control) and with
/// `ApexQuickItem::toggleId` now being drawn on a TOGGLE row as well as a range row -- which is a change to the
/// meaning of an existing field, the kind an exact-match version exists to make loud. To 18 it went with `group`
/// gaining `live`: a group whose rows are a picture of something outside the page, which the panel keeps
/// re-reading while it is on screen (this replaced MediaControl's "refresh the application list" button). To 19
/// it went with `ApexWheelEvent` gaining `extraInfo` -- the message's own extra-info word, which is where Windows
/// says a wheel came from a touchpad or a pen (signature 0xFF515700) rather than from a mouse; the classifier
/// that reads it lives in common/device.h on the C side. To 20 it went with `ApexQuickItem` gaining
/// `switchLabel*`/`toggleLabel*`: the short words beside a row's two switches, which is how the flyout tells
/// "防睡" from "防熄" on KeepAwake's rows. To 21 it went when `reaperPluginRunning` was REPLACED by
/// `activeEngines` -- a set of external smoothing engines in start order instead of one program's yes/no, because
/// Lertaro brought its own smoothing and the note names every engine that has one. To 22 it went the very next day,
/// when `activeEngines` stopped handing back bare `APEX_ENGINE_*` values and started handing back `ApexEngine` --
/// the kind PLUS the program's own name -- because the user's next sentence was "包括以后可能会增加的 APP": with
/// kinds alone, adding an engine meant editing the host AND every feature, and a feature nobody updated would drop
/// the new engine in silence. This
/// feature uses none of these, but the host requires an EXACT match --
/// so forgetting this line does not degrade anything, it makes AutoIME fail to load with a line in the log.
/// (That is exactly what happened once, and the end-to-end wheel gate went red because of it: one feature
/// missing changes what the host decides about a wheel.)
pub const APEX_ABI_VERSION: c_uint = 22;

pub const APEX_FEATURE_ENABLED: c_uint = 1;

/// `APEX_FEATURE_ACTIVE` HAS NO READER IN THIS CRATE, AND IT STAYS ANYWAY: this file's job is to be a COMPLETE
/// transcription of the C header (see the note at the top), and the bits `flags()` may return are a set -- one
/// of them missing would make the mirror say something the header does not. This feature never reports ACTIVE
/// (it has no motion to animate), which is a fact about the feature rather than about the ABI.
#[allow(dead_code)]
pub const APEX_FEATURE_ACTIVE: c_uint = 2;

// ---------------------------------------------------------------------------
// THE STATIC STRUCT IS SHARED WITH THE HOST'S THREAD, SO RUST NEEDS TO BE TOLD.
//
// A `static` must be Sync, and ApexFeature holds raw pointers (the strings, the function addresses), which are
// not. The claim being made here is a real one and it is the same one the host already relies on: THE STRUCT IS
// BUILT ONCE AND NEVER MUTATED. The host reads it from its own thread; this side only ever hands back a pointer
// to it. There is no interior mutability, so shared access is exactly what happens.
//
// ⚠️ IMPLEMENTING Sync IS UNSAFE, WHICH IS WHY IT IS WRITTEN OUT RATHER THAN AVOIDED: it marks the one place
// where a claim about the ABI's semantics is ASSERTED rather than checked. If a field is ever made mutable, or
// a callback ever writes through one of these pointers, this stops being true and the impl must go.
unsafe impl Sync for ApexFeature {}
unsafe impl Send for ApexFeature {}
