use clang_bridge::{completion, goto, hover, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

#[test]
fn hover_markdown_returns_signature_and_brief() {
    let path = write_temp(
        "cb_lsp_hover.hpp",
        "/// Adds two integers.\nint add(int a, int b);",
    );
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    // Line 2, col 5 → `add`
    let md = hover::hover_markdown(&tu, 2, 5).expect("expected hover markdown");
    assert!(md.contains("add"), "signature missing: {md}");
    assert!(md.contains("Adds two integers"), "brief missing: {md}");
}

#[test]
fn goto_definition_resolves_function() {
    // Declaration on line 1, definition on line 2.
    let path = write_temp(
        "cb_lsp_goto.cpp",
        "int square(int x);\nint square(int x) { return x * x; }",
    );
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    // Hover over declaration (line 1 col 5) → definition should be line 2
    let loc = goto::goto_definition(&tu, 1, 5).expect("expected definition location");
    assert_eq!(
        loc.line, 2,
        "expected definition on line 2, got {}",
        loc.line
    );
}

#[test]
fn completion_returns_items() {
    // Source: line 2 ends with `f.` — col 23 is right after the dot.
    // "void use() { Foo f; f." has the dot at col 22, so complete at col 23.
    let src = "struct Foo { int bar; int baz; };\nvoid use() { Foo f; f.";
    let path = write_temp("cb_lsp_complete.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    let items: Vec<_> = completion::complete(&tu, 2, 23, None).collect();
    let labels: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
    assert!(
        labels.contains(&"bar") || labels.contains(&"baz"),
        "expected struct member completions, got: {labels:?}"
    );
}

#[test]
fn completion_kinds_field_and_method() {
    // Member completion must classify a field as Field(5) and a member
    // function as Method(2) — not Function(3).
    let src = "struct Foo { int bar; double baz() const; };\nvoid u(){ Foo f; f. }";
    let path = write_temp("cb_lsp_complete_kinds.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    let line = "void u(){ Foo f; f. }";
    let col = line.rfind('.').unwrap() as u32 + 2; // just past the dot
    let items: Vec<_> = completion::complete(&tu, 2, col, None).collect();
    let bar = items.iter().find(|i| i.label == "bar").expect("field bar");
    assert_eq!(bar.kind, 5, "field → Field(5)");
    assert_eq!(bar.detail.as_deref(), Some("int"), "field detail is its type");
    let baz = items.iter().find(|i| i.label == "baz").expect("method baz");
    assert_eq!(baz.kind, 2, "member function → Method(2)");
}
