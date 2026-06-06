# clang-bridge TODO

API gaps and quality issues found during the 2026-06-06 audit.

---

## High priority

### Fix `SymbolLocator` for reference sites

**Currently:** `SymbolLocator` only matches `NamedDecl` at its *declaration* site.
Hover and goto-definition return nothing when the cursor is on a *use* site
(`DeclRefExpr`, `MemberExpr`, `TypeLoc`, `CXXConstructExpr`).

**Fix needed (`bridge/clang_bridge.cpp`):**
- Add a second visitor pass (or extend `SymbolLocator`) that walks expression nodes
  (`DeclRefExpr`, `MemberExpr`, `CallExpr`, `CXXConstructExpr`) and type locations
  (`TypeLoc`, `ElaboratedTypeLoc`, `TemplateSpecializationTypeLoc`).
- When the cursor falls inside such a node, extract the referenced `NamedDecl`
  (via `DeclRefExpr::getDecl()`, `MemberExpr::getMemberDecl()`, etc.).
- Deduplicate: the existing `NamedDecl` match path remains for declaration lines.

`cb_hover_markdown`, `cb_goto_definition`, and `cb_symbol_at` all share this logic
and are all broken for the same reason. The fix should be a single shared
`locate_symbol_at(ASTUnit*, unsigned line, unsigned col) -> const NamedDecl*`
helper called from all three (removing the current copy-paste).

---

### `cb_document_symbols` — editor outline / symbol list

Missing function: `CB_SymbolInfo* cb_document_symbols(CB_TU*, size_t* count)`.

Visitor collects all `NamedDecl` nodes (functions, classes, namespaces, fields,
enums, typedefs, variables) with name, kind, definition range, and selection range.
Parent/child nesting via `DeclContext` walking.

Maps to LSP `textDocument/documentSymbol`.

---

### Diagnostic end-ranges and fix-it hints

`CB_Diag` currently only has a point location (line, col). Clang diagnostics carry:
- `SourceRange` — start and end of the underlined region.
- `FixItHint` — replacement range + replacement text.

Both should be added to `CB_Diag` and exposed in `src/diag.rs`.

This enables:
- Precise editor squiggles (vs single-character underlines).
- `textDocument/codeAction` quick-fix support.

---

### `cb_signature_help` — parameter tooltip at call sites

Missing function: `CB_SigHelp* cb_signature_help(CB_TU*, unsigned line, unsigned col)`.

Uses `ASTUnit::CodeComplete` with kind `CCC_Other` (or invoke the clang code-completion
in a call context). Extract `OverloadCandidate` items for the function being called,
determine the active parameter index from cursor position within the argument list.

Maps to LSP `textDocument/signatureHelp`.

---

### `cb_hover_full` — full doc comment rendering

`cb_hover_markdown` returns brief comment + signature but doesn't traverse the full
`clang::comments::FullComment` AST. Missing:

- `@param` / `\param` blocks per parameter.
- `@returns` / `\returns` block.
- `@throws` / `\throws` blocks.
- `@note`, `@warning`, `@deprecated` blocks.
- Parent context (enclosing namespace/class).
- "Defined in file:line" link.

Traversal via `clang::comments::CommentVisitor<T>` in a new `DocCommentRenderer` class.

---

## Medium priority

### `cb_inlay_hints` — parameter names and deduced types

Missing function: `CB_InlayHint* cb_inlay_hints(CB_TU*, unsigned start_line,
unsigned end_line, size_t* count)`.

Two kinds:
- **Parameter names**: walk `CallExpr::arguments()`; for each unnamed argument,
  emit `paramName:` before it using `ParmVarDecl::getName()`.
- **Deduced types**: walk `VarDecl` with `isAutoDecl()` or `TypeSourceInfo` pointing
  to a deduced type; emit the fully elaborated type string.

Maps to LSP `textDocument/inlayHint`.

---

### `cb_type_at` — type string for arbitrary expression

Missing function: `const char* cb_type_at(CB_TU*, unsigned line, unsigned col)`.

