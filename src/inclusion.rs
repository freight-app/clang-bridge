use crate::ffi;

/// A single `#include` directive recorded in the TU.
#[derive(Debug, Clone)]
pub struct Inclusion {
    /// The file that contains the `#include` directive.
    pub including_file: String,
    /// The resolved path of the included header.
    pub included_file: String,
    /// 1-based line of the directive.
    pub line: u32,
    /// 1-based column of the opening `"` or `<`.
    pub start_col: u32,
    /// 1-based column past the closing `"` or `>`.
    pub end_col: u32,
}

/// Owned list of inclusions returned by [`inclusions`].
pub struct InclusionList(*mut ffi::CB_InclusionList);

impl InclusionList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_inclusion_count(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, i: usize) -> Inclusion {
        let mut raw = ffi::CB_Inclusion {
            including_file: std::ptr::null(),
            included_file:  std::ptr::null(),
            line:      0,
            start_col: 0,
            end_col:   0,
        };
        unsafe { ffi::cb_inclusion_get(self.0, i, &mut raw) };
        let s = |p: *const std::ffi::c_char| {
            if p.is_null() { String::new() }
            else { unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };
        Inclusion {
            including_file: s(raw.including_file),
            included_file:  s(raw.included_file),
            line:      raw.line,
            start_col: raw.start_col,
            end_col:   raw.end_col,
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = Inclusion> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for InclusionList {
    fn drop(&mut self) {
        unsafe { ffi::cb_inclusion_list_destroy(self.0) }
    }
}

/// Collect all `#include` directives recorded in the TU.
///
/// Maps to LSP `textDocument/documentLink`.
pub fn inclusions(tu: &crate::TranslationUnit) -> InclusionList {
    InclusionList(unsafe { ffi::cb_inclusions(tu.0) })
}
