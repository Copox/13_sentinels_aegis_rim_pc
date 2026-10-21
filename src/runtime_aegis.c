#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

static int enabled, neo_mode;
int runtime_neo_mode(void) { return neo_mode; }
void runtime_aegis_configure(const char *serial) {
    enabled = serial && !strcmp(serial, "CUSA18093");
    neo_mode = 0;
    const char *path = getenv("BB_CONFIG");
    FILE *config = path ? fopen(path, "r") : NULL;
    char mode[32] = "base";
    if (config) {
        char line[512], value[32];
        while (fgets(line, sizeof(line), config)) {
            if (sscanf(line, " render_mode = %31s", value) == 1)
                snprintf(mode, sizeof(mode), "%s", value);
        }
        fclose(config);
    }
    const char *override = getenv("BB_AEGIS_RENDER_MODE");
    if (override && *override) snprintf(mode, sizeof(mode), "%s", override);
    if (strcmp(mode, "base") && strcmp(mode, "pro")) {
        fprintf(stderr, "Runtime: invalid render_mode '%s'; expected base or pro\n", mode);
        exit(1);
    }
    neo_mode = enabled && !strcmp(mode, "pro");
    if (enabled)
        printf("Runtime: render mode %s; guest/GPU Neo=%d; guest clock remains 60 Hz\n",
               neo_mode ? "PS4 Pro (native 3840x2160)" : "base PS4", neo_mode);
}

/* Optional libc instrumentation and live-stream/share services are unavailable
 * in this offline runtime. */
static ABI uintptr_t instrumentation_unavailable(void) { return 0; }

static const RuntimeExport exports[] = {
    {"_sceKernelSetThreadAtexitCount", instrumentation_unavailable},
    {"_sceKernelSetThreadAtexitReport", instrumentation_unavailable},
    {"sceKernelGetSanitizerMallocReplaceExternal", instrumentation_unavailable},
    {"sceKernelGetSanitizerNewReplaceExternal", instrumentation_unavailable},
    {"sceGameLiveStreamingInitialize", instrumentation_unavailable},
    {"sceGameLiveStreamingTerminate", instrumentation_unavailable},
    {"sceGameLiveStreamingEnableLiveStreaming", instrumentation_unavailable},
    {"sceShareUtilityAdvanceInitialize", instrumentation_unavailable},
    {"sceShareUtilityAdvanceSetUploadContentData", instrumentation_unavailable},
    {"sceShareUtilityAdvanceTerminate", instrumentation_unavailable},
};

uintptr_t runtime_aegis_resolve(const char *name) {
    return enabled ? RUNTIME_LOOKUP(exports, name) : 0;
}
