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
CB_TransUnit *cb_parse(
    CB_Index   *idx,
    const char *source_file,
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
