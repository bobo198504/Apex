//! The matching engine's semantics, pinned.
//!
//! ⚠️ WHY THIS FILE EXISTS AT ALL: the upstream project has no tests, and this is the part of it that fails
//! SILENTLY. A pattern that stops matching does not raise anything -- it simply never fires, and the symptom
//! is "the input method did something odd in one program", which is about the hardest kind of report to act
//! on. So the rules below are the USER'S OWN, copied out of the config.json they actually run, rather than
//! rules invented here: a test written from the code only proves the code agrees with itself.

use crate::model::{AppConfig, Rule, WindowInfo};

fn rule(json: &str) -> Rule {
    serde_json::from_str(json).expect("the rule should parse")
}

/// A WindowInfo with only the fields a given test cares about filled in.
///
/// ⚠️ `container` FILLS `ancestor_texts`, NOT `container_text`, AND THAT DISTINCTION COST A FAILING TEST.
/// They look like two names for one thing and they are not: `container_text` is the value the feature SHOWS
/// (one readable label, and the upstream code even falls back to the first ancestor when it is empty), while
/// the CONDITION reads `ancestor_texts` -- the whole chain, "any of them matches". A test that filled only the
/// friendly field built a window the rule could never match, and the failure read like a porting bug in a rule
/// the user actually runs. Copying upstream's own helper (`build_info`) is what makes this right: it sets both.
pub(crate) fn win(process: &str, control_text: &str, container: &str, window_class: &str, control_type: &str) -> WindowInfo {
    WindowInfo {
        process_name: process.into(),
        process_path: String::new(),
        window_title: String::new(),
        window_class: window_class.into(),
        control_text: control_text.into(),
        control_class: String::new(),
        control_type: control_type.into(),
        automation_id: String::new(),
        container_text: container.into(),
        ancestor_texts: if container.is_empty() { vec![] } else { vec![container.into()] },
        ancestor_classes: vec![],
        window_hwnd: 0,
        control_hwnd: 0,
        click_x: 0,
        click_y: 0,
    }
}

/// One rule from the user's config, with only the conditions they enabled.
fn user_rule(
    id: &str,
    priority: u32,
    action: &str,
    process: Option<&str>,
    window_class: Option<&str>,
    control_text: Option<&str>,
    control_type: Option<&str>,
    container: Option<&str>,
) -> String {
    let pat = |o: Option<&str>| o.unwrap_or("").to_string();
    format!(
        r#"{{"id":"{id}","name":"{id}","enabled":true,"priority":{priority},
        "match_target":"active_control","match_mode":"wildcard",
        "use_process":{},"use_window_title":false,"use_window_class":{},
        "use_control_text":{},"use_control_class":false,"use_control_type":{},
        "use_automation_id":false,"use_container_text":{},"use_ancestor_class":false,
        "process_pattern":"{}","window_title_pattern":"","window_class_pattern":"{}",
        "control_text_pattern":"{}","control_class_pattern":"",
        "control_type_pattern":"{}","automation_id_pattern":"",
        "container_text_pattern":"{}","ancestor_class_pattern":"",
        "action":"{action}"}}"#,
        process.is_some(),
        window_class.is_some(),
        control_text.is_some(),
        control_type.is_some(),
        container.is_some(),
        pat(process),
        pat(window_class),
        pat(control_text),
        pat(control_type),
        pat(container),
    )
}

// ---------------------------------------------------------------------------
// THE USER'S REAL RULES, ONE BY ONE.
// ---------------------------------------------------------------------------

#[test]
fn reaper_search_filter_wants_chinese() {
    // Their rule: reaper.exe AND control text "Search filter" AND container "Media Explorer", priority 99.
    let r = rule(&user_rule(
        "reaper-search", 99, "chinese",
        Some("reaper.exe"), None, Some("Search filter"), None, Some("Media Explorer"),
    ));
    let hit = win("reaper.exe", "Search filter", "Media Explorer", "REAPERwnd", "Edit");
    assert!(r.matches(&hit, &hit), "the rule's own case must match");

    // EVERY enabled condition, falsified one at a time. This is where an && that should be || hides: with all
    // three true the rule matches either way, so only the single-condition cases can tell them apart.
    for (what, miss) in [
        ("process", win("other.exe", "Search filter", "Media Explorer", "", "")),
        ("control text", win("reaper.exe", "Media Explorer", "Media Explorer", "", "")),
        ("container", win("reaper.exe", "Search filter", "Somewhere else", "", "")),
    ] {
        assert!(!r.matches(&miss, &miss), "a wrong {what} must break the rule");
    }
}

