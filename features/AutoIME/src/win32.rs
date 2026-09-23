//! THE WINDOWS HALF: what the host cannot do for this feature, done here.
//!
//! ⚠️ WHAT IS *NOT* HERE ANY MORE, AND WHY, because it is the largest thing this file's history holds. Upstream
//! is a standalone program, so its `win32.rs` owns a window class, a message loop, a tray, a hotkey, a
//! single-instance mutex and a "put the window back on screen" rule. Apex owns every one of those GENERICALLY
//! (AGENTS.md §零), and the port kept the code `pub` "in case" -- so this file carried a window class nothing
//! registered, a mutex nothing created and a hotkey nothing pressed, each looking like the way things were done.
//! `#![deny(dead_code)]` (lib.rs) is what keeps that from coming back: a second, unused copy of something the
//! host already does is not a spare part, it is a second answer to a question that has one.
//!
//! ⚠️ `non_snake_case` IS ALLOWED FOR THE WHOLE FILE, AND IT IS NOT LAZINESS: every struct below is a Windows
//! struct whose FIELD NAMES ARE THE HEADER'S (`cbSize`, `hwndFocus`, `szExeFile`). Renaming them to satisfy a
//! Rust style lint would put a translation between this file and the documentation it is transcribed from --
//! and this project's rule is that a name and its meaning must not drift apart. The same allowance, for the
//! same reason, is at the top of `abi.rs`.
#![allow(non_snake_case)]

use crate::model::{
    config_path, install_root, is_apex_itself, Action, AppConfig, SwitchMethod, WindowInfo,
};
use std::ffi::c_void;
use std::fs::File;
use std::io::Write;
use std::mem;
use std::ptr;
use std::sync::mpsc::{Receiver, Sender};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::collections::HashMap;
use std::time::{Duration, Instant};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CLSCTX_ALL, COINIT_APARTMENTTHREADED,
};
use windows::Win32::UI::Accessibility::{CUIAutomation, IUIAutomation, IUIAutomationElement};

type BOOL = i32;
type DWORD = u32;
type UINT = u32;
type LONG = i32;
type HWND = isize;
type HANDLE = isize;
type HMODULE = isize;
type LRESULT = isize;
type LPARAM = isize;
type WPARAM = usize;
type WINEVENTPROC = unsafe extern "system" fn(HANDLE, DWORD, HWND, LONG, LONG, DWORD, DWORD);

const GA_ROOT: UINT = 2;
const CWP_SKIPINVISIBLE: UINT = 0x0001;
const CWP_SKIPDISABLED: UINT = 0x0002;
const CWP_SKIPTRANSPARENT: UINT = 0x0004;

const PROCESS_QUERY_LIMITED_INFORMATION: DWORD = 0x1000;
const THREAD_QUERY_LIMITED_INFORMATION: DWORD = 0x0800;
const TH32CS_SNAPPROCESS: DWORD = 0x00000002;
const INVALID_HANDLE_VALUE: HANDLE = -1;
const WAIT_OBJECT_0: DWORD = 0x00000000;

const KEYEVENTF_KEYUP: DWORD = 0x0002;

// WinEvent 焦点监听（键盘切换焦点时事件驱动触发规则评估）。
const EVENT_SYSTEM_FOREGROUND: UINT = 0x0003;
const EVENT_OBJECT_FOCUS: UINT = 0x8005;
const WINEVENT_OUTOFCONTEXT: DWORD = 0x0000;
const WINEVENT_SKIPOWNPROCESS: DWORD = 0x0002;

const VK_SHIFT: u8 = 0x10;
const VK_CONTROL: u8 = 0x11;
const VK_MENU: u8 = 0x12;
const VK_LWIN: u8 = 0x5B;
const VK_SPACE: u8 = 0x20;
const VK_LBUTTON: u8 = 0x01;

