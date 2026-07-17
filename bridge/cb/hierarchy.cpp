#include "internal.h"

// ── Call hierarchy ────────────────────────────────────────────────────────────

struct CB_CallHierItem {
    std::string name, detail, file, usr;
    uint32_t    line = 0, col = 0;
};

struct CallEdgeEntry {
    std::string name, detail, file, usr;
    uint32_t    line = 0, col = 0;
    uint32_t    call_line = 0, call_col = 0;
};

struct CB_CallEdgeList {
    std::vector<CallEdgeEntry> edges;
    CallEdgeEntry              current;
};

CB_CallHierItem *cb_call_hierarchy_prepare(CB_TransUnit *tu,
                                           uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    if (!isa<FunctionDecl>(ND)) return nullptr;
    ASTContext    &Ctx = tu->ast->getASTContext();
    SourceManager &SM  = Ctx.getSourceManager();
    auto *item = new CB_CallHierItem{};
    item->name   = ND->getNameAsString();
    item->detail = ND->getQualifiedNameAsString();
    SmallString<128> buf;
    index::generateUSRForDecl(ND, buf);
    item->usr  = buf.str().str();
    auto ploc  = SM.getPresumedLoc(ND->getLocation());
    if (ploc.isValid()) {
        item->file = ploc.getFilename() ? ploc.getFilename() : "";
        item->line = ploc.getLine();
        item->col  = source_location_utf16_col(SM, ND->getLocation());
    }
    return item;
}
void             cb_call_hier_item_destroy(CB_CallHierItem *item) { delete item; }
const char      *cb_call_hier_name  (const CB_CallHierItem *i) { return i->name.c_str(); }
const char      *cb_call_hier_detail(const CB_CallHierItem *i) { return i->detail.c_str(); }
const char      *cb_call_hier_file  (const CB_CallHierItem *i) { return i->file.c_str(); }
uint32_t         cb_call_hier_line  (const CB_CallHierItem *i) { return i->line; }
uint32_t         cb_call_hier_col   (const CB_CallHierItem *i) { return i->col; }
const char      *cb_call_hier_usr   (const CB_CallHierItem *i) { return i->usr.c_str(); }

// Visitor that builds call edges. For outgoing=true: collect all calls FROM
// the function with target_usr. For outgoing=false: collect calls TO it.
class CallGraphVisitor : public RecursiveASTVisitor<CallGraphVisitor> {
    SourceManager     &SM;
    const std::string &target_usr;
    bool               outgoing;
    std::vector<CallEdgeEntry> &out;
    std::vector<const FunctionDecl *> func_stack;

    std::string funcUSR(const FunctionDecl *FD) const {
        SmallString<128> b; index::generateUSRForDecl(FD, b); return b.str().str();
    }
    bool inTarget() const {
        return !func_stack.empty() && funcUSR(func_stack.back()) == target_usr;
    }
public:
    CallGraphVisitor(ASTContext &C, const std::string &usr, bool out_flag,
                     std::vector<CallEdgeEntry> &o)
        : SM(C.getSourceManager()), target_usr(usr), outgoing(out_flag), out(o) {}
    bool shouldVisitTemplateInstantiations() const { return false; }

    bool TraverseFunctionDecl(FunctionDecl *FD) {
        func_stack.push_back(FD);
        RecursiveASTVisitor<CallGraphVisitor>::TraverseFunctionDecl(FD);
        func_stack.pop_back();
        return true;
    }
    bool TraverseCXXMethodDecl(CXXMethodDecl *MD) {
        func_stack.push_back(MD);
        RecursiveASTVisitor<CallGraphVisitor>::TraverseCXXMethodDecl(MD);
        func_stack.pop_back();
        return true;
    }

