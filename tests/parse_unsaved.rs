//! Tests for cb_parse_unsaved and cb_index_last_error.

use clang_bridge::Index;

#[test]
fn parse_unsaved_produces_valid_tu() {
    let src = "int square(int x) { return x * x; }";
    let idx = Index::new();
    let tu = idx.parse_unsaved("/tmp/cb_pu_test.cpp", "", src, &["-std=c++17"]);
    if tu.is_none() {
        panic!("parse_unsaved failed: {:?}", idx.last_error());
    }
    let tu = tu.unwrap();

    // The TU should have a document symbol for 'square'.
    let syms = tu.document_symbols().expect("document symbols");
    assert!(
        syms.iter().any(|s| s.name == "square"),
        "expected 'square' in document symbols"
    );
}

#[test]
fn parse_unsaved_hover_works() {
    let src = "/// Compute the cube.\nint cube(int x) { return x * x * x; }";
    let idx = Index::new();
    let tu = idx
        .parse_unsaved("/tmp/cb_pu_hover.cpp", "", src, &["-std=c++17"])
        .expect("parse_unsaved should succeed");

    let md = clang_bridge::hover::hover_full(&tu, 2, 5).expect("hover");
    assert!(md.contains("cube"), "expected function name in hover: {md}");
    assert!(md.contains("Compute the cube"), "expected doc comment: {md}");
}

#[test]
fn index_last_error_on_nonexistent_file() {
    let idx = Index::new();
    let result = idx.parse("/tmp/this_file_definitely_does_not_exist_abc123.cpp", "", &[]);
    // Parse should fail.
    assert!(result.is_none());
    // last_error should be set.
    let err = idx.last_error();
    assert!(err.is_some(), "expected last_error to be set after failed parse");
}