const WM_IME_CONTROL: UINT = 0x0283;
const IMC_GETOPENSTATUS: WPARAM = 0x0005;
const IMC_SETOPENSTATUS: WPARAM = 0x0006;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct POINT {
    x: LONG,
    y: LONG,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RECT {
    left: LONG,
    top: LONG,
    right: LONG,
    bottom: LONG,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct GUITHREADINFO {
    cbSize: DWORD,
    flags: DWORD,
    hwndActive: HWND,
    hwndFocus: HWND,
    hwndCapture: HWND,
    hwndMenuOwner: HWND,
    hwndMoveSize: HWND,
    hwndCaret: HWND,
    rcCaret: RECT,
}

#[repr(C)]
struct PROCESSENTRY32W {
    dwSize: DWORD,
    cntUsage: DWORD,
    th32ProcessID: DWORD,
    th32DefaultHeapID: usize,
    th32ModuleID: DWORD,
    cntThreads: DWORD,
    th32ParentProcessID: DWORD,
    pcPriClassBase: LONG,
    dwFlags: DWORD,
    szExeFile: [u16; 260],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SYSTEMTIME {
    wYear: u16,
    wMonth: u16,
    wDayOfWeek: u16,
    wDay: u16,
    wHour: u16,
    wMinute: u16,
    wSecond: u16,
    wMilliseconds: u16,
}

#[link(name = "user32")]
extern "system" {
    fn GetCursorPos(lpPoint: *mut POINT) -> BOOL;
    fn WindowFromPoint(pt: POINT) -> HWND;
    fn GetAncestor(hwnd: HWND, gaFlags: UINT) -> HWND;
    fn GetWindowTextW(hwnd: HWND, lpString: *mut u16, nMaxCount: i32) -> i32;
    fn GetClassNameW(hwnd: HWND, lpClassName: *mut u16, nMaxCount: i32) -> i32;
    fn GetWindowThreadProcessId(hwnd: HWND, lpdwProcessId: *mut DWORD) -> DWORD;
    fn ScreenToClient(hwnd: HWND, lpPoint: *mut POINT) -> BOOL;
    fn ChildWindowFromPointEx(hwnd: HWND, pt: POINT, flags: UINT) -> HWND;
    fn GetForegroundWindow() -> HWND;
    fn GetGUIThreadInfo(idThread: DWORD, pgui: *mut GUITHREADINFO) -> BOOL;
    fn keybd_event(bVk: u8, bScan: u8, dwFlags: DWORD, dwExtraInfo: usize);
    fn GetAsyncKeyState(vKey: i32) -> i16;
    fn SendMessageW(hWnd: HWND, Msg: UINT, wParam: WPARAM, lParam: LPARAM) -> LRESULT;
    fn SetWinEventHook(
        eventMin: UINT,
        eventMax: UINT,
        hmodWinEventProc: HMODULE,
        pfnWinEventProc: Option<WINEVENTPROC>,
        idProcess: DWORD,
        idThread: DWORD,
        dwFlags: DWORD,
    ) -> HANDLE;
}

#[link(name = "imm32")]
extern "system" {
    fn ImmGetDefaultIMEWnd(hwnd: HWND) -> HWND;
}

#[link(name = "kernel32")]
extern "system" {
    fn OpenProcess(dwDesiredAccess: DWORD, bInheritHandle: BOOL, dwProcessId: DWORD) -> HANDLE;
    fn CloseHandle(hObject: HANDLE) -> BOOL;
    fn QueryFullProcessImageNameW(
        hProcess: HANDLE,
        dwFlags: DWORD,
        lpExeName: *mut u16,
        lpdwSize: *mut DWORD,
    ) -> BOOL;
    fn CreateToolhelp32Snapshot(dwFlags: DWORD, th32ProcessID: DWORD) -> HANDLE;
    fn Process32FirstW(hSnapshot: HANDLE, lppe: *mut PROCESSENTRY32W) -> BOOL;
    fn Process32NextW(hSnapshot: HANDLE, lppe: *mut PROCESSENTRY32W) -> BOOL;
    fn GetLocalTime(lpSystemTime: *mut SYSTEMTIME);
    fn OpenThread(dwDesiredAccess: DWORD, bInheritHandle: BOOL, dwThreadId: DWORD) -> HANDLE;
    fn CreateEventW(
        lpEventAttributes: *mut c_void,
        bManualReset: BOOL,
        bInitialState: BOOL,
        lpName: *const u16,
    ) -> HANDLE;
    fn SetEvent(hEvent: HANDLE) -> BOOL;
    fn ResetEvent(hEvent: HANDLE) -> BOOL;
    fn WaitForSingleObject(hHandle: HANDLE, dwMilliseconds: DWORD) -> DWORD;
}

struct UiaControl {
    name: String,
    class: String,
    control_type: String,
    automation_id: String,
    container_text: String,
    ancestor_texts: Vec<String>,
    ancestor_classes: Vec<String>,
}

struct UiaAncestorInfo {
    container_text: String,
    ancestor_texts: Vec<String>,
    ancestor_classes: Vec<String>,
}

fn ensure_com_initialized() {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
    }
}

fn uia_automation() -> Option<IUIAutomation> {
    ensure_com_initialized();
    let automation: IUIAutomation =
        unsafe { CoCreateInstance(&CUIAutomation, None, CLSCTX_ALL).ok()? };
    Some(automation)
}

fn uia_control_from_element(element: &IUIAutomationElement) -> Option<UiaControl> {
    unsafe {
        let name = element
            .CurrentName()
            .ok()
            .map(|value| value.to_string())
            .unwrap_or_default();
        let class = element
            .CurrentClassName()
            .ok()
            .map(|value| value.to_string())
            .unwrap_or_default();
        let automation_id = element
            .CurrentAutomationId()
            .ok()
            .map(|value| value.to_string())
            .unwrap_or_default();
        let control_type = element
            .CurrentControlType()
            .ok()
            .map(|id| uia_control_type_name(id.0))
            .unwrap_or_default();

        Some(UiaControl {
            name,
            class,
            control_type,
            automation_id,
            container_text: String::new(),
            ancestor_texts: Vec::new(),
            ancestor_classes: Vec::new(),
        })
    }
}

fn uia_from_point(pt: POINT) -> Option<UiaControl> {
    let automation = uia_automation()?;
    let point = windows::Win32::Foundation::POINT {
        x: pt.x,
        y: pt.y,
    };
    let element = unsafe { automation.ElementFromPoint(point).ok()? };
    let mut control = uia_control_from_element(&element)?;
    let info = uia_ancestor_info(&automation, &element);
    control.container_text = if info.container_text.is_empty() {
        info.ancestor_texts.first().cloned().unwrap_or_default()
    } else {
        info.container_text
    };
    control.ancestor_texts = info.ancestor_texts;
    control.ancestor_classes = info.ancestor_classes;
    Some(control)
}

fn uia_from_hwnd(hwnd: HWND) -> Option<UiaControl> {
    let automation = uia_automation()?;
    let element = unsafe {
        automation
            .ElementFromHandle(windows::Win32::Foundation::HWND(
                hwnd as *mut core::ffi::c_void,
            ))
            .ok()?
    };
    let mut control = uia_control_from_element(&element)?;
    let info = uia_ancestor_info(&automation, &element);
    control.container_text = if info.container_text.is_empty() {
        info.ancestor_texts.first().cloned().unwrap_or_default()
    } else {
        info.container_text
    };
    control.ancestor_texts = info.ancestor_texts;
    control.ancestor_classes = info.ancestor_classes;
    Some(control)
}

fn uia_ancestor_info(
    automation: &IUIAutomation,
    element: &IUIAutomationElement,
) -> UiaAncestorInfo {
    let Ok(walker) = (unsafe { automation.ControlViewWalker() }) else {
        return UiaAncestorInfo {
            container_text: String::new(),
            ancestor_texts: Vec::new(),
            ancestor_classes: Vec::new(),
        };
    };
    let mut info = UiaAncestorInfo {
        container_text: String::new(),
        ancestor_texts: Vec::new(),
        ancestor_classes: Vec::new(),
    };
    let mut current = match unsafe { walker.GetParentElement(element) } {
        Ok(parent) => parent,
        Err(_) => return info,
    };
    for index in 0..16 {
        let control_type = unsafe {
            current
                .CurrentControlType()
                .ok()
                .map(|id| id.0)
                .unwrap_or(0)
        };
        let name = unsafe {
            current
                .CurrentName()
                .ok()
                .map(|value| value.to_string())
                .unwrap_or_default()
        };
        let class = unsafe {
            current
                .CurrentClassName()
                .ok()
                .map(|value| value.to_string())
                .unwrap_or_default()
        };
        if capture_armed() {
            diag(&format!(
                "UIA_ANCESTOR[{index}] type={control_type} ({}) name={name} class={class}",
                uia_control_type_name(control_type)
            ));
        }
        // 容器/标签类控件：Tab、TabItem、ToolBar、Group、Pane、Window、Custom。
        let is_container = matches!(control_type, 50018 | 50019 | 50021 | 50025 | 50026 | 50032 | 50033);
        if is_container {
            if info.container_text.is_empty() && !name.is_empty() {
                info.container_text = name.clone();
            }
            if !name.is_empty() {
                info.ancestor_texts.push(name);
            }
            if !class.is_empty() {
                info.ancestor_classes.push(class);
            }
        }
        match unsafe { walker.GetParentElement(&current) } {
            Ok(parent) => current = parent,
            Err(_) => break,
        }
    }
    info
}

fn uia_control_type_name(id: i32) -> String {
    let name = match id {
        50000 => "Button",
        50001 => "Calendar",
        50002 => "CheckBox",
        50003 => "ComboBox",
        50004 => "Edit",
        50005 => "Hyperlink",
        50006 => "Image",
        50007 => "ListItem",
        50008 => "List",
        50009 => "Menu",
        50010 => "MenuBar",
        50011 => "MenuItem",
        50012 => "ProgressBar",
        50013 => "RadioButton",
        50014 => "ScrollBar",
        50015 => "Slider",
        50016 => "Spinner",
        50017 => "StatusBar",
        50018 => "Tab",
        50019 => "TabItem",
        50020 => "Text",
        50021 => "ToolBar",
        50022 => "ToolTip",
        50023 => "Tree",
        50024 => "TreeItem",
        50025 => "Custom",
        50026 => "Group",
        50027 => "Thumb",
        50028 => "DataGrid",
        50029 => "DataItem",
        50030 => "Document",
        50031 => "SplitButton",
        50032 => "Window",
        50033 => "Pane",
        50034 => "Header",
        50035 => "HeaderItem",
        50036 => "Table",
        50037 => "TitleBar",
        50038 => "Separator",
        50039 => "SemanticZoom",
        50040 => "AppBar",
        _ => return format!("Unknown({id})"),
    };
    name.to_string()
}

pub fn capture_from_cursor() -> Option<WindowInfo> {
    let mut pt = POINT::default();
    if unsafe { GetCursorPos(&mut pt) } == 0 {
        return None;
    }
    capture_from_point(pt)
}

/// 抓取当前拥有键盘焦点的控件信息（键盘激活场景：Tab / 方向键等）。
pub fn capture_from_focus() -> Option<WindowInfo> {
    let hwnd = focused_hwnd()?;
    capture_from_hwnd(hwnd)
}

static CAPTURE_EVENT: OnceLock<HANDLE> = OnceLock::new();

fn capture_event() -> HANDLE {
    *CAPTURE_EVENT.get_or_init(|| {
        let name: Vec<u16> = "AutoIME_CaptureArmed_Event\0".encode_utf16().collect();
        unsafe { CreateEventW(ptr::null_mut(), 1, 0, name.as_ptr()) }
    })
}

pub fn set_capture_armed(armed: bool) {
    let handle = capture_event();
    if handle == 0 {
        return;
    }
    unsafe {
        if armed {
            SetEvent(handle);
        } else {
            ResetEvent(handle);
        }
    }
}

/// WAIT FOR THE USER'S NEXT CLICK, THEN REPORT WHAT THEY CLICKED ON -- upstream's `wait_for_click`.
///
/// ⚠️ WHY A THREAD RATHER THAN THE MONITOR LOOP. The monitor is the wrong place for this in both directions:
/// while a capture is armed its click handler would consume the click as a normal rule evaluation (it skips
/// anyway when `capture_armed`, but then nothing would ever deliver the result), and the whole point is to
/// wait INDEFINITELY for a click the user has not made yet. A thread that lives exactly as long as the wait is
/// the honest shape for that.
///
/// ⚠️⚠️ IT SPAWNS ITS OWN THREAD, AND THAT IS NOT A STYLE CHOICE -- CALLING IT DIRECTLY FREEZES APEX.
///
/// The first version of this function did the waiting inline, and its doc comment claimed the caller would be a
/// background thread. The caller was not: `list_op` runs on the HOST'S MESSAGE THREAD (settings_host.cpp,
/// reached from WM_COPYDATA in the host's window procedure), and the whole panel -- tray menu, settings window,
/// IPC -- lives on that thread. So "wait for the user's next click" meant "block every response Apex can make
/// until the user clicks", and the user reported exactly that: "捕获时，出现卡顿".
///
/// The thread is spawned here rather than at the call site so no future caller can make the same mistake: the
/// only way to use this function is the non-blocking way.
///
/// ⚠️ AND IT DOES NOT INSTALL A HOOK OR STEAL THE CLICK. It polls the button's own state, which is how upstream
/// does it too -- so the click reaches the target program normally and the capture is a bystander. Nothing here
/// moves the cursor or synthesises anything.
///
/// ⚠️ THE FIRST LOOP IS NOT REDUNDANT. Arming is usually a mouse click on the panel's own button (or the
/// hotkey, pressed with the same hand), so the button is often still DOWN when this starts -- without waiting
/// for it to come up, that same press would be read as the capture click and the feature would capture the
/// panel itself.
pub fn wait_for_click(report: Box<dyn Fn(WindowInfo) + Send + 'static>) {
    std::thread::Builder::new()
        // Named like the monitor thread's, so a stack dump or a process explorer says which one is which.
        .name("AutoIME-capture".into())
        .spawn(move || {
            const VK_LBUTTON_STATE: i32 = 0x01; // VK_LBUTTON

            while (unsafe { GetAsyncKeyState(VK_LBUTTON_STATE) } as u16 & 0x8000) != 0 {
                std::thread::sleep(Duration::from_millis(10));
            }
            while (unsafe { GetAsyncKeyState(VK_LBUTTON_STATE) } as u16 & 0x8000) == 0 {
                std::thread::sleep(Duration::from_millis(10));
            }

            // THE SAME 40 ms THE RUNTIME CLICK PATH WAITS, and for the same reason: the window the click
            // activated is still coming to the front, so reading the control under the cursor immediately
            // would read the old one.
            std::thread::sleep(Duration::from_millis(40));

            if let Some(info) = capture_from_cursor() {
                report(info);
            }
        })
        // A THREAD THAT CANNOT START IS REPORTED, NOT FATAL: the button would still be armed (the flag is set
        // by the caller), so the next click would arm and then nothing would consume it -- which is exactly the
        // kind of silent half-failure the log line exists for.
        .err()
        .map(|e| crate::log(&format!("AutoIME: could not start the capture thread: {e}")));
}


fn capture_armed() -> bool {
    let handle = capture_event();
    if handle == 0 {
        return false;
    }
    unsafe { WaitForSingleObject(handle, 0) == WAIT_OBJECT_0 }
}

// ===== 键盘焦点变化监听 =====// 用 WinEvent hook 感知鼠标之外的焦点切换（Tab / 方向键 / Alt+Tab / 点击菜单等），
// 事件回调只向 monitor 线程发送轻量通知，控件抓取与规则评估都在 monitor 线程执行。
//
// ⚠️⚠️ 「hook 必须在有消息循环的线程安装（daemon 主循环），回调由 DispatchMessageW 分发」是上游原话，
// 而在 Apex 里**这句话的两半都不成立**，我也没有实测过它到底要不要紧，所以只留下线索、不动行为：
//   * 上游有一个真消息泵（egui 主循环）；这里没有 —— 安装它的 monitor 线程自己的循环只 sleep +
//     `try_recv`，从不 `GetMessage`/`PeekMessage`/`DispatchMessageW`（见 run_monitor）。
//   * 所以「焦点事件是否真的到达了 focus_rx」是一个**未验证**的问题。要验证只需在 `win_event_proc`
//     里加一行计数、然后只用 Tab/Alt+Tab 切焦点看计数会不会长；在没人做这件事之前，别假设它通。
//   * 说明白代价：若它不通，症状是「用键盘切焦点不重新评估规则，只有鼠标点击才评估」—— 和上游
//     修这个 bug 时（见 run_monitor 上面那段注释）的样子一模一样，且没有任何地方会报错。

static FOCUS_EVENT_TX: OnceLock<Sender<()>> = OnceLock::new();
static FOCUS_EVENT_RX: OnceLock<Mutex<Option<Receiver<()>>>> = OnceLock::new();

/// 取出键盘焦点事件接收端（run_monitor 线程调用，仅能取出一次）。
pub fn take_focus_change_receiver() -> Option<Receiver<()>> {
    let cell = FOCUS_EVENT_RX.get_or_init(|| {
        let (tx, rx) = std::sync::mpsc::channel();
        let _ = FOCUS_EVENT_TX.set(tx);
        Mutex::new(Some(rx))
    });
    cell.lock().ok().and_then(|mut guard| guard.take())
}

pub fn install_focus_hook() {
    unsafe {
        let callback: Option<WINEVENTPROC> = Some(win_event_proc);
        SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            0,
            callback,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
        );
        SetWinEventHook(
            EVENT_OBJECT_FOCUS,
            EVENT_OBJECT_FOCUS,
            0,
            callback,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
        );
    }
}

unsafe extern "system" fn win_event_proc(
    _hook: HANDLE,
    _event: DWORD,
    _hwnd: HWND,
    _id_object: LONG,
    _id_child: LONG,
    _id_event_thread: DWORD,
    _event_time: DWORD,
) {
    if let Some(tx) = FOCUS_EVENT_TX.get() {
        let _ = tx.send(());
    }
}

// ---------------------------------------------------------------------------
// THE FEATURE'S OWN DIAGNOSTIC LOG.
//
// WHY A FILE OF ITS OWN rather than the host's log: what this feature decides is invisible -- which control had
// focus, which rule matched, what the input method was -- while the host's log is the host's (its startup, its
// target cache, the wheel). Both belong to the user, in different places, for different questions.
//
// ⚠️⚠️ THE PORTED VERSION WAS WRONG IN FOUR WAYS AT ONCE, AND THE SYMPTOM WAS A FILE NOBODY COULD USE:
//   * IT WROTE ON EVERY POLL -- `READ_IME ...` at the monitor's 20 ms pace, plus a line per decision. Up to 50
//     file writes a second, for a line that only restates what the decision line already carries. MEASURED on
//     this machine: 8118 lines / 564 KB in one afternoon, of which 7462 were that one line.
//   * IT LANDED IN THE PROGRAM'S OWN FOLDER: `current_exe()` inside a DLL is apex.exe, i.e. the HOST, so the
//     file sat beside apex.exe -- while the ABI says a feature's files belong in `Plugins/<id>/` and nowhere
//     else. A diagnostic next to the program also reads as part of the program.
//   * IT WAS APPENDED AND NEVER ROTATED, so it covered every launch since the file was created (06:19 that
//     day) with nothing marking where one run ended. Every conclusion drawn from it had to be qualified with
//     "this might be from an older build".
//   * IT HAD NO TIME IN IT. Order was the only temporal information, so "how long after the click did the
//     switch happen" -- the question the log exists for -- could not be answered at all.
//
// ⚠️ WHAT KEEPS IT SMALL IS THE CAP, NOT LUCK: one file per run (create truncates), a size ceiling, then
// silence. A runaway loop must not be able to fill the user's disk.
// ---------------------------------------------------------------------------

const DIAG_FILE: &str = "autoime_debug.log";
const DIAG_MAX_BYTES: u64 = 512 * 1024;

/// The open file; `None` until `start_diagnostics` runs -- and forever, if it could not be opened.
static DIAG: Mutex<Option<File>> = Mutex::new(None);
static DIAG_BYTES: AtomicU64 = AtomicU64::new(0);
/// Set once the cap is reached, so the "capped" line is written exactly once.
///
/// ⚠️ A FLAG RATHER THAN COMPARING THE COUNTER: the counter is only ever added to, so it jumps OVER the limit
/// (a line cannot be cut in half) -- a test for `== limit` would then never fire and the file would simply stop
/// with nothing to say why. A truncated log with no explanation reads like a crash.
static DIAG_CAPPED: AtomicBool = AtomicBool::new(false);

/// START A FRESH LOG FOR THIS RUN -- called once by `init`, before anything has anything to say.
///
/// ⚠️ IT TRUNCATES, AND THAT IS THE POINT RATHER THAN A CONVENIENCE: a log that starts empty at every launch is
/// a log whose every line belongs to the build that is running now. The old append-only file could not be read
/// without asking "which version wrote this?", which it could not answer.
///
/// A file that cannot be opened is REPORTED ONCE (into the host's log) and every `diag` is then a no-op: a
/// feature that cannot write its diagnostics must still do its job.
pub fn start_diagnostics() {
    let path = config_path().with_file_name(DIAG_FILE);
    match File::create(&path) {
        Ok(file) => {
            DIAG_BYTES.store(0, Ordering::Relaxed);
            if let Ok(mut slot) = DIAG.lock() {
                *slot = Some(file);
            }
            diag(&format!("---- AutoIME {} ----", env!("CARGO_PKG_VERSION")));
        }
        Err(e) => crate::log(&format!(
            "AutoIME: no diagnostic log ({}): {e}",
            path.display()
        )),
    }
}

/// One timestamped line. Silent before `start_diagnostics`, and silent once the cap is reached.
///
/// ⚠️ IT IS NOT ON THE HOT PATH AND MUST NOT BE PUT THERE: the monitor calls it when it makes a DECISION (a
/// click, a focus change, a capture), not once per loop turn. `onWheel`/`tick` never come near it -- those run
/// at 250 Hz on the host's engine thread, where the ABI forbids I/O.
pub(crate) fn diag(message: &str) {
    let mut slot = match DIAG.lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    let file = match slot.as_mut() {
        Some(f) => f,
        None => return,
    };
    let written = DIAG_BYTES.load(Ordering::Relaxed);
    if DIAG_CAPPED.load(Ordering::Relaxed) {
        return;
    }
    if written >= DIAG_MAX_BYTES {
        // One line to say why the file stops here. Without it a truncated log reads like a crash.
        DIAG_CAPPED.store(true, Ordering::Relaxed);
        let _ = writeln!(
            file,
            "---- log capped at {} KB; nothing further is recorded this run ----",
            DIAG_MAX_BYTES / 1024
        );
        let _ = file.flush();
        return;
    }
    // WHICH THREAD is worth the characters: three of them write here (the host's, the monitor's, the capture's)
    // and the monitor sleeps 20 ms between looks, so interleaved lines are otherwise hard to attribute.
    let thread = std::thread::current();
    let name = thread.name().unwrap_or("host");
    let line = format!("{} [{name}] {message}", local_time_stamp());
    let _ = writeln!(file, "{line}");
    let _ = file.flush();
    DIAG_BYTES.fetch_add(line.len() as u64 + 1, Ordering::Relaxed);
}

/// `HH:MM:SS.mmm`, local. Milliseconds because the question this log answers is "what happened in the 40 ms
/// around that click", which a coarser stamp would not resolve.
fn local_time_stamp() -> String {
    let mut now: SYSTEMTIME = unsafe { mem::zeroed() };
    unsafe {
        GetLocalTime(&mut now);
    }
    format!(
        "{:02}:{:02}:{:02}.{:03}",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds
    )
}

/// RECORD WHAT THE LAST CAPTURE SAW, in the feature's own folder.
///
/// ⚠️ IT IS WRITTEN FOR A HUMAN, AND NOTHING IN APEX READS IT. That is stated rather than left to be discovered,
/// because the alternative reading -- "the panel reads this to show something" -- is the one a reader would reach
/// for, and it is false: the panel reads a feature's CONTROLS and nothing else. (There used to be a one-line
/// read-out a feature could publish; it is gone from the ABI -- 9 -> 10 -- see apex/abi.h.)
///
/// The ported version had BOTH: this file AND an in-process `observed_info()` that read it back, with a comment
/// about a background daemon -- and NOTHING CALLED THE READER. A copy that nothing reads is worse than no copy,
/// because it makes the next reader think the data flows somewhere.
///
/// ⚠️ THE REASON TO KEEP THE WRITE: this feature's decisions are about a control that existed for a moment under
/// the user's cursor, and when a rule does not fire the first question is "what did it actually see". That answer
/// survives a restart here, in a file next to the rules, in the shape of the model itself.
fn set_observed_info(info: WindowInfo) {
    if let Ok(text) = serde_json::to_string(&info) {
        let _ = std::fs::write(observed_file(), text);
    }
}

fn observed_file() -> std::path::PathBuf {
    config_path().with_file_name("observed.json")
}

#[derive(Clone, Copy, Default)]
struct ThreadIme {
    current: Option<bool>,
    baseline: Option<bool>,
    overridden: bool,
}

fn thread_ime_states() -> &'static Mutex<HashMap<u32, ThreadIme>> {
    static STATES: OnceLock<Mutex<HashMap<u32, ThreadIme>>> = OnceLock::new();
    STATES.get_or_init(|| Mutex::new(HashMap::new()))
}

