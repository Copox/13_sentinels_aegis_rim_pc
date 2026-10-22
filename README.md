> **本项目仅限将自己拥有的游戏在 PC 上使用，不支持任何盗版传播行为。请支持正版。**
>
> **This project is solely for running games you personally own on PC. It does not support piracy or the unauthorized distribution of games. Please support official releases.**

# 13 Sentinels: Aegis Rim PC

[中文](README.zh-CN.md) · English

An experimental Windows x64 / Linux x86-64 runtime for CUSA18093, based on
[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). Requires Vulkan.
No game executable, firmware, keys, PRX modules, game artwork, music or saves are included.
The launcher uses original project artwork. Only the supported file set is accepted.

## Build

On Debian 13 or a comparable Linux environment, install Rust 1.85.1+ and:

```bash
sudo apt install build-essential git cmake ninja-build pkg-config python3 python3-pil \
  libsdl3-dev libvulkan-dev libvulkan-memory-allocator-dev libfmt-dev libboost-dev \
  libzydis-dev robin-map-dev libxbyak-dev libxxhash-dev libx11-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  glslang-tools
git clone --recurse-submodules https://github.com/kxn/13_sentinels_aegis_rim_pc.git
cd 13_sentinels_aegis_rim_pc
bash scripts/aegis_build.sh
```

Launch with `out/gpu/AegisLauncher`. For Windows cross-compilation from Linux:

```bash
sudo apt install clang-19 lld-19 gcc-mingw-w64-x86-64-posix \
  g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 zstd curl xz-utils
rustup target add x86_64-pc-windows-gnu
PATH=/usr/lib/llvm-19/bin:$PATH bash scripts/aegis_build_windows.sh
```

Download the official [Python 3.13.8 embedded x64 archive](https://www.python.org/ftp/python/3.13.8/python-3.13.8-embed-amd64.zip), then package:

```bash
python3 scripts/aegis_package_windows.py --prefix .deps/windows/mingw64 \
  --python-zip /path/to/python-3.13.8-embed-amd64.zip --out dist/aegis-windows
```

Build dependencies download on first use. A native MSVC build is not provided.

Alternatively, open **Actions → Build packages → Run workflow**. Builds also run on main
pushes and `v*` tags. Download `aegis-windows` or `aegis-linux` from the completed run's
Artifacts. Linux packages require Python 3 and the SDL3/Vulkan/FFmpeg runtime libraries
from the same or a compatible distribution; they are not AppImages.

## Use

Open `AegisLauncher.exe` on Windows, or `out/gpu/AegisLauncher` on Linux.
Select your own extracted CUSA18093 folder; placing it beside the launcher also works.
The Import PKG tab accepts supported, already-decryptable packages you own; it cannot
decrypt retail packages or combine updates. File verification must pass before launch.

Choose resolution, fullscreen, display refresh, game text language (Simplified or Traditional
Chinese), volume and input mappings, then start.
Game logic stays at 60 Hz. Enter/F confirms; Space/Backspace cancels; WASD moves;
arrow keys select; Esc opens the menu; Alt+Enter toggles fullscreen; F10 toggles statistics.
SDL3 gamepads support hot-plugging. The gamepad tab also offers optional PC-added rumble
when a protagonist mech loses HP in battle (enabled by default); enemies, summons and
other friendly objects do not trigger it. Settings and saves are stored in `user/`; back it up.

To diagnose rumble, use **Test rumble** in the launcher's gamepad tab or press F8 in-game
to test the low- and high-frequency motors separately. Launcher diagnostics are in
`logs/controller-*.log`; guest requests and SDL results are in `logs/<run>/stdout.log`,
with counters every 5 seconds. Logs include the SDL version, Windows DLL path, device
and output capabilities. SDL accepting a request does not confirm physical vibration.
For reports, include both logs and whether you felt vibration in each test.

Source: GPL-2.0-or-later. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md) for attribution.
This project is not affiliated with the game's rights holders.
