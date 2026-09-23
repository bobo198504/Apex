//! THE FEATURE'S SETTINGS PAGE: the rules, described for a panel that knows nothing about them.
//!
//! WHAT THIS FILE IS. The panel's vocabulary is `range`, `bool`, `list`, `select`, `text` and `group`
//! (apex/abi.h). A rule is a `group` -- a repeatable block of fields -- so this file describes one field for
//! each thing a user can set, and applies what comes back.
//!
//! ⚠️ IT DOES NOT KNOW WHAT A PANEL IS. Nothing here formats markup or touches a window; it builds a document
//! and edits a struct. That is what makes the rules testable without a screen (see tests.rs), and it is the
//! same split the C++ features use.
//!
//! ⚠️⚠️ THE MOST IMPORTANT DECISION IN THIS FILE: THERE IS NO CHECKBOX PER CONDITION.
//!
//! Upstream had two fields per condition -- `use_process` (a checkbox) and `process_pattern` (the text) -- and
//! showed both. Reading the engine says they are REDUNDANT: `push_condition` RETURNS EARLY ON AN EMPTY PATTERN
//! (see model.rs), so a condition participates exactly when its pattern is non-empty, whether or not the box is
//! ticked. The checkbox's only unique power was "keep a pattern but do not use it", and it paid for that with
//! nine extra controls per rule in every editor.
//!
//! So the page shows ONE FIELD PER CONDITION -- the pattern -- and this file keeps the two representations in
//! step: a non-empty pattern turns its `use_*` flag on, and clearing it turns the flag off. The STRUCT keeps
//! every field (the file format is unchanged, and a hand-written config.json loads exactly as before); what
//! disappears is a control that could silently contradict the field beside it.
//!
//! ⚠️ `match_target` IS NOT SHOWN EITHER, for the same kind of reason: `AppConfig::migrate()` collapses every
//! value to `ActiveControl` on load, so offering the choice would be offering a setting the program undoes.

use std::os::raw::{c_char, c_int};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};

use crate::log;
use crate::model::{self, Action, AppConfig, MatchMode, Rule};

/// The live configuration: the SAME `Arc` the monitor thread holds (see `fe_init`).
///
/// ⚠️ A MUTEX AROUND AN `Option`, NOT A `OnceLock`. The first version was a OnceLock and it was WRONG in a way
/// that shows up twice: `OnceLock::set` succeeds only ONCE per process, so
///   * a host that UNLOADS and RELOADS a feature (which Apex can do) would have its second init silently
///     ignored -- the feature would run on the previous settings for the rest of the session, and nothing
///     would say so;
///   * every test in settings_tests shares one process, so only the first could install its fixture and the
///     rest ran against THAT one. (That is how it was found -- five tests failing together, all reporting the
///     wrong rule as if the code were broken.)
/// An `Option` inside the lock is the same thing with a writer, and there is exactly one reader path
/// (`with_config`), so nothing else changes.
static CONFIG: Mutex<Option<Arc<Mutex<AppConfig>>>> = Mutex::new(None);

/// THE RULE BEING EDITED, AS A COPY -- the standalone program's `draft`, moved here from the page.
///
/// ⚠️⚠️ WHY IT BELONGS IN THE FEATURE AND NOT IN THE PAGE, which is where it was first put.
///
/// The page had a draft and the feature had the file, and everything that edits a rule had to decide which of
/// the two it was talking to. Most things could: a field edit went to the page's draft, and Save handed the
/// fields over. CAPTURE could not. A capture is produced HERE -- it is this code that waits for the click and
/// reads the control under the cursor -- so it wrote straight into the config and the file while the page was
/// still drawing its own draft. The user's report: "捕获没有进到规则列表的选项框中，数据没进去."
///
/// The standalone never had that seam: `apply_capture(&mut cfg, info)` takes the DRAFT (`editing_config()`),
/// and Save is what publishes it. Putting the draft on this side makes the same sentence true here -- every
/// edit, including a capture, lands in one place, and the page just draws whatever document it is handed.
///
/// It is `(real index, rule)`: the REAL index, not the page's display position, because the display order is
/// derived from priorities and can change under an edit (see AppConfig::real_index_of_display).
static DRAFT: Mutex<Option<Draft>> = Mutex::new(None);

/// ONE OPEN EDIT: which rule, what it looks like so far, and whether it EXISTS outside this edit.
///
/// ⚠️⚠️ `fresh` IS THE DIFFERENCE BETWEEN ABANDONING AN EDIT AND ABANDONING A RULE, and the user asked for the
/// distinction: "自动输入法插件有个交互优化下：新建规则后，没点保存，切到其它规则后不保留" -- answered with
/// "没点保存就切走 = 等于没建（不保留）".
///
/// A rule that was already saved has a version to fall back to, so dropping the draft loses only the typing. A
/// rule that `add` has just created has NO version to fall back to: keeping it after the draft is gone would
/// leave an empty, switched-off "新规则" in the list and in config.json -- a row the user never finished and now
/// has to delete by hand. So a fresh draft is rolled back (`cancel-edit`) and is never written to the file.
#[derive(Clone)]
struct Draft {
    real: usize,
    rule: Rule,
    fresh: bool,
}

/// "I AM STILL WAITING FOR SOMETHING OUTSIDE THIS PANEL" -- reported to the page as the group's `waiting` field.
///
/// ⚠️⚠️ WHY THE FEATURE HAS TO SAY THIS. The panel cannot know when a capture ends; it can only ask. Its first
/// version asked four times a second and decided the wait was over when the DOCUMENT CHANGED -- but the document
/// also changes for reasons that have nothing to do with a capture (the host answers every `listOp` with a
/// snapshot, and the page re-reads after it). So the page stopped watching early and the captured fields stayed
/// invisible until something else re-read the controls, which the user reported as: "捕获事件进行时，鼠标点击后
/// 结果要马上给到参数页，目前没有，要等到点击新建的规则条才会出现". The other direction is just as broken: a
/// capture that fills in what was already there changes nothing, and the old page would poll for ever.
///
/// So this flag is the fact the guess was standing in for, and it is cleared LAST -- see the note where the
/// capture's thread does it. Clearing it before the draft holds the answer would open the same race again, one
/// layer down: the page stops asking the moment it is gone.
static CAPTURE_WAITING: AtomicBool = AtomicBool::new(false);

/// ARM OR DISARM THE CAPTURE *AND* TELL THE PAGE, in one call, so the two cannot drift apart: the Win32 event
/// is what the monitor thread reads (it skips click handling while a capture is armed) and this atomic is what
/// `settings_json` publishes.
///
/// ⚠️ THE TWO HALVES ARE SET TOGETHER BUT CLEARED SEPARATELY -- the click handler clears the Win32 event first
/// and calls this with `false` only once the rule is filled in. See `capture` below.
fn set_capture_waiting(waiting: bool) {
    crate::win32::set_capture_armed(waiting);
    CAPTURE_WAITING.store(waiting, Ordering::SeqCst);
}

