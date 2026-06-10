//! Tests for cb_folding_ranges / tu.folding_ranges() — LSP textDocument/foldingRange.

use clang_bridge::{folding, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

/// A multi-line function body must produce at least one folding range whose
/// start_line < end_line and whose kind is "region".
#[test]
fn folding_function_body() {
    let src = "int add(int a, int b) {\n    int sum = a + b;\n    return sum;\n}";
    let path = write_temp("cb_folding_fn.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let ranges = tu.folding_ranges();
    assert!(
        !ranges.is_empty(),
        "expected at least one folding range for function body, got none"
    );

    let has_region = ranges.iter().any(|r| r.start_line < r.end_line && r.kind == "region");
    assert!(
        has_region,
        "expected a 'region' range with start_line < end_line, got: {:?}",
        ranges.iter().map(|r| (r.start_line, r.end_line, r.kind.clone())).collect::<Vec<_>>()
    );
}

/// A multi-line class definition must produce at least one folding range
/// covering it (start_line < end_line, kind == "region").
#[test]
fn folding_class_body() {
    let src = "class Point {\n    int x;\n    int y;\npublic:\n    Point(int x, int y) : x(x), y(y) {}\n};";
    let path = write_temp("cb_folding_class.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let ranges = folding::folding_ranges(&tu);
    let class_range = ranges
        .iter()
        .find(|r| r.start_line == 1 && r.end_line > 1 && r.kind == "region");
    assert!(
        class_range.is_some(),
        "expected a folding range for the class body starting at line 1, got: {:?}",
        ranges.iter().map(|r| (r.start_line, r.end_line, r.kind.clone())).collect::<Vec<_>>()
    );
}

/// A multi-line namespace must produce at least one folding range for it.
#[test]
fn folding_namespace() {
    // The namespace spans lines 1-4; the FoldingVisitor requires end_line > start_line + 1,
    // so we give it at least three source lines inside.
    let src = "namespace geo {\nstruct Vec {};\nstruct Mat {};\nstruct Quat {};\n}";
    let path = write_temp("cb_folding_ns.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let ranges = tu.folding_ranges();
    let ns_range = ranges
        .iter()
        .find(|r| r.start_line == 1 && r.end_line >= 4 && r.kind == "region");
    assert!(
        ns_range.is_some(),
        "expected a folding range for namespace 'geo', got: {:?}",
        ranges.iter().map(|r| (r.start_line, r.end_line, r.kind.clone())).collect::<Vec<_>>()
    );
}

/// The returned list must be sorted by start_line in ascending order.
#[test]
fn folding_ranges_sorted() {
    // A namespace containing a class containing a function — three nested ranges
    // at different start lines.
    let src = "namespace app {\nclass Widget {\npublic:\n    void render() {\n        int x = 0;\n    }\n};\n}";
    let path = write_temp("cb_folding_sorted.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let ranges = tu.folding_ranges();
    // Need at least two ranges to verify ordering.
    assert!(
        ranges.len() >= 2,
        "expected at least 2 folding ranges for nested namespace/class/function, got {}",
        ranges.len()
    );

    let lines: Vec<u32> = ranges.iter().map(|r| r.start_line).collect();
    let mut sorted = lines.clone();
    sorted.sort_unstable();
    assert_eq!(
        lines, sorted,
        "folding ranges are not sorted by start_line: {lines:?}"
    );
}

/// Multi-line comment blocks fold with kind "comment" (clangd folds the file
/// header and multi-line doc comments).  Regression for missing comment folding.
#[test]
fn folding_comment_block() {
    let src = "/* a multi-line\n   block comment\n   spanning lines */\nint x;\n";
    let path = write_temp("cb_folding_comment.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let ranges = folding::folding_ranges(&tu);
    let comment = ranges.iter().find(|r| r.kind == "comment" && r.start_line == 1);
    assert!(
        comment.is_some(),
        "expected a 'comment' fold for the 3-line block comment, got: {:?}",
        ranges.iter().map(|r| (r.start_line, r.end_line, r.kind.clone())).collect::<Vec<_>>()
    );
}

/// A braced loop body folds as its own "region", distinct from the enclosing
/// function body.  Regression for missing statement-level folding.
#[test]
fn folding_loop_body() {
    let src = "int f(int n) {\n    int t = 0;\n    for (int i = 0; i < n; ++i) {\n        t += i;\n        t += 1;\n    }\n    return t;\n}\n";
    let path = write_temp("cb_folding_loop.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let ranges = folding::folding_ranges(&tu);
    // The for-loop opens on line 3 and closes on line 6.
    let loop_fold = ranges.iter().find(|r| r.start_line == 3 && r.end_line == 6 && r.kind == "region");
    assert!(
        loop_fold.is_some(),
        "expected a 'region' fold for the for-loop body (3-6), got: {:?}",
        ranges.iter().map(|r| (r.start_line, r.end_line, r.kind.clone())).collect::<Vec<_>>()
    );
}
