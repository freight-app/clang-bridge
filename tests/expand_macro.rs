//! Tests for TranslationUnit::expand_macro — macro definition and expansion display.

use clang_bridge::Index;
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

// ── object-like macro ─────────────────────────────────────────────────────────

#[test]
fn expand_macro_object_like() {
    // "ANSWER" on line 2 col 9 — "#define ANSWER 42\nint x = ANSWER;"
    let src = "#define ANSWER 42\nint x = ANSWER;";
    let path = write_temp("cb_macro_expand_obj.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 9 is on "ANSWER" in "int x = ANSWER;" (line 2).
    let result = tu.expand_macro(2, 9).expect("expected Some for ANSWER usage");
    assert!(
        result.contains("42") || result.contains("ANSWER"),
        "expected expansion to mention '42' or 'ANSWER', got: {result}",
    );
}

// ── function-like macro ───────────────────────────────────────────────────────

#[test]
fn expand_macro_function_like() {
    // "ADD" on line 2 — "#define ADD(a, b) ((a) + (b))\nint r = ADD(1, 2);"
    let src = "#define ADD(a, b) ((a) + (b))\nint r = ADD(1, 2);";
    let path = write_temp("cb_macro_expand_fn.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 9 is on "ADD" in "int r = ADD(1, 2);" (line 2).
    let result = tu.expand_macro(2, 9).expect("expected Some for ADD usage");
    assert!(!result.is_empty(), "expected non-empty expansion for ADD, got empty string");
}

// ── definition site ───────────────────────────────────────────────────────────

#[test]
fn expand_macro_definition_site() {
    // Hovering on the #define line itself — result may be Some or None; must not panic.
    let src = "#define ANSWER 42\nint x = ANSWER;";
    let path = write_temp("cb_macro_expand_defsite.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 9 is on "ANSWER" in "#define ANSWER 42" (line 1).
    let _result = tu.expand_macro(1, 9); // must not panic; Some or None are both acceptable
}

// ── non-macro position ────────────────────────────────────────────────────────

#[test]
fn expand_macro_non_macro_position() {
    // "x" in "int x = 5;" is an ordinary variable — expand_macro should return None.
    let src = "int x = 5;";
    let path = write_temp("cb_macro_expand_none.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 5 is on 'x'.
    let result = tu.expand_macro(1, 5);
    assert!(result.is_none(), "expected None for non-macro variable 'x', got: {result:?}");
}