/// The page reads this through the controls document; tests set it without touching the named event, which is
/// process-wide and would arm a capture in a RUNNING Apex if a test ever flipped it.
#[cfg(test)]
pub fn debug_set_capture_waiting(waiting: bool) {
    CAPTURE_WAITING.store(waiting, Ordering::SeqCst);
}

fn draft_get() -> Option<Draft> {
    DRAFT.lock().ok().and_then(|g| g.clone())
}
fn draft_set(d: Option<Draft>) {
    if let Ok(mut g) = DRAFT.lock() {
        *g = d;
    }
}

/// Is a draft open for this REAL index? Used by every writer to decide where an edit goes.
fn draft_holds(real: usize) -> bool {
    matches!(draft_get(), Some(d) if d.real == real)
}

/// THE REAL INDEX OF THE RULE AN UNCOMMITTED `add` IS HOLDING, if there is one.
///
/// ⚠️ FOUND BY ID, NOT BY POSITION. The index is what moves -- a reorder, a remove, or a capture that renames
/// the rule can all renumber the list under an open draft -- and asking "is the rule at position 3 the fresh
/// one" would then be a question about a different rule. The id is minted once (`fresh_id`) and nothing changes
/// it, so it is the only thing that answers this safely.
fn fresh_index(c: &AppConfig) -> Option<usize> {
    let d = draft_get()?;
    if !d.fresh {
        return None;
    }
    c.rules.iter().position(|r| r.id == d.rule.id)
}

/// MOVE A RULE, AND TAKE THE OPEN DRAFT WITH IT.
///
/// ⚠️⚠️ WHY THIS WRAPPER EXISTS, AND WHY IT IS NOT OPTIONAL. The draft is keyed by the rule's POSITION (its real
/// index), and since the list order IS the file order (see `display_order`) a move changes positions -- so a
/// draft left pointing at its old number would, from the next read on, be a draft for whatever rule slid into
/// that slot. The user would then be typing into a rule they never opened, and Save would write it.
///
/// (Before the order became the file order, a move only renumbered PRIORITIES and the positions never changed,
/// which is why this could not happen -- see the note that used to stand on `move_display_item`.)
///
/// The arithmetic is the one a remove-and-insert always has: the rule that moved lands on `to`, everything
/// between shifts by one towards the hole it left.
fn move_rule(c: &mut AppConfig, from: usize, to: usize) -> bool {
    if !c.move_display_item(from, to) {
        return false;
    }
    if let Some(mut d) = draft_get() {
        let moved = shifted_index(d.real, from, to);
        if moved != d.real {
            d.real = moved;
            draft_set(Some(d));
        }
    }
    true
}

/// Where the rule at index `at` ends up after the rule at `from` is moved to `to`.
fn shifted_index(at: usize, from: usize, to: usize) -> usize {
    if at == from {
        to
    } else if from < at && at <= to {
        at - 1          // it was after the hole and inside the span: everything moved up one
    } else if to <= at && at < from {
        at + 1          // it was before the hole and inside the span: everything moved down one
    } else {
        at
    }
}

pub fn set_config(c: Arc<Mutex<AppConfig>>) {
    if let Ok(mut slot) = CONFIG.lock() {
        *slot = Some(c);
    }
}

fn clone_config() -> Option<Arc<Mutex<AppConfig>>> {
    CONFIG.lock().ok().and_then(|g| g.clone())
}

fn with_config<R>(f: impl FnOnce(&mut AppConfig) -> R) -> Option<R> {
    // A poisoned lock means the monitor thread panicked while holding it. Rather than let that panic cross the
    // C ABI (which would abort the host), the edit is dropped: the page re-reads afterwards and shows the
    // unchanged value, so the failure is visible exactly where it matters.
    let arc = clone_config()?;
    let mut guard = arc.lock().ok()?;
    Some(f(&mut guard))
}

// ---------------------------------------------------------------------------
// THE DOCUMENT
// ---------------------------------------------------------------------------

/// The fields of ONE rule. Written once, so the editor and `set_rule_field` cannot drift apart.
fn rule_fields() -> Value {
    // (field id, the `use_*` flag that governs it, Chinese label, English label, an example for the box).
    //
    // ⚠️ THE FLAG IS BACK, AS A CHECKBOX BESIDE THE FIELD: "各参数原先是有个复选框，打钩了才生效，新规则默认只有进程名
    // 是钩起来的." The port had dropped it, arguing that an empty pattern already means "do not test this" -- which
    // is true for MATCHING, and misses what the user wanted the box for: KEEPING a pattern while switching the
    // condition off. Without it the only way to stop testing a value is to delete the value, and a captured
    // pattern (the most valuable thing in a rule) is exactly the thing you would have to delete to try that.
    //
    // The box travels with its field as ONE row (`toggle`, see apex/abi.h) because a condition and its "use it"
    // switch are one decision -- two rows for it would double the height of every rule.
    let conds: [(&str, &str, &str, &str, &str, &str); 9] = [
        ("process_pattern", "use_process", "进程名", "Process", "例如 notepad.exe", "e.g. notepad.exe"),
        ("window_title_pattern", "use_window_title", "窗口标题", "Window title", "支持 * 和 ?", "* and ? allowed"),
        ("window_class_pattern", "use_window_class", "窗口类名", "Window class", "例如 CabinetWClass", "e.g. CabinetWClass"),
        ("control_text_pattern", "use_control_text", "控件文本", "Control text", "支持 * 和 ?", "* and ? allowed"),
        ("control_class_pattern", "use_control_class", "控件类名", "Control class", "例如 Edit", "e.g. Edit"),
        ("control_type_pattern", "use_control_type", "控件类型", "Control type", "例如 Button", "e.g. Button"),
        ("automation_id_pattern", "use_automation_id", "自动化 ID", "Automation ID", "例如 StartButton", "e.g. StartButton"),
        ("container_text_pattern", "use_container_text", "父级标签", "Parent label", "父级里的文字", "text on a parent"),
        ("ancestor_class_pattern", "use_ancestor_class", "父级类名", "Parent class", "父级的类名", "a parent's class"),
    ];

    let mut fields = vec![
        json!({"id": "name", "type": "text", "labelZh": "名称", "labelEn": "Name",
               "placeholderZh": "例如 chat / 输入框", "placeholderEn": "e.g. chat / search box"}),
        json!({"id": "enabled", "type": "bool", "labelZh": "启用", "labelEn": "Enabled"}),
        json!({"id": "priority", "type": "range", "labelZh": "优先级", "labelEn": "Priority",
               "min": 1, "max": 200, "step": 1, "unit": "",
               // ⚠️ THE NOTE NO LONGER CLAIMS TO ORDER ANYTHING: "优先级与排序无关，这个只能手动定义." It used to
               // read "数字小的先匹配" (lower matches first), which was true and is not any more -- the engine
               // tries the rules in the order the list shows them, and that order is the one the user dragged.
               "noteZh": "只作你自己记录，不影响顺序", "noteEn": "Your own note; it does not affect the order"}),
        json!({"id": "action", "type": "select", "labelZh": "命中时", "labelEn": "When matched",
               "options": [
                   {"value": "chinese", "labelZh": "中文", "labelEn": "Chinese"},
                   {"value": "english", "labelZh": "英文", "labelEn": "English"}]}),
        json!({"id": "match_mode", "type": "select", "labelZh": "匹配方式", "labelEn": "Match mode",
               "options": [
                   {"value": "wildcard", "labelZh": "通配符（* 和 ?）", "labelEn": "Wildcard (* and ?)"},
                   {"value": "regex", "labelZh": "正则表达式", "labelEn": "Regular expression"}]}),
    ];
    for (id, flag, zh, en, ez, ee) in conds {
        fields.push(json!({
            "id": id, "type": "text", "labelZh": zh, "labelEn": en,
            "toggle": flag,
            "placeholderZh": ez, "placeholderEn": ee
        }));
    }
    Value::Array(fields)
}

