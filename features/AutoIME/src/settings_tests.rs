//! The settings page's own logic: the document it builds, and the edits it accepts.
//!
//! ⚠️ THE INDEX TRANSLATION IS THE REASON MOST OF THIS FILE EXISTS. The page shows rules in the order the
//! engine tries them (sorted by priority, then name) because that order is what decides which rule wins -- but
//! the config file keeps them in written order. So the page's "number 3" is not the file's "index 3", and an
//! edit addressed by the page's number has to be translated before it touches anything. Getting that wrong
//! edits a DIFFERENT rule than the box the user clicked, silently, and only while the two orders differ --
//! which is the kind of bug that reproduces on one machine and not on the next.

use crate::model::{Action, AppConfig, Rule};
// ONE `win` HELPER FOR BOTH TEST MODULES -- see its own note in tests.rs. A second copy here is how two tests
// come to disagree about what an empty field means.
use crate::tests::win;
use crate::settings;

/// A LOCK SHARED BY EVERY TEST IN THIS FILE, because they all install a fixture into the SAME process-wide
/// config slot.
///
/// ⚠️ WITHOUT IT THEY OVERWRITE EACH OTHER'S FIXTURES, AND THE FAILURES LOOK LIKE CODE BUGS. Cargo runs tests
/// in PARALLEL by default, so a test that set up three rules could find zero by the time it looked -- five
/// tests failed together that way, each reporting a different wrong value. The lock makes them serial, which
/// is the honest model: the settings module really does have one config, and only one process has it.
///
/// (The alternative -- a per-test config -- would mean the module took a config parameter instead of holding
/// one. That is the cleaner design for a library and the wrong one here: the host calls into a C ABI with no
/// place to pass a context, so the feature has to keep its state somewhere process-wide.)
static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Install a fixture and hold the lock for the duration of the test.
///
/// ⚠️ THIS IS THE ONLY WAY A TEST MAY INSTALL A FIXTURE, AND THAT IS CHECKED RATHER THAN ASKED FOR.
/// `set_config` is public because the host calls it at init; a test that used it directly would look identical
/// and quietly opt out of the lock. One did, and the symptom was a rule count that was occasionally 2 instead
/// of 1 -- see the note in adding_a_rule_creates_one_that_matches_nothing. So `a_test_may_not_install_a_fixture_directly`
/// below reads this very file and fails if any test outside this helper calls set_config.
fn use_config(cfg: AppConfig) -> std::sync::MutexGuard<'static, ()> {
    let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner()); // a panic in one test must not poison the rest
    settings::set_config(std::sync::Arc::new(std::sync::Mutex::new(cfg))); // ALLOWED: use_config is the only installer
    // ⚠️ AND THE DRAFT IS CLEARED WITH THE CONFIG, BECAUSE IT IS THE SAME KIND OF STATE.
    //
    // The draft is process-wide, like the config and for the same reason (the host calls in through a C ABI
    // with nowhere to pass a context). A test that opened one and did not commit left it for whichever test ran
    // next -- and `settings_json` DRAWS the draft, so a later test found another test's rule in its own
    // document. The symptom was an assertion about rule order failing in a test that never edits anything:
    // "left: second, right: specific". One line here, and every test starts from a known world.
    settings::debug_clear_draft();
    guard
}

/// The guard above, applied to itself.
///
/// A rule about how tests are written is worth exactly as much as the check that enforces it: this file's
/// header explains the shared-config hazard, and that explanation did not stop the hazard from coming back.
/// So the rule is textual and blunt -- the call may appear on exactly one line in this file, and that line says
/// so. The needle is assembled at runtime, because a check whose own source contains the string it forbids has
/// to exempt itself, and an exemption granted to the checking line is an exemption that also covers a real
/// bypass typed onto that same line.
#[test]
fn a_test_may_not_install_a_fixture_directly() {
    const SELF: &str = include_str!("settings_tests.rs");
    let needle = concat!("settings::", "set_config");
    const MARKER: &str = "ALLOWED: use_config is the only installer";
    let mut offenders: Vec<String> = Vec::new();
    for (i, line) in SELF.lines().enumerate() {
        // Code only: a comment that names the call is documentation, not a bypass.
        let code = line.split("//").next().unwrap_or("");
        if code.contains(needle) && !line.contains(MARKER) {
            offenders.push(format!("line {}: {}", i + 1, line.trim()));
        }
    }
    assert!(
        offenders.is_empty(),
        "a test installed its fixture without the lock (call use_config instead):\n  {}",
        offenders.join("\n  ")
    );
}

fn rule(id: &str, name: &str, priority: u32, action: Action) -> Rule {
    let mut r = Rule::default();
    r.id = id.into();
    r.name = name.into();
    r.priority = priority;
    r.action = action;
    r
}

/// A config whose FILE order and DISPLAY order are deliberately different.
///
/// Written:  general(100), specific(99), middle(100)
/// Sorted:   specific(99), general(100,"general"), middle(100,"middle")  -- note "general" < "middle"
///
/// So display 0 is file 1, display 1 is file 0, display 2 is file 2. A translation that forgot the sort would
/// send display 0 to file 0 -- a different rule.
fn shuffled() -> AppConfig {
    AppConfig {
        rules: vec![
            rule("general", "general", 100, Action::English),
            rule("specific", "specific", 99, Action::Chinese),
            rule("middle", "middle", 100, Action::English),
        ],
        ..Default::default()
    }
}

#[test]
fn the_display_index_is_the_file_position() {
    // ⚠️ THE TRANSLATION COLLAPSED TO THE IDENTITY, AND THAT IS A RESULT RATHER THAN A LOSS. The page's number
    // used to mean "the nth rule the ENGINE would try", which was a SORT of the file -- so every write had to be
    // translated, and getting it wrong edited a different rule than the one on screen (this file's header is
    // about exactly that). The user has since decided the order is the manual one ("优先级与排序无关，这个只能
    // 手动定义"): the engine tries the rules in the order the list shows them, and the list shows them in the
    // order they are written. So the two orders are one order, and the translation is `Some(index)`.
    //
    // It is still a function, and still the only place that answers the question -- because "the page's number
    // and the file's position are the same thing" is a property to state once rather than to assume at each of
    // the six call sites.
    let _g = use_config(AppConfig::default());
    let mut cfg = shuffled();
    assert_eq!(cfg.real_index_of_display(0), Some(0), "display 0 is file 0");
    assert_eq!(cfg.real_index_of_display(1), Some(1));
    assert_eq!(cfg.real_index_of_display(2), Some(2));

    // And writing through it lands on the rule the page meant.
    let r = cfg.rule_at_display_index(0).expect("display 0 exists");
    assert_eq!(r.id, "general", "shuffled() writes general first");
    r.name = "renamed".into();
    assert_eq!(cfg.rules[0].name, "renamed", "file position 0 was the one edited");
    assert_eq!(cfg.rules[1].name, "specific", "and its neighbour was left alone");
}

