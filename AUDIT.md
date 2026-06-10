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

---

## Bugs found by per-function output probing

Probing each API's *actual output* against an independently-derived expectation
(rather than just "non-empty") surfaced four more correctness defects.

### B-9 — diagnostic severity was off by one

**File:** `bridge/cb/diag.cpp` (`cb_diag_iter`)

`clang`'s `DiagnosticsEngine::Level` (`Ignored=0, Note=1, Remark=2, Warning=3,
Error=4, Fatal=5`) was cast directly into the CB severity scale (`note=0,
remark=1, warning=2, error=3, fatal=4`). Every Error was reported as Fatal and
every Note as Remark. The existing diag tests masked it by OR-ing `Error|Fatal`.

**Fix:** explicit `cb_severity_from_level()` mapping. Regression:
`diag_type_error_is_error_not_fatal`, `diag_declared_here_is_note`.

### B-10 — `cb_highlight` emitted duplicate ranges

**File:** `bridge/cb/refs.cpp` (`HighlightCollector::handleDeclOccurrence`)

Same class of bug as B-7: the indexer reports an occurrence twice (a read site
appeared twice). Deduped by `line:col`. Regression:
`highlight_dedups_and_classifies_read_write`.

### B-11 — `cb_type_at` returned None for record-typed variables

**File:** `bridge/cb/symbol.cpp` (`RefLocator::VisitCXXConstructExpr`)

For `Widget w;` the implicit default-construction `CXXConstructExpr` is located
on the *variable*; `RefLocator` matched it (the class name spans the variable
columns) and returned the constructor, shadowing the `VarDecl`. `type_at` then
saw a `CXXConstructorDecl` and returned None; hover showed the constructor.

**Fix:** skip constructions whose paren/brace range is invalid (implicit
default-init). Explicit `Foo(...)`/`Foo{...}` still resolve to the constructor.
Regression: `type_at_returns_type_for_record_variable`.

### B-12 — completion item kinds misclassified methods and destructors

**File:** `bridge/cb/completion.cpp` (`lspKindFor`)

`CXCursor_CXXMethod` mapped to `Function(3)` (should be `Method(2)`), and
destructors / conversion functions fell through to `Text(1)`. Mapped
`CXXMethod`/`Destructor`/`ConversionFunction` → `Method(2)`. Regression:
`completion_kinds_field_and_method`.

## Functions verified correct (no fix needed)

`expand_macro` (recursive nesting), `macro_hover`, `hover_full`
(class/method/field/var), `document_symbols` (ranges/selection/detail),
`goto_definition` (same-file + cross-file), `ast_dump` (functions + class
members), call hierarchy (free functions + methods), `signature_help`
(active-param progression through nested calls, multiple overloads), and inlay
hints (param, deduced type, block-end ≥10 lines, designator, and clangd-style
suppression when an argument's spelling equals the parameter name).

---

## Bugs found by deeper edge-case probing (round 2)

### B-13 — class templates produced a duplicate/mis-typed semantic token

**File:** `bridge/cb/analysis.cpp` (`semtokForDecl`, `cb_semantic_tokens`)

`semtokForDecl`'s default returned VARIABLE, so a `ClassTemplateDecl` was tagged
VARIABLE while its inner `CXXRecordDecl` was TYPE — two tokens at one location.
Template type parameters were also VARIABLE. Classify template decls by the
entity they introduce and dedup tokens sharing a (line,col).

### B-14 — `cb_inclusions` returned transitive system includes

**File:** `bridge/cb/analysis.cpp` (`collect_inclusions`)

It walked every file SLocEntry in the TU, so `#include <cstddef>` surfaced dozens
of nested system headers. documentLink is per-open-document; filter to
`SM.getFileID(incLoc) == getMainFileID()`.

### B-15 — `document_symbols` omitted constructors/destructors/operators

**File:** `bridge/cb/doc.cpp` (`DocSymVisitor::skip`)

`skip()` rejected every non-identifier name, so special members never reached the
outline though `add()` renders them safely via `getNameAsString()`. Only apply
the empty-name check to identifier names.

### B-16 — `goto_definition` did nothing on a macro

**File:** `bridge/cb/goto.cpp` (`cb_goto_definition`)

Added a macro fallback: when no AST symbol is under the cursor, resolve a live
macro and return its `#define` location (`MacroInfo::getDefinitionLoc`).

### B-17 — `hover_full` ran multi-line paragraph comments together

**File:** `bridge/cb/hover.cpp` (`commentInlineText`)

A paragraph's per-line `TextComment` children were concatenated with no
separator ("Brief line.More detail here."). Join them with a single space.

## Round 3 — clangd-oracle differential audit (B-18 … B-23)

