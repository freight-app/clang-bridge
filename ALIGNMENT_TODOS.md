# clang-bridge / clangd alignment TODOs

Reference: `clang-tools-extra/clangd/InlayHints.cpp`, `Hover.cpp`, `InlayHints.h`.
Sorted by impact (visible artifacts first, missing features last).

---

## ✅ Done

- `inViewport` main-file guard (no hints from included headers)
- `VisitCXXConstructExpr` for constructor call parameter hints
- `prettySignature`: `TerseOutput = 1` (no function bodies in hover)
- `DeclLocator`: precise column span, `shouldVisitTemplateInstantiations=false`, implicit/system-header guards
- `locate_symbol_at`: identifier-token guard before symbol lookup

---

## 🔴 Correctness bugs (wrong output)

### IH-1 — Structured bindings emitting wrong hint
`auto [m, v] = tada` should produce `: double` after each binding name, not
`: std::pair<double,double>` before the `[`.
Clangd: `if (auto *DD = dyn_cast<DecompositionDecl>(D))` → per-`BindingDecl` hints using canonical type.
Fix: handle `DecompositionDecl` at the top of `VisitVarDecl`, iterate `DD->bindings()`.

### IH-2 — Type hint placed at identifier START, should be at END
Clangd emits type hints at `HintSide::Right` (end of token range).
We emit at `p.getColumn()` = start of identifier.
Fix: `h.col = p.getColumn() + name.size()` and same for binding decls.

### IH-3 — Underscore-prefixed params: skip vs. strip
We skip hints entirely for any param starting with `_`.
Clangd strips ALL leading underscores and shows the stripped name:
`__value → value:`, `_Tp → Tp:`.
Fix: `while (pname.starts_with('_')) pname = pname.drop_front(1)` in both
`VisitCallExpr` and `VisitCXXConstructExpr`.

### IH-4 — `operator()` / functor calls suppressed
We `return true` for ALL `CXXOperatorCallExpr`.
Clangd only suppresses non-call operators; `operator()` (functors, lambdas) still gets hints.
Fix: allow when `E->getOperator() == OO_Call`.

### IH-5 — `getSpelledIdentifier` arg-matches param
We use `arg->IgnoreParenImpCasts()` and only check `DeclRefExpr`.
Clangd uses `IgnoreUnlessSpelledInSource()` and also matches unqualified
`MemberExpr` with implicit access (`ME->isImplicitAccess() && !ME->getQualifier()`).
Fix: add `getSpelledIdentifier` helper mirroring clangd's version.

