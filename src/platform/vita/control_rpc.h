#ifndef VCO_VITA_CONTROL_RPC_H
#define VCO_VITA_CONTROL_RPC_H
#include "control_worker.h"
/* This context is kernel-owned and immutable while syscalls are in flight.
 * Caller verification must bind a retained Shell process instance, not merely
 * a cached PID or first-caller-wins rule. No verifier means access denied. This
 * authorizes the UI channel only, never the target game's write authority. */
typedef int (*VcoVitaRpcAuthorize)(void *context, int32_t caller);
typedef struct {
    VcoVitaControlWorker *worker;
    int32_t owner;
    VcoVitaRpcAuthorize authorize;
    void *context;
} VcoVitaControlRpc;
typedef struct {
    uint64_t ticket;
    VcoControlRequest request;
} VcoControlResume;
/* These are kernel-side entry helpers; the context is never a user argument.
 * A future exported syscall supplies a privately registered context. Every call
 * enters/exits syscall context and checks the live caller before user copying.
 * Results are VcoMailboxStatus; COPY never implies submission did not happen.
 * Exact output lengths are required; no retained user pointers, no dereference
 * of user payloads outside SDK CopyFromUser/CopyToUser. */
VcoMailboxStatus vco_vita_rpc_submit(VcoVitaControlRpc *, const void *user_request, uint32_t length,
                                     uint64_t *user_ticket);
VcoMailboxStatus vco_vita_rpc_receive(VcoVitaControlRpc *, uint64_t ticket, void *user_reply,
                                      uint32_t length);
VcoMailboxStatus vco_vita_rpc_ack(VcoVitaControlRpc *, uint64_t ticket);
VcoMailboxStatus vco_vita_rpc_resume(VcoVitaControlRpc *, VcoControlResume *user_resume,
                                     uint32_t length);
#endif
