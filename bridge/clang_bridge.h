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
    uint32_t    line;
    uint32_t    col;
    uint8_t     severity; // 0=note 1=remark 2=warning 3=error 4=fatal
    const char *message;
    const char *check_name; // NULL for compiler diags; non-NULL for tidy checks
} CB_Diag;

typedef struct CB_DiagIter CB_DiagIter;

CB_DiagIter *cb_diag_iter(CB_TransUnit *tu);
int          cb_diag_next(CB_DiagIter *it, CB_Diag *out);
void         cb_diag_iter_destroy(CB_DiagIter *it);

// ── clang-tidy (subprocess) ───────────────────────────────────────────────────
//
// Invokes the `clang-tidy` binary as a subprocess (--export-fixes=-) and
// parses its YAML/JSON output.  This avoids needing the clangTidy static libs.

typedef struct CB_TidyIter CB_TidyIter;

/// Run clang-tidy on `source_file` with the given `checks` glob and compile
/// flags.  `clang_tidy_bin` may be NULL to use "clang-tidy" from PATH.
CB_TidyIter *cb_tidy_run(
    const char *clang_tidy_bin,
    const char *source_file,
    const char *checks,
    const char * const *args,
    size_t       nargs
);
/// Returns 1 and fills *out (pointers valid until next call or destroy).
int  cb_tidy_next(CB_TidyIter *it, CB_Diag *out);
void cb_tidy_iter_destroy(CB_TidyIter *it);

#ifdef __cplusplus
}
#endif
