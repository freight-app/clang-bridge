//! Compiler diagnostics (`textDocument/publishDiagnostics`).
//!
//! Each `Diagnostic` now includes the full squiggle range (start → end) and a
//! list of `FixIt` hints that can power `textDocument/codeAction` quick-fixes.

use crate::ffi;
use std::ffi::CStr;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    Note,
    Remark,
    Warning,
    Error,
    Fatal,
}

impl Severity {
    pub(crate) fn from_u8(v: u8) -> Self {
        match v {
            0 => Severity::Note,
            1 => Severity::Remark,
            2 => Severity::Warning,
            3 => Severity::Error,
            _ => Severity::Fatal,
        }
    }
}

/// A single compiler fix-it: replace the range [start, end) with `replacement`.
/// An empty `replacement` is a pure deletion; a zero-length range is an insertion.
#[derive(Debug, Clone)]
pub struct FixIt {
    pub start_line: u32,
    pub start_col: u32,
    pub end_line: u32,
    pub end_col: u32,
    /// Replacement text (empty string = delete the range).
    pub replacement: String,
}

#[derive(Debug, Clone)]
pub struct Diagnostic {
    pub file: String,
    /// Squiggle start, 1-based.
    pub line: u32,
    pub col: u32,
    /// Squiggle end, 1-based. Equals `line`/`col` when no range info is available.
    pub end_line: u32,
    pub end_col: u32,
    pub severity: Severity,
    pub message: String,
    /// Diagnostic check name (e.g. `clang-diagnostic-unused-variable`). `None` if unknown.
    pub check_name: Option<String>,
    /// Compiler-suggested source edits for this diagnostic.
    pub fixits: Vec<FixIt>,
}

/// Iterator over the diagnostics stored in a translation unit.
pub struct DiagIter(pub(crate) *mut ffi::CB_DiagIter);

impl Iterator for DiagIter {
    type Item = Diagnostic;

    fn next(&mut self) -> Option<Diagnostic> {
        let mut raw = ffi::CB_Diag {
            file: std::ptr::null(),
            line: 0,
            col: 0,
            end_line: 0,
            end_col: 0,
            severity: 0,
            message: std::ptr::null(),
            check_name: std::ptr::null(),
        };
        let ok = unsafe { ffi::cb_diag_next(self.0, &mut raw) };
        if ok == 0 {
            return None;
        }

        // Helper: convert a C string pointer to an owned String.
        let s = |p: *const std::ffi::c_char| -> String {
            if p.is_null() { String::new() }
            else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };

        // Collect fix-its for this diagnostic (valid until the next cb_diag_next call).
        let fixit_count = unsafe { ffi::cb_diag_fixit_count(self.0) };
        let mut fixits = Vec::with_capacity(fixit_count);
        for i in 0..fixit_count {
            let mut f = ffi::CB_FixIt {
                start_line: 0,
                start_col: 0,
                end_line: 0,
                end_col: 0,
                replacement: std::ptr::null(),
            };
            unsafe { ffi::cb_diag_fixit_get(self.0, i, &mut f) };
            fixits.push(FixIt {
                start_line:  f.start_line,
                start_col:   f.start_col,
                end_line:    f.end_line,
                end_col:     f.end_col,
                replacement: s(f.replacement),
            });
        }

        Some(Diagnostic {
            file:       s(raw.file),
            line:       raw.line,
            col:        raw.col,
            end_line:   raw.end_line,
            end_col:    raw.end_col,
            severity:   Severity::from_u8(raw.severity),
            message:    s(raw.message),
            check_name: if raw.check_name.is_null() { None } else { Some(s(raw.check_name)) },
            fixits,
        })
    }
}

impl Drop for DiagIter {
    fn drop(&mut self) {
        unsafe { ffi::cb_diag_iter_destroy(self.0) }
    }
}
