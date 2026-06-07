//! Integration test: inlay hints for the cpp/hello example.
//! Compares clang-bridge output against what clangd produces for the same file.

use clang_bridge::{inlay, Index};

const HELLO: &str =
    "/home/max/freight-workspace/crates/freight/examples/cpp/hello";

fn compile_flags() -> Vec<&'static str> {
    vec![
        "-std=c++20",
        "-Iinc",
        "-I.pkgs/mathlib/include",
        "-I.pkgs/vecmath/include",
        "-Wno-gnu-include-next",
    ]
}

fn main_tu() -> (clang_bridge::TranslationUnit, clang_bridge::Index) {
    let idx = Index::new();
    let file = format!("{HELLO}/src/main.cpp");
    let tu = idx
        .parse(&file, HELLO, &compile_flags())
        .expect("failed to parse main.cpp — check flags and include paths");
    (tu, idx)
}

#[test]
fn no_parse_errors_on_hello_main() {
    let idx = Index::new();
    let file = format!("{HELLO}/src/main.cpp");
    let result = idx.parse(&file, HELLO, &compile_flags());
    assert!(
        result.is_some(),
        "Index::parse returned None for main.cpp — fatal parse error. \
         Verify stddef.h is found (resource-dir fix) and stats.hpp is \
         reachable (-Iinc flag applied)."
    );
}

#[test]
fn clang_bridge_hints_for_hello_main() {
    let (tu, _idx) = main_tu();

    // Request hints for the full file (lines 1–35, 1-based).
    let hints = inlay::inlay_hints(&tu, 1, 35);
    let all: Vec<_> = hints.iter().collect();

    println!("\n=== clang-bridge inlay hints for cpp/hello/src/main.cpp ===");
    if all.is_empty() {
        println!("  (none)");
    }
    for h in &all {
        let kind_str = if h.kind == 0 { "param" } else { "type" };
        println!(
            "  line {:2} col {:2} [{}]: {:?}",
            h.line, h.col, kind_str, h.label
        );
    }

    // ── Clangd ground-truth: we should produce ──────────────────────────────
    //  line 12: "values:" for mean(data) and variance(data)  (user-defined fns)
    //  NO hints for std::pair ctor or std::sqrt (system headers, filtered)
    //  NO "__x:" style internal-name hints

    let values_hints: Vec<_> = all
        .iter()
        .filter(|h| h.kind == 0 && h.label == "values:")
        .collect();
    assert_eq!(
        values_hints.len(),
        2,
        "expected 2 'values:' hints for mean(data) and variance(data), got {}",
        values_hints.len()
    );

    // Internal-name hints must never appear.
    let bad: Vec<_> = all.iter().filter(|h| h.label.starts_with("__")).collect();
    assert!(
        bad.is_empty(),
        "internal parameter-name hints leaked: {bad:?}"
    );

    // Constructor hints: Vec2(1,1) should produce x: and y:.
    let xy_hints: Vec<_> = all
        .iter()
        .filter(|h| h.kind == 0 && (h.label == "x:" || h.label == "y:"))
        .collect();
    assert!(
        xy_hints.len() >= 2,
        "expected at least 2 x:/y: hints for Vec2(1,1) constructor, got {}",
        xy_hints.len()
    );

    // NOTE (gap): structured-binding type hints emit ': std::pair<double,double>'
    // for the hidden tuple variable rather than per-binding ': double'.
    // BindingDecl is not visited by VisitVarDecl; this is a documented limitation.

    println!("\nSummary: {} total hints, {} values: hints, {} x:/y: hints",
             all.len(), values_hints.len(), xy_hints.len());
}
