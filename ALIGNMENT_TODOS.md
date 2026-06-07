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
- IH-1 + IH-2: Structured bindings per-binding type hints; placement at identifier END (HintSide::Right)
- PP-1: PrintingPolicy cleanup (`PolishForDeclaration`, `ConstantsAsWritten`, `SuppressTemplateArgsInCXXConstructors`; removed over-verbose `SuppressScope=0`/`SuppressTagKeyword=0`)
- IH-3: Underscore-prefixed params strip leading underscores instead of skipping
- IH-4: `operator()` / functor calls allowed (only suppress non-call operators)
- IH-5: `getSpelledIdentifier` helper matching `IgnoreUnlessSpelledInSource()` + implicit `MemberExpr`
- IH-6: Setter suppression (`set*` functions where param matches field name)
- IH-7: Simple-builtin suppression (`std::move`, `std::forward`, etc.)
- IH-8: `isPrecededByParamNameComment` — skip hint when `/* paramName */` precedes arg
- IH-9: `UserDefinedLiteral` suppression in `VisitCallExpr`
- IH-10: Pack expansion guard (`if (isa<PackExpansionExpr>(arg)) break`)
- IH-11: Deduplication (sort + `std::unique` by `(line, col, label)`)
- IH-13: Deduced return-type hints — `VisitFunctionDecl` + `VisitLambdaExpr` with `getContainedAutoType`
- HV-1: `SuppressInitializers` for large variable initializers (> 200 token span)
- HV-2: `UsingShadowDecl` followed to `getTargetDecl()` in `RefLocator`
- SL-1: Constructor/destructor names handled in `DeclLocator` (`VisitNamedDecl` fallback)
- `AnonymousTagLocations = 0` in all type-hint `PrintingPolicy` instances (prevents `(lambda at file:line)` in hints)

---

## 🟢 Missing features (not wrong, just absent)

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
Fix: add `VisitTypeLoc` — `if (auto *DT = dyn_cast<DecltypeType>(TL.getType()))`
then emit `: UnderlyingType` hint at `TL.getSourceRange().getEnd()`.

---

## 🔵 Hover / symbol-lookup improvements

### HV-3 — Template decl resolution in hover
Clangd calls `D->getDescribedTemplate()` to get the template version of a
function/class and `fillFunctionTypeAndParams` for a rich type layout.
Our `prettySignature` just calls `ND->print()` which may omit template params.

### SL-2 — `RefLocator` missing `CXXConstructorExpr` reference
Constructor calls (`MyClass(...)`) are not `DeclRefExpr` or `MemberExpr`.
Need `VisitCXXConstructExpr` in `RefLocator` to find the constructor decl.

---

## Implementation order suggestion (remaining)

1. IH-16 (`decltype()` type hints via `VisitTypeLoc`) — straightforward  
2. SL-2 (`VisitCXXConstructExpr` in `RefLocator`) — constructor hover/goto  
3. HV-3 (template decl resolution in hover)  
4. IH-15 (block-end hints)  
5. IH-14 (designator hints — complex, needs clang-tidy dep)
