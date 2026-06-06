//! Signature help — `textDocument/signatureHelp`.
//!
//! Returns overload candidates at a call site together with the index of the
//! argument currently under the cursor.  Returns `None` when the cursor is not
//! inside a function-call argument list.

use std::ffi::CStr;

use crate::ffi;

/// Type and name of one parameter in an overload signature.
#[derive(Debug, Clone)]
pub struct Parameter {
    /// "type name" string, e.g. `"int x"` or `"const std::string &s"`.
    pub label: String,
}

/// One overload candidate returned for signature help.
#[derive(Debug, Clone)]
pub struct Overload {
    /// Full function signature string (return type, name, all parameters).
    pub label: String,
    /// Individual parameter labels in declaration order.
    pub params: Vec<Parameter>,
}

/// Result of `signature_help()`.
#[derive(Debug, Clone)]
pub struct SignatureHelp {
    /// 0-based index of the argument currently under the cursor.
    pub active_param: u32,
    /// All overload candidates visible at this call site.
    pub overloads: Vec<Overload>,
}

/// Run signature help at (1-based) `line`/`col`.  Returns `None` when the
/// cursor is not inside a call's argument list or no candidates are found.
///
/// Unlike code completion, this does not require an unsaved-buffer argument —
/// call `tu.reparse(Some(buf))` first if the buffer differs from disk.
pub fn signature_help(
    tu: &crate::TranslationUnit,
    line: u32,
    col: u32,
) -> Option<SignatureHelp> {
    let sh = unsafe { ffi::cb_signature_help(tu.0, line, col) };
    if sh.is_null() {
        return None;
    }

    let active_param = unsafe { ffi::cb_sig_help_active_param(sh) };
    let n_overloads  = unsafe { ffi::cb_sig_help_overload_count(sh) };

    let mut overloads = Vec::with_capacity(n_overloads);
    for i in 0..n_overloads {
        let label = unsafe {
            let ptr = ffi::cb_sig_help_label(sh, i);
            CStr::from_ptr(ptr).to_string_lossy().into_owned()
        };
        let n_params = unsafe { ffi::cb_sig_help_param_count(sh, i) };
        let mut params = Vec::with_capacity(n_params);
        for p in 0..n_params {
            let plabel = unsafe {
                let ptr = ffi::cb_sig_help_param_label(sh, i, p);
                CStr::from_ptr(ptr).to_string_lossy().into_owned()
            };
            params.push(Parameter { label: plabel });
        }
        overloads.push(Overload { label, params });
    }

    unsafe { ffi::cb_sig_help_destroy(sh) };

    Some(SignatureHelp { active_param, overloads })
}
