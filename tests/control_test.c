#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "execution/control.h"
#include "execution/client.h"
static VcoSession session;
static VcoIdentity identity = {42, 12, "PCSB00031"};
static VcoEngine engine;
static VcoCheatEntry entries[20];
static VcoCode codes[20];
static VcoCheatOp ops[20];
static VcoPatch patches[20];
static char names[512];
static unsigned writes;
static bool fail_write;
static int alive(void *ctx, uint64_t token)
{
    assert(ctx == &session);
    return vco_session_matches(&session, token, true);
}
static int write_data(void *ctx, uint64_t token, uint32_t addr, const uint8_t *data, size_t size)
{
    assert(alive(ctx, token) && addr >= 0x81000000u && data && size == 4);
    ++writes;
    return fail_write ? -1 : 0;
}
static void setup(void)
{
    char text[2048];
    size_t used = 0;
    writes = 0;
    fail_write = false;
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    vco_engine_init(&engine);
    for (unsigned i = 0; i < 18; ++i)
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "_V1 Row%u\n$0200 %08X 00000001\n", i, 0x81000000u + i * 4);
    VcoCheatFile file = {entries, 20, 0, codes, 20, 0, names, sizeof(names), 0};
    VcoDiagnostic diag;
    assert(!vco_cheat_parse(text, used, &file, &diag));
    VcoCheatMemory memory = {0};
    memory.context = &session;
    memory.alive = alive;
    memory.write = write_data;
    assert(!vco_engine_bind(&engine, &session, 7, &file, memory, ops, patches, 20));
}
static VcoControlRequest request(unsigned operation, uint64_t sequence, unsigned entry,
                                 unsigned count)
{
    return (VcoControlRequest){sequence,
                               session.token,
                               engine.revision,
                               VCO_CONTROL_ABI,
                               sizeof(VcoControlRequest),
                               operation,
                               entry,
                               operation == VCO_CONTROL_SET,
                               count,
                               {0, 0}};
}
static VcoControlReply reply;
static VcoControlStatus dispatch(VcoControlRequest *r)
{
    return vco_control_dispatch(&engine, 88, 88, r, sizeof(*r), &reply);
}
static void packets(void)
{
    VcoControlRequest r = request(VCO_CONTROL_QUERY, 1, 0, 16);
    assert(!dispatch(&r) && reply.total == 18 && reply.count == 16);
    assert(reply.rows[0].state == VCO_ENTRY_OFF && !reply.rows[0].desired && !writes);
    for (size_t size = 0; size < sizeof(r) + 2; ++size) {
        if (size == sizeof(r))
            continue;
        assert(vco_control_dispatch(&engine, 88, 88, &r, size, &reply) == VCO_CONTROL_PACKET);
        assert(!reply.count && !reply.total && !reply.session);
    }
    assert(vco_control_dispatch(&engine, 88, 42, &r, sizeof(r), &reply) == VCO_CONTROL_OWNER);
    assert(!reply.total && !reply.session && reply.request == 1);
    assert(vco_control_dispatch(&engine, 0, 0, &r, sizeof(r), &reply) == VCO_CONTROL_OWNER);
    assert(vco_control_dispatch(&engine, 88, 88, NULL, sizeof(r), &reply) == VCO_CONTROL_PACKET);
    assert(vco_control_dispatch(&engine, 88, 88, &r, sizeof(r), NULL) == VCO_CONTROL_PACKET);
    uint8_t unaligned[sizeof(r) + 1];
    memcpy(unaligned + 1, &r, sizeof(r));
    assert(!vco_control_dispatch(&engine, 88, 88, unaligned + 1, sizeof(r), &reply));
    memcpy(&reply, &r, sizeof(r));
    assert(!vco_control_dispatch(&engine, 88, 88, &reply, sizeof(r), &reply));
    assert(reply.request == r.request && reply.count == 16 && !writes);
    VcoControlRequest invalid[11];
    for (unsigned i = 0; i < 11; ++i)
        invalid[i] = r;
    invalid[0].abi++;
    invalid[1].size--;
    invalid[2].request = 0;
    invalid[3].reserved[0] = 1;
    invalid[4].reserved[1] = 1;
    invalid[5].operation = 99;
    invalid[6].enabled = 1;
    invalid[7].count = 17;
    invalid[8].count = 0;
    invalid[9].entry = UINT32_MAX;
    invalid[10].entry = 3;
    for (unsigned i = 0; i < 11; ++i)
        assert(dispatch(&invalid[i]) == VCO_CONTROL_PACKET);
    r.entry = 16;
    r.count = 2;
    assert(!dispatch(&r) && reply.first == 16 && reply.count == 2);
    assert(!reply.rows[2].state && !reply.rows[2].acknowledged);
    r.session++;
    assert(dispatch(&r) == VCO_CONTROL_STALE && !reply.total);
    r.session--;
    r.revision++;
    assert(dispatch(&r) == VCO_CONTROL_STALE);
    r.revision--;
    vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    assert(dispatch(&r) == VCO_CONTROL_UNBOUND);
    assert(!writes && !engine.queued);
}
static void commands(void)
{
    VcoControlRequest r = request(VCO_CONTROL_SET, 1, 0, 1);
    assert(!dispatch(&r) && !writes && engine.queued == 1);
    assert(reply.rows[0].state == VCO_ENTRY_ENABLING && reply.rows[0].pending == 1);
    assert(!dispatch(&r) && engine.queued == 1);
    r.enabled = 0;
    assert(dispatch(&r) == VCO_CONTROL_REJECTED && reply.error == VCO_E_ARGUMENT);
    r.enabled = 2;
    assert(dispatch(&r) == VCO_CONTROL_PACKET);
    r.enabled = 1;
    r.request = 2;
    assert(dispatch(&r) == VCO_CONTROL_REJECTED && reply.error == VCO_E_LIMIT);
    vco_engine_step(&engine, 1);
    r = request(VCO_CONTROL_QUERY, 3, 0, 1);
    assert(!dispatch(&r) && reply.rows[0].state == VCO_ENTRY_ON && reply.rows[0].acknowledged == 1);
    assert(!reply.rows[0].pending && writes == 1);
    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    vco_engine_step(&engine, 50001);
    assert(!dispatch(&r) && reply.rows[0].state == VCO_ENTRY_PAUSED &&
           !(reply.flags & VCO_CONTROL_FOREGROUND));
    r = request(VCO_CONTROL_SET, 4, 0, 1);
    r.enabled = 0;
    assert(!dispatch(&r));
    vco_engine_step(&engine, 100001);
    r.operation = VCO_CONTROL_QUERY;
    assert(!dispatch(&r) && reply.rows[0].state == VCO_ENTRY_OFF && writes == 1);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    r = request(VCO_CONTROL_SET, 5, 0, 1);
    fail_write = true;
    assert(!dispatch(&r));
    vco_engine_step(&engine, 150001);
    r = request(VCO_CONTROL_QUERY, 6, 0, 1);
    assert(!dispatch(&r) && reply.rows[0].state == VCO_ENTRY_ERROR && reply.rows[0].line);
    engine.blocked_restore = true;
    assert(!dispatch(&r) && (reply.flags & VCO_CONTROL_BLOCKED));
}
static void queue(void)
{
    for (unsigned i = 0; i < VCO_ENGINE_QUEUE; ++i) {
        VcoControlRequest r = request(VCO_CONTROL_SET, i + 1, i, 1);
        assert(!dispatch(&r));
    }
    VcoControlRequest r = request(VCO_CONTROL_SET, 17, 16, 1);
    assert(dispatch(&r) == VCO_CONTROL_REJECTED && reply.error == VCO_E_LIMIT);
    assert(!writes && engine.queued == VCO_ENGINE_QUEUE);
    vco_engine_step(&engine, 1);
    assert(writes == 16);
    assert(!dispatch(&r));
    vco_engine_step(&engine, 2);
    r = request(VCO_CONTROL_QUERY, 18, 16, 1);
    assert(!dispatch(&r) && reply.rows[0].state == VCO_ENTRY_ERROR &&
           reply.rows[0].error == VCO_E_LIMIT);
}
static void client(void)
{
    static VcoControlClient c;
    vco_control_client_init(&c);
    assert(vco_control_client_bind(&c, session.token, engine.revision, 18));
    VcoControlRequest r, retry;
    assert(!vco_control_client_set(&c, 0, true, &r));
    assert(vco_control_client_query(&c, 0, 6, &r));
    assert(!vco_control_client_query(&c, 6, 6, &retry));
    assert(!dispatch(&r));
    VcoControlReply old = reply;
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)) && c.known[0] && !c.known[6]);
    assert(vco_control_client_set(&c, 0, true, &r));
    assert(c.rows[0].state == VCO_ENTRY_OFF); /* never optimistic ON */
    assert(vco_control_client_retry(&c, &retry) && !memcmp(&r, &retry, sizeof(r)));
    assert(!vco_control_client_accept(&c, &old, sizeof(old)) && c.waiting);
    assert(!dispatch(&r));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(c.rows[0].state == VCO_ENTRY_ENABLING);
    assert(!vco_control_client_set(&c, 0, false, &r));
    vco_engine_step(&engine, 1);
    assert(vco_control_client_query(&c, 0, 6, &r));
    assert(!dispatch(&r));
    VcoControlReply bad = reply;
    bad.rows[5].state = 99;
    assert(!vco_control_client_accept(&c, &bad, sizeof(bad)) &&
           c.rows[0].state == VCO_ENTRY_ENABLING);
    bad = reply;
    bad.total++;
    assert(!vco_control_client_accept(&c, &bad, sizeof(bad)));
    bad = reply;
    bad.count = UINT32_MAX;
    assert(!vco_control_client_accept(&c, &bad, sizeof(bad)));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)) && c.rows[0].state == VCO_ENTRY_ON);
    old = reply;
    assert(vco_control_client_query(&c, 0, 6, &r));
    assert(!dispatch(&r));
    bad = reply;
    bad.rows[0].state = VCO_ENTRY_OFF;
    assert(!vco_control_client_accept(&c, &bad, sizeof(bad)));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(vco_control_client_query(&c, 0, 6, &r));
    uint64_t sequence = c.sequence;
    assert(vco_control_client_bind(&c, session.token + 1, engine.revision + 1, 18));
    assert(c.sequence == sequence && !c.known[0]);
    assert(vco_control_client_query(&c, 0, 6, &r));
    assert(!vco_control_client_accept(&c, &old, sizeof(old)));
    assert(dispatch(&r) == VCO_CONTROL_STALE);
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)) && !c.bound);
    assert(vco_control_client_bind(&c, session.token, engine.revision, 18));
    c.sequence = UINT64_MAX;
    assert(!vco_control_client_query(&c, 0, 6, &r));
}
static void malformed_reply(VcoControlClient *c, const VcoControlReply *bad)
{
    static VcoControlClient before;
    memcpy(&before, c, sizeof(before));
    assert(!vco_control_client_accept(c, bad, sizeof(*bad)));
    assert(!memcmp(&before, c, sizeof(before)));
}
static void client_replies(void)
{
    static VcoControlClient c;
    vco_control_client_init(&c);
    assert(vco_control_client_bind(&c, session.token, engine.revision, 18));
    VcoControlRequest r;
    assert(vco_control_client_query(&c, 0, 2, &r));
    assert(!dispatch(&r));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(vco_control_client_set(&c, 0, true, &r));
    assert(!dispatch(&r));
    VcoControlReply bad = reply;
    bad.rows[0].pending++;
    malformed_reply(&c, &bad); /* OK must acknowledge this exact SET. */
    bad = reply;
    bad.rows[0].state = VCO_ENTRY_DISABLING;
    malformed_reply(&c, &bad);
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(vco_control_client_query(&c, 0, 2, &r));
    assert(!dispatch(&r));
    bad = reply;
    memset(&bad.rows[0], 0, sizeof(bad.rows[0]));
    malformed_reply(&c, &bad); /* Pending work cannot disappear without an acknowledgment. */
    vco_engine_step(&engine, 1);
    assert(!dispatch(&r));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(vco_control_client_query(&c, 0, 2, &r));
    assert(!dispatch(&r));
    bad = reply;
    bad.rows[0].acknowledged--;
    malformed_reply(&c, &bad);
    bad = reply;
    bad.rows[0].error = VCO_E_IO;
    malformed_reply(&c, &bad);
    bad = reply;
    bad.rows[0].line = 1;
    malformed_reply(&c, &bad);
    bad = reply;
    bad.rows[2].pending = UINT64_MAX;
    malformed_reply(&c, &bad); /* Unused rows must not influence the sequence watermark. */
    bad = reply;
    bad.status = VCO_CONTROL_REJECTED;
    bad.error = VCO_E_LIMIT;
    malformed_reply(&c, &bad); /* QUERY has no execution rejection. */
    bad = reply;
    bad.status = VCO_CONTROL_STALE;
    malformed_reply(&c, &bad); /* A populated snapshot is not a canonical rejection. */
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(vco_control_client_query(&c, 0, 2, &r));
    r.session++;
    assert(dispatch(&r) == VCO_CONTROL_STALE);
    for (unsigned i = 0; i < 9; ++i) {
        bad = reply;
        switch (i) {
        case 0:
            bad.session = 1;
            break;
        case 1:
            bad.revision = 1;
            break;
        case 2:
            bad.error = VCO_E_SESSION;
            break;
        case 3:
            bad.first = 1;
            break;
        case 4:
            bad.count = 1;
            break;
        case 5:
            bad.total = 1;
            break;
        case 6:
            bad.flags = VCO_CONTROL_FOREGROUND;
            break;
        case 7:
            bad.rows[0].desired = 1;
            break;
        case 8:
            bad.rows[VCO_CONTROL_PAGE - 1].line = 1;
            break;
        }
        malformed_reply(&c, &bad);
    }
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)) && !c.bound);
}
static void client_reconnect(void)
{
    /* The kernel remains alive when a UI client is recreated. Discovering a row
     * must advance the new client's sequence past commands from the old one. */
    VcoControlRequest r = request(VCO_CONTROL_SET, 100, 0, 1);
    assert(!dispatch(&r));
    static VcoControlClient c;
    vco_control_client_init(&c);
    assert(vco_control_client_bind(&c, session.token, engine.revision, 18));
    assert(vco_control_client_query(&c, 0, 1, &r) && r.request == 1);
    assert(!dispatch(&r));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)) && c.sequence == 100);
    assert(!vco_control_client_set(&c, 0, false, &r));
    vco_engine_step(&engine, 1);
    assert(vco_control_client_query(&c, 0, 1, &r) && r.request == 101);
    assert(!dispatch(&r));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(vco_control_client_set(&c, 0, false, &r) && r.request == 102);
    assert(!dispatch(&r));
    /* Retrying after execution has completed returns a settled acknowledgment. */
    vco_engine_step(&engine, 2);
    assert(!dispatch(&r));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)));
    assert(c.rows[0].state == VCO_ENTRY_OFF && c.rows[0].acknowledged == 102);
    r = request(VCO_CONTROL_SET, UINT64_MAX, 1, 1);
    assert(!dispatch(&r));
    vco_engine_step(&engine, 3);
    vco_control_client_init(&c);
    assert(vco_control_client_bind(&c, session.token, engine.revision, 18));
    assert(vco_control_client_query(&c, 1, 1, &r));
    assert(!dispatch(&r));
    assert(vco_control_client_accept(&c, &reply, sizeof(reply)) && c.sequence == UINT64_MAX);
    assert(!vco_control_client_set(&c, 1, false, &r)); /* Never wrap the request sequence. */
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "packets"))
        packets();
    else if (!strcmp(argv[1], "commands"))
        commands();
    else if (!strcmp(argv[1], "queue"))
        queue();
    else if (!strcmp(argv[1], "client")) {
        client();
        setup();
        client_replies();
        setup();
        client_reconnect();
    } else
        assert(0);
    return 0;
}
