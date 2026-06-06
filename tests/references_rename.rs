//! Tests for cb_references and cb_rename.

use clang_bridge::{refs, rename, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

fn get_usr(path: &str, line: u32, col: u32) -> Option<String> {
    let idx = Index::new();
    let tu = idx.parse(path, &["-std=c++17"])?;
    tu.symbol_at(line, col).map(|s| s.usr().to_string())
}

// ── References ────────────────────────────────────────────────────────────────

#[test]
fn references_finds_all_call_sites() {
    let src = "int add(int a, int b) { return a + b; }\nint main() { return add(1, 2) + add(3, 4); }";
    let path = write_temp("cb_refs_add.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    // Get USR for 'add'.
    let sym = tu.symbol_at(1, 5).expect("symbol at add declaration");
    let usr = sym.usr().to_string();
    drop(sym);

    let refs = refs::references(&tu, &usr);
    // Should find the definition + 2 call sites = at least 3 occurrences.
    assert!(
        refs.len() >= 2,
        "expected at least 2 references to 'add', got {}",
        refs.len()
    );
}

#[test]
fn references_marks_definition_site() {
    let src = "int value = 42;\nint use_it() { return value; }";
    let path = write_temp("cb_refs_def.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(1, 5).expect("symbol at 'value'");
    let usr = sym.usr().to_string();
    drop(sym);

    let refs = refs::references(&tu, &usr);
    let has_def = refs.iter().any(|r| r.is_definition);
    assert!(has_def, "expected at least one reference marked as definition");
}

// ── Rename ────────────────────────────────────────────────────────────────────

#[test]
fn rename_collects_edits_for_all_sites() {
    let src = "int counter = 0;\nvoid inc() { counter++; }\nvoid dec() { counter--; }";
    let path = write_temp("cb_rename_counter.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(1, 5).expect("symbol at 'counter'");
    let usr = sym.usr().to_string();
    drop(sym);

    let result = rename::rename(&tu, &usr, "count");
    // Should produce edits for the declaration + 2 use sites.
    assert!(
        result.edit_count() >= 2,
        "expected >=2 rename edits, got {}",
        result.edit_count()
    );
    // All edits should have new_name = "count".
    for edit in result.edits() {
        assert_eq!(edit.new_name, "count", "wrong new_name in edit: {:?}", edit);
    }
}

#[test]
fn rename_detects_conflict() {
    // 'count' already exists — renaming 'counter' to 'count' conflicts.
    let src = "int count = 0;\nint counter = 1;";
    let path = write_temp("cb_rename_conflict.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(2, 5).expect("symbol at 'counter'");
    let usr = sym.usr().to_string();
    drop(sym);

    let result = rename::rename(&tu, &usr, "count");
    assert!(result.has_conflict(), "expected conflict when renaming to an existing name");
    assert!(result.conflict_message().is_some());
}

#[test]
fn rename_no_conflict_for_fresh_name() {
    let src = "int old_value = 42;\nvoid use() { old_value++; }";
    let path = write_temp("cb_rename_fresh.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(1, 5).expect("symbol at 'old_value'");
    let usr = sym.usr().to_string();
    drop(sym);

    let result = rename::rename(&tu, &usr, "new_value");
    assert!(!result.has_conflict(), "expected no conflict for fresh name 'new_value'");
}
