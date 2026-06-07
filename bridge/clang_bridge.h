#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Opaque handles ────────────────────────────────────────────────────────────

typedef struct CB_Index      CB_Index;
typedef struct CB_TransUnit  CB_TransUnit;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

CB_Index *cb_index_create(void);
void      cb_index_destroy(CB_Index *idx);

/// Parse a translation unit from `source_file` using compile flags `args[0..nargs]`.
/// `working_dir` is used as the compilation directory (the project root from which
/// relative include paths such as `-Iinc` are resolved).  Pass NULL to use ".".
CB_TransUnit *cb_parse(
    CB_Index   *idx,
    const char *source_file,
    const char *working_dir,
    const char * const *args,
    size_t      nargs
);
void cb_transunit_destroy(CB_TransUnit *tu);

// ── Doc extraction ────────────────────────────────────────────────────────────

typedef struct CB_DocIter CB_DocIter;

typedef struct {
    const char *kind;         // "function"|"method"|"field"|"record"|"enum"|
                              //  "enumconst"|"typedef"|"var"|"namespace"
    const char *name;         // qualified name
    const char *usr;          // clang USR (unique symbol reference)
    const char *brief;        // first non-empty line of doc comment
    const char *full_comment; // full formatted comment
    const char *signature;    // pretty-printed declaration
    const char *file;         // source file path
    uint32_t    line;         // 1-based line number
} CB_DocItem;

CB_DocIter *cb_doc_extract(CB_TransUnit *tu);
/// Returns 1 and fills *out (pointers valid until next call or destroy).
int         cb_doc_iter_next(CB_DocIter *it, CB_DocItem *out);
void        cb_doc_iter_destroy(CB_DocIter *it);

// ── Document symbols ──────────────────────────────────────────────────────────

typedef struct {
    const char *name;               // unqualified name
    const char *kind;               // "function"|"method"|"class"|"struct"|"union"|
                                    //  "field"|"enum"|"enumconst"|"typedef"|"var"|"namespace"
    const char *detail;             // signature / type (may be NULL)
    uint32_t    range_start_line;   // 1-based; full declaration range
    uint32_t    range_start_col;
    uint32_t    range_end_line;
    uint32_t    range_end_col;
    uint32_t    sel_line;           // 1-based; name-token (selection range)
    uint32_t    sel_col;
    int32_t     parent;             // index in list of enclosing symbol, -1 = top-level
} CB_DocSym;

typedef struct CB_DocSymList CB_DocSymList;

CB_DocSymList *cb_document_symbols(CB_TransUnit *tu);
size_t         cb_doc_sym_count(const CB_DocSymList *list);
void           cb_doc_sym_get(const CB_DocSymList *list, size_t i, CB_DocSym *out);
void           cb_doc_sym_list_destroy(CB_DocSymList *list);

// ── Symbol lookup (for hover) ─────────────────────────────────────────────────

/// Look up the symbol at (line, col) in `tu`. Returns NULL if not found.
/// Caller must free with cb_symbol_destroy().
typedef struct CB_Symbol CB_Symbol;

CB_Symbol  *cb_symbol_at(CB_TransUnit *tu, uint32_t line, uint32_t col);
const char *cb_symbol_name(const CB_Symbol *sym);
const char *cb_symbol_usr(const CB_Symbol *sym);
const char *cb_symbol_kind(const CB_Symbol *sym);
const char *cb_symbol_brief(const CB_Symbol *sym);
const char *cb_symbol_signature(const CB_Symbol *sym);
const char *cb_symbol_def_file(const CB_Symbol *sym);
uint32_t    cb_symbol_def_line(const CB_Symbol *sym);
void        cb_symbol_destroy(CB_Symbol *sym);

// ── Compiler diagnostics ──────────────────────────────────────────────────────

typedef struct {
    const char *file;
    uint32_t    line;          // squiggle start, 1-based
    uint32_t    col;
    uint32_t    end_line;      // squiggle end (inclusive, 1-based; same as line/col if unknown)
    uint32_t    end_col;
    uint8_t     severity;      // 0=note 1=remark 2=warning 3=error 4=fatal
    const char *message;
    const char *check_name;    // NULL if unknown
} CB_Diag;

/// One source-level replacement suggested by the compiler.
typedef struct {
    uint32_t    start_line, start_col; // replacement range start (1-based)
    uint32_t    end_line,   end_col;   // replacement range end   (1-based)
    const char *replacement;           // text to insert (empty = pure deletion)
} CB_FixIt;

