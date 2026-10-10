#ifndef VCO_CONTROL_CLIENT_H
#define VCO_CONTROL_CLIENT_H
#include "control.h"
typedef struct {
    uint64_t sequence, session, revision;
    uint32_t count, flags, last_status, last_error;
    bool bound, waiting;
    VcoControlRequest flight;
    bool known[VCO_ENGINE_ENTRIES];
    VcoControlRow rows[VCO_ENGINE_ENTRIES];
} VcoControlClient;
/* UI-thread only. Initialize once; rebinding preserves the request sequence. */
void vco_control_client_init(VcoControlClient *client);
bool vco_control_client_bind(VcoControlClient *, uint64_t session, uint64_t revision,
                             unsigned count);
/* One transport request at a time, explicit SET rather than toggle. Before SET,
 * the row must have been queried and have no pending execution command. */
bool vco_control_client_query(VcoControlClient *, unsigned first, unsigned count,
                              VcoControlRequest *);
bool vco_control_client_set(VcoControlClient *, unsigned entry, bool enabled, VcoControlRequest *);
/* Retries reuse the exact request ID and payload; timeout is not an OFF result. */
bool vco_control_client_retry(const VcoControlClient *, VcoControlRequest *);
/* Invalid/late replies leave the in-flight request and confirmed state intact. */
bool vco_control_client_accept(VcoControlClient *, const void *reply, size_t length);
#endif