#[test]
fn chat2_wants_chinese_on_the_process_alone() {
    // Their chat2 rule is process-only. It must fire anywhere inside that program -- including in a control
    // whose text and container are empty, which is the normal case for a window that has not been filled in.
    let r = rule(&user_rule("chat2", 100, "chinese", Some("chat2.exe"), None, None, None, None));
    let w = win("chat2.exe", "", "", "Qt5152QWindowIcon", "");
    assert!(r.matches(&w, &w));
    assert!(!r.matches(&win("chat2b.exe", "", "", "", ""), &win("chat2b.exe", "", "", "", "")));
}

#[test]
fn explorer_address_bar_wants_english() {
    let r = rule(&user_rule(
        "explorer-address", 100, "english",
        Some("explorer.exe"), None, Some("搜索框"), None, None,
    ));
    let w = win("explorer.exe", "搜索框", "", "CabinetWClass", "Edit");
    assert!(r.matches(&w, &w));
    assert!(!r.matches(&win("explorer.exe", "文件夹", "", "", ""), &win("explorer.exe", "文件夹", "", "", "")));
}

#[test]
fn fileman_needs_both_class_and_process() {
    let r = rule(&user_rule(
        "fileman-edit", 100, "english",
        Some("fileman.exe"), Some("fileman.lister"), None, None, None,
    ));
    let w = win("fileman.exe", "", "", "fileman.lister", "");
    assert!(r.matches(&w, &w));
    // The same program in a DIFFERENT window class must not match: the class is half the condition, and a
    // version of this that ignored it would flip the IME across the whole file manager.
    assert!(!r.matches(&win("fileman.exe", "", "", "fileman.other", ""),
                       &win("fileman.exe", "", "", "fileman.other", "")));
}

#[test]
fn qq_needs_the_process_and_a_group_control() {
    let r = rule(&user_rule("qq", 100, "chinese", Some("chat.exe"), None, None, Some("Group"), None));
    assert!(r.matches(&win("chat.exe", "", "", "", "Group"), &win("chat.exe", "", "", "", "Group")));
    assert!(!r.matches(&win("chat.exe", "", "", "", "Edit"), &win("chat.exe", "", "", "", "Edit")));
}

// ---------------------------------------------------------------------------
// THE ENGINE'S OWN RULES -- the parts that are the same for every user.
// ---------------------------------------------------------------------------

#[test]
fn patterns_are_anchored_at_both_ends() {
    let r = rule(&user_rule("p", 1, "english", Some("reaper.exe"), None, None, None, None));
    assert!(r.matches(&win("REAPER.EXE", "", "", "", ""), &win("REAPER.EXE", "", "", "", "")),
            "matching is case-insensitive");
    // ⚠️ A SUBSTRING MATCH WOULD PASS EVERY TEST ABOVE AND STILL BE WRONG. `reaper.exe` must not match a name
    // that merely CONTAINS it -- otherwise a short pattern silently claims programs the user never listed,
    // and there is nothing on screen to say so.
    assert!(!r.matches(&win("notreaper.exe", "", "", "", ""), &win("notreaper.exe", "", "", "", "")));
    assert!(!r.matches(&win("reaper.exe.bak", "", "", "", ""), &win("reaper.exe.bak", "", "", "", "")));
    assert!(!r.matches(&win("reaper", "", "", "", ""), &win("reaper", "", "", "", "")));
}

