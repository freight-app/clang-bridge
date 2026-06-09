#include "internal.h"


// ── Symbol lookup ─────────────────────────────────────────────────────────────

struct CB_Symbol {
    std::string name;
    std::string usr;
    std::string kind;
    std::string brief;
    std::string signature;
    std::string def_file;
    uint32_t    def_line = 0;
};

class RefLocator : public RecursiveASTVisitor<RefLocator> {
public:
    const SourceManager &SM;
    uint32_t target_line, target_col;
    const NamedDecl *found = nullptr;

    RefLocator(const SourceManager &SM, uint32_t l, uint32_t c)
        : SM(SM), target_line(l), target_col(c) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    // Returns true when (target_line, target_col) lands inside the token at
    // tokenLoc of the given length.
    bool inToken(SourceLocation tokenLoc, size_t nameLen) const {
        if (nameLen == 0) return false;
        auto ploc = SM.getPresumedLoc(SM.getSpellingLoc(tokenLoc));
        if (!ploc.isValid()) return false;
        uint32_t startCol = (uint32_t)ploc.getColumn();
        return (uint32_t)ploc.getLine() == target_line &&
               target_col >= startCol &&
               target_col < startCol + (uint32_t)nameLen;
    }

    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (!found) {
            const NamedDecl *D = E->getFoundDecl();
            // Follow using-shadow decls to the real target (HV-2).
            if (auto *USD = dyn_cast<UsingShadowDecl>(D))
                D = USD->getTargetDecl();
            if (inToken(E->getLocation(), safeDeclName(D).size()))
                found = D;
        }
        return true;
    }

    bool VisitMemberExpr(MemberExpr *E) {
        if (!found) {
            const NamedDecl *D = E->getMemberDecl();
            if (inToken(E->getMemberLoc(), safeDeclName(D).size()))
                found = D;
        }
        return true;
    }

    bool VisitTagTypeLoc(TagTypeLoc TL) {
        if (!found) {
            const TagDecl *D = TL.getDecl();
            if (!D->getName().empty() && inToken(TL.getNameLoc(), D->getName().size()))
                found = D;
        }
        return true;
    }

    bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
        if (!found) {
            const TypedefNameDecl *D = TL.getTypePtr()->getDecl();
            if (inToken(TL.getNameLoc(), D->getName().size()))
                found = D;
        }
        return true;
    }

    bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc TL) {
        if (!found) {
            TemplateName TN = TL.getTypePtr()->getTemplateName();
            if (TemplateDecl *TD = TN.getAsTemplateDecl()) {
                if (inToken(TL.getTemplateNameLoc(), TD->getName().size()))
                    found = TD;
            }
        }
        return true;
    }

    // SL-2: constructor call sites — `MyClass(args)` / `MyClass{args}`.
    // CXXTemporaryObjectExpr (a subclass) also hits this visitor.
    bool VisitCXXConstructExpr(CXXConstructExpr *E) {
        if (!found) {
            // Skip implicit/elided constructions (e.g. `Widget w;` default-init):
            // their location coincides with the *variable*, so matching here would
            // shadow the VarDecl and make hover/type_at report the constructor
            // instead of the variable.  Explicit `Foo(...)`/`Foo{...}` carry a
            // valid paren/brace range.
            if (E->getParenOrBraceRange().isInvalid()) return true;
            CXXConstructorDecl *ctor = E->getConstructor();
            if (!ctor) return true;
            StringRef className = ctor->getParent()->getName();
            if (!className.empty() && E->getLocation().isValid() &&
                inToken(E->getLocation(), className.size()))
                found = ctor;
        }
        return true;
    }
};

// Visitor: matches declaration sites (original fallback behaviour).
// Stage 2 — used when no expression reference matched the cursor.
class DeclLocator : public RecursiveASTVisitor<DeclLocator> {
public:
    const SourceManager &SM;
    uint32_t target_line, target_col;
    const NamedDecl *found = nullptr;

    DeclLocator(const SourceManager &SM, uint32_t l, uint32_t c)
        : SM(SM), target_line(l), target_col(c) {}

    // Do not descend into template instantiations: their implicit members
    // are attributed to the instantiation site in user code, which makes
    // them look like user declarations on the same line and pollute results.
    bool shouldVisitTemplateInstantiations() const { return false; }

