#include "cb_internal.h"

// ── Inlay hints ───────────────────────────────────────────────────────────────

struct InlayHintEntry {
    uint32_t    line, col; // 1-based; hint is displayed BEFORE this position
    std::string label;
    uint8_t     kind;      // 0 = parameter name, 1 = deduced type
};

struct CB_InlayHintList {
    std::vector<InlayHintEntry> entries;
    InlayHintEntry current; // stable storage for getter return values
};

class InlayHintVisitor : public RecursiveASTVisitor<InlayHintVisitor> {
public:
    ASTContext          &Ctx;
    const SourceManager &SM;
    std::vector<InlayHintEntry> &hints;
    uint32_t start_line, end_line;

    // Raw source buffer for the main file — used by isPrecededByParamNameComment.
    // Non-owning view; valid for the lifetime of the ASTUnit.
    StringRef MainFileBuf;

    InlayHintVisitor(ASTContext &C, std::vector<InlayHintEntry> &out,
                     uint32_t sl, uint32_t el)
        : Ctx(C), SM(C.getSourceManager()), hints(out),
          start_line(sl), end_line(el) {
        bool Invalid = false;
        StringRef Buf = SM.getBufferData(SM.getMainFileID(), &Invalid);
        MainFileBuf = Invalid ? StringRef{} : Buf;
    }

    bool shouldVisitTemplateInstantiations() const { return false; }

    // ── Clangd-aligned helpers ────────────────────────────────────────────────

    // Returns true for `expr.operator()` and lambda calls (functor pattern).
    static bool isFunctionObjectCallExpr(CallExpr *E) {
        if (auto *OC = dyn_cast<CXXOperatorCallExpr>(E))
            return OC->getOperator() == OO_Call;
        return false;
    }

    // If E is a single unqualified identifier (or implicit member access),
    // return its name — used to suppress hints when arg name == param name.
    // Mirrors clangd's getSpelledIdentifier in InlayHints.cpp.
    static StringRef getSpelledIdentifier(const Expr *E) {
        E = E->IgnoreUnlessSpelledInSource();
        if (auto *DRE = dyn_cast<DeclRefExpr>(E))
            if (!DRE->getQualifier())
                return safeDeclName(DRE->getDecl());
        if (auto *ME = dyn_cast<MemberExpr>(E))
            if (!ME->getQualifier() && ME->isImplicitAccess())
                return safeDeclName(ME->getMemberDecl());
        return {};
    }

    // Suppress hints for std::move / forward / addressof / as_const /
    // move_if_noexcept — their parameter names add no information.
    static bool isSimpleBuiltin(const FunctionDecl *FD) {
        switch (FD->getBuiltinID()) {
            case Builtin::BIaddressof:
            case Builtin::BIas_const:
            case Builtin::BIforward:
            case Builtin::BImove:
            case Builtin::BImove_if_noexcept:
                return true;
            default:
                return false;
        }
    }

    // Suppress the hint when the argument is immediately preceded by a block
    // comment whose content matches the parameter name.
    // Pattern: `/* [=. ]* <paramName> [=. ]* */` right before the expression.
    // Mirrors clangd's isPrecededByParamNameComment in InlayHints.cpp.
    bool isPrecededByParamNameComment(const Expr *E, StringRef ParamName) const {
        if (MainFileBuf.empty()) return false;
        SourceLocation FileLoc = SM.getFileLoc(E->getBeginLoc());
        auto [FID, Offset] = SM.getDecomposedLoc(FileLoc);
        if (FID != SM.getMainFileID()) return false;
        StringRef Before = MainFileBuf.substr(0, Offset).rtrim();
        if (!Before.consume_back("*/")) return false;
        llvm::StringLiteral Ignore = " =.";
        Before     = Before.rtrim(Ignore);
        ParamName  = ParamName.trim(Ignore);
        if (!Before.consume_back(ParamName)) return false;
        Before = Before.rtrim(Ignore);
        return Before.ends_with("/*");
    }