fn rule_item(r: &Rule) -> Value {
    let mut v = serde_json::Map::new();
    v.insert("name".into(), json!(r.name));
    v.insert("enabled".into(), json!(if r.enabled { 1 } else { 0 }));
    v.insert("priority".into(), json!(r.priority));
    v.insert(
        "action".into(),
        json!(match r.action {
            Action::Chinese => "chinese",
            Action::English => "english",
        }),
    );
    v.insert(
        "match_mode".into(),
        json!(match r.match_mode {
            MatchMode::Wildcard => "wildcard",
            MatchMode::Regex => "regex",
        }),
    );
    v.insert("process_pattern".into(), json!(r.process_pattern));
    v.insert("window_title_pattern".into(), json!(r.window_title_pattern));
    v.insert("window_class_pattern".into(), json!(r.window_class_pattern));
    v.insert("control_text_pattern".into(), json!(r.control_text_pattern));
    v.insert("control_class_pattern".into(), json!(r.control_class_pattern));
    v.insert("control_type_pattern".into(), json!(r.control_type_pattern));
    v.insert("automation_id_pattern".into(), json!(r.automation_id_pattern));
    v.insert("container_text_pattern".into(), json!(r.container_text_pattern));
    v.insert("ancestor_class_pattern".into(), json!(r.ancestor_class_pattern));
    // ⚠️ AND THE `use_*` FLAGS, which the page needs in order to DRAW the checkboxes. They are not controls of
    // their own (they travel with their field -- see `toggle` in rule_fields), but they are part of an item's
    // values: a checkbox whose state the page could not read would come back unticked on every redraw.
    v.insert("use_process".into(), json!(bool_bit(r.use_process)));
    v.insert("use_window_title".into(), json!(bool_bit(r.use_window_title)));
    v.insert("use_window_class".into(), json!(bool_bit(r.use_window_class)));
    v.insert("use_control_text".into(), json!(bool_bit(r.use_control_text)));
    v.insert("use_control_class".into(), json!(bool_bit(r.use_control_class)));
    v.insert("use_control_type".into(), json!(bool_bit(r.use_control_type)));
    v.insert("use_automation_id".into(), json!(bool_bit(r.use_automation_id)));
    v.insert("use_container_text".into(), json!(bool_bit(r.use_container_text)));
    v.insert("use_ancestor_class".into(), json!(bool_bit(r.use_ancestor_class)));
    // ⚠️ THE ROW'S TITLE, WHICH IS NOT THE RULE'S NAME: "规则名简化，只显示进程名即可，扩展名也不用显示."
    //
    // The `name` field stays exactly what it is -- the user's label for the rule, editable, and set by a capture
    // -- because it is DATA and the list is a VIEW of it. What the list needs is something short and comparable
    // down a column of ten rules, and for this feature that is the program: a rule that fires in chat is easier to
    // find as "chat" than as "chat.exe / chat". The extension goes too, since every process on Windows has one and it
    // is the same seven characters on every row.
    json!({ "title": rule_title(r), "values": Value::Object(v) })
}

fn bool_bit(b: bool) -> i32 {
    if b { 1 } else { 0 }
}

/// WHAT A ROW IS CALLED: the process name, without its extension.
///
/// Falls back to the rule's own name when there is no process pattern -- an unfinished rule, or one that matches
/// on something else entirely (a window class, a control's text). "新规则" and a captured name both read better
/// than an empty row.
fn rule_title(r: &Rule) -> String {
    let pattern = r.process_pattern.trim();
    if pattern.is_empty() {
        return r.name.clone();
    }
    // The last path component, then without its extension: a pattern may be a bare process name ("chat2.exe"),
    // may carry a folder ("C:\Tools\foo.exe"), and may be a wildcard ("*qq*") that has no extension at all --
    // `file_stem` leaves the last case alone, which is what we want.
    let leaf = pattern.rsplit(['\\', '/']).next().unwrap_or(pattern);
    match std::path::Path::new(leaf).file_stem() {
        Some(stem) if !stem.is_empty() => stem.to_string_lossy().into_owned(),
        _ => leaf.to_string(),
    }
}

