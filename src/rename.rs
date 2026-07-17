use crate::ffi;
use std::ffi::{CStr, CString};

/// A single text replacement needed to rename a symbol.
#[derive(Debug, Clone)]
pub struct RenameEdit {
    /// Source file containing this occurrence.
    pub file: String,
    /// 1-based line.
    pub line: u32,
    /// 1-based column; start of the old name.
    pub col: u32,
    /// UTF-16 code-unit length of the old name (the text to replace).
    pub old_name_len: u32,
    /// The new name to insert.
    pub new_name: String,
}

/// Result of a rename operation: a list of edits plus an optional conflict
/// warning.
pub struct RenameResult(*mut ffi::CB_RenameList);

impl RenameResult {
    /// Number of edit sites.
    pub fn edit_count(&self) -> usize {
        unsafe { ffi::cb_rename_edit_count(self.0) }
    }

    pub fn get_edit(&self, i: usize) -> RenameEdit {
        let mut raw = ffi::CB_RenameEdit {
            file:         std::ptr::null(),
            line:         0,
            col:          0,
            old_name_len: 0,
            new_name:     std::ptr::null(),
        };
        unsafe { ffi::cb_rename_edit_get(self.0, i, &mut raw) };
        let s = |p: *const std::ffi::c_char| {
            if p.is_null() { String::new() }
            else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };
        RenameEdit {
            file:         s(raw.file),
            line:         raw.line,
            col:          raw.col,
            old_name_len: raw.old_name_len,
            new_name:     s(raw.new_name),
        }
    }

    pub fn edits(&self) -> impl Iterator<Item = RenameEdit> + '_ {
        (0..self.edit_count()).map(|i| self.get_edit(i))
    }

    /// Returns `true` when the new name would shadow or conflict with an
    /// existing declaration.  The edit list is still populated for preview.
    pub fn has_conflict(&self) -> bool {
        unsafe { ffi::cb_rename_has_conflict(self.0) != 0 }
    }

    /// Human-readable conflict description, or `None` when safe.
    pub fn conflict_message(&self) -> Option<String> {
        let p = unsafe { ffi::cb_rename_conflict_message(self.0) };
        if p.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
        }
    }
}

impl Drop for RenameResult {
    fn drop(&mut self) {
        unsafe { ffi::cb_rename_list_destroy(self.0) }
    }
}

/// Collect all edits needed to rename the symbol with `usr` to `new_name`.
///
/// If [`RenameResult::has_conflict`] returns `true`, the rename is risky
/// (the name already exists in scope); [`RenameResult::conflict_message`]
/// gives the reason. The edit list is still filled for preview.
///
/// Maps to LSP `textDocument/rename` and `textDocument/prepareRename`.
pub fn rename(tu: &crate::TranslationUnit, usr: &str, new_name: &str) -> RenameResult {
    let Ok(cusr) = CString::new(usr) else {
        return RenameResult(unsafe {
            ffi::cb_rename(tu.0, std::ptr::null(), std::ptr::null())
        });
    };
    let Ok(cname) = CString::new(new_name) else {
        return RenameResult(unsafe {
            ffi::cb_rename(tu.0, std::ptr::null(), std::ptr::null())
        });
    };
    RenameResult(unsafe { ffi::cb_rename(tu.0, cusr.as_ptr(), cname.as_ptr()) })
}
