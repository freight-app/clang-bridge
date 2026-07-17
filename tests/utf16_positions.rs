use clang_bridge::{callhier, goto, inlay, refs, rename, typehier, workspace, Index};
use std::sync::{Mutex, OnceLock};

fn parse_lock() -> std::sync::MutexGuard<'static, ()> {
    // ClangTool temporarily changes the process CWD while parsing. Keep these
    // parallel test cases from racing a TempDir teardown against that restore.
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(())).lock().unwrap()
}

fn utf16_col(line: &str, needle: &str) -> u32 {
    let byte = line.find(needle).expect("needle in source line");
    line[..byte].encode_utf16().count() as u32 + 1
}

fn parse_source(name: &str, source: &str) -> (Index, clang_bridge::TranslationUnit) {
    let _guard = parse_lock();
    let path = std::env::temp_dir().join(name);
    std::fs::write(&path, source).expect("write UTF-16 fixture");
    let index = Index::new();
    let tu = index
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap_or_else(|| panic!("parse fixture: {:?}", index.last_error()));
    (index, tu)
}

#[test]
fn navigation_and_symbol_result_columns_are_utf16() {
    let first = "constexpr const char *marker = u8\"😀\"; int value = 1;";
    let second = "int use() { (void)u8\"😀\"; return value; }";
    let source = format!("{first}\n{second}\n");
    let (index, tu) = parse_source("cb_utf16_results.cpp", &source);
    let def_col = utf16_col(first, "value");
    let use_col = utf16_col(second, "value");

    let symbol = tu
        .symbol_at(2, use_col)
        .expect("symbol lookup at UTF-16 use position");
    let usr = symbol.usr().to_string();
    drop(symbol);

    let definition = goto::goto_definition(&tu, 2, use_col).expect("definition");
    assert_eq!((definition.line, definition.col), (1, def_col));
    assert_eq!(
        (definition.end_line, definition.end_col),
        (1, def_col + "value".encode_utf16().count() as u32)
    );

    let references: Vec<_> = refs::references(&tu, &usr).iter().collect();
    assert!(references
        .iter()
        .any(|entry| entry.line == 1 && entry.col == def_col));
    assert!(references
        .iter()
        .any(|entry| entry.line == 2 && entry.col == use_col));

    let edits: Vec<_> = rename::rename(&tu, &usr, "renamed").edits().collect();
    assert!(edits
        .iter()
        .any(|entry| entry.line == 1 && entry.col == def_col));
    assert!(edits
        .iter()
        .any(|entry| entry.line == 2 && entry.col == use_col));

    let highlights: Vec<_> = tu.highlight(2, use_col).iter().collect();
    assert!(highlights
        .iter()
        .any(|entry| entry.line == 1 && entry.col == def_col && entry.end_col == def_col + 5));
    assert!(highlights
        .iter()
        .any(|entry| entry.line == 2 && entry.col == use_col && entry.end_col == use_col + 5));

    let symbols = tu.document_symbols().expect("document symbols");
    let value = symbols
        .iter()
        .find(|entry| entry.name == "value")
        .expect("value document symbol");
    assert_eq!((value.sel_line, value.sel_col), (1, def_col));

    workspace::workspace_index_add(&index, &tu);
    let workspace_value = workspace::workspace_symbols(&index, "value")
        .iter()
        .find(|entry| entry.name == "value")
        .expect("value workspace symbol");
    assert_eq!((workspace_value.line, workspace_value.col), (1, def_col));

    let ast: serde_json::Value = serde_json::from_str(&tu.ast_dump(1, 2)).expect("AST JSON");
    let value_dump = ast
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["name"] == "value")
        .expect("value AST entry");
    assert_eq!(value_dump["col"], utf16_col(first, "int value"));
}

