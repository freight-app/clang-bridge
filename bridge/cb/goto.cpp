#include "internal.h"

// ── Go-to-definition ──────────────────────────────────────────────────────────

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
        auto mp = SM.getPresumedLoc(MI->getDefinitionLoc());
        if (!mp.isValid()) return 0;
        out->file = strdup(mp.getFilename() ? mp.getFilename() : "");
        out->line = (uint32_t)mp.getLine();
        out->col  = source_location_utf16_col(SM, MI->getDefinitionLoc());
        return 1;
    }

    // Prefer the definition over the first declaration.
    const NamedDecl *target = ND;
    if (auto *FD = dyn_cast<FunctionDecl>(ND))
        if (auto *Def = FD->getDefinition()) target = Def;
    if (auto *TD = dyn_cast<TagDecl>(ND))
        if (auto *Def = TD->getDefinition()) target = Def;

    auto ploc = tu->ast->getASTContext().getSourceManager()
                    .getPresumedLoc(target->getLocation());
    if (!ploc.isValid()) return 0;

    out->file = strdup(ploc.getFilename() ? ploc.getFilename() : "");
    out->line = (uint32_t)ploc.getLine();
    out->col  = source_location_utf16_col(SM, target->getLocation());
    return 1;
}

// ── Code completion ───────────────────────────────────────────────────────────
