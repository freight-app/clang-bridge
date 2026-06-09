//! Tests for cb_type_at (variable type hover) and cb_macro_at (macro hover).

use clang_bridge::{hover, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

// ── cb_type_at ────────────────────────────────────────────────────────────────

#[test]
fn type_at_returns_type_for_variable() {
    let src = "int count = 0;";
    let path = write_temp("cb_typeat_var.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let ty = hover::type_at(&tu, 1, 5).expect("expected type for 'count'");
    assert!(ty.contains("int"), "expected 'int', got: {ty}");
}

#[test]
fn type_at_returns_none_for_function() {
    // Functions don't have a value type in this API — should return None.
    let src = "void greet() {}";
    let path = write_temp("cb_typeat_fn.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 6 is on the function name — cb_type_at only handles VarDecl/FieldDecl.
    let ty = hover::type_at(&tu, 1, 6);
    assert!(ty.is_none(), "expected None for function name, got: {ty:?}");
}

// ── cb_macro_at ───────────────────────────────────────────────────────────────

#[test]
fn macro_hover_shows_definition() {
    let src = "#define ANSWER 42\nint x = ANSWER;";
    let path = write_temp("cb_macro_obj.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 9 is on "ANSWER" in the second line.
    let md = hover::macro_hover(&tu, 2, 9).expect("expected macro hover for ANSWER");
    assert!(md.contains("ANSWER"), "expected macro name: {md}");
    assert!(md.contains("42"), "expected expansion token: {md}");
    assert!(md.contains("```cpp"), "expected code block: {md}");
}

#[test]
fn macro_hover_function_like_shows_params() {
    let src = "#define MAX(a, b) ((a) > (b) ? (a) : (b))\nint x = MAX(1, 2);";
    let path = write_temp("cb_macro_fn.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let md = hover::macro_hover(&tu, 2, 9).expect("expected macro hover for MAX");
    assert!(md.contains("MAX"), "expected macro name: {md}");
    assert!(md.contains('a') && md.contains('b'), "expected param names: {md}");
}

#[test]
fn macro_hover_returns_none_for_non_macro() {
    let src = "int answer = 42;";
    let path = write_temp("cb_macro_none.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let result = hover::macro_hover(&tu, 1, 5);
    assert!(result.is_none(), "expected None for non-macro identifier");
}

/// `type_at` on a record-typed variable must return the class name, not None.
/// Guards against the implicit default-construction expression shadowing the
/// VarDecl in `locate_symbol_at`.
#[test]
fn type_at_returns_type_for_record_variable() {
    let src = "struct Widget {};\nWidget w;";
    let path = write_temp("cb_typeat_record.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    // line 2, col 8 → the variable `w`
    let ty = hover::type_at(&tu, 2, 8);
    assert_eq!(ty.as_deref(), Some("Widget"), "type_at on record var");
}
