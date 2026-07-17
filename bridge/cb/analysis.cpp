#include "internal.h"

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
    std::unordered_set<std::string> seen;
    auto collect = [&](const SrcMgr::SLocEntry &entry) {
        if (!entry.isFile()) return;
        const SrcMgr::FileInfo &FI = entry.getFile();
        if (FI.getIncludeLoc().isInvalid()) return; // main file

        // Preamble files live in loaded SLoc entries. Map their include
        // location back into the current main-file buffer before filtering or
        // measuring the directive range.
        SourceLocation incLoc = ast->mapLocationFromPreamble(FI.getIncludeLoc());
        // Only report directives written in the main file: cb_inclusions backs
        // LSP textDocument/documentLink for the open document, so transitive
        // includes pulled in by system headers must not be returned.
        if (SM.getFileID(incLoc) != SM.getMainFileID()) return;
        auto presumed = SM.getPresumedLoc(incLoc);
        if (!presumed.isValid()) return;

        // Resolve the included filename from the ContentCache.
        const SrcMgr::ContentCache *CC = &FI.getContentCache();
        if (!CC->OrigEntry) return;
        if (CC->OrigEntry == SM.getFileEntryForID(SM.getMainFileID())) return;
        std::string included = CC->OrigEntry->getName().str();
        if (included.empty()) return;

        std::string key = included + ":" + std::to_string(presumed.getLine());
        if (!seen.insert(key).second) return;

        // Find the column range of the path literal ("foo.h" or <foo.h>).
        // Scan the raw source line for the opening delimiter after #include.
        InclusionEntry e;
        e.including_file = presumed.getFilename() ? presumed.getFilename() : "";
        e.included_file  = included;
        e.line      = presumed.getLine();
        e.start_col = source_location_utf16_col(SM, incLoc);
        e.end_col   = e.start_col;
        {
            bool invalid = false;
            const char *buf = SM.getCharacterData(incLoc, &invalid);
            if (!invalid && buf) {
                // Find opening " or < on this line (within 32 chars of '#')
                for (int off = 0; off < 32; ++off) {
                    if (buf[off] == '\n' || buf[off] == '\0') break;
                    if (buf[off] == '"' || buf[off] == '<') {
                        char close = (buf[off] == '"') ? '"' : '>';
                        SourceLocation startLoc = incLoc.getLocWithOffset(off);
                        uint32_t start = source_location_utf16_col(SM, startLoc);
                        // Find the closing delimiter.
                        uint32_t end = start + 1;
                        for (int k = off + 1; k < 256; ++k) {
                            if (buf[k] == '\n' || buf[k] == '\0') break;
                            if (buf[k] == close) {
                                SourceLocation endLoc = incLoc.getLocWithOffset(k + 1);
                                end = source_location_utf16_col(SM, endLoc);
                                break;
                            }
                        }
                        e.start_col = start;
                        e.end_col   = end;
                        break;
                    }
                }
            }
        }
        out.push_back(std::move(e));
    };

    // Post-preamble files and parses without a PCH use positive/local IDs.
    unsigned local_count = SM.local_sloc_entry_size();
    for (unsigned i = 1; i < local_count; ++i)
        collect(SM.getLocalSLocEntry(i));

    // Once ASTUnit builds a precompiled preamble, its headers use
    // negative/loaded IDs and disappear from the local table.
    unsigned loaded_count = SM.loaded_sloc_entry_size();
    for (unsigned i = 0; i < loaded_count; ++i) {
        bool invalid = false;
        const SrcMgr::SLocEntry &entry = SM.getLoadedSLocEntry(i, &invalid);
        if (!invalid) collect(entry);
    }
}

CB_InclusionList *cb_inclusions(CB_TransUnit *tu) {
    return cb_recover(tu, __func__, static_cast<CB_InclusionList *>(nullptr),
                      [&]() -> CB_InclusionList * {
    auto *list = new CB_InclusionList{};
    collect_inclusions(tu->ast.get(), list->entries);
    return list;
    });
}

