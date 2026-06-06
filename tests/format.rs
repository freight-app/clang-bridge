//! Tests for cb_format (clang-format integration).

use clang_bridge::format;

#[test]
fn format_produces_edits_for_unformatted_code() {
    // Deliberately badly formatted: extra spaces around operators.
    let src = "int add(int a,int b){return a+b;}";
    let edits = format::format(src, None);

    // clang-format should produce at least one replacement.
    assert!(!edits.is_empty(), "expected format edits for unformatted code");
}

#[test]
fn format_no_edits_for_already_formatted_code() {
    // LLVM-style canonical formatting.
    let src = "int add(int a, int b) { return a + b; }\n";
    let edits = format::format(src, None);

    // Should produce no replacements (or only whitespace-neutral ones).
    // We check that applying all replacements gives the same string.
    let mut result = src.to_string();
    let mut edit_vec: Vec<_> = edits.iter().collect();
    // Apply in reverse order.
    edit_vec.sort_by(|a, b| b.offset.cmp(&a.offset));
    for e in edit_vec {
        let start = e.offset as usize;
        let end = start + e.length as usize;
        result.replace_range(start..end, &e.replacement);
    }
    assert_eq!(result, src, "formatted output should match input for already-formatted code");
}

#[test]
fn format_edits_apply_correctly() {
    let src = "int f(int a,int b){return a+b;}";
    let edits = format::format(src, None);

    let mut result = src.to_string();
    // Sort by offset descending so edits don't shift each other.
    let mut edit_vec: Vec<_> = edits.iter().collect();
    edit_vec.sort_by(|a, b| b.offset.cmp(&a.offset));
    for e in edit_vec {
        let start = e.offset as usize;
        let end = start + e.length as usize;
        result.replace_range(start..end, &e.replacement);
    }
    // The result should contain spaces after commas (LLVM style).
    assert!(result.contains(", "), "expected spaces after commas after formatting: {result}");
}
