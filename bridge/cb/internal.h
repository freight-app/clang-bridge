// Internal C++ header — NOT part of the public C API (clang_bridge.h).
// Include this in every cb_*.cpp file to get shared types, includes, and helpers.
#pragma once

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
};

struct CB_TransUnit {
    std::unique_ptr<ASTUnit> ast;
};

// ── Shared helper declarations (defined in core.cpp) ──────────────────────────

StringRef   safeDeclName(const NamedDecl *D);
std::string declKind(const NamedDecl *D);
std::string prettySignature(const NamedDecl *ND, ASTContext &Ctx);
bool        isDefinition(const NamedDecl *ND);
std::string find_clang_resource_dir();

/// Locate the most specific NamedDecl under the cursor at (line, col).
const NamedDecl *locate_symbol_at(ASTUnit *ast, uint32_t line, uint32_t col);
