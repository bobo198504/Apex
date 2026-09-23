// ---------------------------------------------------------------------------
// THE LINKER NEEDS TWO IMPORT LIBRARIES THAT RUSTUP'S windows-gnu TOOLCHAIN DOES NOT SHIP.
//
// ⚠️ THIS IS PORTED FROM THE UPSTREAM PROJECT VERBATIM IN EFFECT, and it is here because the failure it fixes
// is confusing in a way that costs an hour: `cargo build` fails with `ld: cannot find -limm32`, which reads
// like a missing Windows SDK rather than like a toolchain packaging detail. rustup's windows-gnu target links
// against its own copy of MinGW's runtime, and that copy omits the import libraries for `imm32` (the IME API
// this feature is built on) and `shlwapi`.
//
// THEY COME FROM THE SHARED TOOLCHAIN, NOT FROM A DOWNLOAD. D:\Projects\Code\_tools\mingw64 has them -- the
// same portable MinGW Apex already builds with -- so nothing is installed and nothing is fetched. They are
// COPIED into the build output and that directory is added to the search path rather than the whole `lib`
// directory being added, because the whole directory also contains the C runtime, which then conflicts with
// rustup's own (upstream measured that; it is written in its build.rs).
//
// The lookup order is upstream's, and each step exists for a reason worth keeping:
//   1. MINGW64_ROOT, for a machine that keeps its toolchain somewhere else;
//   2. the shared _tools directory beside the containing project (the workspace convention -- see AGENTS.md);
//   3. a .tools directory inside the crate, for a clone that has neither.
// ---------------------------------------------------------------------------
use std::path::{Path, PathBuf};

fn valid_root(root: &Path) -> bool {
    root.join("bin").join("windres.exe").exists()
}

fn find_toolchain(manifest: &Path) -> Option<PathBuf> {
    if let Ok(root) = std::env::var("MINGW64_ROOT") {
        let candidate = PathBuf::from(root);
        if valid_root(&candidate) {
            return Some(candidate);
        }
    }
    // features/AutoIME -> features -> Apex -> Code -> _tools
    if let Some(parent) = manifest.parent().and_then(|p| p.parent()).and_then(|p| p.parent()) {
        let shared = parent.join("_tools").join("mingw64");
        if valid_root(&shared) {
            return Some(shared);
        }
    }
    let local = manifest.join(".tools").join("mingw64");
    if valid_root(&local) {
        return Some(local);
    }
    None
}

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    // NO windres STEP HERE. Upstream compiles its own .rc (the exe icon); an Apex feature has no icon -- the
    // tray and the window belong to the host -- so there is nothing to compile.

    if let Some(root) = find_toolchain(&manifest) {
        let link_dir = out_dir.join("link-libs");
        if std::fs::create_dir_all(&link_dir).is_ok() {
            let mut copied = 0;
            for name in ["libimm32.a", "libshlwapi.a"] {
                let src = root.join("lib").join(name);
                if src.exists() && std::fs::copy(&src, link_dir.join(name)).is_ok() {
                    copied += 1;
                }
            }
            if copied > 0 {
                println!("cargo:rustc-link-search=native={}", link_dir.display());
            }
        }
    }
}
