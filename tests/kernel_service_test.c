#include "native_service/fakes.h"
#include "platform/vita/service.h"
#include "platform/vita/close_service.h"
#include "platform/vita/authority.h"
#include "platform/vita/service_trace.h"
#include "platform/vita/patch_trace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static size_t packet_size;
static VcoUploadHeader packet_header;
static VcoServiceStatus status(void)
{
    assert(!vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)));
    VcoServiceStatus result;
    memcpy(&result, fake.user + TEST_STATUS, sizeof(result));
    assert(result.abi == VCO_SERVICE_ABI && result.size == sizeof(result) && !result.reserved &&
           !result.padding[0] && !result.padding[1]);
    return result;
}
static void observe(int32_t pid, uint32_t kind)
{
    VcoForegroundSample sample = service_fake_sample(pid, kind);
    memcpy(fake.user + TEST_SAMPLE, &sample, sizeof(sample));
    assert(!vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(sample)));
}
static VcoServiceStatus game(int32_t pid)
{
    observe(pid, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    VcoServiceStatus s = status();
    assert(s.session && s.pid == pid && s.state == VCO_SERVICE_LOADING);
    return s;
}
static void packet(uint64_t request, int32_t pid)
{
    VcoCode code = {0x0200, 0x81001000, 123, 1};
    VcoCheatEntry entry = {0, 0, 1, 1, {0, 0}};
    VcoCheatFile file = {&entry, 1, 1, &code, 1, 1, NULL, 0, 0};
    const char *id = pid == TEST_GAME ? "PCSB00031" : "PCSB00032";
    uint64_t target_session = status().session;
    assert(!vco_upload_encode(fake.user, TEST_SAMPLE, request, target_session ? target_session : 1,
                              (uint32_t)request, pid, id, &file, &packet_size));
    memcpy(&packet_header, fake.user, sizeof(packet_header));
}
static int load(void)
{
    return vcoServiceLoad(TEST_USER(TEST_PACKET), (uint32_t)packet_size);
}
static VcoServiceStatus bind_list(uint64_t request, int32_t pid)
{
    packet(request, pid);
    assert(!load());
    service_fake_tick();
    VcoServiceStatus s = status();
    assert(s.load_request == request && s.content_key == packet_header.content_key && s.revision &&
           s.count == 1 && s.catalog_revision == request && s.state == VCO_SERVICE_READY);
    return s;
}
static VcoControlRequest request(VcoServiceStatus s, uint64_t number, bool enabled)
{
    return (VcoControlRequest){
        number, s.session, s.revision, VCO_CONTROL_ABI, sizeof(VcoControlRequest), VCO_CONTROL_SET,
        0,      enabled,   1,          {0, 0}};
}
static int submit(VcoControlRequest r)
{
    memcpy(fake.user + TEST_REQUEST, &r, sizeof(r));
    return vcoServiceSubmit(TEST_USER(TEST_REQUEST), sizeof(r), TEST_USER(TEST_TICKET));
}
static uint64_t ticket(void)
{
    uint64_t value;
    memcpy(&value, fake.user + TEST_TICKET, sizeof(value));
    return value;
}
static VcoControlReply receive(uint64_t t)
{
    assert(!vcoServiceReceive(t, TEST_USER(TEST_REPLY), sizeof(VcoControlReply)));
    VcoControlReply reply;
    memcpy(&reply, fake.user + TEST_REPLY, sizeof(reply));
    assert(!vcoServiceAck(t));
    return reply;
}
static VcoControlReply command(VcoServiceStatus s, uint64_t number, bool enabled)
{
    assert(!submit(request(s, number, enabled)));
    service_fake_tick();
    return receive(ticket());
}
static void authority_pause(void)
{
    game(TEST_GAME);
    VcoServiceStatus s = bind_list(1, TEST_GAME);
    fake.expire_during_write = true;
    VcoControlReply reply = command(s, 1, true);
    assert(reply.status == VCO_CONTROL_OK);
    VcoControlRequest query = request(s, 100, false);
    query.operation = VCO_CONTROL_QUERY;
    assert(!submit(query));
    service_fake_tick();
    reply = receive(ticket());
    assert(reply.status == VCO_CONTROL_OK && reply.rows[0].state == VCO_ENTRY_PAUSED &&
           reply.rows[0].desired && reply.rows[0].error == VCO_OK);
    assert(fake.writes == 1 && fake.value == 123);
    service_fake_tick();
    assert(fake.writes == 1);
    fake.expire_during_write = false;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    s = status();
    assert(s.session == reply.session && s.state == VCO_SERVICE_READY && fake.writes == 2);
    reply = command(s, 2, false);
    assert(reply.status == VCO_CONTROL_OK);
    query.request++;
    assert(!submit(query));
    service_fake_tick();
    reply = receive(ticket());
    assert(reply.status == VCO_CONTROL_OK && reply.rows[0].state == VCO_ENTRY_OFF);
    fake.now += VCO_ENGINE_PERIOD_US;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.writes == 2);
}
static void long_observation(unsigned mode)
{
    game(TEST_GAME);
    VcoCode codes[10];
    for (unsigned i = 0; i < 10; ++i)
        codes[i] = (VcoCode){0x0200, 0x81001000, i, i + 1};
    VcoCheatEntry entry = {0, 0, 10, 1, {0, 0}};
    VcoCheatFile file = {&entry, 1, 1, codes, 10, 10, NULL, 0, 0};
    VcoServiceStatus s = status();
    assert(!vco_upload_encode(fake.user, TEST_SAMPLE, 1, s.session, 1, TEST_GAME, "PCSB00031",
                              &file, &packet_size));
    assert(!load());
    service_fake_tick();
    s = status();
    assert(s.state == VCO_SERVICE_READY);
    fake.write_delay_us = 100000;
    fake.observe_during_write = mode;
    int64_t started = fake.now;
    assert(command(s, 1, true).status == VCO_CONTROL_OK);
    assert(fake.writes == (mode == 1 ? 10u : mode == 3 ? 1u : 3u));
    if (mode == 1)
        assert((uint64_t)(fake.now - started) > VCO_VITA_AUTHORITY_LEASE_US && fake.value == 9);
    VcoControlRequest query = request(s, 100, false);
    query.operation = VCO_CONTROL_QUERY;
    assert(!submit(query));
    service_fake_tick();
    VcoControlReply reply = receive(ticket());
    assert(reply.status == VCO_CONTROL_OK && reply.rows[0].desired &&
           reply.rows[0].state == (mode == 1 ? VCO_ENTRY_ON : VCO_ENTRY_PAUSED) &&
           reply.rows[0].error == VCO_OK);
    assert(fake.writes == (mode == 1 ? 20u : mode == 3 ? 1u : 3u));
    /* OFF is still consumed before another periodic write, including while
     * the last sample is background/unknown/expired or never refreshed. */
    assert(command(s, 2, false).status == VCO_CONTROL_OK);
    fake.write_delay_us = fake.observe_during_write = 0;
    unsigned stopped = fake.writes;
    fake.now += VCO_ENGINE_PERIOD_US;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.writes == stopped);
}
static void authorization(void)
{
    fake.caller = 77;
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) == VCO_MAILBOX_OWNER);
    assert(!fake.allocations && !fake.from_calls && !fake.to_calls);
    assert(vco_service_authorize_caller(77) < 0);
    fake.caller = TEST_SHELL;
    assert(status().state == VCO_SERVICE_STARTING);
    assert(fake.refs[2] == 1 && fake.live_blocks == 1);
    fake.caller = 77;
    assert(vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(VcoForegroundSample)) ==
           VCO_MAILBOX_OWNER);
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_OWNER);
    VcoControlRequest r = {0};
    assert(submit(r) == VCO_MAILBOX_OWNER);
    assert(vcoServiceAck(1) == VCO_MAILBOX_OWNER);
    assert(vcoServiceResume(TEST_USER(TEST_REPLY), 64) == VCO_MAILBOX_OWNER);
    fake.caller = TEST_SHELL;
    VcoForegroundSample sample = service_fake_sample(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    memcpy(fake.user + TEST_SAMPLE, &sample, sizeof(sample));
    fake.revoke_owner_call = fake.from_calls + 1;
    assert(vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(sample)) == VCO_MAILBOX_OWNER);
    fake.owner_replaced = false;
    service_fake_tick();
    assert(status().state == VCO_SERVICE_NO_GAME && !fake.binds);
}
static void buffers(void)
{
    assert(status().state == VCO_SERVICE_STARTING);
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), 79) == VCO_MAILBOX_ARGUMENT);
    assert(vcoServiceStatus((void *)(uintptr_t)0xfffffff0u, 80) == VCO_MAILBOX_ARGUMENT);
    assert(vcoServiceObserve((void *)0x1000, sizeof(VcoForegroundSample)) == VCO_MAILBOX_ARGUMENT);
    assert(vcoServiceObserve(TEST_USER(TEST_SAMPLE), 47) == VCO_MAILBOX_ARGUMENT);
    game(TEST_GAME);
    packet(1, TEST_GAME);
    assert(vcoServiceLoad(TEST_USER(0), (uint32_t)VCO_UPLOAD_MAX + 1) == VCO_MAILBOX_ARGUMENT);
    assert(vcoServiceLoad(NULL, 1) == VCO_MAILBOX_ARGUMENT);
    assert(vcoServiceLoad(TEST_USER(0), 0) == VCO_MAILBOX_ARGUMENT);
    fake.fail_from_call = fake.from_calls + 1;
    assert(load() == VCO_MAILBOX_COPY);
    fake.fail_from_call = fake.from_calls + 2;
    assert(load() == VCO_MAILBOX_COPY); /* partial full-packet copy */
    fake.fail_from_call = 0;
    fake.mutate_header_call = fake.from_calls + 2;
    assert(load() == VCO_MAILBOX_ARGUMENT); /* envelope changed during full copy */
    fake.mutate_header_call = 0;
    packet(1, TEST_GAME);
    fake.revoke_owner_call = fake.from_calls + 2;
    assert(load() == VCO_MAILBOX_OWNER);
    fake.owner_replaced = false;
    service_fake_tick();
    assert(!status().load_request && fake.live_blocks == 1);
    fake.fail_to = true;
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) == VCO_MAILBOX_COPY);
    fake.fail_to = false;
    game(TEST_GAME);
    bind_list(1, TEST_GAME);
    /* Malformed observations are copied then rejected by the real authority. */
    VcoForegroundSample sample = service_fake_sample(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    sample.reserved[5] = 1;
    memcpy(fake.user + TEST_SAMPLE, &sample, sizeof(sample));
    assert(!vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(sample)));
    service_fake_tick();
    assert(command(status(), 1, true).status == VCO_CONTROL_REJECTED);
    assert(!fake.writes);
}
static void pipeline(void)
{
    VcoServiceStatus s = game(TEST_GAME);
    packet(1, TEST_GAME);
    assert(!load() && load() == VCO_MAILBOX_BUSY);
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_BUSY);
    /* All storage used after admission belongs to the kernel copy. */
    memset(fake.user, 0xcd, packet_size);
    service_fake_tick();
    s = status();
    assert(s.revision == 1 && s.count == 1 && !fake.writes && fake.live_blocks == 2);
    assert(fake.module_maps == 1 && fake.modules_snapshots == 1);
    fake.fail_to = true;
    assert(submit(request(s, 1, true)) == VCO_MAILBOX_COPY);
    assert(submit(request(s, 1, true)) == VCO_MAILBOX_COPY);
    fake.fail_to = false;
    assert(!submit(request(s, 1, true)) && ticket() == 1);
    assert(vcoServiceReceive(1, TEST_USER(TEST_REPLY), sizeof(VcoControlReply)) ==
           VCO_MAILBOX_WAIT);
    service_fake_tick();
    assert(fake.writes == 1 && fake.value == 123);
    fake.fail_to = true;
    assert(vcoServiceReceive(1, TEST_USER(TEST_REPLY), sizeof(VcoControlReply)) ==
           VCO_MAILBOX_COPY);
    fake.fail_to = false;
    assert(!vcoServiceResume(TEST_USER(TEST_REPLY), 64));
    assert(receive(1).status == VCO_CONTROL_OK);
    assert(command(s, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    packet(1, TEST_GAME);
    unsigned allocations = fake.allocations;
    assert(!load());
    service_fake_tick();
    assert(status().revision == 1 && fake.allocations == allocations && fake.writes == 1);
    fake.now += VCO_ENGINE_PERIOD_US;
    assert(command(s, 2, false).status == VCO_CONTROL_OK && fake.writes == 1);
    assert(command(s, 3, true).status == VCO_CONTROL_OK && fake.writes == 2);
    assert(!submit(request(s, 4, true)));
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(receive(ticket()).status == VCO_CONTROL_UNBOUND && fake.writes == 2);
    s = status();
    assert(!s.revision && !s.count && !s.catalog_revision && !s.content_key && s.load_request == 1);
    assert(fake.live_blocks == 1 && !load()); /* old upload retry must not resurrect */
    service_fake_tick();
    assert(!status().revision);
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(!status().error && fake.live_blocks == 1);
    s = bind_list(2, TEST_GAME);
    assert(s.revision == 2 && fake.writes == 2);
}
static void receipts(void)
{
    game(TEST_GAME);
    VcoServiceStatus good = bind_list(1, TEST_GAME);
    packet(2, TEST_GAME);
    fake.user[packet_size - 1] ^= 0x80;
    assert(!load());
    service_fake_tick();
    VcoServiceStatus s = status();
    assert(s.error == VCO_E_ARGUMENT && s.load_request == 2 &&
           s.content_key == packet_header.content_key && s.revision == good.revision &&
           s.count == 1);
    unsigned allocations = fake.allocations;
    assert(!load());
    service_fake_tick();
    assert(fake.allocations == allocations && status().error == VCO_E_ARGUMENT);
    VcoUploadHeader altered = packet_header;
    ++altered.pid;
    memcpy(fake.user, &altered, sizeof(altered));
    assert(load() == VCO_MAILBOX_ARGUMENT);
    packet(1, TEST_GAME);
    assert(load() == VCO_MAILBOX_ARGUMENT);
    packet(3, TEST_GAME + 1);
    assert(!load());
    service_fake_tick();
    s = status();
    assert(s.load_request == 3 && s.error == VCO_E_SESSION && s.revision == good.revision);
    packet(4, TEST_GAME);
    fake.fail_allocation_call = fake.allocations + 1;
    assert(!load());
    service_fake_tick();
    s = status();
    assert(s.load_request == 4 && s.error == VCO_E_LIMIT && s.revision == good.revision);
    assert(!load());
    service_fake_tick();
    assert(status().error == VCO_E_LIMIT);
    packet(5, TEST_GAME);
    altered = packet_header;
    altered.codes = UINT32_MAX;
    memcpy(fake.user, &altered, sizeof(altered));
    assert(!load());
    service_fake_tick();
    assert(status().load_request == 5 && status().error == VCO_E_ARGUMENT);
    assert(bind_list(6, TEST_GAME).revision > good.revision && !fake.writes);
}
static void lifecycle(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    observe(TEST_GAME, VCO_BACKGROUND_CANDIDATE);
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(status().state == VCO_SERVICE_BACKGROUND && fake.writes == 1);
    observe(TEST_GAME, VCO_FOREGROUND_UNKNOWN);
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(fake.writes == 1);
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.writes == 2 && status().session == first.session);
    fake.now += VCO_VITA_AUTHORITY_LEASE_US;
    service_fake_tick();
    assert(fake.writes == 2 && status().state == VCO_SERVICE_BACKGROUND);
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_event(VCO_PROCESS_EVENT_STOP, TEST_GAME);
    service_fake_tick();
    assert(fake.writes == 2); /* receipt predates invalidation */
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.writes == 3);
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    observe(TEST_GAME + 1, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    VcoServiceStatus second = status();
    assert(second.pid == TEST_GAME + 1 && second.session > first.session && !second.revision &&
           !second.error && fake.refs[0] == 0 && fake.refs[1] == 1 && fake.unbinds == 1);
    second = bind_list(2, TEST_GAME + 1);
    assert(second.revision > first.revision && fake.writes == 3);
    assert(command(first, 2, true).status == VCO_CONTROL_STALE && fake.writes == 3);
    assert(command(second, 1, true).status == VCO_CONTROL_OK && fake.writes == 4);
    fake.epoch |= VCO_PROCESS_EVENTS_LOST;
    observe(TEST_GAME + 1, VCO_FOREGROUND_CANDIDATE);
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(fake.writes == 4 && status().state == VCO_SERVICE_ERROR &&
           status().error == VCO_E_SESSION);
}
static void rearm(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    fake.block_during_write = true;
    assert(command(first, 1, true).status == VCO_CONTROL_OK && !fake.writes);
    fake.block_during_write = false;
    unsigned rearms = fake.rearms;
    VcoServiceStatus second = bind_list(2, TEST_GAME);
    assert(second.session == first.session && second.revision > first.revision &&
           fake.rearms == rearms + 1 && !fake.writes);
    assert(command(second, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.fail_rearm = true;
    packet(3, TEST_GAME);
    assert(!load());
    service_fake_tick();
    VcoServiceStatus failed = status();
    assert(failed.state == VCO_SERVICE_ERROR && failed.error == VCO_E_SESSION &&
           failed.load_request == 3 && !failed.revision && fake.writes == 1);
    fake.fail_rearm = false;
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(status().error == VCO_E_SESSION); /* Ambiguous residual never reset by retry. */
}
static void native_state(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    fake.block_during_write = true;
    assert(command(first, 1, true).status == VCO_CONTROL_OK && !fake.writes);
    service_fake_tick();
    VcoServiceStatus failed = status();
    assert(failed.state == VCO_SERVICE_ERROR && failed.error == VCO_E_PATCH &&
           failed.revision == first.revision && failed.count == 1);
    assert(command(first, 2, false).status == VCO_CONTROL_OK);
    service_fake_tick();
    assert(status().error == VCO_E_PATCH); /* Public OFF does not rearm blocked native callbacks. */
    fake.block_during_write = false;
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    failed = status();
    assert(failed.state == VCO_SERVICE_LOADING && !failed.error && !failed.revision);
    VcoServiceStatus second = bind_list(2, TEST_GAME);
    assert(second.session == first.session && second.revision > first.revision && !fake.writes);
    service_fake_quarantine();
    service_fake_tick();
    failed = status();
    assert(failed.state == VCO_SERVICE_ERROR && failed.error == VCO_E_RESTORE &&
           failed.revision == second.revision && failed.count == 1);
    assert(command(second, 1, false).status == VCO_CONTROL_OK);
    service_fake_tick();
    assert(status().error == VCO_E_RESTORE && !fake.writes);
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(status().state == VCO_SERVICE_ERROR && status().error == VCO_E_RESTORE &&
           status().revision == second.revision && status().count == 1);
    unsigned allocations = fake.allocations;
    packet(3, TEST_GAME);
    assert(!load());
    service_fake_tick();
    assert(status().error == VCO_E_RESTORE && fake.allocations == allocations);
    unsigned reaps = fake.reap_calls;
    fake.fail_title = true;
    service_fake_tick();
    assert(status().error == VCO_E_RESTORE && fake.reap_calls == reaps && !fake.writes);
    fake.fail_title = false;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_RESTORE && fake.reap_calls == reaps);
    service_fake_replace_game();
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.reap_calls == reaps + 1 && !fake.writes); /* No EXIT event was needed. */
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().session > first.session && !status().error && !status().revision &&
           status().state == VCO_SERVICE_LOADING);
    VcoServiceStatus replacement = bind_list(4, TEST_GAME);
    assert(replacement.session > first.session && replacement.revision > second.revision &&
           !fake.writes); /* Proven retired cleanup removes the old native error, never enables. */
}
static void upload_instance(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    packet(2, TEST_GAME);
    fake.replace_game_during_copy = fake.from_calls + 2;
    assert(!load()); /* Old-instance packet waits while the same PID is replaced. */
    VcoServiceStatus next = status();
    assert(next.session > first.session && next.pid == first.pid && !next.revision);
    service_fake_tick();
    next = status();
    assert(next.load_request == 2 && next.error == VCO_E_SESSION && !next.revision &&
           !fake.writes && fake.live_blocks == 1);
    next = bind_list(3, TEST_GAME);
    assert(next.session > first.session && next.revision > first.revision);
    assert(command(next, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    packet(4, TEST_GAME);
    VcoUploadHeader stale = packet_header;
    stale.target_session = first.session;
    memcpy(fake.user, &stale, sizeof(stale));
    assert(load() == VCO_MAILBOX_ARGUMENT);
    stale.target_session = 0;
    memcpy(fake.user, &stale, sizeof(stale));
    assert(load() == VCO_MAILBOX_ARGUMENT);
    VcoServiceStatus unchanged = status();
    assert(unchanged.session == next.session && unchanged.revision == next.revision &&
           unchanged.load_request == 3 && !unchanged.error && fake.live_blocks == 2);
}
static void permanent_loss(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.epoch |= VCO_PROCESS_EVENTS_LOST;
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(status().state == VCO_SERVICE_ERROR && status().error == VCO_E_SESSION &&
           fake.writes == 1);
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && !status().revision && fake.live_blocks == 1);
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    packet(2, TEST_GAME);
    unsigned allocations = fake.allocations;
    assert(!load());
    service_fake_tick();
    assert(status().load_request == 2 && status().error == VCO_E_SESSION &&
           fake.allocations == allocations && fake.writes == 1);
    fake.epoch &=
        ~VCO_PROCESS_EVENTS_LOST; /* Even a broken producer cannot clear the service latch. */
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().state == VCO_SERVICE_ERROR && fake.writes == 1);
}
static void fault_exit_cleanup(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.epoch |= VCO_PROCESS_EVENTS_LOST;
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && fake.refs[0] == 1);
    /* LOST is never retirement proof. With a changed retained object, the
     * independent verifier can prove retirement even for this LOST event. */
    service_fake_replace_game();
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    service_fake_tick();
    VcoServiceStatus ended = status();
    assert(ended.error == VCO_E_SESSION && !ended.pid && !ended.session && !ended.revision);
    assert(!fake.refs[0] && fake.unbinds == 1 && fake.live_blocks == 1 && fake.writes == 1);
    observe(TEST_GAME + 1, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.binds == 1 && !fake.refs[1] && fake.writes == 1);
}
static void independent_unbind(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.fail_memory_unbind = true;
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && fake.unbinds == 1 && fake.live_blocks == 1);
    /* A memory provider error cannot suppress the authority owner's separate
     * known reference release. It cannot permit a new execution session. */
    assert(!fake.refs[0]);
    fake.fail_memory_unbind = false;
    service_fake_tick();
    assert(fake.unbinds == 1 && fake.binds == 1 && fake.writes == 1);
}
static void owner_departure(void)
{
    game(TEST_GAME);
    VcoServiceStatus s = bind_list(1, TEST_GAME);
    assert(command(s, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.shell_pid = -1; /* Missing lookup revokes, but is not replacement proof. */
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(fake.writes == 1);
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(s)) == VCO_MAILBOX_OWNER);
    fake.shell_pid = TEST_SHELL;
    service_fake_tick();
    assert(fake.writes == 1); /* cached receipt cannot renew a revoked candidate */
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.writes == 2);
}
static void shell_restart(bool same_pid)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    assert(!submit(request(first, 2, true))); /* old reply must not cross owners */
    uint64_t old_ticket = ticket();
    packet(2, TEST_GAME);
    assert(!load());                             /* old pending upload must not cross owners */
    assert(!vco_service_call_begin(TEST_SHELL)); /* an entire input syscall remains in flight */
    fake.shell_pid = fake.caller = same_pid ? TEST_SHELL : TEST_SHELL + 1;
    fake.owner_replaced = same_pid;
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(fake.writes == 1 && !fake.capture_resets && fake.refs[2] == 1);
    assert(vco_service_call_begin(fake.caller) == VCO_MAILBOX_BUSY);
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) == VCO_MAILBOX_BUSY);
    assert(vcoServiceAck(old_ticket) == VCO_MAILBOX_BUSY);
    vco_service_call_end();
    service_fake_tick();
    VcoServiceStatus reopened = status();
    assert(!reopened.session && !reopened.revision && !reopened.error &&
           reopened.load_request == 1);
    assert(fake.writes == 1 && fake.capture_resets == 1 && fake.live_blocks == 1 && !fake.refs[0]);
    if (!same_pid) {
        assert(fake.refs[2] == 0 && fake.refs[3] == 1);
        fake.caller = TEST_SHELL;
        assert(vco_service_call_begin(fake.caller) == VCO_MAILBOX_OWNER);
        fake.caller = TEST_SHELL + 1;
    }
    assert(!vco_service_call_begin(fake.caller));
    vco_service_call_end();
    assert(vcoServiceResume(TEST_USER(TEST_REPLY), 64) == VCO_MAILBOX_EMPTY);
    assert(vcoServiceReceive(old_ticket, TEST_USER(TEST_REPLY), sizeof(VcoControlReply)) ==
           VCO_MAILBOX_TICKET);
    game(TEST_GAME);
    VcoServiceStatus second = bind_list(2, TEST_GAME);
    assert(second.session > first.session && second.revision > first.revision && fake.writes == 1);
    assert(!submit(request(second, 1, true)) && ticket() > old_ticket);
    service_fake_tick();
    assert(receive(ticket()).status == VCO_CONTROL_OK && fake.writes == 2);
    assert(command(first, 3, true).status == VCO_CONTROL_STALE);
}
static void copy_restart(bool uploading)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    if (uploading) {
        packet(2, TEST_GAME);
        fake.restart_during_copy = fake.from_calls + 2;
        assert(load() == VCO_MAILBOX_OWNER);
    } else {
        fake.restart_during_copy = fake.from_calls + 1;
        assert(submit(request(first, 1, true)) == VCO_MAILBOX_OWNER);
    }
    assert(!fake.capture_resets && fake.refs[2] == 1 && !fake.writes);
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(first)) == VCO_MAILBOX_BUSY);
    service_fake_tick();
    VcoServiceStatus next = status();
    assert(!next.session && !next.revision && next.load_request == 1 && fake.capture_resets == 1);
    assert(vcoServiceResume(TEST_USER(TEST_REPLY), 64) == VCO_MAILBOX_EMPTY);
    game(TEST_GAME);
    bind_list(2, TEST_GAME);
    assert(!fake.writes);
}
static void bind_failures(void)
{
    fake.expire_bind = true;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && !status().session && fake.binds == 1 &&
           fake.unbinds == 1 && !fake.refs[0]);
    fake.fail_bind = true;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && !fake.refs[0]);
    fake.fail_bind = false;
    fake.fail_features = true;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && !status().session && !fake.refs[0]);
    fake.fail_features = false;
    fake.replace_bind = true;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && !status().session && !fake.refs[0] &&
           !fake.reap_calls);
    game(TEST_GAME);
    VcoServiceStatus s = bind_list(1, TEST_GAME);
    assert(!s.error && command(s, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    service_fake_replace_game();
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick(); /* independent object verification retires old binding */
    assert(fake.writes == 1);
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().session > s.session && !status().revision && !status().error);
}
static void coexistence(void)
{
    /* A loaded legacy executor, or failure to query it, must not deny the
     * validated VCO session. Users switch entries independently for comparison. */
    fake.collision = true;
    game(TEST_GAME);
    VcoServiceStatus s = bind_list(1, TEST_GAME);
    assert(command(s, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    assert(command(s, 2, false).status == VCO_CONTROL_OK);
    fake.collision = false;
    fake.collision_unknown = true;
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(status().state == VCO_SERVICE_READY && !status().error && fake.writes == 1);
    assert(command(s, 3, true).status == VCO_CONTROL_OK && fake.writes == 2);
}
static void quarantine(void)
{
    game(TEST_GAME);
    VcoServiceStatus s = bind_list(1, TEST_GAME);
    assert(command(s, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.fail_free = true;
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(status().error == VCO_E_IO && fake.live_blocks == 2);
    fake.fail_free = false;
    observe(TEST_GAME + 1, VCO_FOREGROUND_CANDIDATE);
    packet(2, TEST_GAME + 1);
    assert(!load());
    service_fake_tick();
    assert(status().load_request == 2 && status().error == VCO_E_IO && fake.binds == 1 &&
           fake.live_blocks == 2 && fake.writes == 1);
}
static void retired_cleanup(bool poison)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.fail_dispose = true;
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(status().error == VCO_E_RESTORE && !fake.reap_calls && fake.refs[0] == 1);
    fake.reap_result = poison ? -1 : 1;
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    observe(TEST_GAME + 1, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_RESTORE && fake.reap_calls == 1 && fake.binds == 1);
    service_fake_tick();
    assert(fake.reap_calls == (poison ? 1u : 2u) && fake.writes == 1);
    fake.reap_result = 0;
    service_fake_tick();
    if (poison) {
        assert(status().error == VCO_E_RESTORE && fake.reap_calls == 1 && fake.binds == 1);
    } else {
        VcoServiceStatus second = status();
        assert(!second.error && second.pid == TEST_GAME + 1 && second.session > first.session &&
               !second.revision && fake.refs[0] == 0 && fake.refs[1] == 1 && fake.live_blocks == 1);
        second = bind_list(2, TEST_GAME + 1);
        assert(second.revision > first.revision && fake.writes == 1);
        assert(command(second, 1, true).status == VCO_CONTROL_OK && fake.writes == 2);
    }
}
static void owner_retirement(unsigned mode)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    bool pending = mode == 2 || mode == 3 || mode == 5;
    if (pending) {
        fake.fail_dispose = true;
        assert(!vcoServiceLoad(NULL, 0));
        service_fake_tick();
        assert(status().error == VCO_E_RESTORE && !fake.reap_calls);
    }
    if (mode == 5)
        fake.owner_replaced = true; /* Rebinding must not retry retirement in this tick. */
    else
        fake.owner_lookup_failure = true;
    if (mode == 1)
        service_fake_replace_game(); /* Independent evidence, with no EXIT in the queue. */
    else if (mode == 4)
        fake.fail_title = true; /* Query failure is never retirement evidence. */
    else
        service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    fake.reap_result = mode == 2 || mode == 5 ? 1 : mode == 3 ? -1 : 0;
    fake.now += VCO_ENGINE_PERIOD_US;
    service_fake_tick();
    assert(fake.writes == 1 && fake.binds == 1);
    if (mode == 4) {
        assert(!fake.reap_calls && !fake.unbinds && fake.refs[0] == 1 && fake.live_blocks == 2);
        fake.fail_title = false;
        service_fake_tick();
        assert(!fake.reap_calls && !fake.unbinds && fake.refs[0] == 1 && fake.writes == 1);
        return;
    }
    assert(fake.reap_calls == 1);
    if (pending) {
        assert(fake.refs[0] == 1 && !fake.unbinds && fake.live_blocks == 2);
        service_fake_tick();
        assert(fake.reap_calls == (mode == 3 ? 1u : 2u) && fake.writes == 1);
        fake.reap_result = 0;
        service_fake_tick();
        if (mode == 3) {
            assert(fake.reap_calls == 1 && !fake.unbinds && fake.refs[0] == 1 &&
                   fake.live_blocks == 2);
            fake.owner_lookup_failure = false;
            assert(status().error == VCO_E_RESTORE);
            return;
        }
        assert(fake.reap_calls == 3);
    }
    assert(!fake.refs[0] && fake.unbinds == 1 && fake.live_blocks == 1);
    unsigned reaped = fake.reap_calls;
    for (unsigned i = 0; i < 3; ++i)
        service_fake_tick();
    assert(fake.reap_calls == reaped && fake.unbinds == 1 && fake.writes == 1 && fake.binds == 1);
    fake.owner_lookup_failure = false;
    VcoServiceStatus ended = status();
    assert(!ended.pid && !ended.session && !ended.revision && !ended.error);
    VcoServiceStatus next = game(TEST_GAME + 1);
    assert(next.session > first.session && fake.writes == 1);
    next = bind_list(2, TEST_GAME + 1);
    assert(next.revision > first.revision && fake.writes == 1);
    assert(command(first, 100, true).status == VCO_CONTROL_STALE);
    VcoControlRequest query = request(next, 101, false);
    query.operation = VCO_CONTROL_QUERY;
    assert(!submit(query));
    service_fake_tick();
    VcoControlReply reply = receive(ticket());
    assert(reply.status == VCO_CONTROL_OK && !reply.rows[0].desired &&
           reply.rows[0].state == VCO_ENTRY_OFF);
    assert(fake.writes == 1);
}
static void owner_reference_failure(bool consumed)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    fake.owner_lookup_failure = true;
    fake.game_release_error = -111;
    fake.consume_failed_game_release = consumed;
    service_fake_tick();
    assert(!fake.game_release_error && fake.refs[0] == (consumed ? 1 : 2));
    assert(!fake.reap_calls && !fake.unbinds && fake.writes == 1 && fake.binds == 1);
    fake.owner_lookup_failure = false;
    assert(status().state == VCO_SERVICE_ERROR && status().error == VCO_E_SESSION);
    for (unsigned i = 0; i < 3; ++i) {
        observe(i ? TEST_GAME + 1 : TEST_GAME, VCO_FOREGROUND_CANDIDATE);
        service_fake_tick();
        assert(status().error == VCO_E_SESSION && fake.writes == 1 && fake.binds == 1 &&
               !fake.reap_calls && !fake.unbinds && fake.refs[0] == (consumed ? 1 : 2));
    }
    /* Later independent retirement may release the original known reference,
     * while the ambiguous temporary release remains unretried and visible. */
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    service_fake_tick();
    assert(status().error == VCO_E_SESSION && fake.reap_calls == 1 && fake.unbinds == 1 &&
           fake.refs[0] == (consumed ? 0 : 1) && fake.live_blocks == 1 && fake.writes == 1);
    service_fake_tick();
    assert(fake.reap_calls == 1 && fake.unbinds == 1 && fake.refs[0] == (consumed ? 0 : 1));
}
static void quarantine_chain(void)
{
    game(TEST_GAME);
    bind_list(1, TEST_GAME);
    fake.fail_dispose = true;
    fake.fail_free = true;
    packet(2, TEST_GAME);
    assert(!load());
    service_fake_tick();
    assert(status().error == VCO_E_IO && fake.live_blocks == 3 && status().load_request == 2);
    fake.fail_dispose = fake.fail_free = false;
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    observe(TEST_GAME + 1, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_IO && !fake.reap_calls && fake.binds == 1 &&
           fake.live_blocks == 3);
}
static void support_failure(void)
{
    fake.fail_patch_support = true;
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(status().error == VCO_E_PATCH && !fake.binds);
    fake.fail_patch_support = false;
    packet(1, TEST_GAME);
    assert(load() == VCO_MAILBOX_ARGUMENT); /* No target session was ever established. */
    service_fake_tick();
    assert(status().error == VCO_E_PATCH && !status().load_request && !fake.binds);
}
static void startup_failure(const char *kind)
{
    if (!strcmp(kind, "startup_alloc"))
        fake.fail_allocation_call = 1;
    else if (!strcmp(kind, "startup_base"))
        fake.fail_base = true;
    else if (!strcmp(kind, "startup_events"))
        fake.fail_events = true;
    else
        fake.fail_worker = true;
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
           VCO_MAILBOX_CLOSED);
    fake.fail_allocation_call = 0;
    fake.fail_base = fake.fail_events = fake.fail_worker = false;
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
           VCO_MAILBOX_CLOSED);
    assert(fake.allocations == 1 && !fake.binds && !fake.writes);
    assert(!vco_service_call_begin(TEST_SHELL)); /* input survives execution startup failure */
    fake.shell_pid = fake.caller = TEST_SHELL + 1;
    assert(vco_service_call_begin(fake.caller) == VCO_MAILBOX_OWNER);
    assert(vco_service_call_begin(fake.caller) == VCO_MAILBOX_BUSY);
    assert(!fake.capture_resets && fake.refs[2] == 1); /* old input call not finished */
    vco_service_call_end();
    assert(!vco_service_call_begin(fake.caller)); /* caller performs quiescent maintenance */
    vco_service_call_end();
    assert(fake.capture_resets == 1 && !fake.refs[2] && fake.refs[3] == 1);
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
           VCO_MAILBOX_CLOSED);
    assert(fake.allocations == 1 && !fake.binds && !fake.writes);
}
static void runtime_restart(bool cleanup_error)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    assert(!vco_service_call_begin(TEST_SHELL)); /* Whole old input helper remains live. */
    service_fake_worker_fault();
    /* Public data-only engine cleanup succeeds; private native residue must
     * still be reported by the worker's finalizer before ownership handoff. */
    if (cleanup_error)
        service_fake_quarantine();
    fake.shell_pid = fake.caller = cleanup_error ? TEST_SHELL : TEST_SHELL + 1;
    fake.owner_replaced = cleanup_error;
    assert(vco_service_call_begin(fake.caller) == VCO_MAILBOX_OWNER);
    assert(!fake.worker_reaps && !fake.capture_resets && fake.refs[2] == 1);
    vco_service_call_end();
    assert(vco_service_call_begin(fake.caller) == VCO_MAILBOX_BUSY);
    assert(fake.worker_waits == 1 && !fake.worker_deletes && !fake.capture_resets);
    service_fake_worker_finish();
    fake.worker_delete_failure = true;
    assert(vco_service_call_begin(fake.caller) == VCO_MAILBOX_BUSY);
    unsigned waits = fake.worker_waits;
    assert(fake.worker_deletes == 1 && !fake.capture_resets && fake.refs[2] == 1);
    fake.worker_delete_failure = false;
    assert(!vco_service_call_begin(fake.caller));
    vco_service_call_end();
    assert(fake.worker_waits == waits && fake.worker_deletes == 2 && fake.capture_resets == 1);
    VcoServiceStatus failed = status();
    assert(failed.state == VCO_SERVICE_ERROR &&
           failed.error == (cleanup_error ? VCO_E_RESTORE : VCO_E_IO));
    assert(!failed.session && !failed.revision && !failed.count && !failed.content_key);
    assert(failed.load_request == first.load_request && fake.writes == 1);
    unsigned allocations = fake.allocations, binds = fake.binds, reap_calls = fake.worker_reaps;
    VcoForegroundSample sample = service_fake_sample(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    memcpy(fake.user + TEST_SAMPLE, &sample, sizeof(sample));
    assert(vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(sample)) == VCO_MAILBOX_CLOSED);
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_CLOSED);
    assert(submit(request(first, 2, true)) != VCO_MAILBOX_OK);
    assert(vcoServiceReceive(1, TEST_USER(TEST_REPLY), sizeof(VcoControlReply)) != VCO_MAILBOX_OK);
    assert(vcoServiceAck(1) != VCO_MAILBOX_OK &&
           vcoServiceResume(TEST_USER(TEST_REPLY), 64) != VCO_MAILBOX_OK);
    assert(!vco_service_call_begin(fake.caller));
    vco_service_call_end();
    assert(status().error == failed.error && fake.allocations == allocations &&
           fake.binds == binds && fake.worker_reaps == reap_calls && fake.writes == 1);
    assert(fake.live_blocks == (cleanup_error ? 1u : 0u));
    assert(fake.refs[0] == (cleanup_error ? 1 : 0)); /* Only uncertain native ownership remains. */
    fake.input_ready = false;
    unsigned copies = fake.to_calls;
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(failed)) == VCO_MAILBOX_CLOSED);
    assert(fake.to_calls == copies); /* Terminal diagnostics still require an admitted scope. */
}
static void runtime_same_owner(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    assert(!vco_service_call_begin(TEST_SHELL));
    service_fake_worker_fault();
    assert(vco_service_call_begin(TEST_SHELL) == VCO_MAILBOX_BUSY);
    assert(!fake.worker_reaps && !fake.capture_resets);
    vco_service_call_end();
    assert(vco_service_call_begin(TEST_SHELL) == VCO_MAILBOX_BUSY);
    assert(fake.worker_reaps == 1 && !fake.worker_deletes);
    service_fake_worker_finish();
    fake.owner_lookup_failure = true;
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) == VCO_MAILBOX_BUSY);
    assert(fake.worker_deletes == 1 && !fake.capture_resets);
    fake.owner_lookup_failure = false;
    VcoServiceStatus failed = status();
    assert(failed.state == VCO_SERVICE_ERROR && failed.error == VCO_E_IO && !failed.session &&
           !failed.revision && !failed.count);
    assert(fake.worker_deletes == 1 && fake.capture_resets == 1 && fake.refs[2] == 1);
    unsigned reaps = fake.worker_reaps;
    assert(!vco_service_call_begin(TEST_SHELL));
    vco_service_call_end();
    assert(status().error == VCO_E_IO && fake.worker_reaps == reaps && fake.writes == 1);
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_CLOSED && !fake.live_blocks && !fake.refs[0]);
}
static void terminal_cleanup(unsigned mode)
{
    /* Actual service/authority/mailbox ownership handoff, SDK/native provider
     * substitutes. No EXIT is required after proven successful finalization. */
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    unsigned frees = fake.frees;
    bool probe_failure = mode == 14 || mode == 15;
    bool residue = (mode >= 1 && mode <= 4) || probe_failure;
    if (residue)
        service_fake_quarantine();
    if (mode == 10)
        fake.worker_cleanup_failure = true; /* Public cleanup error remains conservative. */
    if (mode == 16) {
        packet(2, TEST_GAME);
        assert(!load()); /* Accepted upload, not yet consumed by the old worker. */
    }
    if (mode == 12)
        assert(!vco_service_call_begin(TEST_SHELL));
    service_fake_worker_fault();
    if (mode == 12) {
        assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
               VCO_MAILBOX_BUSY);
        assert(!fake.worker_reaps && fake.frees == frees && fake.refs[0] == 1);
        vco_service_call_end();
        assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
               VCO_MAILBOX_BUSY);
        assert(!fake.worker_deletes && fake.frees == frees);
    }
    service_fake_worker_finish();
    unsigned disposed = fake.dispose_calls;
    if (mode == 12) {
        fake.worker_delete_failure = true;
        assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
               VCO_MAILBOX_BUSY);
        assert(fake.worker_deletes == 1 && fake.frees == frees && fake.refs[0] == 1);
        fake.worker_delete_failure = false;
    } else
        assert(vco_service_call_begin(TEST_SHELL) == VCO_MAILBOX_BUSY);
    assert(!vco_service_call_begin(TEST_SHELL));
    vco_service_call_end();
    assert(fake.frees == frees && fake.refs[0] == 1 && fake.live_blocks == 2);
    /* Input helpers, invalid buffers and unauthenticated callers never own
     * post-worker cleanup, even after join and deletion have succeeded. */
    assert(vcoServiceStatus(NULL, sizeof(VcoServiceStatus)) == VCO_MAILBOX_ARGUMENT);
    fake.caller = 77;
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) == VCO_MAILBOX_OWNER);
    fake.caller = TEST_SHELL;
    assert(fake.frees == frees && fake.refs[0] == 1);
    if (mode == 2)
        service_fake_replace_game();
    if (mode == 3)
        fake.fail_title = true;
    if (mode == 5 || mode == 6) {
        fake.fail_free = true;
        fake.consume_failed_free = mode == 6;
    }
    if (mode == 7 || mode == 8 || probe_failure) {
        fake.game_release_error = -111;
        fake.consume_failed_game_release = mode == 8 || mode == 15;
    }
    if (mode == 9)
        fake.fail_memory_unbind = true;
    if (mode == 13)
        fake.status_during_free = true;
    if (mode == 11) {
        fake.fail_to = true;
        assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
               VCO_MAILBOX_COPY);
        assert(!fake.live_blocks && !fake.refs[0] && fake.unbinds == 1);
        fake.fail_to = false;
    }
    VcoServiceStatus failed = status();
    VcoError expected = probe_failure            ? VCO_E_SESSION
                        : residue || mode == 10  ? VCO_E_RESTORE
                        : mode >= 7 && mode <= 9 ? VCO_E_SESSION
                                                 : VCO_E_IO;
    assert(failed.state == VCO_SERVICE_ERROR && failed.error == expected &&
           failed.load_request == first.load_request && !failed.session && !failed.revision &&
           !failed.count && !failed.content_key);
    if ((residue && mode != 2) || mode == 10) {
        int held = mode == 14 ? 2 : 1; /* An uncertain temporary release is not retried. */
        assert(fake.frees == frees + 1 && fake.refs[0] == held && fake.live_blocks == 1 &&
               !fake.reap_calls && !fake.unbinds);
        if (mode == 3) {
            fake.fail_title = false;
            assert(status().error == expected && !fake.reap_calls && fake.refs[0] == 1);
        }
        service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
        fake.reap_result = mode == 4 ? -1 : 1;
        assert(status().error == expected && fake.reap_calls == 1 && fake.refs[0] == held);
        assert(status().error == expected && fake.reap_calls == (mode == 4 ? 1u : 2u));
        fake.reap_result = 0;
        assert(status().error == expected);
        if (mode == 4) {
            assert(fake.reap_calls == 1 && !fake.unbinds && fake.refs[0] == 1 &&
                   fake.live_blocks == 1);
        } else
            assert(fake.reap_calls == 3 && fake.unbinds == 1 &&
                   fake.refs[0] == (mode == 14 ? 1 : 0) && !fake.live_blocks);
    } else {
        assert(fake.unbinds == 1 && fake.refs[0] == (mode == 7 ? 1 : 0));
        assert(fake.live_blocks == (mode == 5 ? 2u : 0u));
        assert(fake.reap_calls == (mode == 2 ? 1u : 0u));
    }
    fake.fail_free = fake.fail_memory_unbind = false;
    unsigned final_frees = fake.frees, reaped = fake.reap_calls, allocations = fake.allocations;
    for (unsigned i = 0; i < 3; ++i) {
        assert(status().error == expected);
        assert(!vco_service_call_begin(TEST_SHELL));
        vco_service_call_end();
    }
    assert(fake.frees == final_frees && fake.reap_calls == reaped &&
           fake.allocations == allocations && fake.binds == 1 && fake.writes == 1 &&
           fake.dispose_calls == disposed);
    assert(vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(VcoForegroundSample)) ==
           VCO_MAILBOX_CLOSED);
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_CLOSED &&
           submit(request(first, 2, true)) == VCO_MAILBOX_OWNER);
}
static void terminal_empty(void)
{
    (void)status(); /* Start without ever binding a game or allocating an arena. */
    assert(fake.live_blocks == 1 && !fake.refs[0] && !fake.binds);
    service_fake_worker_fault();
    service_fake_worker_finish();
    assert(vco_service_call_begin(TEST_SHELL) == VCO_MAILBOX_BUSY);
    unsigned disposed = fake.dispose_calls;
    VcoServiceStatus failed = status();
    assert(failed.state == VCO_SERVICE_ERROR && failed.error == VCO_E_IO && !failed.session);
    assert(!fake.live_blocks && fake.frees == 1 && !fake.reap_calls && !fake.unbinds &&
           !fake.refs[0] && !fake.writes && fake.dispose_calls == disposed);
    assert(status().error == VCO_E_IO && fake.frees == 1 && !fake.live_blocks);
}
static void input_disabled(void)
{
    fake.input_ready = false;
    assert(vco_service_call_begin(TEST_SHELL) == VCO_MAILBOX_CLOSED);
    assert(vco_service_authorize_caller(TEST_SHELL) < 0);
    assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
           VCO_MAILBOX_CLOSED);
    assert(vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(VcoForegroundSample)) ==
           VCO_MAILBOX_CLOSED);
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_CLOSED);
    assert(submit((VcoControlRequest){0}) == VCO_MAILBOX_CLOSED);
    assert(vcoServiceReceive(1, TEST_USER(TEST_REPLY), sizeof(VcoControlReply)) ==
           VCO_MAILBOX_CLOSED);
    assert(vcoServiceAck(1) == VCO_MAILBOX_CLOSED &&
           vcoServiceResume(TEST_USER(TEST_REPLY), 64) == VCO_MAILBOX_CLOSED);
    assert(vcoServiceTrace(0, TEST_USER(TEST_REPLY), sizeof(VcoTraceBatch)) == VCO_MAILBOX_CLOSED);
    assert(!fake.allocations && !fake.binds && !fake.from_calls && !fake.to_calls &&
           !fake.refs[2] && !fake.writes);
}
static VcoTraceBatch trace_read(uint64_t after)
{
    assert(!vcoServiceTrace(after, TEST_USER(TEST_REPLY), sizeof(VcoTraceBatch)));
    VcoTraceBatch batch;
    memcpy(&batch, fake.user + TEST_REPLY, sizeof(batch));
    assert(batch.abi == VCO_TRACE_ABI && batch.size == sizeof(batch));
    return batch;
}
static void trace_transport(void)
{
    fake.caller = 77;
    assert(vcoServiceTrace(0, TEST_USER(TEST_REPLY), sizeof(VcoTraceBatch)) == VCO_MAILBOX_OWNER);
    assert(!fake.to_calls && !fake.allocations);
    fake.caller = TEST_SHELL;
    VcoTraceBatch empty = trace_read(0);
    assert(!empty.count && !empty.latest_sequence && !empty.oldest_sequence);
    assert(!fake.allocations && !fake.binds && !fake.writes);
    assert(vcoServiceTrace(0, TEST_USER(TEST_REPLY), sizeof(empty) - 1) == VCO_MAILBOX_ARGUMENT);
    assert(vcoServiceTrace(0, (void *)(uintptr_t)0xfffffff0u, sizeof(empty)) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vcoServiceTrace(1, TEST_USER(TEST_REPLY), sizeof(empty)) == VCO_MAILBOX_ARGUMENT);
    VcoServiceStatus s = game(TEST_GAME);
    s = bind_list(1, TEST_GAME);
    assert(command(s, 1, true).status == VCO_CONTROL_OK);
    assert(command(s, 2, false).status == VCO_CONTROL_OK);
    unsigned allocations = fake.allocations, writes = fake.writes, copies = fake.from_calls;
    VcoTraceBatch first = trace_read(0), again = trace_read(0);
    assert(first.count && !memcmp(&first, &again, sizeof(first)));
    fake.fail_to = true;
    assert(vcoServiceTrace(0, TEST_USER(TEST_REPLY), sizeof(first)) == VCO_MAILBOX_COPY);
    fake.fail_to = false;
    again = trace_read(0);
    assert(!memcmp(&first, &again, sizeof(first)));
    bool start = false, probe = false, list = false, observation = false;
    uint64_t cursor = 0;
    for (;;) {
        VcoTraceBatch batch = trace_read(cursor);
        if (!batch.count)
            break;
        for (unsigned i = 0; i < batch.count; ++i) {
            VcoTraceRecord *r = &batch.records[i];
            assert(r->sequence == ++cursor && r->tick == (uint64_t)fake.now);
            start |= r->event == VCO_SERVICE_TRACE_START;
            probe |= r->event == VCO_PATCH_TRACE_PROBE_RESULT;
            list |= r->event == VCO_SERVICE_TRACE_LIST && r->result == 1;
            observation |= r->event == VCO_SERVICE_TRACE_OBSERVE;
        }
    }
    assert(start && probe && list && observation);
    assert(fake.allocations == allocations && fake.writes == writes && fake.from_calls == copies);
    service_fake_worker_fault();
    assert(vcoServiceTrace(cursor, TEST_USER(TEST_REPLY), sizeof(first)) == VCO_MAILBOX_BUSY);
    service_fake_worker_finish();
    VcoTraceBatch ended = trace_read(cursor);
    bool end = false;
    for (unsigned i = 0; i < ended.count; ++i)
        end |= ended.records[i].event == VCO_SERVICE_TRACE_WORKER_END;
    assert(end && fake.allocations == allocations && fake.writes == writes);
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_CLOSED);
}
static void trace_freeze(void)
{
    VcoServiceStatus s = game(TEST_GAME);
    s = bind_list(1, TEST_GAME);
    service_fake_trace(VCO_PATCH_TRACE_INJECT, (int32_t)0x8002d019, 149, 4);
    service_fake_trace(VCO_PATCH_TRACE_INJECT_FAILURE, (int32_t)0x8002d019, 0x8116ce30,
                       VCO_PATCH_STAGE_TAI_INJECT << 8 | 4);
    uint64_t cursor = 0, frozen = 0;
    bool found = false;
    for (;;) {
        VcoTraceBatch batch = trace_read(cursor);
        frozen = batch.latest_sequence;
        if (!batch.count)
            break;
        for (unsigned i = 0; i < batch.count; ++i) {
            const VcoTraceRecord *r = &batch.records[i];
            if (r->event == VCO_PATCH_TRACE_INJECT_FAILURE) {
                assert(r->result == (int32_t)0x8002d019 && r->subject == 0x8116ce30 &&
                       r->detail == (VCO_PATCH_STAGE_TAI_INJECT << 8 | 4));
                found = true;
            }
            cursor = r->sequence;
        }
    }
    assert(found);
    for (unsigned i = 0; i < 1000; ++i)
        service_fake_trace(VCO_PATCH_TRACE_NATIVE_RELEASE, 0, 1000 + i, i);
    assert(command(s, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    assert(command(s, 2, false).status == VCO_CONTROL_OK);
    VcoTraceBatch last = trace_read(frozen - 1);
    assert(last.latest_sequence == frozen && last.count == 1 &&
           last.records[0].event == VCO_PATCH_TRACE_INJECT_FAILURE);
    assert(status().state == VCO_SERVICE_READY); /* Only diagnostics froze. */
}
static void *scope_producer(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 10000; ++i) {
        int result = vco_service_call_begin(TEST_SHELL);
        assert(result == VCO_MAILBOX_OK || result == VCO_MAILBOX_BUSY);
        if (!result)
            vco_service_call_end();
    }
    return NULL;
}
static void concurrent_scopes(void)
{
    assert(!vco_service_call_begin(TEST_SHELL));
    vco_service_call_end();
    pthread_t producers[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&producers[i], NULL, scope_producer, NULL));
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_join(producers[i], NULL));
    assert(fake.refs[2] == 1 && !fake.allocations);
    /* All admitted scopes ended: without any worker, a real Shell replacement
     * can now drain and rebind rather than sticking on a leaked reader count. */
    fake.owner_replaced = true;
    assert(vco_service_call_begin(TEST_SHELL) == VCO_MAILBOX_OWNER);
    assert(!vco_service_call_begin(TEST_SHELL));
    vco_service_call_end();
    assert(fake.capture_resets == 1 && fake.refs[2] == 1 && !fake.allocations);
}
static void scoped_reload(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    assert(!submit(request(first, 2, true))); /* Still waiting in the mailbox. */
    uint64_t pending = ticket();
    unsigned disposed = fake.dispose_calls;
    packet(2, TEST_GAME);
    assert(!load());
    service_fake_tick();
    VcoServiceStatus next = status();
    assert(next.state == VCO_SERVICE_READY && next.revision > first.revision &&
           next.session == first.session && fake.dispose_calls == disposed + 1 &&
           fake.writes == 1 && !fake.worker_waits && !fake.worker_deletes);
    assert(receive(pending).status == VCO_CONTROL_STALE);
    assert(command(next, 1, true).status == VCO_CONTROL_OK && fake.writes == 2);
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(status().state == VCO_SERVICE_LOADING && !status().revision && fake.writes == 2);
    service_fake_tick();
    assert(fake.writes == 2);
    game(TEST_GAME + 1);
    next = bind_list(3, TEST_GAME + 1);
    assert(command(first, 3, true).status == VCO_CONTROL_STALE);
    assert(command(next, 1, true).status == VCO_CONTROL_OK && fake.writes == 3 &&
           !fake.worker_waits && !fake.worker_deletes);
}
static void scoped_failure(void)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    service_fake_quarantine();
    unsigned disposed = fake.dispose_calls;
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    VcoServiceStatus failed = status();
    assert(failed.state == VCO_SERVICE_ERROR && failed.error == VCO_E_RESTORE &&
           failed.revision == first.revision && failed.count == 1 &&
           fake.dispose_calls == disposed + 1 && fake.live_blocks == 2);
    fake.fail_dispose = false; /* A changed SDK result cannot erase the old failure. */
    assert(!vcoServiceLoad(NULL, 0));
    for (unsigned i = 0; i < 20; ++i)
        service_fake_tick();
    assert(status().error == VCO_E_RESTORE && fake.dispose_calls == disposed + 1 &&
           fake.writes == 1 && fake.live_blocks == 2 && !fake.worker_waits);
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    game(TEST_GAME + 1);
    VcoServiceStatus next = bind_list(2, TEST_GAME + 1);
    assert(command(next, 1, true).status == VCO_CONTROL_OK && fake.writes == 2 &&
           !fake.worker_waits && !fake.worker_deletes);
}
static void native_close(bool module)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    service_fake_tick(); /* Publish metadata refreshed by the manual-ON boundary. */
    VcoVitaCloseTarget target;
    assert(!vco_service_close_target(&target));
    assert(target.key.session == first.session && target.key.revision == first.revision &&
           target.key.process_object && target.key.identity.pid == TEST_GAME);
    if (module) {
        target.key.scope = VCO_CLOSE_MODULE;
        target.key.module_uid = 17;
        target.key.module_object = 0x5678; /* Caller-owned retained module fixture. */
        target.key.module_generation = target.module_snapshot;
        VcoEngineCloseKey stale = target.key;
        ++stale.module_generation;
        uint64_t stale_ticket;
        unsigned disposed = fake.dispose_calls;
        assert(!vco_service_close_submit(9, &stale, &stale_ticket));
        service_fake_tick();
        VcoVitaCloseResult denied;
        assert(!vco_service_close_receive(9, stale_ticket, &denied) &&
               denied.target.error == VCO_E_SESSION && fake.dispose_calls == disposed);
        assert(!vco_service_close_release(9, stale_ticket));
        service_fake_tick(); /* Finish the rejected request before reusing its slot. */
    }
    assert(!submit(request(first, 2, true)));
    uint64_t ui_ticket = ticket(), close_ticket, joined;
    assert(!vco_service_close_submit(1, &target.key, &close_ticket));
    assert(!vco_service_close_submit(2, &target.key, &joined) && joined == close_ticket);
    fake.owner_lookup_failure = true; /* No Shell authentication is needed for close. */
    unsigned disposed = fake.dispose_calls, snapshots = fake.modules_snapshots;
    service_fake_tick();
    VcoVitaCloseResult result;
    assert(!vco_service_close_receive(1, close_ticket, &result));
    assert(result.target_complete && !result.owner_stopped && !result.target.error &&
           !result.target.cleanup.entries && !result.target.cleanup.native &&
           vco_close_key_equal(&result.target.key, &target.key));
    assert(fake.writes == 1 && fake.dispose_calls == disposed + 1);
    for (unsigned i = 0; i < 5; ++i)
        service_fake_tick();
    assert(fake.writes == 1 && fake.modules_snapshots == snapshots && !fake.worker_waits);
    assert(!vco_service_close_release(1, close_ticket));
    service_fake_tick();
    assert(fake.dispose_calls == disposed + 1 && fake.writes == 1);
    assert(!vco_service_close_release(2, close_ticket)); /* Model native operation completed. */
    if (module)
        fake.module_uid = 29; /* The original module object/UID is already gone. */
    service_fake_tick();
    assert(fake.writes == 1 && fake.modules_snapshots == snapshots + (module ? 1u : 0u));
    fake.owner_lookup_failure = false;
    VcoControlReply old = receive(ui_ticket);
    assert(old.status == (module ? VCO_CONTROL_STALE : VCO_CONTROL_REJECTED) &&
           old.error == (module ? VCO_OK : VCO_E_SESSION) && fake.writes == 1);
    if (module) {
        VcoServiceStatus resumed = status();
        assert(resumed.session == first.session && resumed.revision > first.revision &&
               resumed.count == 1 && fake.dispose_calls == disposed + 1);
        VcoControlRequest query = request(resumed, 3, false);
        query.operation = VCO_CONTROL_QUERY;
        assert(!submit(query));
        service_fake_tick();
        VcoControlReply rows = receive(ticket());
        assert(rows.status == VCO_CONTROL_OK && rows.rows[0].state == VCO_ENTRY_OFF &&
               !rows.rows[0].desired && fake.writes == 1);
        observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE); /* Renew the departed Shell's lease. */
        service_fake_tick();
        assert(status().state == VCO_SERVICE_READY && fake.writes == 1);
        assert(command(resumed, 1, true).status == VCO_CONTROL_OK && fake.writes == 2);
        assert(command(resumed, 2, false).status == VCO_CONTROL_OK && fake.writes == 2);
    }
    assert(!vcoServiceLoad(NULL, 0));
    service_fake_tick();
    assert(!status().revision && fake.dispose_calls == disposed + (module ? 2u : 1u));
    game(TEST_GAME + 1);
    VcoServiceStatus next = bind_list(2, TEST_GAME + 1);
    assert(command(next, 1, true).status == VCO_CONTROL_OK && fake.writes == (module ? 3u : 2u) &&
           !fake.worker_waits && !fake.worker_deletes);
    assert(vco_service_close_receive(1, close_ticket, &result) == VCO_CLOSE_TICKET);
}
static void native_close_failure(bool module)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    service_fake_tick();
    service_fake_quarantine();
    VcoVitaCloseTarget target;
    assert(!vco_service_close_target(&target));
    if (module) {
        target.key.scope = VCO_CLOSE_MODULE;
        target.key.module_uid = 17;
        target.key.module_object = 0x5678;
        target.key.module_generation = target.module_snapshot;
    }
    uint64_t ticket;
    unsigned disposed = fake.dispose_calls;
    assert(!vco_service_close_submit(1, &target.key, &ticket));
    service_fake_tick();
    VcoVitaCloseResult result;
    assert(!vco_service_close_receive(1, ticket, &result) && result.target_complete &&
           result.target.error == VCO_E_RESTORE && result.target.cleanup.native == VCO_E_RESTORE &&
           !result.target.cleanup.entries);
    fake.fail_dispose = false;
    assert(!vco_service_close_submit(1, &target.key, &ticket));
    assert(!vco_service_close_receive(1, ticket, &result) && result.target.error == VCO_E_RESTORE);
    assert(!vco_service_close_release(1, ticket));
    for (unsigned i = 0; i < 5; ++i)
        service_fake_tick();
    assert(status().error == VCO_E_RESTORE && fake.dispose_calls == disposed + 1 &&
           fake.writes == 1);
    service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    game(TEST_GAME + 1);
    VcoServiceStatus next = bind_list(2, TEST_GAME + 1);
    assert(command(next, 1, true).status == VCO_CONTROL_OK && fake.writes == 2);
}
static VcoVitaCloseTarget module_target(void)
{
    VcoVitaCloseTarget target;
    assert(!vco_service_close_target(&target));
    target.key.scope = VCO_CLOSE_MODULE;
    target.key.module_uid = fake.module_uid;
    target.key.module_object = 0x5678;
    target.key.module_generation = target.module_snapshot;
    return target;
}
static void module_finish_failure(unsigned kind)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    service_fake_tick();
    VcoVitaCloseTarget target = module_target();
    uint64_t close_ticket;
    assert(!vco_service_close_submit(1, &target.key, &close_ticket));
    service_fake_tick();
    VcoVitaCloseResult closed;
    assert(!vco_service_close_receive(1, close_ticket, &closed) && closed.target_complete &&
           !closed.target.error && !closed.target.cleanup.entries && !closed.target.cleanup.native);
    unsigned snapshots = fake.modules_snapshots, maps = fake.module_maps, rearms = fake.rearms;
    fake.fail_module_snapshot = kind == 0;
    fake.fail_module_map = kind == 1;
    fake.fail_rearm = kind == 2;
    assert(!vco_service_close_release(1, close_ticket));
    service_fake_tick();
    VcoServiceStatus failed = status();
    assert(failed.session == first.session && failed.revision == first.revision &&
           failed.count == 1 && failed.state == VCO_SERVICE_ERROR &&
           failed.error == (kind == 2 ? VCO_E_SESSION : VCO_E_METADATA));
    assert(fake.modules_snapshots == snapshots + 1 && fake.module_maps == maps + (kind ? 1u : 0u) &&
           fake.rearms == rearms + (kind == 2 ? 1u : 0u) && fake.writes == 1);
    VcoControlReply denied = command(failed, 2, true);
    assert(denied.status == VCO_CONTROL_REJECTED && denied.error == VCO_E_SESSION);
    for (unsigned i = 0; i < 3; ++i)
        service_fake_tick();
    assert(fake.modules_snapshots == snapshots + 1 && fake.writes == 1 && fake.live_blocks == 2);
    /* Metadata errors never rewrite the already acquired cleanup ACK. */
    assert(!closed.target.error && !closed.target.cleanup.native);
    if (kind != 2) {
        fake.fail_module_snapshot = fake.fail_module_map = false;
        VcoServiceStatus next = bind_list(2, TEST_GAME);
        assert(next.session == first.session && next.revision > first.revision &&
               command(next, 1, true).status == VCO_CONTROL_OK && fake.writes == 2);
    }
}
static void module_finish_identity(bool during)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    service_fake_tick();
    VcoVitaCloseTarget target = module_target();
    uint64_t close_ticket;
    assert(!vco_service_close_submit(1, &target.key, &close_ticket));
    service_fake_tick();
    unsigned rearms = fake.rearms, snapshots = fake.modules_snapshots;
    assert(!vco_service_close_release(1, close_ticket));
    if (during)
        fake.module_snapshot_action = 1;
    else
        service_fake_replace_game();
    service_fake_tick();
    assert(fake.rearms == rearms && fake.modules_snapshots == snapshots + (during ? 1u : 0u) &&
           fake.writes == 1 && !status().revision);
    /* The backend does not query/reuse the original retired module tag. */
    fake.module_snapshot_action = 0;
    game(TEST_GAME + 1);
    VcoServiceStatus next = bind_list(2, TEST_GAME + 1);
    assert(next.session > first.session && command(next, 1, true).status == VCO_CONTROL_OK &&
           fake.writes == 2);
}
static void module_finish_event(bool lost)
{
    game(TEST_GAME);
    VcoServiceStatus first = bind_list(1, TEST_GAME);
    assert(command(first, 1, true).status == VCO_CONTROL_OK && fake.writes == 1);
    service_fake_tick();
    VcoVitaCloseTarget target = module_target();
    uint64_t close_ticket;
    assert(!vco_service_close_submit(1, &target.key, &close_ticket));
    service_fake_tick();
    unsigned rearms = fake.rearms, snapshots = fake.modules_snapshots;
    fake.module_snapshot_action = lost ? 3u : 2u;
    assert(!vco_service_close_release(1, close_ticket));
    service_fake_tick();
    assert(fake.rearms == rearms && fake.modules_snapshots == snapshots + 1 && fake.writes == 1);
    VcoServiceStatus final = status();
    assert(lost ? final.error == VCO_E_SESSION && final.revision == first.revision
                : !final.revision);
    fake.module_snapshot_action = 0;
    for (unsigned i = 0; i < 3; ++i)
        service_fake_tick();
    assert(fake.writes == 1 && fake.rearms == rearms && fake.modules_snapshots == snapshots + 1);
}
static void idle_modules(bool residue)
{
    game(TEST_GAME);
    VcoServiceStatus s = bind_list(1, TEST_GAME);
    assert(fake.modules_snapshots == 1 && fake.module_maps == 1);
    if (residue) {
        for (unsigned kind = 1; kind <= 7; ++kind) {
            /* Private ownership can exist without a public runtime receipt.
             * Even all-OFF must preserve stamps until it has been released. */
            service_fake_patch_residue(kind);
            assert(command(s, kind * 2 - 1, true).status == VCO_CONTROL_OK);
            assert(command(s, kind * 2, false).status == VCO_CONTROL_OK);
            assert(fake.modules_snapshots == 1 && fake.module_maps == 1);
        }
        service_fake_patch_residue(0);
        assert(command(s, 15, true).status == VCO_CONTROL_OK);
        assert(fake.modules_snapshots == 2 && fake.module_maps == 2);
        return;
    }
    assert(command(s, 1, true).status == VCO_CONTROL_OK);
    assert(fake.modules_snapshots == 2 && fake.module_maps == 2 && fake.writes == 1);
    assert(command(s, 2, true).status == VCO_CONTROL_OK);
    assert(fake.modules_snapshots == 2);
    observe(TEST_GAME, VCO_BACKGROUND_CANDIDATE);
    service_fake_tick();
    observe(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
    service_fake_tick();
    assert(fake.modules_snapshots == 2); /* Resume keeps its original mapping. */
    s = status();
    assert(command(s, 3, false).status == VCO_CONTROL_OK);
    assert(command(s, 4, true).status == VCO_CONTROL_OK);
    assert(fake.modules_snapshots == 3 && fake.module_maps == 3);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    service_fake_setup();
    if (!strcmp(argv[1], "authority_pause"))
        authority_pause();
    else if (!strcmp(argv[1], "idle_modules"))
        idle_modules(false);
    else if (!strcmp(argv[1], "idle_module_residue"))
        idle_modules(true);
    else if (!strcmp(argv[1], "authorization"))
        authorization();
    else if (!strcmp(argv[1], "buffers"))
        buffers();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else if (!strcmp(argv[1], "receipts"))
        receipts();
    else if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "permanent_loss"))
        permanent_loss();
    else if (!strcmp(argv[1], "rearm"))
        rearm();
    else if (!strcmp(argv[1], "native_state"))
        native_state();
    else if (!strcmp(argv[1], "scoped_reload"))
        scoped_reload();
    else if (!strcmp(argv[1], "scoped_failure"))
        scoped_failure();
    else if (!strcmp(argv[1], "native_close"))
        native_close(false);
    else if (!strcmp(argv[1], "native_module_close"))
        native_close(true);
    else if (!strcmp(argv[1], "native_close_failure"))
        native_close_failure(false);
    else if (!strcmp(argv[1], "native_module_close_failure"))
        native_close_failure(true);
    else if (!strcmp(argv[1], "module_finish_snapshot"))
        module_finish_failure(0);
    else if (!strcmp(argv[1], "module_finish_map"))
        module_finish_failure(1);
    else if (!strcmp(argv[1], "module_finish_rearm"))
        module_finish_failure(2);
    else if (!strcmp(argv[1], "module_finish_identity"))
        module_finish_identity(false);
    else if (!strcmp(argv[1], "module_finish_racing_identity"))
        module_finish_identity(true);
    else if (!strcmp(argv[1], "module_finish_exit"))
        module_finish_event(false);
    else if (!strcmp(argv[1], "module_finish_lost"))
        module_finish_event(true);
    else if (!strcmp(argv[1], "upload_instance"))
        upload_instance();
    else if (!strcmp(argv[1], "owner_departure"))
        owner_departure();
    else if (!strcmp(argv[1], "shell_restart"))
        shell_restart(false);
    else if (!strcmp(argv[1], "shell_replacement"))
        shell_restart(true);
    else if (!strcmp(argv[1], "rpc_restart"))
        copy_restart(false);
    else if (!strcmp(argv[1], "upload_restart"))
        copy_restart(true);
    else if (!strcmp(argv[1], "bind_failures"))
        bind_failures();
    else if (!strcmp(argv[1], "coexistence"))
        coexistence();
    else if (!strcmp(argv[1], "quarantine"))
        quarantine();
    else if (!strcmp(argv[1], "quarantine_chain"))
        quarantine_chain();
    else if (!strcmp(argv[1], "retired_cleanup"))
        retired_cleanup(false);
    else if (!strcmp(argv[1], "owner_exit_cleanup"))
        owner_retirement(0);
    else if (!strcmp(argv[1], "owner_replaced_target"))
        owner_retirement(1);
    else if (!strcmp(argv[1], "owner_pending_retire"))
        owner_retirement(2);
    else if (!strcmp(argv[1], "owner_retire_poison"))
        owner_retirement(3);
    else if (!strcmp(argv[1], "owner_unknown_target"))
        owner_retirement(4);
    else if (!strcmp(argv[1], "owner_restart_retire"))
        owner_retirement(5);
    else if (!strcmp(argv[1], "owner_reference_before"))
        owner_reference_failure(false);
    else if (!strcmp(argv[1], "owner_reference_after"))
        owner_reference_failure(true);
    else if (!strcmp(argv[1], "fault_exit_cleanup"))
        fault_exit_cleanup();
    else if (!strcmp(argv[1], "independent_unbind"))
        independent_unbind();
    else if (!strcmp(argv[1], "retired_poison"))
        retired_cleanup(true);
    else if (!strcmp(argv[1], "support_failure"))
        support_failure();
    else if (!strcmp(argv[1], "concurrent_scopes"))
        concurrent_scopes();
    else if (!strcmp(argv[1], "runtime_restart"))
        runtime_restart(false);
    else if (!strcmp(argv[1], "runtime_restore"))
        runtime_restart(true);
    else if (!strcmp(argv[1], "trace_transport"))
        trace_transport();
    else if (!strcmp(argv[1], "trace_freeze"))
        trace_freeze();
    else if (!strcmp(argv[1], "long_foreground"))
        long_observation(1);
    else if (!strcmp(argv[1], "long_background"))
        long_observation(2);
    else if (!strcmp(argv[1], "long_expired"))
        long_observation(3);
    else if (!strcmp(argv[1], "long_unknown"))
        long_observation(4);
    else if (!strcmp(argv[1], "long_silent"))
        long_observation(0);
    else if (!strcmp(argv[1], "input_disabled"))
        input_disabled();
    else if (!strcmp(argv[1], "runtime_same_owner"))
        runtime_same_owner();
    else if (!strcmp(argv[1], "terminal_empty"))
        terminal_empty();
    else if (!strncmp(argv[1], "terminal_", 9)) {
        const char *cases[] = {"clean",          "pending",          "replaced",
                               "unknown",        "native_poison",    "free_before",
                               "free_after",     "reference_before", "reference_after",
                               "memory_failure", "public_failure",   "copy_retry",
                               "handoff",        "reentrant",        "probe_before",
                               "probe_after",    "queued_upload"};
        unsigned mode;
        for (mode = 0; mode < sizeof(cases) / sizeof(*cases); ++mode)
            if (!strcmp(argv[1] + 9, cases[mode]))
                break;
        assert(mode < sizeof(cases) / sizeof(*cases));
        terminal_cleanup(mode);
    } else if (!strncmp(argv[1], "startup_", 8))
        startup_failure(argv[1]);
    else
        assert(0);
    assert(fake.entered == fake.exited && !fake.depth);
    puts("kernel service test passed");
    return 0;
}
