//! Tests for inlay hints: parameter-name hints and deduced-type hints.

use clang_bridge::{inlay, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

#[test]
fn param_hint_at_call_site() {
    let src = "void f(int x, int y) {}\nvoid use() { f(1, 2); }";
    let path = write_temp("cb_inlay_param.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 2);
    // There should be two parameter hints: "x:" at col of arg 1, "y:" at col of arg 2.
    let labels: Vec<_> = hints.iter().map(|h| h.label.clone()).collect();
    assert!(
        labels.iter().any(|l| l == "x:"),
        "expected 'x:' param hint, got: {labels:?}"
    );
    assert!(
        labels.iter().any(|l| l == "y:"),
        "expected 'y:' param hint, got: {labels:?}"
    );
}

#[test]
fn type_hint_for_auto_variable() {
    let src = "void f() { auto x = 42; }";
    let path = write_temp("cb_inlay_auto.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 1);
    // There should be one deduced-type hint for `x` with kind=1.
    let type_hints: Vec<_> = hints.iter().filter(|h| h.kind == 1).collect();
    assert!(
        !type_hints.is_empty(),
        "expected a deduced-type hint for 'auto x', got none"
    );
    let label = &type_hints[0].label;
    assert!(
        label.contains("int"),
        "expected type hint containing 'int', got: {label}"
    );
}

#[test]
fn no_hint_when_arg_matches_param_name() {
    // Suppress hint when the argument expression is the same identifier as the param.
    let src = "void f(int x) {}\nvoid use(int x) { f(x); }";
    let path = write_temp("cb_inlay_suppress.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 2);
    let param_hints: Vec<_> = hints.iter().filter(|h| h.kind == 0).collect();
    assert!(
        param_hints.is_empty(),
        "expected hint to be suppressed when arg matches param name, got: {param_hints:?}"
    );
}
