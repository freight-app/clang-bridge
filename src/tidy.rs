use std::ffi::{CStr, CString};
use crate::ffi;
use crate::diag::{Diagnostic, Severity};

/// Iterator over clang-tidy findings for a single file.
pub struct TidyIter(*mut ffi::CB_TidyIter);

/// Run clang-tidy on `source_file`.
///
/// - `clang_tidy_bin`: path to `clang-tidy`; `None` uses `"clang-tidy"` from PATH.
/// - `checks`: comma-separated glob, e.g. `"modernize-*,readability-*"`. `None` means `"*"`.
/// - `args`: compile flags (e.g. `["-std=c++17", "-I/usr/include"]`).
pub fn run(
    clang_tidy_bin: Option<&str>,
    source_file: &str,
    checks: Option<&str>,
    args: &[&str],
) -> TidyIter {
    let bin = clang_tidy_bin.map(|s| CString::new(s).unwrap());
    let src = CString::new(source_file).unwrap();
    let chk = checks.map(|s| CString::new(s).unwrap());
    let cargs: Vec<CString> = args.iter().filter_map(|a| CString::new(*a).ok()).collect();
    let ptrs: Vec<*const std::ffi::c_char> = cargs.iter().map(|s| s.as_ptr()).collect();

    let it = unsafe {
        ffi::cb_tidy_run(
            bin.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            src.as_ptr(),
            chk.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            ptrs.as_ptr(),
            ptrs.len(),
        )
    };
    TidyIter(it)
}

impl Iterator for TidyIter {
    type Item = Diagnostic;

    fn next(&mut self) -> Option<Diagnostic> {
        let mut raw = ffi::CB_Diag {
            file: std::ptr::null(), line: 0, col: 0,
            severity: 0, message: std::ptr::null(), check_name: std::ptr::null(),
        };
        let ok = unsafe { ffi::cb_tidy_next(self.0, &mut raw) };
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
            check_name: if raw.check_name.is_null() { None } else { Some(s(raw.check_name)) },
        }})
    }
}

impl Drop for TidyIter {
    fn drop(&mut self) { unsafe { ffi::cb_tidy_iter_destroy(self.0) } }
}

unsafe impl Send for TidyIter {}
