// SPDX-License-Identifier: GPL-2.0-or-later
#include "aegis_damage_rumble.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

#include "bbport_guest_code_hooks.h"
#include "common/elf_info.h"

extern "C" int runtime_memory_is_mapped(std::uintptr_t address, std::uint64_t size);
extern "C" void runtime_pad_player_damage(int pilot, int before, int after, int maximum);

namespace AegisDamageRumble {
namespace {
constexpr std::uintptr_t Singleton = 0x15cccd8;
template <typename T> T Read(std::uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}
bool Mapped(std::uintptr_t address, std::uint64_t size) {
    return address && size <= std::numeric_limits<std::uintptr_t>::max() - address &&
           runtime_memory_is_mapped(address, size);
}

// Called only at the battle damage commit, after shields/invulnerability and HP clamping.
// Do not observe the generic HP setter: loading, healing and deployment also use it.
void Observe(std::uintptr_t base, std::uintptr_t unit, int before) {
    if (!Mapped(unit, 0x2d4)) return;
    const auto kind = Read<std::uintptr_t>(unit + 0x60);
    if (kind != base + 0x78f650 && kind != base + 0x78f670) return; // Kihei / KiheiFly
    const int after = Read<int>(unit + 0x2cc), maximum = Read<int>(unit + 0x2d0);
    if (before <= 0 || after < 0 || after >= before || maximum <= 0 || before > maximum)
        return;
    if (!Mapped(unit, 0x3740)) return;
    const auto pilot = Read<std::uintptr_t>(unit + 0x3738);
    if (!Mapped(pilot, 4)) return;
    const auto id = Read<std::uint32_t>(pilot);
    if (id > 12) return;
    const auto game = Read<std::uintptr_t>(base + Singleton);
    if (!Mapped(game, 0xf8)) return;
    const auto roster = Read<std::uintptr_t>(game + 0xf0);
    if (!Mapped(roster, 0x98)) return;
    // The exact pilot record must belong to the game's thirteen-character roster.
    // Being a friendly battle object alone is insufficient (terminal, city, summons).
    if (Read<std::uintptr_t>(roster + 0x30 + id * 8) != pilot) return;
    runtime_pad_player_damage(static_cast<int>(id), before, after, maximum);
}

void __attribute__((sysv_abi)) AfterDamage(std::uint64_t base,
                                          const Core::GuestRegisterSnapshot* regs) {
    // Nonzero takes the special path that skips damage calculation and removes
    // all remaining HP; exclude scripted removal from hit feedback.
    if (Read<std::uint8_t>(regs->rbp - 0x3c)) return;
    // Audited damage function preserves the pre-hit HP in its own stack frame.
    Observe(base, regs->r14, Read<int>(regs->rbp - 0x54));
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA18093") return;
    constexpr std::uint64_t Offset = 0xbb176;
    constexpr std::array<std::uint8_t, 6> Instructions = {0x49, 0x8b, 0x06, 0x4c, 0x89, 0xf7};
    constexpr std::array<std::uint8_t, 17> Entry = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58};
    // Verify the HP subtraction, both clamping paths and the continuation, not just a
    // common instruction sequence. Leave unknown builds untouched.
    constexpr std::uint64_t CheckOffset = 0xbb14f;
    constexpr std::array<std::uint8_t, 39> Commit = {
        0x89,0xd1,0x44,0x89,0x65,0xa0,0x89,0x55,0xac,0x44,0x29,0xe1,
        0x44,0x0f,0x4c,0xe2,0x41,0x89,0x8e,0xcc,0x02,0x00,0x00,
        0x78,0x07,0x39,0xc1,0x41,0x89,0xc7,0x7e,0x07,
        0x45,0x89,0xbe,0xcc,0x02,0x00,0x00};
    if (size < Singleton + 8 || size < Offset + Instructions.size() ||
        std::memcmp(image + 0xbae70, Entry.data(), Entry.size()) ||
        std::memcmp(image + CheckOffset, Commit.data(), Commit.size()) ||
        std::memcmp(image + Offset, Instructions.data(), Instructions.size())) {
        std::puts("Aegis damage rumble: unknown damage build; hook skipped");
        return;
    }
    const bool installed = Core::InstallGuestCodeHook(image + Offset, Instructions,
        reinterpret_cast<std::uintptr_t>(image), AfterDamage);
    std::printf("Aegis damage rumble: player HP commit hook %s\n",
                installed ? "installed" : "unavailable");
}
} // namespace AegisDamageRumble
