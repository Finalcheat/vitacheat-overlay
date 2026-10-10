#ifndef VCO_FOREGROUND_PROBE_H
#define VCO_FOREGROUND_PROBE_H
#include <stdint.h>

#define VCO_FOREGROUND_PROBE_ABI 0x00010000u
typedef enum {
    VCO_FOREGROUND_UNKNOWN,
    VCO_FOREGROUND_CANDIDATE,
    VCO_BACKGROUND_CANDIDATE
} VcoForegroundProbeStatus;
typedef enum {
    VCO_FOREGROUND_FAILURE_NONE,
    VCO_FOREGROUND_FAILURE_QUERY,
    VCO_FOREGROUND_FAILURE_IDENTITY,
    VCO_FOREGROUND_FAILURE_CHANGED
} VcoForegroundProbeFailure;

/* Fixed-width 48-byte diagnostic snapshot. Candidate means consistent repeated
 * observations only: this is NOT a lifecycle token or permission to write.
 * Unknown results clear all IDs/title, including when reusing the output. */
typedef struct {
    uint32_t abi, size, status, failure;
    int32_t active_app_id, game_app_id, active_pid, game_pid;
    char game_title_id[10];
    uint8_t reserved[6];
} VcoForegroundSample;

#ifdef __cplusplus
extern "C" {
#endif
/* SceShell user context only. No shared state, memory writes to the game, TTL,
 * foreground lease, or device startup integration. NULL is a no-op. */
void vco_foreground_sample(VcoForegroundSample *out);
#ifdef __cplusplus
}
#endif
#endif