#[test]
fn out_of_range_is_none_rather_than_a_wrap() {
    let _g = use_config(AppConfig::default());
    let mut cfg = shuffled();
    assert_eq!(cfg.real_index_of_display(3), None);
    assert!(cfg.rule_at_display_index(99).is_none());
}

#[test]
fn moving_a_rule_lands_it_exactly_where_it_was_asked_to_go() {
    // ⚠️ THE DISPLAY IS THE FILE ORDER NOW, so a move moves the ENTRY -- and the note that used to stand here
    // ("swapping the file positions changes nothing at all, because the display is DERIVED from the sort") was
    // the right answer to a question that has since been answered differently by the user: 优先级与排序无关.
    let _g = use_config(AppConfig::default());
    let mut cfg = shuffled();
    assert!(cfg.move_display_item(1, 0), "move 'specific' up one place");
    let after: Vec<String> = cfg.sorted_rules().iter().map(|r| r.name.clone()).collect();
    assert_eq!(after, vec!["specific", "general", "middle"], "only the two neighbours changed places");

    // ⚠️ AND IT IS STICKY BY CONSTRUCTION NOW: the list IS the file, so there is no sort left to undo it.
    let again: Vec<String> = cfg.sorted_rules().iter().map(|r| r.name.clone()).collect();
    assert_eq!(again, after);

    // Downwards, on the same data.
    let mut cfg2 = shuffled();
    assert!(cfg2.move_display_item(0, 2), "move 'general' down to the end");
    let d: Vec<String> = cfg2.sorted_rules().iter().map(|r| r.name.clone()).collect();
    assert_eq!(d, vec!["specific", "middle", "general"]);
}

#[test]
fn a_move_leaves_every_priority_alone() {
    // ⚠️ THIS IS THE OPPOSITE OF WHAT THE TEST HERE USED TO ASSERT, AND THE REVERSAL IS THE USER'S DECISION.
    // A move used to renumber every priority 1..n to make the order stick -- so the numbers the user had set by
    // hand were rewritten every time they dragged something, which is what they reported as "现在有些乱了". The
    // priority is theirs; the order is dragged. A move must not touch a single one of them.
    let _g = use_config(AppConfig::default());
    let mut cfg = shuffled();
    let mut before: Vec<(String, u32)> = cfg.rules.iter().map(|r| (r.id.clone(), r.priority)).collect();
    assert!(cfg.move_display_item(2, 0));
    let mut after: Vec<(String, u32)> = cfg.rules.iter().map(|r| (r.id.clone(), r.priority)).collect();
    // ⚠️ COMPARED AS A SET, NOT AS A LIST: a move is *supposed* to change the list order -- that is the whole
    // operation. What must not change is which number belongs to which rule.
    before.sort();
    after.sort();
    assert_eq!(after, before, "the same rules, carrying the same priorities, in a different order");
}

#[test]
fn a_move_out_of_range_reports_refusal() {
    let _g = use_config(AppConfig::default());
    let mut cfg = shuffled();
    assert!(!cfg.move_display_item(0, 9), "past the end");
    assert!(!cfg.move_display_item(9, 0), "from beyond the end");
    let after: Vec<String> = cfg.sorted_rules().iter().map(|r| r.name.clone()).collect();
    assert_eq!(after, vec!["general", "specific", "middle"], "and nothing moved");
}

#[test]
fn the_document_is_valid_json_and_carries_every_rule() {
    // Built through the real entry point, into a buffer, exactly as the host asks for it -- so the buffer
    // handling (the sizing, the terminator) is exercised too, not just the serialisation.
    let _g = use_config(shuffled());

    let mut buf = vec![0u8; 64 * 1024];
    let n = settings::settings_json(buf.as_mut_ptr() as *mut std::os::raw::c_char, buf.len() as i32);
    assert!(n > 0, "a document was written");
    let text = std::str::from_utf8(&buf[..n as usize]).expect("utf-8");
    let doc: serde_json::Value = serde_json::from_str(text).expect("the document must parse");

    let params = doc["params"].as_array().expect("params is an array");
    // switch_method (select), ime_toggle_hotkey (hotkey), rules (group) -- in that order.
    assert_eq!(params.len(), 3);
    assert_eq!(params[0]["type"], "select");
    // ⚠️ `hotkey`, NOT `text`: this one is RECORDED by pressing the keys, not typed ("切换热键点击后是记录新的
    // 快捷键"). The page draws whichever type it is told, so this line is what makes the control a recorder.
    assert_eq!(params[1]["type"], "hotkey");
    assert_eq!(params[1]["id"], "ime_toggle_hotkey");
    assert!(!params[1]["placeholderZh"].as_str().unwrap_or("").is_empty(),
            "a recorder needs to say what to do with it");

    let rules = &params[2];
    assert_eq!(rules["type"], "group");
    assert_eq!(rules["id"], "rules");
    let items = rules["items"].as_array().expect("items");
    assert_eq!(items.len(), 3, "every rule is shown, including a disabled one");
    // In FILE order, which is now the same thing as display order -- see `the_display_index_is_the_file_position`.
    assert_eq!(items[0]["values"]["name"], "general");
    assert_eq!(items[1]["values"]["name"], "specific");

    // ⚠️ THE ROW'S TITLE IS NOT THE RULE'S NAME: "规则名简化，只显示进程名即可，扩展名也不用显示." The list shows
    // the program (`shuffled()`'s rules have none, so it falls back to the name here), while the `name` field
    // stays the user's own label and is what the detail pane edits.
    assert_eq!(items[0]["title"], "general", "no process pattern -> the rule's own name");

    // The field schema must cover every field `set_rule_field` accepts, or a field would be unreachable from
    // the page: this is the list/parser pair that has to agree.
    let fields = rules["fields"].as_array().expect("fields");
    let ids: Vec<&str> = fields.iter().map(|f| f["id"].as_str().unwrap()).collect();
    for want in [
        "name", "enabled", "priority", "action", "match_mode",
        "process_pattern", "window_title_pattern", "window_class_pattern",
        "control_text_pattern", "control_class_pattern", "control_type_pattern",
        "automation_id_pattern", "container_text_pattern", "ancestor_class_pattern",
    ] {
        assert!(ids.contains(&want), "the schema must describe `{want}`");
    }

    // ⚠️ AND THE GROUP DECLARES THE BUTTONS IT NEEDS, WHICH IS THE OTHER HALF OF "capture" BEING USABLE.
    // The op is implemented (see list_op) and the panel draws whatever is declared (see `actions` in
    // apex/abi.h) -- but if this array were empty the button would simply never appear, and nothing anywhere
    // would report a fault: the feature would look like a feature without captures, and the user would have no
    // way to tell that from a button they could not find. So the declaration is asserted here.
    let actions = rules["actions"].as_array().expect("actions is an array");
    let ops: Vec<&str> = actions.iter().map(|a| a["op"].as_str().unwrap()).collect();
    assert!(ops.contains(&"capture"), "the rules group must offer the capture button");
    // A label per language, or the page falls back to the op name ("capture") in the user's UI.
    for a in actions {
        assert!(!a["labelZh"].as_str().unwrap_or("").is_empty(), "a button needs a Chinese label");
        assert!(!a["labelEn"].as_str().unwrap_or("").is_empty(), "a button needs an English label");
    }
}