    // Suppress hints for single-arg functions named set* where the part after
    // "set" matches the (stripped) parameter name.  E.g. setFoo(foo) → no hint.
    static bool isSetter(const FunctionDecl *FD, StringRef pname) {
        if (FD->getNumParams() != 1 || pname.empty()) return false;
        StringRef fname = safeDeclName(FD);
        if (!fname.starts_with_insensitive("set")) return false;
        return fname.drop_front(3).ltrim("_").equals_insensitive(pname);
    }

    bool inViewport(SourceLocation loc) const {
        loc = SM.getSpellingLoc(loc);
        // Only emit hints for tokens that live in the main translation unit file.
        // Without this guard, hints from included headers (e.g. vec2.h) whose
        // line numbers happen to fall within [start_line, end_line] leak through.
        if (SM.getFileID(loc) != SM.getMainFileID()) return false;
        auto p = SM.getPresumedLoc(loc);
        if (!p.isValid()) return false;
        return p.getLine() >= start_line && p.getLine() <= end_line;
    }

    // ── Parameter name hints at call sites ────────────────────────────────────

    bool VisitCallExpr(CallExpr *E) {
        // Skip operator calls, but allow functor/lambda operator() calls.
        if (isa<CXXOperatorCallExpr>(E) && !isFunctionObjectCallExpr(E))
            return true;
        // User-defined literals carry no useful parameter info.
        if (isa<UserDefinedLiteral>(E)) return true;

        const FunctionDecl *FD = E->getDirectCallee();
        if (!FD) return true;
        if (isSimpleBuiltin(FD)) return true;

        // Build stripped param names up front for the setter check.
        // Strip ALL leading underscores (clangd strips them rather than skipping).
        SmallVector<StringRef, 8> pnames;
        for (const ParmVarDecl *PD : FD->parameters()) {
            StringRef n = PD->getName();
            n = n.ltrim("_");
            pnames.push_back(n);
        }
        if (!pnames.empty() && isSetter(FD, pnames[0])) return true;

        // For functor/lambda operator() calls the first argument is the implicit
        // object (the functor/lambda variable itself); skip it so param[0] aligns
        // with the first user-supplied argument, matching clangd's behaviour.
        bool skipObject = isFunctionObjectCallExpr(E);
        unsigned i = 0;
        for (const Expr *arg : E->arguments()) {
            if (skipObject) { skipObject = false; continue; }
            // Pack-expansion breaks the 1:1 arg↔param mapping; stop here.
            if (isa<PackExpansionExpr>(arg)) break;
            if (i >= pnames.size()) break;
            StringRef pname = pnames[i++];
            if (pname.empty()) continue;
            if (!inViewport(arg->getBeginLoc())) continue;

            // Suppress when argument is spelled identically to the param name.
            if (getSpelledIdentifier(arg) == pname) continue;
            // Suppress when a `/* paramName */` comment precedes the argument.
            if (isPrecededByParamNameComment(arg, pname)) continue;

            auto p = SM.getPresumedLoc(SM.getSpellingLoc(arg->getBeginLoc()));
            if (!p.isValid()) continue;
            InlayHintEntry h;
            h.line  = p.getLine();
            h.col   = p.getColumn();
            h.label = (pname + ":").str();
            h.kind  = 0;
            hints.push_back(std::move(h));
        }
        return true;
    }

    // ── Parameter name hints at constructor call sites ────────────────────────
    // VisitCallExpr only covers CallExpr nodes; CXXConstructExpr is a separate
    // AST node type.  Pattern follows clangd's VisitCXXConstructExpr / processCall.

    bool VisitCXXConstructExpr(CXXConstructExpr *E) {
        if (!E->getParenOrBraceRange().isValid()) return true;
        if (E->isStdInitListInitialization()) return true;

        const CXXConstructorDecl *CD = E->getConstructor();
        if (!CD) return true;
        if (CD->isCopyOrMoveConstructor()) return true;

        unsigned i = 0;
        for (const Expr *arg : E->arguments()) {
            if (isa<PackExpansionExpr>(arg)) break;
            if (i >= CD->getNumParams()) break;
            StringRef pname = CD->getParamDecl(i++)->getName().ltrim("_");
            if (pname.empty()) continue;
            if (!inViewport(arg->getBeginLoc())) continue;
            if (getSpelledIdentifier(arg) == pname) continue;
            if (isPrecededByParamNameComment(arg, pname)) continue;

            auto p = SM.getPresumedLoc(SM.getSpellingLoc(arg->getBeginLoc()));
            if (!p.isValid()) continue;
            InlayHintEntry h;
            h.line  = p.getLine();
            h.col   = p.getColumn();
            h.label = (pname + ":").str();
            h.kind  = 0;
            hints.push_back(std::move(h));
        }
        return true;
    }