size_t cb_inclusion_count(const CB_InclusionList *list) {
    return list ? list->entries.size() : 0;
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
    if (isa<NamespaceDecl>(D))           return CB_TOK_NAMESPACE;
    if (isa<TypedefNameDecl>(D))         return CB_TOK_TYPE;
    // Template decls: classify like the entity they introduce so they don't
    // fall through to VARIABLE (and so they agree with the templated inner decl
    // that shares their source location — see the dedup in cb_semantic_tokens).
    if (isa<TypeAliasTemplateDecl>(D))   return CB_TOK_TYPE;
    if (isa<TemplateTypeParmDecl>(D))    return CB_TOK_TYPE;
    if (isa<ClassTemplateDecl>(D))       return CB_TOK_TYPE;
    if (isa<CXXRecordDecl>(D) ||
        isa<RecordDecl>(D) ||
        isa<EnumDecl>(D))                return CB_TOK_TYPE;
    if (isa<EnumConstantDecl>(D))        return CB_TOK_ENUM_MEMBER;
    if (isa<FieldDecl>(D))               return CB_TOK_PROPERTY;
    if (isa<NonTypeTemplateParmDecl>(D)) return CB_TOK_PARAMETER;
    if (isa<ParmVarDecl>(D))             return CB_TOK_PARAMETER;
    if (auto *FTD = dyn_cast<FunctionTemplateDecl>(D))
        return isa<CXXMethodDecl>(FTD->getTemplatedDecl())
                   ? CB_TOK_METHOD : CB_TOK_FUNCTION;
    if (isa<VarTemplateDecl>(D))         return CB_TOK_VARIABLE;
    if (isa<VarDecl>(D))                 return CB_TOK_VARIABLE;
    if (isa<CXXMethodDecl>(D))           return CB_TOK_METHOD;
    if (isa<FunctionDecl>(D))            return CB_TOK_FUNCTION;
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
        // semanticTokens is per-document: only tokens in the main file belong in
        // the stream.  Without this, decls/refs from an included header (e.g.
        // shapes.h's `square`) leak in at that header's line numbers, which the
        // client then misapplies to the open document.  clangd is main-file only.
        if (SM.getFileID(loc) != SM.getMainFileID()) return;
        auto p = SM.getPresumedLoc(loc);
        if (!p.isValid()) return;

        StringRef name = safeDeclName(D);
        if (name.empty()) return;

        SemanticTokenEntry t;
        t.line       = p.getLine();
        t.col        = source_location_utf16_col(SM, loc);
        t.length     = utf16_length(name);
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

    // Type references: base-class specifiers, variable/parameter/return type
    // annotations, and template-parameter uses.  Without this, written type
    // names (e.g. `Shape` in `: Shape`, `Circle` in `geo::Circle c`, `T` in the
    // clamp signature) get no token at all.  Builtin types (int/double) have no
    // decl and are skipped, matching clangd.
    bool VisitTypeLoc(TypeLoc TL) {
        const NamedDecl *D = nullptr;
        SourceLocation   loc;
        if (auto T = TL.getAs<RecordTypeLoc>())                  { D = T.getDecl();            loc = T.getNameLoc(); }
        else if (auto T = TL.getAs<EnumTypeLoc>())               { D = T.getDecl();            loc = T.getNameLoc(); }
        else if (auto T = TL.getAs<TypedefTypeLoc>())            { D = T.getTypePtr()->getDecl(); loc = T.getNameLoc(); }
        else if (auto T = TL.getAs<TemplateTypeParmTypeLoc>())   { D = T.getDecl();            loc = T.getNameLoc(); }
        else if (auto T = TL.getAs<InjectedClassNameTypeLoc>())  { D = T.getDecl();            loc = T.getNameLoc(); }
        if (D && loc.isValid()) emitToken(loc, D);
        return true;
    }

    // Member references in a constructor's initializer list (`: radius(r)`),
    // which clangd highlights as properties.  Also emit the constructor's own
    // name (which is the class name, so VisitNamedDecl skips it) as a class token.
    bool VisitCXXConstructorDecl(CXXConstructorDecl *D) {
        if (!D->isImplicit() && D->getParent())
            emitToken(D->getLocation(), D->getParent());
        for (const CXXCtorInitializer *I : D->inits()) {
            if (I->isMemberInitializer() && I->isWritten())
                if (FieldDecl *F = I->getMember())
                    emitToken(I->getMemberLocation(), F);
        }
        return true;
    }
};

