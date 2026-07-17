//! Differential API audit against clangd.
//!
//! Run with:
//! `cargo test -p clang-bridge --test clangd_oracle -- --ignored --nocapture`

use clang_bridge::{diag::Diagnostic, diag::Severity, sighelp, Index};
use serde_json::{json, Value};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::time::{Duration, Instant};

struct Clangd {
    child: Child,
    stdin: ChildStdin,
    messages: Receiver<Value>,
}

impl Clangd {
    fn start(root: &Path) -> Option<Self> {
        let binary = std::env::var("CLANGD").unwrap_or_else(|_| "clangd".into());
        let mut child = Command::new(binary)
            .args([
                "--background-index=false",
                "--header-insertion=never",
                "--clang-tidy=false",
                "--log=error",
            ])
            .current_dir(root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .ok()?;
        let stdin = child.stdin.take()?;
        let mut stdout = child.stdout.take()?;
        let (tx, messages) = mpsc::channel();
        std::thread::spawn(move || {
            while let Ok(message) = read_message(&mut stdout) {
                if tx.send(message).is_err() {
                    break;
                }
            }
        });

        let mut server = Self {
            child,
            stdin,
            messages,
        };
        server.send(json!({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "rootUri": file_uri(root),
                "capabilities": {
                    "textDocument": {
                        "publishDiagnostics": { "relatedInformation": true },
                        "signatureHelp": {
                            "signatureInformation": {
                                "parameterInformation": { "labelOffsetSupport": true }
                            }
                        },
                        "hover": { "contentFormat": ["markdown"] }
                    }
                }
            }
        }));
        server.recv_until(Duration::from_secs(10), |message| message["id"] == 1)?;
        server.send(json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }));
        Some(server)
    }

    fn diagnostics(&mut self, path: &Path, source: &str) -> Vec<Value> {
        let uri = file_uri(path);
        self.open(path, source);
        self.recv_until(Duration::from_secs(10), |message| {
            message["method"] == "textDocument/publishDiagnostics"
                && message["params"]["uri"] == uri
                && message["params"]["diagnostics"]
                    .as_array()
                    .is_some_and(|diagnostics| !diagnostics.is_empty())
        })
        .and_then(|message| message["params"]["diagnostics"].as_array().cloned())
        .expect("clangd did not publish diagnostics")
    }

    fn open(&mut self, path: &Path, source: &str) {
        let uri = file_uri(path);
        self.send(json!({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": uri,
                    "languageId": "cpp",
                    "version": 1,
                    "text": source
                }
            }
        }));
    }

    fn request(&mut self, id: u64, method: &str, params: Value) -> Value {
        self.send(json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": method,
            "params": params
        }));
        self.recv_until(Duration::from_secs(10), |message| message["id"] == id)
            .and_then(|message| message.get("result").cloned())
            .expect("clangd request did not return a result")
    }

    fn send(&mut self, message: Value) {
        let body = serde_json::to_vec(&message).unwrap();
        write!(self.stdin, "Content-Length: {}\r\n\r\n", body.len()).unwrap();
        self.stdin.write_all(&body).unwrap();
        self.stdin.flush().unwrap();
    }

    fn recv_until(&self, timeout: Duration, predicate: impl Fn(&Value) -> bool) -> Option<Value> {
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = deadline.checked_duration_since(Instant::now())?;
            let message = self.messages.recv_timeout(remaining).ok()?;
            if predicate(&message) {
                return Some(message);
            }
        }
    }
}