### PP-1 — `prettySignature` PrintingPolicy over-verbose
We explicitly set `SuppressScope = 0` and `SuppressTagKeyword = 0`.
In C++ the base printing policy defaults to `SuppressTagKeyword = true`.
Our explicit 0 re-enables tag keywords (`struct std::pair` instead of `std::pair`).
Fix: remove those two lines; add `PolishForDeclaration = 1`,
`ConstantsAsWritten = 1`, `SuppressTemplateArgsInCXXConstructors = 1`
(matching clangd's `getPrintingPolicy`).

---

## 🟡 Logic gaps (hints shown when they shouldn't be)

### IH-6 — No setter suppression
Clangd suppresses hints for single-param functions named `set*` where
the part after "set" matches the param name (e.g. `setFoo(foo)` → no hint).
Fix: `isSetter(FD, stripped_param_name)` check before the arg loop.

### IH-7 — No simple-builtin suppression
Clangd suppresses hints for `std::move`, `std::forward`, `std::addressof`,
`std::as_const`, `std::move_if_noexcept` via `FD->getBuiltinID()`.
Fix: add `isSimpleBuiltin(FD)` switch on `Builtin::BI*` IDs.

### IH-8 — No `/*paramName*/` comment suppression
Clangd skips a hint when the argument is immediately preceded by a C block
comment whose content matches the parameter name.
Fix: scan the raw source buffer backwards from the arg's begin location.

### IH-9 — `UserDefinedLiteral` not suppressed
Clangd skips `isa<UserDefinedLiteral>(E)`. We don't.
Fix: add the guard to `VisitCallExpr`.

### IH-10 — No pack expansion guard
When an argument is a `PackExpansionExpr`, the 1:1 arg↔param mapping breaks.
Clangd breaks out of the loop at that point.
Fix: `if (isa<PackExpansionExpr>(arg)) break;`

---

## 🟢 Missing features (not wrong, just absent)

### IH-11 — No deduplication
Clangd sorts and deduplicates hints after collection (explicit template
instantiations can produce duplicates).
Fix: sort + `std::unique` by `(line, col, label)` in `cb_inlay_hints`.

### IH-12 — No structured-binding per-binding type hints  
(Covered by IH-1 above — same fix.)

### IH-13 — No deduced return-type hints
Clangd adds `-> T` hints when `auto foo()` deduces a return type, and for
lambdas without explicit result types.
Fix: add `VisitFunctionDecl` + `VisitLambdaExpr` checking `getContainedAutoType`.

### IH-14 — No designator hints
Clangd emits `.field =` before each element of an undesignated aggregate init.
Fix: `VisitInitListExpr` + `VisitCXXParenListInitExpr` using
`tidy::utils::getUnwrittenDesignators` (requires clang-tidy dep).

### IH-15 — No block-end hints
Clangd emits `// varname` after closing `}` of long for/while/if/class/namespace blocks.
Fix: `VisitForStmt`, `VisitWhileStmt`, `VisitIfStmt`, `VisitFunctionDecl`,
`VisitTagDecl`, `VisitNamespaceDecl`.

### IH-16 — No `decltype()` type hints
Clangd's `VisitTypeLoc` catches `DecltypeType` and emits the underlying type.

---

## 🔵 Hover / symbol-lookup improvements

### HV-1 — `SuppressInitializers` for large variable initializers
Clangd checks if the init expression is > 200 expanded tokens and sets
`PP.SuppressInitializers = true`. Prevents `const char *x = "very long..."`
from polluting the hover card.

### HV-2 — `using` declarations not followed
When cursor is on a `using X = Y` alias or `using ns::foo`, clangd resolves
to the underlying declaration for comments/signatures.

### HV-3 — Template decl resolution in hover
Clangd calls `D->getDescribedTemplate()` to get the template version of a
function/class and `fillFunctionTypeAndParams` for a rich type layout.
Our `prettySignature` just calls `ND->print()` which may omit template params.

### SL-1 — Constructor/destructor names missed by `DeclLocator`
`safeDeclName` returns empty for non-identifier names (ctors, dtors, operators),
so `DeclLocator` can't match them. Hovering on `MyClass(` finds nothing.
Fix: special-case `CXXConstructorDecl` / `CXXDestructorDecl` / `CXXConversionDecl`
with a separate name-span computation.

### SL-2 — `RefLocator` missing `CXXConstructorExpr` reference
Constructor calls (`MyClass(...)`) are not `DeclRefExpr` or `MemberExpr`.
Need `VisitCXXConstructExpr` in `RefLocator` to find the constructor decl.

---

## Implementation order suggestion

1. IH-1 + IH-2 (structured bindings + position) — visible wrong output today  
2. PP-1 (PrintingPolicy cleanup) — affects every hover card  
3. IH-3 + IH-4 + IH-5 (underscore / functor / getSpelledIdentifier)  
4. IH-6 + IH-7 + IH-9 + IH-10 (suppression guards)  
5. IH-11 (deduplication — cheap)  
6. SL-1 + SL-2 (constructor hover/goto)  
7. IH-13 (return-type hints)  
8. HV-1 + HV-2 + HV-3 (hover polish)  
9. IH-14 + IH-15 + IH-16 (advanced hint kinds)  
