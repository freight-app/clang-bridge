mod ffi;

pub mod completion;
pub mod diag;
pub mod doc;
pub mod docsym;
pub mod format;
pub mod goto;
pub mod hover;
pub mod inclusion;
pub mod inlay;
pub mod refs;
pub mod rename;
pub mod semtok;
pub mod sighelp;
pub mod symbol;

use std::ffi::CString;

/// Handle to a clang compilation index. Reuse across parses.
pub struct Index(*mut ffi::CB_Index);

impl Index {
    pub fn new() -> Self {
        Index(unsafe { ffi::cb_index_create() })
    }

    /// Parse `source_file` with the given compile `args`.
    pub fn parse(&self, source_file: &str, args: &[&str]) -> Option<TranslationUnit> {
        let src = CString::new(source_file).ok()?;
        let cargs: Vec<CString> = args.iter().filter_map(|a| CString::new(*a).ok()).collect();
        let ptrs: Vec<*const std::ffi::c_char> = cargs.iter().map(|s| s.as_ptr()).collect();
        let tu = unsafe { ffi::cb_parse(self.0, src.as_ptr(), ptrs.as_ptr(), ptrs.len()) };
        if tu.is_null() { None } else { Some(TranslationUnit(tu)) }
    }

    /// Parse `contents` as if they were the file at `virtual_path`.
    ///
    /// Useful for LSP servers where the editor holds unsaved edits. On failure
    /// returns `None`; call [`Index::last_error`] for a description.
    pub fn parse_unsaved(
        &self,
        virtual_path: &str,
        contents: &str,
        args: &[&str],
    ) -> Option<TranslationUnit> {
        let vp = CString::new(virtual_path).ok()?;
        let cargs: Vec<CString> = args.iter().filter_map(|a| CString::new(*a).ok()).collect();
        let ptrs: Vec<*const std::ffi::c_char> = cargs.iter().map(|s| s.as_ptr()).collect();
        let tu = unsafe {
            ffi::cb_parse_unsaved(
                self.0,
                vp.as_ptr(),
                contents.as_ptr() as *const _,
                contents.len(),
                ptrs.as_ptr(),
                ptrs.len(),
            )
        };
        if tu.is_null() { None } else { Some(TranslationUnit(tu)) }
    }

    /// Return the error message from the most recent failed [`parse`] or
    /// [`parse_unsaved`] call, or `None` if the last parse succeeded.
    pub fn last_error(&self) -> Option<String> {
        let p = unsafe { ffi::cb_index_last_error(self.0) };
        if p.is_null() {
            None
        } else {
            Some(unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned())
        }
    }
}

impl Default for Index {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Index {
    fn drop(&mut self) {
        unsafe { ffi::cb_index_destroy(self.0) }
    }
}

unsafe impl Send for Index {}

/// A parsed translation unit. Owns the AST.
pub struct TranslationUnit(*mut ffi::CB_TransUnit);

impl TranslationUnit {
    /// All documented declarations in this TU.
    pub fn doc_items(&self) -> doc::DocIter {
        doc::DocIter(unsafe { ffi::cb_doc_extract(self.0) })
    }

    /// Compiler diagnostics stored in this TU.
    pub fn diagnostics(&self) -> diag::DiagIter {
        diag::DiagIter(unsafe { ffi::cb_diag_iter(self.0) })
    }

    /// Look up the symbol at the given source position (1-based line/col).
    /// Returns `None` if no symbol is found at that location.
    pub fn symbol_at(&self, line: u32, col: u32) -> Option<symbol::Symbol> {
        let sym = unsafe { ffi::cb_symbol_at(self.0, line, col) };
        if sym.is_null() {
            None
        } else {
            Some(symbol::Symbol(sym))
        }
    }

    /// Return the flat document symbol list for this TU.
    ///
    /// The list is ordered by source position. Each entry carries a `parent`
    /// index into the same list (-1 = top-level), so callers can reconstruct
    /// the nesting tree. Maps to LSP `textDocument/documentSymbol`.
    pub fn document_symbols(&self) -> Option<docsym::DocSymList> {
        let ptr = unsafe { ffi::cb_document_symbols(self.0) };
        docsym::DocSymList::from_raw(ptr)
    }

    /// All `#include` directives recorded in this TU.
    pub fn inclusions(&self) -> inclusion::InclusionList {
        inclusion::inclusions(self)
    }

    /// Classify every named identifier in this TU (sorted by position).
    pub fn semantic_tokens(&self) -> semtok::SemanticTokenList {
        semtok::semantic_tokens(self)
    }

    /// Find all occurrences of the symbol with `usr` within this TU.
    pub fn references(&self, usr: &str) -> refs::ReferenceList {
        refs::references(self, usr)
    }

    /// Collect all edits to rename the symbol with `usr` to `new_name`.
    pub fn rename(&self, usr: &str, new_name: &str) -> rename::RenameResult {
        rename::rename(self, usr, new_name)
    }

    /// Reparse the translation unit with an optional in-memory replacement for
    /// its main file. Pass `None` to reparse from disk.
    pub fn reparse(&self, content: Option<&str>) -> bool {
        match content {
            Some(s) => {
                let Ok(cs) = CString::new(s) else {
                    return false;
                };
                unsafe { ffi::cb_transunit_reparse(self.0, cs.as_ptr(), s.len()) == 1 }
            }
            None => unsafe { ffi::cb_transunit_reparse(self.0, std::ptr::null(), 0) == 1 },
        }
    }
}

impl Drop for TranslationUnit {
    fn drop(&mut self) {
        unsafe { ffi::cb_transunit_destroy(self.0) }
    }
}

unsafe impl Send for TranslationUnit {}
