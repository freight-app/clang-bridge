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

---

## Remaining — the road to default-on

**End goal:** the bridge becomes the *default* C/C++ backend in `freight lsp`
(clangd demoted to escape hatch). Gate: every method differentially verified
against clangd with zero unexplained regressions, **and** typing latency on a
real project is competitive with clangd.

API coverage is done (table above). What's left falls into four buckets:
known-broken things, unverified things, performance/UX changes, and missing
features. Roughly in priority order:

---

### 1. Still broken / known risks

- [x] **Precompiled-preamble reuse** (2026-07-17) — `core.cpp` now passes
      `PrecompilePreambleAfterNParses=1` to
      `ASTUnit::LoadFromCompilerInvocation`, so reparses can reuse included
      headers instead of rebuilding the whole TU. `tests/preamble.rs` verifies
      that editing the first `#include` invalidates and rebuilds the preamble,
      and that completion after repeated reparse does not reintroduce B-23's
      cached-SourceManager corruption. The explicit `<iostream>/<vector>` debug
      probe measured initial parse 606 ms and reparse p50/p95 60/70 ms.
- [ ] **Synchronous reparse on the LSP loop.** `freight/src/lsp/mod.rs`
      reparses on *every* `didChange` (full-text sync, no debounce) before
      handling the next message — fast typists queue up parses and every
      other request (hover, completion) waits behind them. **Fix (freight
      side):** debounce (~150 ms) + drop stale reparses when a newer text
      version is queued; longer-term move parsing to a worker thread with
      version-checked cancellation (TU is single-thread-only — keep one
      worker per TU).
- [x] **Concurrent initial-parse working-directory safety** (2026-07-17) —
      `ClangTool::run` temporarily changes CWD to each compilation command's
      directory, so independent parses could resolve relative paths against the
      wrong project or fail while restoring a removed temporary directory.
      `core.cpp` now guards the process-global operation with a static mutex;
      AST queries and reparses remain independent. Unsaved parses also use
      LLVM's atomic temporary-file creation instead of a colliding virtual-path
      hash. `tests/concurrent_parse.rs` starts synchronized parses in two
      projects with conflicting relative `config.hpp` files and synchronized
      unsaved parses sharing one virtual path, then verifies every TU remains
      isolated.
- [ ] **In-process crash = whole-server crash.** A clang assertion or
      segfault on malformed input takes down `freight lsp` *for all
      languages* — clangd being a subprocess means an editor only loses one
      server. **Fix:** decide a containment strategy before default-on:
      (a) catch fatal LLVM errors via `llvm::install_fatal_error_handler` +
      longjmp-free unwind to an error return, (b) optional out-of-process
      bridge mode for hardening, or at minimum (c) crash-telemetry +
      auto-restart guidance in the freight LSP supervisor.
- [x] **UTF-16 position encoding** (2026-07-17) — LSP columns are UTF-16 code
      units, while clang emits/consumes byte columns. Shared helpers in
      `symbol.cpp` now handle both boundaries: `utf16_to_byte_col` and
      `translate_line_col_utf16` for incoming positions, plus
      `source_location_utf16_col` and `utf16_length` for outgoing positions and
      lengths.
      - **Incoming** — every cursor position is converted at the boundary:
        `locate_symbol_at` converts `col` once up front (covers hover,
        references, rename, call/type hierarchy, symbol_at), and the
        direct-translate fallbacks in `goto.cpp`/`inlay.cpp`/`extra.cpp` use
        `translate_line_col_utf16`. So clicking a symbol on a multi-byte line
        now lands on the right token. Test:
        `symbol_lookup::symbol_at_uses_utf16_columns`.
      - **Outgoing** — definitions, references, rename edits, document and
        workspace symbols, diagnostics/fix-its, code actions, AST dump,
        inclusion ranges, inlay hints, and call/type hierarchy results emit
        UTF-16 columns. Rename lengths, semantic-token lengths, and macro/header
        spans are UTF-16 code units.
      - **Semantic tokens** — `emitToken` emits UTF-16 col + length. Test:
        `semtok::semantic_token_columns_are_utf16`.
      - Regression coverage: `tests/utf16_positions.rs` exercises navigation,
        symbols, hierarchy, inlay hints, diagnostics/fix-its, AST output, and
        Unicode inclusion ranges with BMP and surrogate-pair characters.
- [x] **Diagnostics from headers are anchored in the main file** (2026-07-17).
      `CB_Diag` preserves the original header range and exposes the outermost
      main-file include anchor, including through loaded preambles and nested
      include chains. Freight publishes `In included file: ...` on the direct
      include path and adds the original header range as `relatedInformation`.
      Regressions cover both the bridge contract and the Freight LSP boundary.
- [ ] **Semantic-token cosmetic gaps** (accepted in B-24, revisit before
      default-on): `auto` not coloured as deduced type; `geo::`-style
      nested-name-specifier qualifiers unvisited since clang 22 dropped
      `ElaboratedType`.
- [x] **Q-4: call hierarchy on lambdas** (2026-07-17) — hierarchy preparation
      maps lambda-typed variables and their invocation references to the
      closure's `operator()` USR while preserving the variable's user-facing
      name and source location. Generic-lambda specializations normalize to the
      primary call operator. Incoming invocation and outgoing body calls for a
      captured generic lambda are covered in `tests/callhier.rs`.

### 2. Differential verification vs clangd (the default-on gate)

Reuse the clangd JSON-RPC oracle harness from the 2026-06-10 audit
(`/tmp/clangd_probe.py` pattern: initialize → didOpen → query → diff):