fn get_thread_ime(thread_id: u32) -> ThreadIme {
    thread_ime_states()
        .lock()
        .ok()
        .and_then(|states| states.get(&thread_id).copied())
        .unwrap_or_default()
}

fn set_baseline_ime(thread_id: u32, open: bool) {
    if let Ok(mut states) = thread_ime_states().lock() {
        states.entry(thread_id).or_default().baseline = Some(open);
    }
}

fn set_current_ime(thread_id: u32, open: bool) {
    if let Ok(mut states) = thread_ime_states().lock() {
        states.entry(thread_id).or_default().current = Some(open);
    }
}

fn set_overridden(thread_id: u32, overridden: bool) {
    if let Ok(mut states) = thread_ime_states().lock() {
        states.entry(thread_id).or_default().overridden = overridden;
    }
}

fn thread_exists(thread_id: u32) -> bool {
    let handle = unsafe { OpenThread(THREAD_QUERY_LIMITED_INFORMATION, 0, thread_id) };
    if handle == 0 {
        false
    } else {
        unsafe {
            CloseHandle(handle);
        }
        true
    }
}

fn prune_dead_threads() {
    if let Ok(mut states) = thread_ime_states().lock() {
        let dead: Vec<u32> = states
            .keys()
            .filter(|&&thread_id| !thread_exists(thread_id))
            .copied()
            .collect();
        for thread_id in dead {
            states.remove(&thread_id);
        }
    }
}

