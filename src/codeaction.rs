use crate::ffi;
use std::ffi::CStr;

/// A fix-it code action derived from a compiler diagnostic.
#[derive(Debug, Clone)]
pub struct CodeAction {
    /// Diagnostic message (used as the action title).
    pub title: String,
    /// File to edit.
    pub file: String,
    /// 1-based start line.
    pub line: u32,
    /// 1-based start column.
    pub col: u32,
    /// 1-based end line (exclusive).
    pub end_line: u32,
    /// 1-based end column (exclusive).
    pub end_col: u32,
    /// Replacement text (empty = pure deletion).
    pub replacement: String,
}

/// Owned list of code actions.
pub struct CodeActionList(*mut ffi::CB_CodeActionList);

impl CodeActionList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_code_action_count(self.0) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn get(&self, i: usize) -> CodeAction {
        let mut raw = ffi::CB_CodeAction {
            title: std::ptr::null(), file: std::ptr::null(),
            line: 0, col: 0, end_line: 0, end_col: 0,
            replacement: std::ptr::null(),
        };
        unsafe { ffi::cb_code_action_get(self.0, i, &mut raw) };
        let s = |p: *const std::ffi::c_char| -> String {
            if p.is_null() { String::new() }
            else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };
        CodeAction {
            title: s(raw.title), file: s(raw.file),
            line: raw.line, col: raw.col,
            end_line: raw.end_line, end_col: raw.end_col,
            replacement: s(raw.replacement),
        }
    }
    pub fn iter(&self) -> impl Iterator<Item = CodeAction> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for CodeActionList {
    fn drop(&mut self) {
        unsafe { ffi::cb_code_action_list_destroy(self.0) }
    }
}

/// Return fix-it code actions applicable near `(line, col)` (±3 lines).
/// Maps to LSP `textDocument/codeAction`.
pub fn code_actions(tu: &crate::TranslationUnit, line: u32, col: u32) -> CodeActionList {
    CodeActionList(unsafe { ffi::cb_code_actions(tu.0, line, col) })
}