typedef struct CB_DiagIter CB_DiagIter;

CB_DiagIter *cb_diag_iter(CB_TransUnit *tu);
/// Returns 1 and fills *out; pointers valid until next call or destroy.
int          cb_diag_next(CB_DiagIter *it, CB_Diag *out);
/// Fix-its for the diagnostic most recently returned by cb_diag_next.
size_t       cb_diag_fixit_count(const CB_DiagIter *it);
void         cb_diag_fixit_get(const CB_DiagIter *it, size_t i, CB_FixIt *out);
void         cb_diag_iter_destroy(CB_DiagIter *it);

// ── Free helpers ─────────────────────────────────────────────────────────────

/// Free a string returned by cb_hover_markdown or other heap-allocating APIs.
void cb_free_string(char *s);

// ── Reparse (unsaved buffer) ──────────────────────────────────────────────────

/// Reparse the TU with an in-memory replacement for its main file.
/// Pass buf=NULL/len=0 to reparse from disk.  Returns 1 on success.
int cb_transunit_reparse(CB_TransUnit *tu,
                         const char *buf, size_t len);

// ── Hover markdown ────────────────────────────────────────────────────────────

/// Return the raw (stripped) doc comment text for the symbol at (line, col).
/// Comment markers (`///`, `/** */`) are removed; formatting is otherwise
/// preserved.  Returns NULL when no comment is attached to the symbol.
/// Caller must free with cb_free_string().
char *cb_raw_comment_at(CB_TransUnit *tu, uint32_t line, uint32_t col);

/// Brief hover: signature in a code block + first comment line.
/// Caller must free the result with cb_free_string().
char *cb_hover_markdown(CB_TransUnit *tu, uint32_t line, uint32_t col);

/// Full hover: signature + structured doc comment (@param/@returns/@note) +
/// definition location footer.  Falls back to raw comment text when no AST
/// comment is available.  Caller must free with cb_free_string().
char *cb_hover_full(CB_TransUnit *tu, uint32_t line, uint32_t col);

// ── Go-to-definition ──────────────────────────────────────────────────────────

typedef struct {
    char     *file;   // heap-allocated; free with cb_free_string()
    uint32_t  line;   // 1-based
    uint32_t  col;    // 1-based
} CB_Location;

/// Fills *out and returns 1 if a definition location is found, 0 otherwise.
/// Caller must free out->file with cb_free_string().
int cb_goto_definition(CB_TransUnit *tu, uint32_t line, uint32_t col,
                       CB_Location *out);

// ── Signature help ────────────────────────────────────────────────────────────

/// Opaque container for overload candidates at a call site.
typedef struct CB_SigHelp CB_SigHelp;

/// Returns overload candidates at (line, col), or NULL if cursor is not inside
/// a function call.  Caller must free with cb_sig_help_destroy().
CB_SigHelp *cb_signature_help(CB_TransUnit *tu, uint32_t line, uint32_t col);
/// 0-based index of the argument currently under the cursor.
uint32_t    cb_sig_help_active_param(const CB_SigHelp *sh);
size_t      cb_sig_help_overload_count(const CB_SigHelp *sh);
/// Full signature label for overload `overload_i`.  Pointer valid until next
/// call on `sh` or cb_sig_help_destroy.
const char *cb_sig_help_label(CB_SigHelp *sh, size_t overload_i);
size_t      cb_sig_help_param_count(const CB_SigHelp *sh, size_t overload_i);
/// Label for parameter `param_i` of overload `overload_i`.
const char *cb_sig_help_param_label(CB_SigHelp *sh, size_t overload_i, size_t param_i);
void        cb_sig_help_destroy(CB_SigHelp *sh);

// ── Parse failure message ─────────────────────────────────────────────────────

/// Returns a human-readable error string from the last failed cb_parse or
/// cb_parse_unsaved call, or NULL if the last parse succeeded.
/// Pointer is owned by the index; do not free.
const char *cb_index_last_error(const CB_Index *idx);

// ── Parse from memory ─────────────────────────────────────────────────────────

/// Parse `contents` (length `len`) as if it were the file at `virtual_path`.
/// Useful for LSP servers where the editor holds unsaved edits.
/// Returns NULL on failure; call cb_index_last_error for details.
/// Caller must free with cb_transunit_destroy().
CB_TransUnit *cb_parse_unsaved(
    CB_Index   *idx,
    const char *virtual_path,
    const char *working_dir,
    const char *contents,
    size_t      len,
    const char * const *args,
    size_t      nargs
);

