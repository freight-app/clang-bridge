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
#include <clang/Basic/Builtins.h>
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
#include <llvm/ADT/DenseSet.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace clang;
using namespace clang::tooling;

// getName() asserts when the DeclarationName is not a plain identifier
// (constructors, destructors, operators, conversion functions).  Use this
// helper everywhere a NamedDecl's name is not known to be an identifier.
static StringRef safeDeclName(const NamedDecl *D) {
    if (!D || !D->getDeclName().isIdentifier()) return {};
    return D->getName();
}

// ── Index ─────────────────────────────────────────────────────────────────────

struct WorkspaceSymEntry {
    std::string name, detail, kind, file, usr;
    uint32_t    line = 0, col = 0;
};

struct CB_Index {
    std::string last_error; // set by cb_parse / cb_parse_unsaved on failure
    // Name→entry table populated by cb_workspace_index_add; key = lowercase name.
    std::unordered_multimap<std::string, WorkspaceSymEntry> sym_index;
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
    // Start from the AST context's policy (inherits C++ SuppressTagKeyword=true
    // and other language-appropriate defaults) rather than a blank policy.
    PrintingPolicy PP = Ctx.getPrintingPolicy();
    // Match clangd's getPrintingPolicy() from Hover.cpp.
    PP.TerseOutput = 1;                         // prototype only, no function body
    PP.PolishForDeclaration = 1;                // clean declaration output
    PP.ConstantsAsWritten = 1;                  // show literal values as written
    PP.SuppressTemplateArgsInCXXConstructors = 1; // less noise in ctor hints
    // Suppress huge initializers: if a VarDecl's init spans more than 200
    // characters in source, omit it from the hover card (clangd pattern, HV-1).
    if (auto *VD = dyn_cast<VarDecl>(ND)) {
        if (const Expr *IE = VD->getInit()) {
            const SourceManager &SM = Ctx.getSourceManager();
            SourceLocation B = IE->getBeginLoc(), E = IE->getEndLoc();
            if (B.isValid() && E.isValid()) {
                auto boff = SM.getFileOffset(SM.getSpellingLoc(B));
                auto eoff = SM.getFileOffset(SM.getSpellingLoc(E));
                if (eoff > boff + 200) PP.SuppressInitializers = 1;
            }
        }
    }
    // Use the TemplateDecl for template functions/classes so the signature
    // includes 'template<...>' parameters (clangd HV-3 pattern).
    const NamedDecl *printND = ND;
    if (const auto *TD = ND->getDescribedTemplate())
        printND = TD;
    std::string sig;
    llvm::raw_string_ostream os(sig);
    printND->print(os, PP);
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

    // Raw source buffer for the main file — used by isPrecededByParamNameComment.
    // Non-owning view; valid for the lifetime of the ASTUnit.
    StringRef MainFileBuf;

    InlayHintVisitor(ASTContext &C, std::vector<InlayHintEntry> &out,
                     uint32_t sl, uint32_t el)
        : Ctx(C), SM(C.getSourceManager()), hints(out),
          start_line(sl), end_line(el) {
        bool Invalid = false;
        StringRef Buf = SM.getBufferData(SM.getMainFileID(), &Invalid);
        MainFileBuf = Invalid ? StringRef{} : Buf;
    }

    bool shouldVisitTemplateInstantiations() const { return false; }

    // ── Clangd-aligned helpers ────────────────────────────────────────────────

    // Returns true for `expr.operator()` and lambda calls (functor pattern).
    static bool isFunctionObjectCallExpr(CallExpr *E) {
        if (auto *OC = dyn_cast<CXXOperatorCallExpr>(E))
            return OC->getOperator() == OO_Call;
        return false;
    }

    // If E is a single unqualified identifier (or implicit member access),
    // return its name — used to suppress hints when arg name == param name.
    // Mirrors clangd's getSpelledIdentifier in InlayHints.cpp.
    static StringRef getSpelledIdentifier(const Expr *E) {
        E = E->IgnoreUnlessSpelledInSource();
        if (auto *DRE = dyn_cast<DeclRefExpr>(E))
            if (!DRE->getQualifier())
                return safeDeclName(DRE->getDecl());
        if (auto *ME = dyn_cast<MemberExpr>(E))
            if (!ME->getQualifier() && ME->isImplicitAccess())
                return safeDeclName(ME->getMemberDecl());
        return {};
    }

    // Suppress hints for std::move / forward / addressof / as_const /
    // move_if_noexcept — their parameter names add no information.
    static bool isSimpleBuiltin(const FunctionDecl *FD) {
        switch (FD->getBuiltinID()) {
            case Builtin::BIaddressof:
            case Builtin::BIas_const:
            case Builtin::BIforward:
            case Builtin::BImove:
            case Builtin::BImove_if_noexcept:
                return true;
            default:
                return false;
        }
    }