Drove clangd 22 (same LLVM as the bridge links) over JSON-RPC against
`tests/fixtures/test.cpp` and diffed every method's output against the bridge.

### B-18 — inlay param hint for a macro arg anchored inside the macro body

**File:** `bridge/cb/inlay.cpp`

`clamp(answer, 0, MAX_ITEMS)`'s `hi:` hint landed on the `#define MAX_ITEMS 128`
line (the macro body) instead of the `MAX_ITEMS` call-site token.  `getSpellingLoc`
on a macro-arg token resolves into the macro definition; switched the hint
positions (and the viewport check) to `getFileLoc`, which clangd uses.

### B-19 — `document_symbols` leaked included-header symbols

**File:** `bridge/cb/doc.cpp` (`DocSymVisitor::skip`)

`square` (defined in shapes.h) appeared in test.cpp's outline.  documentSymbol is
per-document; added a main-file guard. clangd is main-file only.

### B-20 — `document_symbols` range end was one short (not half-open)

**File:** `bridge/cb/doc.cpp` (`add`)

`sr.getEnd()` is the *start* of the last token, so ranges ended one column early
(field `x`: 21:9 vs clangd's 21:10).  Advance with `Lexer::getLocForEndOfToken`.

### B-21 — `semantic_tokens` leaked included-header tokens

**File:** `bridge/cb/analysis.cpp` (`SemanticTokenVisitor::emitToken`)

shapes.h's `square`/params surfaced as tokens at the header's line numbers (8–9),
which the client misapplies to the open document.  Added a main-file guard.

### B-22 — `folding_ranges` missing comment blocks and statement bodies

**File:** `bridge/cb/analysis.cpp` (`FoldingVisitor`, `cb_folding_ranges`)

Bridge folded only decl/brace bodies (14 ranges vs clangd's 17).  Added a
`VisitCompoundStmt` for braced statement bodies (for/while/if; deduped against
function bodies) and a raw-lexer comment scan that merges runs of whole-line
comments and multi-line block comments (skipping trailing `///<`-style comments
after code, which clangd does not fold).  Now matches clangd's 17 exactly.

### B-23 — code completion / signature help corrupted the TranslationUnit

**File:** `bridge/cb/completion.cpp` (`cb_complete`, `cb_signature_help`)

`ASTUnit::CodeComplete` was handed the unit's *own* SourceManager/FileManager, so
the completion re-parse clobbered the cached AST's source state.  After one
`signature_help` call, every AST-visitor query (inlay, highlight, semantic
tokens, document symbols, folding) returned **nothing** — fatal for an LSP server
that reuses one TU across requests.  Fixed by running completion on a fresh
`SourceManager` (reusing the FileManager + diagnostics) exactly as libclang does.

### B-24 — `semantic_tokens` skipped all type references (and several decls)

**File:** `bridge/cb/analysis.cpp` (`SemanticTokenVisitor`, `cb_semantic_tokens`)

A position-level diff against clangd (matching token positions, ignoring the
operator/bracket/modifier/comment types the bridge doesn't model) showed 18
identifier tokens clangd emits that the bridge did not — and **zero** spurious
extras.  The bridge visited declarations and expression references but never
*type references*, so written type names got no highlighting at all.  Added:

- `VisitTypeLoc` — record / enum / typedef / template-type-parameter /
  injected-class-name references (base-class specifiers like `: Shape`, variable
  and parameter type annotations like `geo::Circle c`, template-parameter uses
  like the `T`s in `clamp`).  Builtin types (int/double) have no decl and are
  skipped, exactly as clangd does.
- `VisitCXXConstructorDecl` — the constructor's own name (which equals the class
  name, so the generic decl path skipped it) and the member references in its
  initializer list (`: radius(r)`).
- macro **definition** names (`#define MAX_ITEMS`) via `Preprocessor::macros()`;
  the existing loop only marked macro use sites.

Net: **18 missing → 3, 0 spurious extras.**  The 3 remaining are cosmetic and
left as accepted gaps: the two `auto` placeholders (clangd colours `auto` as a
deduced type; the bridge's token model is named-entity based and leaves keywords
uncoloured) and the `geo::` namespace qualifier of `geo::Circle` (clang 22's
removal of `ElaboratedType` changed qualifier traversal so the written
nested-name-specifier loc is no longer visited in this type position).

## Round-2 functions verified correct (no fix needed)

`format` (spacing edits, style-dir), type hierarchy (direct-only super/subtypes),
references (separate decl/def/call, overload-specific, field/method, deduped),
rename (`old_name_len`, edit sites), call hierarchy (recursion + multiple
callers), workspace symbol kinds, `type_at` (array/reference/auto), hover on
overloaded calls (resolves the chosen overload), completion after `::`, inlay in
range-for and array designators, and `document_symbols` nesting for nested
namespaces / enums / templates.
