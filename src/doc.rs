use std::ffi::CStr;
use crate::ffi;

/// A documented declaration extracted from the AST.
#[derive(Debug, Clone)]
pub struct DocItem {
    pub kind:         String,
    pub name:         String,
    pub usr:          String,
    pub brief:        String,
    pub full_comment: String,
    pub signature:    String,
    pub file:         String,
    pub line:         u32,
}

/// Iterator over [`DocItem`]s from a translation unit.
pub struct DocIter(pub(crate) *mut ffi::CB_DocIter);

impl Iterator for DocIter {
    type Item = DocItem;

    fn next(&mut self) -> Option<DocItem> {
        let mut raw = ffi::CB_DocItem {
            kind:         std::ptr::null(),
            name:         std::ptr::null(),
            usr:          std::ptr::null(),
            brief:        std::ptr::null(),
            full_comment: std::ptr::null(),
            signature:    std::ptr::null(),
            file:         std::ptr::null(),
            line:         0,
        };
        let ok = unsafe { ffi::cb_doc_iter_next(self.0, &mut raw) };
        if ok == 0 { return None; }

        unsafe fn s(p: *const std::ffi::c_char) -> String {
            if p.is_null() { String::new() }
            else { CStr::from_ptr(p).to_string_lossy().into_owned() }
        }
        Some(unsafe { DocItem {
            kind:         s(raw.kind),
            name:         s(raw.name),
            usr:          s(raw.usr),
            brief:        s(raw.brief),
            full_comment: s(raw.full_comment),
            signature:    s(raw.signature),
            file:         s(raw.file),
            line:         raw.line,
        }})
    }
}

impl Drop for DocIter {
    fn drop(&mut self) { unsafe { ffi::cb_doc_iter_destroy(self.0) } }
}
