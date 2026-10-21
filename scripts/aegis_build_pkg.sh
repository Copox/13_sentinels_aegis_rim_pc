#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
# Rust 1.85.1 or later; use rustup target add x86_64-pc-windows-gnu for cross builds.
args=(--locked --release --manifest-path tools/pkg-import/Cargo.toml --target-dir out/pkg-import)
if [[ ${1:-native} == windows ]]; then
    export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-x86_64-w64-mingw32-gcc-posix}
    cargo build "${args[@]}" --target x86_64-pc-windows-gnu
    mkdir -p out/windows-clang
    cp out/pkg-import/x86_64-pc-windows-gnu/release/aegis-pkg.exe out/windows-clang/AegisPkg.exe
else
    cargo build "${args[@]}"
fi
python3 scripts/aegis_pkg_licenses.py
