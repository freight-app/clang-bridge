//! End-to-end API verification against a real on-disk fixture.
//!
//! Unlike the per-feature tests (which build tiny throwaway snippets), this
//! suite parses `tests/fixtures/test.cpp` — a single substantial file covering
//! macros, namespaces, an inheritance hierarchy, an `enum class`, overloaded
//! and templated functions, cross-file calls, and rich doc comments — and
//! asserts that every public API returns the *exact* answer.
//!
//! Source positions are located by searching the fixture text for anchor
//! substrings (`at` / `line_of`), so the assertions stay correct if the file is
//! edited.  The semantic expectations (names, kinds, hierarchy edges, resolved
//! definition lines) are derived independently from reading the fixture.

use clang_bridge::{
    callhier, diag::Severity, goto, highlight, hover, inlay, refs, rename, semtok,
    semtok::tok_type, sighelp, typehier, Index, TranslationUnit,
};
use std::path::Path;

// ── Fixture loading + position helpers ───────────────────────────────────────

struct Fix {
    _idx: Index, // kept alive alongside the TU out of caution
    tu: TranslationUnit,
    src: String,
    hdr: String,
    cpp_path: String,
}

fn load() -> Fix {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let cpp = dir.join("test.cpp");
    let src = std::fs::read_to_string(&cpp).expect("read test.cpp");
    let hdr = std::fs::read_to_string(dir.join("shapes.h")).expect("read shapes.h");
    let idx = Index::new();
    let tu = idx
        .parse(cpp.to_str().unwrap(), dir.to_str().unwrap(), &["-std=c++17"])
        .expect("fixture test.cpp should parse cleanly");
    Fix {
        _idx: idx,
        tu,
        src,
        hdr,
        cpp_path: cpp.to_string_lossy().into_owned(),
    }
}

/// 1-based line number of the first line containing `anchor`.
fn line_of(src: &str, anchor: &str) -> u32 {
    for (i, line) in src.lines().enumerate() {
        if line.contains(anchor) {
            return i as u32 + 1;
        }
    }
    panic!("anchor not found in source: {anchor:?}");
}

/// 1-based (line, col) of `token`, searched on the first line containing
/// `anchor`.  Fixture is ASCII, so byte offset == column offset.
fn at(src: &str, anchor: &str, token: &str) -> (u32, u32) {
    for (i, line) in src.lines().enumerate() {
        if line.contains(anchor) {
            let off = line.find(token).unwrap_or_else(|| {
                panic!("token {token:?} not on line with anchor {anchor:?}: {line:?}")
            });
            return (i as u32 + 1, off as u32 + 1);
        }
    }
    panic!("anchor not found in source: {anchor:?}");
}

// ── symbol_at / goto ─────────────────────────────────────────────────────────

#[test]
fn symbol_at_function_definition() {
    let f = load();
    let (l, c) = at(&f.src, "int add(int a, int b) {", "add");
    let sym = f.tu.symbol_at(l, c).expect("symbol at add definition");
    assert_eq!(sym.name(), "add");
    assert_eq!(sym.kind(), "function");
}

#[test]
fn symbol_at_call_resolves_to_definition() {
    let f = load();
    let (l, c) = at(&f.src, "total = add(total, i);", "add");
    let sym = f.tu.symbol_at(l, c).expect("symbol at add call site");
    assert_eq!(sym.name(), "add");
    assert_eq!(sym.kind(), "function");
    let def_line = line_of(&f.src, "int add(int a, int b) {");
    assert_eq!(
        sym.def_line(),
        def_line,
        "call should resolve to the int-overload definition line"
    );
    assert!(
        sym.def_file().ends_with("test.cpp"),
        "def_file should be the fixture, got {:?}",
        sym.def_file()
    );
}

