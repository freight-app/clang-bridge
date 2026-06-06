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

- [x] **`cb_document_symbols`** (2026-06-06)
  `DocSymVisitor` with `parent_stack` / `decl_to_idx` for nested and
  out-of-class definitions. Rust: `docsym.rs` with `DocSymList`.

- [x] **Diagnostic end-ranges and fix-it hints** (2026-06-06)
  `CB_Diag` extended with `end_line`/`end_col`. `CB_FixIt` struct,
  `cb_diag_fixit_count`, `cb_diag_fixit_get`. Rust: `FixIt` struct in
  `diag.rs`.

- [x] **`cb_signature_help`** (2026-06-06)
  `BridgeSigHelpConsumer` via `ProcessOverloadCandidates`. Rust: `sighelp.rs`.

- [x] **`cb_hover_full`** (2026-06-06)
  Full `FullComment` AST traversal rendering `@param`/`@returns`/`@note`/
  `@warning`/`@deprecated` as Markdown plus definition-location footer.

- [x] **`cb_raw_comment_at`** (2026-06-06)
  `getRawCommentForDeclNoCache` + `getFormattedText`. Exposed in `hover.rs`.

- [x] **`cb_inlay_hints`** (2026-06-06)
  `InlayHintVisitor` emits parameter-name hints (kind=0) at call sites and
  deduced-type hints (kind=1) for `auto` variables. Rust: `inlay.rs`.

- [x] **`cb_type_at`** (2026-06-06)
  Returns `QualType::getAsString` for `VarDecl`/`FieldDecl` at cursor.
  Exposed as `hover::type_at`.

- [x] **`cb_macro_at`** (2026-06-06)
  `Preprocessor::getMacroInfo` hover: shows `#define` spelling, param list,
  expansion tokens, definition-location footer. Exposed as `hover::macro_hover`.

---

## Remaining — freight LSP

### `cb_inclusions` — `#include` graph

Missing: `CB_Inclusion* cb_inclusions(CB_TU*, size_t* count)`.

Register a `PPCallbacks` subclass during parse to record `InclusionDirective`
events: including file, included file, directive source range.

Maps to LSP `textDocument/documentLink`.

*Note: requires pre-parse registration; harder than post-parse queries.*

---

### `cb_parse_unsaved` — parse entirely from memory

`cb_parse` requires a real file on disk; the reparse path accepts an unsaved
buffer but needs the initial parse to have already happened.

Add: `CB_TU* cb_parse_unsaved(const char* filename, const char* contents, size_t len, const char* const* args, int nargs)`.

Use `ASTUnit::LoadFromCompilerInvocation` with an `llvm::MemoryBuffer` as the
virtual main file.

---

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

---

## Extra — broader API (non-freight use cases)

These extend clang-bridge into a general-purpose C/C++ static analysis library,
useful for editors, linters, documentation generators, and refactoring tools.

### `cb_rename` — safe symbol rename

`CB_RenameResult* cb_rename(CB_Index*, const char* usr, const char* new_name)`.

Collect all reference sites from `cb_references`, verify the new name doesn't
conflict with visible declarations, and return a sorted edit list.

Maps to LSP `textDocument/rename` and `textDocument/prepareRename`.

---

### `cb_call_hierarchy` — caller/callee graph

```c
CB_CallHierItem* cb_call_hierarchy_prepare(CB_TU*, uint32_t line, uint32_t col);
CB_CallEdge*     cb_incoming_calls(CB_Index*, const CB_CallHierItem*, size_t* count);
CB_CallEdge*     cb_outgoing_calls(CB_Index*, const CB_CallHierItem*, size_t* count);
```

Uses `RecursiveASTVisitor` to build the call graph for a root symbol. Useful for
impact analysis and call-chain exploration.

Maps to LSP `textDocument/prepareCallHierarchy`, `callHierarchy/incomingCalls`,
`callHierarchy/outgoingCalls`.

---

### `cb_type_hierarchy` — base/derived class chains

```c
CB_TypeHierItem* cb_type_hierarchy_prepare(CB_TU*, uint32_t line, uint32_t col);
CB_TypeHierItem* cb_supertypes(CB_Index*, const CB_TypeHierItem*, size_t* count);
CB_TypeHierItem* cb_subtypes(CB_Index*, const CB_TypeHierItem*, size_t* count);
```

Walk `CXXRecordDecl::bases()` for supertypes and scan the index for subtypes.
Maps to LSP `textDocument/prepareTypeHierarchy`.

---

### `cb_code_actions` — structured quick-fix list

`CB_CodeAction* cb_code_actions(CB_TU*, uint32_t line, uint32_t col, size_t* count)`.

- Surface fix-its from diagnostics already attached to the TU.
- Provide stock actions: extract variable, extract function, add missing `#include`.

Maps to LSP `textDocument/codeAction`.

---

### `cb_highlight` — document highlight / all usages in file

`CB_Range* cb_highlight(CB_TU*, uint32_t line, uint32_t col, size_t* count)`.

Walk the TU for all reference sites of the symbol under cursor. Cheaper than
`cb_references` because it is file-scoped. Maps to LSP
`textDocument/documentHighlight`.

---

### `cb_folding_ranges` — code folding regions

`CB_FoldingRange* cb_folding_ranges(CB_TU*, size_t* count)`.

Emit folding ranges for: function/class bodies, comment blocks, `#if`/`#endif`
preprocessor regions, brace-enclosed blocks. Maps to LSP
`textDocument/foldingRange`.

---

### `cb_workspace_symbols` — fuzzy symbol search

`CB_WorkspaceSymbol* cb_workspace_symbols(CB_Index*, const char* query, size_t* count)`.

Maintain a name→USR index built from all parsed TUs; support fuzzy matching via
a simple trigram or prefix index. Maps to LSP `workspace/symbol`.

---

### `cb_expand_macro` — show full macro expansion

`char* cb_expand_macro(CB_TU*, uint32_t line, uint32_t col)`.

Walk `Preprocessor`'s expansion chain for the token at the cursor, render each
expansion step. Useful as a hover enrichment or standalone linter output.

---

### `cb_ast_dump` — raw AST as JSON

`char* cb_ast_dump(CB_TU*, uint32_t start_line, uint32_t end_line)`.

Serialize the AST subtree for a line range as JSON (node kind, type, range,
children). Useful for debugging, external analysis tools, and tree-sitter parity
checks.
