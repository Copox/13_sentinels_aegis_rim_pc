#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
deps=${AEGIS_WINDOWS_DEPS:-$PWD/.deps/windows}
mkdir -p "$deps"
deps=$(realpath "$deps")
export AEGIS_WIN_SYSROOT="$deps/mingw64"
python3 scripts/aegis_windows_deps.py --root "$deps"
if [[ -e .git ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
# Header-only dependencies already supplied by the Linux build prerequisites.
cp /usr/include/vk_mem_alloc.h "$AEGIS_WIN_SYSROOT/include/"
cp -R /usr/include/xbyak "$AEGIS_WIN_SYSROOT/include/"
zydis_commit=a2278f1d254e492f6a6b39f6cb5d1f5d515659dc
if [[ ! -d $deps/zydis/.git ]]; then
    git clone https://github.com/zyantific/zydis.git "$deps/zydis"
fi
git -C "$deps/zydis" checkout --detach "$zydis_commit"
git -C "$deps/zydis" submodule update --init --recursive
toolchain=$PWD/cmake/windows-mingw.cmake
cmake -S "$deps/zydis/dependencies/zycore" -B "$deps/zycore-clang" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_INSTALL_PREFIX="$AEGIS_WIN_SYSROOT" \
    -DZYCORE_BUILD_TESTS=OFF
cmake --build "$deps/zycore-clang" -j "${AEGIS_BUILD_JOBS:-12}"
cmake --install "$deps/zycore-clang"
cmake -S "$deps/zydis" -B "$deps/zydis-clang" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_INSTALL_PREFIX="$AEGIS_WIN_SYSROOT" \
    -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF \
    -DZYDIS_BUILD_SHARED_LIB=OFF
cmake --build "$deps/zydis-clang" -j "${AEGIS_BUILD_JOBS:-12}"
cmake --install "$deps/zydis-clang"
archive=$deps/ffmpeg-7.1.2.tar.xz
if [[ ! -f $archive ]]; then
    curl --fail --location https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz -o "$archive"
fi
echo "089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304  $archive" | sha256sum -c -
if [[ ! -d $deps/ffmpeg-7.1.2 ]]; then
    tar -xJf "$archive" -C "$deps"
fi
if [[ ! -f $AEGIS_WIN_SYSROOT/bin/avcodec-61.dll ]]; then
    (
        cd "$deps/ffmpeg-7.1.2"
        ./configure --prefix="$AEGIS_WIN_SYSROOT" --target-os=mingw32 --arch=x86_64 \
            --cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-gcc-posix \
            --cxx=x86_64-w64-mingw32-g++-posix --enable-cross-compile --disable-programs \
            --disable-doc --disable-debug --disable-static --enable-shared --disable-everything \
            --enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
            --enable-decoder=h264,hevc,aac,pcm_s16le --enable-demuxer=mov,matroska,wav \
            --enable-parser=h264,hevc,aac --enable-protocol=file --disable-x86asm
        make -j "${AEGIS_BUILD_JOBS:-12}"
        make install
    )
fi
python3 scripts/aegis_assets.py
icon_options=(-DAEGIS_GAME_ICON="$PWD/out/aegis.ico")
cmake -S gpu -B out/windows-clang -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release -DBB_PGO=off -DBB_LTO=OFF \
    "${icon_options[@]}" \
    -DVulkan_INCLUDE_DIR="$AEGIS_WIN_SYSROOT/include" \
    -DVulkan_LIBRARY="$AEGIS_WIN_SYSROOT/lib/libvulkan-1.dll.a"
cmake --build out/windows-clang --target bb-probe aegis-launcher aegis-launcher-bootstrap -j "${AEGIS_BUILD_JOBS:-12}"

bash scripts/aegis_build_pkg.sh windows