    // Suppress the hint when the argument is immediately preceded by a block
    // comment whose content matches the parameter name.
    // Pattern: `/* [=. ]* <paramName> [=. ]* */` right before the expression.
    // Mirrors clangd's isPrecededByParamNameComment in InlayHints.cpp.
    bool isPrecededByParamNameComment(const Expr *E, StringRef ParamName) const {
        if (MainFileBuf.empty()) return false;
        SourceLocation FileLoc = SM.getFileLoc(E->getBeginLoc());
        auto [FID, Offset] = SM.getDecomposedLoc(FileLoc);
        if (FID != SM.getMainFileID()) return false;
        StringRef Before = MainFileBuf.substr(0, Offset).rtrim();
        if (!Before.consume_back("*/")) return false;
        llvm::StringLiteral Ignore = " =.";
        Before     = Before.rtrim(Ignore);
        ParamName  = ParamName.trim(Ignore);
        if (!Before.consume_back(ParamName)) return false;
        Before = Before.rtrim(Ignore);
        return Before.ends_with("/*");
    }

    // Suppress hints for single-arg functions named set* where the part after
    // "set" matches the (stripped) parameter name.  E.g. setFoo(foo) → no hint.
    static bool isSetter(const FunctionDecl *FD, StringRef pname) {
        if (FD->getNumParams() != 1 || pname.empty()) return false;
        StringRef fname = safeDeclName(FD);
        if (!fname.starts_with_insensitive("set")) return false;
        return fname.drop_front(3).ltrim("_").equals_insensitive(pname);
    }

    bool inViewport(SourceLocation loc) const {
        loc = SM.getSpellingLoc(loc);
        // Only emit hints for tokens that live in the main translation unit file.
        // Without this guard, hints from included headers (e.g. vec2.h) whose
        // line numbers happen to fall within [start_line, end_line] leak through.
        if (SM.getFileID(loc) != SM.getMainFileID()) return false;
        auto p = SM.getPresumedLoc(loc);
        if (!p.isValid()) return false;
        return p.getLine() >= start_line && p.getLine() <= end_line;
    }

    // ── Parameter name hints at call sites ────────────────────────────────────

    bool VisitCallExpr(CallExpr *E) {
        // Skip operator calls, but allow functor/lambda operator() calls.
        if (isa<CXXOperatorCallExpr>(E) && !isFunctionObjectCallExpr(E))
            return true;
        // User-defined literals carry no useful parameter info.
        if (isa<UserDefinedLiteral>(E)) return true;

        const FunctionDecl *FD = E->getDirectCallee();
        if (!FD) return true;
        if (isSimpleBuiltin(FD)) return true;

        // Build stripped param names up front for the setter check.
        // Strip ALL leading underscores (clangd strips them rather than skipping).
        SmallVector<StringRef, 8> pnames;
        for (const ParmVarDecl *PD : FD->parameters()) {
            StringRef n = PD->getName();
            n = n.ltrim("_");
            pnames.push_back(n);
        }
        if (!pnames.empty() && isSetter(FD, pnames[0])) return true;

        // For functor/lambda operator() calls the first argument is the implicit
        // object (the functor/lambda variable itself); skip it so param[0] aligns
        // with the first user-supplied argument, matching clangd's behaviour.
        bool skipObject = isFunctionObjectCallExpr(E);
        unsigned i = 0;
        for (const Expr *arg : E->arguments()) {
            if (skipObject) { skipObject = false; continue; }
            // Pack-expansion breaks the 1:1 arg↔param mapping; stop here.
            if (isa<PackExpansionExpr>(arg)) break;
            if (i >= pnames.size()) break;
            StringRef pname = pnames[i++];
            if (pname.empty()) continue;
            if (!inViewport(arg->getBeginLoc())) continue;

            // Suppress when argument is spelled identically to the param name.
            if (getSpelledIdentifier(arg) == pname) continue;
            // Suppress when a `/* paramName */` comment precedes the argument.
            if (isPrecededByParamNameComment(arg, pname)) continue;

            auto p = SM.getPresumedLoc(SM.getSpellingLoc(arg->getBeginLoc()));
            if (!p.isValid()) continue;
            InlayHintEntry h;
            h.line  = p.getLine();
            h.col   = p.getColumn();
            h.label = (pname + ":").str();
            h.kind  = 0;
            hints.push_back(std::move(h));
        }
        return true;
    }

