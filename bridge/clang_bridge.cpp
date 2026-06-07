#include "clang_bridge.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Comment.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TemplateName.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Format/Format.h>
#include <clang/Index/IndexDataConsumer.h>
#include <clang/Index/IndexSymbol.h>
#include <clang/Index/IndexingAction.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace clang;
using namespace clang::tooling;

// ── Index ─────────────────────────────────────────────────────────────────────

struct CB_Index {
    std::string last_error; // set by cb_parse / cb_parse_unsaved on failure
};

CB_Index *cb_index_create() { return new CB_Index{}; }
void cb_index_destroy(CB_Index *idx) { delete idx; }

const char *cb_index_last_error(const CB_Index *idx) {
    return idx->last_error.empty() ? nullptr : idx->last_error.c_str();
}

// ── TransUnit ─────────────────────────────────────────────────────────────────

// Probe the installed clang resource directory once at startup.  Used to
// override ClangTool's auto-computed resource dir (which is relative to the
// freight binary and therefore wrong when freight is not co-located with clang).
static std::string find_clang_resource_dir() {
    FILE *f = popen("clang -print-resource-dir 2>/dev/null", "r");
    if (!f) return {};
    char buf[512];
    bool ok = fgets(buf, sizeof(buf), f) != nullptr;
    pclose(f);
    if (!ok) return {};
    std::string result(buf);
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    return result;
}

struct CB_TransUnit {
    std::unique_ptr<ASTUnit> ast;
};

CB_TransUnit *cb_parse(
    CB_Index   *idx,
    const char *source_file,
    const char *working_dir,
    const char * const *args,
    size_t nargs
) {
    std::vector<std::string> compile_args(args, args + nargs);
    const std::string dir = (working_dir && *working_dir) ? working_dir : ".";

    // FixedCompilationDatabase uses `dir` as the compilation working directory
    // so relative include paths like `-Iinc` resolve against the project root.
    FixedCompilationDatabase db(dir, compile_args);

    std::vector<std::string> sources{source_file};
    ClangTool tool(db, sources);
    tool.setPrintErrorMessage(false);

    // Force the correct installed resource dir to the END of the argument list
    // so it overrides whatever resource dir ClangTool auto-computes from the
    // freight binary location (which is wrong when freight != clang).
    static const std::string s_resource_dir = find_clang_resource_dir();
    if (!s_resource_dir.empty()) {
        tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
            CommandLineArguments{"-resource-dir", s_resource_dir},
            ArgumentInsertPosition::END
        ));
    }

    std::vector<std::unique_ptr<ASTUnit>> asts;
    tool.buildASTs(asts);
    if (asts.empty()) {
        if (idx) idx->last_error = "failed to build AST for: " + std::string(source_file);
        return nullptr;
    }

    if (asts[0] && asts[0]->getDiagnostics().hasFatalErrorOccurred()) {
        if (idx) idx->last_error =
            "fatal error building AST for: " + std::string(source_file);
        return nullptr;
    }

    auto *tu = new CB_TransUnit{};
    tu->ast = std::move(asts[0]);
    return tu;
}

/// Parse entirely from an in-memory buffer.  `virtual_path` names the
/// "file" (used in diagnostics); `contents`/`len` supply the source text.
/// Returns NULL on failure; call cb_index_last_error for details.
///
/// Implementation writes content to a temporary file, parses it, then deletes
/// the file.  This avoids VFS-overlay subtleties in ClangTool.
CB_TransUnit *cb_parse_unsaved(
    CB_Index   *idx,
    const char *virtual_path,
    const char *working_dir,
    const char *contents,
    size_t      len,
    const char * const *args,
    size_t      nargs
) {
    // Build a unique temp file name preserving the extension so Clang can
    // infer the language (e.g. .cpp → C++, .c → C).
    std::string vp_str(virtual_path);
    auto dot = vp_str.rfind('.');
    std::string ext = (dot != std::string::npos) ? vp_str.substr(dot) : ".cpp";
    std::string tmp_path = "/tmp/cb_unsaved_" + std::to_string(
        std::hash<std::string>{}(vp_str) ^ (size_t)(uintptr_t)contents
    ) + ext;
    {
        std::FILE *f = std::fopen(tmp_path.c_str(), "wb");
        if (!f) {
            if (idx) idx->last_error =
                "cannot open temp file: " + tmp_path;
            return nullptr;
        }
        std::fwrite(contents, 1, len, f);
        std::fclose(f);
    }

    std::vector<std::string> compile_args(args, args + nargs);
    const std::string dir = (working_dir && *working_dir) ? working_dir : ".";
    FixedCompilationDatabase db(dir, compile_args);
    std::vector<std::string> sources{tmp_path};
    ClangTool tool(db, sources);
    tool.setPrintErrorMessage(false);

    static const std::string s_resource_dir_unsaved = find_clang_resource_dir();
    if (!s_resource_dir_unsaved.empty()) {
        tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
            CommandLineArguments{"-resource-dir", s_resource_dir_unsaved},
            ArgumentInsertPosition::END
        ));
    }

    std::vector<std::unique_ptr<ASTUnit>> asts;
    tool.buildASTs(asts);
    std::remove(tmp_path.c_str());

    if (asts.empty()) {
        if (idx) idx->last_error =
            "failed to build AST for: " + std::string(virtual_path);
        return nullptr;
    }

    if (asts[0] && asts[0]->getDiagnostics().hasFatalErrorOccurred()) {
        if (idx) idx->last_error =
            "fatal error building AST for: " + std::string(virtual_path);
        return nullptr;
    }

    auto *tu = new CB_TransUnit{};
    tu->ast = std::move(asts[0]);
    return tu;
}

void cb_transunit_destroy(CB_TransUnit *tu) { delete tu; }

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