/// The document, parsed, the way the page receives it. Through the real entry point into a buffer, so the
/// buffer handling is exercised too -- see `the_document_is_valid_json_and_carries_every_rule`.
fn document() -> serde_json::Value {
    let mut buf = vec![0u8; 64 * 1024];
    let n = settings::settings_json(buf.as_mut_ptr() as *mut std::os::raw::c_char, buf.len() as i32);
    assert!(n > 0, "the feature must produce a document");
    serde_json::from_str(std::str::from_utf8(&buf[..n as usize]).expect("utf-8")).expect("json")
}

fn rules_group(doc: &serde_json::Value) -> &serde_json::Value {
    &doc["params"][2]
}

fn begin_edit(index: i32) -> i32 {
    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("begin-edit").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), index)
}

/// ⚠️⚠️ THE PAGE MUST NOT HAVE TO GUESS WHICH ROW IT IS EDITING, AND THESE TESTS ARE THAT RULE.
///
/// The draft lives in this module, so this module is the only side that can answer the question. The page used
/// to answer it itself, and to guess where a new rule had landed -- it picked the LAST row, on the assumption
/// that `add` appends, while the list was drawn in a different order entirely. The user's report for this whole
/// area was "规则添加，编辑，保存功能都不完善", and this is the part of it that silently edits the wrong rule.
#[test]
fn the_document_names_the_row_that_is_being_edited() {
    let _g = use_config(shuffled());
    // Nothing open: the page is told so, rather than being left to assume.
    assert_eq!(rules_group(&document())["editing"], -1);

    // `shuffled()` writes general / specific / middle, and the list shows the file order.
    assert_eq!(begin_edit(1), 1);
    let doc = document();
    assert_eq!(rules_group(&doc)["editing"], 1, "the feature says which row is open");
    let items = rules_group(&doc)["items"].as_array().expect("items");
    assert_eq!(items[1]["values"]["name"], "specific", "and it is the row the page asked for");

    // And it follows the rule rather than the position: a commit closes it.
    let id = std::ffi::CString::new("rules").unwrap();
    let commit = std::ffi::CString::new("commit-edit").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), commit.as_ptr(), empty.as_ptr(), 1), 1);
    assert_eq!(rules_group(&document())["editing"], -1, "nothing is open after the commit");
}

#[test]
fn a_new_rule_is_opened_for_editing_where_it_actually_lands() {
    // The new rule goes to the TOP ("新添加规则放在最上面"), so a page that assumed "the last row" would be
    // editing whatever happened to be at the bottom -- somebody else's rule.
    let cfg = AppConfig {
        rules: vec![
            rule("high1", "aaa", 150, Action::English),
            rule("high2", "bbb", 200, Action::English),
        ],
        ..Default::default()
    };
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("add").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), -1), 1);

    let doc = document();
    let group = rules_group(&doc);
    let items = group["items"].as_array().expect("items");
    assert_eq!(items.len(), 3, "the new rule is in the list");
    let new_at = items
        .iter()
        .position(|i| i["values"]["name"] == "新规则")
        .expect("the new rule is there by name");
    assert_eq!(new_at, 0, "a new rule goes to the top of the list");
    assert_eq!(
        group["editing"], new_at as i64,
        "the document points at the row the new rule really is, not at the last row"
    );
    assert_ne!(
        group["editing"],
        (items.len() - 1) as i64,
        "which is exactly the row the page's old guess would have selected"
    );

    // ⚠️ AND THE DRAFT IS A COPY OF THE NEW RULE, NOT OF WHOEVER WAS AT THAT POSITION. Without this, typing
    // into the opened editor would write the new rule's fields over an existing rule on Save.
    let real = document();
    let items = rules_group(&real)["items"].as_array().expect("items");
    assert_eq!(items[1]["values"]["name"], "aaa", "the rule the old guess would have edited is untouched");
}