    bool VisitCallExpr(CallExpr *CE) {
        const FunctionDecl *callee = CE->getDirectCallee();
        if (!callee || func_stack.empty()) return true;
        auto callLoc = SM.getPresumedLoc(SM.getSpellingLoc(CE->getBeginLoc()));
        if (!callLoc.isValid()) return true;

        if (outgoing) {
            if (!inTarget()) return true;
            auto defLoc = SM.getPresumedLoc(callee->getLocation());
            CallEdgeEntry e;
            e.name      = callee->getNameAsString();
            e.detail    = callee->getQualifiedNameAsString();
            e.file      = defLoc.isValid() && defLoc.getFilename() ? defLoc.getFilename() : "";
            e.line      = defLoc.isValid() ? defLoc.getLine()   : 0;
            e.col       = defLoc.isValid()
                              ? source_location_utf16_col(SM, callee->getLocation())
                              : 0;
            e.usr       = funcUSR(callee);
            e.call_line = callLoc.getLine();
            e.call_col  = source_location_utf16_col(
                SM, SM.getSpellingLoc(CE->getBeginLoc()));
            out.push_back(std::move(e));
        } else {
            if (funcUSR(callee) != target_usr) return true;
            const FunctionDecl *caller = func_stack.back();
            auto callerLoc = SM.getPresumedLoc(caller->getLocation());
            CallEdgeEntry e;
            e.name      = caller->getNameAsString();
            e.detail    = caller->getQualifiedNameAsString();
            e.file      = callerLoc.isValid() && callerLoc.getFilename() ? callerLoc.getFilename() : "";
            e.line      = callerLoc.isValid() ? callerLoc.getLine()   : 0;
            e.col       = callerLoc.isValid()
                              ? source_location_utf16_col(SM, caller->getLocation())
                              : 0;
            e.usr       = funcUSR(caller);
            e.call_line = callLoc.getLine();
            e.call_col  = source_location_utf16_col(
                SM, SM.getSpellingLoc(CE->getBeginLoc()));
            out.push_back(std::move(e));
        }
        return true;
    }
};

CB_CallEdgeList *cb_incoming_calls(CB_TransUnit *tu, const char *usr) {
    auto *list = new CB_CallEdgeList{};
    if (!usr || !*usr) return list;
    CallGraphVisitor vis(tu->ast->getASTContext(), usr, false, list->edges);
    vis.TraverseDecl(tu->ast->getASTContext().getTranslationUnitDecl());
    return list;
}
CB_CallEdgeList *cb_outgoing_calls(CB_TransUnit *tu, const char *usr) {
    auto *list = new CB_CallEdgeList{};
    if (!usr || !*usr) return list;
    CallGraphVisitor vis(tu->ast->getASTContext(), usr, true, list->edges);
    vis.TraverseDecl(tu->ast->getASTContext().getTranslationUnitDecl());
    return list;
}
size_t cb_call_edge_count(const CB_CallEdgeList *list) { return list->edges.size(); }
void cb_call_edge_get(const CB_CallEdgeList *list, size_t i, CB_CallEdge *out) {
    auto *ml = const_cast<CB_CallEdgeList *>(list);
    ml->current   = ml->edges[i];
    out->name      = ml->current.name.c_str();
    out->detail    = ml->current.detail.c_str();
    out->file      = ml->current.file.c_str();
    out->line      = ml->current.line;
    out->col       = ml->current.col;
    out->usr       = ml->current.usr.c_str();
    out->call_line = ml->current.call_line;
    out->call_col  = ml->current.call_col;
}
void cb_call_edge_list_destroy(CB_CallEdgeList *list) { delete list; }

// ── Type hierarchy ────────────────────────────────────────────────────────────

struct CB_TypeHierItem {
    std::string name, detail, file, usr;
    uint32_t    line = 0, col = 0;
};

struct TypeHierEntry {
    std::string name, detail, file, usr;
    uint32_t    line = 0, col = 0;
};

struct CB_TypeHierList {
    std::vector<TypeHierEntry> entries;
    TypeHierEntry              current;
};

static const CXXRecordDecl *findRecordByUSR(ASTContext &Ctx, const std::string &usr) {
    struct Finder : RecursiveASTVisitor<Finder> {
        const std::string &target;
        const CXXRecordDecl *found = nullptr;
        bool shouldVisitTemplateInstantiations() const { return false; }
        Finder(const std::string &u) : target(u) {}
        bool VisitCXXRecordDecl(CXXRecordDecl *D) {
            SmallString<128> b; index::generateUSRForDecl(D, b);
            if (b.str() == target) { found = D; return false; }
            return true;
        }
    } f(usr);
    f.TraverseDecl(Ctx.getTranslationUnitDecl());
    return f.found;
}

CB_TypeHierItem *cb_type_hierarchy_prepare(CB_TransUnit *tu,
                                           uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    if (auto *TND = dyn_cast<TypedefNameDecl>(ND)) {
        QualType qt = TND->getUnderlyingType();
        if (auto *RT = qt->getAs<RecordType>()) ND = RT->getDecl();
    }
    const CXXRecordDecl *RD = dyn_cast<CXXRecordDecl>(ND);
    if (!RD) return nullptr;
    if (auto *def = RD->getDefinition()) RD = def;

    ASTContext    &Ctx = tu->ast->getASTContext();
    SourceManager &SM  = Ctx.getSourceManager();
    auto *item = new CB_TypeHierItem{};
    item->name   = RD->getNameAsString();
    item->detail = RD->getQualifiedNameAsString();
    SmallString<128> buf; index::generateUSRForDecl(RD, buf);
    item->usr = buf.str().str();
    auto ploc = SM.getPresumedLoc(RD->getLocation());
    if (ploc.isValid()) {
        item->file = ploc.getFilename() ? ploc.getFilename() : "";
        item->line = ploc.getLine();
        item->col  = source_location_utf16_col(SM, RD->getLocation());
    }
    return item;
}
void        cb_type_hier_item_destroy(CB_TypeHierItem *i) { delete i; }
const char *cb_type_hier_name  (const CB_TypeHierItem *i) { return i->name.c_str(); }
const char *cb_type_hier_detail(const CB_TypeHierItem *i) { return i->detail.c_str(); }
const char *cb_type_hier_file  (const CB_TypeHierItem *i) { return i->file.c_str(); }
uint32_t    cb_type_hier_line  (const CB_TypeHierItem *i) { return i->line; }
uint32_t    cb_type_hier_col   (const CB_TypeHierItem *i) { return i->col; }
const char *cb_type_hier_usr   (const CB_TypeHierItem *i) { return i->usr.c_str(); }