/// THE INPUT METHOD'S OPEN STATE, read from the IME window of the window that owns the keyboard focus.
///
/// ⚠️ `open`, NOT the conversion mode. `IMC_GETCONVERSIONMODE` keeps reporting the last conversion mode even
/// while the IME is CLOSED (measured here: `conv=1 open=0` for a control typing English), so it says "was
/// Chinese once"; the open status is what the user is actually typing with.
///
/// ⚠️ AND NOTHING IS LOGGED HERE ANY MORE. This is called on every evaluation AND again for the readout, so a
/// line here was the 50-writes-a-second problem (see the note above `DIAG_FILE`). The value is not lost: the
/// decision lines print it as `read=Some(..)`, and the panel's readout shows it as 输入法现在.
fn read_ime_chinese(hwnd: HWND) -> Option<bool> {
    unsafe {
        let ime_wnd = ImmGetDefaultIMEWnd(hwnd);
        if ime_wnd == 0 {
            return None;
        }
        let open = SendMessageW(ime_wnd, WM_IME_CONTROL, IMC_GETOPENSTATUS, 0);
        Some(open != 0)
    }
}

fn set_ime_chinese(hwnd: HWND, chinese: bool) -> bool {
    unsafe {
        let ime_wnd = ImmGetDefaultIMEWnd(hwnd);
        if ime_wnd == 0 {
            return false;
        }
        let lparam: LPARAM = if chinese { 1 } else { 0 };
        let _ = SendMessageW(ime_wnd, WM_IME_CONTROL, IMC_SETOPENSTATUS, lparam);
        true
    }
}

