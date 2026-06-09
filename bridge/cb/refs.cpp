#include "internal.h"

// ── References — all usages of a USR within a TU ─────────────────────────────

struct CB_ReferenceList {
    std::vector<ReferenceEntry> refs;
    ReferenceEntry current;
};

class RefCollector : public index::IndexDataConsumer {
public:
    const std::string          &target_usr;
    std::vector<ReferenceEntry> &refs;
    const SourceManager        *SM = nullptr;
    // The indexer can report the same source location more than once (e.g. an
    // initializer visited in two contexts).  Dedup by file:line:col so callers
    // — including rename — never see a duplicate occurrence.
    std::unordered_set<std::string> seen;

    RefCollector(const std::string &usr, std::vector<ReferenceEntry> &out)
        : target_usr(usr), refs(out) {}

    void initialize(ASTContext &Ctx) override {
        SM = &Ctx.getSourceManager();
    }

    bool handleDeclOccurrence(
        const Decl *D,
        index::SymbolRoleSet Roles,
        llvm::ArrayRef<index::SymbolRelation> /*Relations*/,
        SourceLocation Loc,
        ASTNodeInfo /*ASTNode*/
    ) override {
        if (!SM) return true;
        SmallString<128> usr_buf;
        if (index::generateUSRForDecl(D, usr_buf)) return true;
        if (usr_buf.str() != target_usr) return true;

        Loc = SM->getSpellingLoc(Loc);
        if (SM->isInSystemHeader(Loc)) return true;
        auto p = SM->getPresumedLoc(Loc);
        if (!p.isValid()) return true;

        std::string key = std::string(p.getFilename() ? p.getFilename() : "")
                        + ":" + std::to_string(p.getLine())
                        + ":" + std::to_string(p.getColumn());
        if (!seen.insert(key).second) return true;

        using SR = index::SymbolRole;
        bool is_def = (Roles & (index::SymbolRoleSet)SR::Definition) != 0;

        ReferenceEntry e;
        e.file          = p.getFilename() ? p.getFilename() : "";
        e.line          = p.getLine();
        e.col           = p.getColumn();
        e.is_definition = is_def;
        refs.push_back(std::move(e));
        return true;
    }
};

CB_ReferenceList *cb_references(CB_TransUnit *tu, const char *usr) {
    auto *list = new CB_ReferenceList{};
    if (!usr || !*usr) return list;

    std::string target(usr);
    RefCollector consumer(target, list->refs);

    index::IndexingOptions opts{};
    opts.SystemSymbolFilter =
        index::IndexingOptions::SystemSymbolFilterKind::None;
    opts.IndexFunctionLocals = true;

    index::indexASTUnit(*tu->ast, consumer, opts);
    return list;
}

size_t cb_reference_count(const CB_ReferenceList *list) {
    return list->refs.size();
}

void cb_reference_get(const CB_ReferenceList *list, size_t i,
                      CB_Reference *out) {
    auto *ml = const_cast<CB_ReferenceList *>(list);
    ml->current = ml->refs[i];
    out->file          = ml->current.file.c_str();
    out->line          = ml->current.line;
    out->col           = ml->current.col;
    out->is_definition = ml->current.is_definition ? 1 : 0;
}

void cb_reference_list_destroy(CB_ReferenceList *list) { delete list; }

// ── Rename — collect edits for renaming a symbol ──────────────────────────────

struct RenameEditEntry {
    std::string file;
    uint32_t    line, col;
    uint32_t    old_name_len;
    std::string new_name;
};

struct CB_RenameList {
    std::vector<RenameEditEntry> edits;
    std::string conflict_msg; // non-empty if rename would shadow/conflict
    RenameEditEntry current;
};

/// Collect all edits needed to rename the symbol with `usr` to `new_name`.
/// Returns a CB_RenameList; if cb_rename_has_conflict returns non-zero, the
/// rename is risky and cb_rename_conflict_message contains a human-readable
/// reason.  The edit list is still populated for preview even when a conflict
/// is detected.
CB_RenameList *cb_rename(CB_TransUnit *tu, const char *usr,
                          const char *new_name) {
    auto *list = new CB_RenameList{};
    if (!usr || !*usr || !new_name || !*new_name) return list;

    std::string target(usr);
    std::vector<ReferenceEntry> refs;
    RefCollector rc(target, refs);
    index::IndexingOptions opts{};
    opts.SystemSymbolFilter =
        index::IndexingOptions::SystemSymbolFilterKind::None;
    opts.IndexFunctionLocals = true;
    index::indexASTUnit(*tu->ast, rc, opts);

    // Resolve the old name from the first reference.
    std::string old_name;
    {
        ASTContext &Ctx = tu->ast->getASTContext();
        // Walk declarations to find one matching the USR.
        struct NameFinder : RecursiveASTVisitor<NameFinder> {
            const std::string &target_usr;
            std::string name;
            bool VisitNamedDecl(NamedDecl *D) {
                SmallString<128> buf;
                if (!index::generateUSRForDecl(D, buf) && buf.str() == target_usr)
                    name = D->getNameAsString();
                return true;
            }
            NameFinder(const std::string &u) : target_usr(u) {}
        } finder(target);
        finder.TraverseDecl(Ctx.getTranslationUnitDecl());
        old_name = finder.name;
    }

    // Check for name conflicts: does new_name already exist in the same scope?
    // Simple heuristic: if any NamedDecl at the top level has new_name and a
    // different USR, warn.
    {
        ASTContext &Ctx = tu->ast->getASTContext();
        struct ConflictChecker : RecursiveASTVisitor<ConflictChecker> {
            const std::string &new_name, &target_usr;
            std::string conflict;
            bool VisitNamedDecl(NamedDecl *D) {
                if (D->getNameAsString() != new_name) return true;
                SmallString<128> buf;
                if (index::generateUSRForDecl(D, buf)) return true;
                if (buf.str() != target_usr)
                    conflict = "name '" + new_name + "' is already used at "
                               + D->getLocation().printToString(
                                   D->getASTContext().getSourceManager());
                return true;
            }
            ConflictChecker(const std::string &n, const std::string &u)
                : new_name(n), target_usr(u) {}
        } checker(std::string(new_name), target);
        checker.TraverseDecl(Ctx.getTranslationUnitDecl());
        list->conflict_msg = checker.conflict;
    }

    // Build edits.
    for (const auto &ref : refs) {
        RenameEditEntry e;
        e.file         = ref.file;
        e.line         = ref.line;
        e.col          = ref.col;
        e.old_name_len = (uint32_t)old_name.size();
        e.new_name     = new_name;
        list->edits.push_back(std::move(e));
    }
    return list;
}