#[test]
fn the_checkbox_is_the_flag_and_the_text_does_not_touch_it() {
    // ⚠️ THIS REPLACES THE OPPOSITE TEST, AND THE REVERSAL IS THE USER'S DECISION: "各参数原先是有个复选框，打钩了
    // 才生效，新规则默认只有进程名是钩起来的."
    //
    // The port made the flag FOLLOW the text (a non-empty pattern turned its `use_*` on) because the flag had no
    // control of its own. Now it has one -- the checkbox beside the field -- and the two must not fight:
    // a flag that followed the text would untick itself while the user was clearing the box to type something
    // else, and would tick itself while they were still filling the pattern in.
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("r", "r", 1, Action::English));
    let _g = use_config(cfg);

    let text = std::ffi::CString::new("rules[0].process_pattern").unwrap();
    let flag = std::ffi::CString::new("rules[0].use_process").unwrap();
    let set = |p: &std::ffi::CString, v: &str| {
        let value = std::ffi::CString::new(v).unwrap();
        settings::set_control(p.as_ptr(), value.as_ptr())
    };

    // A rule starts with the flag on and an empty pattern (`Rule::default()`), which is exactly "进程名 is the
    // only one ticked" in a brand-new rule.
    assert!(settings::debug_config().unwrap().rules[0].use_process);

    // Typing a pattern does NOT change the flag: it is the checkbox's business.
    assert_eq!(set(&text, "notepad++.exe"), 1);
    let c = settings::debug_config().unwrap();
    assert_eq!(c.rules[0].process_pattern, "notepad++.exe");
    assert!(c.rules[0].use_process);

    // Unticking keeps the pattern -- which is the whole reason the checkbox is back: without it, switching a
    // condition off would mean deleting the pattern, and a captured pattern is the most valuable thing in a rule.
    assert_eq!(set(&flag, "0"), 1);
    let c = settings::debug_config().unwrap();
    assert!(!c.rules[0].use_process, "the box is off");
    assert_eq!(c.rules[0].process_pattern, "notepad++.exe", "and the pattern is still there");

    // Clearing the text with the box ticked leaves a rule with a ticked, empty condition -- which the ENGINE
    // ignores (see `push_condition`), so it is inert rather than matching everything.
    assert_eq!(set(&flag, "1"), 1);
    assert_eq!(set(&text, ""), 1);
    let c = settings::debug_config().unwrap();
    assert!(c.rules[0].use_process && c.rules[0].process_pattern.is_empty());
    let w = win("anything.exe", "", "", "", "");
    assert!(c.rules[0].matches(&w, &w),
            "an empty pattern takes no part, so the rule has no conditions left and matches -- which is why a \
             new rule is created switched OFF (see the `add` branch)");
}

#[test]
fn an_unknown_path_is_refused_rather_than_ignored() {
    let _g = use_config(AppConfig::default());
    for bad in ["rules[0].nonsense", "rules[99].name", "not_a_group", "rules[0]", "rules[x].name"] {
        let p = std::ffi::CString::new(bad).unwrap();
        let v = std::ffi::CString::new("x").unwrap();
        assert_eq!(settings::set_control(p.as_ptr(), v.as_ptr()), 0, "`{bad}` must be refused");
    }
}

#[test]
fn adding_a_rule_creates_one_that_matches_nothing() {
    // A new rule has no patterns, and an empty pattern is not a condition -- so it matches NOTHING until the
    // user types something. A rule that matched everything would flip the input method across the whole
    // machine the instant it appeared.
    //
    // ⚠️ THROUGH use_config, NOT set_config. Every test here shares ONE global config and cargo runs tests in
    // parallel, so the lock is the only thing keeping them apart. This test used to install its config
    // directly, which is why the suite once produced "assert_eq!(g.rules.len(), 1): left 2, right 1" and never
    // reproduced on its own: a_new_rule_must_not_match_everything was adding its rule to the config this test
    // had just put in place, so each of them found both rules. It read as a flaky assertion about rule counts;
    // it was one test skipping the lock.
    let _g = use_config(AppConfig::default());

    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("add").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), -1), 1);

    let g = settings::debug_config().expect("config");
    assert_eq!(g.rules.len(), 1);
    // ⚠️ SWITCHED OFF, and this assertion is the point of the test (see a_new_rule_must_not_match_everything
    // for the arithmetic). Somewhere between writing this test and running it, the intent moved from "an empty
    // enabled rule happens to match nothing" to "an empty rule must not be enabled at all" -- the first is
    // FALSE (an empty rule matches everything), and expecting it is how this line was wrong.
    assert!(!g.rules[0].enabled, "a new rule is created switched OFF");
    assert!(!g.rules[0].id.is_empty(), "with an id of its own");
    assert!(g.active_rules().is_empty(), "so the engine ignores it until it is filled in and enabled");
}

#[test]
fn removing_uses_the_page_index() {
    // The bug this pins: removing by the page's number without translating it deletes a different rule than
    // the one whose button was pressed. (The translation is the identity now -- the list is the file order --
    // but the RULE is unchanged: the number comes from the page and is the page's alone.)
    let _g = use_config(shuffled());
    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("remove").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), 0), 1);

    let g = settings::debug_config().expect("config");
    let names: Vec<String> = g.sorted_rules().iter().map(|r| r.name.clone()).collect();
    assert!(!names.contains(&"general".to_string()), "row 0 was 'general', so that is what went");
    assert!(names.contains(&"specific".to_string()) && names.contains(&"middle".to_string()));
}

#[test]
fn a_new_rule_must_not_match_everything() {
    // ⚠️ THE DANGEROUS ONE. `Rule::default()` (upstream's own new-rule shape) enables `use_process` with an
    // EMPTY pattern -- and an empty pattern is not a condition, so `all()` over zero conditions is TRUE: the
    // rule matches EVERY program on the machine.
    //
    // Upstream had the same shape, but it was reachable only from its own editor, where the user typed a
    // pattern before saving. A "+" button in a settings page makes it reachable in one click -- and for as
    // long as the rule stays that way, the input method flips in every program the user touches. So a new
    // rule is created SWITCHED OFF: the user fills it in and then turns it on, which is one deliberate act
    // and leaves the feature inert in the meantime.
    let _g = use_config(AppConfig::default());
    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("add").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), -1), 1);

    let g = settings::debug_config().expect("config");
    assert_eq!(g.rules.len(), 1);
    assert!(!g.rules[0].enabled, "a new rule is created switched OFF");
    assert!(g.active_rules().is_empty(), "so the engine ignores it until it is filled in and enabled");

    // And the reason is checked directly, so this test cannot pass for the wrong reason: with the rule
    // enabled and no patterns, the engine really would match everything.
    let mut probe = g.rules[0].clone();
    probe.enabled = true;
    let any = crate::model::WindowInfo {
        process_name: "anything.exe".into(), process_path: String::new(), window_title: String::new(),
        window_class: String::new(), control_text: String::new(), control_class: String::new(),
        control_type: String::new(), automation_id: String::new(), container_text: String::new(),
        ancestor_texts: vec![], ancestor_classes: vec![],
        window_hwnd: 0, control_hwnd: 0, click_x: 0, click_y: 0,
    };
    assert!(probe.matches(&any, &any), "the hazard is real, not hypothetical");
}

