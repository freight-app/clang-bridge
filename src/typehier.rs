use crate::ffi;
use std::ffi::{CStr, CString};

/// Opaque type-hierarchy item (a class or struct).
pub struct TypeHierItem(*mut ffi::CB_TypeHierItem);

impl TypeHierItem {
    fn s(p: *const std::ffi::c_char) -> String {
        if p.is_null() { String::new() }
        else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
    }
    pub fn name(&self)   -> String { Self::s(unsafe { ffi::cb_type_hier_name(self.0) }) }
    pub fn detail(&self) -> String { Self::s(unsafe { ffi::cb_type_hier_detail(self.0) }) }
    pub fn file(&self)   -> String { Self::s(unsafe { ffi::cb_type_hier_file(self.0) }) }
    pub fn line(&self)   -> u32    { unsafe { ffi::cb_type_hier_line(self.0) } }
    pub fn col(&self)    -> u32    { unsafe { ffi::cb_type_hier_col(self.0) } }
    pub fn usr(&self)    -> String { Self::s(unsafe { ffi::cb_type_hier_usr(self.0) }) }
}

impl Drop for TypeHierItem {
    fn drop(&mut self) { unsafe { ffi::cb_type_hier_item_destroy(self.0) } }
}

/// A type in the hierarchy (base or derived class).
#[derive(Debug, Clone)]
pub struct TypeHierEntry {
    pub name: String,
    pub detail: String,
    pub file: String,
    pub line: u32,
    pub col: u32,
    pub usr: String,
}

/// Owned list of type hierarchy entries.
pub struct TypeHierList(*mut ffi::CB_TypeHierList);

impl TypeHierList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_type_hier_count(self.0) }
    }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, i: usize) -> TypeHierEntry {
        let mut raw = ffi::CB_TypeHierEntry {
            name: std::ptr::null(), detail: std::ptr::null(),
            file: std::ptr::null(), line: 0, col: 0, usr: std::ptr::null(),
        };
        unsafe { ffi::cb_type_hier_get(self.0, i, &mut raw) };
        let s = |p: *const std::ffi::c_char| -> String {
            if p.is_null() { String::new() }
            else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
        };
        TypeHierEntry {
            name: s(raw.name), detail: s(raw.detail), file: s(raw.file),
            line: raw.line, col: raw.col, usr: s(raw.usr),
        }
    }
    pub fn iter(&self) -> impl Iterator<Item = TypeHierEntry> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for TypeHierList {
    fn drop(&mut self) { unsafe { ffi::cb_type_hier_list_destroy(self.0) } }
}

/// Prepare a type-hierarchy item for the class/struct at `(line, col)`.
/// Returns `None` when the cursor is not on a type.
/// Maps to LSP `textDocument/prepareTypeHierarchy`.
pub fn type_hierarchy_prepare(
    tu: &crate::TranslationUnit, line: u32, col: u32,
) -> Option<TypeHierItem> {
    let p = unsafe { ffi::cb_type_hierarchy_prepare(tu.0, line, col) };
    if p.is_null() { None } else { Some(TypeHierItem(p)) }
}

/// Return the direct base classes of the type identified by `usr`.
/// Maps to LSP `typeHierarchy/supertypes`.
pub fn supertypes(tu: &crate::TranslationUnit, usr: &str) -> TypeHierList {
    let Ok(cu) = CString::new(usr) else { return TypeHierList(unsafe { ffi::cb_supertypes(tu.0, std::ptr::null()) }); };
    TypeHierList(unsafe { ffi::cb_supertypes(tu.0, cu.as_ptr()) })
}

/// Return all types in `tu` that directly derive from the type identified by `usr`.
/// Maps to LSP `typeHierarchy/subtypes`.
pub fn subtypes(tu: &crate::TranslationUnit, usr: &str) -> TypeHierList {
    let Ok(cu) = CString::new(usr) else { return TypeHierList(unsafe { ffi::cb_subtypes(tu.0, std::ptr::null()) }); };
    TypeHierList(unsafe { ffi::cb_subtypes(tu.0, cu.as_ptr()) })
}
