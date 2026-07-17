// Internal C++ header — NOT part of the public C API (clang_bridge.h).
// Include this in every cb_*.cpp file to get shared types, includes, and helpers.
#pragma once

#include "clang_bridge.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Comment.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
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
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/Support/CrashRecoveryContext.h>
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

// ── Shared internal structs ───────────────────────────────────────────────────

struct WorkspaceSymEntry {
    std::string name, detail, kind, file, usr;
    uint32_t    line = 0, col = 0;
};

struct ReferenceEntry {
    std::string file;
    uint32_t    line, col;
    bool        is_definition;
};

struct CB_Index {
    std::string last_error;
    std::unordered_multimap<std::string, WorkspaceSymEntry> sym_index;
    bool poisoned = false;
};

struct CB_TransUnit {
    std::unique_ptr<ASTUnit> ast;
    std::string last_error;
    bool poisoned = false;
};

// ── Shared helper declarations (defined in core.cpp) ──────────────────────────

StringRef   safeDeclName(const NamedDecl *D);
std::string declKind(const NamedDecl *D);
std::string prettySignature(const NamedDecl *ND, ASTContext &Ctx);
bool        isDefinition(const NamedDecl *ND);
std::string find_clang_resource_dir();

/// Locate the most specific NamedDecl under the cursor at (line, col).
const NamedDecl *locate_symbol_at(ASTUnit *ast, uint32_t line, uint32_t col);

/// Translate a 1-based `(line, utf16_col)` position from the LSP client into a
/// `SourceLocation`, converting the UTF-16 column to a byte column against the
/// line's buffer. clang's `translateLineCol` expects byte columns, but LSP
/// positions are UTF-16 code units — so on any line with multi-byte characters
/// the raw column lands on the wrong token. Use this for every incoming cursor.
clang::SourceLocation translate_line_col_utf16(const clang::SourceManager &SM,
                                               clang::FileID fid,
                                               uint32_t line,
                                               uint32_t utf16_col);

/// Convert clang's 1-based byte column for `loc` into a 1-based UTF-16 column
/// for the LSP-facing C API. The location is reduced to its file location so
/// macro expansion sites are measured against the buffer clients display.
uint32_t source_location_utf16_col(const clang::SourceManager &SM,
                                  clang::SourceLocation loc);

/// Count UTF-16 code units in UTF-8 source text.
uint32_t utf16_length(llvm::StringRef text);

/// Execute Clang/LLVM work behind LLVM's platform crash-recovery boundary.
/// A crashed TU is poisoned and its AST intentionally leaked: running its
/// destructor or querying it again is unsafe after signal recovery.
bool cb_run_safely(CB_Index *idx, const char *operation,
                   llvm::function_ref<void()> fn);
bool cb_run_safely(CB_TransUnit *tu, const char *operation,
                   llvm::function_ref<void()> fn);
bool cb_run_safely(llvm::function_ref<void()> fn);

template <typename Result, typename Fn>
Result cb_recover(CB_TransUnit *tu, const char *operation,
                  Result fallback, Fn &&fn) {
    Result result = fallback;
    if (!cb_run_safely(tu, operation, [&] { result = fn(); }))
        return fallback;
    return result;
}

template <typename Result, typename Fn>
Result cb_recover(CB_Index *idx, const char *operation,
                  Result fallback, Fn &&fn) {
    Result result = fallback;
    if (!cb_run_safely(idx, operation, [&] { result = fn(); }))
        return fallback;
    return result;
}

template <typename Result, typename Fn>
Result cb_recover(Result fallback, Fn &&fn) {
    Result result = fallback;
    if (!cb_run_safely([&] { result = fn(); }))
        return fallback;
    return result;
}
