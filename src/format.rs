use crate::ffi;
use std::ffi::{CStr, CString};

/// A single text replacement produced by clang-format.
#[derive(Debug, Clone)]
pub struct FormatEdit {
    /// Byte offset in the source buffer.
    pub offset: u32,
    /// Number of bytes to delete at `offset`.
    pub length: u32,
    /// Text to insert in place of the deleted range.
    pub replacement: String,
}

/// Owned list of format edits returned by [`format`].
pub struct FormatList(*mut ffi::CB_FormatList);

impl FormatList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_format_edit_count(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, i: usize) -> FormatEdit {
        let mut raw = ffi::CB_FormatEdit {
            offset: 0,
            length: 0,
            replacement: std::ptr::null(),
        };
        unsafe { ffi::cb_format_edit_get(self.0, i, &mut raw) };
        let replacement = if raw.replacement.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(raw.replacement) }
                .to_string_lossy()
                .into_owned()
        };
        FormatEdit { offset: raw.offset, length: raw.length, replacement }
    }

    pub fn iter(&self) -> impl Iterator<Item = FormatEdit> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for FormatList {
    fn drop(&mut self) {
        unsafe { ffi::cb_format_list_destroy(self.0) }
    }
}

/// Format `source` using the `.clang-format` file found by walking up from
/// `style_dir` (pass `None` to use the LLVM built-in style).
///
/// Returns a list of non-overlapping text replacements. Apply them in
/// **reverse-offset order** so earlier edits don't shift later offsets.
///
/// Maps to LSP `textDocument/formatting` and `textDocument/rangeFormatting`.
pub fn format(source: &str, style_dir: Option<&str>) -> FormatList {
    let style_ptr = match style_dir.and_then(|s| CString::new(s).ok()) {
        Some(cs) => cs.into_raw(),
        None => std::ptr::null_mut(),
    };
    let list = unsafe {
        ffi::cb_format(source.as_ptr() as *const _, source.len(), style_ptr)
    };
    // Reclaim the CString if we allocated one.
    if !style_ptr.is_null() {
        unsafe { let _ = CString::from_raw(style_ptr); }
    }
    FormatList(list)
}
