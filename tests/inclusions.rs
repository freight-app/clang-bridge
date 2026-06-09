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

/// `cb_inclusions` backs LSP documentLink for the open document, so it must
/// return only directives written in the main file — never the transitive
/// system headers a `<...>` include pulls in.
#[test]
fn inclusions_only_main_file_directives() {
    write_temp("cb_incl_local.h", "int helper();\n");
    let main = write_temp(
        "cb_incl_main.cpp",
        "#include <cstddef>\n#include \"cb_incl_local.h\"\nint x;",
    );
    let dir = std::env::temp_dir();
    let idx = Index::new();
    let tu = idx
        .parse(main.to_str().unwrap(), dir.to_str().unwrap(), &["-std=c++17"])
        .unwrap();
    let incs: Vec<_> = inclusion::inclusions(&tu).iter().collect();
    assert_eq!(
        incs.len(), 2,
        "expected exactly the 2 main-file includes, got {:?}",
        incs.iter().map(|i| i.included_file.clone()).collect::<Vec<_>>()
    );
    assert!(incs.iter().all(|i| i.including_file.ends_with("cb_incl_main.cpp")));
    assert!(incs.iter().any(|i| i.included_file.ends_with("cb_incl_local.h")));
    assert!(incs.iter().any(|i| i.included_file.ends_with("cstddef")));
}