    // ── Designator hints for aggregate initializer lists (IH-14) ────────────────
    // For undesignated brace-init-lists, emit `.field =` (record) or `[N]=`
    // (array) before each element, matching clangd's VisitInitListExpr behaviour.

    bool VisitInitListExpr(InitListExpr *ILE) {
        // Process only the syntactic (user-written) form; the semantic form has
        // Clang-synthesised padding/base-class slots that don't map to user text.
        if (!ILE->isSyntacticForm()) return true;
        unsigned nInits = ILE->getNumInits();
        if (nInits == 0) return true;

        QualType ILEType = ILE->getType().getCanonicalType();

        // ── Array aggregate: [0]=, [1]=, … ────────────────────────────────────
        if (ILEType->isArrayType()) {
            for (unsigned i = 0; i < nInits; ++i) {
                const Expr *init = ILE->getInit(i);
                if (isa<DesignatedInitExpr>(init)) continue;
                SourceLocation loc = SM.getSpellingLoc(init->getBeginLoc());
                if (!inViewport(loc)) continue;
                auto p = SM.getPresumedLoc(loc);
                if (!p.isValid()) continue;
                InlayHintEntry h;
                h.line  = p.getLine();
                h.col   = p.getColumn();
                h.label = "[" + std::to_string(i) + "]=";
                h.kind  = 3;
                hints.push_back(std::move(h));
            }
            return true;
        }

        // ── Record aggregate: .fieldName = … ──────────────────────────────────
        const RecordDecl *RD = nullptr;
        if (const auto *RT = ILEType->getAs<RecordType>())
            RD = RT->getDecl();
        if (!RD) return true;
        // Only aggregate class/struct types get designator hints.
        if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD))
            if (!CRD->isAggregate()) return true;

        auto field_it = RD->field_begin();
        for (unsigned i = 0; i < nInits; ++i) {
            // Advance past anonymous/unnamed bitfields — they're not user-visible.
            while (field_it != RD->field_end() &&
                   (field_it->isUnnamedBitField() || field_it->getName().empty()))
                ++field_it;
            if (field_it == RD->field_end()) break;

            const Expr *init = ILE->getInit(i);
            if (isa<DesignatedInitExpr>(init)) { ++field_it; continue; }

            StringRef fieldName = field_it->getName();
            // Suppress when the init value is spelled the same as the field name,
            // matching clangd's getSpelledIdentifier-based suppression.
            if (!fieldName.empty() && getSpelledIdentifier(init) == fieldName) {
                ++field_it; continue;
            }

            SourceLocation loc = SM.getSpellingLoc(init->getBeginLoc());
            if (!inViewport(loc)) { ++field_it; continue; }
            auto p = SM.getPresumedLoc(loc);
            if (!p.isValid()) { ++field_it; continue; }

            InlayHintEntry h;
            h.line  = p.getLine();
            h.col   = p.getColumn();
            h.label = "." + fieldName.str() + " =";
            h.kind  = 3;
            hints.push_back(std::move(h));
            ++field_it;
        }
        return true;
    }

    // ── Deduced return-type hints ─────────────────────────────────────────────
    // Emit `-> T` after the closing `)` of a function whose return type is
    // deduced (written as `auto`) and has no explicit trailing return.
    // Pattern follows clangd VisitFunctionDecl / addReturnTypeHint.

    void addReturnTypeHint(FunctionDecl *D, SourceLocation anchorLoc) {
        const AutoType *AT = D->getReturnType()->getContainedAutoType();
        if (!AT || AT->getDeducedType().isNull()) return;
        if (!inViewport(anchorLoc)) return;
        QualType retType = D->getReturnType();
        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressScope = 1;
        std::string typeStr = retType.getAsString(PP);
        if (typeStr == "auto" || typeStr.empty()) return;
        auto p = SM.getPresumedLoc(anchorLoc);
        if (!p.isValid()) return;
        InlayHintEntry h;
        h.line  = p.getLine();
        h.col   = p.getColumn() + 1; // one past the `)` or `]`
        h.label = "-> " + typeStr;
        h.kind  = 1;
        hints.push_back(std::move(h));
    }

    bool VisitFunctionDecl(FunctionDecl *D) {
        if (D->isImplicit()) return true;
        // Return-type hint (non-trailing deduced only).
        auto *FPT = dyn_cast<FunctionProtoType>(D->getType().getTypePtr());
        if (!(FPT && FPT->hasTrailingReturn())) {
            if (auto FTL = D->getFunctionTypeLoc())
                addReturnTypeHint(D, FTL.getRParenLoc());
        }
        // Block-end hint for function definitions.
        if (D->isThisDeclarationADefinition()) {
            if (const Stmt *body = D->getBody())
                addBlockEndHint(body->getSourceRange(), "", blockNameForFn(D), "");
        }
        return true;
    }

    bool VisitLambdaExpr(LambdaExpr *E) {
        FunctionDecl *D = E->getCallOperator();
        if (!D || E->hasExplicitResultType()) return true;
        SourceLocation anchor;
        if (!E->hasExplicitParameters())
            anchor = E->getIntroducerRange().getEnd(); // after `]`
        else if (auto FTL = D->getFunctionTypeLoc())
            anchor = FTL.getRParenLoc();               // after `)`
        if (anchor.isValid())
            addReturnTypeHint(D, anchor);
        return true;
    }

    // ── Block-end hints ──────────────────────────────────────────────────────────
    // Emit ` // label` after closing `}` of blocks spanning ≥10 lines.
    // Pattern follows clangd VisitForStmt / VisitTagDecl / VisitNamespaceDecl.

    static constexpr unsigned kBlockEndMinLines = 10;
    llvm::DenseSet<const IfStmt *> elseIfSet;

    static std::string blockNameForFn(const FunctionDecl *D) {
        DeclarationName DN = D->getDeclName();
        if (DN.isIdentifier())
            return DN.getAsIdentifierInfo()->getName().str();
        if (auto *CD = dyn_cast<CXXConstructorDecl>(D))
            return CD->getParent()->getName().str();
        if (auto *DD = dyn_cast<CXXDestructorDecl>(D))
            return ("~" + DD->getParent()->getName()).str();
        std::string s;
        llvm::raw_string_ostream os(s);
        DN.print(os, PrintingPolicy(D->getASTContext().getLangOpts()));
        return s;
    }

    static std::string summarizeCondition(const Expr *E) {
        if (!E) return "";
        E = E->IgnoreImplicit();
        if (auto *DRE = dyn_cast<DeclRefExpr>(E))
            return safeDeclName(DRE->getFoundDecl()).str();
        if (auto *ME = dyn_cast<MemberExpr>(E))
            return safeDeclName(ME->getMemberDecl()).str();
        if (auto *CE = dyn_cast<CallExpr>(E)) {
            std::string s = summarizeCondition(CE->getCallee());
            if (!s.empty()) return s + (CE->getNumArgs() == 0 ? "()" : "(...)");
        }
        return "";
    }

    void addBlockEndHint(SourceRange braceRange, StringRef declPrefix,
                         StringRef name, StringRef optPunct) {
        if (MainFileBuf.empty()) return;
        SourceLocation beginLoc = SM.getFileLoc(braceRange.getBegin());
        SourceLocation endLoc   = SM.getFileLoc(braceRange.getEnd());
        auto [bFID, bOff] = SM.getDecomposedLoc(beginLoc);
        auto [eFID, eOff] = SM.getDecomposedLoc(endLoc);
        if (bFID != SM.getMainFileID() || eFID != SM.getMainFileID()) return;

        StringRef restOfLine = MainFileBuf.substr(eOff).split('\n').first;
        if (!restOfLine.starts_with("}")) return;
        StringRef trailing = restOfLine.drop_front().trim();
        if (!trailing.empty() && trailing != optPunct) return;

        unsigned beginLine = SM.getLineNumber(bFID, bOff);
        unsigned endLine   = SM.getLineNumber(eFID, eOff);
        if (endLine < beginLine + kBlockEndMinLines - 1) return;

        std::string label = declPrefix.str();
        if (!label.empty() && !name.empty()) label += ' ';
        label += name.str();
        if (label.empty() || label.size() > 60) return;
        label = " // " + label;

        auto p = SM.getPresumedLoc(endLoc);
        if (!p.isValid()) return;
        if ((unsigned)p.getLine() < start_line || (unsigned)p.getLine() > end_line) return;

        uint32_t closingLen = 1 + (uint32_t)(trailing.empty() ? 0 : optPunct.size());
        InlayHintEntry h;
        h.line  = (uint32_t)p.getLine();
        h.col   = (uint32_t)p.getColumn() + closingLen;
        h.label = std::move(label);
        h.kind  = 2;
        hints.push_back(std::move(h));
    }

    void markBlockEnd(const Stmt *body, StringRef lbl, StringRef name = "") {
        if (auto *CS = dyn_cast_or_null<CompoundStmt>(body))
            addBlockEndHint(CS->getSourceRange(), lbl, name, "");
    }

    bool VisitForStmt(ForStmt *S) {
        std::string name;
        if (auto *DS = dyn_cast_or_null<DeclStmt>(S->getInit());
                DS && DS->isSingleDecl())
            name = safeDeclName(cast<NamedDecl>(DS->getSingleDecl())).str();
        else
            name = summarizeCondition(S->getCond());
        markBlockEnd(S->getBody(), "for", name);
        return true;
    }

    bool VisitCXXForRangeStmt(CXXForRangeStmt *S) {
        markBlockEnd(S->getBody(), "for",
                     safeDeclName(S->getLoopVariable()).str());
        return true;
    }

    bool VisitWhileStmt(WhileStmt *S) {
        markBlockEnd(S->getBody(), "while", summarizeCondition(S->getCond()));
        return true;
    }

    bool VisitSwitchStmt(SwitchStmt *S) {
        markBlockEnd(S->getBody(), "switch", summarizeCondition(S->getCond()));
        return true;
    }

    bool VisitIfStmt(IfStmt *S) {
        if (auto *elseIf = dyn_cast_or_null<IfStmt>(S->getElse()))
            elseIfSet.insert(elseIf);
        auto *endCS = dyn_cast<CompoundStmt>(
            S->getElse() ? S->getElse() : S->getThen());
        if (endCS) {
            std::string name = elseIfSet.count(S) ? "" : summarizeCondition(S->getCond());
            addBlockEndHint({S->getThen()->getBeginLoc(), endCS->getRBracLoc()},
                            "if", name, "");
        }
        return true;
    }

    bool VisitTagDecl(TagDecl *D) {
        if (!D->isThisDeclarationADefinition()) return true;
        std::string prefix = D->getKindName().str();
        if (auto *ED = dyn_cast<EnumDecl>(D)) {
            if (ED->isScoped())
                prefix += ED->isScopedUsingClassTag() ? " class" : " struct";
        }
        addBlockEndHint(D->getBraceRange(), prefix,
                        safeDeclName(D).str(), ";");
        return true;
    }

    bool VisitNamespaceDecl(NamespaceDecl *D) {
        addBlockEndHint(D->getSourceRange(), "namespace",
                        safeDeclName(D).str(), "");
        return true;
    }

    // ── Deduced-type hints for `auto` variable declarations ───────────────────

    bool VisitVarDecl(VarDecl *D) {
        if (SM.isInSystemHeader(D->getLocation())) return true;

        // Structured bindings: `auto [x, y] = foo()`.
        // Emit a per-binding hint using canonical type (avoids
        // `tuple_element<I,A>::type` noise).  Clangd pattern.
        if (auto *DD = dyn_cast<DecompositionDecl>(D)) {
            PrintingPolicy PP(Ctx.getLangOpts());
            PP.SuppressScope = 1;
            PP.AnonymousTagLocations = 0; // print "(anonymous)" not "(lambda at file:line)"
            for (BindingDecl *BD : DD->bindings()) {
                QualType BT = BD->getType();
                if (BT.isNull() || BT->isDependentType()) continue;
                if (!inViewport(BD->getLocation())) continue;
                auto bploc = SM.getPresumedLoc(SM.getSpellingLoc(BD->getLocation()));
                if (!bploc.isValid()) continue;
                StringRef bname = BD->getName();
                if (bname.empty()) continue;
                InlayHintEntry h;
                h.line  = bploc.getLine();
                // Place after the binding name, not before (HintSide::Right).
                h.col   = bploc.getColumn() + (uint32_t)bname.size();
                h.label = ": " + BT.getCanonicalType().getAsString(PP);
                h.kind  = 1;
                hints.push_back(std::move(h));
            }
            return true;
        }

        if (!inViewport(D->getLocation())) return true;
        if (isa<ParmVarDecl>(D)) return true;

        // TypeSourceInfo holds the written `auto`; D->getType() holds the deduced type.
        if (!D->getTypeSourceInfo()->getType()->getContainedAutoType()) return true;
        QualType deduced = D->getType();
        if (deduced->isUndeducedAutoType()) return true;

        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressScope = 1;
        PP.AnonymousTagLocations = 0;
        std::string typeStr = deduced.getAsString(PP);
        if (typeStr == "auto" || typeStr.empty()) return true;
        // Resolve type-trait aliases (e.g. remove_cv_t<double> → double).
        if (typeStr.find("_t<") != std::string::npos) {
            std::string canon = deduced.getCanonicalType().getAsString(PP);
            if (!canon.empty() && canon != "auto")
                typeStr = canon;
        }
        // Suppress implementation-detail names (starting with __).
        if (typeStr.size() >= 2 && typeStr[0] == '_' && typeStr[1] == '_')
            return true;

        auto p = SM.getPresumedLoc(D->getLocation());
        if (!p.isValid()) return true;
        StringRef vname = D->getName();
        InlayHintEntry h;
        h.line  = p.getLine();
        // Place after the variable name (HintSide::Right), not before.
        h.col   = p.getColumn() + (uint32_t)vname.size();
        h.label = ": " + typeStr;
        h.kind  = 1;
        hints.push_back(std::move(h));
        return true;
    }

    // ── decltype() type hints (IH-16) ─────────────────────────────────────────
    // Emit `: T` after `decltype(expr)` type specifiers, showing the resolved type.

    bool VisitTypeLoc(TypeLoc TL) {
        const auto *DT = dyn_cast<DecltypeType>(TL.getType());
        if (!DT) return true;
        QualType UT = DT->getUnderlyingType();
        if (UT.isNull() || UT->isDependentType()) return true;
        SourceLocation endLoc = TL.getSourceRange().getEnd();
        if (!inViewport(endLoc)) return true;
        auto p = SM.getPresumedLoc(SM.getSpellingLoc(endLoc));
        if (!p.isValid()) return true;
        unsigned tokLen = Lexer::MeasureTokenLength(
            SM.getSpellingLoc(endLoc), SM, Ctx.getLangOpts());
        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressScope = 1;
        PP.AnonymousTagLocations = 0;
        std::string typeStr = UT.getAsString(PP);
        if (typeStr.empty()) return true;
        InlayHintEntry h;
        h.line  = p.getLine();
        h.col   = p.getColumn() + tokLen;
        h.label = ": " + typeStr;
        h.kind  = 1;
        hints.push_back(std::move(h));
        return true;
    }
};

