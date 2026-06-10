//! Verifies the bridge's handling of a plain **C** translation unit (`test.c`).
//!
//! The rest of the suite is C++-only; this exercises the C path: structs,
//! typedefs, enums, function pointers, macros, and static/extern linkage.
//! Positions are located by searching the fixture text so edits stay safe.

use clang_bridge::{goto, refs, Index, TranslationUnit};
use std::path::Path;

struct Fix {
    _idx: Index,
    tu: TranslationUnit,
    src: String,
}

fn load() -> Fix {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let c = dir.join("test.c");
    let src = std::fs::read_to_string(&c).expect("read test.c");
    let idx = Index::new();
    let tu = idx
        .parse(c.to_str().unwrap(), dir.to_str().unwrap(), &["-std=c11"])
        .expect("test.c should parse as C");
    Fix { _idx: idx, tu, src }
}

fn line_of(src: &str, anchor: &str) -> u32 {
    for (i, line) in src.lines().enumerate() {
        if line.contains(anchor) {
            return i as u32 + 1;
        }
    }
    panic!("anchor not found: {anchor:?}");
}

fn at(src: &str, anchor: &str, token: &str) -> (u32, u32) {
    for (i, line) in src.lines().enumerate() {
        if line.contains(anchor) {
            let off = line.find(token).expect("token on anchor line");
            return (i as u32 + 1, off as u32 + 1);
        }
    }
    panic!("anchor not found: {anchor:?}");
}

#[test]
fn c_parses_without_diagnostics() {
    let f = load();
    let diags: Vec<_> = f.tu.diagnostics().collect();
    assert!(diags.is_empty(), "test.c should be warning-clean, got {diags:?}");
}

#[test]
fn c_document_symbols_nest_struct_fields() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();

    // The struct tag must appear as a container...
    let point_idx = syms
        .iter()
        .position(|s| s.name == "Point" && s.kind == "struct")
        .expect("struct Point should be in the outline");

    // ...and its fields must nest under it (not float to the top level).
    for field in ["x", "y"] {
        let fs = syms.iter().find(|s| s.name == field && s.kind == "field")
            .unwrap_or_else(|| panic!("field {field} missing"));
        assert_eq!(
            fs.parent, point_idx as i32,
            "C struct field {field} should nest under Point, got parent={}",
            fs.parent
        );
    }
}

#[test]
fn c_document_symbols_have_expected_entries() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();
    let has = |name: &str, kind: &str| syms.iter().any(|s| s.name == name && s.kind == kind);

    assert!(has("Direction", "enum"), "enum Direction");
    assert!(has("DIR_EAST", "enumconst"), "enum constant DIR_EAST");
    assert!(has("IntMapFn", "typedef"), "function-pointer typedef IntMapFn");
    assert!(has("manhattan", "function"), "function manhattan");
    assert!(has("clampi", "function"), "static function clampi");
    assert!(has("point_count", "var"), "extern-linkage var point_count");

    // enum constants nest under the enum.
    let dir_idx = syms.iter().position(|s| s.name == "Direction").unwrap();
    let east = syms.iter().find(|s| s.name == "DIR_EAST").unwrap();
    assert_eq!(east.parent, dir_idx as i32, "DIR_EAST nests under Direction");
}

#[test]
fn c_goto_resolves_call_to_definition() {
    let f = load();
    // `manhattan(origin, p)` in main resolves to the definition of manhattan.
    let (l, c) = at(&f.src, "int d = manhattan(origin, p);", "manhattan");
    let loc = goto::goto_definition(&f.tu, l, c).expect("goto manhattan");
    let def_line = line_of(&f.src, "int manhattan(Point a, Point b)");
    assert_eq!(loc.line, def_line, "manhattan definition line");
}

#[test]
fn c_references_find_all_uses() {
    let f = load();
    // clampi: defined once, called once (inside apply_clamped).
    let (l, c) = at(&f.src, "static int clampi(int v, int lo, int hi)", "clampi");
    let sym = f.tu.symbol_at(l, c).expect("symbol at clampi");
    let r = refs::references(&f.tu, sym.usr());
    assert!(
        r.len() >= 2,
        "expected clampi decl + at least one call, got {}",
        r.len()
    );
    assert!(r.iter().any(|x| x.is_definition), "one occurrence is the definition");
}
