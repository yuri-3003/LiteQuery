// Build script for the LiteQuery Rust bindings.
//
// Compiles LiteQuery's C++ engine (the same sources CMake builds) into a static
// library with the `cc` crate and links it into the Rust binary. Only the
// public C API (litequery.h) is used from Rust, but the whole engine is compiled
// in so there is nothing to install separately — `cargo build` is self-contained.
//
// The engine sources live in the repository, two directories up from this
// crate. When packaged for crates.io they would be vendored under the crate;
// here we reference them by relative path for an in-repo build.

use std::path::{Path, PathBuf};

fn main() {
    // Repo root relative to bindings/rust/.
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let root = manifest.join("..").join("..");
    let src = root.join("src");
    let include = root.join("include");

    let sources = [
        "parser/lexer.cpp",
        "parser/parser.cpp",
        "planner/logical_plan.cpp",
        "planner/optimizer.cpp",
        "execution/eval.cpp",
        "execution/physical_plan.cpp",
        "execution/fast_aggregate.cpp",
        "storage/csv_reader.cpp",
        "api/connection.cpp",
        "api/c_api.cpp",
    ];

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++17")
        .warnings(false) // the engine is warning-clean under its own flags; keep cc quiet
        .include(&include)
        .include(include.join("litequery"))
        .include(src.join("parser"))
        .include(src.join("planner"))
        .include(src.join("catalog"))
        .include(src.join("storage"))
        .include(src.join("execution"))
        .include(src.join("api"));

    for s in sources {
        let p: PathBuf = src.join(s);
        rerun_if_changed(&p);
        build.file(p);
    }
    rerun_if_changed(&include.join("litequery").join("litequery.h"));

    build.compile("litequery_engine");

    // On the GNU/MinGW toolchain, the C++ standard library must be linked.
    // `cc` links libstdc++ automatically for C++ builds on most targets; make it
    // explicit for the windows-gnu and linux-gnu cases to be safe.
    let target = std::env::var("TARGET").unwrap_or_default();
    if target.contains("windows-gnu") || target.contains("linux") {
        println!("cargo:rustc-link-lib=stdc++");
    } else if target.contains("apple") {
        println!("cargo:rustc-link-lib=c++");
    }
}

fn rerun_if_changed(p: &Path) {
    println!("cargo:rerun-if-changed={}", p.display());
}
