use regex::Regex;
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MatchTarget {
    ActiveWindow,
    ActiveControl,
    MouseWindow,
    MouseControl,
}

impl MatchTarget {
    pub fn to_active(self) -> Self {
        match self {
            MatchTarget::ActiveWindow
            | MatchTarget::MouseWindow
            | MatchTarget::MouseControl => MatchTarget::ActiveControl,
            other => other,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Action {
    Chinese,
    English,
}

// ⚠️ THERE IS NO `label()` ON ANY OF THESE ENUMS, AND ITS ABSENCE IS THE PORT'S DOING RATHER THAN AN OMISSION.
// Upstream drew its own egui settings window, so every enum carried the words that window printed. Apex's panel
// is generic: the words a user reads come from `settings.rs` (the controls this feature describes), and the
// readout in `win32.rs` prints its own. A second copy here would be a second place a label lives -- and the
// first one to be forgotten when a word changes.

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SwitchMethod {
    Simulate,
    Ime,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MatchMode {
    Wildcard,
    Regex,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct WindowInfo {
    pub process_name: String,
    pub process_path: String,
    pub window_title: String,
    pub window_class: String,
    pub control_text: String,
    pub control_class: String,
    pub control_type: String,
    pub automation_id: String,
    pub container_text: String,
    #[serde(default)]
    pub ancestor_texts: Vec<String>,
    #[serde(default)]
    pub ancestor_classes: Vec<String>,
    pub window_hwnd: usize,
    pub control_hwnd: usize,
    pub click_x: i32,
    pub click_y: i32,
}

impl WindowInfo {
    pub fn summary(&self) -> String {
        let ancestors = if self.ancestor_texts.is_empty() {
            String::new()
        } else {
            format!("\n父层链: {}", self.ancestor_texts.join(" / "))
        };
        format!(
            "进程: {}\n窗口标题: {}\n窗口类: {}\n控件文本: {}\n控件类: {}\n控件类型: {}\n自动化ID: {}\n父级标签: {}{}\nEXE: {}",
            self.process_name,
            self.window_title,
            self.window_class,
            self.control_text,
            self.control_class,
            self.control_type,
            self.automation_id,
            self.container_text,
            ancestors,
            self.process_path
        )
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Rule {
    pub id: String,
    pub name: String,
    pub enabled: bool,
    pub priority: u32,
    pub match_target: MatchTarget,
    pub match_mode: MatchMode,
    #[serde(default = "default_true")]
    pub use_process: bool,
    #[serde(default = "default_true")]
    pub use_window_title: bool,
    #[serde(default = "default_true")]
    pub use_window_class: bool,
    #[serde(default = "default_true")]
    pub use_control_text: bool,
    #[serde(default = "default_true")]
    pub use_control_class: bool,
    #[serde(default = "default_true")]
    pub use_control_type: bool,
    #[serde(default = "default_true")]
    pub use_automation_id: bool,
    #[serde(default)]
    pub use_container_text: bool,
    #[serde(default)]
    pub use_ancestor_class: bool,
    pub process_pattern: String,
    pub window_title_pattern: String,
    pub window_class_pattern: String,
    pub control_text_pattern: String,
    pub control_class_pattern: String,
    #[serde(default)]
    pub control_type_pattern: String,
    #[serde(default)]
    pub automation_id_pattern: String,
    #[serde(default)]
    pub container_text_pattern: String,
    #[serde(default)]
    pub ancestor_class_pattern: String,
    pub action: Action,
}

fn default_true() -> bool {
    true
}

impl Default for Rule {
    fn default() -> Self {
        Self {
            id: new_id(),
            name: "新规则".to_string(),
            enabled: true,
            priority: 100,
            match_target: MatchTarget::ActiveControl,
            match_mode: MatchMode::Wildcard,
            use_process: true,
            use_window_title: false,
            use_window_class: false,
            use_control_text: false,
            use_control_class: false,
            use_control_type: false,
            use_automation_id: false,
            use_container_text: false,
            use_ancestor_class: false,
            process_pattern: String::new(),
            window_title_pattern: String::new(),
            window_class_pattern: String::new(),
            control_text_pattern: String::new(),
            control_class_pattern: String::new(),
            control_type_pattern: String::new(),
            automation_id_pattern: String::new(),
            container_text_pattern: String::new(),
            ancestor_class_pattern: String::new(),
            action: Action::English,
        }
    }
}

impl Rule {
    pub fn matches(&self, window: &WindowInfo, control: &WindowInfo) -> bool {
        let (win, ctrl) = match self.match_target {
            MatchTarget::ActiveWindow | MatchTarget::MouseWindow => (window, None),
            MatchTarget::ActiveControl | MatchTarget::MouseControl => (window, Some(control)),
        };

        let mut conditions: Vec<bool> = Vec::new();
        if self.use_process {
            self.push_condition(&mut conditions, &self.process_pattern, &win.process_name);
        }
        if self.use_window_title {
            self.push_condition(&mut conditions, &self.window_title_pattern, &win.window_title);
        }
        if self.use_window_class {
            self.push_condition(&mut conditions, &self.window_class_pattern, &win.window_class);
        }

        if let Some(ctrl) = ctrl {
            if self.use_control_text {
                self.push_condition(&mut conditions, &self.control_text_pattern, &ctrl.control_text);
            }
            if self.use_control_class {
                self.push_condition(&mut conditions, &self.control_class_pattern, &ctrl.control_class);
            }
            if self.use_control_type {
                self.push_condition(&mut conditions, &self.control_type_pattern, &ctrl.control_type);
            }
            if self.use_automation_id {
                self.push_condition(&mut conditions, &self.automation_id_pattern, &ctrl.automation_id);
            }
            if self.use_container_text {
                let pattern = self.container_text_pattern.trim();
                if !pattern.is_empty() {
                    let matched = ctrl
                        .ancestor_texts
                        .iter()
                        .any(|text| self.field_matches(pattern, text));
                    conditions.push(matched);
                }
            }
            if self.use_ancestor_class {
                let pattern = self.ancestor_class_pattern.trim();
                if !pattern.is_empty() {
                    let matched = ctrl
                        .ancestor_classes
                        .iter()
                        .any(|class| self.field_matches(pattern, class));
                    conditions.push(matched);
                }
            }
        }

        conditions.iter().all(|matched| *matched)
    }

    fn push_condition(&self, conditions: &mut Vec<bool>, pattern: &str, value: &str) {
        if pattern.trim().is_empty() {
            return;
        }
        conditions.push(self.field_matches(pattern, value));
    }

    fn field_matches(&self, pattern: &str, value: &str) -> bool {
        let pattern = pattern.trim();
        let value = value.trim();
        if pattern.is_empty() {
            return true;
        }

        match self.match_mode {
            MatchMode::Wildcard => wildcard_match(pattern, value),
            MatchMode::Regex => match Regex::new(&format!("(?i:{pattern})")) {
                Ok(re) => re.is_match(value),
                Err(_) => value.to_lowercase() == pattern.to_lowercase(),
            },
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct AppConfig {
    #[serde(default = "default_ime_toggle_hotkey")]
    pub ime_toggle_hotkey: String,
    #[serde(default)]
    pub window_pos: Option<(f32, f32)>,
    #[serde(default = "default_switch_method")]
    pub switch_method: SwitchMethod,
    pub rules: Vec<Rule>,
}

fn default_ime_toggle_hotkey() -> String {
    "Ctrl+Space".to_string()
}

fn default_switch_method() -> SwitchMethod {
    SwitchMethod::Simulate
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            ime_toggle_hotkey: default_ime_toggle_hotkey(),
            window_pos: None,
            switch_method: default_switch_method(),
            rules: Vec::new(),
        }
    }
}

impl AppConfig {
    pub fn migrate(&mut self) {
        for rule in &mut self.rules {
            rule.match_target = rule.match_target.to_active();
        }
    }

    /// THE ORDER THE LIST SHOWS, AS FILE POSITIONS -- and it is simply the order the rules are WRITTEN IN.
    ///
    /// ⚠️⚠️ PRIORITY NO LONGER DECIDES THIS, BY THE USER'S DECISION: "优先级与排序无关，这个只能手动定义."
    ///
    /// It used to sort by (priority, then name), and that made the priority slider a *second* way to arrange the
    /// list -- one that fought the first. Dragging a row had to renumber every priority to make the order stick
    /// (see `move_display_item`), so the user's own numbers were rewritten every time they moved something, and
    /// the two things they thought of as separate ("where it sits" and "the number I set") kept overwriting each
    /// other. Now the list IS the file order: dragging moves the entry, and no number is touched. The priority
    /// field stays in the editor and in the file as the user's own note; nothing here reads it.
    ///
    /// ⚠️ ONE IMPLEMENTATION, BECAUSE FOUR THINGS ASK THIS QUESTION: what the document draws, which rule the
    /// page's number means, where a manual move puts a rule, and where a new rule goes. They have to agree.
    pub fn display_order(&self) -> Vec<usize> {
        (0..self.rules.len()).collect()
    }

    /// PUT THE RULES IN THE GIVEN FILE-POSITION ORDER, leaving every field -- priorities included -- alone.
    pub fn set_display_order(&mut self, order: &[usize]) {
        if order.len() != self.rules.len() {
            return;
        }
        self.rules = order.iter().filter_map(|&real| self.rules.get(real).cloned()).collect();
    }

    pub fn sorted_rules(&self) -> Vec<Rule> {
        self.display_order()
            .into_iter()
            .map(|real| self.rules[real].clone())
            .collect()
    }

    pub fn active_rules(&self) -> Vec<Rule> {
        self.sorted_rules()
            .into_iter()
            .filter(|rule| rule.enabled)
            .collect()
    }

}

// ⚠️ THE ONE LINE THAT COULD NOT BE PORTED AS-IS, AND GETTING IT WRONG WOULD HAVE BEEN SILENT.
//
// Upstream is a standalone exe, so "beside the exe" (current_exe) IS its own folder. Inside Apex this file is a
// DLL loaded into apex.exe, so `current_exe()` is the HOST: the config would have been read from, and written
// to, the Apex folder -- while the user's own rules sat unread in Plugins\AutoIME\. Nothing would have errored.
// The rules would simply never fire, and their settings would look forgotten.
//
// Apex gives every feature its own folder through `featureDir()` (ApexHost), which is exactly "beside the
// DLL". It is filled in by `init`, which runs before anything here can -- the monitor thread is started there
// too -- so the value is always set by the time it is read.
static CONFIG_DIR: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();

pub fn set_config_dir(dir: PathBuf) {
    let _ = CONFIG_DIR.set(dir);
}

pub fn config_path() -> PathBuf {
    match CONFIG_DIR.get() {
        Some(dir) => dir.join("config.json"),
        // Nobody has said where we are, which means init has not run. Rather than guess -- and write into the
        // HOST's folder, which is what guessing did -- this returns a path nothing will use: reading it yields
        // the defaults and writing it fails. Both are visible in a log; a file in the wrong folder is not.
        None => PathBuf::from("\u{0}no-feature-dir"),
    }
}

// ---------------------------------------------------------------------------
// "IS THIS WINDOW APEX ITSELF?" -- the host, or the settings panel.
//
// ⚠️ WHY THIS IS NOT "IS IT apex.exe". Apex is two programs in one folder (apex.exe and apex-settings.exe,
// see AGENTS.md §一 and settings_ipc.h), and the second one is where the user is looking while they read the
// rules. A rule that fires there flips the input method of the very page being used to write the rule -- and
// the guard that was ported from upstream compared PROCESS NAMES, so it only ever recognised the host: the
// panel went through as if it were any other program. MEASURED, in this feature's own diagnostic log:
// `SWITCH rule=... proc=apex-settings.exe`.
//
// THE JUDGEMENT IS THE FOLDER, WHICH IS THE PROJECT'S OWN DEFINITION of "this installation" -- the host makes
// exactly the same call in settings_ipc.h (`SameInstallDirectory`: "判据是同一个目录，不是同一个 exe"). It also
// covers a third process nobody has written yet, which a list of names would not.
//
// ⚠️ THE ROOT COMES FROM `featureDir`, NOT FROM A SEARCH. The ABI documents that folder as `Plugins/<id>/`, so
// two levels up from it is the installation. Nothing is scanned and no window is enumerated.
// ---------------------------------------------------------------------------

static INSTALL_ROOT: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();

/// Set once by `init`, from the feature's own folder (see the note above).
///
/// ⚠️ A `OnceLock` IS SAFE HERE WHERE IT WAS WRONG FOR THE SETTINGS: this value is the folder the running
/// program is executing from, which cannot change while it runs, so a second `init` (the host may unload and
/// reload a feature) has nothing to update. The settings had to be re-installable because the user can change
/// them at any moment -- that is the difference, and it is why one uses a lock and the other does not.
pub fn set_install_root(dir: PathBuf) {
    let _ = INSTALL_ROOT.set(dir);
}

pub fn install_root() -> Option<&'static Path> {
    INSTALL_ROOT.get().map(|p| p.as_path())
}

/// WHERE THE INSTALLATION IS, GIVEN THE FOLDER THE HOST HANDED US.
///
/// ⚠️ IT IS A FUNCTION RATHER THAN TWO `.parent()` CALLS AT THE CALL SITE BECAUSE THE SHAPE IS A PROMISE. The
/// ABI documents `featureDir` as `Plugins/<id>/` (apex/abi.h), so the installation -- the folder holding
/// apex.exe and apex-settings.exe -- is two levels up from it. That promise is the whole basis of
/// `is_apex_itself`, and a test can pin it; a `.parent().parent()` written inline can only be read.
///
/// `None` when the path is too short to have two parents: the caller then keeps the weaker fallback rather than
/// guessing at a root (see `is_apex_itself`).
pub fn installation_of(feature_dir: &Path) -> Option<PathBuf> {
    // ⚠️ `parent()` IGNORES A TRAILING SEPARATOR, which matters here: the host's answer carries one (the log
    // says `Plugins\AutoIME\`), and the naive reading -- "the last component is empty, so one parent up is
    // still AutoIME" -- would put the installation at `Plugins\`, i.e. every feature would be "Apex itself".
    // Pinned by `the_installation_is_two_levels_up_from_the_feature_folder`.
    feature_dir.parent()?.parent().map(|p| p.to_path_buf())
}

/// Is this captured window one of Apex's own?
///
/// THE FOLDER DECIDES WHENEVER THERE IS ONE TO READ. `process_path` is empty-or-a-bare-name exactly when the
/// query failed (an elevated program -- see `process_path` in win32.rs), and only then does the name get a say,
/// via `host_exe_name` (the host's own process name).
///
/// ⚠️ THE FALLBACK IS WEAKER THAN THE FOLDER CHECK AND CANNOT SEE THE SETTINGS PANEL -- the panel is a
/// different exe. It is here because it is what the port had, and because "no folder" is not a reason to stop
/// looking; it is NOT a second opinion that may overrule the folder. Getting that wrong is a bug this function
/// had for one test run: written as "if the folder does not match, try the name", a program called `apex.exe` in
/// ANY folder counted as Apex -- so a whole machine's worth of unrelated programs named apex.exe would silently
/// stop following the user's rules (see `a_program_in_another_folder_is_not_apex_even_with_the_same_prefix`).
///
/// ⚠️ AND ITS ONE REMAINING LOOSENESS IS THE SAFE DIRECTION: with no installation known (`None`) or no folder
/// in the path, the name decides -- so a program called apex.exe somewhere else is treated as Apex. A missed
/// exclusion flips the input method once in Apex's own window; a wrong exclusion silently stops the rules
/// working in a program the user cares about. (The same asymmetry `decision.h` is built on.)
pub fn is_apex_itself(info: &WindowInfo, install_root: Option<&Path>, host_exe_name: &str) -> bool {
    if let Some(folder) = parent_folder(&info.process_path) {
        if let Some(root) = install_root {
            return folder == folder_key(&root.to_string_lossy());
        }
        // The path was readable but there is no installation to compare it against (init could not work the
        // folder out): fall through to the name rather than answer "no" for the host itself.
    }
    !host_exe_name.trim().is_empty()
        && info.process_name.trim().eq_ignore_ascii_case(host_exe_name.trim())
}

/// The folder a file path sits in, as a comparable key -- `None` when the string has no folder in it at all.
fn parent_folder(file: &str) -> Option<String> {
    let normalised = file.trim().replace('/', "\\");
    let cut = normalised.rfind('\\')?;
    let key = folder_key(&normalised[..cut]);
    if key.is_empty() {
        None
    } else {
        Some(key)
    }
}

/// How a folder is compared: one separator, no trailing one, case-folded (Windows paths are case-insensitive).
///
/// ⚠️ IT IS COMPARED WHOLE, NEVER AS A PREFIX. `D:\App protable\Apex-backup` starts with the installation's
/// path and is a different installation -- and "portable" here means the user really does keep copies side by
/// side, which is why the host's own single-instance check is about the folder too (AGENTS.md §3.6.4).
fn folder_key(folder: &str) -> String {
    folder.replace('/', "\\").trim_end_matches('\\').to_ascii_lowercase()
}

pub fn load_config() -> AppConfig {
    let path = config_path();
    match fs::read_to_string(&path) {
        Ok(text) => {
            let mut config =
                serde_json::from_str::<AppConfig>(&text).unwrap_or_default();
            config.migrate();
            config
        }
        Err(_) => AppConfig::default(),
    }
}

pub fn save_config(config: &AppConfig) -> Result<(), String> {
    let path = config_path();
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let text = serde_json::to_string_pretty(config).map_err(|e| e.to_string())?;
    fs::write(&path, text).map_err(|e| e.to_string())
}

pub fn new_id() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    format!("{nanos:x}")
}

pub fn wildcard_match(pattern: &str, value: &str) -> bool {
    let pattern = pattern.to_lowercase();
    let value = value.to_lowercase();
    let p: Vec<char> = pattern.chars().collect();
    let v: Vec<char> = value.chars().collect();
    let (mut i, mut j) = (0usize, 0usize);
    let (mut star, mut mark) = (usize::MAX, 0usize);

    while j < v.len() {
        if i < p.len() && (p[i] == '?' || p[i] == v[j]) {
            i += 1;
            j += 1;
        } else if i < p.len() && p[i] == '*' {
            star = i;
            mark = j;
            i += 1;
        } else if star != usize::MAX {
            i = star + 1;
            mark += 1;
            j = mark;
        } else {
            return false;
        }
    }

    while i < p.len() && p[i] == '*' {
        i += 1;
    }
    i == p.len()
}

impl AppConfig {
    /// THE RULE THE PAGE CALLS "NUMBER n" -- the n-th in the order the engine will try them.
    ///
    /// ⚠️ WHY THIS EXISTS: the page shows the rules SORTED (so the user can see which one wins), but the file
    /// keeps them in whatever order they were written. Indexing the file with a number the page got from the
    /// sorted list edits A DIFFERENT RULE than the box the user clicked -- silently, and only for as long as
    /// the two orders differ, which is exactly the kind of bug that reproduces on one machine and not another.
    ///
    /// So the page's number is translated here, in one place, with the same sort key the engine uses. A test in
    /// tests.rs pins it against a list where the two orders genuinely differ.
    ///
    /// ⚠️ `cfg(test)`, AND THAT IS A STATEMENT RATHER THAN HOUSEKEEPING: the translation the PROGRAM uses is
    /// `real_index_of_display`, which every writer calls. This one exists so a test can reach a rule the way the
    /// page numbers them; a non-test build has no caller at all, which the compiler says out loud (see
    /// `#![deny(dead_code)]` in lib.rs).
    #[cfg(test)]
    pub fn rule_at_display_index(&mut self, index: usize) -> Option<&mut Rule> {
        let real = self.real_index_of_display(index)?;
        self.rules.get_mut(real)
    }

    /// The file position of the rule the page calls "number n". The single translation -- see
    /// `rule_at_display_index` for why it is needed at all.
    pub fn real_index_of_display(&self, index: usize) -> Option<usize> {
        self.display_order().get(index).copied()
    }

    /// FILL A RULE IN FROM A CAPTURED CONTROL -- upstream's `apply_capture`, ported.
    ///
    /// This is the half of "capture" that is about DATA rather than about windows, and it is deliberately here
    /// rather than next to the Win32 code: what a capture means for a rule is a property of the RULE, and it is
    /// testable without a screen (see `a_capture_fills_the_rule_and_leaves_it_enabled`).
    ///
    /// ⚠️ THE DEFAULT FLAGS ARE NARROWER FOR A NEW RULE, AND THAT IS UPSTREAM'S DECISION, KEPT DELIBERATELY.
    /// `Rule::default()` has every field switched on, but the fields a capture fills in are mostly GENERIC --
    /// the window title, the window class, the control's class -- and they are identical across hundreds of
    /// controls. A new rule that matched on all of them would look precise while really meaning "any control in
    /// this program", so upstream turns them off and leaves the two that identify a control: its process and
    /// its own text. An EXISTING rule keeps whatever the user chose; only a brand-new one is narrowed.
    ///
    /// ⚠️ AND A CAPTURED RULE IS SWITCHED ON. This is the opposite of the rule created by the page's "+"
    /// button, and the difference is intent: a new rule from "+" is an empty shell that would match everything
    /// (see `a_new_rule_must_not_match_everything`), while a captured one has real patterns in it and the act
    /// of capturing the control IS the user saying "this is the one I mean".
    pub fn apply_capture(&mut self, real_index: usize, info: &WindowInfo, rule_was_new: bool) {
        if let Some(rule) = self.rules.get_mut(real_index) {
            rule.apply_capture(info, rule_was_new);
        }
    }
}

impl Rule {
    /// FILL THIS RULE IN FROM A CAPTURED CONTROL -- the body of what `AppConfig::apply_capture` does, on a
    /// single rule.
    ///
    /// ⚠️ ON `Rule` RATHER THAN ON `AppConfig`, BECAUSE THE COMMON CALLER HAS A RULE AND NOT A CONFIG. A capture
    /// is applied to the rule being EDITED, which lives in the feature's draft (`settings.rs::DRAFT`) -- there is
    /// no `AppConfig` around it, and inventing one to reach this code would mean cloning and copying back (which
    /// is what the first version did, and it read as work nobody could account for).
    pub fn apply_capture(&mut self, info: &WindowInfo, rule_was_new: bool) {
        let rule = self;

        // What to call it in the list. The control's own text is the most recognisable label; its type or
        // class is the fallback when the control has no text at all (an icon, a bare pane).
        let control_label = if !info.control_text.trim().is_empty() {
            info.control_text.trim()
        } else if !info.control_type.trim().is_empty() {
            info.control_type.trim()
        } else {
            info.control_class.trim()
        };
        rule.name = format!("{} / {}", info.process_name.trim(), control_label);

        rule.process_pattern = info.process_name.trim().to_string();
        rule.window_title_pattern = info.window_title.trim().to_string();
        rule.window_class_pattern = info.window_class.trim().to_string();
        rule.control_text_pattern = smart_control_pattern(&info.control_text);
        rule.control_class_pattern = info.control_class.trim().to_string();
        rule.control_type_pattern = info.control_type.trim().to_string();
        rule.automation_id_pattern = info.automation_id.trim().to_string();
        rule.container_text_pattern = info.container_text.trim().to_string();
        rule.ancestor_class_pattern = info.ancestor_classes.first().cloned().unwrap_or_default();

        if rule_was_new {
            rule.use_process = true;
            rule.use_window_title = false;
            rule.use_window_class = false;
            // Only switch on a condition that has something in it: a flag on an empty pattern is not a
            // condition (see `push_condition`), and leaving it on reads as "this rule uses the container text"
            // when it does not.
            rule.use_control_text = !info.control_text.trim().is_empty();
            rule.use_control_class = false;
            rule.use_control_type = false;
            rule.use_automation_id = false;
            rule.use_container_text = !info.container_text.trim().is_empty();
            rule.use_ancestor_class = false;
        }

        rule.enabled = true;
    }
}

impl AppConfig {
    /// MOVE THE RULE AT DISPLAY POSITION `from` TO DISPLAY POSITION `to`, and make that stick.
    ///
    /// ⚠️ IT MOVES THE ENTRY, AND NOTHING ELSE -- and the note that used to stand here said the opposite, twice.
    /// It recorded that "swapping the file positions changes nothing at all, because the display is DERIVED from
    /// the sort", which was true while the list was sorted by priority: the next sort undid the swap and the move
    /// read as dead. THE USER HAS SINCE DECIDED THAT THE ORDER IS THE MANUAL ONE ("优先级与排序无关，这个只能手动
    /// 定义"), so the display is no longer derived from anything -- the file order IS the order -- and moving the
    /// entry is now the whole of the operation. It also stops the renumbering that used to rewrite the user's own
    /// priority numbers every time they dragged something.
    ///
    /// Returns false when `to` is out of range, so the caller can report "nothing happened" and the page can
    /// re-read (rather than showing a move that will not survive the next redraw).
    pub fn move_display_item(&mut self, from: usize, to: usize) -> bool {
        let n = self.rules.len();
        if from >= n || to >= n {
            return false;
        }
        let mut order = self.display_order();
        let moved = order.remove(from);
        order.insert(to, moved);
        self.set_display_order(&order);
        true
    }
}

/// A CONTROL'S TEXT, TURNED INTO A PATTERN THAT WILL MATCH IT AGAIN -- upstream's `smart_control_pattern`.
///
/// The one case it handles: Explorer's address bar reads "在 搜索框 中搜索" when empty and carries the typed
/// text when not, so a pattern taken verbatim stops matching the moment the user searches for something. The
/// wildcard keeps the rule on the BAR rather than on whatever is currently in it.
///
/// It is deliberately not cleverer than that. Every other control's text is used as it stands, because
/// guessing at a pattern is how a rule comes to match something the user never pointed at.
pub fn smart_control_pattern(text: &str) -> String {
    let text = text.trim();
    if let Some(rest) = text.strip_prefix('在') {
        if rest.ends_with("中搜索") {
            return "在*中搜索".to_string();
        }
    }
    text.to_string()
}