/// `{"params":[...]}`, written into the caller's buffer. Returns the byte count, or <= 0 for "nothing".
pub fn settings_json(out: *mut c_char, out_size: c_int) -> c_int {
    if out.is_null() || out_size <= 1 {
        return 0;
    }

    let cfg = match clone_config().and_then(|a| a.lock().ok().map(|g| g.clone())) {
        Some(c) => c,
        None => AppConfig::default(),
    };

    // ⚠️ `active_order()`, NOT `active_rules()`: the page must show the DISABLED rules too, or a rule switched
    // off in the editor would vanish from the list the moment it was switched off and could never be switched
    // back on. (The engine's own list -- `active_rules` -- is what it matches against, and that one does skip
    // them; the two are different questions and this is the one the page is asking.)
    //
    // ⚠️ AND THE DRAFT IS SUBSTITUTED IN, SO THE PAGE DRAWS WHAT IS BEING EDITED. This is what makes a capture
    // visible: the capture lands in the draft, the page re-reads, and this line hands it the draft's row. A
    // page that drew the config instead would show the old values until Save -- which is the bug the user
    // reported as "捕获没有进到规则列表的选项框中".
    let draft = draft_get();
    let items: Vec<Value> = (0..cfg.rules.len())
        .filter_map(|display| {
            let real = cfg.real_index_of_display(display)?;
            let shown = match &draft {
                Some(d) if d.real == real => &d.rule,
                _ => &cfg.rules[real],
            };
            Some(rule_item(shown))
        })
        .collect();
    // ⚠️⚠️ WHICH ROW IS BEING EDITED, IN THE PAGE'S OWN NUMBERING -- AND THIS FIELD IS THE POINT OF IT.
    //
    // The draft lives on THIS side (see DRAFT above), so this is the only side that knows which rule is being
    // edited. The page used to keep a flag of its own and had to GUESS where a new rule had landed: it selected
    // "the last row", on the assumption that `add` appends. `add` does append -- to the FILE -- while the page
    // shows the rules SORTED by priority and then name, so a new rule (priority 100) lands wherever it sorts.
    // When the guess was wrong the user was dropped into editing a DIFFERENT rule, and pressing Save would have
    // written the new rule's fields over it.
    //
    // So the answer is PUBLISHED rather than guessed, and the page draws what it is given -- the same rule the
    // rest of this document follows. `-1` means "nothing is being edited".
    let editing: i64 = match &draft {
        Some(d) => (0..cfg.rules.len())
            .find(|&display| cfg.real_index_of_display(display) == Some(d.real))
            .map(|display| display as i64)
            .unwrap_or(-1),
        None => -1,
    };

    let doc = json!({
        "params": [
            {
                "id": "switch_method", "type": "select",
                "labelZh": "切换方式", "labelEn": "Switch method",
                "value": match cfg.switch_method {
                    model::SwitchMethod::Simulate => "simulate",
                    model::SwitchMethod::Ime => "ime",
                },
                "options": [
                    {"value": "ime", "labelZh": "输入法底层切换（推荐）", "labelEn": "IME open state (recommended)"},
                    {"value": "simulate", "labelZh": "模拟按键", "labelEn": "Simulate keystrokes"}
                ]
            },
            {
                "id": "ime_toggle_hotkey", "type": "hotkey",
                "labelZh": "切换热键", "labelEn": "Toggle hotkey",
                "value": cfg.ime_toggle_hotkey,
                // ⚠️ THE HINT SAYS WHAT TO DO, because a control nobody can guess is a control nobody uses: it is
                // not a text box any more, it is "click it and press the keys".
                "placeholderZh": "点击后按下新的快捷键", "placeholderEn": "Click, then press the keys"
            },
            {
                "id": "rules", "type": "group",
                // ⚠️ "master", NOT THE DEFAULT "stack", AND IT IS THIS FEATURE'S CALL TO MAKE. The user runs
                // NINE rules and said the stacked version was unusable: "现在这样做是变好看了，但是不方便，
                // 规则一多，要不断往下翻". Whether a group wants a list depends on how many items its users
                // really have, which only the feature knows -- so the panel is told, not asked to guess.
                //
                // (A group of two or three would look deliberate in "stack" and fussy in "master"; the panel
                // keeps both and this line is where the choice lives.)
                "layout": "master",
                "labelZh": "规则", "labelEn": "Rules",
                "fields": rule_fields(),
                "items": items,
                // WHICH ROW IS OPEN FOR EDITING (display index), or -1. See the note where `editing` is computed:
                // this is the feature's own state, published so the page does not have to guess it.
                "editing": editing,
                // ⚠️ THE SWITCH THAT BELONGS IN THE ROW, NOT IN THE DETAIL PANE: "规则荐的'启用'移到规则列表的
                // 各项右侧." It is `enabled`, which stays a field (the page reads and writes it by id) but is drawn
                // at the right-hand end of each row instead of among the fields -- so a rule can be switched off
                // from the list without opening it, which is the whole point of putting it there.
                "rowToggle": "enabled",
                // THE BUTTONS THIS FEATURE NEEDS (apex/abi.h, `actions`). Both were in the program this feature
                // replaces -- "点击捕获已就绪：请点击要捕获的控件" and the keyboard version of the same idea -- and
                // they cannot be typed, because the answer is a live window, a control class and a process name
                // that exist only while the user is pointing at them.
                //
                // ⚠️ THE LABELS SAY WHAT WILL HAPPEN, because the button changes the meaning of the user's next
                // click: after pressing it, clicking anything is a capture rather than an ordinary click. A
                // button that says only "捕获" would leave that surprise undisclosed.
                "actions": [
                    { "op": "capture", "key": "Ctrl+Alt+Q",
                      "labelZh": "点击捕获",
                      "labelEn": "Capture" }
                ],
                // ⚠️ AND WHETHER THAT BUTTON IS STILL WAITING FOR ITS CLICK (apex/abi.h, `waiting`). The page
                // polls while this is set and stops when it is gone, so this one field is what makes a completed
                // capture appear on the page without the user having to touch anything else. `false` is sent as
                // `null` rather than omitted, so the document always has the same shape.
                "waiting": if CAPTURE_WAITING.load(Ordering::SeqCst) { json!("capture") } else { Value::Null }
            }
        ],
        // ⚠️ WHICH FILE "打开设置文件" SHOULD OPEN: "右上方打开设置文件功能实现它."
        //
        // The host cannot know it: the ABI fixes the FOLDER a feature owns (`Plugins/<id>/`) and says nothing
        // about a file name, and this feature's settings are `config.json` while the host's fallback guess is
        // `<id>.ini` (which for AutoIME does not exist -- so the button opened nothing). The feature knows, so it
        // says, and the page passes it back with the request (see `settingsFile` in apex/abi.h).
        "settingsFile": "config.json",
        // ⚠️ AND ONE LINE ABOUT WHAT THIS FEATURE IS FOR, shown small beside the version at the top of its page:
        // "插件页最上方标题位，版本号后，可以小字简单说明插件的主要功能." The panel cannot write this -- it has no
        // idea what any feature does -- so the feature says it, in both languages, like every other label here.
        "summaryZh": "按当前焦点控件自动切换中英文输入法",
        "summaryEn": "Switches the input method by the focused control"
    });

    // ⚠️ SERIALISED BY `serde_json`, NOT BY `format!`, AND THE REASON IS THE USER'S OWN DATA: a rule pattern
    // is free text, and a Windows path contains backslashes -- `{"pattern":"C:\Games"}` is INVALID JSON (the
    // `\G` is not an escape) and the page's `JSON.parse` would reject the entire document. The symptom is a
    // feature page that draws nothing, which this project has now met three times (see AppendJsonString in the
    // SmoothWheel feature, and the curve-document notes). Hand-rolling the escaping is how it happens;
    // delegating it is how it stops.
    let text = doc.to_string();
    let bytes = text.as_bytes();
    let n = bytes.len().min((out_size as usize) - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out as *mut c_char as *mut u8, n);
        *(out as *mut u8).add(n) = 0;
    }
    n as c_int
}

// ---------------------------------------------------------------------------
// EDITS
// ---------------------------------------------------------------------------

/// Split "rules[3].process_pattern" into its parts, or None if it is not that shape.
///
/// ⚠️ THE SYNTAX IS DELIBERATELY MINIMAL -- one bracket pair, one dot -- because both ends of it are written
/// by hand, in two languages. A richer one would need a parser here to be safe; a control id may not contain
/// `[` or `.` (stated in abi.h), which is what makes this unambiguous without one.
fn parse_group_path(path: &str) -> Option<(&str, usize, &str)> {
    let open = path.find('[')?;
    let close = path[open..].find(']')? + open;
    let group = &path[..open];
    let index: usize = path[open + 1..close].parse().ok()?;
    let rest = path.get(close + 1..)?.strip_prefix('.')?;
    if group.is_empty() || rest.is_empty() {
        return None;
    }
    Some((group, index, rest))
}

/// Does this text mean "true", for a `bool` control?
///
/// ⚠️ IT ACCEPTS WHAT THE PAGE SENDS AND WHAT A PERSON WOULD WRITE. `"1"` is what the page sends today; the
/// words cost three lines and remove a whole class of "the switch does nothing" from anything that ever sends
/// a different spelling (a future page, a hand-made request, a test).
fn as_bool(v: &str) -> bool {
    matches!(v.trim().to_ascii_lowercase().as_str(), "1" | "true" | "on" | "yes")
}