static std::string declKind(const NamedDecl *D) {
    if (isa<CXXMethodDecl>(D))         return "method";
    if (isa<FunctionDecl>(D))          return "function";
    if (isa<FieldDecl>(D))             return "field";
    if (auto *RD = dyn_cast<CXXRecordDecl>(D)) {
        if (RD->isUnion())  return "union";
        if (RD->isStruct()) return "struct";
        return "class";
    }
    if (auto *RD = dyn_cast<RecordDecl>(D)) {
        return RD->isUnion() ? "union" : "struct";
    }
    if (isa<EnumDecl>(D))              return "enum";
    if (isa<EnumConstantDecl>(D))      return "enumconst";
    if (isa<TypedefNameDecl>(D))       return "typedef";
    if (isa<VarDecl>(D))               return "var";
    if (isa<NamespaceDecl>(D))         return "namespace";
    return "unknown";
}

static std::string prettySignature(const NamedDecl *ND, ASTContext &Ctx) {
    PrintingPolicy PP(Ctx.getLangOpts());
    PP.SuppressScope = 0;
    PP.SuppressTagKeyword = 0;
    std::string sig;
    llvm::raw_string_ostream os(sig);
    ND->print(os, PP);
    return sig;
}

// Return true if this decl is a definition (not just a declaration).
static bool isDefinition(const NamedDecl *ND) {
    if (auto *FD = dyn_cast<FunctionDecl>(ND))
        return FD->isThisDeclarationADefinition();
    if (auto *RD = dyn_cast<CXXRecordDecl>(ND))
        return RD->isThisDeclarationADefinition();
    if (auto *VD = dyn_cast<VarDecl>(ND))
        return VD->isThisDeclarationADefinition();
    // Fields, enum constants, typedefs, namespaces — always "defining"
    return true;
}

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

// ── Diagnostics ───────────────────────────────────────────────────────────────

struct FixItEntry {
    uint32_t    start_line = 0, start_col = 0;
    uint32_t    end_line   = 0, end_col   = 0;
    std::string replacement; // empty = pure deletion
};

struct DiagEntry {
    std::string file;
    uint32_t    line     = 0;
    uint32_t    col      = 0;
    uint32_t    end_line = 0; // squiggle range end (same as line/col if no range)
    uint32_t    end_col  = 0;
    uint8_t     severity = 0;
    std::string message;
    std::string check_name;
    std::vector<FixItEntry> fixits;
};

struct CB_DiagIter {
    std::vector<DiagEntry> entries;
    size_t pos = 0;
    DiagEntry current; // stable storage for cb_diag_next pointer fields
};

CB_DiagIter *cb_diag_iter(CB_TransUnit *tu) {
    auto *it = new CB_DiagIter{};
    const SourceManager &SM = tu->ast->getSourceManager();
    for (auto it2 = tu->ast->stored_diag_begin();
         it2 != tu->ast->stored_diag_end(); ++it2) {
        const StoredDiagnostic &sd = *it2;
        DiagEntry e;

        auto ploc = SM.getPresumedLoc(sd.getLocation());
        if (ploc.isValid()) {
            e.file = ploc.getFilename() ? ploc.getFilename() : "";
            e.line = e.end_line = (uint32_t)ploc.getLine();
            e.col  = e.end_col  = (uint32_t)ploc.getColumn();
        }

        // Expand squiggle range from the first reported source range.
        for (const auto &range : sd.getRanges()) {
            auto sl = SM.getPresumedLoc(range.getBegin());
            auto el = SM.getPresumedLoc(range.getEnd());
            if (sl.isValid()) { e.line = sl.getLine(); e.col = sl.getColumn(); }
            if (el.isValid()) { e.end_line = el.getLine(); e.end_col = el.getColumn(); }
            break; // first range is sufficient
        }

        e.severity = (uint8_t)sd.getLevel();
        e.message  = sd.getMessage().str();

        // Collect fix-it hints (source replacements / quick-fixes).
        for (const auto &fixit : sd.getFixIts()) {
            FixItEntry f;
            auto sl = SM.getPresumedLoc(fixit.RemoveRange.getBegin());
            auto el = SM.getPresumedLoc(fixit.RemoveRange.getEnd());
            if (sl.isValid()) { f.start_line = sl.getLine(); f.start_col = sl.getColumn(); }
            if (el.isValid()) { f.end_line   = el.getLine(); f.end_col   = el.getColumn(); }
            f.replacement = fixit.CodeToInsert;
            e.fixits.push_back(std::move(f));
        }

        it->entries.push_back(std::move(e));
    }
    return it;
}

int cb_diag_next(CB_DiagIter *it, CB_Diag *out) {
    if (it->pos >= it->entries.size()) return 0;
    it->current = it->entries[it->pos++];
    out->file       = it->current.file.c_str();
    out->line       = it->current.line;
    out->col        = it->current.col;
    out->end_line   = it->current.end_line;
    out->end_col    = it->current.end_col;
    out->severity   = it->current.severity;
    out->message    = it->current.message.c_str();
    out->check_name = it->current.check_name.empty()
                    ? nullptr : it->current.check_name.c_str();
    return 1;
}

// Fix-it access — valid immediately after cb_diag_next returns 1.
size_t cb_diag_fixit_count(const CB_DiagIter *it) {
    return it->current.fixits.size();
}

void cb_diag_fixit_get(const CB_DiagIter *it, size_t i, CB_FixIt *out) {
    const auto &f = it->current.fixits[i];
    out->start_line  = f.start_line;
    out->start_col   = f.start_col;
    out->end_line    = f.end_line;
    out->end_col     = f.end_col;
    out->replacement = f.replacement.c_str();
}

void cb_diag_iter_destroy(CB_DiagIter *it) { delete it; }

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

// Forward declaration — defined in the "Symbol lookup" section below.
static const NamedDecl *locate_symbol_at(ASTUnit *ast, uint32_t line, uint32_t col);

