#include "internal.h"

// ── Go-to-definition ──────────────────────────────────────────────────────────

int cb_goto_definition(CB_TransUnit *tu, uint32_t line, uint32_t col,
                       CB_Location *out) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return 0;

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
    out->col  = (uint32_t)ploc.getColumn();
    return 1;
}

// ── Code completion ───────────────────────────────────────────────────────────

