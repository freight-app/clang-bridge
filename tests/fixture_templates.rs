//! Verifies the bridge against template-heavy code in `templates.cpp`:
//! variadic templates, full/partial specialisation, CRTP, alias templates.

use clang_bridge::{Index, TranslationUnit};
use std::path::Path;

struct Fix {
    _idx: Index,
    tu: TranslationUnit,
}

fn load() -> Fix {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let cpp = dir.join("templates.cpp");
    let idx = Index::new();
    let tu = idx
        .parse(cpp.to_str().unwrap(), dir.to_str().unwrap(), &["-std=c++17"])
        .expect("templates.cpp should parse");
    Fix { _idx: idx, tu }
}

#[test]
fn templates_parse_without_diagnostics() {
    let f = load();
    let diags: Vec<_> = f.tu.diagnostics().collect();
    assert!(diags.is_empty(), "templates.cpp should be clean, got {diags:?}");
}

#[test]
fn specialization_members_do_not_orphan_to_top_level() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();
    // Every `value` static member (primary + full spec + partial spec + Count)
    // must nest under its enclosing struct, never float to the top level.
    let values: Vec<_> = syms.iter().filter(|s| s.name == "value").collect();
    assert!(values.len() >= 3, "expected several `value` members, got {}", values.len());
    for v in &values {
        assert_ne!(
            v.parent, -1,
            "a `value` member orphaned to the top level (specialization not treated \
             as a container): {:?}",
            values.iter().map(|s| (s.sel_line, s.parent)).collect::<Vec<_>>()
        );
    }
}

#[test]
fn crtp_and_alias_present_and_nested() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();
    let has = |n: &str, k: &str| syms.iter().any(|s| s.name == n && s.kind == k);

    assert!(has("Ptr", "typedef"), "alias template Ptr");
    assert!(has("Count", "struct"), "variadic Count");
    assert!(has("repeat_add", "function"), "non-type-param function repeat_add");

    // CRTP: Square's members nest under Square.
    let square = syms.iter().position(|s| s.name == "Square" && s.kind == "struct")
        .expect("struct Square");
    let area = syms.iter().find(|s| s.name == "area" && s.kind == "method").expect("Square::area");
    assert_eq!(area.parent, square as i32, "Square::area nests under Square");
    let side = syms.iter().find(|s| s.name == "side").expect("Square::side");
    assert_eq!(side.parent, square as i32, "Square::side nests under Square");
}
