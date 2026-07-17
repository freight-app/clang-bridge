//! Tests for call hierarchy: prepare, incoming calls, and outgoing calls.

use clang_bridge::{callhier, Index};
use std::io::Write;

fn write_temp(name: &str, content: &str) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    write!(f, "{content}").unwrap();
    path
}

// Source shared by several tests:
//   line 1: void helper() {}
//   line 2: void caller() { helper(); }
const BASIC_SRC: &str = "void helper() {}\nvoid caller() { helper(); }";

#[test]
fn callhier_prepare_on_function() {
    let path = write_temp("cb_callhier_basic.cpp", BASIC_SRC);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // "caller" starts at line 2, col 6 ('c' of "caller").
    let item = callhier::call_hierarchy_prepare(&tu, 2, 6)
        .expect("expected Some(CallHierItem) on a function position");
    assert_eq!(
        item.name(),
        "caller",
        "expected name 'caller', got '{}'",
        item.name()
    );
}

#[test]
fn callhier_prepare_returns_none_on_non_function() {
    // A plain variable declaration — not a callable.
    let src = "int global_var = 42;\nvoid use_it() { (void)global_var; }";
    let path = write_temp("cb_callhier_nonfunction.cpp", src);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // col 5 on line 1 lands on "global_var" (a variable, not a function).
    let item = callhier::call_hierarchy_prepare(&tu, 1, 5);
    assert!(
        item.is_none(),
        "expected None for a variable position, got Some(name='{}')",
        item.as_ref().map(|i| i.name()).unwrap_or_default()
    );
}

#[test]
fn callhier_outgoing_calls() {
    let path = write_temp("cb_callhier_outgoing.cpp", BASIC_SRC);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // Prepare "caller" to obtain its USR.
    let item = callhier::call_hierarchy_prepare(&tu, 2, 6)
        .expect("expected CallHierItem for 'caller'");
    let usr = item.usr();
    assert!(!usr.is_empty(), "expected non-empty USR for 'caller'");

    let edges = callhier::outgoing_calls(&tu, &usr);
    let callee_names: Vec<String> = edges.iter().map(|e| e.name.clone()).collect();
    assert!(
        callee_names.iter().any(|n| n.contains("helper")),
        "expected 'helper' in outgoing calls from 'caller', got: {callee_names:?}"
    );
}

#[test]
fn callhier_incoming_calls() {
    let path = write_temp("cb_callhier_incoming.cpp", BASIC_SRC);
    let idx = Index::new();
    let tu = idx.parse(path.to_str().unwrap(), "", &["-std=c++17"]).unwrap();

    // Prepare "helper" (line 1, col 6) to obtain its USR.
    let item = callhier::call_hierarchy_prepare(&tu, 1, 6)
        .expect("expected CallHierItem for 'helper'");
    let usr = item.usr();
    assert!(!usr.is_empty(), "expected non-empty USR for 'helper'");

    let edges = callhier::incoming_calls(&tu, &usr);
    let caller_names: Vec<String> = edges.iter().map(|e| e.name.clone()).collect();
    assert!(
        caller_names.iter().any(|n| n.contains("caller")),
        "expected 'caller' in incoming calls to 'helper', got: {caller_names:?}"
    );

    // The call site should be on line 2 (where caller() calls helper()).
    let caller_edge = edges
        .iter()
        .find(|e| e.name.contains("caller"))
        .expect("expected an edge from 'caller'");
    assert!(
        caller_edge.call_line >= 1,
        "expected a valid (>=1) call_line, got {}",
        caller_edge.call_line
    );
    assert_eq!(
        caller_edge.call_line, 2,
        "expected call_line == 2 (the line where caller() invokes helper()), got {}",
        caller_edge.call_line
    );
}

#[test]
fn callhier_lambda_variable_maps_to_closure_call_operator() {
    let src = concat!(
        "int helper() { return 1; }\n",
        "int caller() {\n",
        "  auto fn = [base = 2](auto x) { return helper() + x + base; };\n",
        "  return fn(1);\n",
        "}\n",
    );
    let path = write_temp("cb_callhier_lambda.cpp", src);
    let idx = Index::new();
    let tu = idx
        .parse(path.to_str().unwrap(), "", &["-std=c++17"])
        .unwrap();

    let declaration = callhier::call_hierarchy_prepare(&tu, 3, 8)
        .expect("lambda hierarchy from variable declaration");
    let invocation =
        callhier::call_hierarchy_prepare(&tu, 4, 10).expect("lambda hierarchy from invocation");
    assert_eq!(declaration.name(), "fn");
    assert_eq!(invocation.name(), "fn");
    assert_eq!(declaration.usr(), invocation.usr());
    assert_eq!((declaration.line(), declaration.col()), (3, 8));

    let outgoing: Vec<_> = callhier::outgoing_calls(&tu, &declaration.usr())
        .iter()
        .collect();
    assert!(
        outgoing.iter().any(|edge| edge.name == "helper"),
        "lambda outgoing calls: {outgoing:?}"
    );

    let incoming: Vec<_> = callhier::incoming_calls(&tu, &declaration.usr())
        .iter()
        .collect();
    let caller = incoming
        .iter()
        .find(|edge| edge.name == "caller")
        .expect("caller invokes lambda");
    assert_eq!(caller.call_line, 4);
    assert_eq!(
        caller.call_col, 12,
        "operator() call anchors at the opening parenthesis"
    );
}
