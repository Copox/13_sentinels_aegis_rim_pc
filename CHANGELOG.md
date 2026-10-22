# 变更记录 / Changelog

## v1.1.0 — 2026-10-22

### 中文

**新增**

- 游戏文本语言可在 Launcher 选择简体中文、繁体中文或韩语，默认跟随系统。列表只包含游戏目录中实际存在的 `ROBO_XX.CPK` 与 `ROBO_PATCH2_XX.CPK`，不再显示没有语言包的选项。
- Launcher 界面支持简体中文和繁体中文，默认跟随系统；移除没有对应游戏语言包的 English 选项。旧的 `ui_language` / `language` 配置会自动迁移。
- 常见手柄兼容：显式启用 SDL HIDAPI / RawInput 的 Xbox、Switch Pro、Stadia 驱动；没有 SDL 映射的原始摇杆会按标准位置生成 fallback 映射，而不是直接忽略。
- 手柄晚连接修复：启动后再连接手柄可以立即使用；窗口暂时没有焦点时也会处理手柄事件。
- Launcher 手柄日志会列出所有 SDL 摇杆 / gamepad 的名称、GUID、VID/PID、路径和 `gamepad` 状态，方便定位设备识别问题。
- 主角机兵在战斗中实际扣血时可选短震动（默认开启）；不改变游戏原有震动，敌人、召唤物和其他友方对象不触发。
- 手柄震动诊断与 Launcher 设备交接：启动游戏时释放手柄，退出后恢复；日志记录 SDL 接受结果与实际输出。

**修复**

- 修复启动时没有手柄、之后插入不生效的问题。
- 修复旧语言设置迁移后默认不跟随系统的问题。
- 修复选择不存在的语言包时仍显示该语言的问题。

**已知限制**

- 游戏没有英文语言包，因此不提供英文游戏文本；English Launcher 界面已移除。
- Linux 的人物打光差异仍待处理；暂无原生 MSVC 构建流程。
- Switch Pro 的 USB 连接已由用户确认可用；Stadia 与蓝牙手柄仍在排查，需要设备日志继续定位。

### English

**Added**

- The game text language can be selected in the launcher: Simplified Chinese, Traditional Chinese or Korean, following the system by default. Only languages with both `ROBO_XX.CPK` and `ROBO_PATCH2_XX.CPK` in the game folder are listed.
- The launcher interface supports Simplified and Traditional Chinese and follows the system by default. The English option was removed because the game has no English language pack. Old `ui_language` / `language` settings migrate automatically.
- Common controller support: SDL HIDAPI / RawInput drivers are explicitly enabled for Xbox, Switch Pro and Stadia pads. Raw joysticks without an SDL mapping get a standard-position fallback mapping instead of being ignored.
- Gamepad hot-plug fix: a controller connected after launch works immediately, including while the window has not received focus yet.
- Launcher controller logs list every SDL joystick / gamepad with name, GUID, VID/PID, path and `gamepad` state to help diagnose device recognition.
- Optional short rumble when a protagonist mech actually loses HP in battle (enabled by default). Native rumble is unchanged; enemies, summons and other friendly objects do not trigger it.
- Rumble diagnostics and launcher device handoff: the launcher releases controllers when the game starts and restores them on exit; logs record SDL acceptance and output.

**Fixed**

- A controller connected after launch is now picked up instead of requiring a restart.
- Old fixed language settings now migrate to follow-system.
- Languages whose packs are missing are no longer offered.

**Known limitations**

- The game has no English language pack, so English game text is not provided, and the English launcher UI was removed.
- The Linux character-lighting difference is still open; no native MSVC build is provided.
- Switch Pro over USB is confirmed working by the user; Stadia and Bluetooth controllers are still being diagnosed and need device logs.

## v1.0.0 — 2026-10-21

首次公开发布。Windows x64 / Linux x86-64 运行时、图形 Launcher、PKG 导入与游戏文件校验。
First public release: Windows x64 / Linux x86-64 runtime, graphical launcher, PKG import and game-file verification.

[发布页 / Release](https://github.com/kxn/13_sentinels_aegis_rim_pc/releases/tag/v1.0.0)