    // ── Parameter name hints at constructor call sites ────────────────────────
    // VisitCallExpr only covers CallExpr nodes; CXXConstructExpr is a separate
    // AST node type.  Pattern follows clangd's VisitCXXConstructExpr / processCall.

    bool VisitCXXConstructExpr(CXXConstructExpr *E) {
        if (!E->getParenOrBraceRange().isValid()) return true;
        if (E->isStdInitListInitialization()) return true;

        const CXXConstructorDecl *CD = E->getConstructor();
        if (!CD) return true;
        if (CD->isCopyOrMoveConstructor()) return true;

        unsigned i = 0;
        for (const Expr *arg : E->arguments()) {
            if (isa<PackExpansionExpr>(arg)) break;
            if (i >= CD->getNumParams()) break;
            StringRef pname = CD->getParamDecl(i++)->getName().ltrim("_");
            if (pname.empty()) continue;
            if (!inViewport(arg->getBeginLoc())) continue;
            if (getSpelledIdentifier(arg) == pname) continue;
            if (isPrecededByParamNameComment(arg, pname)) continue;

            auto p = SM.getPresumedLoc(SM.getSpellingLoc(arg->getBeginLoc()));
            if (!p.isValid()) continue;
            InlayHintEntry h;
            h.line  = p.getLine();
            h.col   = p.getColumn();
            h.label = (pname + ":").str();
            h.kind  = 0;
            hints.push_back(std::move(h));
        }
        return true;
    }

    // ── Designator hints for aggregate initializer lists (IH-14) ────────────────
    // For undesignated brace-init-lists, emit `.field =` (record) or `[N]=`
    // (array) before each element, matching clangd's VisitInitListExpr behaviour.

