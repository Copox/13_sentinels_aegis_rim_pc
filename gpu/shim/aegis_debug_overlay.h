// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
namespace Vulkan { class Instance; }
namespace AegisDebugOverlay {
void Init(const Vulkan::Instance& instance);
void Render(float frame_ms, u32 output_width, u32 output_height);
}