#[test]
fn goto_definition_crosses_into_header() {
    let f = load();
    // The call `square(answer)` in main() is defined in shapes.h.
    let (l, c) = at(&f.src, "auto doubled = square(answer);", "square");
    let loc = goto::goto_definition(&f.tu, l, c).expect("goto square definition");
    assert!(
        loc.file.ends_with("shapes.h"),
        "expected definition in shapes.h, got {:?}",
        loc.file
    );
    let hdr_def_line = line_of(&f.hdr, "inline int square(int n)");
    assert_eq!(loc.line, hdr_def_line, "square def line in shapes.h");
}

// ── hover ────────────────────────────────────────────────────────────────────

#[test]
fn hover_full_shows_signature_and_param_docs() {
    let f = load();
    let (l, c) = at(&f.src, "int add(int a, int b) {", "add");
    let md = hover::hover_full(&f.tu, l, c).expect("hover for add");
    assert!(md.contains("int add(int a, int b)"), "signature missing:\n{md}");
    assert!(md.contains("first addend"), "param doc missing:\n{md}");
    assert!(md.contains("sum of a and b"), "returns doc missing:\n{md}");
}

#[test]
fn macro_hover_shows_value() {
    let f = load();
    let (l, c) = at(&f.src, "clamp(answer, 0, MAX_ITEMS)", "MAX_ITEMS");
    let md = hover::macro_hover(&f.tu, l, c).expect("macro hover for MAX_ITEMS");
    assert!(md.contains("128"), "expansion value missing:\n{md}");
}

// ── #include graph ───────────────────────────────────────────────────────────

#[test]
fn inclusions_record_header() {
    let f = load();
    let incls: Vec<_> = f.tu.inclusions().iter().collect();
    let shapes = incls
        .iter()
        .find(|i| i.included_file.ends_with("shapes.h"))
        .expect("an inclusion resolving to shapes.h");
    assert!(
        shapes.including_file.ends_with("test.cpp"),
        "including file should be test.cpp, got {:?}",
        shapes.including_file
    );
    let inc_line = line_of(&f.src, "#include \"shapes.h\"");
    assert_eq!(shapes.line, inc_line);

    // The path literal "shapes.h" is 10 columns wide including both quotes.
    let (_, quote_col) = at(&f.src, "#include \"shapes.h\"", "\"");
    assert_eq!(shapes.start_col, quote_col, "start at opening quote");
    assert_eq!(shapes.end_col, quote_col + 10, "end past closing quote");
}

// ── references / rename ──────────────────────────────────────────────────────

#[test]
fn references_finds_definition_and_two_calls() {
    let f = load();
    let (l, c) = at(&f.src, "int add(int a, int b) {", "add");
    let usr = f.tu.symbol_at(l, c).unwrap().usr().to_string();

    let refs = refs::references(&f.tu, &usr);
    // Definition + call in sum_to + call in main = 3.  The double overload has
    // a different USR and must not be counted.
    assert_eq!(refs.len(), 3, "expected exactly 3 occurrences of int add");
    let defs = refs.iter().filter(|r| r.is_definition).count();
    assert_eq!(defs, 1, "exactly one occurrence is the definition");
}

#[test]
fn rename_int_add_touches_every_site() {
    let f = load();
    let (l, c) = at(&f.src, "int add(int a, int b) {", "add");
    let usr = f.tu.symbol_at(l, c).unwrap().usr().to_string();

    let result = rename::rename(&f.tu, &usr, "plus");
    assert_eq!(result.edit_count(), 3, "rename should edit all 3 sites");
    for edit in result.edits() {
        assert_eq!(edit.new_name, "plus");
        assert_eq!(edit.old_name_len, 3, "len of \"add\"");
    }
    assert!(!result.has_conflict(), "\"plus\" is a fresh name");
}

// ── document symbols ─────────────────────────────────────────────────────────