- [ ] **Diagnostics** — message/range/severity/relatedInformation. clangd
      publishes these *asynchronously* after didOpen: the harness must pump the
      raw fd (Python's buffered stream deadlocks).
- [ ] **Signature help** — active-parameter tracking through nested/partial calls.
- [ ] **Hover** — content + range parity (markdown shape may differ; diff the
      facts: signature, doc, type).
- [ ] **Call/type hierarchy** — edge sets on the fixture hierarchy.
- [ ] **Completion** — item kinds/details/sort order on member access and `::`.
- [ ] **Formatting** — output equality under the same `.clang-format`.
- [ ] **Cross-file / multi-TU** — references/workspace symbols spanning TUs via
      `cb_workspace_index_add`; current fixtures only cross into `shapes.h`.
- [ ] **Soak test** — drive both servers with a recorded real editing session
      (the `scripts/lsp_hints_compare.py` approach generalised) and diff every
      response, not just single-shot queries.

### 3. User-experience changes (latency + ergonomics)

Beyond the reparse items in §1:

- [x] **Completion latency budget** (2026-07-17) — the ignored timing probe in
      `tests/preamble.rs` runs ten reparses and completions on a
      `<iostream>/<vector>` TU. On this LLVM 22 debug build, completion measured
      p50/p95 7.5/8.1 ms, below the 100 ms target. Re-run the ignored
      `iostream_vector_reparse_and_completion_latency` test with `--nocapture`
      to collect local figures.
- [ ] **TU memory cap.** `Clang.rs` keeps one live `ASTUnit` per opened file,
      evicted only on `didClose`. A long session over many files holds
      hundreds of MB. Add an LRU (keep N hottest TUs, e.g. 4–8; re-parse on
      revisit) — clangd's `--background-index` + idle-AST limit is the model.
- [ ] **Goto-definition result range is a zero-width point** (start == end).
      Editors highlight nothing on landing. Return the full identifier token
      range (use `Lexer::getLocForEndOfToken`, as document_symbols already does).
- [ ] **Progress + status surfacing.** Long first-parses are invisible — the
      editor just shows nothing. Emit `$/progress` (work-done) from freight
      around initial parse / refresh_flags, and a status-bar state
      (parsing/idle/error) the VS Code extension can show.
- [ ] **Stale-flag refresh.** `refresh_flags` clears *all* TUs whenever
      compile_commands changes → every open file re-parses at once. Only
      evict TUs whose flags actually changed.

### 4. Missing features worth adding (clangd has them, users notice)

- [ ] **Stock code actions** (Q-3): today only diagnostic fix-its surface.
      Highest-value additions, in order: "add missing `#include`" (pairs
      naturally with freight's include-hygiene classification — it knows the
      owning package), "organize/sort includes", "expand `auto`",
      "swap if branches", "extract variable/function" (Sema-heavy; last).
- [ ] **Completion: auto-insert the `#include`** for symbols completed from
      headers not yet included (clangd's include-insertion). Freight angle:
      restrict candidates to *declared* packages — this is the include-hygiene
      story applied to completion, and would be *better* than clangd.
- [ ] **Snippet completions** — function completions currently insert the bare
      name; emit LSP snippet format with `($1)` placeholders + signature
      in the detail, honouring the client's `snippetSupport` capability.
- [ ] **`semanticTokens/range` + `/full/delta`** — VS Code requests range
      tokens for the viewport first; without it, large files colour late.
      Delta avoids re-sending the full token list per keystroke.
- [ ] **Inactive-region greying** — clangd's `textDocument/inactiveRegions`
      extension (preprocessor-disabled blocks). The preprocessor data is
      already available; the VS Code extension needs the client half.
- [ ] **`textDocument/selectionRange`** (smart expand-selection) and
      **`linkedEditingRange`** — cheap AST walks, high perceived polish.
- [ ] **`onTypeFormatting`** for `}`/`;`/newline via clang-format's
      incremental mode.
- [ ] **Background project index** — references/rename/workspace-symbols are
      currently scoped to *opened* TUs plus `cb_workspace_index_add` calls.
      Index all manifest sources idle-time (freight knows the exact file set —
      no compile_commands guessing like clangd) so rename is truly
      project-wide on first use.
- [x] **IH-14: aggregate-designator inlay hints** (2026-07-17) — semantic-form
      recovery now emits clangd-style `.field=` and `[N]=` labels for records,
      arrays, inherited aggregates, and brace-elided nested subobjects. It
      suppresses written designators and matching comments, skips idiomatic
      zero initializers and non-designatable bases, and supports C++20
      parenthesized aggregate initialization. Regressions cover all of these
      mapping boundaries.

### 5. Test debt (from the audit rounds)

- [x] Position-encoding fixture (multi-byte UTF-8) —
      `tests/utf16_positions.rs`, `tests/semtok.rs`, and
      `tests/symbol_lookup.rs` cover incoming and outgoing positions with BMP
      and surrogate-pair characters.
- [ ] Multi-TU workspace fixture (two `.cpp` + shared header) covering
      cross-TU references, rename, and workspace symbols after reparse
      (Q-5 regression: no duplicate entries).
- [x] Preamble-reuse regression tests — `tests/preamble.rs` covers an edit to
      the first `#include` and completion-after-reparse without AST corruption.
- [x] Latency micro-bench — the ignored `tests/preamble.rs` timing probe reports
      initial parse and reparse/completion p50/p95 on a
      `<vector>`/`<iostream>` TU without imposing machine-dependent assertions.
