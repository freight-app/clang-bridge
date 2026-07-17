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
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

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
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

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
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let md = hover::hover_full(&tu, 1, 5).expect("expected full hover");
    // Should include "Defined in ..." footer
    assert!(
        md.contains("Defined in"),
        "expected definition location in hover, got: {md}"
    );
}

#[test]
fn hover_template_function_includes_template_params() {
    // HV-3: hovering on a function template should show 'template<typename T>' in the signature.
    let src = "template<typename T>\nT identity(T x) { return x; }";
    let path = write_temp("cb_hf_tmpl.cpp", src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let md = hover::hover_full(&tu, 2, 3).expect("expected hover on template function");
    assert!(
        md.contains("template") && md.contains("typename"),
        "expected template params in hover signature, got: {md}"
    );
}

/// A multi-line `///` paragraph must render with its lines space-joined, not
/// concatenated ("Brief line.More detail" → "Brief line. More detail").
#[test]
fn hover_full_joins_paragraph_lines_with_space() {
    let path = write_temp(
        "cb_hover_para.cpp",
        "/// Brief line.\n/// More detail here.\nint fn(int x);",
    );
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();
    let md = hover::hover_full(&tu, 3, 5).expect("hover");
    assert!(
        md.contains("Brief line. More detail here."),
        "lines should be space-joined:\n{md}"
    );
    assert!(
        !md.contains("line.More"),
        "lines must not run together:\n{md}"
    );
}

#[test]
fn hover_full_reports_the_resolved_placeholder_type() {
    let path = write_temp(
        "cb_hover_auto.cpp",
        "int main() { auto answer = 42; return answer; }",
    );
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let md = hover::hover_full(&tu, 1, 39).expect("hover on answer use");
    assert!(md.contains("Type: `int`"), "resolved type missing:\n{md}");
}

#[test]
fn hover_range_covers_the_complete_identifier() {
    let path = write_temp(
        "cb_hover_range.cpp",
        "int target = 42;\nint value = target;",
    );
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let range = hover::hover_range(&tu, 2, 15).expect("range from middle of target");
    assert_eq!(range.start_line, 2);
    assert_eq!(range.start_col, 13);
    assert_eq!(range.end_line, 2);
    assert_eq!(range.end_col, 19);
}
