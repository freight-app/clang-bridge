#include "cb_internal.h"


// ── Doc extraction ─────────────────────────────────────────────────────────────

struct DocEntry {
    std::string kind;
    std::string name;
    std::string usr;
    std::string brief;
    std::string full_comment;
    std::string signature;
    std::string file;
    uint32_t    line = 0;
};

class DocVisitor : public RecursiveASTVisitor<DocVisitor> {
public:
    ASTContext &Ctx;
    const SourceManager &SM;
    std::vector<DocEntry> entries;

    DocVisitor(ASTContext &C) : Ctx(C), SM(C.getSourceManager()) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    bool VisitNamedDecl(NamedDecl *ND) {
        // Skip pure re-declarations only when a definition already exists elsewhere.
        // For functions without a body (declarations in headers), still index them.
        if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
            // Index the first declaration; skip subsequent ones to avoid dupes.
            if (FD != FD->getFirstDecl()) return true;
        } else if (!isDefinition(ND)) {
            return true;
        }
        if (SM.isInSystemHeader(ND->getLocation())) return true;

        auto loc = SM.getPresumedLoc(ND->getLocation());
        if (!loc.isValid()) return true;

        const RawComment *RC = Ctx.getRawCommentForDeclNoCache(ND);
        if (!RC) return true;

        DocEntry e;
        e.kind = declKind(ND);
        e.name = ND->getQualifiedNameAsString();
        e.file = loc.getFilename() ? loc.getFilename() : "";
        e.line = (uint32_t)loc.getLine();

        {
            llvm::SmallString<128> buf;
            index::generateUSRForDecl(ND, buf);
            e.usr = buf.str().str();
        }

        e.full_comment = RC->getFormattedText(SM, Ctx.getDiagnostics());
        {
            std::istringstream ss(e.full_comment);
            std::string line;
            while (std::getline(ss, line)) {
                size_t s = line.find_first_not_of(" \t\r\n");
                if (s == std::string::npos) continue;
                e.brief = line.substr(s);
                break;
            }
        }
        e.signature = prettySignature(ND, Ctx);
        entries.push_back(std::move(e));
        return true;
    }
};

struct CB_DocIter {
    std::vector<DocEntry> entries;
    size_t pos = 0;
    DocEntry current;
};

CB_DocIter *cb_doc_extract(CB_TransUnit *tu) {
    auto *it = new CB_DocIter{};
    ASTContext &Ctx = tu->ast->getASTContext();
    DocVisitor V(Ctx);
    V.TraverseDecl(Ctx.getTranslationUnitDecl());
    it->entries = std::move(V.entries);
    return it;
}

int cb_doc_iter_next(CB_DocIter *it, CB_DocItem *out) {
    if (it->pos >= it->entries.size()) return 0;
    it->current = it->entries[it->pos++];
    out->kind         = it->current.kind.c_str();
    out->name         = it->current.name.c_str();
    out->usr          = it->current.usr.c_str();
    out->brief        = it->current.brief.c_str();
    out->full_comment = it->current.full_comment.c_str();
    out->signature    = it->current.signature.c_str();
    out->file         = it->current.file.c_str();
    out->line         = it->current.line;
    return 1;
}

void cb_doc_iter_destroy(CB_DocIter *it) { delete it; }



// ── Document symbols ──────────────────────────────────────────────────────────

struct DocSymEntry {
    std::string name, kind, detail;
    uint32_t range_start_line = 0, range_start_col = 0;
    uint32_t range_end_line   = 0, range_end_col   = 0;
    uint32_t sel_line = 0, sel_col = 0;
    int32_t  parent = -1;
};

struct CB_DocSymList {
    std::vector<DocSymEntry> entries;
    DocSymEntry current; // stable storage for cb_doc_sym_get pointers
};

class DocSymVisitor : public RecursiveASTVisitor<DocSymVisitor> {
    using Base = RecursiveASTVisitor<DocSymVisitor>;
public:
    ASTContext          &Ctx;
    const SourceManager &SM;
    std::vector<DocSymEntry> &entries;
    std::vector<int32_t>      parent_stack{-1};
    // Maps a Decl* (class/enum definition) → its index in `entries`, so that
    // out-of-class method definitions can locate their parent class entry.
    std::unordered_map<const Decl *, int32_t> decl_to_idx;

    DocSymVisitor(ASTContext &C, std::vector<DocSymEntry> &out)
        : Ctx(C), SM(C.getSourceManager()), entries(out) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    bool skip(const NamedDecl *ND) const {
        // Non-identifier names (constructors, operators, etc.) are not useful
        // in the document symbol outline and getName() asserts on them.
        if (!ND->getDeclName().isIdentifier()) return true;
        return SM.isInSystemHeader(ND->getLocation()) || ND->getName().empty();
    }