#[test]
fn hierarchy_and_inlay_result_columns_are_utf16() {
    let base_line = "constexpr const char *base_marker = u8\"😀\"; struct Base {};";
    let child_line = "constexpr const char *child_marker = u8\"😀\"; struct Child : Base {};";
    let callee_line =
        "constexpr const char *call_marker = u8\"😀\"; int callee(int amount) { return amount; }";
    let caller_line = "int caller() { (void)u8\"😀\"; return callee(1); }";
    let source = format!("{base_line}\n{child_line}\n{callee_line}\n{caller_line}\n");
    let (_, tu) = parse_source("cb_utf16_hierarchy.cpp", &source);

    let child_use_col = utf16_col(child_line, "Child");
    let child = typehier::type_hierarchy_prepare(&tu, 2, child_use_col).expect("Child hierarchy");
    assert_eq!(child.col(), child_use_col);
    let child_usr = child.usr();

    let base = typehier::supertypes(&tu, &child_usr)
        .iter()
        .find(|entry| entry.name == "Base")
        .expect("Base supertype");
    assert_eq!((base.line, base.col), (1, utf16_col(base_line, "Base")));

    let base_usr = tu
        .symbol_at(1, utf16_col(base_line, "Base"))
        .expect("Base symbol")
        .usr()
        .to_string();
    let child_subtype = typehier::subtypes(&tu, &base_usr)
        .iter()
        .find(|entry| entry.name == "Child")
        .expect("Child subtype");
    assert_eq!((child_subtype.line, child_subtype.col), (2, child_use_col));

    let call_col = utf16_col(caller_line, "callee");
    let callee = callhier::call_hierarchy_prepare(&tu, 4, call_col).expect("callee hierarchy");
    assert_eq!(
        (callee.line(), callee.col()),
        (3, utf16_col(callee_line, "callee"))
    );
    let incoming = callhier::incoming_calls(&tu, &callee.usr())
        .iter()
        .find(|edge| edge.name == "caller")
        .expect("incoming caller edge");
    assert_eq!((incoming.call_line, incoming.call_col), (4, call_col));

    let amount_hint = inlay::inlay_hints(&tu, 4, 4)
        .iter()
        .find(|hint| hint.label == "amount:")
        .expect("amount parameter hint");
    assert_eq!(
        (amount_hint.line, amount_hint.col),
        (4, utf16_col(caller_line, "1)"))
    );
}

#[test]
fn diagnostics_and_fixit_columns_are_utf16() {
    let error_line = "void broken() { (void)u8\"😀\"; int value = ; }";
    let (_, error_tu) = parse_source("cb_utf16_diag.cpp", error_line);
    let diagnostic = error_tu
        .diagnostics()
        .find(|diag| diag.message.contains("expected expression"))
        .expect("expected-expression diagnostic");
    assert_eq!(
        (diagnostic.line, diagnostic.col),
        (1, utf16_col(error_line, "; }"))
    );

    let fix_line = "void needs_semicolon() { (void)u8\"😀\"; int value = 1 }";
    let (_, fix_tu) = parse_source("cb_utf16_fixit.cpp", fix_line);
    let action = fix_tu
        .code_actions(1, 1)
        .iter()
        .find(|action| action.replacement == ";")
        .expect("semicolon fix-it action");
    let expected = utf16_col(fix_line, " }");
    assert_eq!((action.line, action.col), (1, expected));
    assert_eq!((action.end_line, action.end_col), (1, expected));
}

#[test]
fn inclusion_ranges_count_utf16_units_in_header_names() {
    let _guard = parse_lock();
    let dir = tempfile::tempdir().expect("inclusion fixture");
    let header_name = "café😀.h";
    std::fs::write(dir.path().join(header_name), "#pragma once\n").expect("write header");
    let source = format!("#include \"{header_name}\"\nint value;\n");
    let source_path = dir.path().join("main.cpp");
    std::fs::write(&source_path, &source).expect("write source");

    let index = Index::new();
    let tu = index
        .parse(
            source_path.to_str().unwrap(),
            dir.path().to_str().unwrap(),
            &["-std=c++17", "-I."],
        )
        .expect("parse inclusion fixture");
    let inclusion = tu.inclusions().iter().next().expect("include entry");
    let line = source.lines().next().unwrap();
    assert_eq!(inclusion.start_col, utf16_col(line, "\""));
    assert_eq!(
        inclusion.end_col,
        line.encode_utf16().count() as u32 + 1,
        "end column is one past the closing quote"
    );
}