    bool VisitNamedDecl(NamedDecl *ND) {
        if (ND->isImplicit()) return true;
        if (SM.isInSystemHeader(ND->getLocation())) return true;

        // For plain identifiers use safeDeclName.  For constructors / destructors
        // the declaration name is the parent class name (SL-1).
        StringRef name = safeDeclName(ND);
        if (name.empty()) {
            if (auto *CD = dyn_cast<CXXConstructorDecl>(ND))
                name = CD->getParent()->getName();
            else if (auto *DD = dyn_cast<CXXDestructorDecl>(ND))
                name = DD->getParent()->getName();
            // Operators, conversions, etc. — not reachable by plain cursor hover.
        }
        if (name.empty()) return true;

        auto ploc = SM.getPresumedLoc(ND->getLocation());
        if (!ploc.isValid()) return true;
        uint32_t startCol = (uint32_t)ploc.getColumn();
        if ((uint32_t)ploc.getLine() == target_line &&
            target_col >= startCol &&
            target_col < startCol + (uint32_t)name.size())
            found = ND;
        return true;
    }
};

// Checks expression reference nodes first, then declaration sites.
const NamedDecl *locate_symbol_at(ASTUnit *ast, uint32_t line, uint32_t col) {
    ASTContext &Ctx = ast->getASTContext();
    const SourceManager &SM = Ctx.getSourceManager();

    // Clangd pattern: only proceed when the cursor is on an identifier token.
    // Hovering on punctuation (::, (, *, etc.) should return nothing rather
    // than a garbage result from the DeclLocator fallback.
    {
        SourceLocation loc = SM.translateLineCol(SM.getMainFileID(), line, col);
        if (loc.isValid()) {
            Token tok;
            if (!Lexer::getRawToken(loc, tok, SM, Ctx.getLangOpts(),
                                    /*IgnoreWhiteSpace=*/true)) {
                if (tok.getKind() != tok::identifier &&
                    tok.getKind() != tok::raw_identifier &&
                    tok.getKind() != tok::kw_auto &&
                    tok.getKind() != tok::kw_operator)
                    return nullptr;
            }
        }
    }

    RefLocator refV(SM, line, col);
    refV.TraverseDecl(Ctx.getTranslationUnitDecl());
    if (refV.found) return refV.found;

    DeclLocator declV(SM, line, col);
    declV.TraverseDecl(Ctx.getTranslationUnitDecl());
    return declV.found;
}

CB_Symbol *cb_symbol_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    ASTContext &Ctx = tu->ast->getASTContext();

    auto *sym = new CB_Symbol{};
    sym->name = ND->getQualifiedNameAsString();
    sym->kind = declKind(ND);
    sym->signature = prettySignature(ND, Ctx);

    {
        llvm::SmallString<128> buf;
        index::generateUSRForDecl(ND, buf);
        sym->usr = buf.str().str();
    }

    const RawComment *RC = Ctx.getRawCommentForDeclNoCache(ND);
    if (RC) {
        std::string full = RC->getFormattedText(Ctx.getSourceManager(), Ctx.getDiagnostics());
        std::istringstream ss(full);
        std::string l;
        while (std::getline(ss, l)) {
            size_t s = l.find_first_not_of(" \t\r\n");
            if (s == std::string::npos) continue;
            sym->brief = l.substr(s);
            break;
        }
    }

    // Definition location
    auto ploc = Ctx.getSourceManager().getPresumedLoc(ND->getLocation());
    if (ploc.isValid()) {
        sym->def_file = ploc.getFilename() ? ploc.getFilename() : "";
        sym->def_line = (uint32_t)ploc.getLine();
    }

    return sym;
}

const char *cb_symbol_name(const CB_Symbol *s)      { return s->name.c_str(); }
const char *cb_symbol_usr(const CB_Symbol *s)       { return s->usr.c_str(); }
const char *cb_symbol_kind(const CB_Symbol *s)      { return s->kind.c_str(); }
const char *cb_symbol_brief(const CB_Symbol *s)     { return s->brief.c_str(); }
const char *cb_symbol_signature(const CB_Symbol *s) { return s->signature.c_str(); }
const char *cb_symbol_def_file(const CB_Symbol *s)  { return s->def_file.c_str(); }
uint32_t    cb_symbol_def_line(const CB_Symbol *s)  { return s->def_line; }
void        cb_symbol_destroy(CB_Symbol *s)         { delete s; }

