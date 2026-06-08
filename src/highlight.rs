use crate::ffi;

/// A single highlight occurrence of a symbol.
#[derive(Debug, Clone)]
pub struct HighlightEntry {
    /// 1-based line.
    pub line: u32,
    /// 1-based column start.
    pub col: u32,
    /// 1-based column end (exclusive).
    pub end_col: u32,
    /// 1=text, 2=read, 3=write (definition site).
    pub kind: u8,
}

/// Owned list of document highlights.
pub struct HighlightList(*mut ffi::CB_HighlightList);

impl HighlightList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_highlight_count(self.0) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn get(&self, i: usize) -> HighlightEntry {
        let mut raw = ffi::CB_HighlightEntry { line: 0, col: 0, end_line: 0, end_col: 0, kind: 0 };
        unsafe { ffi::cb_highlight_get(self.0, i, &mut raw) };
        HighlightEntry { line: raw.line, col: raw.col, end_col: raw.end_col, kind: raw.kind }
    }
    pub fn iter(&self) -> impl Iterator<Item = HighlightEntry> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for HighlightList {
    fn drop(&mut self) {
        unsafe { ffi::cb_highlight_list_destroy(self.0) }
    }
}

/// Find all occurrences of the symbol at `(line, col)` within the main file.
/// Maps to LSP `textDocument/documentHighlight`.
pub fn highlight(tu: &crate::TranslationUnit, line: u32, col: u32) -> HighlightList {
    HighlightList(unsafe { ffi::cb_highlight(tu.0, line, col) })
}
