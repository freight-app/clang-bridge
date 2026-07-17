#include "internal.h"

// ── Go-to-definition ──────────────────────────────────────────────────────────

static bool fill_location_range(const SourceManager &SM,
                                const LangOptions &LangOpts,
                                SourceLocation begin, SourceLocation end_token,
                                CB_Location *out) {
    begin = SM.getFileLoc(begin);
    end_token = SM.getFileLoc(end_token);
    SourceLocation end = Lexer::getLocForEndOfToken(end_token, 0, SM, LangOpts);
    auto start_ploc = SM.getPresumedLoc(begin);
    auto end_ploc = SM.getPresumedLoc(end);
    if (!start_ploc.isValid() || !end_ploc.isValid()) return false;

    out->file = strdup(start_ploc.getFilename() ? start_ploc.getFilename() : "");
    out->line = static_cast<uint32_t>(start_ploc.getLine());
    out->col = source_location_utf16_col(SM, begin);
    out->end_line = static_cast<uint32_t>(end_ploc.getLine());
    out->end_col = source_location_utf16_col(SM, end);
    return true;
}

int cb_goto_definition(CB_TransUnit *tu, uint32_t line, uint32_t col,
                       CB_Location *out) {
    ASTContext          &Ctx = tu->ast->getASTContext();
    const SourceManager &SM  = Ctx.getSourceManager();
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) {
        // Macro fallback: jump to the `#define` of the macro under the cursor.
        SourceLocation target = translate_line_col_utf16(SM, SM.getMainFileID(), line, col);
        if (!target.isValid()) return 0;
        Token tok;
        if (Lexer::getRawToken(target, tok, SM, Ctx.getLangOpts(),
                               /*IgnoreWhiteSpace=*/true))
            return 0;
        if (tok.isNot(tok::identifier) && tok.isNot(tok::raw_identifier)) return 0;
        std::string sp = Lexer::getSpelling(tok, SM, Ctx.getLangOpts());
        if (sp.empty()) return 0;
        Preprocessor &PP = tu->ast->getPreprocessor();
        const IdentifierInfo *II = PP.getIdentifierInfo(sp);
        const MacroInfo *MI = II ? PP.getMacroInfo(II) : nullptr;
        if (!MI) return 0;
        return fill_location_range(SM, Ctx.getLangOpts(),
                                   MI->getDefinitionLoc(), MI->getDefinitionLoc(),
                                   out) ? 1 : 0;
    }

    // Prefer the definition over the first declaration.
    const NamedDecl *target = ND;
    if (auto *FD = dyn_cast<FunctionDecl>(ND))
        if (auto *Def = FD->getDefinition()) target = Def;
    if (auto *TD = dyn_cast<TagDecl>(ND))
        if (auto *Def = TD->getDefinition()) target = Def;

    SourceLocation end_token = target->getLocation();
    if (const auto *function = dyn_cast<FunctionDecl>(target)) {
        SourceLocation name_end = function->getNameInfo().getEndLoc();
        if (name_end.isValid()) end_token = name_end;
    }
    return fill_location_range(SM, Ctx.getLangOpts(), target->getLocation(),
                               end_token, out) ? 1 : 0;
}

// ── Code completion ───────────────────────────────────────────────────────────