/// THE RULE FIELD TABLE: set one field of one rule. Returns false for an unknown field -- the page then
/// re-reads and the old value comes back, which is the visible form of a refusal.
///
/// ⚠️ THE `use_*` FLAGS ARE WRITTEN HERE, BY THE CHECKBOX THAT GOVERNS EACH PATTERN. They used to be derived
/// from the text (`set_pair` turned the flag on when the pattern was non-empty) because no control wrote them;
/// the checkbox is that control now, and deriving them as well would mean two writers for one field.
fn set_rule_field(r: &mut Rule, field: &str, v: &str) -> bool {
    match field {
        "name" => r.name = v.to_string(),
        "enabled" => r.enabled = as_bool(v),
        "priority" => r.priority = v.trim().parse().unwrap_or(r.priority),
        "action" => {
            r.action = match v.trim() {
                "chinese" => Action::Chinese,
                "english" => Action::English,
                _ => return false,
            }
        }
        "match_mode" => {
            r.match_mode = match v.trim() {
                "wildcard" => MatchMode::Wildcard,
                "regex" => MatchMode::Regex,
                _ => return false,
            }
        }
        // EVERY CONDITION, as one arm each: the pattern...
        "process_pattern" => set_pattern(&mut r.process_pattern, v),
        "window_title_pattern" => set_pattern(&mut r.window_title_pattern, v),
        "window_class_pattern" => set_pattern(&mut r.window_class_pattern, v),
        "control_text_pattern" => set_pattern(&mut r.control_text_pattern, v),
        "control_class_pattern" => set_pattern(&mut r.control_class_pattern, v),
        "control_type_pattern" => set_pattern(&mut r.control_type_pattern, v),
        "automation_id_pattern" => set_pattern(&mut r.automation_id_pattern, v),
        "container_text_pattern" => set_pattern(&mut r.container_text_pattern, v),
        "ancestor_class_pattern" => set_pattern(&mut r.ancestor_class_pattern, v),
        // ...and the checkbox that decides whether it takes part.
        "use_process" => r.use_process = as_bool(v),
        "use_window_title" => r.use_window_title = as_bool(v),
        "use_window_class" => r.use_window_class = as_bool(v),
        "use_control_text" => r.use_control_text = as_bool(v),
        "use_control_class" => r.use_control_class = as_bool(v),
        "use_control_type" => r.use_control_type = as_bool(v),
        "use_automation_id" => r.use_automation_id = as_bool(v),
        "use_container_text" => r.use_container_text = as_bool(v),
        "use_ancestor_class" => r.use_ancestor_class = as_bool(v),
        _ => return false,
    }
    true
}

/// Set a condition's pattern -- THE TEXT ONLY. Whether the condition takes part is the checkbox's answer (see
/// `set_rule_field` and `toggle` in rule_fields).
///
/// ⚠️ IT USED TO BE `*flag = !pattern.trim().is_empty()`, which was the right answer while the flag had no
/// control of its own: the flag exists in the FILE, and a file claiming "this rule uses the process name" beside
/// an empty pattern is a sentence nobody can act on. Now the flag HAS a control -- the checkbox the user ticks --
/// and the two must not fight: a flag that followed the text would untick itself the moment the user cleared the
/// box to type something else, and would re-tick itself while they were still filling the pattern in.
fn set_pattern(pattern: &mut String, v: &str) {
    *pattern = v.to_string();
}

/// A fresh id, in the shape upstream used: nanoseconds since the epoch, in hex.
///
/// ⚠️ IT EXISTS SO THE FEATURE OWNS ITS OWN IDS. The page asks for a new rule with "add" and gets whatever
/// this returns; a page that generated ids would be inventing part of the feature's data model.
fn fresh_id() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => format!("{:x}", d.as_nanos()),
        Err(_) => "0".to_string(),
    }
}

/// SET ONE CONTROL (apex/abi.h: `setControl`).
pub fn set_control(path: *const c_char, value: *const c_char) -> c_int {
    if path.is_null() || value.is_null() {
        return 0;
    }
    let path = unsafe { std::ffi::CStr::from_ptr(path) }.to_string_lossy().into_owned();
    let value = unsafe { std::ffi::CStr::from_ptr(value) }.to_string_lossy().into_owned();

    // ---- the two settings that are not rules ----
    if path == "switch_method" {
        return match value.trim() {
            "ime" => with_config(|c| c.switch_method = model::SwitchMethod::Ime).map(|_| 1).unwrap_or(0),
            "simulate" => {
                with_config(|c| c.switch_method = model::SwitchMethod::Simulate).map(|_| 1).unwrap_or(0)
            }
            _ => 0,
        };
    }
    if path == "ime_toggle_hotkey" {
        // ⚠️⚠️ IT IS VALIDATED HERE NOW, BY ASKING THE CODE THAT SENDS THE KEYS (see `combo_is_usable`).
        //
        // The comment this replaces said validating here "would need the hotkey grammar in this module as well as
        // in the one that uses it, and a second copy of a grammar is how the two come to disagree about what is
        // valid" -- which was true, and was the reason nothing was checked at all. The answer was not to copy the
        // grammar but to ASK it: `win32::combo_is_usable` is the same parser `simulate_combo` presses with.
        //
        // ⚠️ AND IT IS REFUSED RATHER THAN STORED, WHICH MATTERS BECAUSE THE PAGE NOW RECORDS WHAT THE USER
        // PRESSES. A combination this feature cannot send would be stored, shown back to the user as their
        // setting, and then silently do nothing when pressed. Refusing (return 0) makes the page re-read the
        // control, so the box snaps back to the combination that is actually in force.
        if !crate::win32::combo_is_usable(&value) {
            log(&format!("AutoIME: refused the toggle hotkey {value:?}"));
            return 0;
        }
        let stored = with_config(|c| c.ime_toggle_hotkey = value.clone()).map(|_| 1).unwrap_or(0);
        // ⚠️ AND IT IS SAVED AT ONCE, because recording a combination is a deliberate act with nothing after it:
        // there is no Save button on this page, so waiting for the host's one-second debounce would be the only
        // thing standing between the user and a lost shortcut. (A write failure is logged inside `save_settings`;
        // the value is already in memory, so the caller is not told the change did not happen.)
        if stored == 1 {
            save_settings();
        }
        return stored;
    }

    // ---- a field inside a rule ----
    let (group, index, field) = match parse_group_path(&path) {
        Some(t) => t,
        None => return 0, // some other control this feature does not have
    };
    if group != "rules" {
        return 0;
    }
    with_config(|c| {
        // ⚠️ THE INDEX IS THE PAGE'S, AND IT IS TRANSLATED -- see AppConfig::real_index_of_display. The page
        // shows the rules SORTED (so the user can see which one wins) while the file keeps them in written
        // order, so the page's "number 3" is NOT the file's "index 3". Using it directly would edit a
        // DIFFERENT rule than the box that was clicked -- silently, and only while the two orders differ.
        //
        // (An `if let` rather than a `match` with a guard: a guard binds IMMUTABLY, so the field could not be
        // written through it. The mutation has to happen inside the block.)
        let real = match c.real_index_of_display(index) {
            Some(r) => r,
            None => return 0,
        };
        // ⚠️ AND IF THIS RULE IS BEING EDITED, THE EDIT GOES INTO THE DRAFT. That is the whole of the
        // edit/save model in one branch: while a draft is open the file is untouched, and Save is what
        // publishes it. Nothing else in this function has to know about editing.
        if draft_holds(real) {
            let ok = {
                let mut d = match DRAFT.lock() {
                    Ok(g) => g,
                    Err(_) => return 0,
                };
                match d.as_mut() {
                    Some(dr) if dr.real == real => set_rule_field(&mut dr.rule, field, &value),
                    _ => false,
                }
            };
            return if ok { 1 } else { 0 };
        }
        if let Some(r) = c.rules.get_mut(real) {
            if set_rule_field(r, field, &value) {
                return 1;
            }
        }
        0
    })
    .unwrap_or(0)
}

