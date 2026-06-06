mod ffi;

pub mod doc;
pub mod diag;

use std::ffi::CString;

/// Handle to a clang compilation index. Cheap to create; reuse across parses.
pub struct Index(*mut ffi::CB_Index);

impl Index {
    pub fn new() -> Self {
        Index(unsafe { ffi::cb_index_create() })
    }

    /// Parse `source_file` with the given compile `args` (e.g. `-std=c++17 -I/usr/include`).
    pub fn parse(&self, source_file: &str, args: &[&str]) -> Option<TranslationUnit> {
        let src = CString::new(source_file).ok()?;
        let cargs: Vec<CString> = args.iter().filter_map(|a| CString::new(*a).ok()).collect();
        let ptrs: Vec<*const std::ffi::c_char> = cargs.iter().map(|s| s.as_ptr()).collect();
        let tu = unsafe {
            ffi::cb_parse(self.0, src.as_ptr(), ptrs.as_ptr(), ptrs.len())
        };
        if tu.is_null() { None } else { Some(TranslationUnit(tu)) }
    }
}

impl Default for Index {
    fn default() -> Self { Self::new() }
}

impl Drop for Index {
    fn drop(&mut self) { unsafe { ffi::cb_index_destroy(self.0) } }
}

// SAFETY: CB_Index itself has no thread-local state.
unsafe impl Send for Index {}

/// A parsed translation unit. Owns the AST.
pub struct TranslationUnit(*mut ffi::CB_TransUnit);

impl TranslationUnit {
    /// Extract all documented declarations from this TU.
    pub fn doc_items(&self) -> doc::DocIter {
        let it = unsafe { ffi::cb_doc_extract(self.0) };
        doc::DocIter(it)
    }

    /// Iterate over compiler diagnostics stored in this TU.
    pub fn diagnostics(&self) -> diag::DiagIter {
        let it = unsafe { ffi::cb_diag_iter(self.0) };
        diag::DiagIter(it)
    }
}

impl Drop for TranslationUnit {
    fn drop(&mut self) { unsafe { ffi::cb_transunit_destroy(self.0) } }
}

unsafe impl Send for TranslationUnit {}