// ── Inlay hints ───────────────────────────────────────────────────────────────

struct InlayHintEntry {
    uint32_t    line, col; // 1-based; hint is displayed BEFORE this position
    std::string label;
    uint8_t     kind;      // 0 = parameter name, 1 = deduced type
};

struct CB_InlayHintList {
    std::vector<InlayHintEntry> entries;
    InlayHintEntry current; // stable storage for getter return values
};

class InlayHintVisitor : public RecursiveASTVisitor<InlayHintVisitor> {
public:
    ASTContext          &Ctx;
    const SourceManager &SM;
    std::vector<InlayHintEntry> &hints;
    uint32_t start_line, end_line;

    InlayHintVisitor(ASTContext &C, std::vector<InlayHintEntry> &out,
                     uint32_t sl, uint32_t el)
        : Ctx(C), SM(C.getSourceManager()), hints(out),
          start_line(sl), end_line(el) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    bool inViewport(SourceLocation loc) const {
        auto p = SM.getPresumedLoc(SM.getSpellingLoc(loc));
        if (!p.isValid()) return false;
        return p.getLine() >= start_line && p.getLine() <= end_line;
    }

    // ── Parameter name hints at call sites ────────────────────────────────────

    bool VisitCallExpr(CallExpr *E) {
        // Skip operator-call expressions — they clutter the view.
        if (isa<CXXOperatorCallExpr>(E)) return true;

        const FunctionDecl *FD = E->getDirectCallee();
        if (!FD) return true;

        auto pbegin = FD->param_begin();
        auto pend   = FD->param_end();
        unsigned i = 0;
        for (auto *arg : E->arguments()) {
            if (pbegin + i >= pend) break;
            const ParmVarDecl *PD = *(pbegin + i);
            ++i;
            if (PD->getName().empty()) continue;
            if (!inViewport(arg->getBeginLoc())) continue;

            // Suppress the hint when the argument is already the same name
            // (e.g. `f(x)` where param is also named x).
            if (auto *DRE = dyn_cast<DeclRefExpr>(arg->IgnoreParenImpCasts()))
                if (DRE->getDecl()->getName() == PD->getName()) continue;

            auto p = SM.getPresumedLoc(SM.getSpellingLoc(arg->getBeginLoc()));
            if (!p.isValid()) continue;
            InlayHintEntry h;
            h.line  = p.getLine();
            h.col   = p.getColumn();
            h.label = PD->getNameAsString() + ":";
            h.kind  = 0;
            hints.push_back(std::move(h));
        }
        return true;
    }

    // ── Deduced-type hints for `auto` variable declarations ───────────────────

    bool VisitVarDecl(VarDecl *D) {
        if (SM.isInSystemHeader(D->getLocation())) return true;
        if (!inViewport(D->getLocation())) return true;
        if (isa<ParmVarDecl>(D)) return true; // parameters are always typed

        // Check if the variable was declared with `auto`.
        const AutoType *AT = D->getTypeSourceInfo()
                               ->getType()->getContainedAutoType();
        if (!AT) return true;

        QualType deduced = D->getType();
        if (deduced->isUndeducedAutoType()) return true; // not yet deduced

        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressScope = 1; // keep it concise
        std::string typeStr = deduced.getAsString(PP);
        if (typeStr == "auto" || typeStr.empty()) return true;

        auto p = SM.getPresumedLoc(D->getLocation());
        if (!p.isValid()) return true;
        InlayHintEntry h;
        h.line  = p.getLine();
        h.col   = p.getColumn();
        h.label = ": " + typeStr;
        h.kind  = 1;
        hints.push_back(std::move(h));
        return true;
    }
};

CB_InlayHintList *cb_inlay_hints(CB_TransUnit *tu,
                                   uint32_t start_line, uint32_t end_line) {
    auto *list = new CB_InlayHintList{};
    ASTContext &Ctx = tu->ast->getASTContext();
    InlayHintVisitor V(Ctx, list->entries, start_line, end_line);
    V.TraverseDecl(Ctx.getTranslationUnitDecl());
    return list;
}

size_t cb_inlay_hint_count(const CB_InlayHintList *list) {
    return list->entries.size();
}

void cb_inlay_hint_get(const CB_InlayHintList *list, size_t i, CB_InlayHint *out) {
    auto *ml = const_cast<CB_InlayHintList *>(list);
    ml->current = ml->entries[i];
    out->line  = ml->current.line;
    out->col   = ml->current.col;
    out->label = ml->current.label.c_str();
    out->kind  = ml->current.kind;
}

void cb_inlay_hint_list_destroy(CB_InlayHintList *list) { delete list; }

// ── Type at cursor ─────────────────────────────────────────────────────────────

/// Return the type string for the variable/field/parameter at (line, col),
/// or NULL.  Useful for enriching hover when no doc comment exists.
char *cb_type_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    const NamedDecl *ND = locate_symbol_at(tu->ast.get(), line, col);
    if (!ND) return nullptr;
    ASTContext &Ctx = tu->ast->getASTContext();

    QualType qt;
    if (auto *VD = dyn_cast<VarDecl>(ND))        qt = VD->getType();
    else if (auto *FD = dyn_cast<FieldDecl>(ND)) qt = FD->getType();
    else                                          return nullptr;

    if (qt.isNull()) return nullptr;
    PrintingPolicy PP(Ctx.getLangOpts());
    PP.SuppressScope = 0;
    std::string s = qt.getAsString(PP);
    return s.empty() ? nullptr : strdup(s.c_str());
}

// ── Macro hover ───────────────────────────────────────────────────────────────

