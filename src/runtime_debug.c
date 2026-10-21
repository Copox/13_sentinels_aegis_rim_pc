#include "../gpu/aegis_runtime_stats.h"
#include <string.h>

void runtime_debug_snapshot(AegisRuntimeStats *stats) {
    memset(stats, 0, sizeof(*stats));
    runtime_thread_debug(stats);
    runtime_memory_debug(stats);
    runtime_audio_debug(stats);
}