// ── #include graph ────────────────────────────────────────────────────────────

typedef struct {
    const char *including_file; // file that contains the directive
    const char *included_file;  // resolved path of the included header
    uint32_t    line;           // 1-based line of the #include directive
    uint32_t    start_col;      // 1-based column of the opening quote/angle
    uint32_t    end_col;        // 1-based column past the closing quote/angle
} CB_Inclusion;

typedef struct CB_InclusionList CB_InclusionList;

/// Collect all #include directives recorded in the TU.
/// Maps to LSP textDocument/documentLink.
/// Caller must free with cb_inclusion_list_destroy().
CB_InclusionList *cb_inclusions(CB_TransUnit *tu);
size_t            cb_inclusion_count(const CB_InclusionList *list);
/// Fills *out; string pointers valid until next call to cb_inclusion_get or destroy.
void              cb_inclusion_get(const CB_InclusionList *list, size_t i, CB_Inclusion *out);
void              cb_inclusion_list_destroy(CB_InclusionList *list);

// ── Semantic tokens ───────────────────────────────────────────────────────────

/// Token type constants (compatible with LSP SemanticTokenTypes).
#define CB_TOK_NAMESPACE   0
#define CB_TOK_TYPE        1
#define CB_TOK_FUNCTION    2
#define CB_TOK_METHOD      3
#define CB_TOK_PROPERTY    4
#define CB_TOK_VARIABLE    5
#define CB_TOK_PARAMETER   6
#define CB_TOK_ENUM_MEMBER 7
#define CB_TOK_MACRO       8

typedef struct {
    uint32_t line;       // 1-based
    uint32_t col;        // 1-based
    uint32_t length;     // name length in characters
    uint8_t  token_type; // CB_TOK_* constant above
} CB_SemanticToken;

typedef struct CB_SemanticTokenList CB_SemanticTokenList;

/// Classify every named identifier in the TU. Results are sorted by
/// (line, col). Maps to LSP textDocument/semanticTokens/full.
/// Caller must free with cb_semantic_token_list_destroy().
CB_SemanticTokenList *cb_semantic_tokens(CB_TransUnit *tu);
size_t                cb_semantic_token_count(const CB_SemanticTokenList *list);
/// Fills *out; valid until next call or destroy.
void                  cb_semantic_token_get(const CB_SemanticTokenList *list,
                                            size_t i, CB_SemanticToken *out);
void                  cb_semantic_token_list_destroy(CB_SemanticTokenList *list);

// ── clang-format ─────────────────────────────────────────────────────────────

typedef struct {
    uint32_t    offset;      // byte offset in the source buffer
    uint32_t    length;      // bytes to delete at offset
    const char *replacement; // text to insert (may be empty = pure deletion)
} CB_FormatEdit;

typedef struct CB_FormatList CB_FormatList;

/// Format `source` (length `len`) using the .clang-format file found by
/// walking up from `style_dir` (pass NULL to use LLVM style).
/// Returns a list of non-overlapping text replacements. Apply in reverse-offset
/// order so earlier edits don't shift later offsets.
/// Maps to LSP textDocument/formatting.
/// Caller must free with cb_format_list_destroy().
CB_FormatList *cb_format(const char *source, size_t len, const char *style_dir);
size_t         cb_format_edit_count(const CB_FormatList *list);
/// Fills *out; replacement pointer valid until next call or destroy.
void           cb_format_edit_get(const CB_FormatList *list, size_t i, CB_FormatEdit *out);
void           cb_format_list_destroy(CB_FormatList *list);

// ── References ────────────────────────────────────────────────────────────────

typedef struct {
    const char *file;          // source file path
    uint32_t    line;          // 1-based
    uint32_t    col;           // 1-based
    int         is_definition; // 1 if this occurrence is the definition site
} CB_Reference;

typedef struct CB_ReferenceList CB_ReferenceList;

/// Find all occurrences of the symbol identified by `usr` within `tu`.
/// `usr` is a Clang Unified Symbol Reference string (from cb_symbol_usr).
/// Maps to LSP textDocument/references.
/// Caller must free with cb_reference_list_destroy().
CB_ReferenceList *cb_references(CB_TransUnit *tu, const char *usr);
size_t            cb_reference_count(const CB_ReferenceList *list);
/// Fills *out; file pointer valid until next call or destroy.
void              cb_reference_get(const CB_ReferenceList *list, size_t i, CB_Reference *out);
void              cb_reference_list_destroy(CB_ReferenceList *list);

