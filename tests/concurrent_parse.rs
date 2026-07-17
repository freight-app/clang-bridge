//! Regression coverage for ClangTool's process-wide working-directory change.

use clang_bridge::{diag::Severity, goto, Index};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Barrier};

fn write_project(root: &Path, symbol: &str, value: i32) -> PathBuf {
    std::fs::create_dir_all(root).expect("create project");
    std::fs::write(
        root.join("config.hpp"),
        format!("inline int {symbol}() {{ return {value}; }}\n"),
    )
    .expect("write header");
    let source = root.join("main.cpp");
    std::fs::write(
        &source,
        format!("#include \"config.hpp\"\nint use() {{ return {symbol}(); }}\n"),
    )
    .expect("write source");
    source
}

#[test]
fn parallel_projects_keep_relative_includes_scoped_to_their_working_dir() {
    let fixture = tempfile::tempdir().expect("concurrent parse fixture");
    let left_root = fixture.path().join("left");
    let right_root = fixture.path().join("right");
    let left_source = write_project(&left_root, "left_value", 1);
    let right_source = write_project(&right_root, "right_value", 2);

    const THREADS: usize = 12;
    let barrier = Arc::new(Barrier::new(THREADS));
    let mut workers = Vec::new();
    for index in 0..THREADS {
        let (root, source, symbol) = if index % 2 == 0 {
            (left_root.clone(), left_source.clone(), "left_value")
        } else {
            (right_root.clone(), right_source.clone(), "right_value")
        };
        let barrier = Arc::clone(&barrier);
        workers.push(std::thread::spawn(move || {
            barrier.wait();
            let clang = Index::new();
            let tu = clang
                .parse(
                    source.to_str().unwrap(),
                    root.to_str().unwrap(),
                    &["-std=c++17", "-I."],
                )
                .unwrap_or_else(|| panic!("parse {symbol}: {:?}", clang.last_error()));

            let errors: Vec<_> = tu
                .diagnostics()
                .filter(|diag| diag.severity == Severity::Error || diag.severity == Severity::Fatal)
                .map(|diag| diag.message)
                .collect();
            assert!(errors.is_empty(), "{symbol} diagnostics: {errors:?}");

            let line = format!("int use() {{ return {symbol}(); }}");
            let col = line.find(symbol).unwrap() as u32 + 1;
            let definition = goto::goto_definition(&tu, 2, col)
                .unwrap_or_else(|| panic!("definition for {symbol}"));
            assert_eq!(
                std::fs::canonicalize(&definition.file).expect("canonical definition"),
                std::fs::canonicalize(root.join("config.hpp")).expect("canonical expected")
            );
        }));
    }

    for worker in workers {
        worker.join().expect("parallel parse worker");
    }
}

#[test]
fn parallel_unsaved_parses_with_the_same_virtual_path_keep_their_own_content() {
    const THREADS: usize = 12;
    let barrier = Arc::new(Barrier::new(THREADS));
    let mut workers = Vec::new();
    for index in 0..THREADS {
        let barrier = Arc::clone(&barrier);
        workers.push(std::thread::spawn(move || {
            let symbol = format!("thread_value_{index}");
            let source = format!(
                "int {symbol}() {{ return {index}; }}\nint use() {{ return {symbol}(); }}\n"
            );
            barrier.wait();
            let clang = Index::new();
            let tu = clang
                .parse_unsaved(
                    "/tmp/shared_concurrent_buffer.cpp",
                    "/tmp",
                    &source,
                    &["-std=c++17"],
                )
                .unwrap_or_else(|| panic!("unsaved {index}: {:?}", clang.last_error()));

            let declaration_col = "int ".len() as u32 + 1;
            assert_eq!(
                tu.symbol_at(1, declaration_col)
                    .unwrap_or_else(|| panic!("symbol for thread {index}"))
                    .name(),
                symbol
            );
        }));
    }

    for worker in workers {
        worker.join().expect("parallel unsaved worker");
    }
}
