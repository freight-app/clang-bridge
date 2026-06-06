use std::io::Write;

#[test]
fn tidy_finds_modernize_warnings() {
    let dir = std::env::temp_dir();
    let path = dir.join("cb_test_tidy.cpp");
    {
        let mut f = std::fs::File::create(&path).unwrap();
        // NULL macro usage triggers modernize-use-nullptr
        writeln!(f, "#include <cstddef>\nvoid foo() {{ int *p = NULL; (void)p; }}").unwrap();
    }

    let findings: Vec<_> = clang_bridge::tidy::run(
        None,
        path.to_str().unwrap(),
        Some("modernize-use-nullptr"),
        &["-std=c++17"],
    ).collect();

    // We should get at least one modernize-use-nullptr warning.
    let has_nullptr_warn = findings.iter().any(|d| {
        d.check_name.as_deref() == Some("modernize-use-nullptr")
    });
    assert!(has_nullptr_warn,
        "expected modernize-use-nullptr warning, got: {findings:#?}");
}
