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

- [x] **Thread-safety comment** (2026-06-08)
  Added to `CB_TransUnit` typedef in `clang_bridge.h` and to `TranslationUnit`
  in `src/lib.rs`: a single TU must not be accessed concurrently.

- [x] **Stale completion documented** (2026-06-08)
  `cb_complete` doc comment in `clang_bridge.h` notes that callers should
  `cb_transunit_reparse` with the unsaved buffer before invoking completion.

- [x] **`cb_highlight` / `cb_folding_ranges` / `cb_code_actions`** (2026-06-08)
  Document highlight, folding regions, and fix-it code actions. Rust:
  `highlight.rs`, `folding.rs`, `codeaction.rs`.

- [x] **`cb_workspace_index_add` / `cb_workspace_symbols`** (2026-06-08)
  Name→entry index on `CB_Index`; fuzzy case-insensitive prefix search.
  Rust: `workspace.rs`.

- [x] **`cb_call_hierarchy`** (2026-06-08)
  `cb_call_hierarchy_prepare`, `cb_incoming_calls`, `cb_outgoing_calls` via
  `CallGraphVisitor`. Rust: `callhier.rs`.

- [x] **`cb_type_hierarchy`** (2026-06-08)
  `cb_type_hierarchy_prepare`, `cb_supertypes`, `cb_subtypes` via
  `CXXRecordDecl::bases()` and reverse USR scan. Rust: `typehier.rs`.

- [x] **`cb_expand_macro`** (2026-06-08)
  Recursive macro expansion up to 5 levels. Rust: `expand_macro` on
  `TranslationUnit`.

- [x] **`cb_ast_dump`** (2026-06-08)
  JSON array of named declarations in a line range. Rust: `ast_dump` on
  `TranslationUnit`.

- [x] **Bridge split into per-feature .cpp files** (2026-06-08)
  `clang_bridge.cpp` split into 13 focused files: `cb_core`, `cb_doc`,
  `cb_diag`, `cb_inlay`, `cb_symbol`, `cb_hover`, `cb_goto`, `cb_completion`,
  `cb_analysis`, `cb_refs`, `cb_workspace`, `cb_hierarchy`, `cb_extra`.
  Shared types and declarations live in `cb_internal.h`.

---

## Freight LSP — 100% coverage achieved 🎉

All LSP methods needed for freight's C/C++ language server are implemented:

| LSP Method | API |
|---|---|
| `textDocument/hover` | `cb_hover_full`, `cb_hover_markdown`, `cb_raw_comment_at`, `cb_type_at`, `cb_macro_at` |
| `textDocument/definition` | `cb_goto_definition` |
| `textDocument/completion` | `cb_complete` |
| `textDocument/publishDiagnostics` | `cb_diag_iter` + fix-its |
| `textDocument/documentSymbol` | `cb_document_symbols` |
| `textDocument/signatureHelp` | `cb_signature_help` |
| `textDocument/inlayHint` | `cb_inlay_hints` |
| `textDocument/semanticTokens/full` | `cb_semantic_tokens` |
| `textDocument/formatting` | `cb_format` |
| `textDocument/references` | `cb_references` |
| `textDocument/rename` + `prepareRename` | `cb_rename` |
| `textDocument/documentLink` | `cb_inclusions` |
| `textDocument/documentHighlight` | `cb_highlight` |
| `textDocument/foldingRange` | `cb_folding_ranges` |
| `textDocument/codeAction` | `cb_code_actions` |
| `textDocument/prepareCallHierarchy` | `cb_call_hierarchy_prepare` |
| `callHierarchy/incomingCalls` | `cb_incoming_calls` |
| `callHierarchy/outgoingCalls` | `cb_outgoing_calls` |
| `textDocument/prepareTypeHierarchy` | `cb_type_hierarchy_prepare` |
| `typeHierarchy/supertypes` | `cb_supertypes` |
| `typeHierarchy/subtypes` | `cb_subtypes` |
| `workspace/symbol` | `cb_workspace_symbols` |
| Parse from memory | `cb_parse_unsaved` |
| Parse error reporting | `cb_index_last_error` |
