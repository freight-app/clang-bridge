use std::process::Command;

fn main() {
    // Find the best available clang++ version (prefer 22, fall back to 21).
    let cxx = find_clangxx();

    // Ask llvm-config for include dirs and lib dirs.
    let llvm_config = find_llvm_config();
    let include_flags = llvm_config_flags(&llvm_config, "--cxxflags");
    let lib_flags = llvm_config_flags(&llvm_config, "--ldflags");

    cc::Build::new()
        .cpp(true)
        .compiler(&cxx)
        .file("bridge/clang_bridge.cpp")
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
    println!("cargo:rustc-link-lib=dylib=clang-cpp");
    println!("cargo:rustc-link-lib=dylib=LLVM");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    // clang-tidy is only available as a static lib on most distros
    println!("cargo:rustc-link-search=native=/usr/lib/llvm-22/lib");
    println!("cargo:rustc-link-search=native=/usr/lib");

    println!("cargo:rerun-if-changed=bridge/clang_bridge.cpp");
    println!("cargo:rerun-if-changed=bridge/clang_bridge.h");
}

fn find_clangxx() -> String {
    for candidate in ["clang++-22", "clang++-21", "clang++"] {
        if Command::new(candidate).arg("--version").output().is_ok() {
            return candidate.to_string();
        }
    }
    "clang++".to_string()
}

fn find_llvm_config() -> String {
    for candidate in ["llvm-config-22", "llvm-config-21", "llvm-config"] {
        if Command::new(candidate).arg("--version").output().is_ok() {
            return candidate.to_string();
        }
    }
    "llvm-config".to_string()
}

fn llvm_config_flags(bin: &str, arg: &str) -> Vec<String> {
    let Ok(out) = Command::new(bin).arg(arg).output() else { return vec![]; };
    String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .map(str::to_owned)
        .collect()
}
