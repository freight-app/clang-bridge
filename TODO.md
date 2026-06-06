# clang-bridge TODO

API gaps and quality issues. Updated after each work session.

---

## Done

- [x] **`locate_symbol_at` — reference-site resolution** (2026-06-06)
  `RefLocator` walks `DeclRefExpr`, `MemberExpr`, `TagTypeLoc`,
  `TypedefTypeLoc`, `TemplateSpecializationTypeLoc` before falling back to
  declaration-site matching. All three entry points (`cb_hover_markdown`,
  `cb_goto_definition`, `cb_symbol_at`) use the shared helper — copy-paste
  eliminated.

- [x] **`cb_goto_definition` follows `TagDecl` definitions** (2026-06-06)
  Classes and structs now resolve to their definition, not the first forward
  declaration.

- [x] **build.rs env overrides** (2026-06-06)
  `CLANG_BRIDGE_CLANGXX` and `CLANG_BRIDGE_LLVM_CONFIG` let the caller
  override the compiler and `llvm-config` binary. `libdir` derived from
  `llvm-config --libdir` instead of being hardcoded.

---

## High priority

### `cb_document_symbols` — editor outline / symbol list

Missing function: `CB_SymbolList* cb_document_symbols(CB_TU*, size_t* count)`.

Visitor collects all `NamedDecl` nodes (functions, classes, namespaces, fields,
enums, typedefs, variables) with name, kind, definition range, and selection range.
Parent/child nesting via `DeclContext` walking; each item carries a
`parent_index` (or -1 for top-level).

Maps to LSP `textDocument/documentSymbol`.

---

### Diagnostic end-ranges and fix-it hints

`CB_Diag` only has a point location. Clang diagnostics carry:
- `SourceRange` — start/end of the squiggly-underline region.
- `FixItHint` — replacement range + replacement text.

Add `end_line`, `end_col` to `CB_Diag` and a separate `CB_FixIt` array.
Expose both in `src/diag.rs`.

Enables: precise editor underlines and `textDocument/codeAction` quick-fixes.

---

### `cb_signature_help` — parameter tooltip at call sites

Missing function: `CB_SigHelp* cb_signature_help(CB_TU*, unsigned line, unsigned col)`.

Use `ASTUnit::CodeComplete` in an overload context to produce `OverloadCandidate`
items. Extract the active parameter index from the cursor's position within the
argument list.

Maps to LSP `textDocument/signatureHelp`.

---

### `cb_hover_full` — full doc comment rendering

`cb_hover_markdown` returns brief + signature only. Add full traversal of the
`clang::comments::FullComment` AST via `clang::comments::CommentVisitor<T>`:

- `@param` / `\param` blocks.
- `@returns` / `\returns` block.
- `@throws`, `@note`, `@warning`, `@deprecated`.
- Parent context (enclosing namespace / class).
- "Defined in `file:line`" footer.

---

## Medium priority

### `cb_inlay_hints` — parameter names and deduced types

Missing: `CB_InlayHint* cb_inlay_hints(CB_TU*, unsigned start_line, unsigned end_line, size_t* count)`.

- **Parameter names**: walk `CallExpr::arguments()`; emit `name:` label before
  each positional argument using `ParmVarDecl::getName()`.
- **Deduced types**: walk `VarDecl` with deduced type; emit elaborated type string
  after the variable name.

Maps to LSP `textDocument/inlayHint`.

---

### `cb_type_at` — type string for arbitrary expression

Missing: `const char* cb_type_at(CB_TU*, unsigned line, unsigned col)`.

Find the innermost `Expr` covering the cursor; call
`Expr::getType().getAsString(PrintingPolicy)`. Useful for hover on variables
with no doc comment.

---

### `cb_macro_at` — macro definition hover

`cb_hover_markdown` returns nothing for macro use sites.

- Walk `Preprocessor::getMacroInfo(II)` to find the definition.
- Return definition location + expansion text.
- Either extend `cb_hover_markdown` or expose as `cb_macro_at`.

---

### `cb_inclusions` — `#include` graph

Missing: `CB_Inclusion* cb_inclusions(CB_TU*, size_t* count)`.

Register a `PPCallbacks` subclass during parse to record `InclusionDirective`
events: including file, included file, directive source range.

Maps to LSP `textDocument/documentLink`.

---

### `cb_parse_unsaved` — parse entirely from memory

`cb_parse` requires a real file on disk; the reparse path accepts an unsaved
buffer but needs the initial parse to have already happened.

Add: `CB_TU* cb_parse_unsaved(const char* filename, const char* contents, size_t len, const char* const* args, int nargs)`.

Use `ASTUnit::LoadFromCompilerInvocation` with an `llvm::MemoryBuffer` as the
virtual main file.

---

## Lower priority

### `cb_semantic_tokens` — per-token kind map

Missing: `CB_Token* cb_semantic_tokens(CB_TU*, size_t* count)`.

Classify every identifier: namespace, type, function, method, parameter,
variable, field, enum-member, macro, keyword.

Maps to LSP `textDocument/semanticTokens/full`.

---

### `cb_format` — clang-format in-process

Missing: `CB_Edit* cb_format(const char* source, size_t len, const char* style_dir, size_t* edit_count)`.

Call `clang::format::reformat` with `FormatStyle` from the nearest `.clang-format`.
Return text-edit array (range + replacement).

Maps to LSP `textDocument/formatting` and `textDocument/rangeFormatting`.

---

### `cb_references` — all usages by USR

Missing: `CB_Location* cb_references(CB_Index*, const char* usr, size_t* count)`.

Use `clang::index::IndexingAction` across all parsed TUs to find every
`DeclOccurrence` matching the USR.

Maps to LSP `textDocument/references` and `workspace/symbol`.

---

### `cb_index_last_error` — parse failure message

`cb_parse` returns `nullptr` on failure with no error text. Store the last error
in `CB_Index`; expose `const char* cb_index_last_error(CB_Index*)`.

---

## Correctness / quality

### Thread-safety comment

Add a doc comment to `CB_TransUnit` (in `clang_bridge.h`) and to
`TranslationUnit` (in `src/lib.rs`) stating the invariant: a single TU must not
be accessed concurrently. The Rust `&mut self` API enforces this; the C API does
not.

### Stale completion on unsaved buffer

`cb_complete` runs the completer against the last-committed AST, not the
live buffer passed in `unsaved_buf`. The unsaved buffer is forwarded to the
completer but the AST is stale. Fix: reparse with the buffer before invoking
`CodeComplete`, or document clearly that callers must call `cb_reparse` first.
