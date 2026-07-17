#include "internal.h"

// ── Workspace symbol index ────────────────────────────────────────────────────

class WorkspaceIndexer : public RecursiveASTVisitor<WorkspaceIndexer> {
    CB_Index      *idx;
    SourceManager &SM;

    void addDecl(const NamedDecl *D) {
        StringRef name = safeDeclName(D);
        if (name.empty()) return;
        if (D->isImplicit()) return;
        if (SM.isInSystemHeader(D->getLocation())) return;
        auto ploc = SM.getPresumedLoc(D->getLocation());
        if (!ploc.isValid()) return;
        SmallString<128> buf;
        index::generateUSRForDecl(D, buf);
        WorkspaceSymEntry e;
        e.name   = name.str();
        e.detail = D->getQualifiedNameAsString();
        e.kind   = declKind(D);
        e.file   = ploc.getFilename() ? ploc.getFilename() : "";
        e.line   = ploc.getLine();
        e.col    = source_location_utf16_col(SM, D->getLocation());
        e.usr    = buf.str().str();
        std::string lower = name.lower();
        idx->sym_index.emplace(std::move(lower), std::move(e));
    }
public:
    WorkspaceIndexer(CB_Index *i, ASTContext &C)
        : idx(i), SM(C.getSourceManager()) {}
    bool shouldVisitTemplateInstantiations() const { return false; }
    bool VisitNamedDecl(NamedDecl *D) {
        if (isa<NamespaceDecl>(D)) return true;
        if (!isa<FunctionDecl>(D) && !isa<CXXRecordDecl>(D) &&
            !isa<EnumDecl>(D)     && !isa<VarDecl>(D) &&
            !isa<TypedefNameDecl>(D) && !isa<EnumConstantDecl>(D))
            return true;
        addDecl(D);
        return true;
    }
};

void cb_workspace_index_add(CB_Index *idx, CB_TransUnit *tu) {
    if (!idx || !tu) return;
    // Remove stale entries for this file so reparse doesn't create duplicates.
    {
        std::string main_path = tu->ast->getMainFileName().str();
        if (!main_path.empty()) {
            for (auto it = idx->sym_index.begin(); it != idx->sym_index.end(); ) {
                if (it->second.file == main_path)
                    it = idx->sym_index.erase(it);
                else
                    ++it;
            }
        }
    }
    WorkspaceIndexer wi(idx, tu->ast->getASTContext());
    wi.TraverseDecl(tu->ast->getASTContext().getTranslationUnitDecl());
}

struct CB_WorkspaceSymList {
    std::vector<WorkspaceSymEntry> results;
    WorkspaceSymEntry              current;
};

CB_WorkspaceSymList *cb_workspace_symbols(CB_Index *idx, const char *query) {
    auto *list = new CB_WorkspaceSymList{};
    if (!idx) return list;
    const bool empty_query = (!query || !*query);
    std::string q(query ? query : "");
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    std::unordered_set<std::string> seen;
    for (const auto &[key, entry] : idx->sym_index) {
        if (!empty_query && key.find(q) == std::string::npos) continue;
        if (!seen.insert(entry.usr).second) continue;
        list->results.push_back(entry);
        if (list->results.size() >= 500) break;
    }
    if (!empty_query) {
        std::sort(list->results.begin(), list->results.end(),
                  [&q](const WorkspaceSymEntry &a, const WorkspaceSymEntry &b) {
                      std::string la = a.name, lb = b.name;
                      std::transform(la.begin(), la.end(), la.begin(), ::tolower);
                      std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
                      bool pa = la.find(q) == 0, pb = lb.find(q) == 0;
                      if (pa != pb) return pa > pb;
                      return la < lb;
                  });
    }
    return list;
}
size_t cb_workspace_sym_count(const CB_WorkspaceSymList *list) {
    return list->results.size();
}
void cb_workspace_sym_get(const CB_WorkspaceSymList *list, size_t i,
                          CB_WorkspaceSym *out) {
    auto *ml = const_cast<CB_WorkspaceSymList *>(list);
    ml->current = ml->results[i];
    out->name   = ml->current.name.c_str();
    out->detail = ml->current.detail.c_str();
    out->kind   = ml->current.kind.c_str();
    out->file   = ml->current.file.c_str();
    out->line   = ml->current.line;
    out->col    = ml->current.col;
    out->usr    = ml->current.usr.c_str();
}
void cb_workspace_sym_list_destroy(CB_WorkspaceSymList *list) { delete list; }

// ── Call hierarchy ────────────────────────────────────────────────────────────