/// ADD, REMOVE OR REORDER A RULE (apex/abi.h: `listOp`).
pub fn list_op(id: *const c_char, op: *const c_char, value: *const c_char, index: c_int) -> c_int {
    if id.is_null() || op.is_null() {
        return 0;
    }
    let id = unsafe { std::ffi::CStr::from_ptr(id) }.to_string_lossy().into_owned();
    let op = unsafe { std::ffi::CStr::from_ptr(op) }.to_string_lossy().into_owned();
    // ⚠️ `value` IS READ FOR `move` ONLY, AND THAT IS THE POINT OF THE FIELD RATHER THAN AN ABUSE OF IT: the op
    // needs TWO numbers (where the row is, where it was dropped) and the call has one numeric slot (`index`). The
    // destination travels as text because that is what the field is -- see abi.h, `listOp`.
    let value = if value.is_null() {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(value) }.to_string_lossy().into_owned()
    };
    if id != "rules" {
        return 0;
    }

    let outcome = with_config(|c| match op.as_str() {
        // ---- THE EDIT / SAVE / CANCEL MODEL, WHICH IS THE STANDALONE PROGRAM'S, MOVED HERE ------------------
        //
        // The page drives it and this side holds the data (see DRAFT). The three ops are the three things the
        // standalone's editor did, in its own words: press 编辑 (`editing = true`, draft = a copy), press 保存
        // (`publish`), or press 取消 (`self.draft = None`).
        //
        // ⚠️ `begin-edit` ON A RULE THAT IS ALREADY DRAFTED IS NOT AN ERROR: the page re-reads after every op
        // and may send it twice, and re-copying would DISCARD whatever the user had already typed. Keeping the
        // open draft is the safe reading of the second request.
        "begin-edit" => {
            let i = index.max(0) as usize;
            let real = match c.real_index_of_display(i) {
                Some(r) => r,
                None => return 0,
            };
            if !draft_holds(real) {
                match c.rules.get(real) {
                    Some(r) => draft_set(Some(Draft { real, rule: r.clone(), fresh: false })),
                    None => return 0,
                }
            }
            1
        }
        // PUBLISH: the draft replaces the rule, the file is written, the draft is done.
        //
        // ⚠️ THE WRITE HAPPENS AFTER THIS MATCH, NOT ON THE HOST'S DEBOUNCE (see the `commit-edit` case in
        // `list_op` below), because a commit is a deliberate act rather than a stream of edits -- and the
        // standalone wrote the file on Save for the same reason.
        //
        // ⚠️ IT USED TO BE A COMMENT AND NOTHING ELSE: this branch published the draft in memory and trusted the
        // host's one-second debounce to write it. The comment said the file was written "here", so a reader had
        // no reason to look for the write -- and the window in which a Save could be lost (a kill, a crash, the
        // panel dying) was exactly one second wide. The host's SettingsTouch still runs as well; the two agree.
        "commit-edit" => {
            let i = index.max(0) as usize;
            let real = match c.real_index_of_display(i) {
                Some(r) => r,
                None => return 0,
            };
            let taken = {
                let mut d = match DRAFT.lock() {
                    Ok(g) => g,
                    Err(_) => return 0,
                };
                match d.as_ref() {
                    Some(dr) if dr.real == real => d.take(),
                    _ => None,
                }
            };
            match taken {
                Some(t) => {
                    if real < c.rules.len() {
                        c.rules[real] = t.rule;
                        return 1;
                    }
                    0
                }
                None => 0, // nothing was being edited: a no-op, not a failure to report as a change
            }
        }
        "cancel-edit" => {
            // ⚠️ IT ALWAYS REPORTS SUCCESS. "There is nothing to cancel" and "your edit was thrown away" both
            // leave the page in the state it asked for, and the page has already redrawn itself -- a 0 here
            // would make the panel log a refusal for something that worked.
            //
            // ⚠️⚠️ AND A RULE THAT WAS CREATED BY `add` AND NEVER SAVED GOES WITH THE DRAFT. That is the user's
            // answer to being asked what should happen ("没点保存就切走 = 等于没建（不保留）"): a rule with no saved
            // version has nothing to fall back to, so keeping it would leave an empty, switched-off 新规则 in the
            // list -- and, one debounce later, in config.json -- for the user to delete by hand. Dropping the
            // draft is what the page asked for; dropping the rule is what makes "not saved" mean "not created".
            //
            // (An EXISTING rule's edit is still just dropped: it has a saved version, so walking away costs the
            // typing and nothing else -- the rule the user asked for earlier: "编辑第一条，点第2条，再回第1条，内容
            // 应该变成编前的数据".)
            if let Some(real) = fresh_index(&c) {
                c.rules.remove(real);
            }
            draft_set(None);
            1
        }
        "add" => {
            // ⚠️ AN OPEN DRAFT IS DISCARDED BEFORE THE NEW ONE IS MADE, WHICH IS THE STANDALONE'S OWN
            // BEHAVIOUR: its 新建规则 moved the selection AND set `editing = true` on the new row, so the old
            // draft was gone by construction. A draft is a copy of ONE rule, and adding another makes it
            // meaningless -- keeping it would let a later Save write a rule the user had already navigated away
            // from.
            draft_set(None);
            // ⚠️ THE FEATURE BUILDS THE NEW RULE, so the page never invents a default -- `Rule::default()` is
            // upstream's own shape ("new rule", priority 100, process, English).
            //
            // ⚠️⚠️ AND IT IS CREATED SWITCHED OFF, BECAUSE OTHERWISE IT MATCHES EVERY PROGRAM ON THE MACHINE.
            // This is arithmetic rather than caution: the default has `use_process` ON with an EMPTY pattern,
            // and an empty pattern is not a condition (`push_condition` returns early on it), so `all()` over
            // ZERO conditions is TRUE -- it matches any focused control, anywhere. MEASURED in
            // `a_new_rule_must_not_match_everything`: with the flag on, the brand-new rule matches
            // "anything.exe".
            //
            // Upstream's default is the same, but reaching it meant using its own editor, where the pattern
            // was typed before the rule could act. A "+" in a settings page makes it one click -- and until
            // the user filled it in, the input method would flip in every program they touched. Off is one
            // deliberate act to undo and leaves the feature inert in the meantime.
            let mut r = Rule::default();
            r.id = fresh_id();
            r.name = "新规则".to_string();
            r.enabled = false;
            // ⚠️ FIRST IN THE FILE, WHICH IS FIRST IN THE LIST: "新添加规则放在最上面." The order is the file order
            // (see display_order), so putting it on top is one insert -- and NOT a renumbering of anybody's
            // priority, which is the user's own number and no longer has anything to do with the order.
            c.rules.insert(0, r);
            let new_real = 0;
            // ⚠️ AND THE NEW RULE IS OPENED FOR EDITING HERE, WHERE ITS ROW IS KNOWN.
            //
            // The standalone did the same thing in one act -- 新建规则 selected the new row AND put it in edit
            // mode -- and the reason is arithmetic rather than taste: a new rule is an EMPTY SHELL (no patterns,
            // switched off), so there is nothing to look at until it is filled in, and filling it in is what the
            // editor is for.
            //
            // The PAGE used to arrange that by assuming the new rule was the last row. It is APPENDED TO THE
            // FILE while the page draws the list in priority order -- so the last row is whatever sorts last,
            // which is now the one place the new rule is guaranteed NOT to be. Opening the draft here makes the
            // question go away: the document carries `editing = <the new row>`, and the page follows it (see the
            // `editing` note above).
            if let Some(new_rule) = c.rules.get(new_real) {
                // ⚠️ `fresh: true` -- THIS RULE HAS NEVER BEEN SAVED, and that is what makes abandoning the edit
                // abandon the rule as well (see `cancel-edit` and `Draft`).
                draft_set(Some(Draft { real: new_real, rule: new_rule.clone(), fresh: true }));
            }
            1
        }
        "remove" => {
            draft_set(None); // the rule being edited may be the one that just went -- see "add"
            // ⚠️ THE INDEX IS THE PAGE'S, so it is translated before it touches the file -- see
            // AppConfig::real_index_of_display. Removing by the raw number would delete a different rule than
            // the one whose Delete button was pressed.
            let i = index.max(0) as usize;
            match c.real_index_of_display(i) {
                Some(real) => {
                    c.rules.remove(real);
                    1
                }
                None => 0,
            }
        }
        // ⚠️ THE INDEX IS THE PAGE'S, AND A MOVE RENUMBERS THE PRIORITIES -- see
        // AppConfig::move_display_item for why neither "swap the priorities" nor "swap the file positions"
        // works. The short version: priorities tie (eight of this user's nine rules are at 100), and a tie is
        // broken by NAME, so a swap cannot make the order the user asked for.
        //
        // ⚠️ MOVE IS WHAT A DRAG SENDS, AND IT IS ONE MESSAGE WHERE move-up/move-down WOULD BE ONE PER PLACE.
        // `index` is where the row is now and `value` is where it was dropped (a decimal number): the page knows
        // both, because the user physically moved it. A chain of move-up/move-down messages would also mean one
        // re-read of the whole page per step, so the list would visibly walk its way to the destination.
        //
        // ⚠️⚠️ NONE OF THE THREE DROPS THE DRAFT ANY MORE, AND THAT IS NOW SAFE RATHER THAN MERELY NICER.
        // The old note here said a move had to drop it: the draft is keyed by the rule's REAL index and a move
        // renumbers every priority, so "the page's index and the draft's would no longer mean the same row".
        // That was true while the PAGE kept its own idea of which row was open. It no longer does: the document
        // carries `editing` -- the display index of the rule the draft is for -- and it is recomputed on every
        // read, so a row that moved is followed. A move touches priorities only, never the file positions the
        // draft is keyed by, so the draft still names exactly the rule it named before.
        "move-up" => {
            let i = index.max(0) as usize;
            (i > 0 && move_rule(c, i, i - 1)) as c_int
        }
        "move-down" => {
            let i = index.max(0) as usize;
            move_rule(c, i, i + 1) as c_int
        }
        "move" => {
            let from = index.max(0) as usize;
            let to = match value.trim().parse::<usize>() {
                Ok(to) => to,
                Err(_) => return 0, // not a position: refuse rather than guess at one
            };
            move_rule(c, from, to) as c_int
        }
        // CAPTURE: "use the control I click on next to fill this rule in".
        //
        // ⚠️ IT RETURNS IMMEDIATELY AND THE ANSWER ARRIVES ON A THREAD. The panel is waiting for this call's
        // reply; blocking it for as long as it takes the user to find a control would freeze the page they are
        // trying to use. So the op arms the wait and returns; the thread fills the rule in when the click comes,
        // and the page sees the result on its next read -- see settings_json, which draws the DRAFT.
        //
        // ⚠️⚠️ AND IT FILLS THE DRAFT, NOT THE CONFIG. This is the part that was wrong: a capture produced in
        // this function was being written into the live config while the page drew its own draft, so the user
        // saw nothing happen -- "捕获没有进到规则列表的选项框中，数据没进去." The standalone's `apply_capture` takes
        // the draft for exactly this reason (see DRAFT above), and now so does this.
        //
        // ⚠️ THE DRAFT MUST BE OPEN, AND THE PAGE IS WHAT ENSURES IT (ArmCapture refuses unless the rule is
        // being edited). If it is not, the capture is dropped rather than written through to the file: writing
        // would apply a change the user never confirmed, which is the mistake this whole model exists to avoid.
        "capture" => {
            let i = index.max(0) as usize;
            // ⚠️ THE RULE IS RESOLVED BEFORE ARMING. If the index is out of range there is nothing to fill in,
            // and arming anyway would leave a thread waiting for a click that could only log an error. This is
            // the same page-index translation Delete uses.
            let Some(real) = c.real_index_of_display(i) else {
                return 0;
            };
            if !draft_holds(real) {
                log("AutoIME: capture ignored -- the rule is not being edited");
                return 0;
            }
            let name = draft_get().map(|d| d.rule.name).unwrap_or_default();

            set_capture_waiting(true);
            log(&format!("AutoIME: capture armed for rule '{name}' -- click the control"));

            crate::win32::wait_for_click(Box::new(move |info| {
                // ⚠️ DISARM ON EVERY EXIT, INCLUDING THIS ONE. The monitor skips its click handling while a
                // capture is armed (that is how the armed click is kept from being evaluated as a normal rule),
                // so forgetting to clear it would silently stop rule matching for the rest of the session --
                // and the symptom would look like a matching bug, nowhere near the capture button.
                crate::win32::set_capture_armed(false);

                let label = format!(
                    "{} / {} / {}",
                    info.process_name.trim(),
                    info.window_title.trim(),
                    info.control_class.trim()
                );
                // ⚠️ INTO THE DRAFT, AND ONLY IF IT IS STILL THE SAME RULE. The user may have switched rows or
                // cancelled while the click was being waited for; filling a draft that has moved on would
                // overwrite a different rule than the one the capture was started for.
                //
                // ⚠️ NOTE THERE IS NO EARLY RETURN IN HERE: the page is told the wait is over BELOW, and a `?`
                // or a `return` on this path would leave it polling for ever. A lock that cannot be taken is
                // handled by doing nothing rather than by leaving.
                if let Ok(mut d) = DRAFT.lock() {
                    if let Some(dr) = d.as_mut() {
                        if dr.real == real {
                            // `rule_was_new = false`: an existing rule keeps the user's own switches and only
                            // has its fields filled -- see AppConfig::apply_capture.
                            let mut probe = AppConfig::default();
                            probe.rules.push(dr.rule.clone());
                            probe.apply_capture(0, &info, false);
                            if let Some(updated) = probe.rules.into_iter().next() {
                                dr.rule = updated;
                            }
                        }
                    }
                }
                // ⚠️⚠️ AND ONLY NOW DOES THE PAGE STOP WAITING. `waiting` is what it polls on, so clearing it
                // any earlier -- with the Win32 event above, say -- would let a poll land in the gap and stop the
                // watch on a document that does not have the captured values in it yet. The order of these two
                // lines is the whole reason the result appears without touching the row.
                set_capture_waiting(false);
                log(&format!("AutoIME: captured {label}"));
                // ⚠️ AND THE PAGE FINDS OUT BY RE-READING, NOT BY A MESSAGE FROM HERE.
                //
                // It used to publish this line into the feature's readout and the page watched for the text to
                // change. Both halves of that are gone: the readout is removed ("最上方的状态提示去了"), and the
                // page now polls the CONTROLS while the document says `waiting` (see `watchForCapture` in
                // panel.html) -- which is the thing it actually wants to redraw, so the signal and the redraw
                // are one event instead of two that had to be kept in step.
            }));
            1
        }
        _ => 0,
    })
    .unwrap_or(0);

    // ⚠️⚠️ A COMMIT IS ON DISK BEFORE THIS RETURNS, AND IT IS DONE HERE BECAUSE OF THE LOCK.    //
    // `save_settings` takes the config lock to snapshot the rules; `with_config` above is holding that same lock
    // for the whole match, so writing from inside it would deadlock this thread against itself. Out here the lock
    // is released and the snapshot is taken fresh -- which is also what makes the write include what was just
    // committed.
    //
    // ⚠️ ONLY A COMMIT, NOT EVERY OP: `add`, `remove` and a field edit are covered by the host's debounce (the
    // ABI's own model -- "persist when the change has settled"), and each of them is one row of a stream. A
    // commit is the single act the user calls "保存", so it is the one that must not be losable to a kill in the
    // next second. MEASURED before this: the commit reached the file only via the debounce, so the comment in
    // the `commit-edit` case above was describing code that did not exist.
    if outcome == 1 && op == "commit-edit" {
        save_settings();
    }
    outcome
}

