#include "execution/engine.h"
#include "execution/control.h"
#include <assert.h>
#include <string.h>

#define BASE UINT32_C(0x81000000)
static VcoEngine engine;
static VcoSession session;
static VcoIdentity identity = {42, 99, "PCSB00031"};
static VcoCheatEntry entries[4];
static VcoCode codes[8];
static VcoCheatOp ops[8];
static VcoPatch receipts[8];
static char names[128];
static VcoCheatFile file;
static struct {
    uint8_t bytes[64];
    unsigned writes, injects, releases, finalizes, private_releases;
    unsigned fail_release;
    VcoError native_error;
    struct {
        uint8_t saved[4];
        uint32_t address;
        uint64_t token;
        bool active;
    } patches[8];
    bool private_live;
} device;
static bool live(uint64_t token)
{
    return vco_session_matches(&session, token, true) &&
           session.identity.instance == identity.instance;
}
static int alive(void *context, uint64_t token)
{
    assert(context == &device);
    return live(token);
}
static int write_bytes(void *context, uint64_t token, uint32_t address, const uint8_t *in,
                       size_t size)
{
    assert(context == &device && size <= 4 && address >= BASE && address + size <= BASE + 64);
    if (!live(token))
        return VCO_ACCESS_SESSION;
    memcpy(device.bytes + (address - BASE), in, size);
    ++device.writes;
    return 0;
}
static int inject(void *context, uint64_t token, uint32_t address, const uint8_t *in, size_t size,
                  int32_t *handle)
{
    assert(context == &device && device.injects < 8 && size == 4 && address >= BASE &&
           address + size <= BASE + 64);
    if (!live(token))
        return VCO_ACCESS_SESSION;
    unsigned index = device.injects++;
    device.patches[index].address = address;
    device.patches[index].token = token;
    device.patches[index].active = true;
    memcpy(device.patches[index].saved, device.bytes + (address - BASE), size);
    memcpy(device.bytes + (address - BASE), in, size);
    *handle = (int32_t)index;
    return 0;
}
static int release(void *context, uint64_t token, int32_t handle)
{
    assert(context == &device && handle >= 0 && (unsigned)handle < device.injects &&
           device.patches[handle].active && device.patches[handle].token == token);
    device.patches[handle].active = false; /* Native attempt consumes even on failure. */
    ++device.releases;
    if (device.fail_release == device.releases || !vco_session_matches(&session, token, false))
        return -1;
    memcpy(device.bytes + (device.patches[handle].address - BASE), device.patches[handle].saved, 4);
    return 0;
}
static VcoError finalize(void *context)
{
    assert(context == &device && engine.closing && !engine.close_complete && !engine.queued &&
           !engine.scheduled);
    for (unsigned i = 0; i < engine.count; ++i)
        assert(!engine.rows[i].desired && !engine.rows[i].pending);
    ++device.finalizes;
    if (device.private_live && !device.native_error) {
        device.private_live = false;
        ++device.private_releases;
    }
    return device.native_error;
}
static VcoError other_finalize(void *context)
{
    (void)context;
    assert(0); /* Changing providers cannot reuse the old confirmation. */
    return VCO_E_IO;
}
static VcoExecutionCommand command(unsigned entry, uint64_t request, bool enabled)
{
    return (VcoExecutionCommand){request, engine.token, engine.revision, entry, enabled};
}
static VcoEngineCloseKey key(VcoEngineCloseScope scope)
{
    return (VcoEngineCloseKey){.identity = engine.identity,
                               .session = engine.token,
                               .revision = engine.revision,
                               .transaction = engine.last_close_transaction + 1,
                               .scope = scope,
                               .process_object = 0x1234,
                               .module_object = scope == VCO_CLOSE_MODULE ? 0x5678 : 0,
                               .module_uid = scope == VCO_CLOSE_MODULE ? 17 : 0,
                               .module_generation = scope == VCO_CLOSE_MODULE ? 8 : 0};
}
static void bind_file(void)
{
    VcoCheatMemory memory = {.context = &device,
                             .alive = alive,
                             .write = write_bytes,
                             .inject = inject,
                             .release = release};
    assert(!vco_engine_bind(&engine, &session, engine.last_revision + 1, &file, memory, ops,
                            receipts, 8));
}
static void setup(void)
{
    memset(&device, 0, sizeof(device));
    memset(device.bytes, 0xaa, sizeof(device.bytes));
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    vco_engine_init(&engine);
    file = (VcoCheatFile){entries, 4, 0, codes, 8, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    const char *text = "_V1 Data\n$0200 81000000 11223344\n"
                       "_V0 Patch one\n$A200 81000010 E3A00001\n"
                       "_V0 Patch two\n$A200 81000014 E3A00002\n"
                       "_V0 Unsupported\n$FFFF 00000000 00000000\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    bind_file();
    assert(engine.rows[3].state == VCO_ENTRY_UNSUPPORTED);
}
static void activate(void)
{
    for (unsigned i = 0; i < 3; ++i)
        assert(!vco_engine_submit(&engine, command(i, 1, true)));
    vco_engine_step(&engine, 0);
    assert(device.writes == 1 && device.injects == 2 && !device.releases);
}
static void cycle(void)
{
    setup();
    activate();
    assert(!vco_engine_submit(&engine, command(0, 2, true)));
    VcoExecutionCommand old = command(0, 3, true);
    VcoEngineCloseKey closing = key(VCO_CLOSE_MODULE);
    VcoEngineCloseResult result = {VCO_E_IO, VCO_E_IO};
    assert(!vco_engine_close(&engine, &closing, finalize, &device, &result));
    assert(!result.entries && !result.native && engine.bound && engine.count == 4 &&
           engine.close_complete && device.releases == 2 && device.finalizes == 1);
    assert(device.bytes[0] == 0x44 && device.bytes[16] == 0xaa && device.bytes[20] == 0xaa);
    assert(engine.rows[0].acknowledged == 1 && !engine.rows[0].pending);
    for (unsigned i = 0; i < 3; ++i)
        assert(engine.rows[i].state == VCO_ENTRY_OFF && !engine.rows[i].desired);
    assert(engine.rows[3].state == VCO_ENTRY_UNSUPPORTED);
    assert(vco_engine_submit(&engine, old) == VCO_E_SESSION);
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    assert(device.writes == 1 && device.injects == 2 && device.releases == 2);
    assert(!vco_engine_close(&engine, &closing, finalize, &device, NULL));
    assert(device.finalizes == 1 && device.releases == 2);

    /* Snapshots remain available. A renewed revision rejects old UI requests. */
    VcoControlRequest query = {
        10, engine.token, engine.revision, VCO_CONTROL_ABI, sizeof(query), VCO_CONTROL_QUERY, 0, 0,
        4,  {0, 0}};
    VcoControlReply reply;
    assert(vco_control_dispatch(&engine, 88, 88, &query, sizeof(query), &reply) == VCO_CONTROL_OK);
    assert(reply.rows[0].state == VCO_ENTRY_OFF && !reply.rows[0].desired);
    assert(vco_engine_reopen(&engine, &closing, engine.revision) == VCO_E_ARGUMENT);
    assert(!vco_engine_reopen(&engine, &closing, engine.revision + 1));
    assert(vco_control_dispatch(&engine, 88, 88, &query, sizeof(query), &reply) ==
           VCO_CONTROL_STALE);
    assert(vco_engine_submit(&engine, old) == VCO_E_SESSION);
    vco_engine_step(&engine, 100000);
    assert(device.writes == 1 && device.injects == 2); /* _V1 stays OFF. */
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 100001);
    assert(device.writes == 2);
    VcoEngineCloseKey next = key(VCO_CLOSE_LIST);
    assert(!vco_engine_close(&engine, &next, finalize, &device, NULL));
    assert(!vco_engine_unbind(&engine) && device.finalizes == 2 && device.releases == 2);
}
static void identities(void)
{
    setup();
    activate();
    VcoEngineCloseKey closing = key(VCO_CLOSE_MODULE);
    for (unsigned variant = 0; variant < 12; ++variant) {
        VcoEngineCloseKey bad = closing;
        VcoEngineCloseResult result = {VCO_E_METADATA, VCO_E_BUDGET};
        if (variant == 0)
            ++bad.identity.pid;
        if (variant == 1)
            ++bad.identity.instance;
        if (variant == 2)
            bad.identity.title_id[3] = 'X';
        if (variant == 3)
            ++bad.session;
        if (variant == 4)
            ++bad.revision;
        if (variant == 5)
            bad.transaction = 0;
        if (variant == 6)
            bad.scope = (VcoEngineCloseScope)99;
        if (variant == 7)
            bad.process_object = 0;
        if (variant == 8)
            bad.module_object = 0;
        if (variant == 9)
            bad.module_uid = -1;
        if (variant == 10)
            bad.module_generation = 0;
        if (variant == 11)
            bad.scope = VCO_CLOSE_PROCESS; /* Noncanonical module tags. */
        assert(vco_engine_close(&engine, &bad, finalize, &device, &result));
        assert(result.entries == VCO_E_METADATA && result.native == VCO_E_BUDGET &&
               !engine.closing && !device.releases && !device.finalizes);
    }
    assert(vco_engine_close(&engine, &closing, NULL, &device, NULL) == VCO_E_ARGUMENT);
    assert(!vco_engine_close(&engine, &closing, finalize, &device, NULL));
    for (unsigned variant = 0; variant < 6; ++variant) {
        VcoEngineCloseKey bad = closing;
        if (variant == 0)
            ++bad.transaction;
        if (variant == 1)
            bad.scope = VCO_CLOSE_PROCESS, bad.module_object = 0, bad.module_uid = 0,
            bad.module_generation = 0;
        if (variant == 2)
            ++bad.process_object;
        if (variant == 3)
            ++bad.module_object;
        if (variant == 4)
            ++bad.module_uid;
        if (variant == 5)
            ++bad.module_generation;
        assert(vco_engine_close(&engine, &bad, finalize, &device, NULL) == VCO_E_SESSION);
        assert(vco_engine_reopen(&engine, &bad, engine.revision + 1) == VCO_E_SESSION);
    }
    assert(vco_engine_close(&engine, &closing, other_finalize, &device, NULL) == VCO_E_ARGUMENT);
    assert(vco_engine_close(&engine, &closing, finalize, &engine, NULL) == VCO_E_ARGUMENT);
    assert(vco_engine_close(&engine, &closing, NULL, NULL, NULL) == VCO_E_ARGUMENT);
    assert(device.finalizes == 1 && device.releases == 2);
    /* The original confirmation is readable after session loss, but cannot
     * reopen a new same-PID instance even if its title is unchanged. */
    VcoIdentity next = identity;
    ++next.instance;
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &next));
    assert(!vco_engine_close(&engine, &closing, finalize, &device, NULL));
    assert(vco_engine_reopen(&engine, &closing, engine.revision + 1) == VCO_E_SESSION);
    assert(!vco_engine_unbind(&engine));
    bind_file();
    assert(vco_engine_close(&engine, &closing, finalize, &device, NULL) == VCO_E_SESSION);
    assert(!vco_engine_unbind(&engine));

    setup();
    activate();
    closing = key(VCO_CLOSE_PROCESS);
    vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    memset(device.bytes, 0xcc, sizeof(device.bytes));
    assert(vco_engine_close(&engine, &closing, finalize, &device, NULL) == VCO_E_RESTORE);
    assert(device.releases == 2 && engine.bound && engine.identity.instance == identity.instance);
    for (unsigned i = 0; i < sizeof(device.bytes); ++i)
        assert(device.bytes[i] == 0xcc);
    assert(vco_engine_unbind(&engine) == VCO_E_RESTORE);
}
static void failures(void)
{
    for (unsigned variant = 0; variant < 3; ++variant) {
        setup();
        activate();
        device.private_live = true;
        if (variant == 0)
            device.fail_release = 1;
        if (variant == 1)
            device.native_error = VCO_E_IO;
        if (variant == 2) {
            device.fail_release = 1;
            assert(!vco_engine_submit(&engine, command(1, 2, false)));
            vco_engine_step(&engine, 1);
            assert(engine.blocked_restore);
        }
        VcoEngineCloseKey closing = key(VCO_CLOSE_PROCESS);
        VcoEngineCloseResult result;
        VcoError expected = variant == 1 ? VCO_E_IO : VCO_E_RESTORE;
        assert(vco_engine_close(&engine, &closing, finalize, &device, &result) == expected);
        assert(device.releases == 2 && device.finalizes == 1 && engine.blocked_restore);
        assert(variant == 1 ? result.entries == VCO_OK && result.native == VCO_E_IO
                            : result.entries == VCO_E_RESTORE && result.native == VCO_OK);
        assert(variant == 1 ? device.private_live : device.private_releases == 1);
        assert(vco_engine_reopen(&engine, &closing, engine.revision + 1) == expected);
        device.fail_release = 0;
        device.native_error = VCO_OK;
        assert(vco_engine_close(&engine, &closing, finalize, &device, NULL) == expected);
        vco_engine_step(&engine, 100000);
        assert(device.releases == 2 && device.finalizes == 1 && device.writes == 1);
        VcoCheatMemory memory = {.context = &device, .alive = alive, .write = write_bytes};
        assert(vco_engine_bind(&engine, &session, engine.last_revision + 1, &file, memory, ops,
                               receipts, 8) == VCO_E_RESTORE);
        assert(engine.bound && engine.close_complete &&
               engine.close_result.native == result.native);
        assert(vco_engine_unbind(&engine) == VCO_E_RESTORE && device.releases == 2);
    }
}
static void transactions(void)
{
    setup();
    VcoEngineCloseKey closing = key(VCO_CLOSE_LIST);
    assert(!vco_engine_close(&engine, &closing, NULL, NULL, NULL));
    assert(!vco_engine_reopen(&engine, &closing, engine.revision + 1));
    VcoEngineCloseKey replay = key(VCO_CLOSE_LIST);
    replay.transaction = closing.transaction;
    assert(vco_engine_close(&engine, &replay, NULL, NULL, NULL) == VCO_E_SESSION);
    assert(!engine.closing);
    VcoEngineCloseKey next = key(VCO_CLOSE_MODULE);
    assert(!vco_engine_close(&engine, &next, finalize, &device, NULL));
    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    assert(!vco_engine_reopen(&engine, &next, engine.revision + 1));
    assert(vco_engine_submit(&engine, command(0, 1, true)) == VCO_E_SESSION);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    assert(!vco_engine_submit(&engine, command(1, 1, true)));
    vco_engine_step(&engine, 0);
    assert(device.injects == 1);
    next = key(VCO_CLOSE_MODULE);
    ++next.module_generation; /* Same module UID/object, new protected generation. */
    assert(!vco_engine_close(&engine, &next, finalize, &device, NULL));
    uint64_t transaction = engine.last_close_transaction;
    assert(!vco_engine_unbind(&engine) && engine.last_close_transaction == transaction);
    bind_file();
    replay = key(VCO_CLOSE_LIST);
    replay.transaction = transaction;
    assert(vco_engine_close(&engine, &replay, NULL, NULL, NULL) == VCO_E_SESSION);
    assert(!vco_engine_submit(&engine, command(2, 1, true)));
    vco_engine_step(&engine, 1);
    engine.last_close_transaction = UINT64_MAX;
    next = key(VCO_CLOSE_LIST);
    assert(vco_engine_close(&engine, &next, NULL, NULL, NULL) == VCO_E_ARGUMENT);
    assert(!vco_engine_unbind(&engine) && device.releases == 2);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "cycle"))
        cycle();
    else if (!strcmp(argv[1], "identities"))
        identities();
    else if (!strcmp(argv[1], "failures"))
        failures();
    else if (!strcmp(argv[1], "transactions"))
        transactions();
    else
        assert(0);
    return 0;
}
