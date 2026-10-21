// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t threads_created, threads_exited;
    uint64_t direct_bytes, direct_capacity;
    uint64_t audio_buffers, audio_nonzero, audio_errors, audio_ports;
    uint32_t audio_gain_milli;
} AegisRuntimeStats;
void runtime_debug_snapshot(AegisRuntimeStats* stats);
void runtime_thread_debug(AegisRuntimeStats* stats);
void runtime_memory_debug(AegisRuntimeStats* stats);
void runtime_audio_debug(AegisRuntimeStats* stats);
#ifdef __cplusplus
}
#endif
