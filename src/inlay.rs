use crate::ffi;

/// A single inlay hint: a short label rendered inline in the editor.
#[derive(Debug, Clone)]
pub struct InlayHint {
    /// 1-based line; the hint is displayed *before* this position.
    pub line: u32,
    /// 1-based column.
    pub col: u32,
    /// Display text — e.g. `"x:"` for a parameter hint, `": int"` for a type hint.
    pub label: String,
    /// `0` = parameter name, `1` = deduced type (`auto` variable).
    pub kind: u8,
}

/// Owned list of inlay hints returned by [`inlay_hints`].
pub struct InlayHintList(*mut ffi::CB_InlayHintList);

impl InlayHintList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_inlay_hint_count(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, i: usize) -> InlayHint {
        let mut raw = ffi::CB_InlayHint {
            line:  0,
            col:   0,
            label: std::ptr::null(),
            kind:  0,
        };
        unsafe { ffi::cb_inlay_hint_get(self.0, i, &mut raw) };
        let label = if raw.label.is_null() {
            String::new()
        } else {
            unsafe { std::ffi::CStr::from_ptr(raw.label) }
                .to_string_lossy()
                .into_owned()
        };
        InlayHint { line: raw.line, col: raw.col, label, kind: raw.kind }
    }

    pub fn iter(&self) -> impl Iterator<Item = InlayHint> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for InlayHintList {
    fn drop(&mut self) {
        unsafe { ffi::cb_inlay_hint_list_destroy(self.0) }
    }
}

/// Collect inlay hints for lines `[start_line, end_line]` (1-based, inclusive).
///
/// Returns parameter-name hints at call sites and deduced-type hints for
/// `auto`-declared variables.
pub fn inlay_hints(
    tu: &crate::TranslationUnit,
    start_line: u32,
    end_line: u32,
) -> InlayHintList {
    let ptr = unsafe { ffi::cb_inlay_hints(tu.0, start_line, end_line) };
    InlayHintList(ptr)
}