impl Drop for Clangd {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn read_message(stdout: &mut impl Read) -> std::io::Result<Value> {
    let mut headers = Vec::new();
    while !headers.ends_with(b"\r\n\r\n") {
        let mut byte = [0];
        stdout.read_exact(&mut byte)?;
        headers.push(byte[0]);
    }
    let header_text = String::from_utf8_lossy(&headers);
    let length = header_text
        .lines()
        .find_map(|line| {
            line.strip_prefix("Content-Length:")
                .and_then(|value| value.trim().parse::<usize>().ok())
        })
        .ok_or_else(|| std::io::Error::other("missing Content-Length"))?;
    let mut body = vec![0; length];
    stdout.read_exact(&mut body)?;
    serde_json::from_slice(&body).map_err(std::io::Error::other)
}

fn file_uri(path: &Path) -> String {
    format!("file://{}", path.canonicalize().unwrap().display())
}

fn normalized_message(message: &str) -> String {
    let message = message
        .strip_prefix("In included file: ")
        .unwrap_or(message);
    message
        .strip_suffix(" (fix available)")
        .unwrap_or(message)
        .to_ascii_lowercase()
}

fn lsp_severity(severity: Severity) -> u64 {
    match severity {
        Severity::Note | Severity::Remark => 4,
        Severity::Warning => 2,
        Severity::Error | Severity::Fatal => 1,
    }
}

fn bridge_range(diagnostic: &Diagnostic) -> Value {
    json!({
        "start": {
            "line": diagnostic.line.saturating_sub(1),
            "character": diagnostic.col.saturating_sub(1)
        },
        "end": {
            "line": diagnostic.end_line.saturating_sub(1),
            "character": diagnostic.end_col.saturating_sub(1)
        }
    })
}

fn bridge_hover_range(range: clang_bridge::hover::HoverRange) -> Value {
    json!({
        "start": {
            "line": range.start_line.saturating_sub(1),
            "character": range.start_col.saturating_sub(1)
        },
        "end": {
            "line": range.end_line.saturating_sub(1),
            "character": range.end_col.saturating_sub(1)
        }
    })
}

fn clangd_hover_text(hover: &Value) -> &str {
    hover["contents"]["value"]
        .as_str()
        .or_else(|| hover["contents"].as_str())
        .expect("clangd markdown hover contents")
}

fn parse(path: &Path, working_dir: &Path) -> (Index, clang_bridge::TranslationUnit) {
    let index = Index::new();
    let tu = index
        .parse(
            path.to_str().unwrap(),
            working_dir.to_str().unwrap(),
            &["-std=c++17"],
        )
        .expect("bridge should retain a TU for malformed source");
    (index, tu)
}

fn compiler_diagnostics(diagnostics: Vec<Value>) -> Vec<Value> {
    diagnostics
        .into_iter()
        .filter(|diagnostic| diagnostic["source"] == "clang")
        .collect()
}

#[test]
#[ignore = "requires clangd; run explicitly for the differential audit"]
fn diagnostics_match_clangd_on_broken_source_and_nested_header() {
    let fixtures = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let broken = fixtures.join("broken.cpp");
    let broken_source = std::fs::read_to_string(&broken).unwrap();
    let mut clangd = Clangd::start(&fixtures).expect("clangd must be available");
    let clangd_broken = compiler_diagnostics(clangd.diagnostics(&broken, &broken_source));
    let (_index, tu) = parse(&broken, &fixtures);
    let bridge_broken: Vec<_> = tu.diagnostics().collect();

    assert_eq!(bridge_broken.len(), clangd_broken.len());
    for bridge in &bridge_broken {
        let clangd = clangd_broken
            .iter()
            .find(|diagnostic| {
                normalized_message(diagnostic["message"].as_str().unwrap())
                    == normalized_message(&bridge.message)
            })
            .unwrap_or_else(|| panic!("clangd omitted bridge diagnostic: {bridge:?}"));
        assert_eq!(
            clangd["range"],
            bridge_range(bridge),
            "range for {}",
            bridge.message
        );
        assert_eq!(clangd["severity"], lsp_severity(bridge.severity));
        assert_eq!(clangd["relatedInformation"], json!([]));
    }

    let dir = tempfile::tempdir().unwrap();
    let main = dir.path().join("main.cpp");
    let outer = dir.path().join("outer.hpp");
    let inner = dir.path().join("inner.hpp");
    let main_source = "#include \"outer.hpp\"\nint main() { return 0; }\n";
    std::fs::write(&main, main_source).unwrap();
    std::fs::write(&outer, "#pragma once\n#include \"inner.hpp\"\n").unwrap();
    std::fs::write(&inner, "#pragma once\ninline int broken = missing_name;\n").unwrap();

    let mut header_clangd = Clangd::start(dir.path()).expect("clangd must be available");
    let clangd_header = compiler_diagnostics(header_clangd.diagnostics(&main, main_source));
    let (_index, tu) = parse(&main, dir.path());
    let bridge = tu
        .diagnostics()
        .find(|diagnostic| diagnostic.message.contains("missing_name"))
        .expect("bridge nested-header diagnostic");
    let clangd = clangd_header
        .iter()
        .find(|diagnostic| {
            diagnostic["message"]
                .as_str()
                .is_some_and(|m| m.contains("missing_name"))
        })
        .expect("clangd nested-header diagnostic");

    assert_eq!(clangd["severity"], lsp_severity(bridge.severity));
    assert_eq!(
        normalized_message(clangd["message"].as_str().unwrap()),
        normalized_message(&bridge.message)
    );
    assert_eq!(
        clangd["range"],
        json!({
            "start": { "line": 0, "character": 9 },
            "end": { "line": 0, "character": 20 }
        })
    );
    assert_eq!(bridge.include_anchor.as_ref().unwrap().line, 1);
    assert_eq!(bridge.include_anchor.as_ref().unwrap().col, 10);
    assert_eq!(
        clangd["relatedInformation"][0]["location"]["uri"],
        file_uri(&inner)
    );
    assert_eq!(
        clangd["relatedInformation"][0]["location"]["range"],
        bridge_range(&bridge)
    );
}

fn clangd_parameter_labels(signature: &Value) -> Vec<String> {
    let label = signature["label"].as_str().unwrap();
    signature["parameters"]
        .as_array()
        .unwrap()
        .iter()
        .map(|parameter| {
            let offsets = parameter["label"].as_array().unwrap();
            let start = offsets[0].as_u64().unwrap() as usize;
            let end = offsets[1].as_u64().unwrap() as usize;
            label[start..end].to_string()
        })
        .collect()
}

#[test]
#[ignore = "requires clangd; run explicitly for the differential audit"]
fn signature_help_matches_clangd_for_nested_and_partial_calls() {
    let source = concat!(
        "int inner(int first, int second);\n",
        "int outer(int alpha, int beta, int gamma);\n",
        "void test() {\n",
        "  outer(inner(1, 2), 3, 4);\n",
        "  outer(1, inner(2, 3), 4);\n",
        "  outer(1, 2, );\n",
        "}"
    );
    let dir = tempfile::tempdir().unwrap();
    let main = dir.path().join("main.cpp");
    std::fs::write(&main, source).unwrap();
    let mut clangd = Clangd::start(dir.path()).expect("clangd must be available");
    clangd.open(&main, source);
    let (_index, tu) = parse(&main, dir.path());

    let cases = [
        (3, 15, "inner", 0),
        (3, 18, "inner", 1),
        (3, 22, "outer", 1),
        (4, 21, "inner", 1),
        (4, 25, "outer", 2),
        (5, 14, "outer", 2),
    ];
    for (offset, (line, character, function, active_parameter)) in cases.into_iter().enumerate() {
        let clangd_help = clangd.request(
            10 + offset as u64,
            "textDocument/signatureHelp",
            json!({
                "textDocument": { "uri": file_uri(&main) },
                "position": { "line": line, "character": character },
                "context": { "triggerKind": 1 }
            }),
        );
        let bridge_help = sighelp::signature_help(&tu, line + 1, character + 1)
            .unwrap_or_else(|| panic!("bridge signature help at {line}:{character}"));

        assert_eq!(clangd_help["activeParameter"], active_parameter);
        assert_eq!(bridge_help.active_param, active_parameter);
        let clangd_signature = clangd_help["signatures"]
            .as_array()
            .unwrap()
            .iter()
            .find(|signature| signature["label"].as_str().unwrap().contains(function))
            .expect("clangd overload");
        let bridge_signature = bridge_help
            .overloads
            .iter()
            .find(|signature| signature.label.contains(function))
            .expect("bridge overload");
        let bridge_params: Vec<_> = bridge_signature
            .params
            .iter()
            .map(|parameter| parameter.label.clone())
            .collect();
        assert_eq!(clangd_parameter_labels(clangd_signature), bridge_params);
    }
}

#[test]
#[ignore = "requires clangd; run explicitly for the differential audit"]
fn hover_matches_clangd_facts_and_identifier_ranges() {
    let source = concat!(
        "/// Add two integers.\n",
        "int add(int a, int b) { return a + b; }\n",
        "int main() {\n",
        "  auto answer = add(40, 2);\n",
        "  return answer;\n",
        "}\n",
    );
    let dir = tempfile::tempdir().unwrap();
    let main = dir.path().join("main.cpp");
    std::fs::write(&main, source).unwrap();
    let mut clangd = Clangd::start(dir.path()).expect("clangd must be available");
    clangd.open(&main, source);
    let (_index, tu) = parse(&main, dir.path());

    let cases = [
        (3, 18, &["int add(int a, int b)", "Add two integers"][..]),
        (4, 11, &["int", "answer"][..]),
    ];
    for (offset, (line, character, facts)) in cases.into_iter().enumerate() {
        let clangd_hover = clangd.request(
            30 + offset as u64,
            "textDocument/hover",
            json!({
                "textDocument": { "uri": file_uri(&main) },
                "position": { "line": line, "character": character }
            }),
        );
        let bridge_hover = clang_bridge::hover::hover_full(&tu, line + 1, character + 1)
            .unwrap_or_else(|| panic!("bridge hover at {line}:{character}"));
        let range = clang_bridge::hover::hover_range(&tu, line + 1, character + 1)
            .expect("bridge identifier range");

        assert_eq!(clangd_hover["range"], bridge_hover_range(range));
        for fact in facts {
            assert!(
                clangd_hover_text(&clangd_hover).contains(fact),
                "clangd omitted {fact:?}: {}",
                clangd_hover_text(&clangd_hover)
            );
            assert!(
                bridge_hover.contains(fact),
                "bridge omitted {fact:?}: {bridge_hover}"
            );
        }
    }
}
