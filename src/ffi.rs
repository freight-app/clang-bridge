//! Raw FFI declarations matching bridge/clang_bridge.h.
#![allow(non_camel_case_types, dead_code)]

use std::ffi::c_char;

#[repr(C)] pub struct CB_Index(());
#[repr(C)] pub struct CB_TransUnit(());
#[repr(C)] pub struct CB_DocIter(());
#[repr(C)] pub struct CB_DiagIter(());

#[repr(C)]
pub struct CB_DocItem {
    pub kind:         *const c_char,
    pub name:         *const c_char,
    pub usr:          *const c_char,
    pub brief:        *const c_char,
    pub full_comment: *const c_char,
    pub signature:    *const c_char,
    pub file:         *const c_char,
    pub line:         u32,
}

#[repr(C)]
pub struct CB_Diag {
    pub file:       *const c_char,
    pub line:       u32,
    pub col:        u32,
    pub severity:   u8,
    pub message:    *const c_char,
    pub check_name: *const c_char,
}

unsafe impl Send for CB_DocItem {}
unsafe impl Send for CB_Diag {}

extern "C" {
    pub fn cb_index_create() -> *mut CB_Index;
    pub fn cb_index_destroy(idx: *mut CB_Index);

    pub fn cb_parse(
        idx:         *mut CB_Index,
        source_file: *const c_char,
        args:        *const *const c_char,
        nargs:       usize,
    ) -> *mut CB_TransUnit;
    pub fn cb_transunit_destroy(tu: *mut CB_TransUnit);

    pub fn cb_doc_extract(tu: *mut CB_TransUnit) -> *mut CB_DocIter;
    pub fn cb_doc_iter_next(it: *mut CB_DocIter, out: *mut CB_DocItem) -> i32;
    pub fn cb_doc_iter_destroy(it: *mut CB_DocIter);

    pub fn cb_diag_iter(tu: *mut CB_TransUnit) -> *mut CB_DiagIter;
    pub fn cb_diag_next(it: *mut CB_DiagIter, out: *mut CB_Diag) -> i32;
    pub fn cb_diag_iter_destroy(it: *mut CB_DiagIter);
}
