#include "internal.h"

// ── Code actions ──────────────────────────────────────────────────────────────

struct CodeActionEntry {
    std::string title, file, replacement;
    uint32_t    line = 0, col = 0, end_line = 0, end_col = 0;
};
struct CB_CodeActionList {
    std::vector<CodeActionEntry> actions;
    CodeActionEntry              current;
};

CB_CodeActionList *cb_code_actions(CB_TransUnit *tu, uint32_t line, uint32_t /*col*/) {
    return cb_recover(tu, __func__, static_cast<CB_CodeActionList *>(nullptr),
                      [&]() -> CB_CodeActionList * {
    auto *list = new CB_CodeActionList{};
    const SourceManager &SM = tu->ast->getSourceManager();
    for (auto it = tu->ast->stored_diag_begin();
         it != tu->ast->stored_diag_end(); ++it) {
        const StoredDiagnostic &sd = *it;
        auto ploc = SM.getPresumedLoc(sd.getLocation());
        if (!ploc.isValid()) continue;
        uint32_t dline = ploc.getLine();
        if (line > 0 && (dline + 3 < line || dline > line + 3)) continue;
        for (const auto &fi : sd.getFixIts()) {
            auto sl = SM.getPresumedLoc(fi.RemoveRange.getBegin());
            if (!sl.isValid()) continue;
            auto el = SM.getPresumedLoc(fi.RemoveRange.getEnd());
            CodeActionEntry e;
            e.title       = sd.getMessage().str();
            e.file        = sl.getFilename() ? sl.getFilename() : "";
            e.line        = sl.getLine();
            e.col         = source_location_utf16_col(SM, fi.RemoveRange.getBegin());
            e.end_line    = el.isValid() ? el.getLine()   : sl.getLine();
            e.end_col     = el.isValid()
                                ? source_location_utf16_col(SM, fi.RemoveRange.getEnd())
                                : e.col;
            e.replacement = fi.CodeToInsert;
            list->actions.push_back(std::move(e));
        }
    }
    return list;
    });
}
size_t cb_code_action_count(const CB_CodeActionList *list) {
    return list ? list->actions.size() : 0;
}
void cb_code_action_get(const CB_CodeActionList *list, size_t i,
                        CB_CodeAction *out) {
    auto *ml = const_cast<CB_CodeActionList *>(list);
    ml->current  = ml->actions[i];
    out->title       = ml->current.title.c_str();
    out->file        = ml->current.file.c_str();
    out->line        = ml->current.line;
    out->col         = ml->current.col;
    out->end_line    = ml->current.end_line;
    out->end_col     = ml->current.end_col;
    out->replacement = ml->current.replacement.c_str();
}
void cb_code_action_list_destroy(CB_CodeActionList *list) { delete list; }


// ── Macro expansion ───────────────────────────────────────────────────────────

char *cb_expand_macro(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    return cb_recover(tu, __func__, static_cast<char *>(nullptr), [&]() -> char * {
    ASTContext          &Ctx = tu->ast->getASTContext();
    const SourceManager &SM  = Ctx.getSourceManager();
    const LangOptions   &LO  = Ctx.getLangOpts();
    Preprocessor        &PP  = tu->ast->getPreprocessor();

    SourceLocation target = translate_line_col_utf16(SM, SM.getMainFileID(), line, col);
    if (!target.isValid()) return nullptr;

    Token tok;
    if (Lexer::getRawToken(target, tok, SM, LO, false)) return nullptr;
    if (tok.getKind() != tok::identifier && tok.getKind() != tok::raw_identifier)
        return nullptr;

    const IdentifierInfo *II = tok.getKind() == tok::identifier
                                ? tok.getIdentifierInfo() : nullptr;
    if (!II) {
        std::string sp = Lexer::getSpelling(tok, SM, LO);
        if (sp.empty()) return nullptr;
        II = &Ctx.Idents.get(sp);
    }
    const MacroInfo *MI = PP.getMacroInfo(II);
    if (!MI) return nullptr;

    // Recursively expand macro body tokens (up to 5 levels deep).
    std::function<std::string(const MacroInfo*, const IdentifierInfo*, int)> expand =
        [&](const MacroInfo *mi, const IdentifierInfo *self, int depth) -> std::string {
            if (depth > 5) return "...";
            std::string body;
            for (const Token &t : mi->tokens()) {
                std::string sp = Lexer::getSpelling(t, SM, LO);
                if (!sp.empty() &&
                    (t.getKind() == tok::identifier ||
                     t.getKind() == tok::raw_identifier)) {
                    const IdentifierInfo *inner = &Ctx.Idents.get(sp);
                    const MacroInfo *innerMI = PP.getMacroInfo(inner);
                    if (innerMI && inner != self)
                        body += expand(innerMI, inner, depth + 1);
                    else
                        body += sp;
                } else {
                    body += sp;
                }
                body += " ";
            }
            while (!body.empty() && body.back() == ' ') body.pop_back();
            return body;
        };

    // Show: definition line  +  fully-expanded form (if different)
    std::string def_body;
    for (const Token &t : MI->tokens()) {
        def_body += Lexer::getSpelling(t, SM, LO);
        def_body += " ";
    }
    while (!def_body.empty() && def_body.back() == ' ') def_body.pop_back();

    std::string out;
    out += "#define " + II->getName().str();
    if (!MI->isObjectLike()) {
        out += "(";
        bool first = true;
        for (const IdentifierInfo *p : MI->params()) {
            if (!first) out += ", ";
            out += p->getName().str(); first = false;
        }
        if (MI->isVariadic()) out += first ? "..." : ", ...";
        out += ")";
    }
    out += " " + def_body;

    std::string expanded = expand(MI, II, 0);
    if (expanded != def_body && !expanded.empty())
        out += "\n→ " + expanded;

    return strdup(out.c_str());
    });
}

