use crate::ffi;
use std::ffi::CStr;

/// A code folding region.
#[derive(Debug, Clone)]
pub struct FoldingRange {
    /// 1-based start line.
    pub start_line: u32,
    /// 1-based end line (inclusive).
    pub end_line: u32,
    /// `"region"` or `"comment"`.
    pub kind: String,
}

/// Owned list of folding ranges.
pub struct FoldingRangeList(*mut ffi::CB_FoldingRangeList);

impl FoldingRangeList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_folding_range_count(self.0) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn get(&self, i: usize) -> FoldingRange {
        let mut raw = ffi::CB_FoldingRange { start_line: 0, end_line: 0, kind: std::ptr::null() };
        unsafe { ffi::cb_folding_range_get(self.0, i, &mut raw) };
        let kind = if raw.kind.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(raw.kind) }.to_string_lossy().into_owned()
        };
        FoldingRange { start_line: raw.start_line, end_line: raw.end_line, kind }
    }
    pub fn iter(&self) -> impl Iterator<Item = FoldingRange> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for FoldingRangeList {
    fn drop(&mut self) {
        unsafe { ffi::cb_folding_range_list_destroy(self.0) }
    }
}

/// Emit folding ranges for functions, classes, enums, and namespaces.
/// Maps to LSP `textDocument/foldingRange`.
pub fn folding_ranges(tu: &crate::TranslationUnit) -> FoldingRangeList {
    FoldingRangeList(unsafe { ffi::cb_folding_ranges(tu.0) })
}