/// THE CONFIG AS IT GOES TO THE FILE: the live one, minus a rule that `add` created and nobody has saved.
///
/// ⚠️ SPLIT OUT OF `save_settings` SO THE RULE CAN BE TESTED WITHOUT A FILESYSTEM. A test cannot call
/// `save_settings` -- with no feature directory there is nowhere to write, so it answers 0 -- and setting the
/// directory for a test would be a process-wide `OnceCell` shared by every other test in the binary. What is
/// worth asserting is the DECISION ("this rule is not written"), and that is this function.
fn config_for_writing() -> Option<AppConfig> {
    let mut c = clone_config().and_then(|a| a.lock().ok().map(|g| g.clone()))?;
    if let Some(real) = fresh_index(&c) {
        c.rules.remove(real);
    }
    Some(c)
}

/// WRITE THE FILE (apex/abi.h: `saveSettings`).
///
/// ⚠️⚠️ AN UNCOMMITTED NEW RULE IS NOT IN THE FILE, AND THAT IS A SECOND HALF RATHER THAN A DETAIL. `add` puts
/// the new rule in the CONFIG (the page has to draw it, and every other op addresses rules by their position in
/// that list), and the host saves on a one-second debounce -- so without this, the empty 新规则 would be in
/// config.json within a second of being created, and would survive the panel being closed mid-edit. That is
/// exactly the "not saved = not created" the user asked for, and it has to hold on the paths where the draft is
/// never cancelled: closing the panel, quitting Apex, or a kill.
///
/// ⚠️ THE LIVE CONFIG IS NOT TOUCHED -- only the copy that is written (see `config_for_writing`). The draft is
/// still an edit in progress, and removing the rule from memory here would delete it out from under the editor
/// the user is typing in.
pub fn save_settings() -> c_int {
    let snapshot = match config_for_writing() {
        Some(c) => c,
        None => return 0,
    };
    match model::save_config(&snapshot) {
        Ok(()) => 1,
        Err(e) => {
            // Logged rather than swallowed: a settings page that appears to save and does not is exactly the
            // failure this feature would be blamed for.
            log(&format!("AutoIME: could not write config.json: {e}"));
            0
        }
    }
}

