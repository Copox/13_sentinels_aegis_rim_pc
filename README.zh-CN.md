> **本项目仅限将自己拥有的游戏在 PC 上使用，不支持任何盗版传播行为。请支持正版。**
>
> **This project is solely for running games you personally own on PC. It does not support piracy or the unauthorized distribution of games. Please support official releases.**

# 十三机兵防卫圈 PC 运行时

中文 · [English](README.md)

基于 [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) 的实验性 Windows x64 /
Linux x86-64 运行时，面向 CUSA18093，需要 Vulkan。
不包含游戏程序、固件、密钥、PRX 模块、游戏美术、音乐或存档。启动器使用原创项目素材，
仅接受资源清单支持的文件集。

## 编译

在 Debian 13 或相近 Linux 环境安装 Rust 1.85.1+，然后执行：

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

运行 `out/gpu/AegisLauncher`。在 Linux 上交叉编译 Windows 版本：

```bash
sudo apt install clang-19 lld-19 gcc-mingw-w64-x86-64-posix \
  g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 zstd curl xz-utils
rustup target add x86_64-pc-windows-gnu
PATH=/usr/lib/llvm-19/bin:$PATH bash scripts/aegis_build_windows.sh
```

下载官方 [Python 3.13.8 嵌入式 x64 压缩包](https://www.python.org/ftp/python/3.13.8/python-3.13.8-embed-amd64.zip)，然后打包：

```bash
python3 scripts/aegis_package_windows.py --prefix .deps/windows/mingw64 \
  --python-zip /path/to/python-3.13.8-embed-amd64.zip --out dist/aegis-windows
```

首次构建会下载依赖，目前不提供原生 MSVC 构建流程。

也可打开 **Actions → Build packages / 构建运行包 → Run workflow** 手动打包。
推送主分支或 `v*` 标签也会构建，在完成运行的 Artifacts 下载 `aegis-windows` 或
`aegis-linux`。Linux 包需要同版本或兼容发行版的 Python 3、SDL3、Vulkan、FFmpeg
运行库，不是 AppImage。

## 使用

Windows 打开 `AegisLauncher.exe`，Linux 运行 `out/gpu/AegisLauncher`。
选择自己拥有的、已解包的 CUSA18093 目录，也可放在启动器旁边。
“导入 PKG”仅接受支持的、已经可解密的自有游戏包，不解密加密零售包，也不合并更新。
文件校验通过后才能启动。

启动设置的“界面语言”和“游戏语言”默认跟随系统；界面只提供简中、繁中，游戏文本只提供游戏目录里实际存在的语言包（简中、繁中或韩语）。
选择分辨率、全屏、显示刷新、音量和输入映射，再开始游戏。
游戏逻辑固定 60Hz。
Enter/F 确认，Space/Backspace 返回，WASD 移动，方向键选择，Esc 打开菜单，
Alt+Enter 切换全屏，F10 开关状态面板。SDL3 手柄支持热插拔；已启用 Xbox／Switch Pro／Stadia 等常见驱动，未识别的摇杆会按标准位置 fallback。
手柄映射页可开关 PC 新增的“主角机兵受伤震动”（默认开启）；仅战斗中实际扣血触发，
敌人、召唤物和其他友方对象不会触发。设置与存档在 `user/` 中，请自行备份。

震动排查：在 Launcher 的手柄映射页点击“测试震动”，或在游戏内按 F8，
分别测试低频、高频马达。Launcher 设备与测试日志保存在 `logs/controller-*.log`，
游戏请求与 SDL 返回值在 `logs/<本次运行>/stdout.log`；每 5 秒记录一次计数。
日志会记录 SDL 版本、Windows DLL 路径、设备和输出能力；SDL 接受请求不代表马达实际动作。
反馈问题时请同时提供这两份日志，以及两处测试是否感到震动。

源码采用 GPL-2.0-or-later；许可证与来源见 [LICENSE](LICENSE) 和 [NOTICE.md](NOTICE.md)。
本项目与游戏权利方没有关联。