CB_InlayHintList *cb_inlay_hints(CB_TransUnit *tu,
                                   uint32_t start_line, uint32_t end_line) {
    auto *list = new CB_InlayHintList{};
    ASTContext &Ctx = tu->ast->getASTContext();
    InlayHintVisitor V(Ctx, list->entries, start_line, end_line);
    V.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Sort by position then deduplicate — explicit template instantiations can
    // produce identical hints at the same location.
    auto &e = list->entries;
    std::sort(e.begin(), e.end(), [](const InlayHintEntry &a, const InlayHintEntry &b) {
        if (a.line != b.line) return a.line < b.line;
        if (a.col  != b.col)  return a.col  < b.col;
        return a.label < b.label;
    });
    e.erase(std::unique(e.begin(), e.end(), [](const InlayHintEntry &a,
                                               const InlayHintEntry &b) {
        return a.line == b.line && a.col == b.col && a.label == b.label;
    }), e.end());

    return list;
}

size_t cb_inlay_hint_count(const CB_InlayHintList *list) {
    return list->entries.size();
}

void cb_inlay_hint_get(const CB_InlayHintList *list, size_t i, CB_InlayHint *out) {
    auto *ml = const_cast<CB_InlayHintList *>(list);
    ml->current = ml->entries[i];
    out->line  = ml->current.line;
    out->col   = ml->current.col;
    out->label = ml->current.label.c_str();
    out->kind  = ml->current.kind;
}

