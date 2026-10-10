#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include "execution/mailbox.h"
static VcoEngine engine;
static VcoSession session;
static VcoControlMailbox mailbox;
static VcoCheatEntry entries[2];
static VcoCode codes[2];
static VcoCheatOp ops[2];
static VcoPatch patches[2];
static char names[128];
static unsigned writes, releases;
static bool fail_release;
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
static int inject(void *context, uint64_t token, uint32_t address, const uint8_t *in, size_t size,
                  int32_t *handle)
{
    assert(alive(context, token) && address == 0x81000010u && in && size == 4);
    *handle = 100;
    return 0;
}
static int release(void *context, uint64_t token, int32_t handle)
{
    (void)context;
    assert(token == session.token && handle == 100);
    ++releases;
    return fail_release ? -1 : 0;
}
static void setup(void)
{
    writes = releases = 0;
    fail_release = false;
    VcoIdentity identity = {42, 3, "PCSB00031"};
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    vco_engine_init(&engine);
    VcoCheatFile file = {entries, 2, 0, codes, 2, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    const char text[] = "_V1 Data\n$0200 81000000 11223344\n_V0 Patch\n$A200 81000010 E3A00001\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    VcoCheatMemory memory = {0};
    memory.alive = alive;
    memory.write = write_memory;
    memory.inject = inject;
    memory.release = release;
    assert(!vco_engine_bind(&engine, &session, 1, &file, memory, ops, patches, 2));
    vco_mailbox_init(&mailbox, &engine, 88);
}
static VcoControlRequest request(uint64_t id, unsigned operation, unsigned entry, bool enabled)
{
    return (VcoControlRequest){id,
                               session.token,
                               engine.revision,
                               VCO_CONTROL_ABI,
                               sizeof(VcoControlRequest),
                               operation,
                               entry,
                               enabled,
                               1,
                               {0, 0}};
}
static void delivery(void)
{
    VcoControlRequest r = request(1, VCO_CONTROL_SET, 0, true), original = r;
    uint64_t ticket = 0, retry = 0;
    assert(vco_mailbox_submit(&mailbox, 1, &r, sizeof(r), &ticket) == VCO_MAILBOX_OWNER && !ticket);
    assert(vco_mailbox_submit(&mailbox, 88, &r, sizeof(r) - 1, &ticket) == VCO_MAILBOX_ARGUMENT);
    assert(!vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket));
    r.enabled = false; /* submitted bytes are owned by the mailbox */
    assert(vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &retry) == VCO_MAILBOX_BUSY && !retry);
    VcoControlReply reply, again;
    memset(&reply, 0xa5, sizeof(reply));
    again = reply;
    assert(vco_mailbox_receive(&mailbox, 88, ticket, &reply) == VCO_MAILBOX_WAIT);
    assert(!memcmp(&reply, &again, sizeof(reply)));
    assert(vco_mailbox_ack(&mailbox, 88, ticket) == VCO_MAILBOX_WAIT);
    assert(vco_mailbox_pump(&mailbox, 1) && writes == 1);
    assert(!vco_mailbox_submit(&mailbox, 88, &original, sizeof(original), &retry) &&
           retry == ticket);
    assert(!vco_mailbox_receive(&mailbox, 88, ticket, &reply));
    assert(reply.rows[0].state == VCO_ENTRY_ENABLING && reply.rows[0].pending == 1);
    assert(!vco_mailbox_receive(&mailbox, 88, ticket, &again) &&
           !memcmp(&reply, &again, sizeof(reply)));
    assert(vco_mailbox_pump(&mailbox, 50001) &&
           writes == 2); /* unread reply cannot stall freezing */
    assert(!vco_mailbox_ack(&mailbox, 88, ticket));
    assert(vco_mailbox_ack(&mailbox, 88, ticket) == VCO_MAILBOX_TICKET);
    r = request(2, VCO_CONTROL_QUERY, 0, false);
    assert(!vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &retry) && retry != ticket);
    assert(vco_mailbox_ack(&mailbox, 88, ticket) == VCO_MAILBOX_TICKET);
    assert(vco_mailbox_pump(&mailbox, 50002));
    assert(!vco_mailbox_receive(&mailbox, 88, retry, &reply));
    assert(reply.rows[0].state == VCO_ENTRY_ON && reply.rows[0].acknowledged == 1);
    assert(!vco_mailbox_ack(&mailbox, 88, retry));
    VcoIdentity identity = session.identity;
    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    r.request = 3;
    assert(!vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket));
    assert(vco_mailbox_pump(&mailbox, 50003));
    assert(!vco_mailbox_receive(&mailbox, 88, ticket, &reply));
    assert(reply.rows[0].state == VCO_ENTRY_PAUSED && !(reply.flags & VCO_CONTROL_FOREGROUND));
    assert(!vco_mailbox_ack(&mailbox, 88, ticket));
    /* Invalid content gets a worker rejection reply, never engine side effects. */
    r.reserved[0] = 1;
    assert(!vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket));
    assert(vco_mailbox_pump(&mailbox, 50003));
    assert(!vco_mailbox_receive(&mailbox, 88, ticket, &reply) &&
           reply.status == VCO_CONTROL_PACKET);
}
static void reconnect(void)
{
    uint64_t ticket = 0, recovered = 0;
    VcoControlRequest old = request(100, VCO_CONTROL_SET, 0, true), copy;
    assert(vco_mailbox_resume(&mailbox, 88, &copy, &recovered) == VCO_MAILBOX_EMPTY);
    assert(!vco_mailbox_submit(&mailbox, 88, &old, sizeof(old), &ticket));
    /* UI disappears after submit before receiving its ticket. Recovery must not
     * execute a second SET or discard the first request. */
    assert(vco_mailbox_resume(&mailbox, 77, &copy, &recovered) == VCO_MAILBOX_OWNER);
    assert(!vco_mailbox_resume(&mailbox, 88, &copy, &recovered) && recovered == ticket);
    assert(!memcmp(&copy, &old, sizeof(copy)));
    assert(vco_mailbox_pump(&mailbox, 1) && writes == 1);
    assert(!vco_mailbox_resume(&mailbox, 88, &copy, &recovered));
    VcoControlReply reply;
    assert(!vco_mailbox_receive(&mailbox, 88, recovered, &reply) && reply.request == 100);
    assert(!vco_mailbox_ack(&mailbox, 88, recovered));
    VcoControlRequest query = request(1, VCO_CONTROL_QUERY, 0, false);
    assert(!vco_mailbox_submit(&mailbox, 88, &query, sizeof(query), &ticket));
    assert(vco_mailbox_pump(&mailbox, 2));
    assert(!vco_mailbox_receive(&mailbox, 88, ticket, &reply));
    assert(reply.rows[0].state == VCO_ENTRY_ON && reply.rows[0].acknowledged == 100 && writes == 1);
}
static void shutdown_case(void)
{
    VcoControlRequest r = request(1, VCO_CONTROL_SET, 0, true);
    uint64_t ticket;
    VcoError error = VCO_E_ARGUMENT;
    assert(!vco_mailbox_stop_result(&mailbox, &error) && error == VCO_E_ARGUMENT);
    assert(!vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket));
    vco_mailbox_stop(&mailbox);
    assert(vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket) == VCO_MAILBOX_CLOSED);
    assert(!vco_mailbox_pump(&mailbox, 1) && !writes && !engine.bound);
    assert(vco_mailbox_stop_result(&mailbox, &error) && error == VCO_OK);
    assert(!vco_mailbox_pump(&mailbox, 100001));
    setup();
    r = request(1, VCO_CONTROL_SET, 1, true);
    assert(!vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket));
    assert(vco_mailbox_pump(&mailbox, 1));
    fail_release = true;
    vco_mailbox_stop(&mailbox);
    assert(!vco_mailbox_pump(&mailbox, 2));
    assert(vco_mailbox_stop_result(&mailbox, &error) && error == VCO_E_RESTORE && releases == 1);
    assert(!vco_mailbox_pump(&mailbox, 3) && releases == 1);
}
static void bounds(void)
{
    VcoControlRequest r = request(1, VCO_CONTROL_QUERY, 0, false);
    uint64_t ticket = 42;
    mailbox.gate = 1; /* deterministically simulate another short-copy operation */
    assert(vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket) == VCO_MAILBOX_BUSY &&
           ticket == 42);
    assert(vco_mailbox_pump(&mailbox, 1));
    mailbox.gate = 0;
    mailbox.sequence = UINT64_MAX;
    assert(vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket) == VCO_MAILBOX_EXHAUSTED);
    assert(!writes && ticket == 42);
    assert(vco_mailbox_submit(NULL, 88, &r, sizeof(r), &ticket) == VCO_MAILBOX_ARGUMENT);
}
static void *worker(void *unused)
{
    (void)unused;
    for (uint64_t tick = 1; vco_mailbox_pump(&mailbox, tick); tick += 1000)
        sched_yield();
    return NULL;
}
static void concurrent(void)
{
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, worker, NULL));
    for (uint64_t id = 1; id <= 2000; ++id) {
        VcoControlRequest r = request(id, VCO_CONTROL_QUERY, 0, false);
        uint64_t ticket = 0;
        VcoMailboxStatus status;
        unsigned tries = 0;
        do {
            status = vco_mailbox_submit(&mailbox, 88, &r, sizeof(r), &ticket);
            sched_yield();
            assert(++tries < 1000000);
        } while (status == VCO_MAILBOX_BUSY);
        assert(status == VCO_MAILBOX_OK);
        VcoControlRequest recovered;
        uint64_t recovered_ticket = 0;
        tries = 0;
        do {
            status = vco_mailbox_resume(&mailbox, 88, &recovered, &recovered_ticket);
            sched_yield();
            assert(++tries < 1000000);
        } while (status == VCO_MAILBOX_BUSY);
        assert(status == VCO_MAILBOX_OK && recovered_ticket == ticket &&
               !memcmp(&recovered, &r, sizeof(r)));
        VcoControlReply reply;
        tries = 0;
        do {
            status = vco_mailbox_receive(&mailbox, 88, ticket, &reply);
            sched_yield();
            assert(++tries < 1000000);
        } while (status == VCO_MAILBOX_BUSY || status == VCO_MAILBOX_WAIT);
        assert(status == VCO_MAILBOX_OK && reply.request == id && !reply.status);
        assert(reply.rows[0].state == VCO_ENTRY_OFF);
        tries = 0;
        do {
            status = vco_mailbox_ack(&mailbox, 88, ticket);
            sched_yield();
            assert(++tries < 1000000);
        } while (status == VCO_MAILBOX_BUSY);
        assert(status == VCO_MAILBOX_OK);
    }
    vco_mailbox_stop(&mailbox);
    assert(!pthread_join(thread, NULL));
    VcoError error;
    assert(vco_mailbox_stop_result(&mailbox, &error) && !error && !writes);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "delivery"))
        delivery();
    else if (!strcmp(argv[1], "reconnect"))
        reconnect();
    else if (!strcmp(argv[1], "shutdown"))
        shutdown_case();
    else if (!strcmp(argv[1], "bounds"))
        bounds();
    else if (!strcmp(argv[1], "concurrent"))
        concurrent();
    else
        assert(0);
    return 0;
}