#[test]
fn document_symbols_have_correct_kinds_and_nesting() {
    let f = load();
    let syms: Vec<_> = f
        .tu
        .document_symbols()
        .expect("document symbols")
        .iter()
        .collect();

    let find = |name: &str, kind: &str| -> Option<usize> {
        syms.iter().position(|s| s.name == name && s.kind == kind)
    };

    let point_idx = find("Point", "struct").expect("struct Point");
    let norm_idx = find("norm", "method").expect("method norm");
    assert_eq!(
        syms[norm_idx].parent, point_idx as i32,
        "norm should be nested under Point"
    );

    let channel_idx = find("Channel", "enum").expect("enum Channel");
    let red_idx = find("Red", "enumconst").expect("enumconst Red");
    assert_eq!(
        syms[red_idx].parent, channel_idx as i32,
        "Red should be nested under Channel"
    );

    assert!(find("main", "function").is_some(), "main present");
    assert!(find("add", "function").is_some(), "add present");
    // Selection range of Point points at the name token.
    let (pl, pc) = at(&f.src, "struct Point {", "Point");
    assert_eq!(syms[point_idx].sel_line, pl);
    assert_eq!(syms[point_idx].sel_col, pc);
}

// ── inlay hints ──────────────────────────────────────────────────────────────

#[test]
fn inlay_hints_param_names_and_deduced_types() {
    let f = load();
    let hints: Vec<_> = inlay::inlay_hints(&f.tu, 1, 10_000).iter().collect();

    let call_line = line_of(&f.src, "auto answer = add(40, 2);");

    // Parameter-name hint (kind 0) for the first argument of add(40, 2).
    let has_param = hints
        .iter()
        .any(|h| h.line == call_line && h.kind == 0 && h.label.contains('a'));
    assert!(has_param, "expected an `a:` param hint on the add(40,2) call; got {hints:?}");

    // Deduced-type hint (kind 1) for `auto answer` → int.
    let has_type = hints
        .iter()
        .any(|h| h.line == call_line && h.kind == 1 && h.label.contains("int"));
    assert!(has_type, "expected a `: int` type hint for answer; got {hints:?}");
}

/// A macro-expanded argument's param hint must anchor at the call-site spelling,
/// not inside the macro body.  `clamp(answer, 0, MAX_ITEMS)` passes `MAX_ITEMS`
/// as `hi`; clangd places `hi:` on the `MAX_ITEMS` token (the call line), not on
/// the `#define MAX_ITEMS 128` line.  Regression for the getSpellingLoc bug.
#[test]
fn inlay_hint_macro_arg_anchors_at_call_site() {
    let f = load();
    let hints: Vec<_> = inlay::inlay_hints(&f.tu, 1, 10_000).iter().collect();

    let call_line = line_of(&f.src, "int capped = clamp(answer, 0, MAX_ITEMS);");
    let define_line = line_of(&f.src, "#define MAX_ITEMS 128");

    let hi: Vec<_> = hints.iter().filter(|h| h.label == "hi:").collect();
    assert_eq!(hi.len(), 1, "exactly one `hi:` hint expected; got {hints:?}");
    assert_eq!(hi[0].line, call_line, "`hi:` must anchor on the clamp() call line");
    assert!(
        hi.iter().all(|h| h.line != define_line),
        "`hi:` must not land inside the macro definition (line {define_line})"
    );
}

/// Code completion (signature help) must not corrupt the TU.  An LSP server
/// reuses one TranslationUnit across many requests; running `signature_help`
/// (or completion) used to hand `CodeComplete` the unit's own SourceManager,
/// clobbering the cached AST so every later AST-visitor query returned nothing.
/// Regression: AST-backed queries must still work after signature help.
#[test]
fn signature_help_does_not_corrupt_translation_unit() {
    let f = load();

    let inlay_before = inlay::inlay_hints(&f.tu, 1, 10_000).len();
    let hl_before = highlight::highlight(&f.tu, 89, 9).len();
    assert!(inlay_before > 0 && hl_before > 0, "sanity: queries work before");

    let (sl, sc) = at(&f.src, "auto answer = add(40, 2);", "40");
    let sh = sighelp::signature_help(&f.tu, sl, sc).expect("signature help at add(40,2)");
    assert!(!sh.overloads.is_empty(), "expected overloads");

    let inlay_after = inlay::inlay_hints(&f.tu, 1, 10_000).len();
    let hl_after = highlight::highlight(&f.tu, 89, 9).len();
    assert_eq!(inlay_after, inlay_before, "inlay broke after signature_help");
    assert_eq!(hl_after, hl_before, "highlight broke after signature_help");
}

