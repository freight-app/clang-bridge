use clang_bridge::Index;
use std::io::Write;

/// Write a temp header with doc comments and verify extraction.
#[test]
fn extracts_doc_items_from_header() {
    let dir = std::env::temp_dir();
    let path = dir.join("cb_test_doc.hpp");
    {
        let mut f = std::fs::File::create(&path).unwrap();
        writeln!(
            f,
            r#"
/// Adds two integers.
/// @param a first operand
/// @param b second operand
/// @return the sum
int add(int a, int b);

/// A simple point.
struct Point {{
    /// x coordinate
    float x;
    /// y coordinate
    float y;
}};
"#
        )
        .unwrap();
    }

    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), &["-std=c++17"])
        .expect("parse failed");

    let items: Vec<_> = tu.doc_items().collect();
    let names: Vec<&str> = items.iter().map(|i| i.name.as_str()).collect();

    assert!(names.contains(&"add"), "expected 'add', got {names:?}");
    assert!(names.contains(&"Point"), "expected 'Point', got {names:?}");

    let add = items.iter().find(|i| i.name == "add").unwrap();
    assert!(!add.brief.is_empty(), "brief should not be empty");
    assert!(!add.signature.is_empty(), "signature should not be empty");
}
