#ifndef VCO_AGENT_BROKER_H
#define VCO_AGENT_BROKER_H
#include "protocol.h"

#define VCO_MODULE_CALLS 8u
typedef struct {
    uint64_t session, ticket, claim;
} VcoModuleCall;

/* All access is serialized by the native adapter, including user copies. This
 * broker never calls an executor, retains game pointers or owns taiHEN patches.
 * owner/caller must come from independently authenticated syscall context. */
typedef struct {
    uint64_t next_session, next_ticket;
    int32_t owner;
    bool attached, runnable, quiescent;
    unsigned phase;
    VcoAgentBinding binding;
    VcoAgentMessage message;
    VcoAgentResult result;
    VcoServiceStatus status;
    VcoControlRequest control;
    uint8_t *upload;
    size_t upload_capacity;
    uint64_t next_claim, module_ticket, module_claims[VCO_MODULE_CALLS];
    bool module_pending, module_ready, module_clear;
    VcoError module_error;
} VcoAgentBroker;
VcoAgentStatus vco_broker_init(VcoAgentBroker *, int32_t owner, void *upload, size_t capacity);
/* Loader/terminal/foreground hooks only, never exposed as game registration.
 * open invalidates old messages; end performs no target memory operation. */
VcoAgentStatus vco_broker_open(VcoAgentBroker *, int32_t pid, const char title[10],
                               VcoAgentBinding *);
VcoAgentStatus vco_broker_end(VcoAgentBroker *, uint64_t session);
VcoAgentStatus vco_broker_observe(VcoAgentBroker *, uint64_t session, bool runnable);
VcoAgentStatus vco_broker_attach(VcoAgentBroker *, int32_t caller, const VcoAgentBinding *);
/* Trusted adapter only, with old callers quiescent. Rebinding starts a clear
 * transaction, never transfers old Shell intent into the replacement Shell. */
VcoAgentStatus vco_broker_rebind_owner(VcoAgentBroker *, int32_t owner);
/* A slot remains owned until Shell ACK. Poll/reply retries are idempotent. */
VcoAgentStatus vco_broker_load(VcoAgentBroker *, int32_t caller, const void *, size_t,
                               uint64_t *ticket);
VcoAgentStatus vco_broker_clear(VcoAgentBroker *, int32_t caller, uint64_t session,
                                uint64_t *ticket);
VcoAgentStatus vco_broker_submit(VcoAgentBroker *, int32_t caller, const VcoControlRequest *,
                                 uint64_t *ticket);
VcoAgentStatus vco_broker_poll(VcoAgentBroker *, int32_t caller, uint64_t session,
                               VcoAgentMessage *, void *payload, size_t capacity);
VcoAgentStatus vco_broker_publish(VcoAgentBroker *, int32_t caller, const VcoAgentResult *);
VcoAgentStatus vco_broker_receive(VcoAgentBroker *, int32_t caller, uint64_t ticket,
                                  VcoAgentResult *);
VcoAgentStatus vco_broker_ack(VcoAgentBroker *, int32_t caller, uint64_t ticket);
VcoAgentStatus vco_broker_resume(VcoAgentBroker *, int32_t caller, VcoControlRequest *,
                                 uint64_t *ticket);
VcoAgentStatus vco_broker_status(VcoAgentBroker *, int32_t caller, VcoServiceStatus *);
/* Trusted public module-call adapter only. Each live call holds a copied claim;
 * no target object or lock is retained. Original SDK calls start only after the
 * game worker's all-OFF/rebind receipt. A timeout cancels its claim, never its
 * in-flight cleanup. New Shell work remains blocked until cleanup and all
 * joined original calls have ended. All calls require the native broker gate. */
bool vco_broker_module_busy(const VcoAgentBroker *);
VcoAgentStatus vco_broker_module_begin(VcoAgentBroker *, int32_t caller, VcoModuleCall *);
VcoAgentStatus vco_broker_module_ready(VcoAgentBroker *, const VcoModuleCall *, VcoError *);
VcoAgentStatus vco_broker_module_end(VcoAgentBroker *, const VcoModuleCall *);
#endif
