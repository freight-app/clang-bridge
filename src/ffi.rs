//! Raw FFI declarations matching bridge/clang_bridge.h.
#![allow(non_camel_case_types, dead_code)]

use std::ffi::c_char;

#[repr(C)] pub struct CB_Index(());
#[repr(C)] pub struct CB_TransUnit(());
#[repr(C)] pub struct CB_DocIter(());
#[repr(C)] pub struct CB_DiagIter(());
#[repr(C)] pub struct CB_Symbol(());
#[repr(C)] pub struct CB_TidyIter(());
#[repr(C)] pub struct CB_CompletionIter(());

#[repr(C)]
pub struct CB_Location {
    pub file: *mut std::ffi::c_char,
    pub line: u32,
    pub col:  u32,
}

#[repr(C)]
pub struct CB_CompletionItem {
    pub label:         *const c_char,
    pub kind:          u8,
    pub detail:        *const c_char,
    pub documentation: *const c_char,
}

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

extern "C" {
    // Index / TransUnit
    pub fn cb_index_create() -> *mut CB_Index;
    pub fn cb_index_destroy(idx: *mut CB_Index);
    pub fn cb_parse(idx: *mut CB_Index, source_file: *const c_char,
                    args: *const *const c_char, nargs: usize) -> *mut CB_TransUnit;
    pub fn cb_transunit_destroy(tu: *mut CB_TransUnit);

    // Doc extraction
    pub fn cb_doc_extract(tu: *mut CB_TransUnit) -> *mut CB_DocIter;
    pub fn cb_doc_iter_next(it: *mut CB_DocIter, out: *mut CB_DocItem) -> i32;
    pub fn cb_doc_iter_destroy(it: *mut CB_DocIter);

    // Symbol lookup
    pub fn cb_symbol_at(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut CB_Symbol;
    pub fn cb_symbol_name(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_usr(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_kind(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_brief(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_signature(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_def_file(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_def_line(sym: *const CB_Symbol) -> u32;
    pub fn cb_symbol_destroy(sym: *mut CB_Symbol);

    // Compiler diagnostics
    pub fn cb_diag_iter(tu: *mut CB_TransUnit) -> *mut CB_DiagIter;
    pub fn cb_diag_next(it: *mut CB_DiagIter, out: *mut CB_Diag) -> i32;
    pub fn cb_diag_iter_destroy(it: *mut CB_DiagIter);

    // clang-tidy
    pub fn cb_tidy_run(clang_tidy_bin: *const c_char, source_file: *const c_char,
                       checks: *const c_char, args: *const *const c_char,
                       nargs: usize) -> *mut CB_TidyIter;
    pub fn cb_tidy_next(it: *mut CB_TidyIter, out: *mut CB_Diag) -> i32;
    pub fn cb_tidy_iter_destroy(it: *mut CB_TidyIter);

    // Free helpers
    pub fn cb_free_string(s: *mut c_char);

    // Reparse
    pub fn cb_transunit_reparse(tu: *mut CB_TransUnit,
                                buf: *const c_char, len: usize) -> i32;

    // Hover markdown
    pub fn cb_hover_markdown(tu: *mut CB_TransUnit,
                             line: u32, col: u32) -> *mut c_char;

    // Go-to-definition
    pub fn cb_goto_definition(tu: *mut CB_TransUnit, line: u32, col: u32,
                              out: *mut CB_Location) -> i32;

    // Code completion
    pub fn cb_complete(tu: *mut CB_TransUnit,
                       line: u32, col: u32,
                       unsaved_buf: *const c_char, unsaved_len: usize)
                       -> *mut CB_CompletionIter;
    pub fn cb_completion_next(it: *mut CB_CompletionIter,
                              out: *mut CB_CompletionItem) -> i32;
    pub fn cb_completion_iter_destroy(it: *mut CB_CompletionIter);
}