pub fn run_monitor(config: Arc<Mutex<AppConfig>>) {
    // ⚠️ WHOSE WINDOWS TO IGNORE -- AND THE ANSWER IS "EVERYTHING RUNNING OUT OF APEX'S OWN FOLDER", NOT
    // "apex.exe". The distinction is the whole of a bug that shipped: the guard below used to compare PROCESS
    // NAMES against the host's, and Apex is TWO programs in one folder (apex.exe, apex-settings.exe -- see
    // AGENTS.md §一), so the settings panel went through this check as if it were any other program and a rule
    // that matched it flipped the input method of the page the user was writing the rule on. MEASURED, in this
    // feature's own log: `SWITCH rule=... proc=apex-settings.exe`.
    //
    // The judgement lives in `model::is_apex_itself` because it is a fact about PATHS rather than about windows:
    // it needs no screen, so it is a pure function with tests, and the folder it compares against is the same
    // "this installation" the host itself uses (settings_ipc.h, SameInstallDirectory).
    let own_name = crate::own_exe_name();
    let root = install_root();

    // ⚠️⚠️ THE FOCUS HOOK HAS TO BE INSTALLED HERE, AND FOR A LONG TIME IT NEVER WAS.
    //
    // `install_focus_hook` was ported and then called by nobody: upstream's daemon calls it at start-up
    // (daemon.rs), and in the port that line had no equivalent, so `FOCUS_EVENT_TX` was never populated, the
    // hook was never registered, and `focus_rx` below waited on a channel nothing could ever send to. The
    // symptom is silent and narrow -- switching to a program with the KEYBOARD (Tab, Alt+Tab, a shortcut) does
    // not re-evaluate the rules; only a mouse click does -- and nothing anywhere reported it, because the
    // monitor loop simply never saw an event.
    //
        // ⚠️ IT MUST BE INSTALLED FROM THIS THREAD rather than from `fe_init` on the host's own message thread:
        // `WINEVENT_OUTOFCONTEXT` delivers the callback to the thread that installed the hook, and the host's
        // thread is busy with the wheel -- a callback doing a UI Automation walk there would land in the input
        // path. (Whether this thread's loop is enough to have the events DELIVERED is an open question, stated
        // where the hook is defined; it is not assumed here.)
        install_focus_hook();

    // 键盘焦点变化（WinEvent hook）的事件通道：Tab / 方向键 / Alt+Tab 等。
    let focus_rx = take_focus_change_receiver();

    let shutdown = crate::shutdown_flag();
    let mut prev_lbtn_down = false;
    let mut last_prune = Instant::now();

    loop {
        std::thread::sleep(Duration::from_millis(20));

        // ⚠️ THE SWITCH IS CHECKED HERE, AND WITHOUT IT THE FEATURE CANNOT BE TURNED OFF. The host's switch
        // works by not CALLING a feature that is off -- complete for a feature that only acts when called, and
        // useless for one with its own thread. So the thread asks (ApexHost::featureEnabled) and does nothing
        // while the answer is no. It keeps running and keeps its IME state: stopping the thread and starting
        // it again would throw away the per-thread baselines, so a feature switched off and on again would
        // lose track of where each program's IME was.
        if !crate::feature_is_enabled() {
            prev_lbtn_down = false; // so that enabling mid-click does not read as a fresh click
            continue;
        }

        // THE HOST ASKED US TO STOP: the feature is being unloaded (or Apex is exiting), so the loop ends
        // and the thread returns. It is a flag rather than a kill so the thread can finish its current step --
        // it may be inside a SendMessage to another process's IME window.
        if shutdown.load(Ordering::Acquire) {
            crate::LEFT_THE_LOOP.store(true, Ordering::Release);
            crate::log("AutoIME: the monitor thread is stopping");
            return;
        }

        if last_prune.elapsed() >= Duration::from_secs(5) {
            last_prune = Instant::now();
            prune_dead_threads();
        }

        // 键盘焦点事件到达后，稍等目标窗口完成焦点转移再抓取。
        let focus_pending = match focus_rx.as_ref().and_then(|rx| rx.try_recv().ok()) {
            Some(()) => {
                std::thread::sleep(Duration::from_millis(40));
                true
            }
            None => false,
        };

        let lbtn_down = (unsafe { GetAsyncKeyState(VK_LBUTTON as i32) } as u16 & 0x8000) != 0;
        let clicked = lbtn_down && !prev_lbtn_down;
        prev_lbtn_down = lbtn_down;

        if !clicked && !focus_pending {
            continue;
        }
        if capture_armed() {
            continue;
        }

        let cfg = match config.lock() {
            Ok(guard) => guard.clone(),
            Err(_) => continue,
        };

        // 键盘切换时抓取真正的焦点控件；鼠标点击仍抓取点击位置下的控件。
        if clicked {
            // 点击后稍等前台窗口切换，再抓取点击位置下的控件用于规则匹配。
            std::thread::sleep(Duration::from_millis(40));
        }

        // 两种触发共用的评估流程。
        let active = if clicked {
            match capture_from_cursor() {
                Some(active) => active,
                None => continue,
            }
        } else {
            match capture_from_focus() {
                Some(active) => active,
                None => continue,
            }
        };

        // APEX'S OWN WINDOWS ARE NOT A TARGET -- the host's, and the settings panel's. See the note at the top
        // of this function for what went wrong when this compared exe names.
        if is_apex_itself(&active, root, &own_name) {
            continue;
        }
        set_observed_info(active.clone());

        // 鼠标点击时要求点击的窗口确实成为前台窗口（过滤点击任务栏等未激活场景）；
        // 键盘焦点路径本身就以焦点控件为准，无需此检查。
        if clicked && unsafe { GetForegroundWindow() } != active.window_hwnd as HWND {
            continue;
        }

        // ⚠️ THE WHOLE RULE IS KEPT, NOT JUST ITS ID AND ACTION, because the readout shows WHICH CONDITIONS
        // decided it and what values they saw. Keeping only the id meant a second lookup later, from a thread
        // that would have to borrow the config again -- and the readout would then show whatever the rule says
        // NOW rather than what it said when it fired.
        let mut matched_rule: Option<crate::model::Rule> = None;

        for rule in cfg.active_rules() {
            if rule.matches(&active, &active) {
                matched_rule = Some(rule.clone());
                break;
            }
        }
        let matched = matched_rule.as_ref().map(|r| (r.id.clone(), r.action));

        // IME 状态以真正拥有键盘焦点的控件为准，避免下拉菜单/遮罩等临时窗口干扰。
        let focus_hwnd = focused_hwnd().unwrap_or(active.control_hwnd as HWND);
        evaluate_and_switch(&cfg, &active, matched.as_ref(), focus_hwnd);

        // (The readout that used to be published here -- `active.live_summary(...)` with the input method's state
        // read a second time for it -- is gone: see the note where it used to be defined. What it cost was a
        // second `SendMessage` into the target's IME window on every decision, for a box the user has removed.)
    }
}

