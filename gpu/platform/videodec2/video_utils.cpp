// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_utils.h"

#include "common/alignment.h"
#include "common/assert.h"

#include <libavutil/frame.h>

#include <algorithm>
#include <cstring>

namespace Libraries::Videodec {

void CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src) {
    const auto dst_pitch = Common::AlignUp<u32>(src.width, 64);
    const auto dst_height = Common::AlignUp<u32>(src.height, 16);

    ASSERT(((dst_pitch * dst_height * 3) / 2) <= max_size);

    const auto luma_dst = dst;
    const auto chroma_dst = dst + dst_pitch * dst_height;

    // FFmpeg may pad source rows even when the destination pitch equals the width.
    // Copy row by row and extend the edge into the alignment padding.
    for (u32 y = 0; y < dst_height; ++y) {
        const u32 sy = std::min<u32>(y, src.height - 1);
        u8* row = luma_dst + y * dst_pitch;
        std::memcpy(row, src.data[0] + sy * src.linesize[0], src.width);
        std::memset(row + src.width, row[src.width - 1], dst_pitch - src.width);
    }
    for (u32 y = 0; y < dst_height / 2; ++y) {
        const u32 sy = std::min<u32>(y, src.height / 2 - 1);
        u8* row = chroma_dst + y * dst_pitch;
        std::memcpy(row, src.data[1] + sy * src.linesize[1], src.width);
        for (u32 x = src.width; x < dst_pitch; x += 2) {
            row[x] = row[src.width - 2];
            row[x + 1] = row[src.width - 1];
        }
    }
}

} // namespace Libraries::Videodec
