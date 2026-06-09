# clang-bridge API Audit

_Date: 2026-06-08. All 47 existing tests pass._

---

## Test coverage gaps

APIs that have **zero** test coverage:

| API | Module | Notes |
|-----|--------|-------|
| `diagnostics` + fix-its | `diag.rs` | Only tested implicitly; no dedicated test for end-range or FixIt fields |
| `reparse` | `lib.rs` | `cb_transunit_reparse` — not tested at all |
| `document_highlight` | `highlight.rs` | `cb_highlight` — untested |
| `folding_ranges` | `folding.rs` | `cb_folding_ranges` — untested |
| `code_actions` | `codeaction.rs` | `cb_code_actions` — untested |
| `workspace_index_add` / `workspace_symbols` | `workspace.rs` | Both untested |
| `call_hierarchy` | `callhier.rs` | All three functions untested |
| `type_hierarchy` | `typehier.rs` | All three functions untested |
| `expand_macro` | `lib.rs` | `cb_expand_macro` — untested |
| `ast_dump` | `lib.rs` | `cb_ast_dump` — untested |

---

## Bugs

### B-1 — `cb_inlay_hints` header documents only 2 of 4 kinds

**File:** `bridge/clang_bridge.h` line 338  
**Severity:** Documentation bug; callers interpret undocumented values wrong.

The header says:
```c
uint8_t kind;   // 0 = parameter name, 1 = deduced type
```

But the bridge emits four kinds:
- `0` = parameter-name hint
- `1` = deduced-type hint (`auto`)
- `2` = block-end label (e.g. `} // longfn`)
- `3` = designator hint (aggregate init `.field =`)

**Fix:** Update the header comment to document all four kinds.

---

### B-2 — `cb_highlight` misclassifies non-definition declarations as Read

