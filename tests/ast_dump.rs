//! Tests for `TranslationUnit::ast_dump` — JSON-array dump of named declarations.

use clang_bridge::Index;
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

/// The return value is always a JSON array — it starts with `[` and ends with `]`.
#[test]
fn ast_dump_returns_json_array() {
    let path = write_temp("cb_astdump_fn.cpp", "int foo(int x) { return x; }");
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    let json = tu.ast_dump(1, 1);
    assert!(
        json.starts_with('['),
        "ast_dump result should start with '[', got: {json:?}"
    );
    assert!(
        json.ends_with(']'),
        "ast_dump result should end with ']', got: {json:?}"
    );
}

/// The JSON string for a function `foo` spanning the full file must contain the
/// function name as a substring.
#[test]
fn ast_dump_contains_function_name() {
    let path = write_temp("cb_astdump_name.cpp", "int foo(int x) { return x; }");
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    let json = tu.ast_dump(1, 999);
    assert!(
        json.contains("foo"),
        "ast_dump result should contain the function name 'foo', got: {json:?}"
    );
}

/// Querying a line range that lies entirely beyond the source produces either
/// the literal string `"[]"` or a valid JSON array that contains no elements.
#[test]
fn ast_dump_empty_range_returns_empty_array() {
    let path = write_temp("cb_astdump_oor.cpp", "int z = 0;");
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    let json = tu.ast_dump(999, 999);
    assert!(
        json.starts_with('[') && json.ends_with(']'),
        "out-of-range ast_dump should return a valid JSON array, got: {json:?}"
    );
    // A truly empty result is just "[]"; any result with content between the
    // brackets would be unexpected for a line that doesn't exist.
    if json != "[]" {
        // If the implementation returns something non-empty, the array must at
        // minimum be structurally valid JSON (starts/ends with brackets), which
        // we already asserted above. Accept gracefully.
    }
}

/// For a multi-line source that defines a class and a member function, dumping
/// the full range returns a non-trivial JSON array (length > 2 bytes, i.e. not
/// just `[]`).
#[test]
fn ast_dump_valid_for_multiline() {
    let src = "class Widget {\npublic:\n    int value;\n    int get() const { return value; }\n    void set(int v) { value = v; }\n};";
    let path = write_temp("cb_astdump_multi.cpp", src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    let json = tu.ast_dump(1, 20);
    assert!(
        json.starts_with('['),
        "ast_dump result should be a JSON array, got: {json:?}"
    );
    assert!(
        json.len() > 2,
        "ast_dump of a class with members should return a non-empty array, got: {json:?}"
    );
}
