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
    StringRef cursor_ident;   // the identifier actually written under the cursor
    const NamedDecl *found = nullptr;

    RefLocator(const SourceManager &SM, uint32_t l, uint32_t c, StringRef ident)
        : SM(SM), target_line(l), target_col(c), cursor_ident(ident) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    // Returns true when (target_line, target_col) lands inside the `name` token
    // at tokenLoc *and* `name` equals the identifier physically written under the
    // cursor.  The name check is essential: clang synthesises nodes at a concept-
    // constrained call site (std::size_t, __builtin_addressof, …) whose reported
    // location is the call site but which name a different entity; matching by
    // the cursor's literal identifier rejects those so the cursor resolves to the
    // real symbol.  (getPresumedLoc and getDecomposedLoc disagree for such nodes,
    // so checking source text at the node's own location is not reliable.)
    bool inToken(SourceLocation tokenLoc, StringRef name) const {
        if (name.empty() || name != cursor_ident) return false;
        auto ploc = SM.getPresumedLoc(SM.getSpellingLoc(tokenLoc));
        if (!ploc.isValid()) return false;
        uint32_t startCol = (uint32_t)ploc.getColumn();
        return (uint32_t)ploc.getLine() == target_line &&
               target_col >= startCol &&
               target_col < startCol + (uint32_t)name.size();
    }

    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (!found) {
            const NamedDecl *D = E->getFoundDecl();
            // Follow using-shadow decls to the real target (HV-2).
            if (auto *USD = dyn_cast<UsingShadowDecl>(D))
                D = USD->getTargetDecl();
            // Skip references to compiler builtins (e.g. __builtin_addressof),
            // which clang synthesises at user source locations — notably at a
            // concept-constrained call site — and which would otherwise shadow
            // the real symbol under the cursor.
            if (auto *FD = dyn_cast<FunctionDecl>(D))
                if (FD->getBuiltinID() != 0) return true;
            if (inToken(E->getLocation(), safeDeclName(D)))
                found = D;
        }
        return true;
    }

    bool VisitMemberExpr(MemberExpr *E) {
        if (!found) {
            const NamedDecl *D = E->getMemberDecl();
            if (inToken(E->getMemberLoc(), safeDeclName(D)))
                found = D;
        }
        return true;
    }

    // A direct (non-member) call: resolve the cursor on the callee token to the
    // callee itself.  CallExpr is visited in pre-order, before the nodes clang
    // synthesises at a concept-constrained call site (std::size_t, addressof,
    // …), so matching here wins over those and yields the real function.
    bool VisitCallExpr(CallExpr *E) {
        if (found) return true;
        const FunctionDecl *FD = E->getDirectCallee();
        if (!FD) return true;
        if (FD->getBuiltinID() != 0) return true;   // skip synthesised builtin calls
        const Expr *callee = E->getCallee() ? E->getCallee()->IgnoreImplicit() : nullptr;
        if (auto *DRE = dyn_cast_or_null<DeclRefExpr>(callee))
            if (inToken(DRE->getLocation(), safeDeclName(FD)))
                found = FD;
        return true;
    }

    bool VisitTagTypeLoc(TagTypeLoc TL) {
        if (!found) {
            const TagDecl *D = TL.getDecl();
            if (!D->getName().empty() && inToken(TL.getNameLoc(), D->getName()))
                found = D;
        }
        return true;
    }

    bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
        if (!found) {
            const TypedefNameDecl *D = TL.getTypePtr()->getDecl();
            if (inToken(TL.getNameLoc(), D->getName()))
                found = D;
        }
        return true;
    }

    bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc TL) {
        if (!found) {
            TemplateName TN = TL.getTypePtr()->getTemplateName();
            if (TemplateDecl *TD = TN.getAsTemplateDecl()) {
                if (inToken(TL.getTemplateNameLoc(), TD->getName()))
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
                inToken(E->getLocation(), className))
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
/// Convert a 1-based UTF-16 column (what the LSP client sends) into the 1-based
/// byte column clang uses. Identity for ASCII lines; on a line with multi-byte
/// characters the byte column is larger.
static uint32_t utf16_to_byte_col(const SourceManager &SM, FileID fid,
                                  uint32_t line, uint32_t utf16_col) {
    SourceLocation lineStart = SM.translateLineCol(fid, line, 1);
    if (lineStart.isInvalid()) return utf16_col; // best effort
    bool invalid = false;
    const char *data = SM.getCharacterData(lineStart, &invalid);
    if (invalid || !data) return utf16_col;
    // Advance `utf16_col - 1` UTF-16 units from the line start, counting the
    // UTF-8 bytes consumed → the 1-based byte column.
    uint32_t want = utf16_col > 0 ? utf16_col - 1 : 0;
    uint32_t units = 0;
    size_t bytes = 0;
    while (units < want && data[bytes] != '\0' && data[bytes] != '\n') {
        unsigned char c = (unsigned char)data[bytes];
        size_t cp = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += (cp == 4) ? 2 : 1; // 4-byte UTF-8 = surrogate pair = 2 units
        bytes += cp;
    }
    return (uint32_t)bytes + 1;
}

SourceLocation translate_line_col_utf16(const SourceManager &SM, FileID fid,
                                        uint32_t line, uint32_t utf16_col) {
    return SM.translateLineCol(fid, line,
                               utf16_to_byte_col(SM, fid, line, utf16_col));
}

uint32_t utf16_length(StringRef text) {
    uint32_t units = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + bytes > text.size()) bytes = 1;
        units += bytes == 4 ? 2 : 1;
        i += bytes;
    }
    return units;
}

