#ifndef VCO_RESOURCE_PROFILE_H
#define VCO_RESOURCE_PROFILE_H
#include <stdbool.h>
#include <stdint.h>
#include "../storage_paths.h"
#define VCO_RESOURCE_LOG VCO_STORAGE_DIR "/vco-resource-profile.log"

/* Developer build only. Normal builds contain neither sampling nor I/O.
 * The view thread is the sole visibility writer. The existing control worker
 * is the sole sampler; call only after releasing the service's locks. */
#ifdef VCO_RESOURCE_PROFILE
#define VCO_RESOURCE_PROFILE_REVISION 4
#define VCO_RESOURCE_PROFILE_START_ATTEMPTS 8u
#define VCO_RESOURCE_PROFILE_END_ATTEMPTS 8u
#define VCO_RESOURCE_PROFILE_START_MARKER "shell_start_resource_profile_r4"
#ifdef __cplusplus
extern "C" {
#endif
void vco_resource_profile_visible(bool visible);
void vco_resource_profile_tick(int32_t observer, int32_t control, uint32_t state, unsigned enabled,
                               uint64_t session);
#ifdef __cplusplus
}
#endif
#else
static inline void vco_resource_profile_visible(bool visible)
{
    (void)visible;
}
#endif
#endif
