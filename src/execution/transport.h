#ifndef VCO_CONTROL_TRANSPORT_H
#define VCO_CONTROL_TRANSPORT_H
#include "client.h"
#include "mailbox.h"
/* Injected transport callbacks must make one bounded, nonblocking operation.
 * Failed output copies may leave a submitted request or consumed acknowledgement
 * on the server. Output parameters are usable only after VCO_MAILBOX_OK. */
typedef struct {
    VcoMailboxStatus (*submit)(void *, const VcoControlRequest *, uint64_t *ticket);
    VcoMailboxStatus (*resume)(void *, VcoControlRequest *, uint64_t *ticket);
    VcoMailboxStatus (*receive)(void *, uint64_t ticket, VcoControlReply *);
    VcoMailboxStatus (*ack)(void *, uint64_t ticket);
} VcoTransportOps;
typedef enum {
    VCO_TRANSPORT_DISABLED,
    VCO_TRANSPORT_RESUME,
    VCO_TRANSPORT_IDLE,
    VCO_TRANSPORT_SUBMIT,
    VCO_TRANSPORT_RECEIVE,
    VCO_TRANSPORT_ACK
} VcoTransportPhase;
/* UI-thread only. Own the object for the lifetime of its callbacks/context; do
 * not edit its client directly or call the client begin/accept functions. Its
 * public client member is the confirmed UI snapshot. Recovery scratch avoids an
 * approximately 8.5 KiB temporary client on the UI callback stack. */
typedef struct {
    VcoControlClient client, recovery;
    VcoTransportOps ops;
    void *context;
    VcoControlRequest request;
    uint64_t ticket;
    VcoTransportPhase phase;
    VcoMailboxStatus last_status;
    bool current, invalid_reply;
} VcoControlTransport;
/* New UI instances first RESUME/drain any old request before becoming idle.
 * Malformed old requests may be drained only by an exact canonical PACKET
 * rejection; they cannot publish a snapshot or advance the request sequence. */
bool vco_transport_init(VcoControlTransport *, VcoTransportOps, void *context);
/* Rebinding clears confirmed rows but preserves an outstanding transport ticket
 * until its old reply is validated and acknowledged. Unsent old commands are
 * never submitted after rebinding. A fresh QUERY is required before SET. */
bool vco_transport_bind(VcoControlTransport *, uint64_t session, uint64_t revision, unsigned count);
/* Reconnect to the same callbacks/server identity. It clears confirmed state,
 * drains any old server request, and never automatically repeats an old SET.
 * Replacing the callback context or server identity requires a new object. */
void vco_transport_reconnect(VcoControlTransport *);
bool vco_transport_query(VcoControlTransport *, unsigned first, unsigned count);
bool vco_transport_set(VcoControlTransport *, unsigned entry, bool enabled);
/* At most one callback per tick; IDLE does none. Busy/wait/copy failures retain
 * the exact request/ticket. Malformed replies return ARGUMENT, set invalid_reply,
 * and remain unacknowledged. Callers may surface the failure without faking OFF.
 * Only validated server snapshots can change client rows. */
VcoMailboxStatus vco_transport_tick(VcoControlTransport *);
bool vco_transport_idle(const VcoControlTransport *);
#endif