// ── AST dump ──────────────────────────────────────────────────────────────────

char *cb_ast_dump(CB_TransUnit *tu, uint32_t start_line, uint32_t end_line) {
    return cb_recover(tu, __func__, static_cast<char *>(nullptr), [&]() -> char * {
    ASTContext    &Ctx = tu->ast->getASTContext();
    SourceManager &SM  = Ctx.getSourceManager();
    FileID         fid = SM.getMainFileID();
    LangOptions    LO  = Ctx.getLangOpts();

    struct DumpEntry { std::string kind, name, type_str;
                       uint32_t line, col, end_line, end_col; };
    std::vector<DumpEntry> entries;

    class DumpVisitor : public RecursiveASTVisitor<DumpVisitor> {
        SourceManager &SM; FileID fid;
        uint32_t sl, el; LangOptions LO;
        std::vector<DumpEntry> &out;
        bool inRange(SourceLocation loc) const {
            SourceLocation sp = SM.getSpellingLoc(loc);
            if (!sp.isValid() || SM.getFileID(sp) != fid) return false;
            auto p = SM.getPresumedLoc(sp);
            return p.isValid() && p.getLine() >= sl && p.getLine() <= el;
        }
    public:
        DumpVisitor(SourceManager &sm, FileID f, uint32_t s, uint32_t e,
                    LangOptions lo, std::vector<DumpEntry> &o)
            : SM(sm), fid(f), sl(s), el(e), LO(lo), out(o) {}
        bool shouldVisitTemplateInstantiations() const { return false; }
        bool VisitNamedDecl(NamedDecl *D) {
            if (!inRange(D->getLocation())) return true;
            if (D->isImplicit()) return true;
            DumpEntry e;
            e.kind = D->getDeclKindName();
            e.name = D->getNameAsString();
            if (auto *VD = dyn_cast<ValueDecl>(D))
                e.type_str = VD->getType().getAsString(PrintingPolicy(LO));
            auto pb = SM.getPresumedLoc(SM.getSpellingLoc(D->getBeginLoc()));
            auto pe = SM.getPresumedLoc(SM.getSpellingLoc(D->getEndLoc()));
            e.line     = pb.isValid() ? pb.getLine()   : 0;
            e.col      = pb.isValid()
                             ? source_location_utf16_col(SM, SM.getSpellingLoc(D->getBeginLoc()))
                             : 0;
            e.end_line = pe.isValid() ? pe.getLine()   : 0;
            e.end_col  = pe.isValid()
                             ? source_location_utf16_col(SM, SM.getSpellingLoc(D->getEndLoc()))
                             : 0;
            out.push_back(std::move(e));
            return true;
        }
    } vis(SM, fid, start_line, end_line, LO, entries);
    vis.TraverseDecl(Ctx.getTranslationUnitDecl());

    auto esc = [](const std::string &s) {
        std::string r; r.reserve(s.size());
        for (char c : s) {
            if      (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else                r += c;
        }
        return r;
    };
    std::string json = "[";
    bool first = true;
    for (const auto &e : entries) {
        if (!first) json += ","; first = false;
        json += "{\"kind\":\"" + esc(e.kind) + "\",\"name\":\"" + esc(e.name) + "\"";
        if (!e.type_str.empty())
            json += ",\"type\":\"" + esc(e.type_str) + "\"";
        json += ",\"line\":"     + std::to_string(e.line);
        json += ",\"col\":"      + std::to_string(e.col);
        json += ",\"end_line\":" + std::to_string(e.end_line);
        json += ",\"end_col\":"  + std::to_string(e.end_col);
        json += "}";
    }
    json += "]";
    return strdup(json.c_str());
    });
}