#[test]
fn a_star_matches_a_run_and_a_question_mark_exactly_one() {
    let star = rule(&user_rule("s", 1, "english", Some("game*"), None, None, None, None));
    assert!(star.matches(&win("game.exe", "", "", "", ""), &win("game.exe", "", "", "", "")));
    assert!(star.matches(&win("gameplay", "", "", "", ""), &win("gameplay", "", "", "", "")));
    assert!(!star.matches(&win("mygame.exe", "", "", "", ""), &win("mygame.exe", "", "", "", "")));

    let q = rule(&user_rule("q", 1, "english", Some("tool?.exe"), None, None, None, None));
    assert!(q.matches(&win("tool1.exe", "", "", "", ""), &win("tool1.exe", "", "", "", "")));
    assert!(q.matches(&win("tools.exe", "", "", "", ""), &win("tools.exe", "", "", "", "")));
    assert!(!q.matches(&win("tool.exe", "", "", "", ""), &win("tool.exe", "", "", "", "")),
            "? is exactly one character, not zero");
    assert!(!q.matches(&win("tool12.exe", "", "", "", ""), &win("tool12.exe", "", "", "", "")));
}

#[test]
fn a_disabled_condition_is_not_a_condition() {
    // use_control_text is false and the pattern is empty. If a disabled condition were still collected, the
    // rule would demand an empty control text -- and would then match almost nothing, silently.
    let r = rule(&user_rule("only-process", 1, "english", Some("fileman.exe"), None, None, None, None));
    assert!(r.matches(&win("fileman.exe", "anything", "whatever", "", ""),
                      &win("fileman.exe", "anything", "whatever", "", "")));
}

#[test]
fn the_list_order_is_the_order_the_engine_tries_and_the_first_match_wins() {
    // ⚠️⚠️ THE ORDER IS THE MANUAL ONE, WHICH IS A CHANGE THE USER MADE: "优先级与排序无关，这个只能手动定义."
    //
    // This test used to assert the opposite -- that the priority numbers decided the order (the user's reaper
    // rules were 99 and 100 for exactly that reason). They no longer do: the engine tries the rules in the order
    // the list shows them, and the list is ordered by hand (dragging). The priority field is the user's own note.
    //
    // So the property that survives is the one that always mattered: THE FIRST MATCH WINS, and therefore the
    // SPECIFIC rule has to come FIRST. It is the user's job to put it there -- and the list is where they can see
    // the order they made.
    let mut cfg = AppConfig {
        rules: vec![
            // Written in the order they are tried: the specific one first.
            rule(&user_rule("search-filter", 100, "chinese", Some("reaper.exe"), None, Some("Search filter"), None, None)),
            rule(&user_rule("general-reaper", 100, "english", Some("reaper.exe"), None, None, None, None)),
        ],
        ..Default::default()
    };
    cfg.migrate();

    let order = cfg.active_rules();
    assert_eq!(order[0].id, "search-filter", "the file order is the order they are tried");
    assert_eq!(order[1].id, "general-reaper");

    // ... and the first match wins, which is what makes the order mean anything.
    let w = win("reaper.exe", "Search filter", "", "", "");
    let winner = order.iter().find(|r| r.matches(&w, &w)).expect("one of them matches");
    assert_eq!(winner.action, crate::model::Action::Chinese, "the specific rule's action wins");

    // The same window WITHOUT the search filter falls through to the general rule.
    let w2 = win("reaper.exe", "Arrange", "", "", "");
    let winner2 = order.iter().find(|r| r.matches(&w2, &w2)).expect("the general rule matches");
    assert_eq!(winner2.action, crate::model::Action::English);

    // ⚠️ AND THE PRIORITY NUMBERS ARE IGNORED, which is the other half of the change and worth pinning: the same
    // two rules, with the numbers swapped, are tried in the same order.
    let mut swapped = cfg.clone();
    swapped.rules[0].priority = 200;   // the specific rule, now "later" by number
    swapped.rules[1].priority = 1;
    let order2 = swapped.active_rules();
    assert_eq!(order2[0].id, "search-filter", "the numbers no longer decide anything");
    let winner3 = order2.iter().find(|r| r.matches(&w, &w)).expect("one of them matches");
    assert_eq!(winner3.action, crate::model::Action::Chinese, "so the specific rule still wins");
}

