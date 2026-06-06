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
#include <clang/Index/IndexDataConsumer.h>
#include <clang/Index/IndexingAction.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;

// ── Index ─────────────────────────────────────────────────────────────────────

struct CB_Index {};

CB_Index *cb_index_create() { return new CB_Index{}; }
void cb_index_destroy(CB_Index *idx) { delete idx; }

// ── TransUnit ─────────────────────────────────────────────────────────────────

struct CB_TransUnit {
    std::unique_ptr<ASTUnit> ast;
};

CB_TransUnit *cb_parse(
    CB_Index   * /*idx*/,
    const char *source_file,
    const char * const *args,
    size_t nargs
) {
    std::vector<std::string> compile_args(args, args + nargs);

    // FixedCompilationDatabase wraps a single file with given compile flags.
    FixedCompilationDatabase db(".", compile_args);

    std::vector<std::string> sources{source_file};
    ClangTool tool(db, sources);
    tool.setPrintErrorMessage(false);

    std::vector<std::unique_ptr<ASTUnit>> asts;
    tool.buildASTs(asts);
    if (asts.empty()) return nullptr;

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
    if (isa<CXXMethodDecl>(D))    return "method";
    if (isa<FunctionDecl>(D))     return "function";
    if (isa<FieldDecl>(D))        return "field";
    if (isa<CXXRecordDecl>(D))    return "record";
    if (isa<EnumDecl>(D))         return "enum";
    if (isa<EnumConstantDecl>(D)) return "enumconst";
    if (isa<TypedefNameDecl>(D))  return "typedef";
    if (isa<VarDecl>(D))          return "var";
    if (isa<NamespaceDecl>(D))    return "namespace";
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

struct DiagEntry {
    std::string file;
    uint32_t    line;
    uint32_t    col;
    uint8_t     severity;
    std::string message;
    std::string check_name;
};

struct CB_DiagIter {
    std::vector<DiagEntry> entries;
    size_t pos = 0;
    DiagEntry current;
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
            e.line = (uint32_t)ploc.getLine();
            e.col  = (uint32_t)ploc.getColumn();
        }
        e.severity = (uint8_t)sd.getLevel();
        e.message  = sd.getMessage().str();
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
    out->severity   = it->current.severity;
    out->message    = it->current.message.c_str();
    out->check_name = it->current.check_name.empty()
                    ? nullptr : it->current.check_name.c_str();
    return 1;
}

void cb_diag_iter_destroy(CB_DiagIter *it) { delete it; }

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
