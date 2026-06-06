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
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let toks = semtok::semantic_tokens(&tu);
    assert!(!toks.is_empty(), "expected tokens");

    // 'add' should be classified as a function.
    let fn_toks: Vec<_> = toks.iter().filter(|t| t.token_type == tok_type::FUNCTION).collect();
    assert!(!fn_toks.is_empty(), "expected at least one FUNCTION token");
}

#[test]
fn semantic_tokens_sorted_by_position() {
    let src = "int x; int y; int z;";
    let path = write_temp("cb_semtok_sort.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

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
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let toks = semtok::semantic_tokens(&tu);
    let type_toks: Vec<_> = toks.iter().filter(|t| t.token_type == tok_type::TYPE).collect();
    assert!(!type_toks.is_empty(), "expected TYPE token for struct Point");
}
