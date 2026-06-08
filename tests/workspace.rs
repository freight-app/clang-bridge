//! Tests for workspace symbol indexing and search.

use clang_bridge::{workspace, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

#[test]
fn workspace_finds_function_by_name() {
    let src = "void foo(int x) { (void)x; }\nvoid bar(int y) { (void)y; }";
    let path = write_temp("cb_workspace_fn.cpp", src);

    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    workspace::workspace_index_add(&idx, &tu);

    let results = workspace::workspace_symbols(&idx, "foo");
    assert!(
        results.len() >= 1,
        "expected at least 1 result for query 'foo', got {}",
        results.len()
    );
    assert!(
        results.iter().any(|s| s.name.contains("foo")),
        "expected a symbol named 'foo' in results, got: {:?}",
        results.iter().map(|s| s.name.clone()).collect::<Vec<_>>()
    );
}

#[test]
fn workspace_empty_query_returns_all() {
    let src = "void foo() {}\nvoid bar() {}";
    let path = write_temp("cb_workspace_all.cpp", src);

    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    workspace::workspace_index_add(&idx, &tu);

    let results = workspace::workspace_symbols(&idx, "");
    let names: Vec<String> = results.iter().map(|s| s.name.clone()).collect();
    assert!(
        results.len() >= 2,
        "expected at least 2 results for empty query, got {}: {:?}",
        results.len(),
        names
    );
    assert!(
        names.iter().any(|n| n.contains("foo")),
        "expected 'foo' in empty-query results: {names:?}"
    );
    assert!(
        names.iter().any(|n| n.contains("bar")),
        "expected 'bar' in empty-query results: {names:?}"
    );
}

#[test]
fn workspace_case_insensitive_search() {
    let src = "class MyClass { public: void method() {} };";
    let path = write_temp("cb_workspace_case.cpp", src);

    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    workspace::workspace_index_add(&idx, &tu);

    let results = workspace::workspace_symbols(&idx, "myclass");
    assert!(
        results.len() >= 1,
        "expected at least 1 result for lowercase query 'myclass', got {}",
        results.len()
    );
    assert!(
        results.iter().any(|s| s.name.to_lowercase().contains("myclass")),
        "expected a symbol matching 'myclass' (case-insensitive), got: {:?}",
        results.iter().map(|s| s.name.clone()).collect::<Vec<_>>()
    );
}

#[test]
fn workspace_dedup_on_reparse() {
    let src = "void unique_fn() {}";
    let path = write_temp("cb_workspace_dedup.cpp", src);

    let idx = Index::new();

    // First parse + index.
    let tu1 = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    workspace::workspace_index_add(&idx, &tu1);

    let count_after_first = workspace::workspace_symbols(&idx, "unique_fn").len();
    assert!(
        count_after_first >= 1,
        "expected at least 1 result after first index, got {count_after_first}"
    );

    // Second parse (reparse) + index — should replace, not duplicate.
    let tu2 = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    workspace::workspace_index_add(&idx, &tu2);

    let count_after_second = workspace::workspace_symbols(&idx, "unique_fn").len();
    assert_eq!(
        count_after_second, count_after_first,
        "expected dedup: count should not grow after reparse ({count_after_first} -> {count_after_second})"
    );
}
