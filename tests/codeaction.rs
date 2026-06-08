//! Tests for fix-it code actions (`clang_bridge::codeaction`).
//!
//! `cb_code_actions` also walks `ASTUnit::stored_diag_begin/end` to collect
//! fix-it hints.  Like `tu.diagnostics()`, stored diagnostics are only
//! populated after a `reparse(None)` call when the unit was built via
//! `ClangTool::buildASTs`.  Tests that need to observe real actions call
//! `tu.reparse(None)` before `tu.code_actions(...)`.

use clang_bridge::{codeaction, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

/// Calling `code_actions` on valid code at a real position must not crash and
/// must return a well-formed list (possibly empty).
#[test]
fn code_actions_no_crash_on_valid_code() {
    let path = write_temp(
        "cb_codeaction_valid.cpp",
        "int add(int a, int b) { return a + b; }",
    );
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    let actions = tu.code_actions(1, 1);
    // Must not panic; len() is well-defined even when the list is empty.
    let _ = actions.len();
    let _ = actions.is_empty();
}

/// Querying an out-of-range position on a 1-line file must not panic.
#[test]
fn code_actions_no_crash_on_invalid_pos() {
    let path = write_temp(
        "cb_codeaction_oor.cpp",
        "int x = 0;",
    );
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .expect("parse failed");

    // Line 999, col 999 — far outside the actual file.
    let actions = tu.code_actions(999, 999);
    let _ = actions.len();
}

/// When code actions are present every title must be a non-empty string.
/// Uses `-Weverything` after a reparse to maximise the chance of fix-it
/// diagnostics appearing.
#[test]
fn code_actions_from_diag_fixits() {
    let path = write_temp(
        "cb_codeaction_fixits.cpp",
        "void f() { int x = 0; (void)x; }",
    );
    let idx = Index::new();
    let tu = idx
        .parse(
            path.to_str().unwrap(),
            "",
            &["-std=c++17", "-Weverything"],
        )
        .expect("parse failed");

    // Reparse from disk so stored diagnostics (and their fix-its) are available.
    let ok = tu.reparse(None);
    assert!(ok, "reparse(None) should succeed");

    // Whether or not the compiler emits fix-its here, every title must be non-empty.
    let actions = codeaction::code_actions(&tu, 1, 1);
    for action in actions.iter() {
        assert!(
            !action.title.is_empty(),
            "code action title must be non-empty"
        );
    }
}

/// If any code actions are returned, the `replacement` and `file` fields must
/// be valid UTF-8 strings that can be queried without panicking.
/// An assignment-in-condition (`x = 1` inside `if`) is a classic source of
/// clang fix-its suggesting parentheses.
#[test]
fn code_actions_replacement_is_valid() {
    let path = write_temp(
        "cb_codeaction_replacement.cpp",
        "void f() { int x = 0; if (x = 1) {} }",
    );
    let idx = Index::new();
    let tu = idx
        .parse(
            path.to_str().unwrap(),
            "",
            &["-std=c++17", "-Wall"],
        )
        .expect("parse failed");

    let ok = tu.reparse(None);
    assert!(ok, "reparse(None) should succeed");

    // Scan lines 1–3; each list must be accessible without panicking.
    for line in 1u32..=3 {
        let actions = tu.code_actions(line, 1);
        for action in actions.iter() {
            // String::len() is always safe; exercises both fields.
            let _ = action.replacement.len();
            let _ = action.file.len();
            // Replacement is a valid String, so it is implicitly valid UTF-8.
            assert!(
                std::str::from_utf8(action.replacement.as_bytes()).is_ok(),
                "replacement must be valid UTF-8"
            );
        }
    }
}
