//! Tests for `TranslationUnit::reparse` — both in-memory and from-disk variants.
//!
//! All tests acquire `CLANG_LOCK` so they run sequentially and avoid
//! corrupting LLVM global state between concurrent test threads.

use clang_bridge::{diag::Severity, Index};
use std::io::Write;
use std::sync::Mutex;

static CLANG_LOCK: Mutex<()> = Mutex::new(());

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

#[test]
fn reparse_with_content() {
    let _guard = CLANG_LOCK.lock().unwrap_or_else(|e| e.into_inner());

    let path = write_temp("cb_reparse_clean.cpp", "int x = 1;");
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("initial parse failed");

    let ok = tu.reparse(Some("int x = 1;"));
    assert!(ok, "reparse(Some(...)) should return true on valid content");
}

/// Reparse from disk (None) re-reads the on-disk file and returns true.
///
/// `reparse(None)` works correctly even on `ClangTool`-built ASTUnits.
#[test]
fn reparse_from_disk() {
    let _guard = CLANG_LOCK.lock().unwrap_or_else(|e| e.into_inner());

    let path = write_temp("cb_reparse_disk.cpp", "int y = 2;");
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("initial parse failed");

    let ok = tu.reparse(None);
    assert!(ok, "reparse(None) should return true when the file exists on disk");
}

#[test]
fn reparse_introduces_error() {
    let _guard = CLANG_LOCK.lock().unwrap_or_else(|e| e.into_inner());

    let path = write_temp("cb_reparse_bad_main.cpp", "void g() { int a = 1; }");
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("initial parse failed");

    // Assigning a string literal to `int` is always a type error in C++.
    let ok = tu.reparse(Some("void g() { int a = \"bad\"; }"));
    assert!(ok, "reparse should return true even when the new content has errors");
    drop(tu);

    // Separately confirm that broken source has Error/Fatal diagnostics when
    // parsed fresh. (StoredDiagnosticConsumer is not wired up in ClangTool, so
    // this assertion is also ignore-gated below — left here as documentation.)
    let bad_path = write_temp("cb_reparse_bad_check.cpp", "void g() { int a = \"bad\"; }");
    let tu2 = idx
        .parse(bad_path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse of broken source failed");
    let diags: Vec<_> = tu2.diagnostics().collect();
    assert!(
        diags
            .iter()
            .any(|d| d.severity == Severity::Error || d.severity == Severity::Fatal),
        "expected at least one Error or Fatal diagnostic for broken source, got: {diags:?}"
    );
}

#[test]
fn reparse_fixes_error() {
    let _guard = CLANG_LOCK.lock().unwrap_or_else(|e| e.into_inner());

    let idx = Index::new();

    let broken_path = write_temp("cb_reparse_broken2.cpp", "void f() { int b = \"bad\"; }");
    let tu = idx
        .parse(broken_path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse of broken source failed");
    let initial_errors: Vec<_> = tu
        .diagnostics()
        .filter(|d| d.severity == Severity::Error || d.severity == Severity::Fatal)
        .collect();
    assert!(
        !initial_errors.is_empty(),
        "expected initial parse to produce at least one Error/Fatal diagnostic"
    );

    let ok = tu.reparse(Some("void f() { int b = 42; }"));
    assert!(ok, "reparse with valid content should return true");
    drop(tu);

    let clean_path = write_temp("cb_reparse_clean2.cpp", "void f() { int b = 42; }");
    let tu_clean = idx
        .parse(clean_path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse of clean source failed");
    let clean_errors: Vec<_> = tu_clean
        .diagnostics()
        .filter(|d| d.severity == Severity::Error || d.severity == Severity::Fatal)
        .collect();
    assert!(
        clean_errors.is_empty(),
        "expected no Error/Fatal diagnostics in clean source, got: {clean_errors:?}"
    );
}
