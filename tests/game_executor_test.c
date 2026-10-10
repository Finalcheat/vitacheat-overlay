#include "agent/broker.h"
#include "agent/executor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BASE UINT32_C(0x81000000)
static VcoAgentBroker broker;
static VcoGameExecutor executor;
static VcoAgentBinding binding;
static unsigned char staging[VCO_UPLOAD_MAX], packet[VCO_UPLOAD_MAX], payload[VCO_UPLOAD_MAX];
static VcoCheatEntry source_entries[VCO_ENGINE_ENTRIES];
static VcoCode source_codes[VCO_UPLOAD_CODES];
static char names[65536];
static VcoCheatFile source;
static size_t length;
static uint8_t game[64], saved[4];
static unsigned writes, injections, releases, allocations, frees;
static bool alloc_fail, release_fail, patch_live;
static uint64_t now = 50000;
static void *allocate(void *context, size_t size)
{
    (void)context;
    if (alloc_fail)
        return NULL;
    ++allocations;
    return malloc(size);
}
static void deallocate(void *context, void *ptr)
{
    (void)context;
    assert(ptr);
    ++frees;
    free(ptr);
}
static int alive(void *context, uint64_t session)
{
    (void)context;
    return session == binding.session && !executor.revoked;
}
static int read_bytes(void *context, uint64_t token, uint32_t address, uint8_t *out, size_t size)
{
    (void)context;
    if (!alive(NULL, token) || address < BASE || (uint64_t)address + size > BASE + sizeof(game))
        return -1;
    memcpy(out, game + (address - BASE), size);
    return 0;
}
static int write_bytes(void *context, uint64_t token, uint32_t address, const uint8_t *in,
                       size_t size)
{
    (void)context;
    if (!alive(NULL, token) || address < BASE || (uint64_t)address + size > BASE + sizeof(game))
        return -1;
    ++writes;
    memcpy(game + (address - BASE), in, size);
    return 0;
}
static int inject(void *context, uint64_t token, uint32_t address, const uint8_t *in, size_t size,
                  int32_t *handle)
{
    assert(!patch_live && address == BASE && size <= sizeof(saved));
    assert(!read_bytes(context, token, address, saved, size));
    assert(!write_bytes(context, token, address, in, size));
    patch_live = true;
    ++injections;
    *handle = 123;
    return 0;
}
static int release_patch(void *context, uint64_t token, int32_t handle)
{
    assert(patch_live && handle == 123);
    ++releases;
    patch_live = false; /* User release consumes its UID even on failure. */
    return release_fail ? -1 : write_bytes(context, token, BASE, saved, sizeof(saved));
}
static int module(void *context, uint64_t token, unsigned index, unsigned segment, uint32_t *base,
                  uint32_t *size)
{
    (void)context;
    (void)index;
    (void)segment;
    if (!alive(NULL, token))
        return VCO_ACCESS_SESSION;
    *base = BASE;
    *size = sizeof(game);
    return 0;
}
static int pad(void *context, uint64_t token, uint32_t *device, uint32_t *buttons)
{
    (void)context;
    if (!alive(NULL, token))
        return VCO_ACCESS_SESSION;
    *device = *buttons = 0;
    return 0;
}
static void setup(void)
{
    assert(!vco_broker_init(&broker, 99, staging, sizeof(staging)));
    assert(!vco_broker_open(&broker, 77, "PCSB00031", &binding));
    assert(!vco_broker_attach(&broker, 77, &binding));
    assert(!vco_broker_observe(&broker, binding.session, true));
    VcoAgentAllocator allocator = {NULL, allocate, deallocate};
    VcoCheatMemory memory = {NULL,   alive, read_bytes, write_bytes,
                             module, pad,   inject,     release_patch};
    assert(!vco_game_executor_init(&executor, &binding, allocator, memory, NULL, NULL));
    source =
        (VcoCheatFile){source_entries, VCO_ENGINE_ENTRIES, 0, source_codes, VCO_UPLOAD_CODES, 0,
                       names,          sizeof(names),      0};
}
static void encode(const char *text, uint64_t request)
{
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, strlen(text), &source, &diagnostic));
    assert(!vco_upload_encode(packet, sizeof(packet), request, binding.session, (uint32_t)request,
                              binding.pid, binding.title_id, &source, &length));
}
static VcoAgentResult deliver(uint64_t ticket, bool ack)
{
    VcoAgentMessage message;
    VcoAgentResult result, received;
    assert(!vco_broker_poll(&broker, 77, binding.session, &message, payload, sizeof(payload)));
    assert(message.ticket == ticket);
    now += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &message, payload, message.length, now, &result));
    assert(!vco_broker_publish(&broker, 77, &result));
    assert(!vco_broker_receive(&broker, 99, ticket, &received));
    assert(!memcmp(&received, &result, sizeof(result)));
    if (ack)
        assert(!vco_broker_ack(&broker, 99, ticket));
    return result;
}
static VcoAgentResult upload(bool ack)
{
    uint64_t ticket;
    assert(!vco_broker_load(&broker, 99, packet, length, &ticket));
    return deliver(ticket, ack);
}
static VcoAgentResult control(unsigned entry, bool enabled, uint64_t request)
{
    VcoControlRequest command = {.request = request,
                                 .session = binding.session,
                                 .revision = executor.engine.revision,
                                 .abi = VCO_CONTROL_ABI,
                                 .size = sizeof(command),
                                 .operation = VCO_CONTROL_SET,
                                 .entry = entry,
                                 .enabled = enabled,
                                 .count = 1};
    uint64_t ticket;
    assert(!vco_broker_submit(&broker, 99, &command, &ticket));
    return deliver(ticket, true);
}
static void tick(void)
{
    VcoAgentMessage message;
    VcoAgentResult result;
    assert(!vco_broker_poll(&broker, 77, binding.session, &message, payload, sizeof(payload)));
    assert(message.operation == VCO_AGENT_IDLE);
    now += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &message, NULL, 0, now, &result));
}
static void data(void)
{
    encode("_V1 Value\n$0200 81000000 00000040\n", 1);
    VcoAgentResult result = upload(true);
    assert(!result.error && result.status.count == 1 && result.status.catalog_revision == 1);
    assert(!writes && executor.engine.rows[0].state == VCO_ENTRY_OFF);
    control(0, true, 1);
    assert(game[0] == 0x40 && executor.engine.rows[0].state == VCO_ENTRY_ON);
    tick();
    assert(writes == 2);
    control(0, false, 2);
    unsigned before = writes;
    game[0] = 9;
    tick();
    assert(game[0] == 9 && writes == before);
    assert(!vco_game_executor_stop(&executor));
    assert(allocations == frees);
}
static void patches(void)
{
    game[0] = 0xaa;
    encode("_V0 Patch\n$A200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    assert(game[0] == 0x40 && patch_live && injections == 1);
    tick();
    assert(injections == 1);
    control(0, false, 2);
    assert(game[0] == 0xaa && releases == 1 && !patch_live);
    control(0, true, 3);
    assert(!vco_game_executor_stop(&executor));
    assert(game[0] == 0xaa && releases == 2 && allocations == frees);
    assert(!vco_game_executor_stop(&executor) && releases == 2);
}
static void pause_game(void)
{
    game[0] = 0xaa;
    encode("_V0 Patch\n$A200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    assert(patch_live);
    assert(!vco_broker_observe(&broker, binding.session, false));
    tick();
    assert(game[0] == 0xaa && releases == 1 && executor.engine.rows[0].state == VCO_ENTRY_PAUSED);
    unsigned before = writes;
    tick();
    assert(writes == before);
    assert(!vco_broker_observe(&broker, binding.session, true));
    tick();
    assert(patch_live && injections == 2 && executor.engine.rows[0].state == VCO_ENTRY_ON);
    assert(!vco_game_executor_stop(&executor) && releases == 2 && allocations == frees);
}
static void retry(void)
{
    encode("_V0 Value\n$0200 81000000 00000040\n", 1);
    uint64_t ticket, repeated;
    assert(!vco_broker_load(&broker, 99, packet, length, &ticket));
    assert(!vco_broker_load(&broker, 99, packet, length, &repeated) && repeated == ticket);
    VcoAgentMessage message;
    assert(!vco_broker_poll(&broker, 77, binding.session, &message, payload, sizeof(payload)));
    VcoAgentResult result;
    assert(!vco_game_executor_handle(&executor, &message, payload, message.length, now, &result));
    unsigned before = allocations;
    VcoAgentResult again;
    assert(!vco_game_executor_handle(&executor, &message, payload, message.length, now, &again));
    assert(allocations == before && !memcmp(&result, &again, sizeof(result)));
    assert(!vco_broker_publish(&broker, 77, &result));
    assert(!vco_broker_publish(&broker, 77, &again));
    assert(!vco_broker_ack(&broker, 99, ticket));
    control(0, true, 1);
    uint64_t revision = executor.engine.revision;
    assert(!upload(true).error); /* A retry with a new delivery ticket preserves ON. */
    assert(executor.engine.revision == revision && executor.engine.rows[0].state == VCO_ENTRY_ON);
    assert(!vco_game_executor_stop(&executor));
}
static void replacement(void)
{
    game[0] = 0xaa;
    encode("_V0 Patch\n$A200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    uint64_t revision = executor.engine.revision;
    encode("_V1 New\n$0200 81000004 00000050\n", 2);
    assert(!upload(true).error);
    assert(releases == 1 && game[0] == 0xaa && !game[4]);
    assert(executor.engine.revision > revision && executor.engine.rows[0].state == VCO_ENTRY_OFF);
    uint64_t ticket;
    assert(!vco_broker_clear(&broker, 99, binding.session, &ticket));
    assert(!deliver(ticket, true).error && !executor.arena && !executor.engine.bound);
    assert(allocations == frees);
    assert(!vco_game_executor_stop(&executor));
}
static void failures(void)
{
    encode("_V0 Patch\n$A200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    void *arena = executor.arena;
    encode("_V0 New\n$0200 81000004 00000050\n", 2);
    alloc_fail = true;
    assert(upload(true).error == VCO_E_LIMIT && executor.arena == arena && patch_live);
    alloc_fail = false;
    packet[length - 1] ^= 0x80;
    assert(upload(true).error == VCO_E_ARGUMENT && executor.arena == arena && !releases);
    packet[length - 1] ^= 0x80;
    release_fail = true;
    assert(upload(true).error == VCO_E_RESTORE && executor.arena == arena);
    assert(releases == 1 && executor.engine.closing && executor.engine.blocked_restore);
    assert(vco_game_executor_stop(&executor) == VCO_E_RESTORE && releases == 1);
    /* Simulate process storage reclaim only; no retry of a consumed native UID. */
    deallocate(NULL, executor.arena);
    executor.arena = NULL;
    assert(allocations == frees);
}
static void termination(void)
{
    encode("_V0 Value\n$0200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    unsigned before = writes;
    assert(!vco_broker_end(&broker, binding.session));
    VcoAgentMessage message;
    assert(vco_broker_poll(&broker, 77, binding.session, &message, payload, sizeof(payload)) ==
           VCO_AGENT_STALE);
    vco_game_executor_revoke(&executor);
    message = (VcoAgentMessage){
        .session = binding.session, .abi = VCO_AGENT_ABI, .size = sizeof(message), .runnable = 1};
    VcoAgentResult result;
    assert(vco_game_executor_handle(&executor, &message, NULL, 0, now + 50000, &result) ==
           VCO_E_SESSION);
    assert(writes == before && !releases);
    assert(vco_game_executor_stop(&executor) == VCO_E_SESSION);
    deallocate(NULL, executor.arena);
    executor.arena = NULL;
    assert(allocations == frees);
}
static void termination_patch(void)
{
    encode("_V0 Patch\n$A200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    assert(patch_live && injections == 1);
    assert(!vco_broker_end(&broker, binding.session));
    vco_game_executor_revoke(&executor);
    assert(vco_game_executor_stop(&executor) == VCO_E_SESSION);
    assert(!releases && patch_live); /* Host does not pretend to execute taiHEN's exit hook. */
    deallocate(NULL, executor.arena);
    executor.arena = NULL;
    assert(allocations == frees);
}
static void identity(void)
{
    VcoAgentBinding old = binding, next;
    assert(!vco_broker_end(&broker, old.session));
    assert(!vco_broker_open(&broker, 77, "PCSB00031", &next));
    assert(next.session > old.session);
    assert(vco_broker_attach(&broker, 77, &old) == VCO_AGENT_STALE);
    assert(vco_broker_attach(&broker, 88, &next) == VCO_AGENT_OWNER);
    assert(!vco_broker_attach(&broker, 77, &next));
    encode("_V0 Value\n$0200 81000000 00000040\n", 1);
    uint64_t ticket = 999;
    assert(vco_broker_load(&broker, 99, packet, length, &ticket) == VCO_AGENT_STALE &&
           ticket == 999);
    assert(vco_broker_end(&broker, old.session) == VCO_AGENT_STALE);
    assert(vco_broker_observe(&broker, old.session, true) == VCO_AGENT_STALE);
    assert(vco_broker_clear(&broker, 99, old.session, &ticket) == VCO_AGENT_STALE);
    assert(!vco_game_executor_stop(&executor));
}
static void packets(void)
{
    VcoAgentMessage message = {
        .session = binding.session, .abi = VCO_AGENT_ABI, .size = sizeof(message), .runnable = 1};
    VcoAgentResult result;
    message.reserved = 1;
    assert(vco_game_executor_handle(&executor, &message, NULL, 0, now, &result) == VCO_E_ARGUMENT);
    message.reserved = 0;
    message.runnable = 2;
    assert(vco_game_executor_handle(&executor, &message, NULL, 0, now, &result) == VCO_E_ARGUMENT);
    message.runnable = 1;
    message.session++;
    assert(vco_game_executor_handle(&executor, &message, NULL, 0, now, &result) == VCO_E_SESSION);
    encode("_V0 Value\n$0200 81000000 00000040\n", 1);
    uint64_t ticket;
    assert(!vco_broker_load(&broker, 99, packet, length, &ticket));
    unsigned char unchanged[16];
    memset(unchanged, 0x5a, sizeof(unchanged));
    assert(vco_broker_poll(&broker, 77, binding.session, &message, unchanged, sizeof(unchanged)) ==
           VCO_AGENT_LIMIT);
    for (unsigned i = 0; i < sizeof(unchanged); ++i)
        assert(unchanged[i] == 0x5a);
    assert(vco_broker_ack(&broker, 99, ticket) == VCO_AGENT_BUSY);
    assert(vco_broker_receive(&broker, 99, ticket, &result) == VCO_AGENT_EMPTY);
    memset(packet, 0, length); /* Poll uses the owned upload copy. */
    result = deliver(ticket, false);
    assert(!result.error);
    VcoAgentResult corrupted = result;
    corrupted.status.pid++;
    assert(vco_broker_publish(&broker, 77, &corrupted) == VCO_AGENT_ARGUMENT);
    corrupted = result;
    corrupted.ticket++;
    assert(vco_broker_publish(&broker, 77, &corrupted) == VCO_AGENT_STALE);
    assert(!vco_broker_ack(&broker, 99, ticket));
    assert(vco_broker_receive(&broker, 99, ticket, &result) == VCO_AGENT_STALE);
    assert(!vco_game_executor_stop(&executor));
}
static void ownership(void)
{
    encode("_V0 Value\n$0200 81000000 00000040\n", 1);
    uint64_t ticket = 999;
    assert(vco_broker_load(&broker, 77, packet, length, &ticket) == VCO_AGENT_OWNER &&
           ticket == 999);
    VcoServiceStatus status;
    assert(vco_broker_status(&broker, 77, &status) == VCO_AGENT_OWNER);
    assert(!vco_broker_load(&broker, 99, packet, length, &ticket));
    VcoAgentMessage message;
    assert(vco_broker_poll(&broker, 99, binding.session, &message, payload, sizeof(payload)) ==
           VCO_AGENT_OWNER);
    encode("_V0 New\n$0200 81000004 00000050\n", 2);
    uint64_t other;
    assert(vco_broker_load(&broker, 99, packet, length, &other) == VCO_AGENT_BUSY);
    assert(!deliver(ticket, true).error);
    assert(!vco_broker_observe(&broker, binding.session, false));
    assert(!vco_broker_status(&broker, 99, &status) && status.state == VCO_SERVICE_BACKGROUND);
    control(0, true, 1);
    assert(!writes && executor.engine.rows[0].state == VCO_ENTRY_OFF);
    assert(!vco_game_executor_stop(&executor));
}
static void replies(void)
{
    encode("_V0 Value\n$0200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    VcoControlRequest request = {.request = 1,
                                 .session = binding.session,
                                 .revision = executor.engine.revision,
                                 .abi = VCO_CONTROL_ABI,
                                 .size = sizeof(request),
                                 .operation = VCO_CONTROL_QUERY,
                                 .count = 1};
    uint64_t ticket;
    assert(!vco_broker_submit(&broker, 99, &request, &ticket));
    VcoAgentMessage message;
    assert(!vco_broker_poll(&broker, 77, binding.session, &message, payload, sizeof(payload)));
    VcoAgentResult result;
    assert(!vco_game_executor_handle(&executor, &message, payload, message.length, now, &result));
    for (unsigned i = 0; i < 7; ++i) {
        VcoAgentResult corrupt = result;
        switch (i) {
        case 0:
            corrupt.control.count = VCO_CONTROL_PAGE + 1;
            break;
        case 1:
            corrupt.control.first = UINT32_MAX;
            break;
        case 2:
            corrupt.control.rows[0].state = VCO_ENTRY_UNSUPPORTED + 1;
            break;
        case 3:
            corrupt.control.rows[0].desired = 2;
            break;
        case 4:
            corrupt.control.revision++;
            break;
        case 5:
            corrupt.status.content_key = 0;
            break;
        case 6:
            corrupt.status.error = VCO_E_IO;
            break;
        }
        assert(vco_broker_publish(&broker, 77, &corrupt) == VCO_AGENT_ARGUMENT);
    }
    assert(!vco_broker_publish(&broker, 77, &result));
    assert(!vco_broker_ack(&broker, 99, ticket));
    assert(!vco_game_executor_stop(&executor));
}
static void capacity(void)
{
    source.entry_count = 1;
    source.code_count = VCO_UPLOAD_CODES;
    source.entries[0] = (VcoCheatEntry){.count = VCO_UPLOAD_CODES};
    for (unsigned i = 0; i < VCO_UPLOAD_CODES; ++i)
        source.codes[i] = (VcoCode){0x0200, BASE, i, i + 1};
    assert(!vco_upload_encode(packet, sizeof(packet), 1, binding.session, 1, binding.pid,
                              binding.title_id, &source, &length));
    assert(!upload(true).error);
    assert(executor.file.code_count == VCO_UPLOAD_CODES &&
           executor.engine.slots[0].program.count == VCO_UPLOAD_CODES &&
           executor.engine.rows[0].state == VCO_ENTRY_OFF);
    assert(!vco_game_executor_stop(&executor));
    broker.next_session = UINT64_MAX;
    VcoAgentBinding out = binding;
    assert(vco_broker_open(&broker, 77, "PCSB00031", &out) == VCO_AGENT_LIMIT);
    assert(out.session == binding.session && broker.binding.session == binding.session);
}
static void fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    char text[65536];
    size_t size = fread(text, 1, sizeof(text), file);
    assert(!ferror(file) && size < sizeof(text));
    fclose(file);
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, size, &source, &diagnostic));
    assert(source.entry_count == 31 && source.code_count == 982);
    assert(!vco_upload_encode(packet, sizeof(packet), 1, binding.session, 1, binding.pid,
                              binding.title_id, &source, &length));
    assert(!upload(true).error);
    for (unsigned i = 0; i < executor.engine.count; ++i)
        assert(executor.engine.rows[i].state == VCO_ENTRY_OFF);
    assert(executor.engine.slots[30].program.count == 199);
    assert(!vco_game_executor_stop(&executor) && allocations == frees);
}
static void module_boundary(const char *mode)
{
    game[0] = 0xaa;
    encode("_V1 Patch\n$A200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    uint64_t revision = executor.engine.revision;
    void *arena = executor.arena;
    unsigned old_allocations = allocations;
    VcoControlRequest old = {.request = 2,
                             .session = binding.session,
                             .revision = revision,
                             .abi = VCO_CONTROL_ABI,
                             .size = sizeof(old),
                             .operation = VCO_CONTROL_SET,
                             .entry = 0,
                             .count = 1,
                             .enabled = 1};
    uint64_t old_ticket;
    assert(!vco_broker_submit(&broker, 99, &old, &old_ticket));
    VcoModuleCall call, joined[VCO_MODULE_CALLS];
    if (!strcmp(mode, "module_limits")) {
        broker.next_ticket = UINT64_MAX;
        call = (VcoModuleCall){9, 9, 9};
        assert(vco_broker_module_begin(&broker, 77, &call) == VCO_AGENT_LIMIT);
        assert(broker.message.ticket == old_ticket && call.claim == 9);
        broker.next_ticket = old_ticket;
        broker.next_claim = UINT64_MAX;
        assert(vco_broker_module_begin(&broker, 77, &call) == VCO_AGENT_LIMIT);
        assert(broker.message.ticket == old_ticket && !vco_broker_module_busy(&broker));
        assert(!vco_game_executor_stop(&executor) && releases == 1 && allocations == frees);
        return;
    }
    assert(vco_broker_module_begin(&broker, 78, &call) == VCO_AGENT_OWNER);
    assert(!vco_broker_module_begin(&broker, 77, &call));
    assert(call.ticket > old_ticket && call.claim && vco_broker_module_busy(&broker));
    assert(vco_broker_submit(&broker, 99, &old, &old_ticket) == VCO_AGENT_BUSY);
    assert(vco_broker_load(&broker, 99, packet, length, &old_ticket) == VCO_AGENT_BUSY);
    assert(vco_broker_clear(&broker, 99, binding.session, &old_ticket) == VCO_AGENT_BUSY);
    VcoError error;
    assert(vco_broker_module_ready(&broker, &call, &error) == VCO_AGENT_EMPTY);
    bool timeout = !strcmp(mode, "module_timeout");
    bool failure = !strcmp(mode, "module_failure");
    bool owner_change = !strcmp(mode, "module_owner");
    bool terminal_case = !strcmp(mode, "module_terminal") || !strcmp(mode, "module_replacement");
    if (!strcmp(mode, "module_join")) {
        for (unsigned i = 0; i < VCO_MODULE_CALLS - 1; ++i) {
            assert(!vco_broker_module_begin(&broker, 77, &joined[i]));
            assert(joined[i].ticket == call.ticket && joined[i].claim != call.claim);
        }
        assert(vco_broker_module_begin(&broker, 77, &joined[VCO_MODULE_CALLS - 1]) ==
               VCO_AGENT_LIMIT);
    }
    if (owner_change)
        assert(!vco_broker_rebind_owner(&broker, 100));
    if (timeout) {
        assert(!vco_broker_module_end(&broker, &call));
        assert(vco_broker_module_busy(&broker)); /* Late cleanup still required. */
    }
    if (terminal_case) {
        assert(!vco_broker_end(&broker, binding.session));
        assert(!vco_broker_module_busy(&broker));
        assert(vco_broker_module_end(&broker, &call) == VCO_AGENT_STALE);
        if (!strcmp(mode, "module_replacement")) {
            VcoAgentBinding replacement;
            assert(!vco_broker_open(&broker, 77, "PCSB00031", &replacement));
            assert(replacement.session != call.session);
            assert(!vco_broker_attach(&broker, 77, &replacement));
            VcoModuleCall current;
            assert(!vco_broker_module_begin(&broker, 77, &current));
            assert(vco_broker_module_end(&broker, &call) == VCO_AGENT_STALE);
            assert(vco_broker_module_busy(&broker));
            assert(!vco_broker_end(&broker, replacement.session));
        }
        vco_game_executor_revoke(&executor);
        /* Process death never restores via this executor. */
        assert(releases == 0 && patch_live);
        patch_live = false;
        deallocate(NULL, executor.arena);
        executor.arena = NULL;
        return;
    }
    release_fail = failure;
    VcoAgentMessage message;
    VcoAgentResult result, again;
    assert(!vco_broker_poll(&broker, 77, binding.session, &message, payload, sizeof(payload)));
    assert(message.operation == VCO_AGENT_MODULE_PAUSE && !message.length && !message.runnable);
    assert(!vco_game_executor_handle(&executor, &message, NULL, 0, now, &result));
    assert(releases == 1 && !patch_live && executor.arena == arena &&
           allocations == old_allocations);
    assert(!vco_game_executor_handle(&executor, &message, NULL, 0, now, &again));
    assert(!memcmp(&result, &again, sizeof(result)) && releases == 1);
    assert(!vco_broker_publish(&broker, 77, &result));
    if (!timeout) {
        assert(!vco_broker_module_ready(&broker, &call, &error));
        assert(error == (failure ? VCO_E_RESTORE : VCO_OK));
    }
    assert(!vco_broker_ack(&broker, broker.owner, result.ticket));
    if (!failure) {
        assert(!result.error && game[0] == 0xaa && executor.engine.revision > revision);
        assert(result.status.content_key && result.status.catalog_revision == 1);
        assert(executor.engine.rows[0].state == VCO_ENTRY_OFF && !executor.engine.rows[0].desired);
    } else {
        assert(result.error == VCO_E_RESTORE && executor.engine.blocked_restore);
        assert(executor.engine.revision == revision);
    }
    if (!strcmp(mode, "module_join")) {
        for (unsigned i = 0; i < VCO_MODULE_CALLS - 1; ++i) {
            assert(!vco_broker_module_end(&broker, &joined[i]));
            assert(vco_broker_module_busy(&broker));
        }
    }
    if (!timeout) {
        assert(!vco_broker_module_end(&broker, &call));
        assert(vco_broker_module_end(&broker, &call) == VCO_AGENT_STALE);
    }
    assert(!vco_broker_module_busy(&broker));
    if (owner_change) {
        assert(!vco_broker_poll(&broker, 77, binding.session, &message, payload, sizeof(payload)));
        assert(message.operation == VCO_AGENT_CLEAR);
        assert(!vco_game_executor_handle(&executor, &message, NULL, 0, now, &result));
        assert(!vco_broker_publish(&broker, 77, &result));
        assert(!vco_broker_ack(&broker, 100, result.ticket));
        assert(!executor.engine.bound && !executor.arena);
    } else if (!failure) {
        assert(!vco_broker_submit(&broker, 99, &old, &old_ticket));
        assert(deliver(old_ticket, true).error == VCO_E_SESSION);
        assert(!patch_live && injections == 1); /* Old ON cannot reappear. */
        assert(!vco_broker_observe(&broker, binding.session, true));
        control(0, true, 3);
        assert(patch_live && injections == 2);
        assert(!vco_game_executor_stop(&executor));
    } else {
        assert(vco_game_executor_stop(&executor) == VCO_E_RESTORE && releases == 1);
        deallocate(NULL, executor.arena);
        executor.arena = NULL;
    }
    assert(allocations == frees);
}
static void stopped_receipt(const char *mode)
{
    game[0] = 0xaa;
    encode("_V1 Patch\n$A200 81000000 00000040\n", 1);
    assert(!upload(true).error);
    control(0, true, 1);
    VcoAgentResult stopped;
    assert(vco_game_executor_stopped_result(&executor, &stopped) == VCO_E_SESSION);
    VcoModuleCall call = {0};
    if (!strcmp(mode, "stopped_module") || !strcmp(mode, "stopped_owner")) {
        assert(!vco_broker_module_begin(&broker, 77, &call));
        if (!strcmp(mode, "stopped_owner"))
            assert(!vco_broker_rebind_owner(&broker, 100));
    }
    if (!strcmp(mode, "stopped_restore_failure")) {
        release_fail = true;
        assert(vco_game_executor_stop(&executor) == VCO_E_RESTORE);
        assert(vco_game_executor_stopped_result(&executor, &stopped) == VCO_E_SESSION);
        assert(!broker.quiescent && !executor.stopped && releases == 1);
        deallocate(NULL, executor.arena);
        executor.arena = NULL;
        return;
    }
    assert(!vco_game_executor_stop(&executor));
    assert(game[0] == 0xaa && releases == 1 && !patch_live && allocations == frees);
    assert(!vco_game_executor_stopped_result(&executor, &stopped));
    assert(!stopped.ticket && stopped.operation == VCO_AGENT_STOPPED);
    if (!strcmp(mode, "stopped_stale")) {
        VcoAgentBinding current;
        assert(!vco_broker_open(&broker, 77, "PCSB00031", &current));
        assert(!vco_broker_attach(&broker, 77, &current));
        assert(vco_broker_publish(&broker, 77, &stopped) == VCO_AGENT_STALE);
        assert(!broker.quiescent && broker.attached);
        return;
    }
    assert(vco_broker_publish(&broker, 99, &stopped) == VCO_AGENT_OWNER);
    VcoAgentResult forged = stopped;
    forged.ticket = 1;
    assert(vco_broker_publish(&broker, 77, &forged) == VCO_AGENT_ARGUMENT);
    forged = stopped;
    forged.control.error = 1;
    assert(vco_broker_publish(&broker, 77, &forged) == VCO_AGENT_ARGUMENT);
    assert(!vco_broker_publish(&broker, 77, &stopped));
    assert(broker.quiescent && !broker.attached && !broker.runnable);
    assert(!vco_broker_publish(&broker, 77, &stopped));
    assert(vco_broker_attach(&broker, 77, &binding) == VCO_AGENT_CLOSED);
    if (call.claim) {
        VcoError cleanup;
        assert(!vco_broker_module_ready(&broker, &call, &cleanup) && !cleanup);
        assert(!vco_broker_module_end(&broker, &call));
        assert(!vco_broker_module_busy(&broker) && !broker.module_clear);
    }
    assert(!vco_broker_rebind_owner(&broker, 100));
    assert(!vco_broker_publish(&broker, 77, &stopped)); /* Immutable after Shell restart. */
    VcoServiceStatus status;
    assert(!vco_broker_status(&broker, 100, &status));
    assert(status.state == VCO_SERVICE_ERROR && status.error == VCO_E_SESSION && !status.revision);
    forged = stopped;
    ++forged.status.load_request;
    assert(vco_broker_publish(&broker, 77, &forged) == VCO_AGENT_ARGUMENT);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    setup();
    if (!strcmp(argv[1], "data"))
        data();
    else if (!strcmp(argv[1], "patches"))
        patches();
    else if (!strcmp(argv[1], "pause"))
        pause_game();
    else if (!strcmp(argv[1], "retry"))
        retry();
    else if (!strcmp(argv[1], "replacement"))
        replacement();
    else if (!strcmp(argv[1], "failures"))
        failures();
    else if (!strcmp(argv[1], "termination"))
        termination();
    else if (!strcmp(argv[1], "termination_patch"))
        termination_patch();
    else if (!strcmp(argv[1], "identity"))
        identity();
    else if (!strcmp(argv[1], "packets"))
        packets();
    else if (!strcmp(argv[1], "replies"))
        replies();
    else if (!strcmp(argv[1], "ownership"))
        ownership();
    else if (!strcmp(argv[1], "capacity"))
        capacity();
    else if (!strcmp(argv[1], "fixture"))
        fixture(argv[2]);
    else if (!strncmp(argv[1], "module_", 7))
        module_boundary(argv[1]);
    else if (!strncmp(argv[1], "stopped_", 8))
        stopped_receipt(argv[1]);
    else
        assert(!"unknown case");
    printf("game executor %s passed\n", argv[1]);
    return 0;
}