    // Add ND to entries with the given parent index (-2 = use parent_stack.back()).
    int32_t add(const NamedDecl *ND, int32_t parent = -2) {
        if (parent == -2) parent = parent_stack.back();
        DocSymEntry e;
        e.name   = ND->getNameAsString();
        e.kind   = declKind(ND);
        e.detail = prettySignature(ND, Ctx);
        e.parent = parent;
        auto sr = ND->getSourceRange();
        if (auto p = SM.getPresumedLoc(sr.getBegin()); p.isValid()) {
            e.range_start_line = p.getLine(); e.range_start_col = p.getColumn();
        }
        if (auto p = SM.getPresumedLoc(sr.getEnd()); p.isValid()) {
            e.range_end_line = p.getLine(); e.range_end_col = p.getColumn();
        }
        if (auto p = SM.getPresumedLoc(ND->getLocation()); p.isValid()) {
            e.sel_line = p.getLine(); e.sel_col = p.getColumn();
        }
        int32_t idx = static_cast<int32_t>(entries.size());
        decl_to_idx[ND] = idx;
        entries.push_back(std::move(e));
        return idx;
    }

    // ── Containers: add, push parent index, recurse, pop ─────────────────────

    bool TraverseNamespaceDecl(NamespaceDecl *D) {
        if (SM.isInSystemHeader(D->getLocation())) return true;
        if (!D->getName().empty()) {
            int32_t idx = add(D);
            parent_stack.push_back(idx);
            Base::TraverseNamespaceDecl(D);
            parent_stack.pop_back();
        } else {
            // Anonymous namespace: recurse but don't create an entry.
            Base::TraverseNamespaceDecl(D);
        }
        return true;
    }

    bool TraverseCXXRecordDecl(CXXRecordDecl *D) {
        if (skip(D)) return true;
        if (!D->isThisDeclarationADefinition()) return true;
        int32_t idx = add(D);
        parent_stack.push_back(idx);
        Base::TraverseCXXRecordDecl(D);
        parent_stack.pop_back();
        return true;
    }

    bool TraverseEnumDecl(EnumDecl *D) {
        if (skip(D)) return true;
        int32_t idx = add(D);
        parent_stack.push_back(idx);
        Base::TraverseEnumDecl(D);
        parent_stack.pop_back();
        return true;
    }

    // ── Leaves: add at current parent level; don't recurse into bodies ────────

    bool addFunction(FunctionDecl *D) {
        if (skip(D)) return true;
        // Skip in-class declarations when an out-of-class definition exists.
        if (!D->isThisDeclarationADefinition() && D->hasBody()) return true;

        // Out-of-class method definition: find the parent class entry via
        // decl_to_idx rather than using parent_stack (which would be -1 here).
        int32_t parent = parent_stack.back();
        if (auto *MD = dyn_cast<CXXMethodDecl>(D)) {
            if (const CXXRecordDecl *cls = MD->getParent()) {
                if (const CXXRecordDecl *def = cls->getDefinition()) {
                    auto it = decl_to_idx.find(def);
                    if (it != decl_to_idx.end()) parent = it->second;
                }
            }
        }
        add(D, parent);
        return true; // no body traversal
    }

    // RecursiveASTVisitor dispatches CXX subtypes to their own Traverse methods,
    // not to TraverseFunctionDecl.  Route all of them through addFunction.
    bool TraverseFunctionDecl(FunctionDecl *D)       { return addFunction(D); }
    bool TraverseCXXMethodDecl(CXXMethodDecl *D)     { return addFunction(D); }
    bool TraverseCXXConstructorDecl(CXXConstructorDecl *D) { return addFunction(D); }
    bool TraverseCXXDestructorDecl(CXXDestructorDecl *D)   { return addFunction(D); }
    bool TraverseCXXConversionDecl(CXXConversionDecl *D)   { return addFunction(D); }

    bool VisitFieldDecl(FieldDecl *D) {
        if (!skip(D)) add(D);
        return true;
    }

    bool VisitEnumConstantDecl(EnumConstantDecl *D) {
        if (!skip(D)) add(D);
        return true;
    }

    bool VisitTypedefNameDecl(TypedefNameDecl *D) {
        if (!skip(D)) add(D);
        return true;
    }

    bool VisitVarDecl(VarDecl *D) {
        if (skip(D)) return true;
        if (!D->isFileVarDecl() && !D->isStaticDataMember()) return true;
        add(D);
        return true;
    }
};

CB_DocSymList *cb_document_symbols(CB_TransUnit *tu) {
    auto *list = new CB_DocSymList{};
    ASTContext &Ctx = tu->ast->getASTContext();
    DocSymVisitor V(Ctx, list->entries);
    V.TraverseDecl(Ctx.getTranslationUnitDecl());
    return list;
}

size_t cb_doc_sym_count(const CB_DocSymList *list) {
    return list->entries.size();
}

void cb_doc_sym_get(const CB_DocSymList *list, size_t i, CB_DocSym *out) {
    auto *ml = const_cast<CB_DocSymList *>(list);
    ml->current = ml->entries[i];
    out->name             = ml->current.name.c_str();
    out->kind             = ml->current.kind.c_str();
    out->detail           = ml->current.detail.empty() ? nullptr : ml->current.detail.c_str();
    out->range_start_line = ml->current.range_start_line;
    out->range_start_col  = ml->current.range_start_col;
    out->range_end_line   = ml->current.range_end_line;
    out->range_end_col    = ml->current.range_end_col;
    out->sel_line         = ml->current.sel_line;
    out->sel_col          = ml->current.sel_col;
    out->parent           = ml->current.parent;
}

void cb_doc_sym_list_destroy(CB_DocSymList *list) { delete list; }



