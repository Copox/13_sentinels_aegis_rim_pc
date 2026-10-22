// SPDX-License-Identifier: GPL-2.0-or-later
#define SDL_MAIN_HANDLED
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "settings.h"
#include "i18n.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "../src/pad_diagnostics.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
using namespace Launcher;
namespace {
Settings settings;
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *background;
int tab = 0, capture = -1;
bool capture_pad = false, dirty = false, quit = false, capture_ui = false, auto_launch = false;
std::string status, screenshot;
SDL_Scancode pending_modifier = SDL_SCANCODE_UNKNOWN;
std::mutex dialog_mutex;
std::string chosen_folder;
bool dialog_done = false;
int dialog_kind = 0;
bool importing = false, auto_import = false, launching = false;
std::string launch_error;
fs::path launch_output;
std::string pkg_file, import_folder = "CUSA18093", import_message;
fs::path import_destination, import_stage, import_progress;
std::vector<SDL_Gamepad *> controllers;
std::string rumble_message;
FILE *controller_log;
fs::path controller_log_path;
bool controllers_suspended = false;
int rumble_test_phase = 0;
bool rumble_test_failed = false;
Uint64 rumble_test_deadline = 0;
SDL_JoystickID rumble_test_id = 0;
void Controllers();
void SuspendControllers();
void ResumeControllers();
fs::path last_log;
#ifdef _WIN32
HANDLE child = nullptr;
std::wstring Wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.pop_back();
    return w;
}
std::wstring Quote(const std::wstring &s) {
    std::wstring r = L"\"";
    size_t slash = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++slash;
            continue;
        }
        r.append(slash * (c == L'"' ? 2 : 1), L'\\');
        slash = 0;
        if (c == L'"')
            r += L'\\';
        r += c;
    }
    r.append(slash * 2, L'\\');
    return r + L'"';
}
#else
pid_t child = 0;
#endif
void Error(const std::string &text) {
    status = text;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Aegis", text.c_str(), window);
}
bool Save() {
    try {
        settings.Save();
        dirty = false;
        status = I18n::Tr("设置已保存");
        return true;
    } catch (const std::exception &e) {
        Error(e.what());
        return false;
    }
}
bool Spawn(const fs::path &exe, const std::vector<std::string> &args) {
#ifdef _WIN32
    if (!fs::exists(exe)) {
        Error(I18n::Tr("启动组件缺失：") + Utf8(exe));
        return false;
    }
    std::wstring command = Quote(exe.wstring());
    for (auto &a : args)
        command += L" " + Quote(Wide(a));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(last_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = input;
    PROCESS_INFORMATION pi{};
    BOOL ok = log != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE &&
              CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, settings.root.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE)
        CloseHandle(input);
    if (!ok) {
        Error(I18n::Tr("无法启动组件，请检查文件和目录权限。"));
        return false;
    }
    CloseHandle(pi.hThread);
    child = pi.hProcess;
#else
    std::string program = Utf8(exe);
    std::vector<char *> argv;
    argv.push_back(program.data());
    for (auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, last_log.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    int result = posix_spawnp(&child, program.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (result) {
        child = 0;
        Error(I18n::Tr("无法启动运行组件。"));
        return false;
    }
#endif
    return true;
}
bool Running() {
    if (child && launching && fs::exists(launch_output / "pid")) {
        launching = false;
        SDL_HideWindow(window);
    }
#ifdef _WIN32
    if (!child)
        return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(child, &code) || code == STILL_ACTIVE)
        return true;
    CloseHandle(child);
    child = nullptr;
#else
    if (!child)
        return false;
    int wait_status = 0;
    if (waitpid(child, &wait_status, WNOHANG) == 0)
        return true;
    child = 0;
    int code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 1;
#endif
    if (importing) {
        importing = false;
        auto state = Read(import_progress);
        if (code == 0 && state["phase"] == "complete") {
            settings.folder = Utf8(import_destination);
            settings.NormalizeFolder();
            dirty = true;
            Save();
            tab = 0;
            status = I18n::Tr("PKG 导入完成 · 可以开始游戏");
            import_message = status;
        } else {
            import_message = state.contains("error") ? state["error"]
                                                     : I18n::Tr("导入失败，请查看日志：") + Utf8(last_log);
            status = I18n::Tr("PKG 导入失败");
        }
    } else {
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
        if (launching) {
            auto check = Read(launch_output / "image/verification.ini");
            if (check["phase"] == "error") {
                launch_error = check["error"];
                status = I18n::Tr("游戏文件校验失败 · 已停止启动");
            } else {
                launch_error = I18n::Tr("启动准备未完成，请查看日志：") + Utf8(last_log);
                status = I18n::Tr("游戏未能启动 · 请查看 logs");
            }
            launching = false;
        } else status = code == 0 ? I18n::Tr("游戏已退出 · 日志已保存") : I18n::Tr("游戏未正常启动或退出 · 请查看 logs");
    }
    ResumeControllers();
    return false;
}
void Start() {
    if (child)
        return;
    if (!fs::is_directory(settings.GamePath())) {
        Error(I18n::Tr("请选择存在的游戏目录。"));
        return;
    }
    if (!Save())
        return;
    const auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char stamp[48];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
    std::string name = std::string(stamp) + "-" + std::to_string(SDL_GetTicks());
    auto out = settings.root / "logs" / name;
    fs::create_directories(settings.root / "logs");
    last_log = settings.root / "logs" / ("launcher-" + name + ".log");
    std::vector<std::string> args = {"-X",
                                     "utf8",
                                     Utf8(settings.root / "scripts/aegis_run.py"),
                                     "--game",
                                     Utf8(settings.GamePath()),
                                     "--out",
                                     Utf8(out),
                                     "--user",
                                     Utf8(settings.root / "user"),
                                     settings.fullscreen ? "--fullscreen" : "--windowed",
                                     "--timeout",
                                     "86400"};
#ifdef _WIN32
    auto exe = settings.root / "python/pythonw.exe";
#else
    fs::path exe = "python3";
#endif
    // Only the game process should own controller output while it is running.
    SuspendControllers();
    if (Spawn(exe, args)) {
        launch_output = out;
        launching = true;
        launch_error.clear();
        status = I18n::Tr("正在检查游戏文件…");
    } else ResumeControllers();
}
void PickPath(int kind) {
    dialog_kind = kind;
    auto callback = [](void *, const char *const *paths, int) {
        std::lock_guard lock(dialog_mutex);
        if (paths && paths[0])
            chosen_folder = paths[0];
        dialog_done = true;
    };
    if (kind == 1) {
        static const SDL_DialogFileFilter filter[] = {{"PlayStation 4 package", "pkg"}};
        SDL_ShowOpenFileDialog(callback, nullptr, window, filter, 1, nullptr, false);
    } else {
        auto initial = Utf8(kind == 0 ? settings.GamePath() : settings.root);
        SDL_ShowOpenFolderDialog(callback, nullptr, window, initial.c_str(), false);
    }
}
void StartImport() {
    if (child)
        return;
    if (pkg_file.empty() || import_folder.empty()) {
        import_message = I18n::Tr("请选择 PKG 文件和解包目录。");
        return;
    }
    auto p = fs::u8path(import_folder);
    import_destination = (p.is_absolute() ? p : settings.root / p).lexically_normal();
    if (fs::exists(import_destination)) {
        import_message = I18n::Tr("目标目录已存在，请选择新的目录；不会覆盖已有游戏。");
        return;
    }
    auto id = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    import_stage = import_destination.parent_path() / (".aegis-import-" + id);
    fs::create_directories(settings.root / "logs");
    import_progress = settings.root / "logs" / ("import-" + id + ".ini");
    last_log = settings.root / "logs" / ("import-" + id + ".log");
#ifdef _WIN32
    auto exe = settings.root / "out/AegisPkg.exe";
#else
    auto exe = settings.root / "out/pkg-import/release/aegis-pkg";
#endif
    std::vector<std::string> args = {
        Utf8(fs::absolute(fs::u8path(pkg_file))), Utf8(import_destination), Utf8(import_stage),
        Utf8(import_progress), Utf8(settings.root / "profiles/CUSA18093.json"),
        Utf8(settings.root / "user/verification-cache")};
    importing = Spawn(exe, args);
    import_message = importing ? I18n::Tr("正在读取 PKG…") : I18n::Tr("无法启动解包组件");
}
void CancelImport() {
    if (!importing || !child)
        return;
#ifdef _WIN32
    TerminateProcess(child, 1);
    WaitForSingleObject(child, INFINITE);
    CloseHandle(child);
    child = nullptr;
#else
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    child = 0;
#endif
    importing = false;
    std::error_code ec;
    fs::remove_all(import_stage, ec);
    import_message = ec ? I18n::Tr("已停止导入，临时目录未能删除：") + Utf8(import_stage)
                        : I18n::Tr("已取消导入，原有游戏保持不变。");
}
void ImportPage() {
    ImGui::TextUnformatted(I18n::Tr("从 PKG 导入游戏"));
    ImGui::TextDisabled(
        I18n::Tr("选择 CUSA18093 完整本体包，解包后自动选中游戏目录。更新包、DLC 或无法解密的包不支持。"));
    ImGui::Spacing();
    ImGui::BeginDisabled(importing);
    char file[4096];
    SDL_strlcpy(file, pkg_file.c_str(), sizeof(file));
    ImGui::TextUnformatted(I18n::Tr("PKG 文件"));
    ImGui::SetNextItemWidth(-110);
    if (ImGui::InputText("##pkg", file, sizeof(file)))
        pkg_file = file;
    ImGui::SameLine();
    if (ImGui::Button(I18n::Tr("选择…"), {-1, 0}))
        PickPath(1);
    char folder[4096];
    SDL_strlcpy(folder, import_folder.c_str(), sizeof(folder));
    ImGui::TextUnformatted(I18n::Tr("解包到新目录"));
    ImGui::SetNextItemWidth(-110);
    if (ImGui::InputText("##destination", folder, sizeof(folder)))
        import_folder = folder;
    ImGui::SameLine();
    if (ImGui::Button(I18n::Tr("位置…"), {-1, 0}))
        PickPath(2);
    ImGui::TextDisabled(I18n::Tr("默认放在 Launcher 旁边的 CUSA18093。请预留解包空间；已有目录不会被覆盖。"));
    if (ImGui::Button(I18n::Tr("开始导入 PKG")))
        StartImport();
    ImGui::EndDisabled();
    if (importing) {
        auto state = Read(import_progress);
        float done = std::strtof(state["done"].c_str(), nullptr),
              total = std::strtof(state["total"].c_str(), nullptr);
        const auto phase = state["phase"];
        const char *label = phase == "verify"    ? I18n::Tr("校验游戏文件…")
                            : phase == "extract" ? I18n::Tr("正在解包…")
                                                 : I18n::Tr("读取包与文件系统…");
        ImGui::TextUnformatted(label);
        double bytes = std::strtod(state["bytes"].c_str(), nullptr),
               all_bytes = std::strtod(state["total_bytes"].c_str(), nullptr);
        ImGui::ProgressBar(phase == "verify" && all_bytes > 0 ? float(bytes / all_bytes) : total > 0 ? done / total : 0, {-1, 0});
        ImGui::Text(I18n::Tr("已完成 %.0f / %.0f 个资源文件"), done, total);
        ImGui::TextDisabled("%s", state["file"].c_str());
        if (phase == "verify") ImGui::TextDisabled(I18n::Tr("已校验 %.2f / %.2f GB"), bytes / 1e9, all_bytes / 1e9);
        if (ImGui::Button(I18n::Tr("取消导入")))
            CancelImport();
    } else if (!import_message.empty())
        ImGui::TextWrapped("%s", import_message.c_str());
}
void Controllers() {
    if (controllers_suspended) return;
    if (rumble_test_phase)
        pad_log(controller_log,"launcher rumble test cancelled by device change");
    rumble_test_phase = 0;
    for (auto *p : controllers)
        SDL_CloseGamepad(p);
    controllers.clear();
    rumble_message.clear();
    pad_log_joysticks(controller_log,"launcher-devices");
    pad_map_unknown_joysticks(controller_log);
    int n = 0;
    auto *ids = SDL_GetGamepads(&n);
    pad_log(controller_log,"launcher enumerate count=%d error=%s",n,ids ? "" : SDL_GetError());
    for (int i = 0; ids && i < n; ++i)
        if (auto *p = SDL_OpenGamepad(ids[i])) {
            controllers.push_back(p);
            pad_log_device(controller_log,p,"launcher-open");
        } else pad_log(controller_log,"launcher open id=%u failed: %s",(unsigned)ids[i],SDL_GetError());
    SDL_free(ids);
}
void SuspendControllers() {
    if (controllers_suspended) return;
    pad_log(controller_log,"handoff: releasing all launcher gamepads and SDL gamepad subsystem before starting game process");
    for (auto *p : controllers) {
        if (rumble_test_phase && SDL_GetGamepadID(p) == rumble_test_id)
            pad_log_rumble(controller_log,p,"launcher-test-cancel",0,0,0);
        SDL_CloseGamepad(p);
    }
    controllers.clear();
    rumble_test_phase = 0;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    controllers_suspended = true;
}
void ResumeControllers() {
    if (!controllers_suspended) return;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        pad_log(controller_log,"resume gamepads failed: %s",SDL_GetError());
        return;
    }
    controllers_suspended = false;
    pad_log(controller_log,"game process exited; launcher resumes gamepad ownership");
    Controllers();
}
void TickRumbleTest() {
    if (!rumble_test_phase || SDL_GetTicks() < rumble_test_deadline) return;
    auto *p = SDL_GetGamepadFromID(rumble_test_id);
    if (!p) {
        pad_log(controller_log,"launcher test device id=%u disconnected",(unsigned)rumble_test_id);
        rumble_test_phase = 0;
        rumble_message = I18n::Tr("测试中断：手柄已断开");
    } else if (rumble_test_phase == 1) {
        const bool ok = pad_log_rumble(controller_log,p,"launcher-high-motor",0,0xffff,1000);
        rumble_test_failed |= !ok;
        rumble_test_phase = 2;
        rumble_test_deadline = SDL_GetTicks() + 1100;
        rumble_message = ok ? I18n::Tr("高频马达测试中…") : std::string(I18n::Tr("高频震动失败：")) + SDL_GetError();
    } else {
        pad_log_rumble(controller_log,p,"launcher-test-stop",0,0,0);
        rumble_test_phase = 0;
        rumble_message = rumble_test_failed ? I18n::Tr("测试结束 · 至少一个震动请求失败，请查看日志")
            : I18n::Tr("SDL 已接受两次请求 · 请确认实际震动情况");
    }
}
std::string KeyLabel(std::string s) {
    if (s.empty())
        return I18n::Tr("未绑定");
    std::stringstream in(s);
    std::string item, result;
    while (std::getline(in, item, ',')) {
        if (auto at = item.find("scancode:"); at != item.npos) {
            int c = std::atoi(item.c_str() + at + 9);
            item = item.substr(0, at) + SDL_GetScancodeName(SDL_Scancode(c));
        }
        if (!result.empty())
            result += " / ";
        result += item;
    }
    return result;
}
std::string PadLabel(const std::string &s) {
    if (s == "a")
        return "A / ×";
    if (s == "b")
        return "B / ○";
    if (s == "x")
        return "X / □";
    if (s == "y")
        return "Y / △";
    static const std::map<std::string, std::string> names = {
        {"south", "A / ×"},         {"east", "B / ○"},           {"west", "X / □"},
        {"north", "Y / △"},         {"leftshoulder", "LB / L1"}, {"rightshoulder", "RB / R1"},
        {"lefttrigger", "LT / L2"}, {"righttrigger", "RT / R2"}, {"leftstick", "LS / L3"},
        {"rightstick", "RS / R3"},  {"back", "View / 触摸板"},   {"start", "Menu / Options"},
        {"dpup", "十字键 ↑"},       {"dpdown", "十字键 ↓"},      {"dpleft", "十字键 ←"},
        {"dpright", "十字键 →"}};
    auto it = names.find(s);
    return s.empty() ? std::string(I18n::Tr("未绑定"))
                     : it == names.end() ? s : std::string(I18n::Tr(it->second.c_str()));
}
void Mappings(bool pad) {
    if (pad) {
        const char *presets[] = {I18n::Tr("Xbox 标准位置"), I18n::Tr("PlayStation 标准位置"), I18n::Tr("Xbox 确认 / 返回互换"),
                                 I18n::Tr("Nintendo 字母习惯"), I18n::Tr("自定义映射")};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##preset", &settings.preset, presets, 5)) {
            if (settings.preset < 4)
                settings.Preset(settings.preset);
            dirty = true;
        }
        ImGui::TextDisabled(I18n::Tr("连接后自动识别；摇杆按位置对应，下面可改按钮与扳机。"));
        if (controllers.empty())
            ImGui::TextColored({.85f, .7f, .44f, 1}, I18n::Tr("当前未连接手柄 · 键盘仍可使用"));
        else {
            ImGui::Text(I18n::Tr("已连接：%s"), SDL_GetGamepadName(controllers.front()));
            ImGui::BeginDisabled(rumble_test_phase != 0);
            if (ImGui::Button(I18n::Tr("测试震动 · 两个马达"))) {
                auto *p = controllers.front();
                pad_log_device(controller_log,p,"launcher-test-device");
                pad_log_rumble(controller_log,p,"launcher-test-reset",0,0,0);
                const bool ok = pad_log_rumble(controller_log,p,"launcher-low-motor",0xffff,0,1000);
                rumble_test_failed = !ok;
                rumble_test_id = SDL_GetGamepadID(p);
                rumble_test_phase = 1;
                rumble_test_deadline = SDL_GetTicks() + 1100;
                rumble_message = ok ? I18n::Tr("低频马达测试中…") : std::string(I18n::Tr("低频震动失败：")) + SDL_GetError();
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped("%s", rumble_message.c_str());
        }
        ImGui::TextDisabled(I18n::Tr("游戏内按 F8 独立测试震动；诊断记录在 logs/controller-*.log"));
        if (ImGui::Checkbox(I18n::Tr("主角机兵受伤震动（PC 新增）"), &settings.damage_rumble)) dirty = true;
        if (ImGui::Button(I18n::Tr("打开手柄日志目录"))) {
            const std::string url = "file://" + Utf8(settings.root / "logs");
#ifdef _WIN32
            ShellExecuteW(nullptr,L"open",(settings.root / "logs").c_str(),nullptr,nullptr,SW_SHOWNORMAL);
#else
            SDL_OpenURL(url.c_str());
#endif
        }
    } else {
        if (ImGui::Button(I18n::Tr("恢复默认键盘"))) {
            for (int i = 0; i < 25; ++i)
                settings.SetBinding(i, false, Actions[i].key);
            dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(I18n::Tr("点击按键后输入；支持 Ctrl / Shift 组合键。"));
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -62))) {
        ImGui::TableSetupColumn(I18n::Tr("游戏操作"), ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn(pad ? I18n::Tr("手柄按钮") : I18n::Tr("键盘按键"), ImGuiTableColumnFlags_WidthStretch,
                                1.3f);
        ImGui::TableSetupColumn("##clear", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (pad ? 17 : 25); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(I18n::Tr(Actions[i].title));
            ImGui::TableNextColumn();
            std::string text =
                pad ? PadLabel(settings.Binding(i, true)) : KeyLabel(settings.Binding(i, false));
            if (ImGui::Button((text + "##bind").c_str(), {-1, 0})) {
                capture = i;
                capture_pad = pad;
                ImGui::OpenPopup(I18n::Tr("等待输入"));
            }
            if (ImGui::BeginPopupModal(I18n::Tr("等待输入"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s: %s", pad ? I18n::Tr("手柄映射") : I18n::Tr("键盘映射"),
                            I18n::Tr(Actions[i].title));
                ImGui::TextUnformatted(pad ? I18n::Tr("按下手柄上的按钮或扳机") : I18n::Tr("按下新按键，Esc 取消"));
                ImGui::TextDisabled(I18n::Tr("当前绑定会被替换，其他操作不变。"));
                if (capture < 0 || ImGui::Button(I18n::Tr("取消"))) {
                    capture = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(I18n::Tr("清除"))) {
                settings.SetBinding(i, pad, "");
                dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void Theme(float scale) {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowRounding = 12;
    s.FrameRounding = 7;
    s.PopupRounding = 10;
    s.ScrollbarRounding = 9;
    s.WindowPadding = {26, 20};
    s.FramePadding = {13, 7};
    s.ItemSpacing = {14, 10};
    s.Colors[ImGuiCol_Text] = {.95f, .93f, .87f, 1};
    s.Colors[ImGuiCol_TextDisabled] = {.66f, .73f, .76f, 1};
    s.Colors[ImGuiCol_WindowBg] = {.045f, .075f, .10f, .94f};
    s.Colors[ImGuiCol_ChildBg] = {.045f, .075f, .10f, .94f};
    s.Colors[ImGuiCol_FrameBg] = {.12f, .19f, .23f, .98f};
    s.Colors[ImGuiCol_FrameBgHovered] = {.18f, .29f, .34f, 1};
    s.Colors[ImGuiCol_FrameBgActive] = {.22f, .35f, .40f, 1};
    s.Colors[ImGuiCol_Button] = {.12f, .25f, .31f, .95f};
    s.Colors[ImGuiCol_ButtonHovered] = {.20f, .39f, .46f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {.24f, .45f, .52f, 1};
    s.Colors[ImGuiCol_CheckMark] = {1, .66f, .28f, 1};
    s.Colors[ImGuiCol_SliderGrab] = {.9f, .56f, .22f, 1};
    s.ScaleAllSizes(scale);
}
void Draw() {
    int pw, ph;
    SDL_GetRenderOutputSize(renderer, &pw, &ph);
    SDL_SetRenderDrawColor(renderer, 10, 16, 22, 255);
    SDL_RenderClear(renderer);
    if (background) {
        float tw, th;
        SDL_GetTextureSize(background, &tw, &th);
        float factor = std::max(pw / tw, ph / th);
        SDL_FRect dst{(pw - tw * factor) / 2, (ph - th * factor) / 2, tw * factor, th * factor};
        SDL_RenderTexture(renderer, background, nullptr, &dst);
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    auto &io = ImGui::GetIO();
    // A game folder change or a missing pack can change the resolved language between frames.
    const auto ui_language = I18n::FromCode(settings.UiLanguage());
    if (ui_language != I18n::Language())
        I18n::SetLanguage(ui_language);
    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    auto *draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilledMultiColor({0, h * .25f}, {w, h}, 0x0010151c, 0x0010151c, 0xd9141b24,
                                  0xe6141b24);
    ImGui::SetNextWindowPos({w * .04f, h * .035f});
    ImGui::SetNextWindowBgAlpha(.84f);
    ImGui::Begin("edition", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    ImGui::TextColored({1, .72f, .38f, 1}, "AEGIS   /   PC EDITION");
    ImGui::End();
    ImGui::SetNextWindowPos({w * .04f, h * .115f});
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("identity", nullptr, ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoInputs);
    ImGui::SetWindowFontScale(2.4f);
    ImGui::TextColored({.93f, .96f, 1.f, 1}, "AEGIS");
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextColored({.63f, .76f, .82f, 1}, "13 SENTINELS: AEGIS RIM  /  PC RUNTIME");
    ImGui::End();
    float panel_w = tab == 0 ? std::min(w * .64f, std::max(w * .39f, 540.f)) : w * .64f;
    ImGui::SetNextWindowPos({w * .04f, h * .30f});
    ImGui::SetNextWindowSize({panel_w, h * .65f});
    ImGui::Begin(I18n::Tr("设置"), nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    const char *tabs[] = {I18n::Tr("启动设置"), I18n::Tr("手柄映射"), I18n::Tr("键盘映射"), I18n::Tr("导入 PKG")};
    for (int i = 0; i < 4; ++i) {
        if (i)
            ImGui::SameLine();
        bool selected = i == tab;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, {.72f, .38f, .12f, 1});
        if (ImGui::Button(tabs[i]))
            tab = i;
        if (selected)
            ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginChild("settings-body", {0, -(ImGui::GetFrameHeight() + 22)}, ImGuiChildFlags_None);
    ImGui::PushTextWrapPos(0);
    if (tab == 0) {
        ImGui::TextUnformatted(I18n::Tr("游戏所在目录"));
        char path[4096];
        SDL_strlcpy(path, settings.folder.c_str(), sizeof(path));
        ImGui::SetNextItemWidth(-110);
        if (ImGui::InputText("##folder", path, sizeof(path))) {
            settings.folder = path;
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(I18n::Tr("浏览…"), {-1, 0}))
            PickPath(0);
        if (!settings.ValidGame())
            ImGui::TextColored(ImVec4(1, .68f, .35f, 1),
                               I18n::Tr("游戏目录不完整：需要 eboot.bin 与所选语言的 CPK"));
        ImGui::Spacing();
        ImGui::TextUnformatted(I18n::Tr("界面语言"));
        const auto available_languages = settings.AvailableGameLanguages();
        const bool can_traditional_ui =
            available_languages.empty() || settings.HasGameLanguage(10);
        std::vector<const char *> ui_languages{I18n::Tr("跟随系统"), "简体中文"};
        std::vector<std::string> ui_codes{"auto", "zh_CN"};
        if (can_traditional_ui) {
            ui_languages.push_back("繁體中文");
            ui_codes.push_back("zh_TW");
        }
        int ui_choice = 0;
        for (size_t i = 0; i < ui_codes.size(); ++i)
            if (settings.ui_language == ui_codes[i])
                ui_choice = int(i);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##ui-language", &ui_choice, ui_languages.data(),
                         int(ui_languages.size()))) {
            settings.ui_language = ui_codes[ui_choice];
            I18n::SetLanguage(I18n::FromCode(settings.UiLanguage()));
            dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(I18n::Tr("启动器界面语言；切换后立即生效"));
        ImGui::Spacing();
        ImGui::TextUnformatted(I18n::Tr("游戏语言"));
        std::vector<const char *> game_languages{I18n::Tr("跟随系统")};
        std::vector<int> game_values{0};
        for (int code : {11, 10, 9}) {
            if (!available_languages.empty() &&
                std::find(available_languages.begin(), available_languages.end(), code) ==
                    available_languages.end())
                continue;
            game_values.push_back(code);
            game_languages.push_back(code == 11   ? I18n::Tr("简体中文")
                                    : code == 10 ? I18n::Tr("繁体中文")
                                                 : "한국어");
        }
        int game_choice = 0;
        if (settings.game_language != "auto") {
            const int current = std::stoi(settings.game_language);
            for (size_t i = 0; i < game_values.size(); ++i)
                if (game_values[i] == current)
                    game_choice = int(i);
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##game-language", &game_choice, game_languages.data(),
                         int(game_languages.size()))) {
            settings.game_language =
                game_values[game_choice] == 0 ? "auto" : std::to_string(game_values[game_choice]);
            dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(I18n::Tr("决定游戏文本语言；重启游戏后生效"));
        ImGui::Spacing();
        ImGui::TextUnformatted(I18n::Tr("画面分辨率"));
        const char *res[] = {I18n::Tr("1080p  ·  标准"), I18n::Tr("1440p  ·  兼容模式"), I18n::Tr("4K  ·  原生 Pro"), I18n::Tr("4K  ·  兼容模式")};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##resolution", &settings.resolution, res, 4))
            dirty = true;
        if (ImGui::Checkbox(I18n::Tr("全屏启动"), &settings.fullscreen))
            dirty = true;
        ImGui::SameLine();
        if (ImGui::Checkbox(I18n::Tr("垂直同步"), &settings.vsync))
            dirty = true;
        ImGui::TextUnformatted(I18n::Tr("显示帧率"));
        const char *fps[] = {"60 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS", I18n::Tr("不限"), I18n::Tr("自定义")};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##refresh", &settings.refresh, fps, 7))
            dirty = true;
        if (settings.refresh == 6) {
            ImGui::TextUnformatted(I18n::Tr("帧率上限（FPS）"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##custom-refresh", &settings.custom_refresh)) {
                dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("30–1000 FPS");
        }
        int volume = int(settings.gain * 100 + .5f);
        ImGui::TextUnformatted(I18n::Tr("主音量"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##volume", &volume, 0, 400, "%d%%")) {
            settings.gain = volume / 100.f;
            dirty = true;
        }
    } else if (tab == 3)
        ImportPage();
    else
        Mappings(tab == 1);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() -
                         ImGui::GetStyle().WindowPadding.y);
    if (ImGui::Button(I18n::Tr("保存设置")))
        Save();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", dirty            ? I18n::Tr("有未保存的修改")
                              : status.empty() ? I18n::Tr("设置保存在本地")
                                               : status.c_str());
    ImGui::End();
    const float launch_height = std::max(h * (launching ? .30f : .16f), launching ? 300.f : 180.f);
    ImGui::SetNextWindowPos({w * .73f, h * .95f - launch_height});
    ImGui::SetNextWindowSize({w * .23f, launch_height});
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("launch", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    ImGui::PushStyleColor(ImGuiCol_Button, {.83f, .43f, .13f, .97f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, .60f, .23f, 1});
    ImGui::BeginDisabled(importing || launching);
    if (ImGui::Button(launching ? I18n::Tr("正在准备…") : I18n::Tr("开始游戏   >"), {-1, ImGui::GetFontSize() * 2.8f}))
        Start();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    ImGui::PushTextWrapPos(0);
    if (launching) {
        auto check = Read(launch_output / "image/verification.ini");
        bool hashing = check["phase"] == "verify";
        ImGui::TextWrapped("%s", hashing ? check["verification_mode"] == "first" ? I18n::Tr("首次使用此目录，正在完整校验…") : I18n::Tr("正在检查游戏文件的变化…") : I18n::Tr("正在准备游戏…"));
        if (hashing) {
            double bytes = std::strtod(check["bytes"].c_str(), nullptr),
                   total = std::strtod(check["total_bytes"].c_str(), nullptr);
            ImGui::ProgressBar(total > 0 ? float(bytes / total) : 0, {-1, 0});
            ImGui::TextDisabled("%.2f / %.2f GB", bytes / 1e9, total / 1e9);
            ImGui::TextWrapped("%s", check["file"].c_str());
        }
    } else ImGui::TextDisabled(I18n::Tr("F10 运行状态面板   /   支持手柄"));
    ImGui::PopTextWrapPos();
    ImGui::End();
    if (!launch_error.empty()) ImGui::OpenPopup(I18n::Tr("无法启动游戏"));
    ImGui::SetNextWindowSize({std::min(w * .7f, 900.f), 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(I18n::Tr("无法启动游戏"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", launch_error.c_str());
        if (ImGui::Button(I18n::Tr("确定"))) { launch_error.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    if (capture_ui) {
        if (auto *image = SDL_RenderReadPixels(renderer, nullptr)) {
            SDL_SaveBMP(image, screenshot.c_str());
            SDL_DestroySurface(image);
        }
        capture_ui = false;
        quit = true;
    }
    SDL_RenderPresent(renderer);
}
int Main(int argc, char **argv) {
    SDL_SetMainReady();
    // Detect a controller connected after the launcher opened, even while another window has
    // focus; the game uses the same setting before it starts.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    pad_set_compatibility_hints();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        return 1;
    fs::path root = fs::u8path(SDL_GetBasePath());
    for (int i = 0; i < 5 && !fs::exists(root / "scripts/aegis_run.py"); ++i)
        root = root.parent_path();
    settings.Load(root);
    I18n::SetLanguage(I18n::FromCode(settings.UiLanguage()));
    fs::current_path(root);
    fs::create_directories(root / "logs");
    const auto log_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char log_stamp[48];
    std::strftime(log_stamp,sizeof(log_stamp),"%Y%m%d-%H%M%S",std::localtime(&log_time));
    controller_log_path = root / "logs" / (std::string("controller-") + log_stamp + "-" + std::to_string(SDL_GetTicksNS()) + ".log");
#ifdef _WIN32
    controller_log = _wfopen(controller_log_path.c_str(),L"w");
#else
    controller_log = std::fopen(controller_log_path.c_str(),"w");
#endif
    if (!controller_log)
        throw std::runtime_error(I18n::Tr("无法写入手柄诊断日志 / Cannot write controller diagnostics"));
    pad_log_sdl(controller_log,"launcher");
    int width = 1920, height = 1080;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc)
            settings.folder = argv[++i];
        else if (a == "--screenshot" && i + 1 < argc)
            screenshot = argv[++i];
        else if (a == "--tab" && i + 1 < argc)
            tab = std::clamp(std::atoi(argv[++i]), 0, 3);
        else if (a == "--launch")
            auto_launch = true;
        else if (a == "--import" && i + 1 < argc) {
            pkg_file = argv[++i];
            auto_import = true;
            tab = 3;
        } else if (a == "--destination" && i + 1 < argc)
            import_folder = argv[++i];
        else if (a == "--size" && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &width, &height);
    }
    settings.NormalizeFolder();
    window =
        SDL_CreateWindow("十三机兵防卫圈 · Aegis", std::max(width, 1100), std::max(height, 700),
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
        return 1;
    SDL_SetWindowMinimumSize(window, 1100, 700);
    const char *drivers =
#ifdef _WIN32
        "direct3d11,direct3d12,opengl";
#else
        "opengl,opengles2";
#endif
    renderer = SDL_CreateRenderer(window, drivers);
    if (!renderer) {
        Error(I18n::Tr("无法创建硬件加速界面，请检查显卡驱动。"));
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    auto art = root / "assets/launcher-background.bmp";
    if (!fs::exists(art))
        art = root / "out/launcher-background.bmp";
    if (auto *bmp = SDL_LoadBMP(Utf8(art).c_str())) {
        background = SDL_CreateTextureFromSurface(renderer, bmp);
        SDL_DestroySurface(bmp);
    }
    auto icon_path = root / "assets/aegis.bmp";
    if (!fs::exists(icon_path)) icon_path = root / "out/aegis.bmp";
    if (auto *icon = SDL_LoadBMP(Utf8(icon_path).c_str())) {
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    std::vector<fs::path> fonts;
#ifdef _WIN32
    wchar_t windows_dir[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (length && length < MAX_PATH) {
        const auto directory = fs::path(windows_dir) / "Fonts";
        // Traditional Chinese uses JhengHei when present, Simplified/English use YaHei.
        if (I18n::Language() == I18n::Lang::ZhTW)
            for (auto *name : {L"msjh.ttc", L"msjhbd.ttc", L"mingliu.ttc"})
                fonts.push_back(directory / name);
        for (auto *name : {L"msyh.ttc", L"msyh.ttf", L"simhei.ttf", L"simsun.ttc"})
            fonts.push_back(directory / name);
    }
#else
    fonts.emplace_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
#endif
    for (const auto& font : fonts)
        if (fs::exists(font)) {
            ImFontConfig config;
#ifndef _WIN32
            // Noto Sans CJK is a collection: 0 JP, 1 HK, 2 KR, 3 SC, 4 TC.
            config.FontNo = I18n::Language() == I18n::Lang::ZhTW ? 4 : 3;
#endif
            if (!io.Fonts->AddFontFromFileTTF(Utf8(font).c_str(), 25, &config))
                io.Fonts->AddFontFromFileTTF(Utf8(font).c_str(), 25);
            break;
        }
    Theme(1);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    Controllers();
    int frames = 0;
    while (!quit) {
        bool running = Running();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                quit = true;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
                Controllers();
            if (capture >= 0) {
                std::string value;
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE)
                        capture = -1;
                    else if (!capture_pad) {
                        auto code = event.key.scancode;
                        if (code == SDL_SCANCODE_LCTRL || code == SDL_SCANCODE_RCTRL ||
                            code == SDL_SCANCODE_LSHIFT || code == SDL_SCANCODE_RSHIFT ||
                            code == SDL_SCANCODE_LALT || code == SDL_SCANCODE_RALT)
                            pending_modifier = code;
                        else {
                            value = "scancode:" + std::to_string(code);
                            auto mod = event.key.mod;
                            if (mod & SDL_KMOD_CTRL)
                                value = "Left Ctrl+" + value;
                            else if (mod & SDL_KMOD_SHIFT)
                                value = "Left Shift+" + value;
                            else if (mod & SDL_KMOD_ALT)
                                value = "Left Alt+" + value;
                            pending_modifier = SDL_SCANCODE_UNKNOWN;
                        }
                    }
                }
                if (!capture_pad && event.type == SDL_EVENT_KEY_UP &&
                    event.key.scancode == pending_modifier) {
                    value = "scancode:" + std::to_string(pending_modifier);
                    pending_modifier = SDL_SCANCODE_UNKNOWN;
                }
                if (capture_pad && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                    value = SDL_GetGamepadStringForButton(SDL_GamepadButton(event.gbutton.button));
                if (capture_pad && event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                    event.gaxis.value > 16384) {
                    if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                        value = "lefttrigger";
                    if (event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                        value = "righttrigger";
                }
                if (!value.empty() && capture >= 0) {
                    settings.SetBinding(capture, capture_pad, value);
                    dirty = true;
                    capture = -1;
                }
                continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&event);
        }
        TickRumbleTest();
        {
            std::lock_guard lock(dialog_mutex);
            if (dialog_done) {
                if (!chosen_folder.empty()) {
                    if (dialog_kind == 0) {
                        settings.folder = chosen_folder;
                        settings.NormalizeFolder();
                        dirty = true;
                    } else if (dialog_kind == 1)
                        pkg_file = chosen_folder;
                    else
                        import_folder = Utf8(fs::u8path(chosen_folder) / "CUSA18093");
                }
                chosen_folder.clear();
                dialog_done = false;
            }
        }
        if (!running || importing || launching) {
            ++frames;
            if (frames >= 12 && !screenshot.empty() && !running)
                capture_ui = true;
            Draw();
            if (auto_launch && frames == 5) {
                auto_launch = false;
                Start();
            }
            if (auto_import && frames == 5) {
                auto_import = false;
                StartImport();
            }
        } else
            SDL_Delay(60);
    }
    if (importing)
        CancelImport();
    if (launching && child) {
#ifdef _WIN32
        TerminateProcess(child, 1);
        WaitForSingleObject(child, INFINITE);
        CloseHandle(child);
#else
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
#endif
    }
    if (dirty)
        Save();
    for (auto *p : controllers)
        SDL_CloseGamepad(p);
    pad_log(controller_log,"launcher shutdown");
    if (controller_log) std::fclose(controller_log);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (background)
        SDL_DestroyTexture(background);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
} // namespace
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    auto w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    std::vector<char *> argv;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), n, nullptr, nullptr);
        s.pop_back();
        args.push_back(s);
    }
    for (auto &a : args)
        argv.push_back(a.data());
    LocalFree(w);
    try {
        return Main(argc, argv.data());
    } catch (const std::exception &e) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Aegis", e.what(), nullptr);
        return 1;
    }
}
#else
int main(int argc, char **argv) {
    try {
        return Main(argc, argv);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
#endif
