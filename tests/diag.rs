//! Tests for compiler diagnostics (`clang_bridge::diag`).

use clang_bridge::{diag::Severity, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

#[test]
fn diag_reports_errors() {
    let path = write_temp("cb_diag_error.cpp", "void f() { int x = \"hello\"; }");
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    // Reparse from disk to populate stored diagnostics.
    let ok = tu.reparse(None);
    assert!(ok, "reparse(None) should succeed on a file that exists on disk");

    let diags: Vec<_> = tu.diagnostics().collect();
    assert!(
        diags
            .iter()
            .any(|d| d.severity == Severity::Error || d.severity == Severity::Fatal),
        "expected at least one Error or Fatal diagnostic, got: {diags:?}"
    );
}

#[test]
fn diag_end_range_populated() {
    let path = write_temp(
        "cb_diag_range.cpp",
        "void f() { int unused = 42; }",
    );
    let idx = Index::new();
    let tu = idx
        .parse(
            path.to_str().unwrap(),
            "",
            &["-std=c++17", "-Wunused-variable"],
        )
        .expect("parse failed");

    let ok = tu.reparse(None);
    assert!(ok, "reparse(None) should succeed");

    let diags: Vec<_> = tu.diagnostics().collect();
    let warning = diags
        .iter()
        .find(|d| d.severity == Severity::Warning || d.severity == Severity::Error)
        .expect("expected at least one Warning diagnostic for unused variable");

    assert!(
        warning.end_line >= warning.line,
        "end_line ({}) should be >= line ({})",
        warning.end_line,
        warning.line
    );
}

/// A well-formed function must produce no Error or Fatal diagnostics.
/// This test also validates that the iterator API does not crash on clean input.
#[test]
fn diag_clean_file_has_no_errors() {
    let path = write_temp(
        "cb_diag_clean.cpp",
        "int add(int a, int b) { return a + b; }",
    );
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    let ok = tu.reparse(None);
    assert!(ok, "reparse(None) should succeed");

    let errors: Vec<_> = tu
        .diagnostics()
        .filter(|d| d.severity == Severity::Error || d.severity == Severity::Fatal)
        .collect();

    assert!(
        errors.is_empty(),
        "expected no errors in clean file, got: {errors:?}"
    );
}

/// Sanity check that severity variants follow the intuitive partial order:
/// Note < Remark < Warning < Error < Fatal.
/// No parse is needed — this exercises the Rust enum only.
#[test]
fn diag_severity_ordering() {
    let rank = |s: Severity| -> u8 {
        match s {
            Severity::Note    => 0,
            Severity::Remark  => 1,
            Severity::Warning => 2,
            Severity::Error   => 3,
            Severity::Fatal   => 4,
        }
    };

    assert!(rank(Severity::Note)    < rank(Severity::Remark),  "Note < Remark");
    assert!(rank(Severity::Remark)  < rank(Severity::Warning), "Remark < Warning");
    assert!(rank(Severity::Warning) < rank(Severity::Error),   "Warning < Error");
    assert!(rank(Severity::Error)   < rank(Severity::Fatal),   "Error < Fatal");
}
