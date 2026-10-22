// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "i18n.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
namespace Launcher {
namespace fs = std::filesystem;
inline std::string Utf8(const fs::path &p) {
    auto s = p.u8string();
    return {s.begin(), s.end()};
}
inline std::string Trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
using Ini = std::map<std::string, std::string>;
inline Ini Read(const fs::path &p) {
    Ini m;
    std::ifstream f(p);
    std::string s;
    while (std::getline(f, s)) {
        auto e = s.find('=');
        if (e != s.npos && !s.empty() && s[0] != '#' && s[0] != ';')
            m[Trim(s.substr(0, e))] = Trim(s.substr(e + 1));
    }
    return m;
}
inline void Write(const fs::path &p, const Ini &m) {
    fs::create_directories(p.parent_path());
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        for (auto &[k, v] : m)
            f << k << '=' << v << '\n';
        f.flush();
        if (!f)
            throw std::runtime_error(I18n::Tr("无法保存设置：") + Utf8(p));
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(I18n::Tr("无法替换设置文件：") + Utf8(p));
#else
    fs::rename(tmp, p);
#endif
}
struct Action {
    const char *id;
    const char *title;
    const char *key;
    const char *pad;
};
inline constexpr std::array<Action, 25> Actions{
    {{"cross", "确认 / 互动", "Return,F", "south"},
     {"circle", "取消 / 返回", "Space,Backspace", "east"},
     {"square", "方块 □", "R", "west"},
     {"triangle", "三角 △", "V", "north"},
     {"l1", "L1", "Q", "leftshoulder"},
     {"r1", "R1", "E", "rightshoulder"},
     {"l2", "L2", "1", "lefttrigger"},
     {"r2", "R2", "3", "righttrigger"},
     {"l3", "L3", "Left Ctrl", "leftstick"},
     {"r3", "R3", "C", "rightstick"},
     {"options", "游戏菜单", "Escape", "start"},
     {"touchpad", "触摸板", "Tab", "back"},
     {"touchpad_right", "触摸板右侧", "", ""},
     {"up", "十字键 ↑", "Up", "dpup"},
     {"down", "十字键 ↓", "Down", "dpdown"},
     {"left", "十字键 ←", "Left", "dpleft"},
     {"right", "十字键 →", "Right", "dpright"},
     {"move_up", "移动 ↑", "W", ""},
     {"move_down", "移动 ↓", "S", ""},
     {"move_left", "移动 ←", "A", ""},
     {"move_right", "移动 →", "D", ""},
     {"look_up", "右摇杆 ↑", "I", ""},
     {"look_down", "右摇杆 ↓", "K", ""},
     {"look_left", "右摇杆 ←", "J", ""},
     {"look_right", "右摇杆 →", "L", ""}}};
inline std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
// The user's UI locale. LC_ALL=C must not hide a useful LANG (SSH and some shells set it).
inline std::string SystemLocale() {
    for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char *value = std::getenv(name);
        if (!value || !*value)
            continue;
        std::string s = LowerAscii(value);
        if (s == "c" || s == "posix" || s == "c.utf-8")
            continue;
        return s;
    }
    return {};
}
inline std::string SystemUiLanguage() {
#ifdef _WIN32
    switch (GetUserDefaultUILanguage()) {
    case 0x0404: // zh-TW
    case 0x0C04: // zh-HK
    case 0x1404: // zh-MO
        return "zh_TW";
    case 0x0804: // zh-CN
    case 0x1004: // zh-SG
        return "zh_CN";
    default:
        return "en";
    }
#else
    const std::string s = SystemLocale();
    if (s.rfind("zh_tw", 0) == 0 || s.rfind("zh_hk", 0) == 0 || s.rfind("zh_mo", 0) == 0)
        return "zh_TW";
    if (s.rfind("zh", 0) == 0)
        return "zh_CN";
    return "en";
#endif
}
// The game's text language follows the same locale: 9 Korean, 10 Traditional, 11 Simplified.
inline int SystemGameLanguage() {
#ifdef _WIN32
    switch (GetUserDefaultUILanguage()) {
    case 0x0404:
    case 0x0C04:
    case 0x1404:
        return 10;
    case 0x0412:
        return 9;
    default:
        return 11;
    }
#else
    const std::string s = SystemLocale();
    if (s.rfind("zh_tw", 0) == 0 || s.rfind("zh_hk", 0) == 0 || s.rfind("zh_mo", 0) == 0)
        return 10;
    if (s.rfind("ko", 0) == 0)
        return 9;
    return 11;
#endif
}
struct Settings {
    Ini runtime, launcher;
    fs::path root;
    std::string folder;
    int resolution = 0, refresh = 0, preset = 0;
    // "auto" follows the system; the game values are PS4 language codes 9/10/11.
    std::string ui_language = "auto", game_language = "auto";
    int custom_refresh = 180;
    bool fullscreen = false, vsync = true;
    bool damage_rumble = true;
    float gain = 1;
    // Keep old 4K settings on the compatibility path unless Pro is explicitly selected.
    static constexpr const char *Resolutions[] = {"1920x1080", "2560x1440", "3840x2160", "3840x2160"};
    static constexpr int Rates[] = {60, 120, 144, 165, 240, 0};
    fs::path GamePath() const {
        auto p = fs::u8path(folder);
        return (p.is_absolute() ? p : root / p).lexically_normal();
    }
    std::string UiLanguage() const {
        return ui_language == "auto" ? SystemUiLanguage() : ui_language;
    }
    int GameLanguage() const {
        if (game_language == "10")
            return 10;
        if (game_language == "9")
            return 9;
        if (game_language == "11")
            return 11;
        return SystemGameLanguage();
    }
    void NormalizeFolder() {
        if (folder.empty())
            return;
        auto p = GamePath();
#ifdef _WIN32
        auto drive = Utf8(p.root_name()), base = Utf8(root.root_name());
        auto lower = [](std::string &s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
        };
        lower(drive);
        lower(base);
        if (drive != base) {
            folder = Utf8(p);
            return;
        }
        // Filesystem paths compare drive letters case-sensitively; Windows does not.
        p = root.root_path() / p.relative_path();
#endif
        auto relative = p.lexically_relative(root);
        folder = Utf8(relative.empty() ? p : relative);
    }
    void Load(const fs::path &r) {
        root = fs::absolute(r).lexically_normal();
        runtime = Read(root / "user/aegis.ini");
        launcher = Read(root / "user/launcher.ini");
        folder = launcher.contains("game_folder") ? launcher["game_folder"] : "CUSA18093";
        NormalizeFolder();
        resolution = 0;
        for (int i : {0, 1, 3})
            if (runtime["render_resolution"] == Resolutions[i])
                resolution = i;
        if (runtime["render_mode"] == "pro") resolution = 2;
        refresh = 0;
        try {
            int rate = std::stoi(runtime.at("display_fps"));
            if (rate >= 30 && rate <= 1000) {
                custom_refresh = rate;
                refresh = 6;
            }
        } catch (...) {}
        for (int i = 0; i < 6; ++i)
            if (runtime["display_fps"] == std::to_string(Rates[i]))
                refresh = i;
        fullscreen = launcher["fullscreen"] == "1";
        vsync = runtime["display_vsync"] != "0";
        damage_rumble = runtime["damage_rumble"] != "0";
        try {
            preset = std::stoi(launcher.at("controller_preset"));
        } catch (...) {
            preset = 0;
        }
        if (preset < 0 || preset > 4)
            preset = 0;
        gain = 1;
        try {
            if (runtime.contains("audio_gain"))
                gain = std::stof(runtime["audio_gain"]);
        } catch (...) {
        }
        if (!(gain >= 0 && gain <= 4))
            gain = 1;
        if (!runtime.contains("audio_mix_version") && gain == 2)
            gain = 1; // Retire the old automatic 200% compensation once.
        ui_language = launcher.contains("ui_language") ? launcher["ui_language"] : "auto";
        if (ui_language != "zh_CN" && ui_language != "zh_TW" && ui_language != "en")
            ui_language = "auto";
        if (launcher.contains("game_language"))
            game_language = launcher["game_language"];
        else if (runtime.contains("language") && runtime["language"] != "11")
            game_language = runtime["language"]; // keep an old explicit TW/KO choice
        else
            game_language = "auto"; // 11 was the old fixed default; it now follows the system
        if (game_language != "11" && game_language != "10" && game_language != "9")
            game_language = "auto";
    }
    std::string Binding(int i, bool pad) const {
        std::string k = std::string(pad ? "pad." : "key.") + Actions[i].id;
        auto it = runtime.find(k);
        return it == runtime.end() ? (pad ? Actions[i].pad : Actions[i].key) : it->second;
    }
    void SetBinding(int i, bool pad, const std::string &value) {
        runtime[std::string(pad ? "pad." : "key.") + Actions[i].id] = value;
        if (pad)
            preset = 4;
    }
    void Preset(int p) {
        preset = p;
        for (int i = 0; i < 17; ++i)
            SetBinding(i, true, Actions[i].pad);
        if (p == 2 || p == 3) {
            SetBinding(0, true, "east");
            SetBinding(1, true, "south");
        }
        if (p == 3) {
            SetBinding(2, true, "north");
            SetBinding(3, true, "west");
        }
        preset = p;
    }
    bool ValidGame() const {
        if (folder.empty())
            return false;
        auto p = GamePath();
        std::error_code ec;
        const int language = GameLanguage();
        bool ok = fs::is_regular_file(p / "eboot.bin", ec) &&
                  fs::is_regular_file(p / "ROBO_CN.CPK", ec) &&
                  fs::is_regular_file(p / "ROBO_PATCH2_CN.CPK", ec);
        if (language == 10)
            ok = ok && fs::is_regular_file(p / "ROBO_TW.CPK", ec) &&
                 fs::is_regular_file(p / "ROBO_PATCH2_TW.CPK", ec);
        else if (language == 9)
            ok = ok && fs::is_regular_file(p / "ROBO_KO.CPK", ec) &&
                 fs::is_regular_file(p / "ROBO_PATCH2_KO.CPK", ec);
        return ok;
    }
    void Save() {
        NormalizeFolder();
        runtime["render_resolution"] = Resolutions[resolution];
        runtime["render_mode"] = resolution == 2 ? "pro" : "base";
        runtime["audio_gain"] = std::to_string(gain);
        runtime["audio_mix_version"] = "1";
        runtime["language"] = std::to_string(GameLanguage());
        runtime["damage_rumble"] = damage_rumble ? "1" : "0";
        custom_refresh = std::clamp(custom_refresh, 30, 1000);
        runtime["display_fps"] = std::to_string(refresh == 6 ? custom_refresh : Rates[refresh]);
        runtime["display_vsync"] = vsync ? "1" : "0";
        launcher["game_folder"] = folder;
        launcher["fullscreen"] = fullscreen ? "1" : "0";
        launcher["controller_preset"] = std::to_string(preset);
        launcher["ui_language"] = ui_language;
        launcher["game_language"] = game_language;
        Write(root / "user/aegis.ini", runtime);
        Write(root / "user/launcher.ini", launcher);
    }
};
} // namespace Launcher