/// 根据命中结果执行切换或恢复（鼠标点击与键盘焦点共用）。
fn evaluate_and_switch(
    cfg: &AppConfig,
    active: &WindowInfo,
    matched: Option<&(String, Action)>,
    focus_hwnd: HWND,
) {
    let mut pid: DWORD = 0;
    let thread_id = unsafe { GetWindowThreadProcessId(focus_hwnd, &mut pid) };
    if thread_id == 0 {
        return;
    }

    let read_chinese = read_ime_chinese(focus_hwnd);

    match matched {
        Some((_id, action)) => {
            let desired_chinese = matches!(action, Action::Chinese);
            let state = get_thread_ime(thread_id);
            let actual = read_chinese.or(state.current).unwrap_or(false);

            if !state.overridden {
                set_baseline_ime(thread_id, actual);
            }

            let need_toggle = actual != desired_chinese;
            diag(&format!(
                "SWITCH rule={_id} desired={desired_chinese} thread={thread_id} actual={actual} read={read_chinese:?} baseline={:?} need={need_toggle} proc={}",
                state.baseline,
                active.process_name
            ));
            if need_toggle {
                apply_switch(cfg, focus_hwnd, desired_chinese);
                set_current_ime(thread_id, desired_chinese);
            } else if read_chinese.is_some() {
                set_current_ime(thread_id, actual);
            }
            set_overridden(thread_id, true);
        }
        None => {
            let state = get_thread_ime(thread_id);
            if state.overridden {
                let actual = read_chinese.or(state.current).unwrap_or(false);
                diag(&format!(
                    "RESTORE thread={thread_id} actual={actual} baseline={:?} read={read_chinese:?} proc={}",
                    state.baseline, active.process_name
                ));
                if let Some(baseline) = state.baseline {
                    if actual != baseline {
                        apply_switch(cfg, focus_hwnd, baseline);
                        set_current_ime(thread_id, baseline);
                    }
                }
                set_overridden(thread_id, false);
            } else if let Some(actual) = read_chinese {
                // 未命中规则且未覆盖：持续把真实状态同步为基线，捕获用户手动切换。
                set_baseline_ime(thread_id, actual);
                set_current_ime(thread_id, actual);
            }
        }
    }
}

