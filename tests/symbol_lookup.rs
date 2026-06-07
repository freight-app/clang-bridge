use clang_bridge::Index;
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    writeln!(f, "{content}").unwrap();
    path
}

#[test]
fn symbol_at_resolves_function_declaration() {
    let dir = std::env::temp_dir();
    let path = dir.join("cb_test_sym.hpp");
    {
        let mut f = std::fs::File::create(&path).unwrap();
        writeln!(f, "/// Multiplies two values.\nint multiply(int a, int b);").unwrap();
    }

    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // Line 2, column 5 should hit `multiply`.
    let sym = tu.symbol_at(2, 5).expect("expected symbol at (2,5)");
    assert_eq!(sym.name(), "multiply");
    assert_eq!(sym.kind(), "function");
    assert!(!sym.signature().is_empty());
}

#[test]
fn symbol_at_resolves_variable_reference() {
    // Cursor on the use of `x` in the return statement, not its declaration.
    let src = "int foo(int x) { return x; }";
    // x declaration: col 12 ("int foo(int x)")
    // x use:         col 25 ("return x")
    let path = write_temp("cb_sym_varref.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(1, 25).expect("expected symbol at use site");
    assert_eq!(sym.name(), "x", "expected param 'x', got '{}'", sym.name());
}

#[test]
fn symbol_at_resolves_member_access() {
    // Cursor on `.y` in `p.y`.
    let src = "struct Point { int x; int y; };\nint get_y(Point p) { return p.y; }";
    // Line 2: "int get_y(Point p) { return p.y; }"
    //                                           ^ col 31 = 'y'
    let path = write_temp("cb_sym_member.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(2, 31).expect("expected symbol at member access");
    assert_eq!(sym.name(), "Point::y", "expected 'Point::y', got '{}'", sym.name());
}

#[test]
fn symbol_at_resolves_type_reference() {
    // Cursor on the type name `Point` in a variable declaration, not in the struct def.
    let src = "struct Point { int x; int y; };\nPoint make() { return {0,0}; }";
    // Line 2: "Point make() { return {0,0}; }"
    //          ^ col 1 = 'P' of 'Point'
    let path = write_temp("cb_sym_typeref.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(2, 1).expect("expected symbol at type reference");
    assert_eq!(sym.name(), "Point", "expected 'Point', got '{}'", sym.name());
}

#[test]
fn goto_resolves_to_definition_from_call_site() {
    // `square` is declared on line 1, defined on line 2.
    // Cursor is on the *call site* on line 3 — goto should still reach line 2.
    let src = "int square(int x);\nint square(int x) { return x * x; }\nint main() { return square(3); }";
    // Line 3: "int main() { return square(3); }"
    //                              ^ col 21 = 's' of 'square'
    let path = write_temp("cb_goto_callsite.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let loc = clang_bridge::goto::goto_definition(&tu, 3, 21)
        .expect("expected definition from call site");
    assert_eq!(loc.line, 2, "expected definition on line 2, got {}", loc.line);
}

#[test]
fn symbol_at_constructor_call_site() {
    // Cursor on `MyClass` in `MyClass(1)` temporary object expression.
    let src = "struct MyClass { MyClass(int x) {} };\nvoid f() { MyClass(1); }";
    // Line 2: "void f() { MyClass(1); }"
    //                     ^ col 12 = 'M' of 'MyClass'
    let path = write_temp("cb_sym_ctor_call.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    let sym = tu.symbol_at(2, 12).expect("expected symbol at constructor call");
    // Should resolve to the constructor (name = "MyClass::MyClass" or just "MyClass")
    assert!(
        sym.name().contains("MyClass"),
        "expected constructor symbol containing 'MyClass', got '{}'",
        sym.name()
    );
}