/// Semantic tokens must cover *type references*, not just declarations and
/// expression references: base-class specifiers, variable type annotations,
/// template-parameter uses, and constructor-initializer members.  clangd emits
/// all of these; the bridge used to emit none.  Regression for the TypeLoc gap.
#[test]
fn semantic_tokens_cover_type_references() {
    let f = load();
    let toks = semtok::semantic_tokens(&f.tu);
    let kind_at = |line: u32, col: u32| -> Option<u8> {
        toks.iter().find(|t| t.line == line && t.col == col).map(|t| t.token_type)
    };
    let check = |anchor: &str, token: &str, want: u8, what: &str| {
        let (l, c) = at(&f.src, anchor, token);
        assert_eq!(kind_at(l, c), Some(want), "{what} at {l}:{c}");
    };
    // base-class reference `Shape` in `struct Circle : Shape`
    check("struct Circle : Shape", "Shape", tok_type::TYPE, "base-class ref");
    // variable type `Circle` in `geo::Circle circle(2.0)`
    check(
        "geo::Circle circle",
        "geo",
        tok_type::NAMESPACE,
        "namespace qualifier",
    );
    check(
        "geo::Circle circle",
        "Circle",
        tok_type::TYPE,
        "variable type ref",
    );
    // deduced placeholders are type tokens in clangd's legend
    check(
        "auto answer = add",
        "auto",
        tok_type::TYPE,
        "deduced auto type",
    );
    check(
        "auto doubled = square",
        "auto",
        tok_type::TYPE,
        "second deduced auto type",
    );
    // template-parameter use `T` in the clamp signature
    check("T clamp(T value", "T", tok_type::TYPE, "template-param use");
    // constructor-initializer member `radius` in `: radius(r)`
    check("explicit Circle(double r) : radius(r)", "radius", tok_type::PROPERTY, "ctor-init member");
}

// ── document symbols: header isolation + range end ───────────────────────────

/// documentSymbol is per-document: a symbol defined in an included header
/// (`square` in shapes.h) must not appear in the outline.  Matches clangd.
#[test]
fn document_symbols_exclude_included_header() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();
    assert!(
        syms.iter().all(|s| s.name != "square"),
        "square is defined in shapes.h and must not appear in test.cpp's outline; got {:?}",
        syms.iter().map(|s| s.name.clone()).collect::<Vec<_>>()
    );
}

/// LSP ranges are half-open: a symbol's range end points one past the last
/// token.  For field `x` (declared `int x;`), the end column must be one past
/// the `x` name token, matching clangd's getLocForEndOfToken behaviour.
#[test]
fn document_symbols_range_end_is_end_of_token() {
    let f = load();
    let syms: Vec<_> = f.tu.document_symbols().expect("document symbols").iter().collect();
    let x = syms.iter().find(|s| s.name == "x" && s.kind == "field").expect("field x");
    let (name_line, name_col) = at(&f.src, "int x;", "x");
    assert_eq!(x.range_end_line, name_line, "x range end line");
    assert_eq!(
        x.range_end_col, name_col + 1,
        "x range end col should point one past the `x` token (col {name_col})"
    );
}

// ── semantic tokens ──────────────────────────────────────────────────────────