    bool VisitInitListExpr(InitListExpr *ILE) {
        // Process only the syntactic (user-written) form; the semantic form has
        // Clang-synthesised padding/base-class slots that don't map to user text.
        if (!ILE->isSyntacticForm()) return true;
        unsigned nInits = ILE->getNumInits();
        if (nInits == 0) return true;

        QualType ILEType = ILE->getType().getCanonicalType();

        // ── Array aggregate: [0]=, [1]=, … ────────────────────────────────────
        if (ILEType->isArrayType()) {
            for (unsigned i = 0; i < nInits; ++i) {
                const Expr *init = ILE->getInit(i);
                if (isa<DesignatedInitExpr>(init)) continue;
                SourceLocation loc = SM.getSpellingLoc(init->getBeginLoc());
                if (!inViewport(loc)) continue;
                auto p = SM.getPresumedLoc(loc);
                if (!p.isValid()) continue;
                InlayHintEntry h;
                h.line  = p.getLine();
                h.col   = p.getColumn();
                h.label = "[" + std::to_string(i) + "]=";
                h.kind  = 3;
                hints.push_back(std::move(h));
            }
            return true;
        }

        // ── Record aggregate: .fieldName = … ──────────────────────────────────
        const RecordDecl *RD = nullptr;
        if (const auto *RT = ILEType->getAs<RecordType>())
            RD = RT->getDecl();
        if (!RD) return true;
        // Only aggregate class/struct types get designator hints.
        if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD))
            if (!CRD->isAggregate()) return true;

        auto field_it = RD->field_begin();
        for (unsigned i = 0; i < nInits; ++i) {
            // Advance past anonymous/unnamed bitfields — they're not user-visible.
            while (field_it != RD->field_end() &&
                   (field_it->isUnnamedBitField() || field_it->getName().empty()))
                ++field_it;
            if (field_it == RD->field_end()) break;

            const Expr *init = ILE->getInit(i);
            if (isa<DesignatedInitExpr>(init)) { ++field_it; continue; }

            StringRef fieldName = field_it->getName();
            // Suppress when the init value is spelled the same as the field name,
            // matching clangd's getSpelledIdentifier-based suppression.
            if (!fieldName.empty() && getSpelledIdentifier(init) == fieldName) {
                ++field_it; continue;
            }

            SourceLocation loc = SM.getSpellingLoc(init->getBeginLoc());
            if (!inViewport(loc)) { ++field_it; continue; }
            auto p = SM.getPresumedLoc(loc);
            if (!p.isValid()) { ++field_it; continue; }

            InlayHintEntry h;
            h.line  = p.getLine();
            h.col   = p.getColumn();
            h.label = "." + fieldName.str() + " =";
            h.kind  = 3;
            hints.push_back(std::move(h));
            ++field_it;
        }
        return true;
    }

    // ── Deduced return-type hints ─────────────────────────────────────────────
    // Emit `-> T` after the closing `)` of a function whose return type is
    // deduced (written as `auto`) and has no explicit trailing return.
    // Pattern follows clangd VisitFunctionDecl / addReturnTypeHint.

    void addReturnTypeHint(FunctionDecl *D, SourceLocation anchorLoc) {
        const AutoType *AT = D->getReturnType()->getContainedAutoType();
        if (!AT || AT->getDeducedType().isNull()) return;
        if (!inViewport(anchorLoc)) return;
        QualType retType = D->getReturnType();
        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressScope = 1;
        std::string typeStr = retType.getAsString(PP);
        if (typeStr == "auto" || typeStr.empty()) return;
        auto p = SM.getPresumedLoc(anchorLoc);
        if (!p.isValid()) return;
        InlayHintEntry h;
        h.line  = p.getLine();
        h.col   = p.getColumn() + 1; // one past the `)` or `]`
        h.label = "-> " + typeStr;
        h.kind  = 1;
        hints.push_back(std::move(h));
    }

    bool VisitFunctionDecl(FunctionDecl *D) {
        if (D->isImplicit()) return true;
        // Return-type hint (non-trailing deduced only).
        auto *FPT = dyn_cast<FunctionProtoType>(D->getType().getTypePtr());
        if (!(FPT && FPT->hasTrailingReturn())) {
            if (auto FTL = D->getFunctionTypeLoc())
                addReturnTypeHint(D, FTL.getRParenLoc());
        }
        // Block-end hint for function definitions.
        if (D->isThisDeclarationADefinition()) {
            if (const Stmt *body = D->getBody())
                addBlockEndHint(body->getSourceRange(), "", blockNameForFn(D), "");
        }
        return true;
    }

    bool VisitLambdaExpr(LambdaExpr *E) {
        FunctionDecl *D = E->getCallOperator();
        if (!D || E->hasExplicitResultType()) return true;
        SourceLocation anchor;
        if (!E->hasExplicitParameters())
            anchor = E->getIntroducerRange().getEnd(); // after `]`
        else if (auto FTL = D->getFunctionTypeLoc())
            anchor = FTL.getRParenLoc();               // after `)`
        if (anchor.isValid())
            addReturnTypeHint(D, anchor);
        return true;
    }

    // ── Block-end hints ──────────────────────────────────────────────────────────
    // Emit ` // label` after closing `}` of blocks spanning ≥10 lines.
    // Pattern follows clangd VisitForStmt / VisitTagDecl / VisitNamespaceDecl.

    static constexpr unsigned kBlockEndMinLines = 10;
    llvm::DenseSet<const IfStmt *> elseIfSet;

    static std::string blockNameForFn(const FunctionDecl *D) {
        DeclarationName DN = D->getDeclName();
        if (DN.isIdentifier())
            return DN.getAsIdentifierInfo()->getName().str();
        if (auto *CD = dyn_cast<CXXConstructorDecl>(D))
            return CD->getParent()->getName().str();
        if (auto *DD = dyn_cast<CXXDestructorDecl>(D))
            return ("~" + DD->getParent()->getName()).str();
        std::string s;
        llvm::raw_string_ostream os(s);
        DN.print(os, PrintingPolicy(D->getASTContext().getLangOpts()));
        return s;
    }

    static std::string summarizeCondition(const Expr *E) {
        if (!E) return "";
        E = E->IgnoreImplicit();
        if (auto *DRE = dyn_cast<DeclRefExpr>(E))
            return safeDeclName(DRE->getFoundDecl()).str();
        if (auto *ME = dyn_cast<MemberExpr>(E))
            return safeDeclName(ME->getMemberDecl()).str();
        if (auto *CE = dyn_cast<CallExpr>(E)) {
            std::string s = summarizeCondition(CE->getCallee());
            if (!s.empty()) return s + (CE->getNumArgs() == 0 ? "()" : "(...)");
        }
        return "";
    }

    void addBlockEndHint(SourceRange braceRange, StringRef declPrefix,
                         StringRef name, StringRef optPunct) {
        if (MainFileBuf.empty()) return;
        SourceLocation beginLoc = SM.getFileLoc(braceRange.getBegin());
        SourceLocation endLoc   = SM.getFileLoc(braceRange.getEnd());
        auto [bFID, bOff] = SM.getDecomposedLoc(beginLoc);
        auto [eFID, eOff] = SM.getDecomposedLoc(endLoc);
        if (bFID != SM.getMainFileID() || eFID != SM.getMainFileID()) return;

        StringRef restOfLine = MainFileBuf.substr(eOff).split('\n').first;
        if (!restOfLine.starts_with("}")) return;
        StringRef trailing = restOfLine.drop_front().trim();
        if (!trailing.empty() && trailing != optPunct) return;

        unsigned beginLine = SM.getLineNumber(bFID, bOff);
        unsigned endLine   = SM.getLineNumber(eFID, eOff);
        if (endLine < beginLine + kBlockEndMinLines - 1) return;

        std::string label = declPrefix.str();
        if (!label.empty() && !name.empty()) label += ' ';
        label += name.str();
        if (label.empty() || label.size() > 60) return;
        label = " // " + label;

        auto p = SM.getPresumedLoc(endLoc);
        if (!p.isValid()) return;
        if ((unsigned)p.getLine() < start_line || (unsigned)p.getLine() > end_line) return;

        uint32_t closingLen = 1 + (uint32_t)(trailing.empty() ? 0 : optPunct.size());
        InlayHintEntry h;
        h.line  = (uint32_t)p.getLine();
        h.col   = (uint32_t)p.getColumn() + closingLen;
        h.label = std::move(label);
        h.kind  = 2;
        hints.push_back(std::move(h));
    }

    void markBlockEnd(const Stmt *body, StringRef lbl, StringRef name = "") {
        if (auto *CS = dyn_cast_or_null<CompoundStmt>(body))
            addBlockEndHint(CS->getSourceRange(), lbl, name, "");
    }

    bool VisitForStmt(ForStmt *S) {
        std::string name;
        if (auto *DS = dyn_cast_or_null<DeclStmt>(S->getInit());
                DS && DS->isSingleDecl())
            name = safeDeclName(cast<NamedDecl>(DS->getSingleDecl())).str();
        else
            name = summarizeCondition(S->getCond());
        markBlockEnd(S->getBody(), "for", name);
        return true;
    }

    bool VisitCXXForRangeStmt(CXXForRangeStmt *S) {
        markBlockEnd(S->getBody(), "for",
                     safeDeclName(S->getLoopVariable()).str());
        return true;
    }

    bool VisitWhileStmt(WhileStmt *S) {
        markBlockEnd(S->getBody(), "while", summarizeCondition(S->getCond()));
        return true;
    }

    bool VisitSwitchStmt(SwitchStmt *S) {
        markBlockEnd(S->getBody(), "switch", summarizeCondition(S->getCond()));
        return true;
    }

    bool VisitIfStmt(IfStmt *S) {
        if (auto *elseIf = dyn_cast_or_null<IfStmt>(S->getElse()))
            elseIfSet.insert(elseIf);
        auto *endCS = dyn_cast<CompoundStmt>(
            S->getElse() ? S->getElse() : S->getThen());
        if (endCS) {
            std::string name = elseIfSet.count(S) ? "" : summarizeCondition(S->getCond());
            addBlockEndHint({S->getThen()->getBeginLoc(), endCS->getRBracLoc()},
                            "if", name, "");
        }
        return true;
    }

    bool VisitTagDecl(TagDecl *D) {
        if (!D->isThisDeclarationADefinition()) return true;
        std::string prefix = D->getKindName().str();
        if (auto *ED = dyn_cast<EnumDecl>(D)) {
            if (ED->isScoped())
                prefix += ED->isScopedUsingClassTag() ? " class" : " struct";
        }
        addBlockEndHint(D->getBraceRange(), prefix,
                        safeDeclName(D).str(), ";");
        return true;
    }

    bool VisitNamespaceDecl(NamespaceDecl *D) {
        addBlockEndHint(D->getSourceRange(), "namespace",
                        safeDeclName(D).str(), "");
        return true;
    }

    // ── Deduced-type hints for `auto` variable declarations ───────────────────

    bool VisitVarDecl(VarDecl *D) {
        if (SM.isInSystemHeader(D->getLocation())) return true;

        // Structured bindings: `auto [x, y] = foo()`.
        // Emit a per-binding hint using canonical type (avoids
        // `tuple_element<I,A>::type` noise).  Clangd pattern.
        if (auto *DD = dyn_cast<DecompositionDecl>(D)) {
            PrintingPolicy PP(Ctx.getLangOpts());
            PP.SuppressScope = 1;
            PP.AnonymousTagLocations = 0; // print "(anonymous)" not "(lambda at file:line)"
            for (BindingDecl *BD : DD->bindings()) {
                QualType BT = BD->getType();
                if (BT.isNull() || BT->isDependentType()) continue;
                if (!inViewport(BD->getLocation())) continue;
                auto bploc = SM.getPresumedLoc(SM.getSpellingLoc(BD->getLocation()));
                if (!bploc.isValid()) continue;
                StringRef bname = BD->getName();
                if (bname.empty()) continue;
                InlayHintEntry h;
                h.line  = bploc.getLine();
                // Place after the binding name, not before (HintSide::Right).
                h.col   = bploc.getColumn() + (uint32_t)bname.size();
                h.label = ": " + BT.getCanonicalType().getAsString(PP);
                h.kind  = 1;
                hints.push_back(std::move(h));
            }
            return true;
        }

        if (!inViewport(D->getLocation())) return true;
        if (isa<ParmVarDecl>(D)) return true;

        // TypeSourceInfo holds the written `auto`; D->getType() holds the deduced type.
        if (!D->getTypeSourceInfo()->getType()->getContainedAutoType()) return true;
        QualType deduced = D->getType();
        if (deduced->isUndeducedAutoType()) return true;

        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressScope = 1;
        PP.AnonymousTagLocations = 0;
        std::string typeStr = deduced.getAsString(PP);
        if (typeStr == "auto" || typeStr.empty()) return true;
        // Resolve type-trait aliases (e.g. remove_cv_t<double> → double).
        if (typeStr.find("_t<") != std::string::npos) {
            std::string canon = deduced.getCanonicalType().getAsString(PP);
            if (!canon.empty() && canon != "auto")
                typeStr = canon;
        }
        // Suppress implementation-detail names (starting with __).
        if (typeStr.size() >= 2 && typeStr[0] == '_' && typeStr[1] == '_')
            return true;

        auto p = SM.getPresumedLoc(D->getLocation());
        if (!p.isValid()) return true;
        StringRef vname = D->getName();
        InlayHintEntry h;
        h.line  = p.getLine();
        // Place after the variable name (HintSide::Right), not before.
        h.col   = p.getColumn() + (uint32_t)vname.size();
        h.label = ": " + typeStr;
        h.kind  = 1;
        hints.push_back(std::move(h));
        return true;
    }

    // ── decltype() type hints (IH-16) ─────────────────────────────────────────
    // Emit `: T` after `decltype(expr)` type specifiers, showing the resolved type.

    bool VisitTypeLoc(TypeLoc TL) {
        const auto *DT = dyn_cast<DecltypeType>(TL.getType());
        if (!DT) return true;
        QualType UT = DT->getUnderlyingType();
        if (UT.isNull() || UT->isDependentType()) return true;
        SourceLocation endLoc = TL.getSourceRange().getEnd();
        if (!inViewport(endLoc)) return true;
        auto p = SM.getPresumedLoc(SM.getSpellingLoc(endLoc));
        if (!p.isValid()) return true;
        unsigned tokLen = Lexer::MeasureTokenLength(
            SM.getSpellingLoc(endLoc), SM, Ctx.getLangOpts());
        PrintingPolicy PP(Ctx.getLangOpts());
        PP.SuppressScope = 1;
        PP.AnonymousTagLocations = 0;
        std::string typeStr = UT.getAsString(PP);
        if (typeStr.empty()) return true;
        InlayHintEntry h;
        h.line  = p.getLine();
        h.col   = p.getColumn() + tokLen;
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

    // Sort by position then deduplicate — explicit template instantiations can
    // produce identical hints at the same location.
    auto &e = list->entries;
    std::sort(e.begin(), e.end(), [](const InlayHintEntry &a, const InlayHintEntry &b) {
        if (a.line != b.line) return a.line < b.line;
        if (a.col  != b.col)  return a.col  < b.col;
        return a.label < b.label;
    });
    e.erase(std::unique(e.begin(), e.end(), [](const InlayHintEntry &a,
                                               const InlayHintEntry &b) {
        return a.line == b.line && a.col == b.col && a.label == b.label;
    }), e.end());

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
            // Follow using-shadow decls to the real target (HV-2).
            if (auto *USD = dyn_cast<UsingShadowDecl>(D))
                D = USD->getTargetDecl();
            if (inToken(E->getLocation(), safeDeclName(D).size()))
                found = D;
        }
        return true;
    }

    bool VisitMemberExpr(MemberExpr *E) {
        if (!found) {
            const NamedDecl *D = E->getMemberDecl();
            if (inToken(E->getMemberLoc(), safeDeclName(D).size()))
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

    // SL-2: constructor call sites — `MyClass(args)` / `MyClass{args}`.
    // CXXTemporaryObjectExpr (a subclass) also hits this visitor.
    bool VisitCXXConstructExpr(CXXConstructExpr *E) {
        if (!found) {
            CXXConstructorDecl *ctor = E->getConstructor();
            if (!ctor) return true;
            StringRef className = ctor->getParent()->getName();
            if (!className.empty() && E->getLocation().isValid() &&
                inToken(E->getLocation(), className.size()))
                found = ctor;
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

    // Do not descend into template instantiations: their implicit members
    // are attributed to the instantiation site in user code, which makes
    // them look like user declarations on the same line and pollute results.
    bool shouldVisitTemplateInstantiations() const { return false; }

    bool VisitNamedDecl(NamedDecl *ND) {
        if (ND->isImplicit()) return true;
        if (SM.isInSystemHeader(ND->getLocation())) return true;

        // For plain identifiers use safeDeclName.  For constructors / destructors
        // the declaration name is the parent class name (SL-1).
        StringRef name = safeDeclName(ND);
        if (name.empty()) {
            if (auto *CD = dyn_cast<CXXConstructorDecl>(ND))
                name = CD->getParent()->getName();
            else if (auto *DD = dyn_cast<CXXDestructorDecl>(ND))
                name = DD->getParent()->getName();
            // Operators, conversions, etc. — not reachable by plain cursor hover.
        }
        if (name.empty()) return true;

        auto ploc = SM.getPresumedLoc(ND->getLocation());
        if (!ploc.isValid()) return true;
        uint32_t startCol = (uint32_t)ploc.getColumn();
        if ((uint32_t)ploc.getLine() == target_line &&
            target_col >= startCol &&
            target_col < startCol + (uint32_t)name.size())
            found = ND;
        return true;
    }
};

// Shared symbol resolution: finds the NamedDecl at (1-based) line/col.
// Checks expression reference nodes first, then declaration sites.
static const NamedDecl *locate_symbol_at(ASTUnit *ast, uint32_t line, uint32_t col) {
    ASTContext &Ctx = ast->getASTContext();
    const SourceManager &SM = Ctx.getSourceManager();

    // Clangd pattern: only proceed when the cursor is on an identifier token.
    // Hovering on punctuation (::, (, *, etc.) should return nothing rather
    // than a garbage result from the DeclLocator fallback.
    {
        SourceLocation loc = SM.translateLineCol(SM.getMainFileID(), line, col);
        if (loc.isValid()) {
            Token tok;
            if (!Lexer::getRawToken(loc, tok, SM, Ctx.getLangOpts(),
                                    /*IgnoreWhiteSpace=*/true)) {
                if (tok.getKind() != tok::identifier &&
                    tok.getKind() != tok::raw_identifier &&
                    tok.getKind() != tok::kw_auto &&
                    tok.getKind() != tok::kw_operator)
                    return nullptr;
            }
        }
    }

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
        StringRef name = safeDeclName(D);
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
        unsigned tok_len = Lexer::MeasureTokenLength(Loc, *SM, LO);
        using SR = index::SymbolRole;
        bool is_def = (Roles & (index::SymbolRoleSet)SR::Definition) != 0;
        out.push_back({p.getLine(), p.getColumn(),
                       p.getColumn() + tok_len, is_def ? (uint8_t)3 : (uint8_t)2});
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

// ── Folding ranges ────────────────────────────────────────────────────────────

struct FoldingEntry { uint32_t start_line, end_line; std::string kind; };

struct CB_FoldingRangeList {
    std::vector<FoldingEntry> entries;
    FoldingEntry              current;
};

class FoldingVisitor : public RecursiveASTVisitor<FoldingVisitor> {
    SourceManager            &SM;
    FileID                    main_fid;
    std::vector<FoldingEntry> &out;

    void addRange(SourceRange R, const char *kind) {
        if (R.isInvalid()) return;
        SourceLocation b = SM.getSpellingLoc(R.getBegin());
        SourceLocation e = SM.getSpellingLoc(R.getEnd());
        if (SM.getFileID(b) != main_fid) return;
        auto pb = SM.getPresumedLoc(b);
        auto pe = SM.getPresumedLoc(e);
        if (!pb.isValid() || !pe.isValid()) return;
        if (pe.getLine() <= pb.getLine() + 1) return;
        out.push_back({pb.getLine(), pe.getLine(), kind});
    }
public:
    FoldingVisitor(SourceManager &sm, FileID fid, std::vector<FoldingEntry> &o)
        : SM(sm), main_fid(fid), out(o) {}
    bool shouldVisitTemplateInstantiations() const { return false; }
    bool VisitFunctionDecl(FunctionDecl *D) {
        if (D->hasBody() && D->getBody())
            addRange(D->getBody()->getSourceRange(), "region");
        return true;
    }
    bool VisitCXXRecordDecl(CXXRecordDecl *D) {
        if (D->isCompleteDefinition()) addRange(D->getBraceRange(), "region");
        return true;
    }
    bool VisitEnumDecl(EnumDecl *D) {
        if (D->isCompleteDefinition()) addRange(D->getBraceRange(), "region");
        return true;
    }
    bool VisitNamespaceDecl(NamespaceDecl *D) {
        addRange(SourceRange(D->getBeginLoc(), D->getRBraceLoc()), "region");
        return true;
    }
};

CB_FoldingRangeList *cb_folding_ranges(CB_TransUnit *tu) {
    auto *list = new CB_FoldingRangeList{};
    ASTContext    &Ctx     = tu->ast->getASTContext();
    SourceManager &SM      = Ctx.getSourceManager();
    FileID         main_fid = SM.getMainFileID();

    FoldingVisitor vis(SM, main_fid, list->entries);
    vis.TraverseDecl(Ctx.getTranslationUnitDecl());

    std::sort(list->entries.begin(), list->entries.end(),
              [](const FoldingEntry &a, const FoldingEntry &b) {
                  return a.start_line < b.start_line; });
    return list;
}

size_t cb_folding_range_count(const CB_FoldingRangeList *list) {
    return list->entries.size();
}
void cb_folding_range_get(const CB_FoldingRangeList *list, size_t i,
                          CB_FoldingRange *out) {
    auto *ml = const_cast<CB_FoldingRangeList *>(list);
    ml->current = ml->entries[i];
    out->start_line = ml->current.start_line;
    out->end_line   = ml->current.end_line;
    out->kind       = ml->current.kind.c_str();
}
void cb_folding_range_list_destroy(CB_FoldingRangeList *list) { delete list; }

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
            e.col         = sl.getColumn();
            e.end_line    = el.isValid() ? el.getLine()   : sl.getLine();
            e.end_col     = el.isValid() ? el.getColumn() : sl.getColumn();
            e.replacement = fi.CodeToInsert;
            list->actions.push_back(std::move(e));
        }
    }
    return list;
}
size_t cb_code_action_count(const CB_CodeActionList *list) {
    return list->actions.size();
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
        e.col    = ploc.getColumn();
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
    WorkspaceIndexer wi(idx, tu->ast->getASTContext());
    wi.TraverseDecl(tu->ast->getASTContext().getTranslationUnitDecl());
}

struct CB_WorkspaceSymList {
    std::vector<WorkspaceSymEntry> results;
    WorkspaceSymEntry              current;
};

CB_WorkspaceSymList *cb_workspace_symbols(CB_Index *idx, const char *query) {
    auto *list = new CB_WorkspaceSymList{};
    if (!idx || !query || !*query) return list;
    std::string q(query);
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    std::unordered_set<std::string> seen;
    for (const auto &[key, entry] : idx->sym_index) {
        if (key.find(q) == std::string::npos) continue;
        if (!seen.insert(entry.usr).second) continue;
        list->results.push_back(entry);
        if (list->results.size() >= 200) break;
    }
    std::sort(list->results.begin(), list->results.end(),
              [&q](const WorkspaceSymEntry &a, const WorkspaceSymEntry &b) {
                  std::string la = a.name, lb = b.name;
                  std::transform(la.begin(), la.end(), la.begin(), ::tolower);
                  std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
                  bool pa = la.find(q) == 0, pb = lb.find(q) == 0;
                  if (pa != pb) return pa > pb;
                  return la < lb;
              });
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
        item->col  = ploc.getColumn();
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
            e.col       = defLoc.isValid() ? defLoc.getColumn() : 0;
            e.usr       = funcUSR(callee);
            e.call_line = callLoc.getLine();
            e.call_col  = callLoc.getColumn();
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
            e.col       = callerLoc.isValid() ? callerLoc.getColumn() : 0;
            e.usr       = funcUSR(caller);
            e.call_line = callLoc.getLine();
            e.call_col  = callLoc.getColumn();
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
        item->col  = ploc.getColumn();
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
            e.line = p.getLine(); e.col = p.getColumn();
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
                    e.line = p.getLine(); e.col = p.getColumn();
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

// ── Macro expansion ───────────────────────────────────────────────────────────

char *cb_expand_macro(CB_TransUnit *tu, uint32_t line, uint32_t col) {
    ASTContext          &Ctx = tu->ast->getASTContext();
    const SourceManager &SM  = Ctx.getSourceManager();
    const LangOptions   &LO  = Ctx.getLangOpts();
    Preprocessor        &PP  = tu->ast->getPreprocessor();

    SourceLocation target = SM.translateLineCol(SM.getMainFileID(), line, col);
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
}

// ── AST dump ──────────────────────────────────────────────────────────────────

char *cb_ast_dump(CB_TransUnit *tu, uint32_t start_line, uint32_t end_line) {
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
            e.col      = pb.isValid() ? pb.getColumn() : 0;
            e.end_line = pe.isValid() ? pe.getLine()   : 0;
            e.end_col  = pe.isValid() ? pe.getColumn() : 0;
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
}
