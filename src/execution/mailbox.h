#ifndef VCO_CONTROL_MAILBOX_H
#define VCO_CONTROL_MAILBOX_H
#include "control.h"
typedef enum {
    VCO_MAILBOX_OK = 0,
    VCO_MAILBOX_BUSY = 1,
    VCO_MAILBOX_WAIT = 2,
    VCO_MAILBOX_TICKET = 3,
    VCO_MAILBOX_CLOSED = 4,
    VCO_MAILBOX_OWNER = 5,
    VCO_MAILBOX_ARGUMENT = 6,
    VCO_MAILBOX_EXHAUSTED = 7,
    VCO_MAILBOX_EMPTY = 8,
    VCO_MAILBOX_COPY = 9
} VcoMailboxStatus;
/* One copied transport request/reply, matching the UI client's single-flight
 * policy. The execution engine still owns its bounded command queue. All client
 * operations try the gate once; disk, engine execution and waiting never occur
 * under the gate. State is private: use these functions, not direct field access. */
typedef struct {
    uint32_t gate, state, closing, stopped;
    VcoEngine *engine;
    int32_t owner;
    uint64_t sequence, ticket;
    VcoControlRequest request;
    VcoControlReply reply;
    VcoError stop_error;
} VcoControlMailbox;
/* Initialize once before publishing. No reinit/reuse while clients/worker retain
 * the object, including after stop. Engine lifetime exceeds mailbox lifetime. */
void vco_mailbox_init(VcoControlMailbox *, VcoEngine *, int32_t owner);
VcoMailboxStatus vco_mailbox_submit(VcoControlMailbox *, int32_t caller, const void *, size_t,
                                    uint64_t *ticket);
/* Recreated UI transport recovers an outstanding request without guessing or
 * cancelling it. Drain/ack this reply, then QUERY fresh state before any SET. */
VcoMailboxStatus vco_mailbox_resume(VcoControlMailbox *, int32_t caller, VcoControlRequest *request,
                                    uint64_t *ticket);
/* receive is non-consuming; ack only after the transport copied the entire reply
 * to its destination. Failed CopyToUser therefore cannot lose a response. */
VcoMailboxStatus vco_mailbox_receive(VcoControlMailbox *, int32_t caller, uint64_t ticket,
                                     VcoControlReply *);
VcoMailboxStatus vco_mailbox_ack(VcoControlMailbox *, int32_t caller, uint64_t ticket);
/* Single worker only, including bind/session changes; 1 continue, 0 stopped.
 * SET reply means accepted, not executed. Queries observe a consistent engine
 * snapshot. Periodic ticks continue while the client has an unread response. */
int vco_mailbox_pump(VcoControlMailbox *, uint64_t now_us);
/* Read-only lifecycle query for a native worker before taking ownership. */
int vco_mailbox_open(const VcoControlMailbox *);
void vco_mailbox_stop(VcoControlMailbox *);
int vco_mailbox_stop_result(const VcoControlMailbox *, VcoError *out);
#endif
