//! Verifies the bridge against modern C++ (C++20) constructs in `modern.cpp`:
//! concepts, structured bindings, lambdas, `if constexpr`, fold expressions.

use clang_bridge::{goto, Index, TranslationUnit};
use std::path::Path;

struct Fix {
    _idx: Index,
    tu: TranslationUnit,
    src: String,
}

fn load() -> Fix {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let cpp = dir.join("modern.cpp");
    let src = std::fs::read_to_string(&cpp).expect("read modern.cpp");
    let idx = Index::new();
    let tu = idx
        .parse(cpp.to_str().unwrap(), dir.to_str().unwrap(), &["-std=c++20"])
        .expect("modern.cpp should parse under c++20");
    Fix { _idx: idx, tu, src }
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
fn modern_parses_without_diagnostics() {
    let f = load();
    let diags: Vec<_> = f.tu.diagnostics().collect();
    assert!(diags.is_empty(), "modern.cpp should be clean under c++20, got {diags:?}");
}

#[test]
fn concept_is_indexed_as_document_symbol() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();
    assert!(
        syms.iter().any(|s| s.name == "Addable" && s.kind == "concept"),
        "C++20 concept `Addable` should appear in the outline; got {:?}",
        syms.iter().map(|s| (s.name.clone(), s.kind.clone())).collect::<Vec<_>>()
    );
}

#[test]
fn aggregate_fields_nest_and_helpers_present() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();
    let has = |n: &str, k: &str| syms.iter().any(|s| s.name == n && s.kind == k);

    assert!(has("Vec2", "struct"), "struct Vec2");
    assert!(has("Mode", "enum"), "enum class Mode");
    assert!(has("sum", "function"), "variadic sum");
    assert!(has("twice", "function"), "constrained twice");
    assert!(has("abs_generic", "function"), "if-constexpr abs_generic");

    let vec2 = syms.iter().position(|s| s.name == "Vec2").unwrap();
    let x = syms.iter().find(|s| s.name == "x" && s.kind == "field").expect("Vec2::x");
    assert_eq!(x.parent, vec2 as i32, "Vec2::x nests under Vec2");
}

#[test]
fn goto_works_through_concept_constrained_call() {
    let f = load();
    // `twice(total)` resolves to the constrained template definition.
    let (l, c) = at(&f.src, "int doubled = twice(total);", "twice");
    let loc = goto::goto_definition(&f.tu, l, c).expect("goto twice");
    // Definition is the `T twice(T value)` line.
    let def = f.src.lines().position(|ln| ln.contains("T twice(T value)")).unwrap() as u32 + 1;
    assert_eq!(loc.line, def, "twice definition line");
}