CB_SemanticTokenList *cb_semantic_tokens(CB_TransUnit *tu) {
    return cb_recover(tu, __func__, static_cast<CB_SemanticTokenList *>(nullptr),
                      [&]() -> CB_SemanticTokenList * {
    auto *list = new CB_SemanticTokenList{};
    ASTContext &Ctx = tu->ast->getASTContext();

    SemanticTokenVisitor V(Ctx, list->tokens);
    V.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Annotate macro use sites as CB_TOK_MACRO.  Every macro invocation creates
    // one or more expansion SLocEntries; we lex the identifier at the *invocation*
    // location (getExpansionLoc — not the macro body's spelling location) to get
    // the macro name, and dedup by invocation offset so nested expansions of one
    // invocation (e.g. a function-like macro body) yield a single token.
    const SourceManager &SM = Ctx.getSourceManager();
    std::unordered_set<unsigned> seen_macro_locs;
    unsigned n = SM.local_sloc_entry_size();
    for (unsigned i = 1; i < n; ++i) {
        const SrcMgr::SLocEntry &sle = SM.getLocalSLocEntry(i);
        if (sle.isFile()) continue; // only expansion records
        SourceLocation expansionLoc =
            SM.getExpansionLoc(sle.getExpansion().getExpansionLocStart());
        if (expansionLoc.isInvalid()) continue;
        if (SM.isInSystemHeader(expansionLoc)) continue;
        if (!seen_macro_locs.insert(expansionLoc.getRawEncoding()).second)
            continue;
        auto p = SM.getPresumedLoc(expansionLoc);
        if (!p.isValid()) continue;

        // Lex the macro-name identifier at the invocation site.
        Token tok;
        if (Lexer::getRawToken(expansionLoc, tok, SM, Ctx.getLangOpts(),
                               /*IgnoreWhiteSpace=*/true))
            continue;
        if (tok.isNot(tok::raw_identifier)) continue;
        std::string spelling = Lexer::getSpelling(tok, SM, Ctx.getLangOpts());
        if (spelling.empty()) continue;

        SemanticTokenEntry t;
        t.line       = p.getLine();
        t.col        = source_location_utf16_col(SM, expansionLoc);
        t.length     = utf16_length(spelling);
        t.token_type = CB_TOK_MACRO;
        list->tokens.push_back(t);
    }

    // Macro *definition* names (`#define MAX_ITEMS`).  The loop above only marks
    // use sites; clangd also tokenizes the name at each #define in this file.
    Preprocessor &PP = tu->ast->getPreprocessor();
    for (const auto &m : PP.macros()) {
        const IdentifierInfo *II = m.first;
        const MacroInfo *MI = PP.getMacroInfo(const_cast<IdentifierInfo *>(II));
        if (!MI) continue;
        SourceLocation sp = SM.getSpellingLoc(MI->getDefinitionLoc());
        if (sp.isInvalid() || SM.getFileID(sp) != SM.getMainFileID()) continue;
        auto p = SM.getPresumedLoc(sp);
        if (!p.isValid()) continue;
        SemanticTokenEntry t;
        t.line       = p.getLine();
        t.col        = source_location_utf16_col(SM, sp);
        t.length     = utf16_length(II->getName());
        t.token_type = CB_TOK_MACRO;
        list->tokens.push_back(t);
    }

    // Sort by file position so callers get a predictable order.
    std::sort(list->tokens.begin(), list->tokens.end(),
              [](const SemanticTokenEntry &a, const SemanticTokenEntry &b) {
                  return a.line != b.line ? a.line < b.line : a.col < b.col;
              });
    // Collapse tokens sharing a (line, col): a ClassTemplateDecl and its
    // templated CXXRecordDecl (or a function template and its inner decl) both
    // report the same name at the same location.
    list->tokens.erase(
        std::unique(list->tokens.begin(), list->tokens.end(),
                    [](const SemanticTokenEntry &a, const SemanticTokenEntry &b) {
                        return a.line == b.line && a.col == b.col;
                    }),
        list->tokens.end());
    return list;
    });
}

size_t cb_semantic_token_count(const CB_SemanticTokenList *list) {
    return list ? list->tokens.size() : 0;
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
    return cb_recover(static_cast<CB_FormatList *>(nullptr),
                      [&]() -> CB_FormatList * {
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
    });
}

size_t cb_format_edit_count(const CB_FormatList *list) {
    return list ? list->edits.size() : 0;
}