#[test]
fn a_disabled_rule_is_never_evaluated() {
    let mut cfg = AppConfig { rules: vec![], ..Default::default() };
    let mut r = rule(&user_rule("off", 1, "english", Some("*"), None, None, None, None));
    r.enabled = false;
    cfg.rules.push(r);
    cfg.migrate();
    assert!(cfg.active_rules().is_empty(), "a switched-off rule must not be in the engine's list");
}

#[test]
fn an_empty_rule_matches_everything_and_that_is_the_documented_behaviour() {
    // No condition enabled -> `all()` over an empty list is TRUE. It is worth pinning because it is the one
    // way a half-built rule can swallow every program on the machine, and a user who ticks nothing and saves
    // should get the rule they wrote, not a surprise.
    let r = rule(&user_rule("empty", 1, "english", None, None, None, None, None));
    assert!(r.matches(&win("anything.exe", "", "", "", ""), &win("anything.exe", "", "", "", "")));
}

#[test]
fn the_users_config_file_parses() {
    // The real file, as deployed. It is 9 rules with every field the panel writes, and a parse failure here
    // means the feature would start with NO rules at all -- the quietest possible way to be broken.
    let text = include_str!("../tests/fixtures/user_config.json");
    let cfg: AppConfig = serde_json::from_str(text).expect("the user's config.json must parse");
    assert_eq!(cfg.rules.len(), 9, "all nine of their rules");
    assert_eq!(cfg.switch_method, crate::model::SwitchMethod::Ime);
    assert_eq!(cfg.ime_toggle_hotkey, "Ctrl+Space");
    // And every one of them survives `migrate()` and is active (none is disabled in the fixture).
    assert_eq!(cfg.active_rules().len(), 9);
}

// ---------------------------------------------------------------------------
// WHOSE WINDOW IS THIS? -- the guard that keeps the rules out of Apex's own windows.
//
// ⚠️ WHY THESE ARE TESTS AND NOT A COMMENT: the shipped version compared PROCESS NAMES, so it recognised
// apex.exe and let apex-settings.exe -- the settings panel, the one place the user is definitely looking while
// they read their rules -- through as an ordinary program. It was found in the feature's own log
// (`SWITCH ... proc=apex-settings.exe`), not by reading the code, and the code's comment claimed the panel WAS
// handled. These pin the folder judgement instead.

use crate::model::{installation_of, is_apex_itself};
use std::path::Path;

/// A captured window that knows where its program lives. `win()` leaves the path empty on purpose (the path is
/// what a failed query leaves behind), so tests about paths fill it in here.
fn win_at(process: &str, path: &str) -> WindowInfo {
    let mut w = win(process, "", "", "", "");
    w.process_path = path.into();
    w
}

const INSTALLATION: &str = r"D:\App protable\Apex";
const FEATURE_DIR: &str = r"D:\App protable\Apex\Plugins\AutoIME\";

#[test]
fn the_installation_is_two_levels_up_from_the_feature_folder() {
    // The host's answer carries a TRAILING SEPARATOR (measured: `AutoIME: folder D:\App
    // protable\Apex\Plugins\AutoIME\`), and `Path::parent` has to see through it. If it did not, the root would
    // come out as `...\Plugins` and EVERY window would be "not Apex" -- or, with one more slip, every window
    // WOULD be Apex and the rules would never fire anywhere.
    assert_eq!(
        installation_of(Path::new(FEATURE_DIR)),
        Some(std::path::PathBuf::from(INSTALLATION))
    );
    // Once more without the trailing separator, which is how a hand-built test path usually looks.
    assert_eq!(
        installation_of(Path::new(r"D:\App protable\Apex\Plugins\AutoIME")),
        Some(std::path::PathBuf::from(INSTALLATION))
    );
    // Too short to have an installation: the caller must fall back rather than be handed a wrong root.
    assert_eq!(installation_of(Path::new(r"AutoIME")), None);
}

