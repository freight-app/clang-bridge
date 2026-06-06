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

/// Create a new compilation index.
CB_Index *cb_index_create(void);
void      cb_index_destroy(CB_Index *idx);

/// Parse a translation unit from `source_file` using the compile flags in
/// `args[0..nargs]`. Returns NULL on hard failure.
CB_TransUnit *cb_parse(
    CB_Index   *idx,
    const char *source_file,
    const char * const *args,
    size_t      nargs
);
void cb_transunit_destroy(CB_TransUnit *tu);

// ── Doc extraction ────────────────────────────────────────────────────────────

/// Opaque iterator over extracted doc items.
typedef struct CB_DocIter CB_DocIter;

typedef struct {
    const char *kind;        // "function" | "method" | "field" | "record" |
                             //  "enum" | "enumconst" | "typedef" | "var" | "namespace"
    const char *name;        // qualified name
    const char *usr;         // USR string (unique symbol reference)
    const char *brief;       // first sentence of doc comment
    const char *full_comment;// raw comment text
    const char *signature;   // pretty-printed declaration
    const char *file;        // definition file path
    uint32_t    line;        // 1-based line number
} CB_DocItem;

/// Extract all documented declarations from `tu`. Returns an iterator;
/// call cb_doc_iter_next() until it returns 0, then cb_doc_iter_destroy().
CB_DocIter *cb_doc_extract(CB_TransUnit *tu);
/// Returns 1 and fills `*out` (pointers valid until next call or destroy).
/// Returns 0 when iteration is exhausted.
int         cb_doc_iter_next(CB_DocIter *it, CB_DocItem *out);
void        cb_doc_iter_destroy(CB_DocIter *it);

// ── Diagnostics / clang-tidy ──────────────────────────────────────────────────

typedef struct {
    const char *file;
    uint32_t    line;
    uint32_t    col;
    uint8_t     severity; // 0=note 1=remark 2=warning 3=error 4=fatal
    const char *message;
    const char *check_name; // non-null for clang-tidy checks
} CB_Diag;

typedef struct CB_DiagIter CB_DiagIter;

CB_DiagIter *cb_diag_iter(CB_TransUnit *tu);
int          cb_diag_next(CB_DiagIter *it, CB_Diag *out);
void         cb_diag_iter_destroy(CB_DiagIter *it);

#ifdef __cplusplus
}
#endif
