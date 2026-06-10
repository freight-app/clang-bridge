#include "internal.h"

// getName() asserts when the DeclarationName is not a plain identifier
// (constructors, destructors, operators, conversion functions).  Use this
// helper everywhere a NamedDecl's name is not known to be an identifier.
StringRef safeDeclName(const NamedDecl *D) {
    if (!D || !D->getDeclName().isIdentifier()) return {};
    return D->getName();
}


// ── Index ─────────────────────────────────────────────────────────────────────

CB_Index *cb_index_create() { return new CB_Index{}; }
void cb_index_destroy(CB_Index *idx) { delete idx; }

const char *cb_index_last_error(const CB_Index *idx) {
    return idx->last_error.empty() ? nullptr : idx->last_error.c_str();
}

// ── TransUnit ─────────────────────────────────────────────────────────────────

// Probe the installed clang resource directory once at startup.  Used to
// override ClangTool's auto-computed resource dir (which is relative to the
// freight binary and therefore wrong when freight is not co-located with clang).
std::string find_clang_resource_dir() {
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

// Custom ToolAction that builds an ASTUnit with CaptureDiagsKind::All so that
// StoredDiagnostics is populated (ClangTool::buildASTs uses None by default,
// which means diagnostics are never visible via stored_diag_begin/end and
// ASTUnit::Reparse with a buffer segfaults).
struct CapturingASTBuilder : public ToolAction {
    std::vector<std::unique_ptr<ASTUnit>> &asts;
    explicit CapturingASTBuilder(std::vector<std::unique_ptr<ASTUnit>> &v) : asts(v) {}

    bool runInvocation(std::shared_ptr<CompilerInvocation> Invocation,
                       FileManager *Files,
                       std::shared_ptr<PCHContainerOperations> PCHContainerOps,
                       DiagnosticConsumer *DiagConsumer) override {
        IntrusiveRefCntPtr<FileManager> FM(Files);
        // LoadFromCompilerInvocation needs shared_ptr<DiagnosticOptions>; copy
        // the options out of the invocation so we can own them separately.
        auto DiagOpts = std::make_shared<DiagnosticOptions>(
            Invocation->getDiagnosticOpts());
        IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
            CompilerInstance::createDiagnostics(
                Files->getVirtualFileSystem(),
                *DiagOpts,
                DiagConsumer, /*ShouldOwnClient=*/false);
        std::unique_ptr<ASTUnit> AST = ASTUnit::LoadFromCompilerInvocation(
            Invocation, std::move(PCHContainerOps),
            DiagOpts, Diags, FM,
            /*OnlyLocalDecls=*/false,
            CaptureDiagsKind::All,
            /*PrecompilePreambleAfterNParses=*/0,
            TU_Complete,
            /*CacheCodeCompletionResults=*/true,
            /*IncludeBriefCommentsInCodeCompletion=*/true,
            /*UserFilesAreVolatile=*/false);
        if (!AST) return false;
        asts.push_back(std::move(AST));
        return true;
    }
};

static std::unique_ptr<ASTUnit> build_ast_with_diagnostics(
    const char *source_file,
    const char *working_dir,
    const char * const *args,
    size_t nargs
) {
    std::vector<std::string> compile_args(args, args + nargs);
    const std::string dir = (working_dir && *working_dir) ? working_dir : ".";
    FixedCompilationDatabase db(dir, compile_args);
    std::vector<std::string> sources{source_file};
    ClangTool tool(db, sources);
    tool.setPrintErrorMessage(false);

    static const std::string s_resource_dir = find_clang_resource_dir();
    if (!s_resource_dir.empty()) {
        tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
            CommandLineArguments{"-resource-dir", s_resource_dir},
            ArgumentInsertPosition::END));
    }

    std::vector<std::unique_ptr<ASTUnit>> asts;
    CapturingASTBuilder builder(asts);
    tool.run(&builder);
    if (asts.empty()) return nullptr;
    return std::move(asts[0]);
}

CB_TransUnit *cb_parse(
    CB_Index   *idx,
    const char *source_file,
    const char *working_dir,
    const char * const *args,
    size_t nargs
) {
    auto ast = build_ast_with_diagnostics(source_file, working_dir, args, nargs);
    if (!ast) {
        if (idx) idx->last_error = "failed to build AST for: " + std::string(source_file);
        return nullptr;
    }
    auto *tu = new CB_TransUnit{};
    tu->ast = std::move(ast);
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

    const std::string dir = (working_dir && *working_dir) ? working_dir : ".";
    auto ast = build_ast_with_diagnostics(tmp_path.c_str(), dir.c_str(), args, nargs);
    std::remove(tmp_path.c_str());

    if (!ast) {
        if (idx) idx->last_error =
            "failed to build AST for: " + std::string(virtual_path);
        return nullptr;
    }
    auto *tu = new CB_TransUnit{};
    tu->ast = std::move(ast);
    return tu;
}

void cb_transunit_destroy(CB_TransUnit *tu) { delete tu; }

std::string declKind(const NamedDecl *D) {
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
    if (isa<ConceptDecl>(D))           return "concept";
    if (isa<VarDecl>(D))               return "var";
    if (isa<NamespaceDecl>(D))         return "namespace";
    return "unknown";
}

std::string prettySignature(const NamedDecl *ND, ASTContext &Ctx) {
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
bool isDefinition(const NamedDecl *ND) {
    if (auto *FD = dyn_cast<FunctionDecl>(ND))
        return FD->isThisDeclarationADefinition();
    if (auto *RD = dyn_cast<CXXRecordDecl>(ND))
        return RD->isThisDeclarationADefinition();
    if (auto *VD = dyn_cast<VarDecl>(ND))
        return VD->isThisDeclarationADefinition();
    // Fields, enum constants, typedefs, namespaces — always "defining"
    return true;
}


// ── Free helpers ──────────────────────────────────────────────────────────────

void cb_free_string(char *s) { free(s); }

// ── Reparse ───────────────────────────────────────────────────────────────────

int cb_transunit_reparse(CB_TransUnit *tu, const char *buf, size_t len) {
    std::vector<ASTUnit::RemappedFile> remapped;
    if (buf && len > 0) {
        StringRef main = tu->ast->getMainFileName();
        // ASTUnit::OwnsRemappedFileBuffers = true by default: Reparse takes
        // ownership of the MemoryBuffer* and will delete it.  Release from
        // the unique_ptr to transfer ownership and avoid a double-free.
        auto *mb = llvm::MemoryBuffer::getMemBufferCopy(
            StringRef(buf, len), main).release();
        remapped.emplace_back(main.str(), mb);
    }
    return tu->ast->Reparse(std::make_shared<PCHContainerOperations>(),
                            remapped) ? 0 : 1;
}

// ── Hover markdown ────────────────────────────────────────────────────────────
