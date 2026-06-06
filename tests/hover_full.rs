//! Tests for cb_hover_full — full structured hover with @param/@returns.

use clang_bridge::{hover, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

#[test]
fn hover_full_includes_signature() {
    let src = "/// Add two integers.\nint add(int a, int b);";
    let path = write_temp("cb_hf_sig.hpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let md = hover::hover_full(&tu, 2, 5).expect("expected full hover");
    assert!(md.contains("add"), "signature missing: {md}");
    assert!(md.contains("```cpp"), "code block missing: {md}");
}

#[test]
fn hover_full_includes_full_comment() {
    // Comment spans multiple lines; hover_markdown only returns the first.
    let src = "/// Brief line.\n/// Second line.\nint value;";
    let path = write_temp("cb_hf_full.hpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let md = hover::hover_full(&tu, 3, 5).expect("expected full hover");
    // The full text should include the second line
    assert!(
        md.contains("Second line"),
        "expected full comment in hover, got: {md}"
    );
}

#[test]
fn hover_full_includes_definition_location() {
    let src = "int square(int x) { return x*x; }";
    let path = write_temp("cb_hf_loc.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let md = hover::hover_full(&tu, 1, 5).expect("expected full hover");
    // Should include "Defined in ..." footer
    assert!(
        md.contains("Defined in"),
        "expected definition location in hover, got: {md}"
    );
}
