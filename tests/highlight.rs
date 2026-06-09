//! Tests for cb_highlight / tu.highlight() — LSP textDocument/documentHighlight.

use clang_bridge::{highlight, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

/// A variable declared on line 1 and used three more times. Calling highlight
/// at the declaration site must return at least 3 entries (the declaration
/// itself plus the read sites).
#[test]
fn highlight_finds_all_occurrences() {
    let src = "int counter = 0;\nvoid a() { counter++; }\nvoid b() { counter--; }\nvoid c() { int x = counter; }";
    let path = write_temp("cb_highlight_all.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // `counter` declaration starts at line 1, col 5.
    let list = tu.highlight(1, 5);
    assert!(
        list.len() >= 3,
        "expected at least 3 highlight entries for 'counter', got {}",
        list.len()
    );
}

/// The entry at the declaration site must carry kind == 3 (write / definition).
#[test]
fn highlight_definition_is_write_kind() {
    let src = "int value = 42;\nint use_it() { return value; }";
    let path = write_temp("cb_highlight_def_kind.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // `value` is declared at line 1, col 5.
    let list = tu.highlight(1, 5);
    assert!(!list.is_empty(), "expected at least one highlight entry");

    let has_write = list.iter().any(|e| e.kind == 3);
    assert!(
        has_write,
        "expected at least one entry with kind==3 (write/definition), got: {:?}",
        list.iter().map(|e| (e.line, e.col, e.kind)).collect::<Vec<_>>()
    );
}

/// Entries at pure read sites must carry kind == 2 (read).
#[test]
fn highlight_read_sites_are_read_kind() {
    // `score` is declared on line 1, then only read (never written again) on
    // lines 2 and 3.
    let src = "int score = 10;\nint double_it() { return score * 2; }\nint triple_it() { return score * 3; }";
    let path = write_temp("cb_highlight_read_kind.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // Highlight the declaration of `score` at line 1, col 5.
    let list = highlight::highlight(&tu, 1, 5);
    assert!(!list.is_empty(), "expected highlight entries for 'score'");

    // Every entry that is NOT the declaration (i.e. not on line 1 at the
    // initialiser) should be a read (kind == 2).
    let non_def_entries: Vec<_> = list.iter().filter(|e| e.kind != 3).collect();
    assert!(
        !non_def_entries.is_empty(),
        "expected at least one read-kind entry besides the definition"
    );
    for e in &non_def_entries {
        assert_eq!(
            e.kind, 2,
            "expected kind==2 (read) for non-definition entry at line {}, col {}",
            e.line, e.col
        );
    }
}

/// Calling highlight on a position that holds no symbol (whitespace between
/// tokens) must not panic and may return an empty list.
#[test]
fn highlight_empty_on_non_symbol() {
    // The space between `int` and `x` is at col 4 on line 1.
    let src = "int x = 1;";
    let path = write_temp("cb_highlight_nosym.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 4 is the space character between `int` and `x`.
    let list = tu.highlight(1, 4);
    // We only assert it does not panic; an empty result is perfectly fine.
    let _ = list.len();
}

/// Highlight must not contain duplicate ranges, and must classify the
/// definition + assignment as write (kind 3) and a plain use as read (kind 2).
#[test]
fn highlight_dedups_and_classifies_read_write() {
    let src = "void f() {\n    int x = 0;\n    x = 5;\n    int y = x;\n    (void)y;\n}";
    let path = write_temp("cb_highlight_rw.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();
    let hl = highlight::highlight(&tu, 2, 9); // 'x' declaration

    let mut seen = std::collections::HashSet::new();
    for e in hl.iter() {
        assert!(seen.insert((e.line, e.col)), "duplicate highlight at {}:{}", e.line, e.col);
    }
    assert_eq!(hl.iter().find(|e| e.line == 2).unwrap().kind, 3, "definition is write");
    assert_eq!(hl.iter().find(|e| e.line == 3).unwrap().kind, 3, "assignment is write");
    assert_eq!(hl.iter().find(|e| e.line == 4).unwrap().kind, 2, "use is read");
}