// ── Rename ────────────────────────────────────────────────────────────────────

typedef struct {
    const char *file;         // source file containing this occurrence
    uint32_t    line;         // 1-based
    uint32_t    col;          // 1-based; start of the old name
    uint32_t    old_name_len; // byte length of the name to replace
    const char *new_name;     // replacement text
} CB_RenameEdit;

typedef struct CB_RenameList CB_RenameList;

/// Collect all edits needed to rename the symbol with `usr` to `new_name`.
/// cb_rename_has_conflict returns 1 when the new name would shadow or conflict
/// with an existing declaration; cb_rename_conflict_message gives the reason.
/// The edit list is still filled even when a conflict is detected (for preview).
/// Maps to LSP textDocument/rename and textDocument/prepareRename.
/// Caller must free with cb_rename_list_destroy().
CB_RenameList *cb_rename(CB_TransUnit *tu, const char *usr, const char *new_name);
size_t         cb_rename_edit_count(const CB_RenameList *list);
/// Fills *out; pointer fields valid until next call or destroy.
void           cb_rename_edit_get(const CB_RenameList *list, size_t i, CB_RenameEdit *out);
/// Returns 1 when the rename conflicts with an existing declaration.
int            cb_rename_has_conflict(const CB_RenameList *list);
/// Returns a human-readable conflict message, or NULL when there is no conflict.
const char    *cb_rename_conflict_message(const CB_RenameList *list);
void           cb_rename_list_destroy(CB_RenameList *list);

// ── Inlay hints ───────────────────────────────────────────────────────────────

typedef struct {
    uint32_t    line;   // 1-based; hint appears before this position
    uint32_t    col;    // 1-based
    const char *label;  // e.g. "x:" for a parameter hint, ": int" for a type hint
    uint8_t     kind;   // 0 = parameter name, 1 = deduced type
} CB_InlayHint;

typedef struct CB_InlayHintList CB_InlayHintList;

/// Collect inlay hints for all lines in [start_line, end_line] (1-based, inclusive).
/// Covers parameter-name hints at call sites and deduced-type hints for `auto` vars.
/// Caller must free with cb_inlay_hint_list_destroy().
CB_InlayHintList *cb_inlay_hints(CB_TransUnit *tu,
                                  uint32_t start_line, uint32_t end_line);
size_t            cb_inlay_hint_count(const CB_InlayHintList *list);
/// Fills *out; label pointer valid until the next call to cb_inlay_hint_get or destroy.
void              cb_inlay_hint_get(const CB_InlayHintList *list, size_t i, CB_InlayHint *out);
void              cb_inlay_hint_list_destroy(CB_InlayHintList *list);

// ── Type at cursor ────────────────────────────────────────────────────────────

/// Return the fully-qualified type string for the variable/field/parameter at
/// (line, col), or NULL when not applicable.  Useful for enriching hover text.
/// Caller must free with cb_free_string().
char *cb_type_at(CB_TransUnit *tu, uint32_t line, uint32_t col);

// ── Macro hover ───────────────────────────────────────────────────────────────

/// Return a Markdown hover block for the macro at (line, col) — shows the
/// `#define` spelling, parameter list (if function-like), expansion tokens,
/// and the definition-location footer.  Returns NULL when (line, col) is not
/// a macro reference.  Caller must free with cb_free_string().
char *cb_macro_at(CB_TransUnit *tu, uint32_t line, uint32_t col);

// ── Code completion ───────────────────────────────────────────────────────────

typedef struct {
    const char *label;          // typed text / identifier
    uint8_t     kind;           // LSP CompletionItemKind (1-25)
    const char *detail;         // return type + signature excerpt
    const char *documentation;  // brief doc comment (may be NULL)
} CB_CompletionItem;

typedef struct CB_CompletionIter CB_CompletionIter;

/// Run code-complete at (line, col) with an optional in-memory replacement for
/// the main file (unsaved_buf/unsaved_len; pass NULL/0 to use on-disk content).
CB_CompletionIter *cb_complete(CB_TransUnit *tu,
                               uint32_t line, uint32_t col,
                               const char *unsaved_buf, size_t unsaved_len);
/// Returns 1 and fills *out (pointers valid until next call or destroy).
int  cb_completion_next(CB_CompletionIter *it, CB_CompletionItem *out);
void cb_completion_iter_destroy(CB_CompletionIter *it);

#ifdef __cplusplus
}
#endif
