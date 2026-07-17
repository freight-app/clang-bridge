# clang-bridge

Rust FFI bindings to LLVM/Clang for in-process C and C++ source intelligence.
Used by `freight lsp` to provide hover, goto-definition, completion, and
diagnostics without spawning an external language server.

---

## Architecture

```
┌────────────────────────────────────────┐
│  freight lsp  (Rust)                   │
│                                        │
│  clang_bridge::{hover, goto,           │
│                 completion, diag, ...} │
└───────────────┬────────────────────────┘
                │  unsafe FFI (extern "C")
┌───────────────▼────────────────────────┐
│  bridge/clang_bridge.{h,cpp}  (C++)    │
│                                        │
│  ASTUnit  ·  RecursiveASTVisitor       │
│  CodeCompleteConsumer                  │
│  clang::index  ·  clang::format        │
└───────────────┬────────────────────────┘
                │  LLVM/Clang shared libs
                │  (libclang-cpp, LLVM)
```

The C++ layer is compiled by `build.rs` via `cc-rs` and linked into the Rust
crate. There is no subprocess, no `libclang` C API — it talks directly to the
Clang C++ APIs through a thin hand-written C ABI (`cb_*` functions).

---

## Current API

| Rust module | C entry point | LSP method |
|---|---|---|
| `hover` | `cb_hover_markdown` | `textDocument/hover` |
| `goto` | `cb_goto_definition` | `textDocument/definition` |
| `completion` | `cb_complete` | `textDocument/completion` |
| `diag` | `cb_diag_iter` | `textDocument/publishDiagnostics` |
| `symbol` | `cb_symbol_at` | internal hover/goto helper |
| `doc` | `cb_doc_extract` | `freight doc` index |
| *(lib.rs)* | `cb_transunit_reparse` | `textDocument/didChange` |

Symbol resolution (`cb_symbol_at`, `cb_hover_markdown`, `cb_goto_definition`)
uses a two-stage AST walk:

1. **Reference sites** — `DeclRefExpr`, `MemberExpr`, `TagTypeLoc`,
   `TypedefTypeLoc`, `TemplateSpecializationTypeLoc`. Matches the cursor
   against the name-token span so hovering on a *use* works, not just the
   declaration.
2. **Declaration sites** — fallback `NamedDecl` location match (original
   behaviour, still needed for parameter lists, class members, etc.).

---

## Build requirements

- Clang/LLVM ≥ 14 dev headers and shared libraries
- `llvm-config` on `$PATH` (or set `CLANG_BRIDGE_LLVM_CONFIG`)
- `clang++` on `$PATH` (or set `CLANG_BRIDGE_CLANGXX`)

On Arch Linux:
```sh
sudo pacman -S llvm clang
```

On Ubuntu/Debian (LLVM 18+):
```sh
sudo apt install llvm-18-dev libclang-18-dev
```

Environment overrides:
```sh
CLANG_BRIDGE_LLVM_CONFIG=/usr/lib/llvm-18/bin/llvm-config cargo build
CLANG_BRIDGE_CLANGXX=clang++-18 cargo build
```

---

## Usage

```rust
use clang_bridge::{hover, goto, completion, Index};

let idx = Index::new();
let tu = idx.parse("/path/to/file.cpp", &["-std=c++20", "-I/usr/include"])
    .expect("parse failed");

// Hover at line 10, column 5 (1-based)
if let Some(md) = hover::hover_markdown(&tu, 10, 5) {
    println!("{md}");
}

// Go to definition
if let Some(loc) = goto::goto_definition(&tu, 10, 5) {
    println!("{}:{}:{}", loc.file, loc.line, loc.col);
}

// Code completion
for item in completion::complete(&tu, 15, 8, None) {
    println!("{} (kind {})", item.label, item.kind);
}

// Diagnostics
for d in tu.diagnostics() {
    println!("[{}] {}:{} — {}", d.severity, d.line, d.col, d.message);
}

// Reparse with unsaved content after an edit
tu.reparse(Some("int main() { return 0; }"));
```

---

## Optional feature flag

When used inside freight, clang-bridge is an optional dependency:

```toml
[dependencies]
clang-bridge = { path = "../clang-bridge", optional = true }

[features]
clang = ["dep:clang-bridge"]
```

When the feature is absent, all entry points in `src/lsp/indexers/Clang.rs`
return empty/None without linking against LLVM.

---

## See also

- [`TODO.md`](TODO.md) — prioritised list of missing APIs and quality gaps
- [`bridge/clang_bridge.h`](bridge/clang_bridge.h) — full C ABI reference
- [`crates/freight/src/lsp/indexers/Clang.rs`](../freight/src/lsp/indexers/Clang.rs) — consumer

---

## License

clang-bridge is available under the [MIT License](LICENSE).
