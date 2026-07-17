use crate::ffi;

pub struct Location {
    pub file: String,
    /// Definition-name range, 1-based UTF-16 with an exclusive end.
    pub line: u32,
    pub col: u32,
    pub end_line: u32,
    pub end_col: u32,
}

/// Return the definition-name range for the symbol at (line, col), or None.
pub fn goto_definition(tu: &crate::TranslationUnit, line: u32, col: u32) -> Option<Location> {
    let mut out = ffi::CB_Location {
        file: std::ptr::null_mut(),
        line: 0,
        col: 0,
        end_line: 0,
        end_col: 0,
    };
    let found = unsafe { ffi::cb_goto_definition(tu.0, line, col, &mut out) };
    if found == 0 || out.file.is_null() {
        return None;
    }
    let file = unsafe {
        std::ffi::CStr::from_ptr(out.file)
            .to_string_lossy()
            .into_owned()
    };
    unsafe { ffi::cb_free_string(out.file) };
    Some(Location {
        file,
        line: out.line,
        col: out.col,
        end_line: out.end_line,
        end_col: out.end_col,
    })
}
