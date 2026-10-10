#ifndef VCO_GAME_PROBE_H
#define VCO_GAME_PROBE_H
#include <stdint.h>
#define VCO_PROBE_ABI 0x00010000u
/* Diagnostic evidence, NOT a session token or permission to write. */
typedef struct {
    uint32_t abi, size;
    int32_t pid, info_result, status_result, process_status;
    int32_t main_thread, modules_result;
    uint32_t module_count, verified_capabilities;
} VcoGameProbe;
#ifdef __cplusplus
extern "C" {
#endif
int vcoProbeGame(int32_t pid, VcoGameProbe *out);
#ifdef __cplusplus
}
#endif
#endif
