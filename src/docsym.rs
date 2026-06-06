//! Document symbol list (`textDocument/documentSymbol`).

use std::ffi::CStr;

use crate::ffi;

/// One entry in the document symbol tree.
#[derive(Debug, Clone)]
pub struct DocSym {
    pub name: String,
    /// One of: `function`, `method`, `class`, `struct`, `union`, `field`,
    /// `enum`, `enumconst`, `typedef`, `var`, `namespace`.
    pub kind: String,
    /// Signature or type string. Empty for namespaces.
    pub detail: String,
    /// Full declaration range (1-based).
    pub range_start_line: u32,
    pub range_start_col: u32,
    pub range_end_line: u32,
    pub range_end_col: u32,
    /// Name-token location (LSP selectionRange, 1-based).
    pub sel_line: u32,
    pub sel_col: u32,
    /// Index of the enclosing symbol in the flat list, or -1 for top-level.
    pub parent: i32,
}

/// Flat ordered list of document symbols with parent indices.
pub struct DocSymList(*mut ffi::CB_DocSymList);

impl DocSymList {
    pub(crate) fn from_raw(ptr: *mut ffi::CB_DocSymList) -> Option<Self> {
        if ptr.is_null() { None } else { Some(Self(ptr)) }
    }

    pub fn len(&self) -> usize {
        unsafe { ffi::cb_doc_sym_count(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, i: usize) -> DocSym {
        let mut raw = ffi::CB_DocSym {
            name: std::ptr::null(),
            kind: std::ptr::null(),
            detail: std::ptr::null(),
            range_start_line: 0,
            range_start_col: 0,
            range_end_line: 0,
            range_end_col: 0,
            sel_line: 0,
            sel_col: 0,
            parent: -1,
        };
        unsafe { ffi::cb_doc_sym_get(self.0, i, &mut raw) };

        let s = |p: *const std::ffi::c_char| -> String {
            if p.is_null() { String::new() }
            else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };

        DocSym {
            name:             s(raw.name),
            kind:             s(raw.kind),
            detail:           s(raw.detail),
            range_start_line: raw.range_start_line,
            range_start_col:  raw.range_start_col,
            range_end_line:   raw.range_end_line,
            range_end_col:    raw.range_end_col,
            sel_line:         raw.sel_line,
            sel_col:          raw.sel_col,
            parent:           raw.parent,
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = DocSym> + '_ {
        (0..self.len()).map(move |i| self.get(i))
    }
}

impl Drop for DocSymList {
    fn drop(&mut self) {
        unsafe { ffi::cb_doc_sym_list_destroy(self.0) }
    }
}

unsafe impl Send for DocSymList {}