// ⚠️ TWO TESTS ABOUT THE READOUT USED TO STAND HERE, AND THEY WENT WITH IT.
//
// They pinned "the readout names the conditions that decide" and "it says when there are no rules at all" --
// real properties of a panel that no longer exists: "自动输入法页，最上方的状态提示去了." Keeping them would have
// meant keeping `active_conditions`, `live_summary` and `rule_count` alive for a display nothing draws, which is
// the kind of "tested but unreachable" code this project has been bitten by before (see `#![deny(dead_code)]`).
//
// What they were protecting is not lost: the ENGINE's own rule -- a condition takes part exactly when its box is
// ticked AND its pattern is not empty -- is pinned by `a_disabled_condition_is_not_a_condition` and by the
// user's-rule tests in tests.rs, which are about matching rather than about printing.

#[test]
fn a_capture_fills_the_rule_and_switches_it_on() {
    // THE DATA HALF OF "点击捕获" -- what a captured control does to the rule it was captured for. The window
    // half (waiting for the click) needs a screen and belongs to the user's own testing; this is the part that
    // can be pinned here, and it is the part where a mistake is silent: a capture that fills the wrong fields
    // produces a rule that looks right and never matches.
    let _g = use_config(AppConfig::default());

    // Start from the page's own "new rule" -- disabled, empty -- so this also covers the case the user is
    // actually in when they press the button.
    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("add").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), -1), 1);

    let mut cfg = settings::debug_config().expect("config");
    assert!(!cfg.rules[0].enabled, "a fresh rule starts switched off");

    let info = crate::tests::win("explorer.exe", "在 搜索框 中搜索", "", "CabinetWClass", "Edit");
    // Display index 0 is the rule the page shows first -- the same translation the op performs.
    let real = cfg.real_index_of_display(0).expect("one rule");
    cfg.apply_capture(real, &info, false);

    let r = &cfg.rules[real];
    assert_eq!(r.process_pattern, "explorer.exe");
    assert_eq!(r.name, "explorer.exe / 在 搜索框 中搜索", "named after what was captured");
    // THE ONE TRANSFORM, and the reason it exists: the address bar carries the typed text, so the captured
    // string would stop matching the moment the user searches for something.
    assert_eq!(r.control_text_pattern, "在*中搜索", "the address bar keeps matching whatever is typed in it");
    assert_eq!(r.window_class_pattern, "CabinetWClass", "the window class came from the capture");
    assert!(r.container_text_pattern.is_empty(), "the fixture has no container text, so none is invented");
    assert!(r.control_class_pattern.is_empty(), "and the control class it did not have stays empty");
    assert!(r.enabled, "capturing a control IS the user saying which one they mean");
}

#[test]
fn a_capture_does_not_widen_an_existing_rule() {
    // ⚠️ THE FLAGS OF AN EXISTING RULE BELONG TO THE USER. `apply_capture` narrows the flags of a NEW rule
    // (upstream's decision -- most captured fields are generic and would make the rule match far more than it
    // looks like). Applying that to a rule the user already tuned would silently rewrite their choices, so the
    // narrowing is only for a rule that is still empty. Checked here rather than left to reading the branch.
    let cfg0 = AppConfig::default();
    let _g = use_config(cfg0);

    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("add").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), -1), 1);

    let mut cfg = settings::debug_config().expect("config");
    let real = cfg.real_index_of_display(0).expect("one rule");
    // What a user would have set by hand: a switch they turned OFF. If a capture narrowed an existing rule's
    // flags, this is the one it would flip back on.
    cfg.rules[real].use_window_title = false;

    let info = crate::tests::win("explorer.exe", "在 搜索框 中搜索", "", "CabinetWClass", "Edit");
    cfg.apply_capture(real, &info, false);

    let r = &cfg.rules[real];
    assert!(!r.use_window_title, "an existing rule keeps the user's own switches");
    assert_eq!(r.window_class_pattern, "CabinetWClass", "the captured fields are filled in");
    assert_eq!(r.process_pattern, "explorer.exe");
    // ⚠️ AND THE PATTERNS *ARE* OVERWRITTEN, WHICH IS UPSTREAM'S BEHAVIOUR AND DELIBERATE. A capture says
    // "this control, now" -- for a rule the user is editing that is the whole point of pressing the button, so
    // every captured field is replaced rather than merged. (Checked against the source this was ported from:
    // `apply_capture` assigns all nine patterns unconditionally and narrows the FLAGS only for a new rule.)
    assert_eq!(r.window_title_pattern, "", "an empty captured title replaces what was there");
}

#[test]
fn an_edit_stays_in_the_draft_until_it_is_committed() {
    // ⚠️ THIS IS THE MODEL THE USER ASKED FOR AND THE ONE THAT WAS MISSING.
    //
    // "列表需要有个编辑保存功能，用户使用逻辑见Auto-ime独立版" -- and the standalone's model is: press 编辑, edit a COPY
    // (`draft`), press 保存 to publish it. Until then the file is untouched.
    //
    // The check is on the CONFIG, not on the draft, because "the file did not change" is the property a user
    // cares about -- a draft that leaked into the config would look identical in the editor and be wrong.
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("r", "original", 1, Action::Chinese));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("begin-edit").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), 0), 1);

    // The draft exists and holds a copy of the rule.
    let (draft_index, drafted, _fresh) = settings::debug_draft().expect("a draft");
    assert_eq!(draft_index, 0);
    assert_eq!(drafted.name, "original");

    // A field edit goes into the DRAFT -- and the config does not move.
    let path = std::ffi::CString::new("rules[0].name").unwrap();
    let value = std::ffi::CString::new("edited").unwrap();
    assert_eq!(settings::set_control(path.as_ptr(), value.as_ptr()), 1);
    assert_eq!(settings::debug_draft().expect("draft").1.name, "edited", "the draft took the edit");
    assert_eq!(settings::debug_config().expect("config").rules[0].name, "original",
               "and the rule itself did not");

    // Committing publishes it.
    let commit = std::ffi::CString::new("commit-edit").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), commit.as_ptr(), empty.as_ptr(), 0), 1);
    assert_eq!(settings::debug_config().expect("config").rules[0].name, "edited");
    assert!(settings::debug_draft().is_none(), "and the draft is done");
}

