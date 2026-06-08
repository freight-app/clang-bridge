//! Tests for the type-hierarchy API: type_hierarchy_prepare, supertypes, subtypes.

use clang_bridge::{typehier, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

/// C++ source used by the hierarchy tests.
const HIER_SRC: &str = "struct Base { virtual void f(); };\nstruct Child : Base { void f() override; };\nstruct GrandChild : Child {};";

// ── type_hierarchy_prepare ────────────────────────────────────────────────────

#[test]
fn typehier_prepare_on_class() {
    // "Base" starts at col 8 on line 1: "struct Base" — 'B' is col 8.
    let path = write_temp("cb_typehier_hier.cpp", HIER_SRC);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let item = typehier::type_hierarchy_prepare(&tu, 1, 8)
        .expect("expected Some(TypeHierItem) for 'Base'");
    assert_eq!(item.name(), "Base", "expected name 'Base', got: {}", item.name());
}

#[test]
fn typehier_prepare_returns_none_on_non_type() {
    // "x" in "int x = 5;" is a variable, not a type — prepare should return None.
    let src = "int x = 5;";
    let path = write_temp("cb_typehier_nontype.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 5 is on 'x'.
    let result = typehier::type_hierarchy_prepare(&tu, 1, 5);
    assert!(result.is_none(), "expected None for variable 'x', got Some");
}

// ── supertypes ────────────────────────────────────────────────────────────────

#[test]
fn typehier_supertypes_of_child() {
    // "Child" inherits from "Base" — supertypes should return an entry named "Base".
    let path = write_temp("cb_typehier_super.cpp", HIER_SRC);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // Obtain the USR for "Child" (line 2, col 8).
    let sym = tu.symbol_at(2, 8).expect("symbol at 'Child'");
    let child_usr = sym.usr().to_string();
    drop(sym);

    let supers = typehier::supertypes(&tu, &child_usr);
    assert!(
        supers.iter().any(|e| e.name == "Base"),
        "expected 'Base' in supertypes of 'Child', got: {:?}",
        supers.iter().map(|e| e.name.clone()).collect::<Vec<_>>(),
    );
}

#[test]
fn typehier_subtypes_of_base() {
    // "Child" derives from "Base" — subtypes of "Base" should contain "Child".
    let path = write_temp("cb_typehier_sub.cpp", HIER_SRC);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // Obtain the USR for "Base" (line 1, col 8).
    let sym = tu.symbol_at(1, 8).expect("symbol at 'Base'");
    let base_usr = sym.usr().to_string();
    drop(sym);

    let subs = typehier::subtypes(&tu, &base_usr);
    assert!(
        subs.iter().any(|e| e.name == "Child"),
        "expected 'Child' in subtypes of 'Base', got: {:?}",
        subs.iter().map(|e| e.name.clone()).collect::<Vec<_>>(),
    );
}

#[test]
fn typehier_no_supertypes_for_root() {
    // "Base" has no base class — supertypes should be empty.
    let path = write_temp("cb_typehier_root.cpp", HIER_SRC);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(1, 8).expect("symbol at 'Base'");
    let base_usr = sym.usr().to_string();
    drop(sym);

    let supers = typehier::supertypes(&tu, &base_usr);
    assert!(
        supers.is_empty(),
        "expected no supertypes for root class 'Base', got {} entries",
        supers.len(),
    );
}
