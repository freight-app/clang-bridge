//! Regression and timing coverage for ASTUnit precompiled-preamble reuse.

use clang_bridge::{completion, diag::Severity, goto, Index};
use std::sync::Mutex;
use std::time::{Duration, Instant};

static CLANG_LOCK: Mutex<()> = Mutex::new(());

fn utf16_col(line: &str, needle: &str) -> u32 {
    let byte = line.find(needle).expect("needle in line");
    line[..byte].encode_utf16().count() as u32 + 1
}

fn errors(tu: &clang_bridge::TranslationUnit) -> Vec<String> {
    tu.diagnostics()
        .filter(|diag| diag.severity == Severity::Error || diag.severity == Severity::Fatal)
        .map(|diag| diag.message)
        .collect()
}

#[test]
fn editing_first_include_rebuilds_preamble() {
    let _guard = CLANG_LOCK.lock().unwrap_or_else(|err| err.into_inner());
    let dir = tempfile::tempdir().expect("preamble fixture");
    std::fs::write(
        dir.path().join("first.hpp"),
        "inline int from_first() { return 1; }\n",
    )
    .expect("write first header");
    std::fs::write(
        dir.path().join("second.hpp"),
        "inline int from_second() { return 2; }\n",
    )
    .expect("write second header");

    let path = dir.path().join("main.cpp");
    let initial = "#include \"first.hpp\"\nint use() { return from_first(); }\n";
    std::fs::write(&path, initial).expect("write source");
    let index = Index::new();
    let tu = index
        .parse(
            path.to_str().unwrap(),
            dir.path().to_str().unwrap(),
            &["-std=c++17", "-I."],
        )
        .expect("initial parse");

    // The first reparse reaches the precompile threshold while preserving the
    // include preamble. The second changes its first directive and must rebuild.
    let warm = "#include \"first.hpp\"\nint use() { return from_first() + 1; }\n";
    assert!(tu.reparse(Some(warm)), "warm reparse");
    let changed = "#include \"second.hpp\"\nint use() { return from_second(); }\n";
    assert!(tu.reparse(Some(changed)), "include-boundary reparse");
    assert!(
        errors(&tu).is_empty(),
        "changed preamble diagnostics: {:?}",
        errors(&tu)
    );

    let inclusions: Vec<_> = tu.inclusions().iter().collect();
    assert!(
        inclusions
            .iter()
            .any(|entry| entry.included_file.ends_with("second.hpp")),
        "rebuilt include graph: {inclusions:?}"
    );
    assert!(
        inclusions
            .iter()
            .all(|entry| !entry.included_file.ends_with("first.hpp")),
        "stale preamble inclusion: {inclusions:?}"
    );

    let use_line = changed.lines().nth(1).unwrap();
    let location = goto::goto_definition(&tu, 2, utf16_col(use_line, "from_second"))
        .expect("definition from rebuilt preamble");
    assert!(
        location.file.ends_with("second.hpp"),
        "expected second.hpp after preamble rebuild, got {}",
        location.file
    );
}

