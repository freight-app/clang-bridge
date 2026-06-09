#include "internal.h"

#include <clang/Frontend/FrontendActions.h>
#include <clang/Sema/CodeCompleteConsumer.h>

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
        case CXCursor_FunctionTemplate:  return 3;  // Function
        case CXCursor_CXXMethod:
        case CXCursor_Destructor:
        case CXCursor_ConversionFunction: return 2; // Method
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
