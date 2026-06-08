use crate::ffi;
use std::ffi::{CStr, CString};

/// A symbol found by workspace symbol search.
#[derive(Debug, Clone)]
pub struct WorkspaceSym {
    /// Unqualified name.
    pub name: String,
    /// Fully-qualified name.
    pub detail: String,
    /// Kind string: `"function"`, `"class"`, `"var"`, etc.
    pub kind: String,
    /// Source file path.
    pub file: String,
    /// 1-based line.
    pub line: u32,
    /// 1-based column.
    pub col: u32,
    /// Clang USR.
    pub usr: String,
}

/// Owned list of workspace symbols.
pub struct WorkspaceSymList(*mut ffi::CB_WorkspaceSymList);

impl WorkspaceSymList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_workspace_sym_count(self.0) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn get(&self, i: usize) -> WorkspaceSym {
        let mut raw = ffi::CB_WorkspaceSym {
            name: std::ptr::null(), detail: std::ptr::null(),
            kind: std::ptr::null(), file: std::ptr::null(),
            line: 0, col: 0, usr: std::ptr::null(),
        };
        unsafe { ffi::cb_workspace_sym_get(self.0, i, &mut raw) };
        let s = |p: *const std::ffi::c_char| -> String {
            if p.is_null() { String::new() }
            else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };
        WorkspaceSym {
            name: s(raw.name), detail: s(raw.detail),
            kind: s(raw.kind), file: s(raw.file),
            line: raw.line, col: raw.col, usr: s(raw.usr),
        }
    }
    pub fn iter(&self) -> impl Iterator<Item = WorkspaceSym> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for WorkspaceSymList {
    fn drop(&mut self) {
        unsafe { ffi::cb_workspace_sym_list_destroy(self.0) }
    }
}

/// Add all named declarations from `tu` to the workspace index in `idx`.
/// Call after each successful parse/reparse to keep the index current.
pub fn workspace_index_add(idx: &crate::Index, tu: &crate::TranslationUnit) {
    unsafe { ffi::cb_workspace_index_add(idx.0, tu.0) }
}

/// Search the workspace index for symbols whose name contains `query`
/// (case-insensitive). Maps to LSP `workspace/symbol`.
pub fn workspace_symbols(idx: &crate::Index, query: &str) -> WorkspaceSymList {
    let Ok(cq) = CString::new(query) else {
        return WorkspaceSymList(unsafe { ffi::cb_workspace_symbols(idx.0, std::ptr::null()) });
    };
    WorkspaceSymList(unsafe { ffi::cb_workspace_symbols(idx.0, cq.as_ptr()) })
}
