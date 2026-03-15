fn main() {
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let project_root = std::path::Path::new(&manifest_dir)
        .parent() // middleware/
        .unwrap()
        .parent() // rexplayer/
        .unwrap();

    cxx_build::bridge("src/lib.rs")
        .std("c++20")
        .include(project_root.join("src/vmm/include")) // For rex/ffi/callbacks.h
        .compile("rex_ffi");

    println!("cargo:rerun-if-changed=src/lib.rs");
}