#[test]
fn cancelling_throws_the_edit_away() {
    // The other half of the model, and the reason a Save button alone would not be enough: a user who changes
    // their mind must be able to get back to the saved rule without retyping what was there.
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("r", "original", 1, Action::Chinese));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    let begin = std::ffi::CString::new("begin-edit").unwrap();
    let cancel = std::ffi::CString::new("cancel-edit").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), begin.as_ptr(), empty.as_ptr(), 0), 1);

    let path = std::ffi::CString::new("rules[0].name").unwrap();
    let value = std::ffi::CString::new("edited").unwrap();
    assert_eq!(settings::set_control(path.as_ptr(), value.as_ptr()), 1);

    assert_eq!(settings::list_op(id.as_ptr(), cancel.as_ptr(), empty.as_ptr(), 0), 1);
    assert!(settings::debug_draft().is_none(), "the draft is gone");
    assert_eq!(settings::debug_config().expect("config").rules[0].name, "original", "and so is the edit");
}

#[test]
fn cancelling_a_new_rule_throws_the_rule_away() {
    // ⚠️ THE USER'S REPORT AND ITS ANSWER, IN THEIR WORDS: "自动输入法插件有个交互优化下：新建规则后，没点保存，切到
    // 其它规则后不保留" -- answered with "没点保存就切走 = 等于没建（不保留）".
    //
    // ⚠️ AND IT IS THE ONE CASE WHERE CANCELLING MUST REMOVE SOMETHING. Dropping an edit to a rule that was
    // already saved costs the typing and nothing else (the test above). A rule that `add` has just created has NO
    // saved version: keeping it would leave an empty, switched-off 新规则 in the list -- and in config.json, one
    // debounce later -- for the user to delete by hand. So "not saved" has to mean "not created", on every path
    // out of the editor, including the ones that never send `cancel-edit` (see `save_settings`).
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("keep", "keep me", 1, Action::Chinese));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    let add = std::ffi::CString::new("add").unwrap();
    let cancel = std::ffi::CString::new("cancel-edit").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), add.as_ptr(), empty.as_ptr(), -1), 1);
    assert_eq!(settings::debug_config().expect("config").rules.len(), 2, "the new rule is in the list for the \
                                                                          editor to draw");
    assert!(settings::debug_draft().expect("draft").2, "and its draft is marked fresh");

    // The user typed something into it and then clicked another rule -- which is what sends `cancel-edit`.
    let path = std::ffi::CString::new("rules[0].process_pattern").unwrap();
    let value = std::ffi::CString::new("notepad.exe").unwrap();
    assert_eq!(settings::set_control(path.as_ptr(), value.as_ptr()), 1);

    assert_eq!(settings::list_op(id.as_ptr(), cancel.as_ptr(), empty.as_ptr(), 0), 1);
    let after = settings::debug_config().expect("config");
    assert_eq!(after.rules.len(), 1, "the rule that was never saved is gone with the draft");
    assert_eq!(after.rules[0].name, "keep me", "and the harmless half is that the rule below it is untouched");
}

#[test]
fn an_unsaved_new_rule_is_never_written_to_the_file_even_if_nobody_cancels() {
    // ⚠️ THE OTHER HALF OF THE SAME RULE, FOR THE PATHS THAT NEVER SEND `cancel-edit`: closing the panel, quitting
    // Apex, or being killed while the editor is open. The host saves on its own debounce (and on the way out), so
    // "the draft is not in the file" has to be a property of the WRITE, not of the page's manners.
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("keep", "keep me", 1, Action::Chinese));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    let add = std::ffi::CString::new("add").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), add.as_ptr(), empty.as_ptr(), -1), 1);

    // The write happens; what it would write is one rule, not two.
    //
    // ⚠️ ASSERTED THROUGH `debug_config_for_writing` RATHER THAN BY CALLING `save_settings`, AND THAT IS NOT A
    // SHORTCUT: a test has no feature directory, so writing fails and `save_settings` answers 0 -- and setting one
    // would be a process-wide `OnceCell` shared by every other test in this binary. The DECISION is the thing
    // under test; the write itself is covered by the real-DLL gate (`check_feature_edit`).
    let written = settings::debug_config_for_writing().expect("the config to write");
    assert_eq!(written.rules.len(), 1, "the file gets only the rule that was really saved");
    assert_eq!(written.rules[0].name, "keep me");
    // ⚠️ AND THE LIVE CONFIG STILL HAS BOTH: the user is still typing into the new rule, so the editor must keep
    // its row. Removing it from memory here would delete it out from under the cursor.
    assert_eq!(settings::debug_config().expect("config").rules.len(), 2,
               "the in-memory list still holds the rule being edited");

    // ... and committing it is what puts it in the file.
    let commit = std::ffi::CString::new("commit-edit").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), commit.as_ptr(), empty.as_ptr(), 0), 1);
    let after = settings::debug_config_for_writing().expect("the config to write");
    assert_eq!(after.rules.len(), 2, "after Save it is a rule like any other");
}

#[test]
fn beginning_an_edit_on_another_rule_replaces_the_draft() {
    // ⚠️ THE USER'S REPORT: "每条的保存/取消，是独立的，切换规则后不保存，现在看起来它是所有规则一起用的."
    //
    // One draft at a time is what makes the edits independent: starting to edit another rule drops the first
    // rule's uncommitted edit rather than keeping it around to be applied later -- and the FIRST rule's saved
    // values are untouched, because the draft was never committed.
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("a", "first", 1, Action::Chinese));
    cfg.rules.push(rule("b", "second", 2, Action::Chinese));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    let begin = std::ffi::CString::new("begin-edit").unwrap();
    let path = std::ffi::CString::new("rules[0].name").unwrap();
    let value = std::ffi::CString::new("edited-first").unwrap();

    assert_eq!(settings::list_op(id.as_ptr(), begin.as_ptr(), empty.as_ptr(), 0), 1);
    assert_eq!(settings::set_control(path.as_ptr(), value.as_ptr()), 1);

    // Edit the SECOND rule: the first edit must be gone, not waiting.
    assert_eq!(settings::list_op(id.as_ptr(), begin.as_ptr(), empty.as_ptr(), 1), 1);
    let (index, drafted, _fresh) = settings::debug_draft().expect("a draft");
    assert_eq!(index, 1, "the draft moved to the second rule");
    assert_eq!(drafted.name, "second", "and it is a copy of that rule, not the edited one");

    let cfg_now = settings::debug_config().expect("config");
    assert_eq!(cfg_now.rules[0].name, "first", "the first rule's saved name is untouched");
    assert_eq!(cfg_now.rules[1].name, "second");
}