#[test]
fn the_settings_panel_is_apex_itself_and_the_exe_name_alone_would_not_say_so() {
    // ⚠️ THE BUG, IN ONE ASSERTION. Both programs are in the installation folder; only one of them is called
    // apex.exe. The guard is handed the host's name -- as the running code does -- and must still recognise the
    // panel, because the folder is the answer.
    let panel = win_at("apex-settings.exe", r"D:\App protable\Apex\apex-settings.exe");
    assert!(
        is_apex_itself(&panel, Some(Path::new(INSTALLATION)), "apex.exe"),
        "the settings panel runs out of Apex's folder, so it IS Apex"
    );
    // The same window with the name comparison alone -- what shipped -- would have let it through. Stated as an
    // assertion so that a future change back to names fails HERE rather than in the user's typing.
    assert_ne!(
        panel.process_name, "apex.exe",
        "the two programs differ by name; that is exactly why names are the wrong judgement"
    );
    // And the host itself is still recognised.
    let host = win_at("apex.exe", r"D:\App protable\Apex\apex.exe");
    assert!(is_apex_itself(&host, Some(Path::new(INSTALLATION)), "apex.exe"));
}

#[test]
fn a_program_in_another_folder_is_not_apex_even_with_the_same_prefix() {
    // ⚠️ PORTABLE MEANS COPIES SIT SIDE BY SIDE, so the comparison must be the WHOLE folder and never a prefix:
    // `D:\App protable\Apex-backup` starts with the installation's path and is a different installation -- one
    // whose own windows have no business being skipped.
    for path in [
        r"D:\App protable\Apex-backup\apex-settings.exe",
        r"D:\App protable\Apex2\apex.exe",
        r"D:\App protable\Apex\Plugins\AutoIME\helper.exe",
        r"C:\Program Files\Mozilla Firefox\browser.exe",
    ] {
        let name = path.rsplit('\\').next().unwrap();
        assert!(
            !is_apex_itself(&win_at(name, path), Some(Path::new(INSTALLATION)), "apex.exe"),
            "{path} is not this installation"
        );
    }
}

#[test]
fn the_folder_comparison_ignores_case_and_separators() {
    // Windows paths are case-insensitive and a program may report its own path with either separator.
    let w = win_at("apex-settings.exe", "d:/APP PROTABLE/apex/Apex-Settings.EXE");
    assert!(is_apex_itself(&w, Some(Path::new(INSTALLATION)), "apex.exe"));
    // A trailing separator on the ROOT is fine too -- the host hands the feature folder over with one.
    let w2 = win_at("apex.exe", r"D:\App protable\Apex\apex.exe");
    assert!(is_apex_itself(&w2, Some(Path::new(r"D:\App protable\Apex\")), "apex.exe"));
}

#[test]
fn an_unreadable_path_falls_back_to_the_exe_name() {
    // ⚠️ `process_path` IS THE EXE NAME when the query failed (an elevated program -- see process_path in
    // win32.rs), so there is no folder to compare and the name is all that is left. It must not be read as
    // "somewhere in the installation".
    let unknown = win_at("browser.exe", "browser.exe");
    assert!(!is_apex_itself(&unknown, Some(Path::new(INSTALLATION)), "apex.exe"));

    let host = win_at("apex.exe", "apex.exe");
    assert!(is_apex_itself(&host, Some(Path::new(INSTALLATION)), "apex.exe"));

    // ⚠️ AND THE FALLBACK'S KNOWN LOOSENESS IS PINNED RATHER THAN LEFT TO BE DISCOVERED: with no installation
    // known (`None` -- init could not work the folder out) and no folder in the path, ANY program called
    // apex.exe is treated as Apex. It is the safe direction to be wrong in -- a missed exclusion flips the
    // input method once in Apex's own window, while a wrong exclusion silently stops the rules working in a
    // program the user cares about.
    let stranger = win_at("apex.exe", r"C:\somewhere else\apex.exe");
    assert!(is_apex_itself(&stranger, None, "apex.exe"));

    // ⚠️ BUT THE FALLBACK MUST NOT OVERRULE A FOLDER THAT WAS READ. With an installation known, the same
    // stranger is just another program -- this is the case that was wrong for one test run.
    assert!(
        !is_apex_itself(&stranger, Some(Path::new(INSTALLATION)), "apex.exe"),
        "a readable path in someone else's folder is not this installation"
    );
}
