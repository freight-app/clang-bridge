//! Tests for cb_inclusions (#include graph).

use clang_bridge::{inclusion, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    f.write_all(content.as_bytes()).unwrap();
    path
}

#[test]
fn inclusions_finds_local_header() {
    // Write a real header to a temp path so the compiler can find it.
    let hdr = write_temp("cb_incl_math.h", "int double_it(int x) { return x * 2; }\n");
    let hdr_path = hdr.to_str().unwrap().to_string();

    let src = format!("#include \"{hdr_path}\"\nint use() {{ return double_it(3); }}");
    let cpp = write_temp("cb_incl_local.cpp", &src);

    let idx = Index::new();
    let tu = idx.parse(cpp.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let incls = inclusion::inclusions(&tu);
    let found = incls.iter().any(|i| i.included_file.contains("cb_incl_math"));
    assert!(
        found,
        "expected cb_incl_math.h in inclusions, got: {:?}",
        incls.iter().map(|i| i.included_file.clone()).collect::<Vec<_>>()
    );
}

#[test]
fn inclusions_records_directive_line() {
    let hdr = write_temp("cb_incl_line.h", "int x = 0;\n");
    let hdr_path = hdr.to_str().unwrap().to_string();

    // The #include is on line 2 (after a comment).
    let src = format!("// comment\n#include \"{hdr_path}\"\nint y;");
    let cpp = write_temp("cb_incl_lineno.cpp", &src);

    let idx = Index::new();
    let tu = idx.parse(cpp.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let incls = inclusion::inclusions(&tu);
    let found = incls.iter().find(|i| i.included_file.contains("cb_incl_line.h"));
    if let Some(inc) = found {
        assert_eq!(inc.line, 2, "expected include on line 2, got {}", inc.line);
    } else {
        panic!(
            "cb_incl_line.h not found in inclusions: {:?}",
            incls.iter().map(|i| i.included_file.clone()).collect::<Vec<_>>()
        );
    }
}
