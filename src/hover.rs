use crate::ffi;

/// Return the raw doc comment text for the symbol at (1-based) `line`/`col`.
///
/// Comment markers (`///`, `/** */`) are stripped; the text is otherwise
/// returned as-written.  Returns `None` when no comment is attached or no
/// symbol is found at the cursor.
pub fn raw_comment_at(tu: &crate::TranslationUnit, line: u32, col: u32) -> Option<String> {
    let raw = unsafe { ffi::cb_raw_comment_at(tu.0, line, col) };
    if raw.is_null() { return None; }
    let s = unsafe { std::ffi::CStr::from_ptr(raw).to_string_lossy().into_owned() };
    unsafe { ffi::cb_free_string(raw) };
    Some(s)
}

/// Brief hover: signature + first doc comment line.  Returns `None` when
/// nothing is found at the cursor position.
pub fn hover_markdown(tu: &crate::TranslationUnit, line: u32, col: u32) -> Option<String> {
    let raw = unsafe { ffi::cb_hover_markdown(tu.0, line, col) };
    if raw.is_null() { return None; }
    let s = unsafe { std::ffi::CStr::from_ptr(raw).to_string_lossy().into_owned() };
    unsafe { ffi::cb_free_string(raw) };
    Some(s)
}

/// Full hover: signature + structured doc comment (@param / @returns / @note
/// rendered as Markdown) + definition-location footer.  Falls back to the raw
/// comment text when no structured AST comment exists.
///
/// Prefer this over `hover_markdown` for `textDocument/hover` responses.
pub fn hover_full(tu: &crate::TranslationUnit, line: u32, col: u32) -> Option<String> {
    let raw = unsafe { ffi::cb_hover_full(tu.0, line, col) };
    if raw.is_null() { return None; }
    let s = unsafe { std::ffi::CStr::from_ptr(raw).to_string_lossy().into_owned() };
    unsafe { ffi::cb_free_string(raw) };
    Some(s)
}

/// Return the fully-qualified type string for the variable/field/parameter at
/// (1-based) `line`/`col`, or `None` when not applicable.
///
/// Useful for enriching hover text when no doc comment is present.
pub fn type_at(tu: &crate::TranslationUnit, line: u32, col: u32) -> Option<String> {
    let raw = unsafe { ffi::cb_type_at(tu.0, line, col) };
    if raw.is_null() { return None; }
    let s = unsafe { std::ffi::CStr::from_ptr(raw).to_string_lossy().into_owned() };
    unsafe { ffi::cb_free_string(raw) };
    Some(s)
}

/// Return a Markdown hover block for the macro at (1-based) `line`/`col`.
///
/// Shows the `#define` spelling, parameter list (if function-like), expansion
/// tokens, and a definition-location footer.  Returns `None` when the cursor
/// is not on a macro reference.
pub fn macro_hover(tu: &crate::TranslationUnit, line: u32, col: u32) -> Option<String> {
    let raw = unsafe { ffi::cb_macro_at(tu.0, line, col) };
    if raw.is_null() { return None; }
    let s = unsafe { std::ffi::CStr::from_ptr(raw).to_string_lossy().into_owned() };
    unsafe { ffi::cb_free_string(raw) };
    Some(s)
}

/// Reparse the TU with an optional in-memory replacement for its main file.
/// Returns true on success.
pub fn reparse(tu: &crate::TranslationUnit, content: Option<&str>) -> bool {
    tu.reparse(content)
}