#[test]
fn semantic_tokens_classify_each_kind() {
    let f = load();
    let toks = semtok::semantic_tokens(&f.tu);
    let kind_at = |line: u32, col: u32| -> Option<u8> {
        toks.iter().find(|t| t.line == line && t.col == col).map(|t| t.token_type)
    };

    // semanticTokens is per-document: no token may come from an included header.
    // shapes.h's symbols sit at its own low line numbers (≤10); the first token
    // in test.cpp is the `MAX_ITEMS` macro.  Regression for the header leak.
    let first_real = line_of(&f.src, "#define MAX_ITEMS 128");
    assert!(
        toks.iter().all(|t| t.line >= first_real),
        "semantic tokens leaked from an included header (token before line {first_real}): {:?}",
        toks.iter().filter(|t| t.line < first_real).map(|t| (t.line, t.col)).collect::<Vec<_>>()
    );

    let check = |anchor: &str, token: &str, want: u8, what: &str| {
        let (l, c) = at(&f.src, anchor, token);
        assert_eq!(kind_at(l, c), Some(want), "{what} token type at {l}:{c}");
    };

    check("enum class Channel {", "Channel", tok_type::TYPE, "enum type");
    check("    Red,", "Red", tok_type::ENUM_MEMBER, "enum member");
    check("int add(int a, int b) {", "add", tok_type::FUNCTION, "function");
    check("int norm() const {", "norm", tok_type::METHOD, "method");
    check("clamp(answer, 0, MAX_ITEMS)", "MAX_ITEMS", tok_type::MACRO, "macro");
}

// ── folding ──────────────────────────────────────────────────────────────────

#[test]
fn folding_ranges_cover_namespace_and_main() {
    let f = load();
    let ranges: Vec<_> = f.tu.folding_ranges().iter().collect();

    let ns_start = line_of(&f.src, "namespace geo {");
    let ns_end = line_of(&f.src, "}  // namespace geo");
    assert!(
        ranges
            .iter()
            .any(|r| r.start_line == ns_start && r.end_line == ns_end && r.kind == "region"),
        "expected a region folding the geo namespace ({ns_start}..{ns_end}); got {ranges:?}"
    );

    let main_start = line_of(&f.src, "int main() {");
    assert!(
        ranges.iter().any(|r| r.start_line == main_start && r.end_line > main_start),
        "expected a folding range for main()'s body; got {ranges:?}"
    );
}

// ── call hierarchy ───────────────────────────────────────────────────────────

#[test]
fn call_hierarchy_incoming_and_outgoing() {
    let f = load();

    // Outgoing from sum_to → add.
    let (sl, sc) = at(&f.src, "int sum_to(int n)", "sum_to");
    let sum_to = callhier::call_hierarchy_prepare(&f.tu, sl, sc).expect("prepare sum_to");
    assert_eq!(sum_to.name(), "sum_to");
    let outgoing: Vec<_> = callhier::outgoing_calls(&f.tu, &sum_to.usr())
        .iter()
        .map(|e| e.name)
        .collect();
    assert!(outgoing.contains(&"add".to_string()), "sum_to calls add; got {outgoing:?}");

    // Incoming to int add ← sum_to and main.
    let (al, ac) = at(&f.src, "int add(int a, int b) {", "add");
    let add = callhier::call_hierarchy_prepare(&f.tu, al, ac).expect("prepare add");
    let incoming: Vec<_> = callhier::incoming_calls(&f.tu, &add.usr())
        .iter()
        .map(|e| e.name)
        .collect();
    assert!(incoming.contains(&"sum_to".to_string()), "add called by sum_to; got {incoming:?}");
    assert!(incoming.contains(&"main".to_string()), "add called by main; got {incoming:?}");
}

// ── type hierarchy ───────────────────────────────────────────────────────────