/// Return a Markdown hover string for the macro at (line, col), or NULL if
/// (line, col) is not a macro use site.  Caller must free with cb_free_string().
char *cb_macro_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    ASTContext &Ctx = tu->ast->getASTContext();
    const SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO   = Ctx.getLangOpts();

    // Translate 1-based (line, col) → SourceLocation in the main file.
    SourceLocation target =
        SM.translateLineCol(SM.getMainFileID(), line, col);
    if (!target.isValid()) return nullptr;

    // Lex the raw token at the cursor position.
    Token tok;
    if (Lexer::getRawToken(target, tok, SM, LO, /*IgnoreWhiteSpace=*/false))
        return nullptr;
    if (tok.getKind() != tok::identifier && tok.getKind() != tok::raw_identifier)
        return nullptr;

    // getIdentifierInfo() asserts on tok::raw_identifier — look it up by spelling.
    const IdentifierInfo *II = (tok.getKind() == tok::identifier)
                                   ? tok.getIdentifierInfo()
                                   : nullptr;
    if (!II) {
        std::string spelling = Lexer::getSpelling(tok, SM, LO);
        if (spelling.empty()) return nullptr;
        II = &Ctx.Idents.get(spelling);
    }
    if (!II) return nullptr;

    Preprocessor &PP = tu->ast->getPreprocessor();
    const MacroInfo *MI = PP.getMacroInfo(II);
    if (!MI) return nullptr;

    // Build the Markdown string.
    std::string md;
    md += "```cpp\n#define ";
    md += II->getName().str();

    if (!MI->isObjectLike()) {
        // Function-like macro: show parameter list.
        md += "(";
        bool first = true;
        for (const IdentifierInfo *param : MI->params()) {
            if (!first) md += ", ";
            md += param->getName().str();
            first = false;
        }
        if (MI->isVariadic()) md += first ? "..." : ", ...";
        md += ")";
    }
    md += " ";

    // Append expansion tokens.
    for (const Token &t : MI->tokens()) {
        md += Lexer::getSpelling(t, SM, LO);
        md += " ";
    }
    md += "\n```";

    // Definition location footer.
    auto defLoc = SM.getPresumedLoc(MI->getDefinitionLoc());
    if (defLoc.isValid() && defLoc.getFilename()) {
        md += "\n\n---\n*Defined in `";
        md += defLoc.getFilename();
        md += ":";
        md += std::to_string(defLoc.getLine());
        md += "`*";
    }

    return strdup(md.c_str());
}

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

// Visitor: matches expression reference sites (DeclRefExpr, MemberExpr, TypeLoc).
// This is Stage 1 — covers the common case where the cursor is on a *use*, not a
// declaration.
class RefLocator : public RecursiveASTVisitor<RefLocator> {
public:
    const SourceManager &SM;
    uint32_t target_line, target_col;
    const NamedDecl *found = nullptr;

