//! clang-tidy runner — invokes `clang-tidy` as a subprocess and parses its
//! diagnostic output into the same `Diagnostic` type used by the AST indexer.

use std::process::{Command, Stdio};

use crate::diag::{Diagnostic, Severity};

/// Run `clang-tidy` on `file` with the given compile `flags`.
///
/// - `checks`: optional `-checks=` argument (e.g. `"modernize-*,-fuchsia-*"`).
///   `None` uses clang-tidy's default check selection (config file or `*`).
/// - `config`: path to a `.clang-tidy` config file; `None` lets clang-tidy
///   search upward from the file as usual.
///
/// Returns an iterator over every diagnostic emitted for any file.  Callers
/// that only want diagnostics for the input file should filter on `d.file`.
pub fn run<'a>(
    checks: Option<&'a str>,
    file: &'a str,
    config: Option<&'a str>,
    flags: &'a [&'a str],
) -> impl Iterator<Item = Diagnostic> + 'a {
    let diags = run_inner(checks, file, config, flags);
    diags.into_iter()
}

fn run_inner(
    checks: Option<&str>,
    file: &str,
    config: Option<&str>,
    flags: &[&str],
) -> Vec<Diagnostic> {
    let mut cmd = Command::new("clang-tidy");
    if let Some(c) = checks {
        cmd.arg(format!("--checks={c}"));
    }
    if let Some(cfg) = config {
        cmd.arg(format!("--config-file={cfg}"));
    }
    cmd.arg(file);
    cmd.arg("--");
    cmd.args(flags);
    cmd.stdout(Stdio::piped()).stderr(Stdio::null());

    let output = match cmd.output() {
        Ok(o) => o,
        Err(_) => return Vec::new(),
    };

    let text = String::from_utf8_lossy(&output.stdout);
    parse_output(&text)
}

/// Parse clang-tidy text output into `Diagnostic` items.
///
/// Expected line format:
/// ```text
/// /path/to/file.cpp:LINE:COL: SEVERITY: MESSAGE [CHECK-NAME]
/// ```
fn parse_output(text: &str) -> Vec<Diagnostic> {
    let mut diags: Vec<Diagnostic> = Vec::new();

    for line in text.lines() {
        // Skip blank lines and lines that look like notes / context.
        // A diagnostic header has the pattern:   path:line:col: severity: msg
        let Some((loc, rest)) = split_location(line) else {
            continue;
        };
        let Some((file, line_num, col_num)) = parse_loc(loc) else {
            continue;
        };
        let Some((sev_str, msg_with_check)) = rest.split_once(':') else {
            continue;
        };
        let sev_str = sev_str.trim();
        let severity = match sev_str {
            "error" | "fatal error" => Severity::Error,
            "warning" => Severity::Warning,
            "note" => Severity::Note,
            "remark" => Severity::Remark,
            _ => continue,
        };

        let msg_with_check = msg_with_check.trim();
        // Extract trailing [check-name] if present.
        let (message, check_name) = if let Some(bracket_start) = msg_with_check.rfind('[') {
            let after = &msg_with_check[bracket_start..];
            if after.ends_with(']') {
                let check = after[1..after.len() - 1].to_string();
                let msg = msg_with_check[..bracket_start].trim_end().to_string();
                (msg, Some(check))
            } else {
                (msg_with_check.to_string(), None)
            }
        } else {
            (msg_with_check.to_string(), None)
        };

        diags.push(Diagnostic {
            file: file.to_string(),
            line: line_num,
            col: col_num,
            end_line: line_num,
            end_col: col_num,
            severity,
            message,
            check_name,
            fixits: Vec::new(),
        });
    }

    diags
}

/// Split `"path:line:col: rest"` into `("path:line:col", "rest")`.
/// Handles Windows drive letters by requiring at least two colons after the path.
fn split_location(line: &str) -> Option<(&str, &str)> {
    // Find the `: ` that separates location from severity.  We look for
    // `: ` after a `:col` segment — that means we need at least 3 colons.
    let bytes = line.as_bytes();
    let mut colon_count = 0;
    for (i, &b) in bytes.iter().enumerate() {
        if b == b':' {
            colon_count += 1;
            // After the third colon, expect ` ` (which would be `: severity`)
            if colon_count >= 3 && i + 1 < bytes.len() && bytes[i + 1] == b' ' {
                return Some((&line[..i], &line[i + 2..]));
            }
        }
    }
    None
}

/// Parse `"path:line:col"` into `(&str, u32, u32)`.
fn parse_loc(loc: &str) -> Option<(&str, u32, u32)> {
    // Split from the right to get col, then line, then path.
    let (rest, col_str) = loc.rsplit_once(':')?;
    let (path, line_str) = rest.rsplit_once(':')?;
    let line: u32 = line_str.parse().ok()?;
    let col: u32 = col_str.parse().ok()?;
    Some((path, line, col))
}