void cb_inlay_hint_list_destroy(CB_InlayHintList *list) { delete list; }

// ── Type at cursor ─────────────────────────────────────────────────────────────

/// Return the type string for the variable/field/parameter at (line, col),
/// or NULL.  Useful for enriching hover when no doc comment exists.
char *cb_type_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    ASTContext &Ctx = tu->ast->getASTContext();

    QualType qt;
    if (auto *VD = dyn_cast<VarDecl>(ND))        qt = VD->getType();
    else if (auto *FD = dyn_cast<FieldDecl>(ND)) qt = FD->getType();
    else                                          return nullptr;

    if (qt.isNull()) return nullptr;
    PrintingPolicy PP(Ctx.getLangOpts());
    PP.SuppressScope = 0;
    std::string s = qt.getAsString(PP);
    return s.empty() ? nullptr : strdup(s.c_str());
}

// ── Macro hover ───────────────────────────────────────────────────────────────

/// Return a Markdown hover string for the macro at (line, col), or NULL if
/// (line, col) is not a macro use site.  Caller must free with cb_free_string().
char *cb_macro_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    ASTContext &Ctx = tu->ast->getASTContext();
    const SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO   = Ctx.getLangOpts();

    // Translate 1-based (line, col) → SourceLocation in the main file.
    SourceLocation target =
        SM.translateLineCol(SM.getMainFileID(), line, col);
    if (!target.isValid()) return nullptr;

    // Lex the raw token at the cursor position.
    Token tok;
    if (Lexer::getRawToken(target, tok, SM, LO, /*IgnoreWhiteSpace=*/false))
        return nullptr;
    if (tok.getKind() != tok::identifier && tok.getKind() != tok::raw_identifier)
        return nullptr;

    // getIdentifierInfo() asserts on tok::raw_identifier — look it up by spelling.
    const IdentifierInfo *II = (tok.getKind() == tok::identifier)
                                   ? tok.getIdentifierInfo()
                                   : nullptr;
    if (!II) {
        std::string spelling = Lexer::getSpelling(tok, SM, LO);
        if (spelling.empty()) return nullptr;
        II = &Ctx.Idents.get(spelling);
    }
    if (!II) return nullptr;

    Preprocessor &PP = tu->ast->getPreprocessor();
    const MacroInfo *MI = PP.getMacroInfo(II);
    if (!MI) return nullptr;

    // Build the Markdown string.
    std::string md;
    md += "```cpp\n#define ";
    md += II->getName().str();

    if (!MI->isObjectLike()) {
        // Function-like macro: show parameter list.
        md += "(";
        bool first = true;
        for (const IdentifierInfo *param : MI->params()) {
            if (!first) md += ", ";
            md += param->getName().str();
            first = false;
        }
        if (MI->isVariadic()) md += first ? "..." : ", ...";
        md += ")";
    }
    md += " ";

    // Append expansion tokens.
    for (const Token &t : MI->tokens()) {
        md += Lexer::getSpelling(t, SM, LO);
        md += " ";
    }
    md += "\n```";

    // Definition location footer.
    auto defLoc = SM.getPresumedLoc(MI->getDefinitionLoc());
    if (defLoc.isValid() && defLoc.getFilename()) {
        md += "\n\n---\n*Defined in `";
        md += defLoc.getFilename();
        md += ":";
        md += std::to_string(defLoc.getLine());
        md += "`*";
    }

    return strdup(md.c_str());
}

// ── Symbol lookup ─────────────────────────────────────────────────────────────