fn apply_switch(cfg: &AppConfig, hwnd: HWND, chinese: bool) {
    match cfg.switch_method {
        SwitchMethod::Simulate => {
            let _ = simulate_combo(&cfg.ime_toggle_hotkey);
        }
        SwitchMethod::Ime => {
            let _ = set_ime_chinese(hwnd, chinese);
        }
    }
}

fn simulate_combo(combo: &str) -> Result<(), String> {
    let (modifiers, key) =
        parse_combo(combo).ok_or_else(|| format!("无法解析快捷键：{combo}"))?;

    unsafe {
        for &modifier in &modifiers {
            keybd_event(modifier, 0, 0, 0);
        }
        keybd_event(key, 0, 0, 0);
        std::thread::sleep(Duration::from_millis(30));
        keybd_event(key, 0, KEYEVENTF_KEYUP, 0);
        for &modifier in modifiers.iter().rev() {
            keybd_event(modifier, 0, KEYEVENTF_KEYUP, 0);
        }
    }

    Ok(())
}

/// MAY THIS COMBINATION BE USED AS A TOGGLE HOTKEY? -- asked by the settings page before it is stored.
///
/// ⚠️ ONE GRAMMAR, ONE PLACE, AND THAT IS THE WHOLE REASON THIS FUNCTION EXISTS RATHER THAN A SECOND PARSER IN
/// settings.rs. The parser below is the one that SENDS the keys (`simulate_combo`); asking it whether a
/// combination is usable is the only way "what the user recorded" and "what can be pressed" cannot disagree.
/// (The settings module used to say, in a comment, that validating there would need the grammar twice -- and a
/// second copy of a grammar is how the two come to disagree about what is valid. This is that comment's answer.)
///
/// ⚠️ A MODIFIER IS REQUIRED, AND THAT IS ABOUT WHAT THE COMBINATION DOES RATHER THAN ABOUT GRAMMAR: this one is
/// pressed to toggle the input method. A bare `A` parses fine and would be sent fine -- and would then fire on
/// every letter the user types.
pub(crate) fn combo_is_usable(combo: &str) -> bool {
    match parse_combo(combo) {
        Some((modifiers, _key)) => !modifiers.is_empty(),
        None => false,
    }
}

fn parse_combo(combo: &str) -> Option<(Vec<u8>, u8)> {
    let mut modifiers = Vec::new();
    let mut key = None;

    for part in combo.split('+').map(str::trim) {
        if part.is_empty() {
            continue;
        }
        match part.to_ascii_uppercase().as_str() {
            "CTRL" | "CONTROL" => modifiers.push(VK_CONTROL),
            "ALT" => modifiers.push(VK_MENU),
            "SHIFT" => modifiers.push(VK_SHIFT),
            "WIN" | "WINDOWS" | "SUPER" => modifiers.push(VK_LWIN),
            other => {
                if key.is_some() {
                    return None;
                }
                key = Some(key_to_vk(other)?);
            }
        }
    }

    let key = key?;
    modifiers.sort_unstable();
    modifiers.dedup();
    Some((modifiers, key))
}

fn key_to_vk(token: &str) -> Option<u8> {
    let upper = token.to_ascii_uppercase();
    let vk = match upper.as_str() {
        "SPACE" => VK_SPACE,
        "TAB" => 0x09,
        "ENTER" | "RETURN" => 0x0D,
        "ESC" | "ESCAPE" => 0x1B,
        "BACKSPACE" => 0x08,
        "`" | "~" => 0xC0,
        "-" => 0xBD,
        "=" => 0xBB,
        "[" => 0xDB,
        "]" => 0xDD,
        "\\" => 0xDC,
        ";" => 0xBA,
        "'" => 0xDE,
        "," => 0xBC,
        "." => 0xBE,
        "/" => 0xBF,
        _ => {
            if upper.len() == 1 {
                let ch = upper.as_bytes()[0];
                if ch.is_ascii_alphanumeric() {
                    return Some(ch);
                }
            }
            if upper.starts_with('F') && upper.len() >= 2 && upper.len() <= 3 {
                if let Ok(number) = upper[1..].parse::<u8>() {
                    if (1..=24).contains(&number) {
                        return Some(0x6F + number);
                    }
                }
            }
            return None;
        }
    };
    Some(vk)
}



fn capture_from_point(pt: POINT) -> Option<WindowInfo> {
    let hwnd = unsafe { WindowFromPoint(pt) };
    if hwnd == 0 {
        return None;
    }
    let mut root = unsafe { GetAncestor(hwnd, GA_ROOT) };
    if root == 0 {
        root = hwnd;
    }
    let control = deepest_child_from_point(root, pt, 0);
    let mut info = build_info(root, control);
    info.click_x = pt.x;
    info.click_y = pt.y;
    if let Some(uia) = uia_from_point(pt) {
        merge_uia_control(&mut info, &uia);
    }
    if capture_armed() {
        diag(&format!("CAPTURE {}", info.summary()));
    }
    Some(info)
}

/// 从指定控件句柄构建完整信息：以该控件为匹配对象（键盘焦点场景）。
fn capture_from_hwnd(hwnd: HWND) -> Option<WindowInfo> {
    if hwnd == 0 {
        return None;
    }
    let mut root = unsafe { GetAncestor(hwnd, GA_ROOT) };
    if root == 0 {
        root = hwnd;
    }
    let mut info = build_info(root, hwnd);
    if let Some(uia) = uia_from_hwnd(hwnd) {
        merge_uia_control(&mut info, &uia);
    }
    if capture_armed() {
        diag(&format!("CAPTURE_FOCUS {}", info.summary()));
    }
    Some(info)
}

