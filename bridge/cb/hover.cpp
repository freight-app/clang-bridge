#include "internal.h"

// ── Hover markdown ────────────────────────────────────────────────────────────

// Extract plain text from a comment node and all its inline children.
static std::string commentInlineText(const comments::Comment *C) {
    std::string out;
    if (!C) return out;
    if (auto *TC = dyn_cast<comments::TextComment>(C)) {
        StringRef t = TC->getText();
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t = t.drop_front();
        return t.str();
    }
    for (auto it = C->child_begin(); it != C->child_end(); ++it)
        out += commentInlineText(*it);
    return out;
}

// Render a FullComment into LSP-ready Markdown with @param / @returns / @note.
static std::string renderFullComment(const comments::FullComment *FC,
                                     ASTContext &Ctx) {
    std::string md;
    auto append = [&](const std::string &s) {
        if (!s.empty()) { if (!md.empty()) md += "\n\n"; md += s; }
    };

    for (auto it = FC->child_begin(); it != FC->child_end(); ++it) {
        if (auto *PC = dyn_cast<comments::ParagraphComment>(*it)) {
            append(commentInlineText(PC));
        } else if (auto *PCC = dyn_cast<comments::ParamCommandComment>(*it)) {
            std::string body = commentInlineText(PCC->getParagraph());
            if (!body.empty())
                append("**`" + PCC->getParamNameAsWritten().str() + "`** — " + body);
        } else if (auto *BCC = dyn_cast<comments::BlockCommandComment>(*it)) {
            std::string cmd = BCC->getCommandName(Ctx.getCommentCommandTraits()).str();
            std::string body = BCC->getParagraph()
                             ? commentInlineText(BCC->getParagraph()) : std::string{};
            if (!body.empty()) {
                if (cmd == "returns" || cmd == "return")
                    append("**Returns:** " + body);
                else if (cmd == "note")
                    append("> **Note:** " + body);
                else if (cmd == "warning" || cmd == "warn")
                    append("> **Warning:** " + body);
                else if (cmd == "deprecated")
                    append("> **Deprecated:** " + body);
                else if (cmd == "throws" || cmd == "throw" || cmd == "exception")
                    append("**Throws:** " + body);
                else
                    append("**" + cmd + ":** " + body);
            }
        }
    }
    return md;
}

/// Return the raw (stripped) doc comment text for the symbol at (line, col),
/// or NULL if none is found.  The text has comment markers removed but
/// otherwise preserves the original formatting.  Caller must free with
/// cb_free_string().
char *cb_raw_comment_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    ASTContext &Ctx = tu->ast->getASTContext();
    const RawComment *RC = Ctx.getRawCommentForDeclNoCache(ND);
    if (!RC) return nullptr;
    std::string text = RC->getFormattedText(Ctx.getSourceManager(), Ctx.getDiagnostics());
    if (text.empty()) return nullptr;
    return strdup(text.c_str());
}

/// Full hover: signature + structured doc comment (param/returns/note) + def location.
/// Falls back to brief + signature when no structured comment is present.
char *cb_hover_full(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    ASTContext &Ctx = tu->ast->getASTContext();

    // Signature code block
    std::string md;
    std::string sig = prettySignature(ND, Ctx);
    if (!sig.empty()) { md += "```cpp\n"; md += sig; md += "\n```"; }

    // Full structured comment (FullComment AST)
    const comments::FullComment *FC = Ctx.getCommentForDecl(ND, nullptr);
    if (FC) {
        std::string cm = renderFullComment(FC, Ctx);
        if (!cm.empty()) { if (!md.empty()) md += "\n\n"; md += cm; }
    } else if (const RawComment *RC = Ctx.getRawCommentForDeclNoCache(ND)) {
        // Fallback: formatted raw comment text
        std::string full = RC->getFormattedText(Ctx.getSourceManager(), Ctx.getDiagnostics());
        if (!full.empty()) { if (!md.empty()) md += "\n\n"; md += full; }
    }

    // Definition location footer
    const NamedDecl *def = ND;
    if (auto *FD = dyn_cast<FunctionDecl>(ND))
        if (auto *D = FD->getDefinition()) def = D;
    if (auto *TD = dyn_cast<TagDecl>(ND))
        if (auto *D = TD->getDefinition()) def = D;
    auto ploc = Ctx.getSourceManager().getPresumedLoc(def->getLocation());
    if (ploc.isValid() && ploc.getFilename()) {
        if (!md.empty()) md += "\n\n";
        md += "---\n*Defined in `";
        md += ploc.getFilename();
        md += ":";
        md += std::to_string(ploc.getLine());
        md += "`*";
    }

    if (md.empty()) return nullptr;
    return strdup(md.c_str());
}

char *cb_hover_markdown(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    ASTContext &Ctx = tu->ast->getASTContext();

    std::string sig = prettySignature(ND, Ctx);
    std::string brief;
    const RawComment *RC = Ctx.getRawCommentForDeclNoCache(ND);
    if (RC) {
        std::string full = RC->getFormattedText(Ctx.getSourceManager(), Ctx.getDiagnostics());
        std::istringstream ss(full);
        std::string l;
        while (std::getline(ss, l)) {
            size_t s = l.find_first_not_of(" \t\r\n");
            if (s == std::string::npos) continue;
            brief = l.substr(s);
            break;
        }
    }

    std::string md;
    if (!sig.empty()) {
        md += "```cpp\n";
        md += sig;
        md += "\n```";
    }
    if (!brief.empty()) {
        if (!md.empty()) md += "\n\n";
        md += brief;
    }
    if (md.empty()) return nullptr;
    return strdup(md.c_str());
}

// ── Go-to-definition ──────────────────────────────────────────────────────────