#[test]
fn adding_a_rule_drops_any_open_draft() {
    // Adding is the third way a draft can become meaningless: the rule it was copied from is not the rule the
    // user is about to work on. The standalone did the same thing by construction -- its 新建规则 moved the
    // selection and set `editing = true` on the NEW row.
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("a", "only", 1, Action::Chinese));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    let begin = std::ffi::CString::new("begin-edit").unwrap();
    let add = std::ffi::CString::new("add").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), begin.as_ptr(), empty.as_ptr(), 0), 1);
    let (old_real, old_rule, _fresh) = settings::debug_draft().expect("a draft on the old rule");
    assert_eq!((old_real, old_rule.name.as_str()), (0, "only"));

    // ⚠️ THE OLD DRAFT IS REPLACED, NOT MERELY CLEARED -- and that distinction is this test's whole content now.
    // `add` leaves a draft open (the NEW rule's: an empty shell is not worth looking at until it can be typed
    // into), so "the draft is none" would be asserting the wrong thing. What must be true is that the draft the
    // user was working on is not the one that is open -- otherwise a later Save would write "only" over
    // whatever they had typed.
    assert_eq!(settings::list_op(id.as_ptr(), add.as_ptr(), empty.as_ptr(), -1), 1);
    let (new_real, new_rule, new_fresh) = settings::debug_draft().expect("the new rule is open for editing");
    // ⚠️ THE INDEX IS THE SAME AND THE DRAFT IS NOT. The new rule is INSERTED AT 0, so the rule that was there
    // is pushed to 1 -- and the old draft (a copy of it) is replaced by a copy of the new rule at the same
    // number. Comparing positions cannot tell those apart; comparing what the draft IS can.
    assert_eq!(new_real, 0, "the draft is on row 0 -- where the new rule went");
    assert_eq!(new_rule.name, "新规则", "and it is a copy of the rule that was just created, not the old one");
    assert!(new_fresh, "and it is marked FRESH: it has never been saved, which is what makes abandoning it \
                        abandon the rule too -- see `cancelling_a_new_rule_throws_the_rule_away`");
    let doc = document();
    assert_eq!(rules_group(&doc)["editing"], 0, "the page is told which row that is -- the first one");
    let items = rules_group(&doc)["items"].as_array().expect("items");
    assert_eq!(items[0]["values"]["name"], "新规则", "and the new rule really is the first row");
    assert_eq!(items[1]["values"]["name"], "only", "with the rule that was there below it");
}

#[test]
fn a_new_rule_goes_to_the_top_of_the_list() {
    // ⚠️ "新添加规则放在最上面." The order is the FILE order now, so this is one insert at the front -- and,
    // importantly, it is NOT a renumbering of anybody's priority: the numbers are the user's own (see
    // `a_move_leaves_every_priority_alone`), and a new rule arriving must not rewrite them.
    let cfg = AppConfig {
        rules: vec![
            rule("a", "aaa", 1, Action::English),
            rule("b", "bbb", 2, Action::English),
            rule("c", "ccc", 3, Action::English),
        ],
        ..Default::default()
    };
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let op = std::ffi::CString::new("add").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), op.as_ptr(), empty.as_ptr(), -1), 1);

    let doc = document();
    let group = rules_group(&doc);
    let items = group["items"].as_array().expect("items");
    let names: Vec<&str> = items.iter().map(|i| i["values"]["name"].as_str().unwrap()).collect();
    assert_eq!(names.len(), 4);
    assert_eq!(names[0], "新规则", "the new rule is the first row");
    assert_eq!(&names[1..], &["aaa", "bbb", "ccc"], "and the rest kept their order");

    // ⚠️ AND THE ROW THE PAGE IS TOLD TO OPEN IS THAT ONE -- the page follows `editing`, so the two must agree.
    assert_eq!(group["editing"], 0);

    // ⚠️ AND NOBODY'S NUMBER MOVED. The rules that were there keep 1, 2, 3 exactly as they were, and the new
    // one keeps the default 100 -- which is a number the user may edit and which decides nothing.
    let ps: Vec<u64> = items.iter().map(|i| i["values"]["priority"].as_u64().expect("a priority")).collect();
    assert_eq!(ps, vec![100, 1, 2, 3], "the new rule's default, then the others' own numbers, untouched");
}

#[test]
fn a_move_does_not_throw_away_the_open_draft() {
    // ⚠️⚠️ A MOVE CHANGES POSITIONS NOW, SO THE DRAFT HAS TO BE CARRIED ACROSS -- and that is not a nicety.
    //
    // The draft is keyed by the rule's position in the file. While the list was sorted by priority, a move only
    // renumbered priorities and positions never changed, so nothing had to be done. The order IS the file order
    // now, so a move renumbers POSITIONS -- and a draft left pointing at its old number would be a draft for
    // whatever rule slid into that slot: the user's typing would land in a rule they never opened.
    //
    // (This is `move_rule`'s whole reason for existing; a test that only looked at the arrangement would pass
    // while that was broken.)
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("a", "aaa", 1, Action::Chinese));
    cfg.rules.push(rule("b", "bbb", 2, Action::English));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    let begin = std::ffi::CString::new("begin-edit").unwrap();
    let path = std::ffi::CString::new("rules[0].name").unwrap();
    let typed = std::ffi::CString::new("typed").unwrap();
    let move_op = std::ffi::CString::new("move").unwrap();
    let to_one = std::ffi::CString::new("1").unwrap();

    // Edit the first rule, type into it, then drag it down one place.
    assert_eq!(settings::list_op(id.as_ptr(), begin.as_ptr(), empty.as_ptr(), 0), 1);
    assert_eq!(settings::set_control(path.as_ptr(), typed.as_ptr()), 1);
    assert_eq!(settings::list_op(id.as_ptr(), move_op.as_ptr(), to_one.as_ptr(), 0), 1);

    let (real, drafted, _fresh) = settings::debug_draft().expect("the draft survived the move");
    assert_eq!(real, 1, "the draft followed the rule to its new position");
    assert_eq!(drafted.name, "typed", "and the typing is still in it");
    // The document says the open row is the second one, which is where that rule now is.
    assert_eq!(rules_group(&document())["editing"], 1, "the page is told where the row went");
}

