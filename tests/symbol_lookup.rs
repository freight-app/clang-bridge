use clang_bridge::Index;
use std::io::Write;

#[test]
fn symbol_at_resolves_function_declaration() {
    let dir = std::env::temp_dir();
    let path = dir.join("cb_test_sym.hpp");
    {
        let mut f = std::fs::File::create(&path).unwrap();
        writeln!(f, "/// Multiplies two values.\nint multiply(int a, int b);").unwrap();
    }

    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), &["-std=c++17"]).unwrap();

    // Line 2, column 5 should hit `multiply`.
    let sym = tu.symbol_at(2, 5).expect("expected symbol at (2,5)");
    assert_eq!(sym.name(), "multiply");
    assert_eq!(sym.kind(), "function");
    assert!(!sym.signature().is_empty());
}
