// SPDX-License-Identifier: GPL-2.0-or-later
#include "aegis_debug_overlay.h"
#include "aegis_runtime_stats.h"
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "imgui.h"
#include "vulkan/instance.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace AegisDebugOverlay {
namespace {
const Vulkan::Instance* gpu;
std::string device_name;
std::array<float, 180> frames{};
unsigned next_frame, frame_count;
struct Sample {
    AegisRuntimeStats runtime{};
    u64 rss{}, vram_used{}, vram_budget{}, frames{}, draws{}, dispatches{};
    double cpu_percent{}, draw_average{}, dispatch_average{}, game_fps{};
    std::chrono::steady_clock::time_point time{};
    double cpu_seconds{};
} sample;
double CpuSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](FILETIME t) { return (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 1e7;
#else
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
#endif
}
u64 ResidentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ?
           counters.WorkingSetSize : 0;
#else
    unsigned long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        const int fields = std::fscanf(f, "%lu %lu", &pages, &resident);
        std::fclose(f);
        if (fields == 2) return u64(resident) * u64(sysconf(_SC_PAGESIZE));
    }
    return 0;
#endif
}
void Refresh() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - sample.time).count();
    if (sample.time.time_since_epoch().count() && elapsed < 1.0) return;
    const double cpu = CpuSeconds();
    const u64 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    const u64 draws = BbStats::draws.load(std::memory_order_relaxed);
    const u64 dispatches = BbStats::dispatches.load(std::memory_order_relaxed);
    if (sample.time.time_since_epoch().count() && elapsed < 5.0) {
        sample.cpu_percent = std::max(0.0, (cpu - sample.cpu_seconds) * 100.0 / elapsed);
        const double count = double(std::max<u64>(1, frame - sample.frames));
        sample.game_fps = double(frame - sample.frames) / elapsed;
        sample.draw_average = double(draws - sample.draws) / count;
        sample.dispatch_average = double(dispatches - sample.dispatches) / count;
    }
    sample.time = now;
    sample.cpu_seconds = cpu;
    sample.frames = frame;
    sample.draws = draws;
    sample.dispatches = dispatches;
    sample.rss = ResidentBytes();
    runtime_debug_snapshot(&sample.runtime);
    if (gpu && gpu->CanReportMemoryUsage()) {
        sample.vram_used = gpu->GetDeviceMemoryUsage();
        sample.vram_budget = gpu->GetDeviceMemoryBudgetNow();
    }
}
double MiB(u64 bytes) { return double(bytes) / (1024.0 * 1024.0); }
} // namespace

void Init(const Vulkan::Instance& instance) {
    gpu = &instance;
    device_name = instance.GetPhysicalDevice().getProperties().deviceName.data();
}
void Render(float ms, u32 output_width, u32 output_height) {
    Refresh();
    if (ms > 0 && ms < 1000) {
        frames[next_frame] = ms;
        next_frame = (next_frame + 1) % frames.size();
        frame_count = std::min<unsigned>(frame_count + 1, frames.size());
    }
    auto ordered = frames;
    std::sort(ordered.begin(), ordered.begin() + frame_count);
    const float p95 = frame_count ? ordered[(frame_count - 1) * 95 / 100] : 0;
    const auto& runtime = sample.runtime;
    const float scale = std::max(float(output_height) / 1080.0f, 0.75f);
    ImGui::SetNextWindowPos({16.0f * scale, 16.0f * scale}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("Aegis Debug Overlay [F10]", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Present: %.1f FPS   %.2f ms   p95 %.2f ms", ms > 0 ? 1000.0f / ms : 0, ms, p95);
    ImGui::PlotLines("##frame interval", frames.data(), int(frame_count),
        frame_count == frames.size() ? int(next_frame) : 0, nullptr, 0.0f, 40.0f,
        {440.0f * scale, 58.0f * scale});
    ImGui::TextUnformatted("Frame interval at presentation; not GPU execution time");
    ImGui::Separator();
    ImGui::Text("Game frame: %ux%u   Final frame: %ux%u", BbStats::present_source_width.load(),
        BbStats::present_source_height.load(), BbStats::encoded_width.load(),
        BbStats::encoded_height.load());
    ImGui::Text("Screen (swapchain): %ux%u", output_width, output_height);
    ImGui::Text("Game frames: %.1f FPS   Guest clock: 60 Hz", sample.game_fps);
    ImGui::Text("Render mode: %s", BbSettings::Get().aegis_pro_mode ? "PS4 Pro / native 4K" : "base PS4 / compatibility");
    ImGui::Text("CPU: %.1f%% (100%% = one core)   RAM: %.0f MiB", sample.cpu_percent, MiB(sample.rss));
    ImGui::Text("Guest workers: %llu active / %llu created",
        (unsigned long long)(runtime.threads_created - std::min(runtime.threads_exited, runtime.threads_created)),
        (unsigned long long)runtime.threads_created);
    ImGui::Text("Guest direct memory: %.0f / %.0f MiB", MiB(runtime.direct_bytes), MiB(runtime.direct_capacity));
    ImGui::Separator();
    ImGui::TextUnformatted(device_name.c_str());
    if (sample.vram_budget) ImGui::Text("GPU heap usage / budget: %.0f / %.0f MiB",
        MiB(sample.vram_used), MiB(sample.vram_budget));
    else ImGui::TextUnformatted("GPU heap budget: unavailable on this driver");
    ImGui::Text("Cached images: %llu / %.0f MiB   Vulkan images: %.0f MiB",
        (unsigned long long)BbStats::live_images.load(), MiB(BbStats::live_image_bytes.load()),
        MiB(BbStats::vk_image_bytes.load()));
    ImGui::Text("Draws / dispatches per frame: %.0f / %.0f", sample.draw_average, sample.dispatch_average);
    ImGui::Text("CPU readback requests: %llu   Memory write faults: %llu",
        (unsigned long long)BbStats::readbacks.load(), (unsigned long long)BbStats::tracker_faults.load());
    ImGui::Separator();
    const char* backend = SDL_GetCurrentAudioDriver();
    ImGui::Text("Audio: %s   Gain: %.0f%%", backend ? backend : "no device / timing only",
        runtime.audio_gain_milli / 10.0f);
    ImGui::Text("Buffers: %llu (%llu nonzero)   Queue errors: %llu",
        (unsigned long long)runtime.audio_buffers, (unsigned long long)runtime.audio_nonzero,
        (unsigned long long)runtime.audio_errors);
    ImGui::TextDisabled("F10 / Insert: hide. Game controls remain active.");
    ImGui::End();
}
} // namespace AegisDebugOverlay
