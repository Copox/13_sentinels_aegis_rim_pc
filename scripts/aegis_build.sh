#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
# Debian 13's Vulkan headers predate enums used by the pinned renderer.
headers_commit=acbb9391c0a597272800cd3f4363661c00a27c77
headers_dir=${AEGIS_VULKAN_HEADERS:-$PWD/.deps/Vulkan-Headers}
if [[ ! -f $headers_dir/include/vulkan/vulkan.hpp ]]; then
    git clone https://github.com/KhronosGroup/Vulkan-Headers.git "$headers_dir"
    git -C "$headers_dir" checkout --detach "$headers_commit"
fi
if [[ -e .git ]]; then
    git submodule update --init --recursive
fi
python3 scripts/aegis_assets.py
cmake -S gpu -B out/gpu -G Ninja -DBB_PGO=off -DBB_LTO=OFF \
    -DVulkan_INCLUDE_DIR="$headers_dir/include"
BB_PGO=off BB_LTO=OFF bash build.sh "$@"

# Startup and PKG import share the content-verification helper.
bash scripts/aegis_build_pkg.sh native
