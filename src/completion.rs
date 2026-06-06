use crate::ffi;
use std::ffi::{CStr, CString};

pub struct CompletionItem {
    pub label: String,
    pub kind: u8,
    pub detail: Option<String>,
    pub documentation: Option<String>,
}

pub struct CompletionIter(*mut ffi::CB_CompletionIter);

impl CompletionIter {
    fn current_item(&self) -> Option<CompletionItem> {
        let mut out = ffi::CB_CompletionItem {
            label: std::ptr::null(),
            kind: 0,
            detail: std::ptr::null(),
            documentation: std::ptr::null(),
        };
        let ok = unsafe { ffi::cb_completion_next(self.0, &mut out) };
        if ok == 0 {
            return None;
        }
        let label = unsafe { cstr_opt(out.label).unwrap_or_default() };
        let detail = unsafe { cstr_opt(out.detail) };
        let documentation = unsafe { cstr_opt(out.documentation) };
        Some(CompletionItem {
            label,
            kind: out.kind,
            detail,
            documentation,
        })
    }
}

impl Iterator for CompletionIter {
    type Item = CompletionItem;
    fn next(&mut self) -> Option<Self::Item> {
        self.current_item()
    }
}

impl Drop for CompletionIter {
    fn drop(&mut self) {
        unsafe { ffi::cb_completion_iter_destroy(self.0) }
    }
}

unsafe impl Send for CompletionIter {}

unsafe fn cstr_opt(p: *const std::ffi::c_char) -> Option<String> {
    if p.is_null() {
        None
    } else {
        Some(CStr::from_ptr(p).to_string_lossy().into_owned())
    }
}

/// Run code completion at (line, col).  Pass `unsaved` to supply in-memory
/// file content (e.g. the current editor buffer).
pub fn complete(
    tu: &crate::TranslationUnit,
    line: u32,
    col: u32,
    unsaved: Option<&str>,
) -> CompletionIter {
    // Keep _cs alive across the call so buf_ptr stays valid.
    let _cs: Option<CString>;
    let (buf_ptr, buf_len) = match unsaved {
        Some(s) => {
            let cs = CString::new(s).unwrap_or_default();
            let ptr = cs.as_ptr();
            let len = s.len();
            _cs = Some(cs);
            (ptr, len)
        }
        None => {
            _cs = None;
            (std::ptr::null(), 0)
        }
    };
    let raw = unsafe { ffi::cb_complete(tu.0, line, col, buf_ptr, buf_len) };
    CompletionIter(raw)
}