#[test]
fn type_hierarchy_super_and_subtypes() {
    let f = load();

    let (cl, cc) = at(&f.src, "struct Circle : Shape", "Circle");
    let circle = typehier::type_hierarchy_prepare(&f.tu, cl, cc).expect("prepare Circle");
    assert_eq!(circle.name(), "Circle");
    let supers: Vec<_> = typehier::supertypes(&f.tu, &circle.usr())
        .iter()
        .map(|e| e.name)
        .collect();
    assert!(supers.contains(&"Shape".to_string()), "Circle : Shape; got {supers:?}");

    let (hl, hc) = at(&f.src, "struct Shape {", "Shape");
    let shape = typehier::type_hierarchy_prepare(&f.tu, hl, hc).expect("prepare Shape");
    let subs: Vec<_> = typehier::subtypes(&f.tu, &shape.usr())
        .iter()
        .map(|e| e.name)
        .collect();
    assert!(subs.contains(&"Circle".to_string()), "Circle derives Shape; got {subs:?}");
    assert!(subs.contains(&"Rectangle".to_string()), "Rectangle derives Shape; got {subs:?}");
}

// ── macro expansion ──────────────────────────────────────────────────────────

#[test]
fn expand_macro_function_like() {
    let f = load();
    let (l, c) = at(&f.src, "int sq = SQUARE(s);", "SQUARE");
    let exp = f.tu.expand_macro(l, c).expect("expand SQUARE");
    assert!(exp.contains('*'), "SQUARE expands to a multiplication:\n{exp}");
}

// ── diagnostics ──────────────────────────────────────────────────────────────

#[test]
fn diagnostics_are_clean() {
    let f = load();
    let errors: Vec<_> = f
        .tu
        .diagnostics()
        .filter(|d| d.severity == Severity::Error || d.severity == Severity::Fatal)
        .collect();
    assert!(errors.is_empty(), "fixture must compile cleanly, got: {errors:?}");
}

// ── doc extraction ───────────────────────────────────────────────────────────

#[test]
fn doc_items_capture_briefs() {
    let f = load();
    let items: Vec<_> = f.tu.doc_items().collect();

    let add = items
        .iter()
        .find(|d| d.name == "add" && d.brief.contains("Add two integers"))
        .expect("doc item for int add");
    assert_eq!(add.kind, "function");
    assert_eq!(add.line, line_of(&f.src, "int add(int a, int b) {"));

    assert!(
        items
            .iter()
            .any(|d| d.name.ends_with("Point") && d.brief.contains("2D point")),
        "doc item for Point missing"
    );
}

// ── AST dump ─────────────────────────────────────────────────────────────────

#[test]
fn ast_dump_is_json_naming_the_function() {
    let f = load();
    let def_line = line_of(&f.src, "int add(int a, int b) {");
    let json = f.tu.ast_dump(def_line, def_line);
    assert!(json.starts_with('['), "ast_dump should be a JSON array:\n{json}");
    assert!(json.ends_with(']'));
    assert!(json.contains("add"), "dump should name `add`:\n{json}");
}

// ── signature help ───────────────────────────────────────────────────────────

#[test]
fn signature_help_at_call_site() {
    let f = load();
    // Cursor on the first argument `40` of add(40, 2).
    let (l, c) = at(&f.src, "auto answer = add(40, 2);", "40");
    let sh = sighelp::signature_help(&f.tu, l, c).expect("signature help for add(...)");
    assert_eq!(sh.active_param, 0, "cursor is on the first argument");
    assert!(
        sh.overloads.iter().any(|o| o.label.contains("add")),
        "expected an `add` overload candidate; got {:?}",
        sh.overloads
    );
}

// ── code actions (smoke: must not crash on the fixture) ──────────────────────

#[test]
fn code_actions_do_not_crash() {
    let f = load();
    let (l, c) = at(&f.src, "int add(int a, int b) {", "add");
    // Clean file: there may be zero fix-its, but the call must be safe and the
    // returned list iterable.
    let actions = f.tu.code_actions(l, c);
    for a in actions.iter() {
        let _ = (a.title.len(), a.replacement.len(), a.file.len());
    }
}