void cb_format_edit_get(const CB_FormatList *list, size_t i, CB_FormatEdit *out) {
    auto *ml = const_cast<CB_FormatList *>(list);
    ml->current = ml->edits[i];
    out->offset      = ml->current.offset;
    out->length      = ml->current.length;
    out->replacement = ml->current.replacement.c_str();
}

void cb_format_list_destroy(CB_FormatList *list) { delete list; }
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
    // Braced statement bodies (for/while/if/switch and bare blocks).  Function
    // bodies are CompoundStmts too, so this also covers them; duplicates of the
    // VisitFunctionDecl range are removed in cb_folding_ranges.
    bool VisitCompoundStmt(CompoundStmt *S) {
        addRange(S->getSourceRange(), "region");
        return true;
    }
};

CB_FoldingRangeList *cb_folding_ranges(CB_TransUnit *tu) {
    return cb_recover(tu, __func__, static_cast<CB_FoldingRangeList *>(nullptr),
                      [&]() -> CB_FoldingRangeList * {
    auto *list = new CB_FoldingRangeList{};
    ASTContext    &Ctx     = tu->ast->getASTContext();
    SourceManager &SM      = Ctx.getSourceManager();
    FileID         main_fid = SM.getMainFileID();

    FoldingVisitor vis(SM, main_fid, list->entries);
    vis.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Multi-line comment blocks fold as kind "comment" (clangd folds the file
    // header and multi-line doc comments).  Adjacent line comments are merged
    // into a single RawComment by clang, so each block has one span.
    // Comment folds: lex the main file in raw mode (the AST's RawCommentList only
    // keeps *doc* comments, so it misses the file header and plain // blocks that
    // clangd folds).  Merge runs of whole-line comments on consecutive lines and
    // multi-line block comments; skip trailing comments that sit after code
    // (e.g. `int x; ///< ...`), which clangd does not fold.
    {
        llvm::MemoryBufferRef MB = SM.getBufferOrFake(main_fid);
        llvm::StringRef buf = MB.getBuffer();
        Lexer lex(main_fid, MB, SM, Ctx.getLangOpts());
        lex.SetCommentRetentionState(true);
        int curStart = -1, curEnd = -1;
        auto flush = [&]() {
            if (curStart >= 0 && curEnd > curStart)
                list->entries.push_back(
                    {(uint32_t)curStart, (uint32_t)curEnd, "comment"});
            curStart = curEnd = -1;
        };
        Token tok;
        for (lex.LexFromRawLexer(tok); tok.isNot(tok::eof);
             lex.LexFromRawLexer(tok)) {
            if (tok.isNot(tok::comment)) continue;
            auto [fid, off] = SM.getDecomposedLoc(tok.getLocation());
            auto pb = SM.getPresumedLoc(tok.getLocation());
            auto pe = SM.getPresumedLoc(tok.getEndLoc());
            if (!pb.isValid() || !pe.isValid()) continue;
            // Whole-line comment? only whitespace precedes it on its line.
            unsigned ls = off;
            while (ls > 0 && buf[ls - 1] != '\n') --ls;
            bool leading = true;
            for (unsigned i = ls; i < off; ++i)
                if (buf[i] != ' ' && buf[i] != '\t') { leading = false; break; }
            if (!leading) { flush(); continue; }
            int s = (int)pb.getLine(), e = (int)pe.getLine();
            if (curStart >= 0 && s <= curEnd + 1)
                curEnd = std::max(curEnd, e);
            else { flush(); curStart = s; curEnd = e; }
        }
        flush();
    }

    std::sort(list->entries.begin(), list->entries.end(),
              [](const FoldingEntry &a, const FoldingEntry &b) {
                  if (a.start_line != b.start_line) return a.start_line < b.start_line;
                  return a.end_line < b.end_line; });
    // Drop duplicate ranges (a function body added by both VisitFunctionDecl and
    // VisitCompoundStmt collapses to one entry).
    list->entries.erase(
        std::unique(list->entries.begin(), list->entries.end(),
                    [](const FoldingEntry &a, const FoldingEntry &b) {
                        return a.start_line == b.start_line &&
                               a.end_line == b.end_line; }),
        list->entries.end());
    return list;
    });
}

size_t cb_folding_range_count(const CB_FoldingRangeList *list) {
    return list ? list->entries.size() : 0;
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