    RefLocator(const SourceManager &SM, uint32_t l, uint32_t c)
        : SM(SM), target_line(l), target_col(c) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    // Returns true when (target_line, target_col) lands inside the token at
    // tokenLoc of the given length.
    bool inToken(SourceLocation tokenLoc, size_t nameLen) const {
        if (nameLen == 0) return false;
        auto ploc = SM.getPresumedLoc(SM.getSpellingLoc(tokenLoc));
        if (!ploc.isValid()) return false;
        uint32_t startCol = (uint32_t)ploc.getColumn();
        return (uint32_t)ploc.getLine() == target_line &&
               target_col >= startCol &&
               target_col < startCol + (uint32_t)nameLen;
    }

    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (!found) {
            const NamedDecl *D = E->getFoundDecl();
            if (inToken(E->getLocation(), D->getName().size()))
                found = D;
        }
        return true;
    }

    bool VisitMemberExpr(MemberExpr *E) {
        if (!found) {
            const NamedDecl *D = E->getMemberDecl();
            if (inToken(E->getMemberLoc(), D->getName().size()))
                found = D;
        }
        return true;
    }

    bool VisitTagTypeLoc(TagTypeLoc TL) {
        if (!found) {
            const TagDecl *D = TL.getDecl();
            if (!D->getName().empty() && inToken(TL.getNameLoc(), D->getName().size()))
                found = D;
        }
        return true;
    }

    bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
        if (!found) {
            const TypedefNameDecl *D = TL.getTypePtr()->getDecl();
            if (inToken(TL.getNameLoc(), D->getName().size()))
                found = D;
        }
        return true;
    }

    bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc TL) {
        if (!found) {
            TemplateName TN = TL.getTypePtr()->getTemplateName();
            if (TemplateDecl *TD = TN.getAsTemplateDecl()) {
                if (inToken(TL.getTemplateNameLoc(), TD->getName().size()))
                    found = TD;
            }
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

    bool VisitNamedDecl(NamedDecl *ND) {
        auto ploc = SM.getPresumedLoc(ND->getLocation());
        if (!ploc.isValid()) return true;
        if ((uint32_t)ploc.getLine() == target_line &&
            (uint32_t)ploc.getColumn() <= target_col)
            found = ND;
        return true;
    }
};

// Shared symbol resolution: finds the NamedDecl at (1-based) line/col.
// Checks expression reference nodes first, then declaration sites.
static const NamedDecl *locate_symbol_at(ASTUnit *ast, uint32_t line, uint32_t col) {
    ASTContext &Ctx = ast->getASTContext();
    const SourceManager &SM = Ctx.getSourceManager();

    RefLocator refV(SM, line, col);
    refV.TraverseDecl(Ctx.getTranslationUnitDecl());
    if (refV.found) return refV.found;

    DeclLocator declV(SM, line, col);
    declV.TraverseDecl(Ctx.getTranslationUnitDecl());
    return declV.found;
}

CB_Symbol *cb_symbol_at(CB_TransUnit *tu, uint32_t line, uint32_t col) {
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
}

const char *cb_symbol_name(const CB_Symbol *s)      { return s->name.c_str(); }
const char *cb_symbol_usr(const CB_Symbol *s)       { return s->usr.c_str(); }
const char *cb_symbol_kind(const CB_Symbol *s)      { return s->kind.c_str(); }
const char *cb_symbol_brief(const CB_Symbol *s)     { return s->brief.c_str(); }
const char *cb_symbol_signature(const CB_Symbol *s) { return s->signature.c_str(); }
const char *cb_symbol_def_file(const CB_Symbol *s)  { return s->def_file.c_str(); }
uint32_t    cb_symbol_def_line(const CB_Symbol *s)  { return s->def_line; }
void        cb_symbol_destroy(CB_Symbol *s)         { delete s; }

#include <clang/Frontend/FrontendActions.h>
#include <clang/Sema/CodeCompleteConsumer.h>
#include <clang/Sema/Sema.h>

// ── Free helpers ──────────────────────────────────────────────────────────────

void cb_free_string(char *s) { free(s); }

// ── Reparse ───────────────────────────────────────────────────────────────────

int cb_transunit_reparse(CB_TransUnit *tu, const char *buf, size_t len) {
    std::vector<ASTUnit::RemappedFile> remapped;
    std::unique_ptr<llvm::MemoryBuffer> owned_buf;
    if (buf && len > 0) {
        StringRef main = tu->ast->getMainFileName();
        owned_buf = llvm::MemoryBuffer::getMemBufferCopy(StringRef(buf, len), main);
        remapped.emplace_back(main.str(), owned_buf.get());
    }
    return tu->ast->Reparse(std::make_shared<PCHContainerOperations>(),
                            remapped) ? 0 : 1;
}

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

struct CompletionEntry {
    std::string label;
    uint8_t     kind;   // LSP CompletionItemKind
    std::string detail;
    std::string documentation;
};

// Map CXCursorKind-equivalent to LSP CompletionItemKind
static uint8_t lspKindFor(CodeCompletionResult::ResultKind rk,
                           CXCursorKind ck) {
    if (rk == CodeCompletionResult::RK_Keyword)  return 14; // Keyword
    if (rk == CodeCompletionResult::RK_Macro)    return 15; // Snippet-ish, use Text
    switch (ck) {
        case CXCursor_FunctionDecl:
        case CXCursor_CXXMethod:
        case CXCursor_FunctionTemplate:  return 3;  // Function
        case CXCursor_Constructor:       return 4;  // Constructor
        case CXCursor_FieldDecl:         return 5;  // Field
        case CXCursor_VarDecl:
        case CXCursor_ParmDecl:          return 6;  // Variable
        case CXCursor_ClassDecl:
        case CXCursor_StructDecl:
        case CXCursor_ClassTemplate:     return 7;  // Class
        case CXCursor_EnumDecl:          return 13; // Enum
        case CXCursor_EnumConstantDecl:  return 20; // EnumMember
        case CXCursor_Namespace:         return 9;  // Module
        case CXCursor_TypedefDecl:
        case CXCursor_TypeAliasDecl:     return 25; // TypeParameter (closest)
        default:                         return 1;  // Text
    }
}

class BridgeCodeCompleteConsumer : public CodeCompleteConsumer {
public:
    std::shared_ptr<GlobalCodeCompletionAllocator> Alloc;
    CodeCompletionTUInfo TUInfo;
    std::vector<CompletionEntry> results;

    BridgeCodeCompleteConsumer()
        : CodeCompleteConsumer(CodeCompleteOptions()),
          Alloc(std::make_shared<GlobalCodeCompletionAllocator>()),
          TUInfo(Alloc) {}

    CodeCompletionAllocator &getAllocator() override { return *Alloc; }
    CodeCompletionTUInfo &getCodeCompletionTUInfo() override { return TUInfo; }

    void ProcessCodeCompleteResults(Sema &S,
                                    CodeCompletionContext ctx,
                                    CodeCompletionResult *Results,
                                    unsigned NumResults) override {
        for (unsigned i = 0; i < NumResults; ++i) {
            auto &R = Results[i];
            if (R.Availability == CXAvailability_NotAvailable) continue;

            CompletionEntry e;
            e.kind = lspKindFor(R.Kind, R.CursorKind);

            if (R.Kind == CodeCompletionResult::RK_Keyword) {
                e.label = R.Keyword;
            } else if (R.Kind == CodeCompletionResult::RK_Macro) {
                e.label = R.Macro->getName().str();
            } else {
                CodeCompletionString *CCS = nullptr;
                if (R.Kind == CodeCompletionResult::RK_Pattern) {
                    CCS = R.Pattern;
                } else {
                    CCS = R.CreateCodeCompletionString(
                        S, ctx, *Alloc, TUInfo,
                        /*IncludeBriefComments=*/true);
                }
                if (!CCS) continue;
                for (auto &chunk : *CCS) {
                    if (chunk.Kind == CodeCompletionString::CK_TypedText)
                        e.label = chunk.Text ? chunk.Text : "";
                    else if (chunk.Kind == CodeCompletionString::CK_ResultType)
                        e.detail = chunk.Text ? chunk.Text : "";
                }
                if (const char *brief = CCS->getBriefComment())
                    e.documentation = brief;
            }
            if (e.label.empty()) continue;
            results.push_back(std::move(e));
        }
    }
};

struct CB_CompletionIter {
    std::vector<CompletionEntry> entries;
    size_t pos = 0;
    CompletionEntry current;
};

CB_CompletionIter *cb_complete(CB_TransUnit *tu,
                                uint32_t line, uint32_t col,
                                const char *unsaved_buf, size_t unsaved_len) {
    auto *it = new CB_CompletionIter{};

    auto consumer = std::make_unique<BridgeCodeCompleteConsumer>();
    BridgeCodeCompleteConsumer *consumerPtr = consumer.get();

    StringRef main_file = tu->ast->getMainFileName();

    std::vector<ASTUnit::RemappedFile> remapped;
    std::unique_ptr<llvm::MemoryBuffer> owned_buf;
    if (unsaved_buf && unsaved_len > 0) {
        owned_buf = llvm::MemoryBuffer::getMemBufferCopy(
            StringRef(unsaved_buf, unsaved_len), main_file);
        remapped.emplace_back(main_file.str(), owned_buf.get());
    }

    SmallVector<StoredDiagnostic, 4> stored_diags;
    SmallVector<const llvm::MemoryBuffer *, 4> owned_buffers;

    LangOptions lang_opts = tu->ast->getASTContext().getLangOpts();

    tu->ast->CodeComplete(
        main_file.str(), line, col,
        remapped,
        /*IncludeMacros=*/true,
        /*IncludeCodePatterns=*/false,
        /*IncludeBriefComments=*/true,
        *consumer,
        std::make_shared<PCHContainerOperations>(),
        tu->ast->getDiagnosticsPtr(),
        lang_opts,
        tu->ast->getSourceManagerPtr(),
        tu->ast->getFileManagerPtr(),
        stored_diags,
        owned_buffers,
        /*Act=*/nullptr
    );

    it->entries = std::move(consumerPtr->results);
    return it;
}

int cb_completion_next(CB_CompletionIter *it, CB_CompletionItem *out) {
    if (it->pos >= it->entries.size()) return 0;
    it->current = it->entries[it->pos++];
    out->label         = it->current.label.c_str();
    out->kind          = it->current.kind;
    out->detail        = it->current.detail.empty()   ? nullptr : it->current.detail.c_str();
    out->documentation = it->current.documentation.empty() ? nullptr : it->current.documentation.c_str();
    return 1;
}

void cb_completion_iter_destroy(CB_CompletionIter *it) { delete it; }

// ── Signature help ────────────────────────────────────────────────────────────

struct SigParam {
    std::string label; // "type name" or just "type"
};

struct SigOverload {
    std::string           label;  // full function signature
    std::vector<SigParam> params;
};

struct CB_SigHelp {
    uint32_t                active_param = 0;
    std::vector<SigOverload> overloads;
    // Stable storage for getter return values
    std::string  current_label;
    std::string  current_param;
};

/// CodeCompleteConsumer that only collects overload candidates.
class BridgeSigHelpConsumer : public CodeCompleteConsumer {
public:
    std::shared_ptr<GlobalCodeCompletionAllocator> Alloc;
    CodeCompletionTUInfo TUInfo;
    uint32_t             active_param = 0;
    std::vector<SigOverload> overloads;

    BridgeSigHelpConsumer()
        : CodeCompleteConsumer(CodeCompleteOptions{}),
          Alloc(std::make_shared<GlobalCodeCompletionAllocator>()),
          TUInfo(Alloc) {}

    CodeCompletionAllocator &getAllocator() override { return *Alloc; }
    CodeCompletionTUInfo    &getCodeCompletionTUInfo() override { return TUInfo; }

    // Not used for signature help, but must be overridden.
    void ProcessCodeCompleteResults(Sema &, CodeCompletionContext,
                                    CodeCompletionResult *, unsigned) override {}

    void ProcessOverloadCandidates(Sema &S, unsigned CurrentArg,
                                   OverloadCandidate *Candidates,
                                   unsigned NumCandidates,
                                   SourceLocation /*OpenParLoc*/,
                                   bool /*Braced*/) override {
        active_param = CurrentArg;
        PrintingPolicy PP(S.getASTContext().getLangOpts());
        PP.SuppressScope = 0;

        for (unsigned i = 0; i < NumCandidates; ++i) {
            auto &cand = Candidates[i];
            const FunctionDecl *FD = cand.getFunction();
            if (!FD) continue;

            SigOverload ov;
            {
                std::string label;
                llvm::raw_string_ostream os(label);
                FD->print(os, PP);
                ov.label = label;
            }
            for (const ParmVarDecl *PD : FD->parameters()) {
                std::string pstr;
                llvm::raw_string_ostream pos(pstr);
                PD->print(pos, PP);
                ov.params.push_back({pstr});
            }
            overloads.push_back(std::move(ov));
        }
    }
};

CB_SigHelp *cb_signature_help(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    auto consumer = std::make_unique<BridgeSigHelpConsumer>();
    auto *cp = consumer.get();

    StringRef main_file = tu->ast->getMainFileName();
    SmallVector<StoredDiagnostic, 4> stored_diags;
    SmallVector<const llvm::MemoryBuffer *, 4> owned_buffers;
    LangOptions lang_opts = tu->ast->getASTContext().getLangOpts();

    tu->ast->CodeComplete(
        main_file.str(), line, col,
        /*RemappedFiles=*/{},
        /*IncludeMacros=*/false,
        /*IncludeCodePatterns=*/false,
        /*IncludeBriefComments=*/false,
        *consumer,
        std::make_shared<PCHContainerOperations>(),
        tu->ast->getDiagnosticsPtr(),
        lang_opts,
        tu->ast->getSourceManagerPtr(),
        tu->ast->getFileManagerPtr(),
        stored_diags,
        owned_buffers,
        /*Act=*/nullptr
    );

    if (cp->overloads.empty()) return nullptr;

    auto *sh = new CB_SigHelp{};
    sh->active_param = cp->active_param;
    sh->overloads    = std::move(cp->overloads);
    return sh;
}

uint32_t cb_sig_help_active_param(const CB_SigHelp *sh) {
    return sh->active_param;
}

size_t cb_sig_help_overload_count(const CB_SigHelp *sh) {
    return sh->overloads.size();
}

const char *cb_sig_help_label(CB_SigHelp *sh, size_t overload_i) {
    sh->current_label = sh->overloads[overload_i].label;
    return sh->current_label.c_str();
}

size_t cb_sig_help_param_count(const CB_SigHelp *sh, size_t overload_i) {
    return sh->overloads[overload_i].params.size();
}

const char *cb_sig_help_param_label(CB_SigHelp *sh, size_t overload_i, size_t param_i) {
    sh->current_param = sh->overloads[overload_i].params[param_i].label;
    return sh->current_param.c_str();
}

void cb_sig_help_destroy(CB_SigHelp *sh) { delete sh; }

// ── #include graph ────────────────────────────────────────────────────────────

struct InclusionEntry {
    std::string including_file;
    std::string included_file;
    uint32_t    line;         // directive line in including_file (1-based)
    uint32_t    start_col;    // column of the opening quote/angle (1-based)
    uint32_t    end_col;      // column past the closing quote/angle (1-based)
};

struct CB_InclusionList {
    std::vector<InclusionEntry> entries;
    InclusionEntry current;
};

/// PPCallbacks subclass that records every #include directive after the TU is
/// already parsed.  We walk the stored InclusionDirective records from the
/// SourceManager rather than registering at parse time.
static void collect_inclusions(ASTUnit *ast,
                               std::vector<InclusionEntry> &out) {
    const SourceManager &SM = ast->getSourceManager();
    // ASTUnit exposes getLocalTopLevelDecls — for inclusions we iterate all
    // FileIDs and query for SLocEntry inclusion records.
    unsigned n = SM.local_sloc_entry_size();
    for (unsigned i = 1; i < n; ++i) {
        const SrcMgr::SLocEntry &entry = SM.getLocalSLocEntry(i);
        if (!entry.isFile()) continue;
        const SrcMgr::FileInfo &FI = entry.getFile();
        if (FI.getIncludeLoc().isInvalid()) continue; // main file

        // The include location is in the including file.
        SourceLocation incLoc = FI.getIncludeLoc();
        auto presumed = SM.getPresumedLoc(incLoc);
        if (!presumed.isValid()) continue;

        // Resolve the included filename from the ContentCache.
        const SrcMgr::ContentCache *CC = &FI.getContentCache();
        if (!CC->OrigEntry) continue;
        std::string included = CC->OrigEntry->getName().str();
        if (included.empty()) continue;

        // Compute column range of the path literal (heuristic: scan the line).
        InclusionEntry e;
        e.including_file = presumed.getFilename() ? presumed.getFilename() : "";
        e.included_file  = included;
        e.line      = presumed.getLine();
        e.start_col = presumed.getColumn();
        e.end_col   = presumed.getColumn(); // refined below if possible
        out.push_back(std::move(e));
    }
}

CB_InclusionList *cb_inclusions(CB_TransUnit *tu) {
    auto *list = new CB_InclusionList{};
    collect_inclusions(tu->ast.get(), list->entries);
    return list;
}

size_t cb_inclusion_count(const CB_InclusionList *list) {
    return list->entries.size();
}

void cb_inclusion_get(const CB_InclusionList *list, size_t i,
                      CB_Inclusion *out) {
    auto *ml = const_cast<CB_InclusionList *>(list);
    ml->current = ml->entries[i];
    out->including_file = ml->current.including_file.c_str();
    out->included_file  = ml->current.included_file.c_str();
    out->line       = ml->current.line;
    out->start_col  = ml->current.start_col;
    out->end_col    = ml->current.end_col;
}

void cb_inclusion_list_destroy(CB_InclusionList *list) { delete list; }

// ── Semantic tokens ───────────────────────────────────────────────────────────

struct SemanticTokenEntry {
    uint32_t    line, col, length; // 1-based line/col; length in UTF-16 code units
    uint8_t     token_type;        // CB_TokenType values
};

struct CB_SemanticTokenList {
    std::vector<SemanticTokenEntry> tokens;
    SemanticTokenEntry current;
};

/// Map a NamedDecl to the LSP/CB semantic token type.
static uint8_t semtokForDecl(const NamedDecl *D) {
    if (isa<NamespaceDecl>(D))      return CB_TOK_NAMESPACE;
    if (isa<TypedefNameDecl>(D))    return CB_TOK_TYPE;
    if (isa<CXXRecordDecl>(D) ||
        isa<RecordDecl>(D) ||
        isa<EnumDecl>(D))           return CB_TOK_TYPE;
    if (isa<EnumConstantDecl>(D))   return CB_TOK_ENUM_MEMBER;
    if (isa<FieldDecl>(D))          return CB_TOK_PROPERTY;
    if (isa<ParmVarDecl>(D))        return CB_TOK_PARAMETER;
    if (isa<VarDecl>(D))            return CB_TOK_VARIABLE;
    if (isa<CXXMethodDecl>(D))      return CB_TOK_METHOD;
    if (isa<FunctionDecl>(D))       return CB_TOK_FUNCTION;
    return CB_TOK_VARIABLE;
}

class SemanticTokenVisitor
    : public RecursiveASTVisitor<SemanticTokenVisitor> {
public:
    ASTContext           &Ctx;
    const SourceManager  &SM;
    std::vector<SemanticTokenEntry> &tokens;

    SemanticTokenVisitor(ASTContext &C, std::vector<SemanticTokenEntry> &out)
        : Ctx(C), SM(C.getSourceManager()), tokens(out) {}

    bool shouldVisitTemplateInstantiations() const { return false; }

    void emitToken(SourceLocation loc, const NamedDecl *D) {
        loc = SM.getSpellingLoc(loc);
        if (SM.isInSystemHeader(loc)) return;
        auto p = SM.getPresumedLoc(loc);
        if (!p.isValid()) return;

        // Length = number of characters in the name.
        StringRef name = D->getName();
        if (name.empty()) return;

        SemanticTokenEntry t;
        t.line       = p.getLine();
        t.col        = p.getColumn();
        t.length     = (uint32_t)name.size();
        t.token_type = semtokForDecl(D);
        tokens.push_back(t);
    }

    // Declarations
    bool VisitNamedDecl(NamedDecl *D) {
        if (D->isImplicit()) return true;
        emitToken(D->getLocation(), D);
        return true;
    }

    // Reference sites
    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (auto *ND = dyn_cast<NamedDecl>(E->getDecl()))
            emitToken(E->getLocation(), ND);
        return true;
    }
    bool VisitMemberExpr(MemberExpr *E) {
        if (auto *ND = dyn_cast<NamedDecl>(E->getMemberDecl()))
            emitToken(E->getMemberLoc(), ND);
        return true;
    }
};

