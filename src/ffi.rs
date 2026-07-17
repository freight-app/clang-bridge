//! Raw FFI declarations matching bridge/clang_bridge.h.
#![allow(non_camel_case_types, dead_code)]

use std::ffi::c_char;

#[repr(C)]
pub struct CB_Index(());
#[repr(C)]
pub struct CB_TransUnit(());
#[repr(C)]
pub struct CB_DocIter(());
#[repr(C)]
pub struct CB_DiagIter(());
#[repr(C)]
pub struct CB_Symbol(());
#[repr(C)]
pub struct CB_CompletionIter(());

#[repr(C)]
pub struct CB_Location {
    pub file: *mut std::ffi::c_char,
    pub line: u32,
    pub col: u32,
}

#[repr(C)]
pub struct CB_CompletionItem {
    pub label: *const c_char,
    pub kind: u8,
    pub detail: *const c_char,
    pub documentation: *const c_char,
}

#[repr(C)]
pub struct CB_DocItem {
    pub kind: *const c_char,
    pub name: *const c_char,
    pub usr: *const c_char,
    pub brief: *const c_char,
    pub full_comment: *const c_char,
    pub signature: *const c_char,
    pub file: *const c_char,
    pub line: u32,
}

#[repr(C)]
pub struct CB_Diag {
    pub file: *const c_char,
    pub line: u32,
    pub col: u32,
    pub end_line: u32,
    pub end_col: u32,
    pub severity: u8,
    pub message: *const c_char,
    pub check_name: *const c_char,
    pub include_anchor_file: *const c_char,
    pub include_anchor_line: u32,
    pub include_anchor_col: u32,
}

#[repr(C)]
pub struct CB_FixIt {
    pub start_line: u32,
    pub start_col: u32,
    pub end_line: u32,
    pub end_col: u32,
    pub replacement: *const c_char,
}

#[repr(C)]
pub struct CB_SigHelp(());

#[repr(C)]
pub struct CB_Inclusion {
    pub including_file: *const c_char,
    pub included_file:  *const c_char,
    pub line:      u32,
    pub start_col: u32,
    pub end_col:   u32,
}

#[repr(C)]
pub struct CB_InclusionList(());

#[repr(C)]
pub struct CB_SemanticToken {
    pub line:       u32,
    pub col:        u32,
    pub length:     u32,
    pub token_type: u8,
}

#[repr(C)]
pub struct CB_SemanticTokenList(());

#[repr(C)]
pub struct CB_FormatEdit {
    pub offset:      u32,
    pub length:      u32,
    pub replacement: *const c_char,
}

#[repr(C)]
pub struct CB_FormatList(());

#[repr(C)]
pub struct CB_Reference {
    pub file:          *const c_char,
    pub line:          u32,
    pub col:           u32,
    pub is_definition: i32,
}

#[repr(C)]
pub struct CB_ReferenceList(());

#[repr(C)]
pub struct CB_RenameEdit {
    pub file:         *const c_char,
    pub line:         u32,
    pub col:          u32,
    pub old_name_len: u32,
    pub new_name:     *const c_char,
}

#[repr(C)]
pub struct CB_RenameList(());

#[repr(C)]
pub struct CB_InlayHint {
    pub line:  u32,
    pub col:   u32,
    pub label: *const c_char,
    pub kind:  u8,
}

#[repr(C)]
pub struct CB_InlayHintList(());

#[repr(C)]
pub struct CB_HighlightEntry {
    pub line:     u32,
    pub col:      u32,
    pub end_line: u32,
    pub end_col:  u32,
    pub kind:     u8,
}
#[repr(C)]
pub struct CB_HighlightList(());

#[repr(C)]
pub struct CB_FoldingRange {
    pub start_line: u32,
    pub end_line:   u32,
    pub kind:       *const c_char,
}
#[repr(C)]
pub struct CB_FoldingRangeList(());

#[repr(C)]
pub struct CB_CodeAction {
    pub title:       *const c_char,
    pub file:        *const c_char,
    pub line:        u32,
    pub col:         u32,
    pub end_line:    u32,
    pub end_col:     u32,
    pub replacement: *const c_char,
}
#[repr(C)]
pub struct CB_CodeActionList(());

#[repr(C)]
pub struct CB_WorkspaceSym {
    pub name:   *const c_char,
    pub detail: *const c_char,
    pub kind:   *const c_char,
    pub file:   *const c_char,
    pub line:   u32,
    pub col:    u32,
    pub usr:    *const c_char,
}
#[repr(C)]
pub struct CB_WorkspaceSymList(());

