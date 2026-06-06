//! Tests for cb_document_symbols / DocSymList.

use clang_bridge::Index;
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

/// Collect all symbols into a vec for easy assertion.
/// `tag` makes the temp-file name unique so parallel tests don't clobber each other.
fn collect(tag: &str, src: &str) -> Vec<(String, String, i32)> {
    let path = write_temp(&format!("cb_docsym_{tag}.cpp"), src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();
    let list = tu.document_symbols().expect("expected symbol list");
    (0..list.len())
        .map(|i| {
            let s = list.get(i);
            (s.name, s.kind, s.parent)
        })
        .collect()
}

#[test]
fn document_symbols_finds_top_level_function() {
    let syms = collect("fn", "int add(int a, int b) { return a + b; }");
    assert!(
        syms.iter().any(|(name, kind, parent)| name == "add" && kind == "function" && *parent == -1),
        "expected top-level function 'add', got: {syms:?}"
    );
}

#[test]
fn document_symbols_finds_class_and_methods() {
    let src = "class Foo { public: void bar(); void baz(); };\nvoid Foo::bar() {}\nvoid Foo::baz() {}";
    let syms = collect("class", src);

    // Foo should appear as a class at top level
    let foo_idx = syms.iter().position(|(n, k, p)| n == "Foo" && k == "class" && *p == -1);
    assert!(foo_idx.is_some(), "expected class Foo, got: {syms:?}");

    // bar and baz should be methods with Foo as parent
    let foo_i = foo_idx.unwrap() as i32;
    let has_bar = syms.iter().any(|(n, k, p)| n == "bar" && k == "method" && *p == foo_i);
    assert!(has_bar, "expected method bar with parent Foo, got: {syms:?}");
}

#[test]
fn document_symbols_finds_struct_fields() {
    let src = "struct Point { int x; int y; };";
    let syms = collect("struct", src);

    let pt_idx = syms.iter().position(|(n, k, _)| n == "Point" && k == "struct");
    assert!(pt_idx.is_some(), "expected struct Point, got: {syms:?}");

    let pt_i = pt_idx.unwrap() as i32;
    assert!(
        syms.iter().any(|(n, k, p)| n == "x" && k == "field" && *p == pt_i),
        "expected field x with parent Point, got: {syms:?}"
    );
    assert!(
        syms.iter().any(|(n, k, p)| n == "y" && k == "field" && *p == pt_i),
        "expected field y with parent Point, got: {syms:?}"
    );
}

#[test]
fn document_symbols_finds_enum_constants() {
    let src = "enum Color { Red, Green, Blue };";
    let syms = collect("enum", src);

    let color_idx = syms.iter().position(|(n, k, _)| n == "Color" && k == "enum");
    assert!(color_idx.is_some(), "expected enum Color, got: {syms:?}");

    let ci = color_idx.unwrap() as i32;
    for name in ["Red", "Green", "Blue"] {
        assert!(
            syms.iter().any(|(n, k, p)| n == name && k == "enumconst" && *p == ci),
            "expected enumconst {name} with parent Color, got: {syms:?}"
        );
    }
}

#[test]
fn document_symbols_namespace_nesting() {
    let src = "namespace math { int square(int x) { return x*x; } }";
    let syms = collect("ns", src);

    let ns_idx = syms.iter().position(|(n, k, p)| n == "math" && k == "namespace" && *p == -1);
    assert!(ns_idx.is_some(), "expected namespace math, got: {syms:?}");

    let ns_i = ns_idx.unwrap() as i32;
    assert!(
        syms.iter().any(|(n, k, p)| n == "square" && k == "function" && *p == ns_i),
        "expected function square nested under math, got: {syms:?}"
    );
}
