//! Verifies graceful degradation on malformed input (`broken.cpp`) — the state
//! an LSP server sees on almost every keystroke. The bridge must still return a
//! translation unit, surface the errors as diagnostics, and answer queries about
//! the parts that did parse, all without crashing.

use clang_bridge::{diag::Severity, Index, TranslationUnit};
use std::path::Path;

fn load() -> Option<TranslationUnit> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let cpp = dir.join("broken.cpp");
    let idx = Index::new();
    let tu = idx.parse(cpp.to_str().unwrap(), dir.to_str().unwrap(), &["-std=c++17"]);
    // keep the index alive for the duration of the TU by leaking it; tests are short
    std::mem::forget(idx);
    tu
}

#[test]
fn broken_source_still_produces_a_translation_unit() {
    assert!(load().is_some(), "a malformed buffer must still yield a TU");
}

#[test]
fn broken_source_reports_errors_as_diagnostics() {
    let tu = load().expect("TU");
    let diags: Vec<_> = tu.diagnostics().collect();
    let errors = diags.iter().filter(|d| matches!(d.severity, Severity::Error | Severity::Fatal)).count();
    assert!(
        errors >= 3,
        "expected the missing-semicolon, undeclared-identifier and expected-expression \
         errors to be reported; got {diags:?}"
    );
    // The undeclared-identifier error must mention the offending name.
    assert!(
        diags.iter().any(|d| d.message.contains("mystery_value")),
        "expected a diagnostic about `mystery_value`; got {:?}",
        diags.iter().map(|d| d.message.clone()).collect::<Vec<_>>()
    );
}

#[test]
fn well_formed_parts_still_resolve() {
    let tu = load().expect("TU");
    // Despite the errors elsewhere, the well-formed `Widget` struct and its
    // members must still appear in the outline, correctly nested.
    let syms: Vec<_> = tu.document_symbols().expect("document symbols").iter().collect();
    let widget = syms.iter().position(|s| s.name == "Widget" && s.kind == "struct")
        .expect("struct Widget should still be indexed");
    let area = syms.iter().find(|s| s.name == "area" && s.kind == "method")
        .expect("Widget::area should still be indexed");
    assert_eq!(area.parent, widget as i32, "Widget::area nests under Widget");

    // A function before the first error still resolves.
    assert!(
        syms.iter().any(|s| s.name == "compute" && s.kind == "function"),
        "compute() should still be indexed"
    );
}