CB_SemanticTokenList *cb_semantic_tokens(CB_TransUnit *tu) {
    auto *list = new CB_SemanticTokenList{};
    ASTContext &Ctx = tu->ast->getASTContext();

    SemanticTokenVisitor V(Ctx, list->tokens);
    V.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Annotate macro use sites as CB_TOK_MACRO.
    const SourceManager &SM = Ctx.getSourceManager();
    Preprocessor &PP = tu->ast->getPreprocessor();
    // Walk all macro expansions in the main file via the SourceManager.
    unsigned n = SM.local_sloc_entry_size();
    for (unsigned i = 1; i < n; ++i) {
        const SrcMgr::SLocEntry &sle = SM.getLocalSLocEntry(i);
        if (sle.isFile()) continue; // only expansion records
        SourceLocation expansionLoc = SM.getExpansionLoc(sle.getExpansion().getExpansionLocStart());
        if (SM.isInSystemHeader(expansionLoc)) continue;
        auto p = SM.getPresumedLoc(expansionLoc);
        if (!p.isValid()) continue;

        // Look up the macro name at the expansion start.
        SourceLocation spellLoc = SM.getSpellingLoc(sle.getExpansion().getSpellingLoc());
        Token tok;
        if (Lexer::getRawToken(spellLoc, tok, SM, Ctx.getLangOpts(), false))
            continue;
        std::string spelling = Lexer::getSpelling(tok, SM, Ctx.getLangOpts());
        if (spelling.empty()) continue;
        const IdentifierInfo *II = PP.getIdentifierInfo(spelling);
        if (!II || !PP.getMacroInfo(II)) continue;

        SemanticTokenEntry t;
        t.line       = p.getLine();
        t.col        = p.getColumn();
        t.length     = (uint32_t)spelling.size();
        t.token_type = CB_TOK_MACRO;
        list->tokens.push_back(t);
    }

    // Sort by file position so callers get a predictable order.
    std::sort(list->tokens.begin(), list->tokens.end(),
              [](const SemanticTokenEntry &a, const SemanticTokenEntry &b) {
                  return a.line != b.line ? a.line < b.line : a.col < b.col;
              });
    return list;
}

