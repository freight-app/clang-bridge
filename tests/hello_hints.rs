//! Integration test: inlay hints on a self-contained C++ source.
//!
//! Exercises the clang-bridge hint behaviours that matter for the LSP:
//! user-function parameter-name hints, constructor argument hints, filtering of
//! system-header (`std::…`) call hints, and never leaking internal `__name`s.
//!
//! This used to parse the `cpp/hello` example by absolute path, but that file
//! now uses `import std;` — which needs a prebuilt std-module BMI a bridge test
//! can't supply (so every `std::` symbol becomes undeclared and the AST yields
//! no hints). The source is inlined here for a stable, hermetic check.

use clang_bridge::{inlay, Index};

const SRC: &str = r#"
#include <utility>
#include <cmath>

namespace stats {
double mean(const double values[], int n);
double variance(const double values[], int n);
}

struct Vec2 {
    double x, y;
    Vec2(double x, double y) : x(x), y(y) {}
};

int main() {
    double data[8] = {2, 4, 4, 4, 5, 5, 7, 9};

    // User-defined functions → parameter-name hints ("values:", "n:").
    double m = stats::mean(data, 8);
    double v = stats::variance(data, 8);

    // System-header call (std::sqrt) → its parameter hint must be filtered.
    double s = std::sqrt(v);

    // Constructor with named params → x:, y: hints.
    Vec2 g(1, 1);

    (void) m;
    (void) v;
    (void) s;
    (void) g;
    return 0;
}
"#;

fn parse() -> Option<clang_bridge::TranslationUnit> {
    Index::new().parse_unsaved("/tmp/cb_hello_hints.cpp", "", SRC, &["-std=c++20"])
}

#[test]
fn no_parse_errors() {
    assert!(
        parse().is_some(),
        "parse returned None — system headers <utility>/<cmath> must be reachable"
    );
}

#[test]
fn clang_bridge_inlay_hints() {
    let tu = parse().expect("failed to parse inline source");
    let n_lines = SRC.lines().count() as u32;
    let hints = inlay::inlay_hints(&tu, 1, n_lines);
    let all: Vec<_> = hints.iter().collect();

    println!("\n=== clang-bridge inlay hints (inline hello source) ===");
    for h in &all {
        let kind = if h.kind == 0 { "param" } else { "type" };
        println!(
            "  line {:2} col {:2} [{}]: {:?}",
            h.line, h.col, kind, h.label
        );
    }

    // `values:` parameter-name hints for the two user-function calls.
    let values = all
        .iter()
        .filter(|h| h.kind == 0 && h.label == "values:")
        .count();
    assert_eq!(values, 2, "expected 2 'values:' hints, got {values}");

    // Internal `__name`-style hints must never leak.
    let bad: Vec<_> = all.iter().filter(|h| h.label.starts_with("__")).collect();
    assert!(
        bad.is_empty(),
        "internal parameter-name hints leaked: {bad:?}"
    );

    // Constructor `x:` / `y:` hints for `Vec2(1, 1)`.
    let xy = all
        .iter()
        .filter(|h| h.kind == 0 && (h.label == "x:" || h.label == "y:"))
        .count();
    assert!(xy >= 2, "expected >=2 x:/y: hints, got {xy}");

    println!(
        "\nSummary: {} hints, {values} values:, {xy} x:/y:",
        all.len()
    );
}
