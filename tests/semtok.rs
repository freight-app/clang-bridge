//! Tests for cb_semantic_tokens.

use clang_bridge::{semtok, semtok::tok_type, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

#[test]
fn semantic_tokens_classify_function() {
    let src = "int add(int a, int b) { return a + b; }";
    let path = write_temp("cb_semtok_fn.cpp", src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let toks = semtok::semantic_tokens(&tu);
    assert!(!toks.is_empty(), "expected tokens");

    // 'add' should be classified as a function.
    let fn_toks: Vec<_> = toks
        .iter()
        .filter(|t| t.token_type == tok_type::FUNCTION)
        .collect();
    assert!(!fn_toks.is_empty(), "expected at least one FUNCTION token");
}

#[test]
fn semantic_token_columns_are_utf16() {
    // `ä` is 2 UTF-8 bytes but 1 UTF-16 code unit; a token after it on the same
    // line must report a UTF-16 column, not clang's byte column.
    let src = "struct S { int /* ä */ field; };";
    let path = write_temp("cb_semtok_utf16.cpp", src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();
    let toks = semtok::semantic_tokens(&tu);

    // Expected 1-based UTF-16 column of `field` = UTF-16 units before it + 1.
    let byte_idx = src.find("field").unwrap();
    let expected_col = src[..byte_idx].encode_utf16().count() as u32 + 1;

    let field = toks
        .iter()
        .find(|t| t.token_type == tok_type::PROPERTY)
        .expect("expected a PROPERTY token for `field`");
    assert_eq!(
        field.col, expected_col,
        "field token column should be UTF-16 ({expected_col}), got {} (byte column?)",
        field.col
    );
    assert_eq!(field.length, 5, "`field` is 5 UTF-16 units long");
}

#[test]
fn semantic_tokens_sorted_by_position() {
    let src = "int x; int y; int z;";
    let path = write_temp("cb_semtok_sort.cpp", src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let toks = semtok::semantic_tokens(&tu);
    let positions: Vec<_> = toks.iter().map(|t| (t.line, t.col)).collect();
    let mut sorted = positions.clone();
    sorted.sort();
    assert_eq!(positions, sorted, "tokens should be sorted by (line, col)");
}

#[test]
fn semantic_tokens_classify_type() {
    let src = "struct Point { int x; int y; };";
    let path = write_temp("cb_semtok_type.cpp", src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let toks = semtok::semantic_tokens(&tu);
    let type_toks: Vec<_> = toks
        .iter()
        .filter(|t| t.token_type == tok_type::TYPE)
        .collect();
    assert!(
        !type_toks.is_empty(),
        "expected TYPE token for struct Point"
    );
}

/// A class template must emit exactly one token (TYPE) for its name — the
/// ClassTemplateDecl and its templated CXXRecordDecl share a location and must
/// not produce a duplicate (one of which previously fell through to VARIABLE).
/// The template type parameter is also a TYPE, not a VARIABLE.
#[test]
fn semantic_tokens_class_template_single_type_token() {
    let decl = "template<class T> struct Vec { T at(int); };";
    let src = format!("{decl}\nVec<int> v;");
    let path = write_temp("cb_semtok_tmpl.cpp", &src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();
    let toks = semtok::semantic_tokens(&tu);

    let vec_col = decl.find("Vec").unwrap() as u32 + 1;
    let at_vec: Vec<_> = toks
        .iter()
        .filter(|t| t.line == 1 && t.col == vec_col)
        .collect();
    assert_eq!(
        at_vec.len(),
        1,
        "exactly one token for the template name, got {at_vec:?}"
    );
    assert_eq!(
        at_vec[0].token_type,
        tok_type::TYPE,
        "class template name is a TYPE"
    );

    let t_col = decl.find("T>").unwrap() as u32 + 1;
    if let Some(t) = toks.iter().find(|t| t.line == 1 && t.col == t_col) {
        assert_eq!(
            t.token_type,
            tok_type::TYPE,
            "template type parameter is a TYPE"
        );
    }

    let mut seen = std::collections::HashSet::new();
    for t in toks.iter() {
        assert!(
            seen.insert((t.line, t.col)),
            "duplicate token at {}:{}",
            t.line,
            t.col
        );
    }
}