#[repr(C)]
pub struct CB_CallHierItem(());
#[repr(C)]
pub struct CB_CallEdge {
    pub name:      *const c_char,
    pub detail:    *const c_char,
    pub file:      *const c_char,
    pub line:      u32,
    pub col:       u32,
    pub usr:       *const c_char,
    pub call_line: u32,
    pub call_col:  u32,
}
#[repr(C)]
pub struct CB_CallEdgeList(());

#[repr(C)]
pub struct CB_TypeHierItem(());
#[repr(C)]
pub struct CB_TypeHierEntry {
    pub name:   *const c_char,
    pub detail: *const c_char,
    pub file:   *const c_char,
    pub line:   u32,
    pub col:    u32,
    pub usr:    *const c_char,
}
#[repr(C)]
pub struct CB_TypeHierList(());

#[repr(C)]
pub struct CB_DocSym {
    pub name: *const c_char,
    pub kind: *const c_char,
    pub detail: *const c_char,
    pub range_start_line: u32,
    pub range_start_col: u32,
    pub range_end_line: u32,
    pub range_end_col: u32,
    pub sel_line: u32,
    pub sel_col: u32,
    pub parent: i32,
}

#[repr(C)]
pub struct CB_DocSymList(());

extern "C" {
    // Index / TransUnit
    pub fn cb_index_create() -> *mut CB_Index;
    pub fn cb_index_destroy(idx: *mut CB_Index);
    pub fn cb_parse(
        idx: *mut CB_Index,
        source_file: *const c_char,
        working_dir: *const c_char,
        args: *const *const c_char,
        nargs: usize,
    ) -> *mut CB_TransUnit;
    pub fn cb_transunit_destroy(tu: *mut CB_TransUnit);

    // Doc extraction
    pub fn cb_doc_extract(tu: *mut CB_TransUnit) -> *mut CB_DocIter;
    pub fn cb_doc_iter_next(it: *mut CB_DocIter, out: *mut CB_DocItem) -> i32;
    pub fn cb_doc_iter_destroy(it: *mut CB_DocIter);

    // Symbol lookup
    pub fn cb_symbol_at(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut CB_Symbol;
    pub fn cb_symbol_name(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_usr(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_kind(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_brief(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_signature(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_def_file(sym: *const CB_Symbol) -> *const c_char;
    pub fn cb_symbol_def_line(sym: *const CB_Symbol) -> u32;
    pub fn cb_symbol_destroy(sym: *mut CB_Symbol);

    // Compiler diagnostics
    pub fn cb_diag_iter(tu: *mut CB_TransUnit) -> *mut CB_DiagIter;
    pub fn cb_diag_next(it: *mut CB_DiagIter, out: *mut CB_Diag) -> i32;
    pub fn cb_diag_fixit_count(it: *const CB_DiagIter) -> usize;
    pub fn cb_diag_fixit_get(it: *const CB_DiagIter, i: usize, out: *mut CB_FixIt);
    pub fn cb_diag_iter_destroy(it: *mut CB_DiagIter);

    // Signature help
    pub fn cb_signature_help(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut CB_SigHelp;
    pub fn cb_sig_help_active_param(sh: *const CB_SigHelp) -> u32;
    pub fn cb_sig_help_overload_count(sh: *const CB_SigHelp) -> usize;
    pub fn cb_sig_help_label(sh: *mut CB_SigHelp, overload_i: usize) -> *const c_char;
    pub fn cb_sig_help_param_count(sh: *const CB_SigHelp, overload_i: usize) -> usize;
    pub fn cb_sig_help_param_label(sh: *mut CB_SigHelp, overload_i: usize, param_i: usize) -> *const c_char;
    pub fn cb_sig_help_destroy(sh: *mut CB_SigHelp);

    // Document symbols
    pub fn cb_document_symbols(tu: *mut CB_TransUnit) -> *mut CB_DocSymList;
    pub fn cb_doc_sym_count(list: *const CB_DocSymList) -> usize;
    pub fn cb_doc_sym_get(list: *const CB_DocSymList, i: usize, out: *mut CB_DocSym);
    pub fn cb_doc_sym_list_destroy(list: *mut CB_DocSymList);

    // Free helpers
    pub fn cb_free_string(s: *mut c_char);

    // Reparse
    pub fn cb_transunit_reparse(tu: *mut CB_TransUnit, buf: *const c_char, len: usize) -> i32;

    // Raw comment text
    pub fn cb_raw_comment_at(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut c_char;

    // Hover markdown
    pub fn cb_hover_markdown(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut c_char;
    pub fn cb_hover_full(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut c_char;

    // Go-to-definition
    pub fn cb_goto_definition(
        tu: *mut CB_TransUnit,
        line: u32,
        col: u32,
        out: *mut CB_Location,
    ) -> i32;

    // Code completion
    pub fn cb_complete(
        tu: *mut CB_TransUnit,
        line: u32,
        col: u32,
        unsaved_buf: *const c_char,
        unsaved_len: usize,
    ) -> *mut CB_CompletionIter;
    pub fn cb_completion_next(it: *mut CB_CompletionIter, out: *mut CB_CompletionItem) -> i32;
    pub fn cb_completion_iter_destroy(it: *mut CB_CompletionIter);

    // Inlay hints
    pub fn cb_inlay_hints(
        tu: *mut CB_TransUnit,
        start_line: u32,
        end_line: u32,
    ) -> *mut CB_InlayHintList;
    pub fn cb_inlay_hint_count(list: *const CB_InlayHintList) -> usize;
    pub fn cb_inlay_hint_get(list: *const CB_InlayHintList, i: usize, out: *mut CB_InlayHint);
    pub fn cb_inlay_hint_list_destroy(list: *mut CB_InlayHintList);

    // Type at cursor
    pub fn cb_type_at(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut c_char;

    // Macro hover
    pub fn cb_macro_at(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut c_char;

    // Parse error
    pub fn cb_index_last_error(idx: *const CB_Index) -> *const c_char;

    // Parse from memory
    pub fn cb_parse_unsaved(
        idx: *mut CB_Index,
        virtual_path: *const c_char,
        working_dir: *const c_char,
        contents: *const c_char,
        len: usize,
        args: *const *const c_char,
        nargs: usize,
    ) -> *mut CB_TransUnit;

    // Inclusions
    pub fn cb_inclusions(tu: *mut CB_TransUnit) -> *mut CB_InclusionList;
    pub fn cb_inclusion_count(list: *const CB_InclusionList) -> usize;
    pub fn cb_inclusion_get(list: *const CB_InclusionList, i: usize, out: *mut CB_Inclusion);
    pub fn cb_inclusion_list_destroy(list: *mut CB_InclusionList);

    // Semantic tokens
    pub fn cb_semantic_tokens(tu: *mut CB_TransUnit) -> *mut CB_SemanticTokenList;
    pub fn cb_semantic_token_count(list: *const CB_SemanticTokenList) -> usize;
    pub fn cb_semantic_token_get(
        list: *const CB_SemanticTokenList,
        i: usize,
        out: *mut CB_SemanticToken,
    );
    pub fn cb_semantic_token_list_destroy(list: *mut CB_SemanticTokenList);

    // Format
    pub fn cb_format(
        source: *const c_char,
        len: usize,
        style_dir: *const c_char,
    ) -> *mut CB_FormatList;
    pub fn cb_format_edit_count(list: *const CB_FormatList) -> usize;
    pub fn cb_format_edit_get(list: *const CB_FormatList, i: usize, out: *mut CB_FormatEdit);
    pub fn cb_format_list_destroy(list: *mut CB_FormatList);

    // References
    pub fn cb_references(tu: *mut CB_TransUnit, usr: *const c_char) -> *mut CB_ReferenceList;
    pub fn cb_reference_count(list: *const CB_ReferenceList) -> usize;
    pub fn cb_reference_get(list: *const CB_ReferenceList, i: usize, out: *mut CB_Reference);
    pub fn cb_reference_list_destroy(list: *mut CB_ReferenceList);

    // Rename
    pub fn cb_rename(
        tu: *mut CB_TransUnit,
        usr: *const c_char,
        new_name: *const c_char,
    ) -> *mut CB_RenameList;
    pub fn cb_rename_edit_count(list: *const CB_RenameList) -> usize;
    pub fn cb_rename_edit_get(list: *const CB_RenameList, i: usize, out: *mut CB_RenameEdit);
    pub fn cb_rename_has_conflict(list: *const CB_RenameList) -> i32;
    pub fn cb_rename_conflict_message(list: *const CB_RenameList) -> *const c_char;
    pub fn cb_rename_list_destroy(list: *mut CB_RenameList);

    // Document highlight
    pub fn cb_highlight(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut CB_HighlightList;
    pub fn cb_highlight_count(list: *const CB_HighlightList) -> usize;
    pub fn cb_highlight_get(list: *const CB_HighlightList, i: usize, out: *mut CB_HighlightEntry);
    pub fn cb_highlight_list_destroy(list: *mut CB_HighlightList);

    // Folding ranges
    pub fn cb_folding_ranges(tu: *mut CB_TransUnit) -> *mut CB_FoldingRangeList;
    pub fn cb_folding_range_count(list: *const CB_FoldingRangeList) -> usize;
    pub fn cb_folding_range_get(list: *const CB_FoldingRangeList, i: usize, out: *mut CB_FoldingRange);
    pub fn cb_folding_range_list_destroy(list: *mut CB_FoldingRangeList);

    // Code actions
    pub fn cb_code_actions(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut CB_CodeActionList;
    pub fn cb_code_action_count(list: *const CB_CodeActionList) -> usize;
    pub fn cb_code_action_get(list: *const CB_CodeActionList, i: usize, out: *mut CB_CodeAction);
    pub fn cb_code_action_list_destroy(list: *mut CB_CodeActionList);

    // Workspace symbols
    pub fn cb_workspace_index_add(idx: *mut CB_Index, tu: *mut CB_TransUnit);
    pub fn cb_workspace_symbols(idx: *mut CB_Index, query: *const c_char) -> *mut CB_WorkspaceSymList;
    pub fn cb_workspace_sym_count(list: *const CB_WorkspaceSymList) -> usize;
    pub fn cb_workspace_sym_get(list: *const CB_WorkspaceSymList, i: usize, out: *mut CB_WorkspaceSym);
    pub fn cb_workspace_sym_list_destroy(list: *mut CB_WorkspaceSymList);

    // Call hierarchy
    pub fn cb_call_hierarchy_prepare(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut CB_CallHierItem;
    pub fn cb_call_hier_item_destroy(item: *mut CB_CallHierItem);
    pub fn cb_call_hier_name(item: *const CB_CallHierItem) -> *const c_char;
    pub fn cb_call_hier_detail(item: *const CB_CallHierItem) -> *const c_char;
    pub fn cb_call_hier_file(item: *const CB_CallHierItem) -> *const c_char;
    pub fn cb_call_hier_line(item: *const CB_CallHierItem) -> u32;
    pub fn cb_call_hier_col(item: *const CB_CallHierItem) -> u32;
    pub fn cb_call_hier_usr(item: *const CB_CallHierItem) -> *const c_char;
    pub fn cb_incoming_calls(tu: *mut CB_TransUnit, usr: *const c_char) -> *mut CB_CallEdgeList;
    pub fn cb_outgoing_calls(tu: *mut CB_TransUnit, usr: *const c_char) -> *mut CB_CallEdgeList;
    pub fn cb_call_edge_count(list: *const CB_CallEdgeList) -> usize;
    pub fn cb_call_edge_get(list: *const CB_CallEdgeList, i: usize, out: *mut CB_CallEdge);
    pub fn cb_call_edge_list_destroy(list: *mut CB_CallEdgeList);

    // Type hierarchy
    pub fn cb_type_hierarchy_prepare(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut CB_TypeHierItem;
    pub fn cb_type_hier_item_destroy(item: *mut CB_TypeHierItem);
    pub fn cb_type_hier_name(item: *const CB_TypeHierItem) -> *const c_char;
    pub fn cb_type_hier_detail(item: *const CB_TypeHierItem) -> *const c_char;
    pub fn cb_type_hier_file(item: *const CB_TypeHierItem) -> *const c_char;
    pub fn cb_type_hier_line(item: *const CB_TypeHierItem) -> u32;
    pub fn cb_type_hier_col(item: *const CB_TypeHierItem) -> u32;
    pub fn cb_type_hier_usr(item: *const CB_TypeHierItem) -> *const c_char;
    pub fn cb_supertypes(tu: *mut CB_TransUnit, usr: *const c_char) -> *mut CB_TypeHierList;
    pub fn cb_subtypes(tu: *mut CB_TransUnit, usr: *const c_char) -> *mut CB_TypeHierList;
    pub fn cb_type_hier_count(list: *const CB_TypeHierList) -> usize;
    pub fn cb_type_hier_get(list: *const CB_TypeHierList, i: usize, out: *mut CB_TypeHierEntry);
    pub fn cb_type_hier_list_destroy(list: *mut CB_TypeHierList);

    // Macro expansion
    pub fn cb_expand_macro(tu: *mut CB_TransUnit, line: u32, col: u32) -> *mut c_char;

    // AST dump
    pub fn cb_ast_dump(tu: *mut CB_TransUnit, start_line: u32, end_line: u32) -> *mut c_char;
}