#[test]
fn the_toggle_hotkey_is_recorded_and_checked_before_it_is_kept() {
    // ⚠️⚠️ THE PAGE RECORDS WHAT THE USER PRESSES, SO SOMETHING HAS TO DECIDE WHETHER IT IS USABLE -- and it is
    // this side, because this side is what later SENDS the keys. A combination that parses nowhere would be
    // stored, shown back to the user as their setting, and then do nothing when pressed: a setting that lies.
    //
    // The grammar is not copied here: `win32::combo_is_usable` asks the same parser `simulate_combo` presses
    // with. These cases pin the RULE (what may be a toggle), not the parser.
    let _g = use_config(AppConfig::default());
    let path = std::ffi::CString::new("ime_toggle_hotkey").unwrap();
    let set = |combo: &str| {
        let v = std::ffi::CString::new(combo).unwrap();
        settings::set_control(path.as_ptr(), v.as_ptr())
    };
    let stored = || settings::debug_config().map(|c| c.ime_toggle_hotkey).unwrap_or_default();

    // What the feature ships with and what the user is likely to record: a modifier and one key.
    assert_eq!(set("Ctrl+Space"), 1);
    assert_eq!(stored(), "Ctrl+Space");
    assert_eq!(set("Ctrl+Shift+A"), 1, "several modifiers are fine");
    assert_eq!(stored(), "Ctrl+Shift+A");
    assert_eq!(set("Alt+F4"), 1, "function keys are in the grammar");
    assert_eq!(stored(), "Alt+F4");

    // ⚠️ A BARE KEY IS REFUSED -- the rule this feature adds on top of the grammar. It parses and it would be
    // sent; it would also fire on every letter typed.
    assert_eq!(set("A"), 0, "a toggle bound to one letter is a trap");
    assert_eq!(stored(), "Alt+F4", "and a refusal leaves what was there");
    // Modifiers with no key: nothing to press.
    assert_eq!(set("Ctrl+Alt"), 0);
    assert_eq!(stored(), "Alt+F4");
    // A key this program cannot send (the grammar has no arrow keys): refused rather than stored and ignored.
    assert_eq!(set("Ctrl+ArrowUp"), 0);
    assert_eq!(stored(), "Alt+F4");
    // ...and nothing at all.
    assert_eq!(set(""), 0);
    assert_eq!(stored(), "Alt+F4");
}

#[test]
fn a_capture_lands_in_the_draft_and_never_in_the_file() {
    // ⚠️⚠️ THIS IS THE BUG THE USER REPORTED: "捕获没有进到规则列表的选项框中，数据没进去."
    //
    // A capture is produced by the feature itself, and the first version wrote it straight into the config --
    // while the page was drawing its own copy, so the user saw nothing. Now it goes into the DRAFT, which is
    // what the page draws, and the file only changes on Save.
    //
    // ⚠️ THE CAPTURE OP ITSELF IS NOT DRIVEN HERE (it waits for a real mouse click -- see wait_for_click). What
    // is checked is the part that can be checked without a mouse: `apply_capture` on the drafted rule, which is
    // what the capture thread does when the click arrives.
    let mut cfg = AppConfig::default();
    cfg.rules.push(rule("r", "original", 1, Action::Chinese));
    let _g = use_config(cfg);

    let id = std::ffi::CString::new("rules").unwrap();
    let empty = std::ffi::CString::new("").unwrap();
    let begin = std::ffi::CString::new("begin-edit").unwrap();
    assert_eq!(settings::list_op(id.as_ptr(), begin.as_ptr(), empty.as_ptr(), 0), 1);

    // ...and then the click arrives: the thread applies the control to whatever is drafted.
    let info = crate::tests::win("explorer.exe", "在 搜索框 中搜索", "", "CabinetWClass", "Edit");
    let (index, mut drafted, _fresh) = settings::debug_draft().expect("a draft");
    drafted.apply_capture(&info, false);
    assert_eq!(drafted.process_pattern, "explorer.exe");
    assert_eq!(drafted.control_text_pattern, "在*中搜索");
    let _ = index;

    // ⚠️ THE FILE IS STILL UNTOUCHED -- that is what "not yet saved" has to mean, and it is the half of the bug
    // that a user notices later (an unexplained change appearing after a cancelled edit).
    let saved = settings::debug_config().expect("config");
    assert_eq!(saved.rules[0].process_pattern, "", "the saved rule has not been captured into");
}

#[test]
fn the_document_says_whether_a_capture_is_still_being_waited_for() {
    // ⚠️⚠️ THIS FIELD IS THE PAGE'S ONLY WAY TO KNOW WHEN A CAPTURE HAS LANDED, and it is here because the
    // page's first attempt GUESSED instead: it declared the wait over as soon as the document changed. The
    // document also changes for reasons that have nothing to do with a capture (the host answers every `listOp`
    // with a snapshot, and the page re-reads after it), so the watch died at arming time and the captured fields
    // stayed invisible -- the user's report: "捕获事件进行时，鼠标点击后结果要马上给到参数页，目前没有，要等到点击
    // 新建的规则条才会出现". The other direction of the same guess was a page that would poll for ever after a
    // capture that changed nothing.
    //
    // ⚠️ THE FLAG IS SET DIRECTLY RATHER THAN BY THE `capture` OP, and that is not a shortcut: the op spawns a
    // thread that waits for a REAL mouse click (see wait_for_click) and arms a NAMED, PROCESS-WIDE event. Firing
    // that from a test would arm a capture in whatever Apex the user happens to be running.
    let _g = use_config(AppConfig::default());

    settings::debug_set_capture_waiting(false);
    assert_eq!(rules_group(&document())["waiting"], serde_json::Value::Null,
               "idle: nothing is being waited for");

    settings::debug_set_capture_waiting(true);
    assert_eq!(rules_group(&document())["waiting"], "capture",
               "waiting: the page must keep asking until this goes away");

    // ⚠️ AND IT GOES AWAY, which is the half that ends the wait. (In the running program this line is executed by
    // the capture thread AFTER the draft holds the answer -- see the note there: clearing it first would let a
    // poll land in the gap and stop the watch on a document that does not have the values in it yet.)
    settings::debug_set_capture_waiting(false);
    assert_eq!(rules_group(&document())["waiting"], serde_json::Value::Null);
}
