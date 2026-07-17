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

/// A type mismatch is an `Error` — not a `Fatal`. Guards the clang-Level →
/// CB-severity mapping (a direct cast is off by one and inflates Error→Fatal).
#[test]
fn diag_type_error_is_error_not_fatal() {
    let path = write_temp("cb_diag_sev_err.cpp", "void f() { int x = \"bad\"; }");
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    let diagnostics: Vec<_> = tu.diagnostics().collect();
    let sevs: Vec<_> = diagnostics.iter().map(|d| d.severity).collect();
    assert!(sevs.contains(&Severity::Error), "expected an Error, got {sevs:?}");
    assert!(!sevs.contains(&Severity::Fatal), "type error must not be Fatal: {sevs:?}");
    assert!(
        diagnostics.iter().all(|d| d.include_anchor.is_none()),
        "main-file diagnostics must not have include anchors: {diagnostics:?}"
    );
}

/// A "'x' declared here" note must be reported at `Note` severity, not `Remark`.
#[test]
fn diag_declared_here_is_note() {
    let path = write_temp("cb_diag_note.cpp", "void print();\nvoid g() { prin(); }");
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    let has_note = tu
        .diagnostics()
        .any(|d| d.severity == Severity::Note && d.message.contains("declared here"));
    assert!(has_note, "expected a Note-severity 'declared here' diagnostic");
}

#[test]
fn header_diagnostic_points_to_outermost_main_file_include() {
    let dir = tempfile::tempdir().expect("header diagnostic fixture");
    let main = dir.path().join("main.cpp");
    let outer = dir.path().join("outer.hpp");
    let inner = dir.path().join("inner.hpp");
    std::fs::write(&main, "#include \"outer.hpp\"\nint main() { return 0; }\n").unwrap();
    std::fs::write(&outer, "#pragma once\n#include \"inner.hpp\"\n").unwrap();
    std::fs::write(&inner, "#pragma once\ninline int broken = missing_name;\n").unwrap();

    let idx = Index::new();
    let tu = idx
        .parse(main.to_str().unwrap(), dir.path().to_str().unwrap(), &["-std=c++17"])
        .expect("parse failed");
    assert!(tu.reparse(None), "reparse should retain header diagnostics");

    let diagnostic = tu
        .diagnostics()
        .find(|d| d.file == inner.to_string_lossy() && d.message.contains("missing_name"))
        .expect("expected diagnostic from nested header");
    assert_eq!(
        diagnostic.include_anchor,
        Some(clang_bridge::diag::IncludeAnchor {
            file: main.to_string_lossy().into_owned(),
            line: 1,
            col: 10,
        })
    );
}