size_t cb_semantic_token_count(const CB_SemanticTokenList *list) {
    return list->tokens.size();
}

void cb_semantic_token_get(const CB_SemanticTokenList *list, size_t i,
                           CB_SemanticToken *out) {
    auto *ml = const_cast<CB_SemanticTokenList *>(list);
    ml->current  = ml->tokens[i];
    out->line       = ml->current.line;
    out->col        = ml->current.col;
    out->length     = ml->current.length;
    out->token_type = ml->current.token_type;
}

void cb_semantic_token_list_destroy(CB_SemanticTokenList *list) { delete list; }

// ── clang-format ─────────────────────────────────────────────────────────────

/// One text replacement from clang-format.
struct FormatEditEntry {
    uint32_t    offset;      // byte offset in the source
    uint32_t    length;      // bytes to remove at offset
    std::string replacement; // text to insert instead
};

struct CB_FormatList {
    std::vector<FormatEditEntry> edits;
    FormatEditEntry current;
};

CB_FormatList *cb_format(const char *source, size_t len,
                          const char *style_dir) {
    using namespace clang::format;
    StringRef code(source, len);

    // Try to load a .clang-format file from style_dir; fall back to LLVM style.
    FormatStyle style = getLLVMStyle();
    if (style_dir && *style_dir) {
        llvm::Expected<FormatStyle> loaded =
            getStyle("file", std::string(style_dir) + "/__dummy__", "LLVM", code);
        if (loaded) style = std::move(*loaded);
    }

    // Format the entire file.
    std::vector<tooling::Range> ranges = { tooling::Range(0, (unsigned)len) };
    tooling::Replacements repls = reformat(style, code, ranges);

    auto *list = new CB_FormatList{};
    for (const tooling::Replacement &r : repls) {
        FormatEditEntry e;
        e.offset      = r.getOffset();
        e.length      = r.getLength();
        e.replacement = r.getReplacementText().str();
        list->edits.push_back(std::move(e));
    }
    return list;
}

size_t cb_format_edit_count(const CB_FormatList *list) {
    return list->edits.size();
}

void cb_format_edit_get(const CB_FormatList *list, size_t i, CB_FormatEdit *out) {
    auto *ml = const_cast<CB_FormatList *>(list);
    ml->current = ml->edits[i];
    out->offset      = ml->current.offset;
    out->length      = ml->current.length;
    out->replacement = ml->current.replacement.c_str();
}

void cb_format_list_destroy(CB_FormatList *list) { delete list; }

// ── References — all usages of a USR within a TU ─────────────────────────────

struct ReferenceEntry {
    std::string file;
    uint32_t    line, col;
    bool        is_definition;
};

struct CB_ReferenceList {
    std::vector<ReferenceEntry> refs;
    ReferenceEntry current;
};

class RefCollector : public index::IndexDataConsumer {
public:
    const std::string          &target_usr;
    std::vector<ReferenceEntry> &refs;
    const SourceManager        *SM = nullptr;

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
