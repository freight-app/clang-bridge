use crate::ffi;
use std::ffi::{CStr, CString};

/// Opaque call-hierarchy item (a callable symbol).
pub struct CallHierItem(*mut ffi::CB_CallHierItem);

impl CallHierItem {
    fn s(p: *const std::ffi::c_char) -> String {
        if p.is_null() { String::new() }
        else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
    }
    pub fn name(&self)   -> String { Self::s(unsafe { ffi::cb_call_hier_name(self.0) }) }
    pub fn detail(&self) -> String { Self::s(unsafe { ffi::cb_call_hier_detail(self.0) }) }
    pub fn file(&self)   -> String { Self::s(unsafe { ffi::cb_call_hier_file(self.0) }) }
    pub fn line(&self)   -> u32    { unsafe { ffi::cb_call_hier_line(self.0) } }
    pub fn col(&self)    -> u32    { unsafe { ffi::cb_call_hier_col(self.0) } }
    pub fn usr(&self)    -> String { Self::s(unsafe { ffi::cb_call_hier_usr(self.0) }) }
}

impl Drop for CallHierItem {
    fn drop(&mut self) { unsafe { ffi::cb_call_hier_item_destroy(self.0) } }
}

/// A call edge: the caller (incoming) or callee (outgoing) plus the call site.
#[derive(Debug, Clone)]
pub struct CallEdge {
    /// Caller or callee name.
    pub name: String,
    /// Qualified name.
    pub detail: String,
    /// Definition file.
    pub file: String,
    /// 1-based definition line.
    pub line: u32,
    /// 1-based definition column.
    pub col: u32,
    /// Clang USR.
    pub usr: String,
    /// 1-based call-site line.
    pub call_line: u32,
    /// 1-based call-site column.
    pub call_col: u32,
}

/// Owned list of call edges.
pub struct CallEdgeList(*mut ffi::CB_CallEdgeList);

impl CallEdgeList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_call_edge_count(self.0) }
    }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, i: usize) -> CallEdge {
        let mut raw = ffi::CB_CallEdge {
            name: std::ptr::null(), detail: std::ptr::null(),
            file: std::ptr::null(), line: 0, col: 0,
            usr: std::ptr::null(), call_line: 0, call_col: 0,
        };
        unsafe { ffi::cb_call_edge_get(self.0, i, &mut raw) };
        let s = |p: *const std::ffi::c_char| -> String {
            if p.is_null() { String::new() }
            else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };
        CallEdge {
            name: s(raw.name), detail: s(raw.detail), file: s(raw.file),
            line: raw.line, col: raw.col, usr: s(raw.usr),
            call_line: raw.call_line, call_col: raw.call_col,
        }
    }
    pub fn iter(&self) -> impl Iterator<Item = CallEdge> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for CallEdgeList {
    fn drop(&mut self) { unsafe { ffi::cb_call_edge_list_destroy(self.0) } }
}

/// Prepare a call-hierarchy item for the callable at `(line, col)`.
/// Returns `None` when the cursor is not on a function or method.
/// Maps to LSP `textDocument/prepareCallHierarchy`.
pub fn call_hierarchy_prepare(
    tu: &crate::TranslationUnit, line: u32, col: u32,
) -> Option<CallHierItem> {
    let p = unsafe { ffi::cb_call_hierarchy_prepare(tu.0, line, col) };
    if p.is_null() { None } else { Some(CallHierItem(p)) }
}

/// Find all functions in `tu` that call the function identified by `usr`.
/// Maps to LSP `callHierarchy/incomingCalls`.
pub fn incoming_calls(tu: &crate::TranslationUnit, usr: &str) -> CallEdgeList {
    let Ok(cu) = CString::new(usr) else { return CallEdgeList(unsafe { ffi::cb_incoming_calls(tu.0, std::ptr::null()) }); };
    CallEdgeList(unsafe { ffi::cb_incoming_calls(tu.0, cu.as_ptr()) })
}

/// Find all functions that the function identified by `usr` calls within `tu`.
/// Maps to LSP `callHierarchy/outgoingCalls`.
pub fn outgoing_calls(tu: &crate::TranslationUnit, usr: &str) -> CallEdgeList {
    let Ok(cu) = CString::new(usr) else { return CallEdgeList(unsafe { ffi::cb_outgoing_calls(tu.0, std::ptr::null()) }); };
    CallEdgeList(unsafe { ffi::cb_outgoing_calls(tu.0, cu.as_ptr()) })
}
