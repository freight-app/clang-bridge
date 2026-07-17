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

#[test]
fn decltype_type_hint() {
    let src = "int foo() { return 1; }\nvoid f() { int x = 0; decltype(x) y = 1; decltype(foo()) z = foo(); }";
    let path = write_temp("cb_inlay_decltype.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 2);
    let labels: Vec<_> = hints.iter().map(|h| h.label.clone()).collect();
    // Both decltype(x) and decltype(foo()) should resolve to `: int`
    let int_hints: Vec<_> = labels.iter().filter(|l| l.as_str() == ": int").collect();
    assert!(
        int_hints.len() >= 2,
        "expected at least two ': int' decltype hints, got: {labels:?}"
    );
}

#[test]
fn block_end_hint_for_long_function() {
    // A function spanning >= 10 lines should get a block-end hint.
    let src = "void longfn() {\n    int a = 1;\n    int b = 2;\n    int c = 3;\n    int d = 4;\n    int e = 5;\n    int f = 6;\n    int g = 7;\n    int h = 8;\n    int i = 9;\n}";
    let path = write_temp("cb_inlay_blockend.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 11);
    let block_end_hints: Vec<_> = hints.iter().filter(|h| h.kind == 2).collect();
    assert!(
        !block_end_hints.is_empty(),
        "expected a block-end hint for 'longfn', got none"
    );
    let lbl = &block_end_hints[0].label;
    assert!(
        lbl.contains("longfn"),
        "block-end hint label should contain 'longfn', got: {lbl}"
    );
}

#[test]
fn no_block_end_hint_for_short_function() {
    // A function spanning < 10 lines should NOT get a block-end hint.
    let src = "int add(int a, int b) {\n    return a + b;\n}";
    let path = write_temp("cb_inlay_noblockend.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 3);
    let block_end_hints: Vec<_> = hints.iter().filter(|h| h.kind == 2).collect();
    assert!(
        block_end_hints.is_empty(),
        "expected no block-end hint for short function, got: {block_end_hints:?}"
    );
}

#[test]
fn aggregate_designators_cover_records_arrays_and_comments() {
    let src = r#"struct Point { int x; int y; };
int x = 7;
Point point{x, 2};
Point commented{/*x=*/ 3, 4};
Point explicit_fields{.x = 5, .y = 6};
int values[]{8, 9};"#;
    let path = write_temp("cb_inlay_designators.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++20"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 6);
    let designators: Vec<_> = hints.iter().filter(|h| h.kind == 3).collect();
    let labels: Vec<_> = designators.iter().map(|h| h.label.as_str()).collect();
    assert_eq!(
        labels,
        vec![".x=", ".y=", ".y=", "[0]=", "[1]="],
        "unexpected aggregate designator hints: {designators:?}"
    );
}

#[test]
fn aggregate_designators_follow_bases_and_brace_elision() {
    let src = r#"struct Base { int base; };
struct Pair { int left; int right; };
struct Derived : Base { Pair pair; int tail; };
Derived value{{1}, 2, 3, 4};"#;
    let path = write_temp("cb_inlay_nested_designators.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 4);
    let labels: Vec<_> = hints
        .iter()
        .filter(|h| h.kind == 3)
        .map(|h| h.label)
        .collect();
    assert_eq!(labels, vec![".base=", ".pair.left=", ".pair.right=", ".tail="]);
}

#[test]
fn aggregate_designators_support_cxx20_parenthesized_initialization() {
    let src = "struct Point { int x; int y; };\nPoint point(1, 2);";
    let path = write_temp("cb_inlay_paren_designators.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++20"]).unwrap();

    let hints = inlay::inlay_hints(&tu, 1, 2);
    let labels: Vec<_> = hints
        .iter()
        .filter(|h| h.kind == 3)
        .map(|h| h.label)
        .collect();
    assert_eq!(labels, vec![".x=", ".y="]);
}
