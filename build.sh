#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
CC=${CC:-cc}
pkg-config --exists vulkan sdl3
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
if [[ -e .git ]]; then git submodule update --init --recursive; fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
python3 scripts/aegis_assets.py
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DBB_PGO=off -DBB_LTO=OFF
cmake --build out/gpu --target bbgpu aegis-launcher -j "${AEGIS_BUILD_JOBS:-8}"
mkdir -p out/atrac9
for source in third_party/LibAtrac9/C/src/*.c; do
    "$CC" -std=c99 -O2 -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"
done
ar rcs out/libatrac9.a out/atrac9/*.o
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c out/libatrac9.a -lm \
    -Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic \
    "${libraries[@]}" -o out/aegis
printf 'Built %s/out/aegis and out/gpu/AegisLauncher\n' "$PWD"
