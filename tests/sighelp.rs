//! Tests for cb_signature_help / SignatureHelp.

use clang_bridge::{sighelp, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

#[test]
fn signature_help_returns_overload_at_call_site() {
    // Source: `add(1, ` — cursor inside the second argument of a call to `add`.
    // Line 2, col 8 is right after the comma (inside arg 1, 0-based).
    let src = "int add(int a, int b) { return a+b; }\nvoid use() { add(1, 2); }";
    // col 21 is at the '2' (second argument, 0-based index 1).
    let path = write_temp("cb_sighelp_basic.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    let sh = sighelp::signature_help(&tu, 2, 21)
        .expect("expected signature help at call site");

    assert!(!sh.overloads.is_empty(), "expected at least one overload");
    let ov = &sh.overloads[0];
    assert!(
        ov.label.contains("add"),
        "expected label to mention 'add', got: {}",
        ov.label
    );
    assert_eq!(ov.params.len(), 2, "expected 2 parameters, got: {:?}", ov.params);
}

#[test]
fn signature_help_tracks_active_param() {
    // "void use() { f(1, 2, 3); }"
    //               123456789012345678901234  (1-based cols)
    //  f is at col 14, '(' col 15, '1' col 16, '2' col 19, '3' col 22
    let src = "void f(int x, int y, int z) {}\nvoid use() { f(1, 2, 3); }";
    let path = write_temp("cb_sighelp_active.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    // col 22 = on '3' (third argument, 0-based index 2)
    let sh = sighelp::signature_help(&tu, 2, 22)
        .expect("expected signature help");

    assert_eq!(
        sh.active_param, 2,
        "expected active_param=2, got {}",
        sh.active_param
    );
}
