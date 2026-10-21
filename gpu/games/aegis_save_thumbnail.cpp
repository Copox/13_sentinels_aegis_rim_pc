// SPDX-License-Identifier: GPL-2.0-or-later
// CUSA18093's scene preview is a JPEG inside ROBO13.DAT, made by its own
// CPU encoder from an offscreen tiled render target (not a VideoOut buffer).
#include "aegis_save_thumbnail.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_guest_code_hooks.h"
#include "common/elf_info.h"
#include "core/memory.h"
#include "render/rasterizer.h"

namespace AegisSaveThumbnail {
namespace {
template <typename T>
T Read(std::uint64_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

void __attribute__((sysv_abi)) BeforeEncode(std::uint64_t,
                                          const Core::GuestRegisterSnapshot* regs) {
    // One JPEG is encoded over several updates. Complete the pending capture
    // before the first slice, rather than changing its pixels halfway through.
    if (Read<std::uint32_t>(regs->rdi + 0x638) == 0) {
        if (auto* rasterizer = Core::Memory::Instance()->GetRasterizer()) {
            rasterizer->SynchronizeImageReadbacks();
        }
    }
}

void __attribute__((sysv_abi)) ConfigureSource(std::uint64_t,
                                              const Core::GuestRegisterSnapshot* regs) {
    // The game's screenshot-task initializer receives {FrameBuffer*, index, ...}.
    // Each FrameBuffer texture is 0x148 bytes; its Gnm surface holds the pixel
    // address at +0xc0 (surface +0x50 in the texture). Register the actual allocated
    // source before the first capture draw, rather than guessing a guest address.
    const auto framebuffer = Read<std::uint64_t>(regs->rsi);
    const auto index = Read<std::uint32_t>(regs->rsi + 8);
    const auto textures = Read<std::uint64_t>(framebuffer + 0x188);
    const auto texture = textures + std::uint64_t(index) * 0x148;
    if (Read<std::uint32_t>(texture) != 2) {
        std::puts("Aegis thumbnail: unsupported screenshot surface");
        return;
    }
    const auto address = Read<std::uint64_t>(texture + 0x110);
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (rasterizer && address) {
        rasterizer->RegisterImageReadback(address);
        std::printf("Aegis thumbnail: source %#llx registered for CPU readback at GPU fences\n",
                    static_cast<unsigned long long>(address));
    }
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA18093") {
        return;
    }
    const char* setting = std::getenv("BB_SAVE_THUMBNAILS");
    if (setting && std::strcmp(setting, "0") == 0) {
        std::puts("Aegis thumbnail: CPU readback disabled (BB_SAVE_THUMBNAILS=0)");
        return;
    }
    // Audited CUSA18093 task initializer: whole, position-independent instructions.
    // Check the surrounding prologue too, so another game build stays untouched.
    constexpr std::uint64_t Offset = 0x5b1290;
    constexpr std::array<std::uint8_t, 16> Prologue = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0xc5,
        0xfc, 0x10, 0x06, 0xc5, 0xfc, 0x10, 0x4e, 0x0c};
    constexpr std::uint64_t EncoderOffset = 0x5b0b90;
    constexpr std::array<std::uint8_t, 20> EncoderPrologue = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
        0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00};
    if (size < Offset + Prologue.size() ||
        std::memcmp(image + Offset, Prologue.data(), Prologue.size()) != 0 ||
        std::memcmp(image + EncoderOffset, EncoderPrologue.data(), EncoderPrologue.size()) != 0) {
        std::puts("Aegis thumbnail: unknown encoder build; hook skipped");
        return;
    }
    const bool installed = Core::InstallGuestCodeHook(
        image + Offset, std::span{Prologue}.first<6>(), 0, ConfigureSource);
    std::printf("Aegis thumbnail: screenshot source hook %s\n",
                installed ? "installed" : "unavailable");
    if (installed) {
        const bool encoder = Core::InstallGuestCodeHook(
            image + EncoderOffset, std::span{EncoderPrologue}.first<6>(), 0, BeforeEncode);
        std::printf("Aegis thumbnail: encoder synchronization hook %s\n",
                    encoder ? "installed" : "unavailable");
    }
}
} // namespace AegisSaveThumbnail
