use crate::ffi;

/// Return an LSP-ready markdown string for the symbol at (line, col), or None.
pub fn hover_markdown(tu: &crate::TranslationUnit, line: u32, col: u32) -> Option<String> {
    let raw = unsafe { ffi::cb_hover_markdown(tu.0, line, col) };
    if raw.is_null() {
        return None;
    }
    let s = unsafe { std::ffi::CStr::from_ptr(raw).to_string_lossy().into_owned() };
    unsafe { ffi::cb_free_string(raw) };
    Some(s)
}

/// Reparse the TU with an optional in-memory replacement for its main file.
/// Returns true on success.
pub fn reparse(tu: &crate::TranslationUnit, content: Option<&str>) -> bool {
    tu.reparse(content)
}