Walk expressions to find the innermost `Expr` at the cursor; call
`Expr::getType().getAsString(PrintingPolicy)` and return it. Useful for hover
enrichment on variables where there's no doc comment.

---

### `cb_parse_unsaved` — parse from memory buffer

`cb_parse` currently requires a file on disk. For editor use the buffer is always
in memory and may differ from the saved file.

Add: `CB_TU* cb_parse_unsaved(const char* filename, const char* contents,
size_t contents_len, const char* const* args, int num_args)`.

Uses `ASTUnit::LoadFromCompilerInvocation` with an `llvm::MemoryBuffer` for the
main file (same path `cb_reparse` already supports via `unsaved_files`).

---

### `cb_inclusions` — `#include` graph

Missing function: `CB_Inclusion* cb_inclusions(CB_TU*, size_t* count)`.

Uses `ASTUnit::getPreprocessor().addPPCallbacks(...)` with a recording
`PPCallbacks` subclass that captures `InclusionDirective` events: including file,
included file, range of the `#include` directive.

Maps to LSP `textDocument/documentLink`.

---

### `cb_macro_at` — macro definition hover

`cb_hover_markdown` returns empty for macro use sites. Add macro detection:

- Use `Preprocessor::getMacroInfo(II)` to find the macro definition.
- Return definition location + expansion text.
- Surface via `cb_hover_markdown` or a separate `cb_macro_at` entry point.

---

## Lower priority

### `cb_semantic_tokens` — per-token classification

Missing function: `CB_Token* cb_semantic_tokens(CB_TU*, size_t* count)`.

Classify every identifier token as: namespace, type, function, method, parameter,
variable, field, enum-member, macro, keyword. Uses `clang::Preprocessor::annotateTokens`
+ `clang::Sema` token classification.

Maps to LSP `textDocument/semanticTokens/full`.

---

### `cb_format` — clang-format in-process

Missing function: `CB_Edit* cb_format(const char* source, size_t len,
const char* style_file_dir, size_t* edit_count)`.

Calls `clang::format::reformat` with `FormatStyle` loaded from the nearest
`.clang-format` file. Returns an array of text edits (range + new text).

Maps to LSP `textDocument/formatting` / `textDocument/rangeFormatting`.

---

### `cb_references` — find all usages by USR

Missing function: `CB_Location* cb_references(CB_Index*, const char* usr, size_t* count)`.

Uses `clang::index::IndexingAction` to walk a set of already-parsed TUs and collect
all `DeclOccurrence` entries matching the given USR.

Maps to LSP `textDocument/references` / `workspace/symbol`.

---

### Error message from `cb_parse`

`cb_parse` returns `nullptr` on failure with no way to retrieve an error string.

Add: `const char* cb_last_error()` returning the most recent parse error, or
store it in `CB_Index` and expose `cb_index_last_error(CB_Index*)`.

---

## Correctness / quality gaps

### Remove `SymbolLocator` copy-paste

`SymbolLocator` (the `RecursiveASTVisitor` subclass) is duplicated across
`cb_hover_markdown`, `cb_goto_definition`, and `cb_symbol_at`. Extract to a shared
static helper function (see "Fix `SymbolLocator`" above).

### Document thread-safety invariant

Add a comment on `CB_TU` (in `clang_bridge.h`) and on `TranslationUnit` (in `src/lib.rs`)
stating the invariant: a single TU must not be accessed from concurrent threads.
The Rust wrapper enforces this via `&mut self` on all mutating methods; the C API
does not.

### Stale completion after unsaved edits

`cb_complete` does not reparse the TU with the live buffer before running the code
completer. It uses whatever AST was last committed. Add an unsaved-buffer path:
`cb_complete_unsaved(CB_TU*, unsigned line, unsigned col, const char* contents, size_t len, ...)`.

### Tidy concurrency (if tidy is re-added)

If subprocess-based tidy is re-introduced: rapid saves fire concurrent tidy
processes whose results arrive out of order and overwrite each other. Fix with a
per-file generation counter — discard results if the stored generation is newer
than the one this subprocess was started for.