/// RE-READ THE FILE (apex/abi.h: `reloadSettings`). Returns 0 when it cannot be parsed, leaving the values
/// already in memory alone -- the same rule the host applies to its own settings.
pub fn reload_settings() -> c_int {
    let fresh = model::load_config();
    with_config(|c| *c = fresh).map(|_| 1).unwrap_or(0)
}

/// A COPY OF THE LIVE CONFIG, for the tests.
///
/// It hands back a CLONE, so a test cannot reach into the live config and leave a change behind for the next
/// one -- tests that share mutable state through a process-wide singleton fail in whatever order they happen
/// to run in, which is the worst kind of test to debug.
///
/// ⚠️ `cfg(test)`, ALL THREE OF THE DEBUG ACCESSORS -- and the third one is new. The program used to call this
/// one for the settings readout's "are there any rules at all" line; that readout is gone (see win32.rs), so
/// nothing in a non-test build calls it any more and `#![deny(dead_code)]` says so out loud.
#[cfg(test)]
pub fn debug_config() -> Option<crate::model::AppConfig> {
    clone_config().and_then(|a| a.lock().ok().map(|g| g.clone()))
}

/// THE OPEN DRAFT, for the tests -- same reasoning as `debug_config`: the ops are the only writers, and without
/// this a test could only look at the FILE, which by design does not change until Save. That would make the
/// edit/save model untestable at exactly the point the user reported a bug.
///
/// ⚠️ `cfg(test)`, BOTH OF THESE: the program has no caller (the page reads the draft through `settings_json`,
/// which substitutes it into the document), and `#![deny(dead_code)]` in lib.rs is what makes that a statement
/// rather than a claim -- a non-test build that called one of these would not compile.
#[cfg(test)]
#[allow(clippy::type_complexity)]
pub fn debug_draft() -> Option<(usize, crate::model::Rule, bool)> {
    draft_get().map(|d| (d.real, d.rule, d.fresh))
}

/// WHAT WOULD BE WRITTEN, for the tests -- see `config_for_writing`. Same `cfg(test)` reasoning as the others:
/// the program reaches it through `save_settings`, which is the only caller.
#[cfg(test)]
pub fn debug_config_for_writing() -> Option<crate::model::AppConfig> {
    config_for_writing()
}

/// Clears the draft. For the tests, which share one process-wide draft and must each start from a known world
/// -- see the note in `use_config`, which calls this for exactly that reason.
#[cfg(test)]
pub fn debug_clear_draft() {
    draft_set(None);
}
