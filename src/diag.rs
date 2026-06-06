use std::ffi::CStr;
use crate::ffi;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    Note,
    Remark,
    Warning,
    Error,
    Fatal,
}

impl Severity {
    fn from_u8(v: u8) -> Self {
        match v {
            0 => Severity::Note,
            1 => Severity::Remark,
            2 => Severity::Warning,
            3 => Severity::Error,
            _ => Severity::Fatal,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Diagnostic {
    pub file:       String,
    pub line:       u32,
    pub col:        u32,
    pub severity:   Severity,
    pub message:    String,
    pub check_name: Option<String>,
}

pub struct DiagIter(pub(crate) *mut ffi::CB_DiagIter);

impl Iterator for DiagIter {
    type Item = Diagnostic;

    fn next(&mut self) -> Option<Diagnostic> {
        let mut raw = ffi::CB_Diag {
            file:       std::ptr::null(),
            line:       0,
            col:        0,
            severity:   0,
            message:    std::ptr::null(),
            check_name: std::ptr::null(),
        };
        let ok = unsafe { ffi::cb_diag_next(self.0, &mut raw) };
        if ok == 0 { return None; }

        unsafe fn s(p: *const std::ffi::c_char) -> String {
            if p.is_null() { String::new() }
            else { CStr::from_ptr(p).to_string_lossy().into_owned() }
        }
        Some(unsafe { Diagnostic {
            file:       s(raw.file),
            line:       raw.line,
            col:        raw.col,
            severity:   Severity::from_u8(raw.severity),
            message:    s(raw.message),
            check_name: if raw.check_name.is_null() { None }
                        else { Some(s(raw.check_name)) },
        }})
    }
}

impl Drop for DiagIter {
    fn drop(&mut self) { unsafe { ffi::cb_diag_iter_destroy(self.0) } }
}
