use crate::ffi;
use std::ffi::CString;

/// A single occurrence of a symbol within the TU.
#[derive(Debug, Clone)]
pub struct Reference {
    /// Source file path.
    pub file: String,
    /// 1-based line.
    pub line: u32,
    /// 1-based column.
    pub col: u32,
    /// `true` when this occurrence is the definition site.
    pub is_definition: bool,
}

/// Owned list of references returned by [`references`].
pub struct ReferenceList(*mut ffi::CB_ReferenceList);

impl ReferenceList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_reference_count(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, i: usize) -> Reference {
        let mut raw = ffi::CB_Reference {
            file:          std::ptr::null(),
            line:          0,
            col:           0,
            is_definition: 0,
        };
        unsafe { ffi::cb_reference_get(self.0, i, &mut raw) };
        let file = if raw.file.is_null() {
            String::new()
        } else {
            unsafe { std::ffi::CStr::from_ptr(raw.file) }
                .to_string_lossy()
                .into_owned()
        };
        Reference {
            file,
            line: raw.line,
            col:  raw.col,
            is_definition: raw.is_definition != 0,
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = Reference> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for ReferenceList {
    fn drop(&mut self) {
        unsafe { ffi::cb_reference_list_destroy(self.0) }
    }
}

/// Find all occurrences of the symbol identified by `usr` within `tu`.
///
/// `usr` is a Clang Unified Symbol Reference string obtained from
/// [`crate::symbol::Symbol::usr`]. Maps to LSP `textDocument/references`.
pub fn references(tu: &crate::TranslationUnit, usr: &str) -> ReferenceList {
    let Ok(cusr) = CString::new(usr) else {
        return ReferenceList(unsafe {
            ffi::cb_references(tu.0, std::ptr::null())
        });
    };
    ReferenceList(unsafe { ffi::cb_references(tu.0, cusr.as_ptr()) })
}
