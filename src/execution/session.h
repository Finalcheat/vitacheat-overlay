#ifndef VCO_SESSION_H
#define VCO_SESSION_H
#include <stdbool.h>
#include <stdint.h>
/* instance must be a lifecycle identity verified by the adapter, never a PID
 * or a sampled address alone. Unknown observations revoke execution authority. */
typedef struct {
    int32_t pid;
    uint64_t instance;
    char title_id[10];
} VcoIdentity;
typedef enum {
    VCO_OBSERVATION_UNKNOWN,
    VCO_OBSERVATION_ABSENT,
    VCO_OBSERVATION_BACKGROUND,
    VCO_OBSERVATION_FOREGROUND
} VcoObservation;
typedef struct {
    VcoIdentity identity;
    uint64_t token, sequence;
    bool present, runnable;
} VcoSession;
void vco_session_init(VcoSession *session);
bool vco_identity_valid(const VcoIdentity *identity);
/* Returns true on identity/liveness/authority transition. Zero token is invalid. */
bool vco_session_observe(VcoSession *session, VcoObservation observation,
                         const VcoIdentity *identity);
bool vco_session_matches(const VcoSession *session, uint64_t token, bool require_foreground);
#endif