CB_TypeHierList *cb_supertypes(CB_TransUnit *tu, const char *usr) {
    auto *list = new CB_TypeHierList{};
    if (!usr || !*usr) return list;
    ASTContext    &Ctx = tu->ast->getASTContext();
    SourceManager &SM  = Ctx.getSourceManager();
    const CXXRecordDecl *RD = findRecordByUSR(Ctx, usr);
    if (!RD || !RD->isCompleteDefinition()) return list;
    for (const auto &base : RD->bases()) {
        const RecordType *RT = base.getType()->getAs<RecordType>();
        if (!RT) continue;
        const CXXRecordDecl *B = dyn_cast<CXXRecordDecl>(RT->getDecl());
        if (!B) continue;
        TypeHierEntry e;
        e.name   = B->getNameAsString();
        e.detail = B->getQualifiedNameAsString();
        SmallString<128> buf; index::generateUSRForDecl(B, buf);
        e.usr  = buf.str().str();
        auto p = SM.getPresumedLoc(B->getLocation());
        if (p.isValid()) {
            e.file = p.getFilename() ? p.getFilename() : "";
            e.line = p.getLine();
            e.col = source_location_utf16_col(SM, B->getLocation());
        }
        list->entries.push_back(std::move(e));
    }
    return list;
}

CB_TypeHierList *cb_subtypes(CB_TransUnit *tu, const char *usr) {
    auto *list = new CB_TypeHierList{};
    if (!usr || !*usr) return list;
    ASTContext    &Ctx = tu->ast->getASTContext();
    SourceManager &SM  = Ctx.getSourceManager();
    std::string    target(usr);
    struct SubFinder : RecursiveASTVisitor<SubFinder> {
        const std::string        &target;
        std::vector<TypeHierEntry> &out;
        SourceManager            &SM;
        bool shouldVisitTemplateInstantiations() const { return false; }
        SubFinder(const std::string &t, std::vector<TypeHierEntry> &o, SourceManager &sm)
            : target(t), out(o), SM(sm) {}
        bool VisitCXXRecordDecl(CXXRecordDecl *D) {
            if (!D->isCompleteDefinition()) return true;
            for (const auto &base : D->bases()) {
                const RecordType *RT = base.getType()->getAs<RecordType>();
                if (!RT) continue;
                SmallString<128> b; index::generateUSRForDecl(RT->getDecl(), b);
                if (b.str() != target) continue;
                TypeHierEntry e;
                e.name   = D->getNameAsString();
                e.detail = D->getQualifiedNameAsString();
                SmallString<128> db; index::generateUSRForDecl(D, db);
                e.usr  = db.str().str();
                auto p = SM.getPresumedLoc(D->getLocation());
                if (p.isValid()) {
                    e.file = p.getFilename() ? p.getFilename() : "";
                    e.line = p.getLine();
                    e.col = source_location_utf16_col(SM, D->getLocation());
                }
                out.push_back(std::move(e));
                break;
            }
            return true;
        }
    } sf(target, list->entries, SM);
    sf.TraverseDecl(Ctx.getTranslationUnitDecl());
    return list;
}

size_t cb_type_hier_count(const CB_TypeHierList *list) { return list->entries.size(); }
void cb_type_hier_get(const CB_TypeHierList *list, size_t i, CB_TypeHierEntry *out) {
    auto *ml = const_cast<CB_TypeHierList *>(list);
    ml->current = ml->entries[i];
    out->name   = ml->current.name.c_str();
    out->detail = ml->current.detail.c_str();
    out->file   = ml->current.file.c_str();
    out->line   = ml->current.line;
    out->col    = ml->current.col;
    out->usr    = ml->current.usr.c_str();
}
void cb_type_hier_list_destroy(CB_TypeHierList *list) { delete list; }