uint32_t source_location_utf16_col(const SourceManager &SM,
                                   SourceLocation loc) {
    loc = SM.getFileLoc(loc);
    if (loc.isInvalid()) return 0;
    PresumedLoc presumed = SM.getPresumedLoc(loc);
    if (!presumed.isValid()) return 0;

    bool invalid = false;
    const char *at = SM.getCharacterData(loc, &invalid);
    if (invalid || !at) return static_cast<uint32_t>(presumed.getColumn());

    size_t byte_prefix = presumed.getColumn() > 0
                             ? static_cast<size_t>(presumed.getColumn() - 1)
                             : 0;
    return utf16_length(StringRef(at - byte_prefix, byte_prefix)) + 1;
}

const NamedDecl *locate_symbol_at(ASTUnit *ast, uint32_t line, uint32_t col) {
    ASTContext &Ctx = ast->getASTContext();
    const SourceManager &SM = Ctx.getSourceManager();
    // The client sends a UTF-16 column; convert it to a byte column once so the
    // byte-based comparisons in RefLocator/DeclLocator below line up.
    col = utf16_to_byte_col(SM, SM.getMainFileID(), line, col);

    // Clangd pattern: only proceed when the cursor is on an identifier token.
    // Hovering on punctuation (::, (, *, etc.) should return nothing rather
    // than a garbage result from the DeclLocator fallback.  Also capture the
    // exact identifier written under the cursor so RefLocator can reject AST
    // nodes whose reported location coincides with the cursor but which name a
    // different entity (synthesised constraint nodes at a concept call site).
    StringRef cursor_ident;
    {
        // `col` is already a byte column (converted above).
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
            // Expand over identifier characters around the cursor to get the
            // whole identifier (translateLineCol may land mid-token).
            auto [fid, off] = SM.getDecomposedLoc(loc);
            bool invalid = false;
            StringRef buf = SM.getBufferData(fid, &invalid);
            if (!invalid && off <= buf.size()) {
                auto isIdent = [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_';
                };
                size_t b = off, e = off;
                while (b > 0 && isIdent(buf[b - 1])) --b;
                while (e < buf.size() && isIdent(buf[e])) ++e;
                cursor_ident = buf.substr(b, e - b);
            }
        }
    }

    RefLocator refV(SM, line, col, cursor_ident);
    refV.TraverseDecl(Ctx.getTranslationUnitDecl());
    if (refV.found) return refV.found;

    DeclLocator declV(SM, line, col);
    declV.TraverseDecl(Ctx.getTranslationUnitDecl());
    return declV.found;
}

CB_Symbol *cb_symbol_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    return cb_recover(tu, __func__, static_cast<CB_Symbol *>(nullptr), [&]() -> CB_Symbol * {
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
    });
}

const char *cb_symbol_name(const CB_Symbol *s)      { return s->name.c_str(); }
const char *cb_symbol_usr(const CB_Symbol *s)       { return s->usr.c_str(); }
const char *cb_symbol_kind(const CB_Symbol *s)      { return s->kind.c_str(); }
const char *cb_symbol_brief(const CB_Symbol *s)     { return s->brief.c_str(); }
const char *cb_symbol_signature(const CB_Symbol *s) { return s->signature.c_str(); }
const char *cb_symbol_def_file(const CB_Symbol *s)  { return s->def_file.c_str(); }
uint32_t    cb_symbol_def_line(const CB_Symbol *s)  { return s->def_line; }
void        cb_symbol_destroy(CB_Symbol *s)         { delete s; }