#[test]
fn completion_after_reparse_preserves_cached_ast() {
    let _guard = CLANG_LOCK.lock().unwrap_or_else(|err| err.into_inner());
    let dir = tempfile::tempdir().expect("completion fixture");
    std::fs::write(
        dir.path().join("api.hpp"),
        "struct Api { int first; int second; };\n",
    )
    .expect("write API header");

    let path = dir.path().join("main.cpp");
    let initial = concat!(
        "#include \"api.hpp\"\n",
        "int use() {\n",
        "  Api value;\n",
        "  return value.first;\n",
        "}\n",
    );
    std::fs::write(&path, initial).expect("write source");
    let index = Index::new();
    let tu = index
        .parse(
            path.to_str().unwrap(),
            dir.path().to_str().unwrap(),
            &["-std=c++17", "-I."],
        )
        .expect("initial parse");

    let warm = initial.replace("value.first", "value.first + 0");
    assert!(tu.reparse(Some(&warm)), "warm reparse");
    let updated = initial.replace("value.first", "value.second");
    assert!(tu.reparse(Some(&updated)), "updated reparse");
    assert!(
        errors(&tu).is_empty(),
        "updated diagnostics: {:?}",
        errors(&tu)
    );

    let updated_line = updated.lines().nth(3).unwrap();
    let second_col = utf16_col(updated_line, "second");
    assert_eq!(
        tu.symbol_at(4, second_col)
            .expect("second before completion")
            .name(),
        "Api::second"
    );
    let symbols_before = tu
        .document_symbols()
        .expect("symbols before completion")
        .len();
    let tokens_before = tu.semantic_tokens().len();

    let incomplete = updated.replace("value.second", "value.");
    let incomplete_line = incomplete.lines().nth(3).unwrap();
    let completion_col = utf16_col(incomplete_line, "value.") + "value.".len() as u32;
    let labels: Vec<_> = completion::complete(&tu, 4, completion_col, Some(&incomplete))
        .map(|item| item.label)
        .collect();
    assert!(
        labels.iter().any(|label| label == "second"),
        "member completions: {labels:?}"
    );

    assert_eq!(
        tu.symbol_at(4, second_col)
            .expect("second after completion")
            .name(),
        "Api::second"
    );
    assert_eq!(
        tu.document_symbols()
            .expect("symbols after completion")
            .len(),
        symbols_before
    );
    assert_eq!(tu.semantic_tokens().len(), tokens_before);
}

fn percentile(samples: &mut [Duration], numerator: usize) -> Duration {
    samples.sort_unstable();
    let index = ((samples.len() * numerator).div_ceil(100)).saturating_sub(1);
    samples[index]
}

#[test]
#[ignore = "timing probe; run explicitly with --ignored --nocapture"]
fn iostream_vector_reparse_and_completion_latency() {
    let _guard = CLANG_LOCK.lock().unwrap_or_else(|err| err.into_inner());
    let dir = tempfile::tempdir().expect("latency fixture");
    let path = dir.path().join("latency.cpp");
    let source = concat!(
        "#include <iostream>\n",
        "#include <vector>\n",
        "int main() {\n",
        "  std::vector<int> values{1, 2, 3};\n",
        "  std::cout << values.size() << '\\n';\n",
        "  return 0;\n",
        "}\n",
    );
    std::fs::write(&path, source).expect("write latency source");

    let index = Index::new();
    let started = Instant::now();
    let tu = index
        .parse(
            path.to_str().unwrap(),
            dir.path().to_str().unwrap(),
            &["-std=c++17"],
        )
        .expect("initial latency parse");
    let initial = started.elapsed();

    let mut reparses = Vec::new();
    let mut completions = Vec::new();
    for iteration in 0..10 {
        let edited = source.replace("return 0;", &format!("return {};", iteration % 2));
        let started = Instant::now();
        assert!(tu.reparse(Some(&edited)), "timed reparse {iteration}");
        reparses.push(started.elapsed());

        let incomplete = edited.replace("values.size()", "values.");
        let line = incomplete.lines().nth(4).unwrap();
        let col = utf16_col(line, "values.") + "values.".len() as u32;
        let started = Instant::now();
        let labels: Vec<_> = completion::complete(&tu, 5, col, Some(&incomplete))
            .map(|item| item.label)
            .collect();
        completions.push(started.elapsed());
        assert!(
            labels.iter().any(|label| label == "size"),
            "completion {iteration}"
        );
    }

    let reparse_p50 = percentile(&mut reparses, 50);
    let reparse_p95 = percentile(&mut reparses, 95);
    let completion_p50 = percentile(&mut completions, 50);
    let completion_p95 = percentile(&mut completions, 95);
    println!(
        "initial={initial:?} reparse[p50={reparse_p50:?}, p95={reparse_p95:?}] \
         completion[p50={completion_p50:?}, p95={completion_p95:?}]"
    );
}
