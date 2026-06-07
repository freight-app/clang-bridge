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
- IH-16: `VisitTypeLoc` for `decltype(expr)` — emits `: T` after the `decltype(...)` specifier
- SL-2: `VisitCXXConstructExpr` in `RefLocator` — constructor call sites now resolve to the constructor decl
- HV-3: `prettySignature` calls `getDescribedTemplate()` so template functions/classes show `template<...>` in hover
- IH-15: Block-end hints — `addBlockEndHint` + `markBlockEnd` helpers; visitors for `FunctionDecl`, `ForStmt`, `CXXForRangeStmt`, `WhileStmt`, `SwitchStmt`, `IfStmt` (with else-chain tracking), `TagDecl`, `NamespaceDecl`; min line limit 10; kind 2 in bridge → LSP kind 4 in Clang.rs
- IH-14: Designator hints — `VisitInitListExpr` handles array (`[N]=`) and record (`.field =`) aggregates; syntactic form only; skips `DesignatedInitExpr`, unnamed bitfields, same-name suppression; bridge kind 3 → LSP kind 0 (None) in Clang.rs

---

## ✅ All known alignment items complete

`scripts/lsp_hints_compare.py` reports 28/28 hints match clangd on `examples/cpp/hello`.