**File:** `bridge/cb/refs.cpp` line 243–245  
**Severity:** Low (kind field is wrong for declarations that aren't the definition).

Current code classifies every occurrence as either Write/Definition (3) or Read (2):
```cpp
bool is_def = (Roles & (index::SymbolRoleSet)SR::Definition) != 0;
out.push_back({..., is_def ? (uint8_t)3 : (uint8_t)2});
```

This misses `SR::Write` — a non-definition occurrence that writes a value (e.g. `x = 5`) is still
classified as Read (2) instead of Write (3).  The available SymbolRole flags include:

- `SR::Definition` — the defining declaration
- `SR::Write` — a write to the symbol (assignment, output-argument, etc.)
- `SR::Read` — (default; anything that isn't Write/Definition)

**Fix:** Use `SR::Definition | SR::Write` as the "write" condition:
```cpp
bool is_write = (Roles & ((index::SymbolRoleSet)SR::Definition |
                           (index::SymbolRoleSet)SR::Write)) != 0;
out.push_back({..., is_write ? (uint8_t)3 : (uint8_t)2});
```

---

### B-3 — `cb_inclusions` start_col / end_col are always equal

**File:** `bridge/cb/analysis.cpp` lines 48–50  
**Severity:** Medium (LSP `documentLink` range is wrong; every link points to a zero-width range).

```cpp
e.start_col = presumed.getColumn();
e.end_col   = presumed.getColumn(); // refined below if possible
```

The "refined below if possible" code was never written. `start_col` and `end_col` always point to
the start of the `#include` keyword, not the quoted path literal.

LSP requires `documentLink.range` to cover exactly the path text between the quotes (or angle
brackets).

**Fix:** The `IncludeLoc` points to the `#` character. The path literal starts after the `"` or
`<`. Use `Lexer::getRawToken` to scan forward from `incLoc` and find the string literal token,
then set `start_col` / `end_col` from that token's start and end columns.

---

### B-4 — `cb_workspace_symbols("")` returns zero results instead of all symbols

**File:** `bridge/cb/workspace.cpp` line 50 / `bridge/clang_bridge.h` line 471  
**Severity:** Medium (LSP spec says empty query = return all symbols or a capped subset).

The C++ implementation:
```cpp
CB_WorkspaceSymList *cb_workspace_symbols(CB_Index *idx, const char *query) {
    ...
    if (!query || !*query) return empty list;   // <— zero results for ""
```

The [LSP spec](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#workspace_symbol)
says: _"If the query string is empty all symbols should be returned (server's discretion)."_

**Fix:** When `query` is empty, return all symbols (capped to a reasonable limit, e.g. 500).

---

### B-5 — `cb_folding_ranges` header advertises "comment" kind that is never emitted

**File:** `bridge/clang_bridge.h` line 415  
**Severity:** Documentation bug.

```c
const char *kind;   // "region" | "comment"
```

The implementation only ever emits `"region"`.  Comment folding would require `PPCallbacks` or
`getRawCommentList()`, neither of which is wired up.

**Fix (option A):** Remove `"comment"` from the header doc comment.  
**Fix (option B):** Implement comment folding via `ASTContext::getRawCommentList()` (available on
this version of Clang — it was the earlier unavailable method `getLocalComments()`).

---

### B-6 — `hierarchy.cpp` has a dangling trailing comment

**File:** `bridge/cb/hierarchy.cpp` line 299  
**Severity:** Cosmetic.

The file ends with:
```cpp
// ── Macro expansion ───────────────────────────────────────────────────────────
```

Macro expansion is in `extra.cpp`. The comment was left over from the split and is misleading.

**Fix:** Delete the trailing comment.

---

## Quality / design issues

### Q-1 — `cb_completion` requires manual reparse before use

**File:** `bridge/clang_bridge.h` line 381  
**Status:** Already documented in the header; surfaced here for visibility.

`cb_complete` runs code-complete against the AST from the last parse. If the buffer has been edited
without a reparse, results are stale. The header notes this. The LSP server (`Clang.rs`) is
responsible for calling `reparse()` before `complete()`. No code change needed; just a reminder
to test this flow.

### Q-2 — `cb_semantic_tokens` emits tokens for both declarations and reference sites

**File:** `bridge/cb/analysis.cpp`

`SemanticTokenVisitor` visits both:
- `VisitNamedDecl` → declaration-site token
- `VisitDeclRefExpr` + `VisitMemberExpr` → use-site tokens

For a function declared once and called twice, the function name appears in the token list three
times (different positions). This is correct LSP behaviour — each occurrence gets its own token.
However, for forward-declared + defined functions the declaration position appears twice (for
the decl and the definition). This is also fine. No change needed.

### Q-3 — `cb_code_actions` only surfaces fix-its, no stock LSP actions

**File:** `bridge/cb/extra.cpp`

Only fix-it diagnostics from stored diagnostics are returned. LSP `codeAction` clients also
expect stock actions: "Add missing #include", "Extract variable", "Convert to range-based for",
etc. These require Sema-level analysis and are significant engineering work. Documented here as
a future enhancement, not a bug.

### Q-4 — `cb_call_hierarchy_prepare` does not handle lambda expressions

**File:** `bridge/cb/hierarchy.cpp` line 25

```cpp
if (!isa<FunctionDecl>(ND)) return nullptr;
```

Lambdas are not `FunctionDecl`s — they are `CXXRecordDecl`s with an `operator()`. A cursor on
a lambda capture expression will return `nullptr`. This is acceptable for the initial
implementation; future work.

### Q-5 — `cb_workspace_index_add` adds duplicate entries on reparse

**File:** `bridge/cb/workspace.cpp`

Each call to `cb_workspace_index_add` appends new entries to the multimap without removing old
ones for the same file. After `reparse()` + `workspace_index_add()`, the file's symbols appear
twice. `workspace_symbols()` results will contain duplicates.

**Fix:** Before adding symbols from a TU, remove all existing entries whose `file` field matches
the TU's main file.

---

## New tests to add

Covering all untested APIs:

| Test file | Coverage |
|-----------|----------|
| `tests/diag.rs` | Diagnostics (severity, end-range, fix-its) |
| `tests/reparse.rs` | `reparse(Some(content))` and `reparse(None)` |
| `tests/highlight.rs` | Document highlight — symbol occurrences + kinds |
| `tests/folding.rs` | Folding ranges — functions, classes, enums, namespaces |
| `tests/codeaction.rs` | Code actions from diagnostics with fix-its |
| `tests/workspace.rs` | `workspace_index_add` + `workspace_symbols` search |
| `tests/callhier.rs` | `call_hierarchy_prepare`, `incoming_calls`, `outgoing_calls` |
| `tests/typehier.rs` | `type_hierarchy_prepare`, `supertypes`, `subtypes` |
| `tests/expand_macro.rs` | `expand_macro` for object-like and function-like macros |
| `tests/ast_dump.rs` | `ast_dump` returns valid JSON with expected fields |

---

## Bugs found by the on-disk fixture (`tests/fixtures/test.cpp` + `tests/fixture_api.rs`)

Running every API against one real, multi-construct file surfaced two
correctness defects that the small per-feature snippets had missed.

### B-7 — `cb_references` / `cb_rename` emit a duplicate occurrence

**File:** `bridge/cb/refs.cpp` (`RefCollector::handleDeclOccurrence`)

The Clang indexer reports some occurrences twice (e.g. a call used to initialise
an `auto` variable is visited in two contexts). The reference list therefore
contained a duplicate `(file,line,col)`, and because `cb_rename` is built on the
same collector, rename produced two edits at one location — which would
double-apply on accept.

**Fix:** dedup by `file:line:col` inside `RefCollector` before pushing. Verified
by `references_finds_definition_and_two_calls` (now exactly 3, was 4).

### B-8 — `cb_semantic_tokens` never emitted `CB_TOK_MACRO` tokens

**File:** `bridge/cb/analysis.cpp` (macro-annotation loop in `cb_semantic_tokens`)

The loop lexed the token at each expansion's **spelling** location (the macro
*body*, e.g. `128` or `(`) instead of its **invocation** location, so the lexed
token was never the macro-name identifier and every candidate was rejected. No
macro was ever highlighted.

**Fix:** lex at `getExpansionLoc(...)` (the invocation site) to recover the macro
name, and dedup by invocation offset so nested expansions of one function-like
invocation emit a single token. Verified by `semantic_tokens_classify_each_kind`.