fn focused_hwnd() -> Option<HWND> {
    let foreground = unsafe { GetForegroundWindow() };
    if foreground == 0 {
        return None;
    }

    let mut pid: DWORD = 0;
    let thread_id = unsafe { GetWindowThreadProcessId(foreground, &mut pid) };
    if thread_id == 0 {
        return None;
    }

    let mut gui: GUITHREADINFO = unsafe { mem::zeroed() };
    gui.cbSize = mem::size_of::<GUITHREADINFO>() as DWORD;
    if unsafe { GetGUIThreadInfo(thread_id, &mut gui) } == 0 {
        return None;
    }

    Some(if gui.hwndFocus != 0 {
        gui.hwndFocus
    } else {
        foreground
    })
}

fn merge_uia_control(info: &mut WindowInfo, uia: &UiaControl) {
    // 容器类（Pane/Window/Group 等）常因 UIA 命中桌面或外层容器，反而覆盖掉
    // Win32 层正确抓到的控件文本/类名，因此这些情况下保留 Win32 结果。
    let is_container = matches!(
        uia.control_type.as_str(),
        "Pane"
            | "Window"
            | "Group"
            | "Tab"
            | "TabItem"
            | "ToolBar"
            | "MenuBar"
            | "Menu"
            | "StatusBar"
            | "TitleBar"
            | "Header"
            | "ToolTip"
    );
    if !is_container {
        if !uia.name.is_empty() {
            info.control_text = uia.name.clone();
        }
        if !uia.class.is_empty() {
            info.control_class = uia.class.clone();
        }
    }
    info.control_type = uia.control_type.clone();
    info.automation_id = uia.automation_id.clone();
    info.container_text = uia.container_text.clone();
    info.ancestor_texts = uia.ancestor_texts.clone();
    info.ancestor_classes = uia.ancestor_classes.clone();
}

fn deepest_child_from_point(root: HWND, pt: POINT, depth: u32) -> HWND {
    if depth > 48 {
        return root;
    }
    let mut client = pt;
    if unsafe { ScreenToClient(root, &mut client) } == 0 {
        return root;
    }
    let child = unsafe {
        ChildWindowFromPointEx(
            root,
            client,
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT,
        )
    };
    if child != 0 && child != root {
        deepest_child_from_point(child, pt, depth + 1)
    } else {
        root
    }
}

fn build_info(window_hwnd: HWND, control_hwnd: HWND) -> WindowInfo {
    let pid = window_process_id(window_hwnd);
    WindowInfo {
        process_name: process_name(pid),
        process_path: process_path(pid),
        window_title: window_text(window_hwnd),
        window_class: class_name(window_hwnd),
        control_text: window_text(control_hwnd),
        control_class: class_name(control_hwnd),
        control_type: String::new(),
        automation_id: String::new(),
        container_text: String::new(),
        ancestor_texts: Vec::new(),
        ancestor_classes: Vec::new(),
        window_hwnd: window_hwnd as usize,
        control_hwnd: control_hwnd as usize,
        click_x: 0,
        click_y: 0,
    }
}

fn window_process_id(hwnd: HWND) -> DWORD {
    let mut pid: DWORD = 0;
    unsafe {
        GetWindowThreadProcessId(hwnd, &mut pid);
    }
    pid
}

fn window_text(hwnd: HWND) -> String {
    let mut buf = [0u16; 1024];
    let len = unsafe { GetWindowTextW(hwnd, buf.as_mut_ptr(), buf.len() as i32) };
    if len <= 0 {
        String::new()
    } else {
        String::from_utf16_lossy(&buf[..len as usize])
    }
}

fn class_name(hwnd: HWND) -> String {
    let mut buf = [0u16; 512];
    let len = unsafe { GetClassNameW(hwnd, buf.as_mut_ptr(), buf.len() as i32) };
    if len <= 0 {
        String::new()
    } else {
        String::from_utf16_lossy(&buf[..len as usize])
    }
}

fn process_name(pid: DWORD) -> String {
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if snapshot == 0 || snapshot == INVALID_HANDLE_VALUE {
        return String::new();
    }

    let mut entry: PROCESSENTRY32W = unsafe { mem::zeroed() };
    entry.dwSize = mem::size_of::<PROCESSENTRY32W>() as DWORD;

    let mut found = String::new();
    if unsafe { Process32FirstW(snapshot, &mut entry) } != 0 {
        loop {
            if entry.th32ProcessID == pid {
                found = String::from_utf16_lossy(&entry.szExeFile)
                    .trim_end_matches('\0')
                    .to_string();
                break;
            }
            if unsafe { Process32NextW(snapshot, &mut entry) } == 0 {
                break;
            }
        }
    }
    unsafe {
        CloseHandle(snapshot);
    }
    found
}

fn process_path(pid: DWORD) -> String {
    let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
    if handle != 0 {
        let mut buf = [0u16; 2048];
        let mut size = buf.len() as DWORD;
        let ok = unsafe { QueryFullProcessImageNameW(handle, 0, buf.as_mut_ptr(), &mut size) };
        unsafe {
            CloseHandle(handle);
        }
        if ok != 0 && size > 0 {
            return String::from_utf16_lossy(&buf[..size as usize]);
        }
    }
    process_name(pid)
}

/// THE PROCESS THIS FEATURE IS RUNNING INSIDE -- apex.exe.
///
/// ⚠️ IT EXISTS BECAUSE `std::env::current_exe()` CANNOT BE USED ANY MORE, and that is a porting trap worth
/// naming: it answers the same thing here (the host's exe) but it is the WRONG QUESTION in both projects for
/// different reasons. Upstream asks "which exe is me" to ignore its own windows; this asks "which exe is the
/// host" to ignore Apex's. Same value, different meaning -- and a reader who sees `current_exe()` in a DLL
/// would not know which of the two was meant.
pub fn own_process_name() -> String {
    process_name(unsafe { windows::Win32::System::Threading::GetCurrentProcessId() })
}

// ---------------------------------------------------------------------------
// ⚠️ THE LIVE READOUT USED TO LIVE HERE -- the panel's "status" box at the top of the feature's page, fed with a
// paragraph about the focused control, the matched rule and the input method's state. THE USER REMOVED IT:
// "自动输入法页，最上方的状态提示去了."
//
// What is worth keeping is WHY the last piece of it had to stop being a readout at all. The capture thread
// published a line here ("已捕获: …") and the PAGE used the change in that text as its signal that a capture had
// finished -- so the page's refresh depended on an invisible string, and the box that displayed it was the only
// reason the string existed. The page now watches the CONTROLS document instead (see `watchForCapture` in
// panel.html): the thing it wants to redraw is the thing it polls, which is the same signal and no display.
//
// The ABI used to allow a feature to publish a readout (`liveText`); this feature gave none, KeepAwake gave one
// and the user removed that too ("其实这个提示可以完全去掉。并不需要"), so the field is out of the ABI entirely
// (9 -> 10, apex/abi.h). Nothing in the program draws a feature's prose above its page any more.
// ---------------------------------------------------------------------------