size_t cb_rename_edit_count(const CB_RenameList *list) {
    return list->edits.size();
}

void cb_rename_edit_get(const CB_RenameList *list, size_t i,
                        CB_RenameEdit *out) {
    auto *ml = const_cast<CB_RenameList *>(list);
    ml->current = ml->edits[i];
    out->file         = ml->current.file.c_str();
    out->line         = ml->current.line;
    out->col          = ml->current.col;
    out->old_name_len = ml->current.old_name_len;
    out->new_name     = ml->current.new_name.c_str();
}

int cb_rename_has_conflict(const CB_RenameList *list) {
    return list->conflict_msg.empty() ? 0 : 1;
}

const char *cb_rename_conflict_message(const CB_RenameList *list) {
    return list->conflict_msg.empty() ? nullptr : list->conflict_msg.c_str();
}

void cb_rename_list_destroy(CB_RenameList *list) { delete list; }

// ── Document highlight ────────────────────────────────────────────────────────
// Find all occurrences of the symbol at (line,col) within the main file.
// Maps to LSP textDocument/documentHighlight.

struct HighlightEntry { uint32_t line, col, end_col; uint8_t kind; };

struct CB_HighlightList {
    std::vector<HighlightEntry> entries;
};

class HighlightCollector : public index::IndexDataConsumer {
    const std::string          &target_usr;
    std::vector<HighlightEntry> &out;
    const SourceManager        *SM = nullptr;
    LangOptions                 LO;
    FileID                      main_fid;
    // The indexer can report one occurrence twice; dedup by line:col so the
    // highlight list never contains duplicate ranges.
    std::unordered_set<std::string> seen;
public:
    HighlightCollector(const std::string &usr, std::vector<HighlightEntry> &o,
                       const LangOptions &lo, FileID fid)
        : target_usr(usr), out(o), LO(lo), main_fid(fid) {}

    void initialize(ASTContext &Ctx) override { SM = &Ctx.getSourceManager(); }

    bool handleDeclOccurrence(const Decl *D, index::SymbolRoleSet Roles,
                              llvm::ArrayRef<index::SymbolRelation>,
                              SourceLocation Loc, ASTNodeInfo) override {
        if (!SM) return true;
        SmallString<128> buf;
        if (index::generateUSRForDecl(D, buf)) return true;
        if (buf.str() != target_usr) return true;
        Loc = SM->getSpellingLoc(Loc);
        if (SM->getFileID(Loc) != main_fid) return true;
        auto p = SM->getPresumedLoc(Loc);
        if (!p.isValid()) return true;
        std::string key = std::to_string(p.getLine()) + ":"
                        + std::to_string(p.getColumn());
        if (!seen.insert(key).second) return true;
        unsigned tok_len = Lexer::MeasureTokenLength(Loc, *SM, LO);
        using SR = index::SymbolRole;
        bool is_write = (Roles & ((index::SymbolRoleSet)SR::Definition |
                                   (index::SymbolRoleSet)SR::Write)) != 0;
        out.push_back({p.getLine(), p.getColumn(),
                       p.getColumn() + tok_len, is_write ? (uint8_t)3 : (uint8_t)2});
        return true;
    }
};

CB_HighlightList *cb_highlight(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    auto *list = new CB_HighlightList{};
    CB_Symbol *sym = cb_symbol_at(tu, line, col);
    if (!sym) return list;
    std::string usr(cb_symbol_usr(sym));
    cb_symbol_destroy(sym);
    if (usr.empty()) return list;

    ASTContext    &Ctx     = tu->ast->getASTContext();
    SourceManager &SM      = Ctx.getSourceManager();
    HighlightCollector consumer(usr, list->entries, Ctx.getLangOpts(),
                                SM.getMainFileID());
    index::IndexingOptions opts{};
    opts.SystemSymbolFilter = index::IndexingOptions::SystemSymbolFilterKind::None;
    opts.IndexFunctionLocals = true;
    index::indexASTUnit(*tu->ast, consumer, opts);
    return list;
}

size_t cb_highlight_count(const CB_HighlightList *list) { return list->entries.size(); }

void cb_highlight_get(const CB_HighlightList *list, size_t i, CB_HighlightEntry *out) {
    const auto &e = list->entries[i];
    out->line     = e.line;
    out->col      = e.col;
    out->end_line = e.line;
    out->end_col  = e.end_col;
    out->kind     = e.kind;
}

void cb_highlight_list_destroy(CB_HighlightList *list) { delete list; }
