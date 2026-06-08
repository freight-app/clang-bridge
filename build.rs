use std::process::Command;

fn main() {
    println!("cargo:rerun-if-env-changed=CLANG_BRIDGE_CLANGXX");
    println!("cargo:rerun-if-env-changed=CLANG_BRIDGE_LLVM_CONFIG");

    // Find the best available clang++ version (prefer 22, fall back to 21).
    let cxx = find_clangxx();

    // Ask llvm-config for include dirs and lib dirs.
    let llvm_config = find_llvm_config();
    let include_flags = llvm_config_flags(&llvm_config, "--cxxflags");
    let lib_flags = llvm_config_flags(&llvm_config, "--ldflags");
    let lib_dir = llvm_config_output(&llvm_config, "--libdir");

    cc::Build::new()
        .cpp(true)
        .compiler(&cxx)
        .file("bridge/cb_core.cpp")
        .file("bridge/cb_doc.cpp")
        .file("bridge/cb_diag.cpp")
        .file("bridge/cb_inlay.cpp")
        .file("bridge/cb_symbol.cpp")
        .file("bridge/cb_hover.cpp")
        .file("bridge/cb_goto.cpp")
        .file("bridge/cb_completion.cpp")
        .file("bridge/cb_analysis.cpp")
        .file("bridge/cb_refs.cpp")
        .file("bridge/cb_workspace.cpp")
        .file("bridge/cb_hierarchy.cpp")
        .file("bridge/cb_extra.cpp")
        .include("bridge")
        .flag("-std=c++17")
        .flag("-fno-rtti")
        // Pass through LLVM include dirs
        .flags(
            include_flags
                .iter()
                .filter(|f| f.starts_with("-I") || f.starts_with("-D"))
                .map(String::as_str),
        )
        .flag("-Wno-unused-parameter")
        .flag("-Wno-comment")
        .compile("clang_bridge");

    // Link against libclang-cpp (the full C++ API, not the restricted C libclang).
    for f in &lib_flags {
        if f.starts_with("-L") {
            println!("cargo:rustc-link-search=native={}", &f[2..]);
        }
    }
    if let Some(dir) = lib_dir.as_deref().filter(|s| !s.is_empty()) {
        println!("cargo:rustc-link-search=native={dir}");
    }
    println!("cargo:rustc-link-lib=dylib=clang-cpp");
    println!("cargo:rustc-link-lib=dylib=LLVM");
    println!("cargo:rustc-link-lib=dylib=stdc++");

    println!("cargo:rerun-if-changed=bridge/cb_internal.h");
    println!("cargo:rerun-if-changed=bridge/clang_bridge.h");
    println!("cargo:rerun-if-changed=bridge/cb_core.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_doc.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_diag.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_inlay.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_symbol.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_hover.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_goto.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_completion.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_analysis.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_refs.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_workspace.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_hierarchy.cpp");
    println!("cargo:rerun-if-changed=bridge/cb_extra.cpp");
}

fn find_clangxx() -> String {
    if let Ok(path) = std::env::var("CLANG_BRIDGE_CLANGXX") {
        if !path.is_empty() {
            return path;
        }
    }
    for candidate in ["clang++-22", "clang++-21", "clang++"] {
        if Command::new(candidate).arg("--version").output().is_ok() {
            return candidate.to_string();
        }
    }
    "clang++".to_string()
}

fn find_llvm_config() -> String {
    if let Ok(path) = std::env::var("CLANG_BRIDGE_LLVM_CONFIG") {
        if !path.is_empty() {
            return path;
        }
    }
    for candidate in ["llvm-config-22", "llvm-config-21", "llvm-config"] {
        if Command::new(candidate).arg("--version").output().is_ok() {
            return candidate.to_string();
        }
    }
    "llvm-config".to_string()
}

fn llvm_config_flags(bin: &str, arg: &str) -> Vec<String> {
    let Ok(out) = Command::new(bin).arg(arg).output() else {
        return vec![];
    };
    String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .map(str::to_owned)
        .collect()
}

fn llvm_config_output(bin: &str, arg: &str) -> Option<String> {
    let out = Command::new(bin).arg(arg).output().ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    Some(s)
}
