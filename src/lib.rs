mod ffi;

pub mod callhier;
pub mod codeaction;
pub mod completion;
pub mod diag;
pub mod doc;
pub mod docsym;
pub mod folding;
pub mod format;
pub mod goto;
pub mod highlight;
pub mod hover;
pub mod inclusion;
pub mod inlay;
pub mod refs;
pub mod rename;
pub mod semtok;
pub mod sighelp;
pub mod symbol;
pub mod tidy;
pub mod typehier;
pub mod workspace;

use std::ffi::CString;

/// Handle to a clang compilation index. Reuse across parses.
pub struct Index(*mut ffi::CB_Index);

impl Index {
    pub fn new() -> Self {
        Index(unsafe { ffi::cb_index_create() })
    }

    /// Parse `source_file` with the given compile `args`.
    ///
    /// `working_dir` is the project root from which relative include paths in
    /// `args` (e.g. `-Iinc`) are resolved.  Pass `""` to use the process cwd.
    pub fn parse(&self, source_file: &str, working_dir: &str, args: &[&str]) -> Option<TranslationUnit> {
        let src = CString::new(source_file).ok()?;
        let wd  = CString::new(working_dir).ok()?;
        let cargs: Vec<CString> = args.iter().filter_map(|a| CString::new(*a).ok()).collect();
        let ptrs: Vec<*const std::ffi::c_char> = cargs.iter().map(|s| s.as_ptr()).collect();
        let tu = unsafe { ffi::cb_parse(self.0, src.as_ptr(), wd.as_ptr(), ptrs.as_ptr(), ptrs.len()) };
        if tu.is_null() { None } else { Some(TranslationUnit(tu)) }
    }

    /// Parse `contents` as if they were the file at `virtual_path`.
    ///
    /// `working_dir` sets the compilation directory for resolving relative
    /// include paths.  Pass `""` to fall back to the process cwd.
    ///
    /// Useful for LSP servers where the editor holds unsaved edits. On failure
    /// returns `None`; call [`Index::last_error`] for a description.
    pub fn parse_unsaved(
        &self,
        virtual_path: &str,
        working_dir: &str,
        contents: &str,
        args: &[&str],
    ) -> Option<TranslationUnit> {
        let vp = CString::new(virtual_path).ok()?;
        let wd = CString::new(working_dir).ok()?;
        let cargs: Vec<CString> = args.iter().filter_map(|a| CString::new(*a).ok()).collect();
        let ptrs: Vec<*const std::ffi::c_char> = cargs.iter().map(|s| s.as_ptr()).collect();
        let tu = unsafe {
            ffi::cb_parse_unsaved(
                self.0,
                vp.as_ptr(),
                wd.as_ptr(),
                contents.as_ptr() as *const _,
                contents.len(),
                ptrs.as_ptr(),
                ptrs.len(),
            )
        };
        if tu.is_null() { None } else { Some(TranslationUnit(tu)) }
    }

    /// Add all named declarations from `tu` into this index's workspace symbol
    /// table. Call after each successful parse/reparse.
    pub fn workspace_index_add(&self, tu: &TranslationUnit) {
        workspace::workspace_index_add(self, tu);
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
    /// Whether LLVM crash recovery has quarantined this translation unit.
    pub fn is_poisoned(&self) -> bool {
        unsafe { ffi::cb_transunit_is_poisoned(self.0) != 0 }
    }

    /// Description of the crash that poisoned this translation unit.
    pub fn last_error(&self) -> Option<String> {
        let ptr = unsafe { ffi::cb_transunit_last_error(self.0) };
        if ptr.is_null() {
            None
        } else {
            Some(unsafe { std::ffi::CStr::from_ptr(ptr) }.to_string_lossy().into_owned())
        }
    }

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

    /// Find all occurrences of the symbol at `(line, col)` in the main file.
    pub fn highlight(&self, line: u32, col: u32) -> highlight::HighlightList {
        highlight::highlight(self, line, col)
    }

    /// Folding regions (functions, classes, enums, namespaces).
    pub fn folding_ranges(&self) -> folding::FoldingRangeList {
        folding::folding_ranges(self)
    }

    /// Fix-it code actions applicable near `(line, col)`.
    pub fn code_actions(&self, line: u32, col: u32) -> codeaction::CodeActionList {
        codeaction::code_actions(self, line, col)
    }

    /// Prepare a call-hierarchy item for the callable at `(line, col)`.
    pub fn call_hierarchy_prepare(&self, line: u32, col: u32) -> Option<callhier::CallHierItem> {
        callhier::call_hierarchy_prepare(self, line, col)
    }

    /// All functions in this TU that call the function with `usr`.
    pub fn incoming_calls(&self, usr: &str) -> callhier::CallEdgeList {
        callhier::incoming_calls(self, usr)
    }

    /// All functions that the function with `usr` calls within this TU.
    pub fn outgoing_calls(&self, usr: &str) -> callhier::CallEdgeList {
        callhier::outgoing_calls(self, usr)
    }

    /// Prepare a type-hierarchy item for the class/struct at `(line, col)`.
    pub fn type_hierarchy_prepare(&self, line: u32, col: u32) -> Option<typehier::TypeHierItem> {
        typehier::type_hierarchy_prepare(self, line, col)
    }

    /// Direct base classes of the type with `usr`.
    pub fn supertypes(&self, usr: &str) -> typehier::TypeHierList {
        typehier::supertypes(self, usr)
    }

    /// All types in this TU that directly derive from the type with `usr`.
    pub fn subtypes(&self, usr: &str) -> typehier::TypeHierList {
        typehier::subtypes(self, usr)
    }

    /// Show the macro definition and full expansion at `(line, col)`.
    pub fn expand_macro(&self, line: u32, col: u32) -> Option<String> {
        let p = unsafe { ffi::cb_expand_macro(self.0, line, col) };
        if p.is_null() { return None; }
        let s = unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned();
        unsafe { ffi::cb_free_string(p) };
        Some(s)
    }

    /// JSON array of named declarations in `[start_line, end_line]` (1-based).
    pub fn ast_dump(&self, start_line: u32, end_line: u32) -> String {
        let p = unsafe { ffi::cb_ast_dump(self.0, start_line, end_line) };
        if p.is_null() { return "[]".to_owned(); }
        let s = unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned();
        unsafe { ffi::cb_free_string(p) };
        s
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

#[cfg(all(test, unix))]
mod crash_recovery_tests {
    use super::{ffi, Index};

    #[test]
    fn abort_inside_recovery_boundary_returns_to_rust() {
        let index = Index::new();
        let recovered = unsafe { ffi::cb_crash_recovery_probe(index.0) };

        assert_eq!(recovered, 0);
        assert_eq!(
            index.last_error().as_deref(),
            Some("clang crashed during crash recovery probe")
        );
    }

    #[test]
    fn abort_poisoned_translation_unit_rejects_later_queries() {
        let index = Index::new();
        let tu = index
            .parse_unsaved(
                "/tmp/cb_crash_recovery.cpp",
                "",
                "int answer = 42;\n",
                &["-std=c++17"],
            )
            .expect("crash recovery fixture should parse");

        let recovered = unsafe { ffi::cb_transunit_crash_recovery_probe(tu.0) };

        assert_eq!(recovered, 0);
        assert!(tu.is_poisoned());
        assert_eq!(
            tu.last_error().as_deref(),
            Some("clang crashed during translation unit crash recovery probe")
        );
        assert_eq!(tu.diagnostics().count(), 0);
        assert!(tu.symbol_at(1, 5).is_none());
    }
}
