use std::ffi::CStr;
use crate::ffi;

/// A resolved symbol at a specific source location.
pub struct Symbol(pub(crate) *mut ffi::CB_Symbol);

impl Symbol {
    pub fn name(&self)      -> &str { unsafe { cstr(ffi::cb_symbol_name(self.0)) } }
    pub fn usr(&self)       -> &str { unsafe { cstr(ffi::cb_symbol_usr(self.0)) } }
    pub fn kind(&self)      -> &str { unsafe { cstr(ffi::cb_symbol_kind(self.0)) } }
    pub fn brief(&self)     -> &str { unsafe { cstr(ffi::cb_symbol_brief(self.0)) } }
    pub fn signature(&self) -> &str { unsafe { cstr(ffi::cb_symbol_signature(self.0)) } }
    pub fn def_file(&self)  -> &str { unsafe { cstr(ffi::cb_symbol_def_file(self.0)) } }
    pub fn def_line(&self)  -> u32  { unsafe { ffi::cb_symbol_def_line(self.0) } }
}

unsafe fn cstr<'a>(p: *const std::ffi::c_char) -> &'a str {
    if p.is_null() { "" } else { CStr::from_ptr(p).to_str().unwrap_or("") }
}

impl Drop for Symbol {
    fn drop(&mut self) { unsafe { ffi::cb_symbol_destroy(self.0) } }
}

unsafe impl Send for Symbol {}
