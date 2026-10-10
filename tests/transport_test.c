#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "execution/transport.h"

static VcoEngine engine;
static VcoSession session;
static VcoControlMailbox mailbox;
static VcoControlTransport transport;
static VcoCheatEntry entries[1];
static VcoCode codes[1];
static VcoCheatOp ops[1];
static VcoPatch patches[1];
static char names[32];
static unsigned writes;
typedef struct {
    unsigned calls, submits, resumes, receives, acks;
    VcoMailboxStatus submit_status, resume_status, receive_status, ack_status;
    bool lost_submit, lost_ack;
    unsigned corrupt;
} Link;
static Link link;

static int alive(void *context, uint64_t token)
{
    (void)context;
    return vco_session_matches(&session, token, true);
}
static int write_memory(void *context, uint64_t token, uint32_t address, const uint8_t *in,
                        size_t size)
{
    assert(alive(context, token) && address == 0x81000000u && in && size == 4);
    ++writes;
    return 0;
}
static VcoMailboxStatus submit(void *context, const VcoControlRequest *request, uint64_t *ticket)
{
    Link *l = context;
    ++l->calls;
    ++l->submits;
    if (l->submit_status) {
        *ticket = UINT64_MAX; /* Failed output copies must never be consumed. */
        return l->submit_status;
    }
    VcoMailboxStatus result = vco_mailbox_submit(&mailbox, 88, request, sizeof(*request), ticket);
    if (!result && l->lost_submit) {
        l->lost_submit = false;
        *ticket = UINT64_MAX;
        return VCO_MAILBOX_COPY;
    }
    return result;
}
static VcoMailboxStatus resume(void *context, VcoControlRequest *request, uint64_t *ticket)
{
    Link *l = context;
    ++l->calls;
    ++l->resumes;
    if (l->resume_status) {
        memset(request, 0xaa, sizeof(*request));
        *ticket = UINT64_MAX;
        return l->resume_status;
    }
    VcoMailboxStatus result = vco_mailbox_resume(&mailbox, 88, request, ticket);
    if (!result && l->corrupt == 10)
        request->reserved[0] = 1;
    return result;
}
static VcoMailboxStatus receive(void *context, uint64_t ticket, VcoControlReply *reply)
{
    Link *l = context;
    ++l->calls;
    ++l->receives;
    if (l->receive_status) {
        memset(reply, 0xaa, sizeof(*reply));
        return l->receive_status;
    }
    VcoMailboxStatus result = vco_mailbox_receive(&mailbox, 88, ticket, reply);
    if (!result) {
        switch (l->corrupt) {
        case 1:
            ++reply->request;
            break;
        case 2:
            ++reply->revision;
            break;
        case 3:
            reply->rows[0].state = VCO_ENTRY_ON; /* No desired flag / acknowledged ID. */
            break;
        case 4:
            reply->total = UINT32_MAX;
            break;
        case 5:
            reply->rows[1].desired = 1; /* Nonzero padding outside requested rows. */
            break;
        case 6:
            reply->status = VCO_CONTROL_STALE; /* Noncanonical rejection. */
            break;
        case 7:
            ++reply->abi;
            break;
        case 8:
            --reply->size;
            break;
        case 9:
            reply->error = 1;
            break;
        case 11:
            reply->flags = VCO_CONTROL_FOREGROUND;
            break;
        case 12:
            ++reply->session;
            break;
        case 13:
            ++reply->first;
            break;
        case 14:
            ++reply->count;
            break;
        case 15:
            reply->rows[0].pending = 1;
            break;
        case 16:
            reply->status = VCO_CONTROL_OK;
            break;
        }
    }
    return result;
}
static VcoMailboxStatus ack(void *context, uint64_t ticket)
{
    Link *l = context;
    ++l->calls;
    ++l->acks;
    if (l->ack_status)
        return l->ack_status;
    VcoMailboxStatus result = vco_mailbox_ack(&mailbox, 88, ticket);
    if (!result && l->lost_ack) {
        l->lost_ack = false;
        return VCO_MAILBOX_COPY;
    }
    return result;
}
static const VcoTransportOps callbacks = {submit, resume, receive, ack};
static void setup(void)
{
    memset(&link, 0, sizeof(link));
    writes = 0;
    VcoIdentity identity = {42, 3, "PCSB00031"};
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    vco_engine_init(&engine);
    VcoCheatFile file = {entries, 1, 0, codes, 1, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    const char text[] = "_V1 Data\n$0200 81000000 11223344\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    VcoCheatMemory memory = {0};
    memory.alive = alive;
    memory.write = write_memory;
    assert(!vco_engine_bind(&engine, &session, 1, &file, memory, ops, patches, 1));
    vco_mailbox_init(&mailbox, &engine, 88);
    assert(vco_transport_init(&transport, callbacks, &link));
    assert(vco_transport_bind(&transport, session.token, engine.revision, 1));
}
static void tick(VcoMailboxStatus expected)
{
    unsigned calls = link.calls;
    assert(vco_transport_tick(&transport) == expected);
    assert(link.calls - calls <= 1); /* No retries or waits inside one UI frame. */
}
static void pump(void)
{
    assert(vco_mailbox_pump(&mailbox, 1));
}
static void ready(void)
{
    tick(VCO_MAILBOX_EMPTY);
    assert(vco_transport_idle(&transport));
    assert(!vco_transport_set(&transport, 0, true));
}
static void query(void)
{
    assert(vco_transport_query(&transport, 0, 1));
    tick(VCO_MAILBOX_OK);
    pump();
    tick(VCO_MAILBOX_OK);
    assert(transport.phase == VCO_TRANSPORT_ACK);
    tick(VCO_MAILBOX_OK);
    assert(vco_transport_idle(&transport));
}
static VcoControlRequest old_request(uint64_t id, unsigned operation)
{
    return (VcoControlRequest){id,
                               session.token,
                               engine.revision,
                               VCO_CONTROL_ABI,
                               sizeof(VcoControlRequest),
                               operation,
                               0,
                               operation == VCO_CONTROL_SET,
                               1,
                               {0, 0}};
}
static void delivery(void)
{
    ready();
    assert(vco_transport_query(&transport, 0, 1));
    assert(!vco_transport_query(&transport, 0, 1));
    tick(VCO_MAILBOX_OK);
    assert(transport.phase == VCO_TRANSPORT_RECEIVE && transport.ticket);
    tick(VCO_MAILBOX_WAIT);
    assert(transport.client.waiting && !transport.client.known[0]);
    pump();
    tick(VCO_MAILBOX_OK);
    assert(transport.client.known[0] && transport.client.rows[0].state == VCO_ENTRY_OFF);
    assert(!transport.client.waiting && !writes);
    assert(!vco_transport_set(&transport, 0, true)); /* ACK still outstanding. */
    tick(VCO_MAILBOX_OK);
    assert(vco_transport_set(&transport, 0, true));
    assert(transport.client.rows[0].state == VCO_ENTRY_OFF && !writes);
    tick(VCO_MAILBOX_OK);
    pump();
    tick(VCO_MAILBOX_OK);
    assert(transport.client.rows[0].state == VCO_ENTRY_ENABLING && writes == 1);
    tick(VCO_MAILBOX_OK);
    assert(!vco_transport_set(&transport, 0, false));
    query();
    assert(transport.client.rows[0].state == VCO_ENTRY_ON && writes == 1);
    assert(vco_transport_set(&transport, 0, false));
    tick(VCO_MAILBOX_OK);
    pump();
    tick(VCO_MAILBOX_OK);
    tick(VCO_MAILBOX_OK);
    query();
    assert(transport.client.rows[0].state == VCO_ENTRY_OFF && writes == 1);
}
static void retries(void)
{
    ready();
    assert(vco_transport_query(&transport, 0, 1));
    VcoControlRequest original = transport.request;
    link.lost_submit = true;
    tick(VCO_MAILBOX_COPY);
    assert(transport.phase == VCO_TRANSPORT_SUBMIT && !transport.ticket);
    assert(!memcmp(&original, &transport.request, sizeof(original)));
    uint64_t server_ticket = 0;
    VcoControlRequest server_request;
    assert(!vco_mailbox_resume(&mailbox, 88, &server_request, &server_ticket));
    pump();
    tick(VCO_MAILBOX_OK);
    assert(transport.ticket == server_ticket && mailbox.sequence == 1);
    link.receive_status = VCO_MAILBOX_COPY;
    tick(VCO_MAILBOX_COPY);
    assert(transport.ticket == server_ticket && transport.client.waiting &&
           !transport.client.known[0]);
    link.receive_status = VCO_MAILBOX_OK;
    tick(VCO_MAILBOX_OK);
    link.ack_status = VCO_MAILBOX_BUSY;
    tick(VCO_MAILBOX_BUSY);
    assert(transport.phase == VCO_TRANSPORT_ACK && transport.ticket == server_ticket);
    assert(transport.client.known[0] && !vco_transport_query(&transport, 0, 1));
    link.ack_status = VCO_MAILBOX_OK;
    link.lost_ack = true;
    tick(VCO_MAILBOX_COPY);
    assert(transport.phase == VCO_TRANSPORT_ACK && transport.ticket == server_ticket);
    tick(VCO_MAILBOX_TICKET);
    assert(transport.phase == VCO_TRANSPORT_RESUME);
    tick(VCO_MAILBOX_EMPTY);
    assert(vco_transport_idle(&transport) && transport.client.known[0] && !writes);
    query();
}
static void recovery(void)
{
    VcoControlRequest old = old_request(100, VCO_CONTROL_SET);
    uint64_t ticket;
    assert(!vco_mailbox_submit(&mailbox, 88, &old, sizeof(old), &ticket));
    link.resume_status = VCO_MAILBOX_COPY;
    tick(VCO_MAILBOX_COPY);
    assert(transport.phase == VCO_TRANSPORT_RESUME && !transport.ticket);
    link.resume_status = VCO_MAILBOX_OK;
    tick(VCO_MAILBOX_OK);
    assert(transport.ticket == ticket && !transport.current);
    tick(VCO_MAILBOX_WAIT);
    pump();
    tick(VCO_MAILBOX_OK);
    assert(transport.phase == VCO_TRANSPORT_ACK && !transport.client.known[0]);
    assert(transport.client.sequence >= 100 && writes == 1);
    tick(VCO_MAILBOX_OK);
    assert(!vco_transport_set(&transport, 0, false));
    query();
    assert(transport.client.rows[0].state == VCO_ENTRY_ON && transport.client.sequence > 100);
    vco_transport_reconnect(&transport);
    assert(!transport.client.known[0] && !transport.client.waiting);
    ready();
    query();
    assert(transport.client.rows[0].state == VCO_ENTRY_ON && writes == 1);

    /* A restarted UI may find a rejection from an obsolete game session. */
    old = old_request(200, VCO_CONTROL_QUERY);
    old.session++;
    assert(!vco_mailbox_submit(&mailbox, 88, &old, sizeof(old), &ticket));
    pump();
    vco_transport_reconnect(&transport);
    tick(VCO_MAILBOX_OK);
    tick(VCO_MAILBOX_OK);
    assert(transport.client.bound && transport.client.session == session.token);
    tick(VCO_MAILBOX_OK);
    assert(!transport.client.known[0]);
    query();
}
static void rebind(void)
{
    ready();
    query();
    assert(vco_transport_set(&transport, 0, true));
    /* No callback ran yet: rebinding cannot execute the unsent SET. */
    assert(vco_transport_bind(&transport, session.token, engine.revision, 1));
    tick(VCO_MAILBOX_EMPTY);
    assert(!writes && !transport.client.known[0]);
    query();
    assert(vco_transport_set(&transport, 0, true));
    link.lost_submit = true;
    tick(VCO_MAILBOX_COPY);
    assert(vco_transport_bind(&transport, session.token, engine.revision + 1, 1));
    tick(VCO_MAILBOX_OK);
    uint64_t ticket = transport.ticket;
    pump();
    tick(VCO_MAILBOX_OK);
    assert(!transport.client.known[0] && transport.client.revision == engine.revision + 1);
    assert(vco_transport_bind(&transport, session.token, engine.revision, 1));
    assert(transport.phase == VCO_TRANSPORT_ACK && transport.ticket == ticket);
    link.ack_status = VCO_MAILBOX_BUSY;
    tick(VCO_MAILBOX_BUSY);
    link.ack_status = VCO_MAILBOX_OK;
    tick(VCO_MAILBOX_OK);
    assert(!transport.client.known[0] && !vco_transport_set(&transport, 0, false));
    query();
    assert(transport.client.rows[0].state == VCO_ENTRY_ON && writes == 1);

    /* Reconnect after receive but before ACK must recover, never resubmit. */
    assert(vco_transport_set(&transport, 0, false));
    tick(VCO_MAILBOX_OK);
    pump();
    tick(VCO_MAILBOX_OK);
    unsigned submissions = link.submits;
    vco_transport_reconnect(&transport);
    tick(VCO_MAILBOX_OK);
    tick(VCO_MAILBOX_OK);
    tick(VCO_MAILBOX_OK);
    assert(link.submits == submissions && !transport.client.known[0]);
    query();
    assert(transport.client.rows[0].state == VCO_ENTRY_OFF && writes == 1);
}
static void invalid_case(void)
{
    ready();
    assert(vco_transport_query(&transport, 0, 1));
    tick(VCO_MAILBOX_OK);
    pump();
    for (link.corrupt = 1; link.corrupt <= 6; ++link.corrupt) {
        tick(VCO_MAILBOX_ARGUMENT);
        assert(transport.invalid_reply && transport.phase == VCO_TRANSPORT_RECEIVE);
        assert(transport.client.waiting && !transport.client.known[0] && !link.acks);
    }
    link.corrupt = 0;
    tick(VCO_MAILBOX_OK);
    tick(VCO_MAILBOX_OK);
    assert(!transport.invalid_reply && transport.client.known[0]);

    VcoControlRequest old = old_request(100, VCO_CONTROL_QUERY);
    uint64_t ticket;
    assert(!vco_mailbox_submit(&mailbox, 88, &old, sizeof(old), &ticket));
    pump();
    vco_transport_reconnect(&transport);
    unsigned acks = link.acks;
    link.corrupt = 10;
    /* A malformed resumed request can be genuine server state, but it may
     * only be drained by a canonical PACKET rejection, never this snapshot. */
    tick(VCO_MAILBOX_OK);
    tick(VCO_MAILBOX_ARGUMENT);
    assert(transport.phase == VCO_TRANSPORT_RECEIVE && link.acks == acks);
    link.corrupt = 0;
    vco_transport_reconnect(&transport);
    tick(VCO_MAILBOX_OK);
    for (link.corrupt = 1; link.corrupt <= 6; ++link.corrupt) {
        tick(VCO_MAILBOX_ARGUMENT);
        assert(transport.invalid_reply && transport.phase == VCO_TRANSPORT_RECEIVE);
        assert(!transport.client.known[0] && link.acks == acks);
    }
    link.corrupt = 0;
    tick(VCO_MAILBOX_OK);
    tick(VCO_MAILBOX_OK);
    assert(!transport.client.known[0] && transport.client.bound && !writes);

    /* Canonical stale reply revokes current state instead of showing OFF/ON. */
    assert(vco_transport_bind(&transport, session.token, engine.revision + 1, 1));
    query();
    assert(!transport.client.bound && !transport.client.known[0]);
    assert(transport.client.last_status == VCO_CONTROL_STALE);

    /* Old UI versions may leave a syntactically malformed request queued.
     * A new UI must drain its real PACKET reply instead of permanently wedging
     * RESUME, and must not let malformed IDs exhaust its sequence counter. */
    for (unsigned malformed = 0; malformed < 12; ++malformed) {
        setup();
        VcoControlRequest bad = old_request(UINT64_MAX, VCO_CONTROL_QUERY);
        switch (malformed) {
        case 0:
            ++bad.abi;
            break;
        case 1:
            --bad.size;
            break;
        case 2:
            bad.reserved[0] = 1;
            break;
        case 3:
            bad.count = 0;
            break;
        case 4:
            bad.count = VCO_CONTROL_PAGE + 1;
            break;
        case 5:
            bad.request = 0;
            break;
        case 6:
            bad.session = 0;
            break;
        case 7:
            bad.revision = 0;
            break;
        case 8:
            bad.enabled = 2;
            break;
        case 9:
            bad.operation = VCO_CONTROL_SET;
            bad.count = 2;
            break;
        case 10:
            bad.operation = 0;
            break;
        case 11:
            bad.reserved[1] = 1;
            break;
        }
        assert(!vco_mailbox_submit(&mailbox, 88, &bad, sizeof(bad), &ticket));
        pump();
        tick(VCO_MAILBOX_OK);
        assert(transport.phase == VCO_TRANSPORT_RECEIVE && !transport.client.sequence);
        if (malformed == 0) {
            for (link.corrupt = 1; link.corrupt <= 16; ++link.corrupt) {
                if (link.corrupt == 10)
                    continue;
                tick(VCO_MAILBOX_ARGUMENT);
                assert(transport.invalid_reply && !link.acks && !transport.client.sequence &&
                       !transport.client.known[0]);
            }
            link.corrupt = 0;
        }
        tick(VCO_MAILBOX_OK);
        assert(transport.phase == VCO_TRANSPORT_ACK && !transport.client.sequence &&
               transport.client.bound && !transport.client.known[0]);
        tick(VCO_MAILBOX_OK);
        assert(vco_transport_idle(&transport) && !writes);
        query();
        assert(transport.client.sequence == 1 && transport.client.known[0]);
    }
}
static void bounded(void)
{
    const VcoMailboxStatus retryable[] = {VCO_MAILBOX_BUSY, VCO_MAILBOX_WAIT, VCO_MAILBOX_COPY};
    for (unsigned i = 0; i < sizeof(retryable) / sizeof(retryable[0]); ++i) {
        link.resume_status = retryable[i];
        for (unsigned frame = 0; frame < 500; ++frame)
            tick(retryable[i]);
        assert(transport.phase == VCO_TRANSPORT_RESUME && !transport.ticket);
    }
    link.resume_status = VCO_MAILBOX_OK;
    ready();
    assert(vco_transport_query(&transport, 0, 1));
    VcoControlRequest original = transport.request;
    for (unsigned i = 0; i < sizeof(retryable) / sizeof(retryable[0]); ++i) {
        link.submit_status = retryable[i];
        for (unsigned frame = 0; frame < 500; ++frame)
            tick(retryable[i]);
        assert(transport.phase == VCO_TRANSPORT_SUBMIT && transport.client.waiting);
        assert(!memcmp(&original, &transport.request, sizeof(original)));
    }
    link.submit_status = VCO_MAILBOX_OK;
    tick(VCO_MAILBOX_OK);
    uint64_t ticket = transport.ticket;
    pump();
    for (unsigned i = 0; i < sizeof(retryable) / sizeof(retryable[0]); ++i) {
        link.receive_status = retryable[i];
        for (unsigned frame = 0; frame < 500; ++frame)
            tick(retryable[i]);
        assert(transport.phase == VCO_TRANSPORT_RECEIVE && transport.ticket == ticket);
        assert(!transport.client.known[0]);
    }
    link.receive_status = VCO_MAILBOX_OK;
    tick(VCO_MAILBOX_OK);
    for (unsigned i = 0; i < sizeof(retryable) / sizeof(retryable[0]); ++i) {
        link.ack_status = retryable[i];
        for (unsigned frame = 0; frame < 500; ++frame)
            tick(retryable[i]);
        assert(transport.phase == VCO_TRANSPORT_ACK && transport.ticket == ticket);
    }
    link.ack_status = VCO_MAILBOX_OK;
    tick(VCO_MAILBOX_OK);
    unsigned calls = link.calls;
    tick(VCO_MAILBOX_OK);
    assert(link.calls == calls);
    assert(!vco_transport_bind(&transport, 0, 1, 1));
    assert(!vco_transport_query(&transport, UINT32_MAX, 1));
    assert(!vco_transport_set(&transport, UINT32_MAX, true));
    assert(vco_transport_tick(NULL) == VCO_MAILBOX_ARGUMENT);
    assert(!vco_transport_init(NULL, callbacks, &link));
    VcoTransportOps missing = callbacks;
    missing.ack = NULL;
    assert(!vco_transport_init(&transport, missing, &link));
    assert(vco_transport_tick(&transport) == VCO_MAILBOX_ARGUMENT);
    assert(!vco_transport_bind(&transport, session.token, engine.revision, 1));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "delivery"))
        delivery();
    else if (!strcmp(argv[1], "retries"))
        retries();
    else if (!strcmp(argv[1], "recovery"))
        recovery();
    else if (!strcmp(argv[1], "rebind"))
        rebind();
    else if (!strcmp(argv[1], "invalid"))
        invalid_case();
    else if (!strcmp(argv[1], "bounded"))
        bounded();
    else
        assert(!"unknown transport test");
    printf("transport %s passed; object %zu bytes\n", argv[1], sizeof(transport));
    return 0;
}
