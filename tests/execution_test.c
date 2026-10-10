#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "execution/engine.h"
#include "execution/memory.h"
#define BASE 0x81000000u
static VcoSession session;
static VcoIdentity identity = {42, 99, "PCSB00031"};
static struct {
    uint8_t bytes[512];
    uint64_t instance;
    unsigned locks, unlocks, reads, writes, permissions;
    int fault;
    bool foreground;
    bool revoke_copy;
} device;
static int validate(void *ctx, const VcoIdentity *id, bool foreground)
{
    (void)ctx;
    return id->pid != identity.pid || id->instance != device.instance ||
                   (foreground && !device.foreground) || device.fault == 1
               ? -1
               : 0;
}
static int lock_range(void *ctx, const VcoIdentity *id, uint32_t a, size_t n)
{
    (void)ctx;
    (void)id;
    (void)a;
    (void)n;
    if (device.fault == 2)
        return -1;
    if (device.fault == 7)
        return VCO_ACCESS_SESSION;
    ++device.locks;
    if (device.fault == 6)
        device.instance++;
    return 0;
}
static int inspect(void *ctx, const VcoIdentity *id, uint32_t a, size_t n, VcoMemoryRegion *region)
{
    (void)ctx;
    (void)id;
    (void)a;
    (void)n;
    if (device.fault == 3)
        return -1;
    if (device.fault == 8)
        return VCO_ACCESS_SESSION;
    *region = (VcoMemoryRegion){BASE, sizeof(device.bytes), device.permissions, device.instance};
    return 0;
}
static int read_bytes(void *ctx, const VcoIdentity *id, uint32_t a, uint8_t *out, size_t n)
{
    (void)ctx;
    (void)id;
    ++device.reads;
    if (device.revoke_copy)
        device.foreground = false;
    if (device.fault == 4)
        return -1;
    assert(a >= BASE && (uint64_t)a + n <= (uint64_t)BASE + sizeof(device.bytes));
    memcpy(out, device.bytes + (a - BASE), n);
    return 0;
}
static int write_bytes(void *ctx, const VcoIdentity *id, uint32_t a, const uint8_t *in, size_t n)
{
    (void)ctx;
    (void)id;
    ++device.writes;
    if (device.revoke_copy)
        device.foreground = false;
    if (device.fault == 4)
        return -1;
    assert(a >= BASE && (uint64_t)a + n <= (uint64_t)BASE + sizeof(device.bytes));
    memcpy(device.bytes + (a - BASE), in, n);
    return 0;
}
static int unlock_range(void *ctx, const VcoIdentity *id, uint32_t a, size_t n)
{
    (void)ctx;
    (void)id;
    (void)a;
    (void)n;
    ++device.unlocks;
    return device.fault == 5 ? -1 : 0;
}
static VcoMemoryAdapter adapter;
static VcoCheatMemory memory;
static void setup(void)
{
    memset(&device, 0, sizeof(device));
    device.instance = identity.instance;
    device.foreground = true;
    device.permissions = VCO_MEM_READ | VCO_MEM_WRITE;
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    adapter.session = &session;
    adapter.transport = (VcoMemoryTransport){NULL,        lock_range,   inspect, read_bytes,
                                             write_bytes, unlock_range, validate};
    memory = vco_memory_callbacks(&adapter);
}
static void test_session(void)
{
    setup();
    uint64_t first = session.token;
    assert(vco_session_matches(&session, first, true));
    assert(!vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    assert(vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity));
    assert(!vco_session_matches(&session, first, true));
    assert(vco_session_matches(&session, first, false));
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    assert(vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL));
    assert(!session.runnable && session.token == first);
    VcoIdentity next = identity;
    ++next.instance;
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &next));
    assert(session.token != first && !vco_session_matches(&session, first, false));
    uint64_t second = session.token;
    assert(vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL));
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &next));
    assert(session.token != second);
    VcoIdentity bad = next;
    memcpy(bad.title_id, "NPXS10000", 10);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &bad);
    assert(!session.runnable);
    session.sequence = UINT64_MAX;
    assert(vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL));
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    assert(!session.present && !session.token);
}
static void test_memory(void)
{
    setup();
    uint8_t value[4] = {1, 2, 3, 4}, out[4] = {0};
    assert(!memory.write(memory.context, session.token, BASE, value, 4));
    assert(!memory.read(memory.context, session.token, BASE, out, 4));
    assert(!memcmp(value, out, 4));
    unsigned before = device.writes;
    device.permissions |= VCO_MEM_EXECUTE;
    assert(memory.write(memory.context, session.token, BASE, value, 4));
    device.permissions = VCO_MEM_READ;
    assert(memory.write(memory.context, session.token, BASE, value, 4));
    assert(device.writes == before);
    assert(memory.write(memory.context, session.token, 0x1000, value, 4));
    assert(memory.write(memory.context, session.token, UINT32_MAX, value, 4));
    assert(memory.write(memory.context, session.token, BASE + sizeof(device.bytes) - 1, value, 4));
    assert(memory.write(memory.context, session.token, BASE, value, 0));
    assert(memory.write(memory.context, session.token, BASE, value, 5));
    assert(memory.write(memory.context, session.token + 1, BASE, value, 4));
    for (int fault = 1; fault <= 6; ++fault) {
        setup();
        device.fault = fault;
        memset(out, 0xaa, sizeof(out));
        assert(memory.read(memory.context, session.token, BASE, out, 4));
        assert(out[0] == 0xaa && device.locks == device.unlocks);
    }
    setup();
    device.foreground = false;
    assert(memory.write(memory.context, session.token, BASE, value, 4));
    assert(!device.writes && !device.locks);
    for (int fault = 7; fault <= 8; ++fault) {
        setup();
        device.fault = fault;
        assert(memory.read(memory.context, session.token, BASE, out, 4) == VCO_MEMORY_AUTHORITY);
        assert(!device.reads && device.locks == device.unlocks);
    }
    setup();
    device.fault = 4;
    device.revoke_copy = true;
    assert(memory.read(memory.context, session.token, BASE, out, 4) == VCO_MEMORY_COPY);
    setup();
    device.fault = 5;
    device.revoke_copy = true;
    assert(memory.read(memory.context, session.token, BASE, out, 4) == VCO_MEMORY_UNLOCK);
}
static VcoEngine engine;
static VcoCheatEntry entries[32];
static VcoCode codes[256];
static VcoCheatOp ops[256];
static VcoPatch patches[256];
static char names[2048];
static VcoCheatFile file;
static void bind_file(const char *text)
{
    VcoDiagnostic d;
    file = (VcoCheatFile){entries, 32, 0, codes, 256, 0, names, sizeof(names), 0};
    assert(vco_cheat_parse(text, strlen(text), &file, &d) == VCO_OK);
    assert(vco_engine_bind(&engine, &session, engine.last_revision + 1, &file, memory, ops, patches,
                           256) == VCO_OK);
}
static VcoExecutionCommand command(unsigned entry, uint64_t request, bool enabled)
{
    return (VcoExecutionCommand){request, session.token, engine.revision, entry, enabled};
}
static void put_root(uint32_t address)
{
    for (unsigned i = 0; i < 4; ++i)
        device.bytes[i] = (uint8_t)(address >> (i * 8));
}
static void test_dynamic_claims(void)
{
    setup();
    vco_engine_init(&engine);
    bind_file("_V0 Pointer\n$3201 81000000 00000000\n$3300 00000000 00000008\n"
              "_V0 Old target\n$0200 81000020 00000009\n");
    put_root(BASE + 32);
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 0);
    assert(engine.rows[0].state == VCO_ENTRY_ON && device.bytes[32] == 8);
    put_root(BASE + 64);
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    assert(engine.rows[0].state == VCO_ENTRY_ON && device.bytes[64] == 8);
    assert(!vco_engine_submit(&engine, command(1, 1, true)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 1);
    assert(engine.rows[1].state == VCO_ENTRY_ON && device.bytes[32] == 9);
    /* A current target remains protected, including an idempotent ON command. */
    assert(!vco_engine_submit(&engine, command(1, 2, true)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 2);
    put_root(BASE + 32);
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 2);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR &&
           engine.rows[0].diagnostic.code == VCO_E_PATCH && device.bytes[32] == 9);
    assert(!vco_engine_unbind(&engine));
}
static uint32_t pressure_address;
static int pressure_write(void *ctx, uint64_t token, uint32_t address, const uint8_t *in, size_t n)
{
    (void)ctx;
    assert(token == session.token && address >= BASE + 0x10000 && address < BASE + 0x100000 &&
           n == 4 && in[0] == 8);
    ++device.writes;
    pressure_address = address;
    return 0;
}
static void test_claim_pressure(void)
{
    setup();
    memory.write = pressure_write;
    vco_engine_init(&engine);
    bind_file("_V0 Moving pointer\n$3201 81000000 00000000\n$3300 00000000 00000008\n");
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    for (unsigned tick = 0; tick < VCO_ENGINE_CLAIMS + 100; ++tick) {
        uint32_t target = BASE + 0x10000 + tick * 4;
        put_root(target);
        vco_engine_step(&engine, (uint64_t)tick * VCO_ENGINE_PERIOD_US);
        assert(engine.rows[0].state == VCO_ENTRY_ON && pressure_address == target &&
               device.writes == tick + 1);
    }
    assert(!vco_engine_unbind(&engine));
}
static void test_authority_pause(void)
{
    setup();
    vco_engine_init(&engine);
    bind_file("_V0 Two writes\n$0200 81000000 00000008\n$0200 81000004 00000009\n");
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    device.revoke_copy = true;
    vco_engine_step(&engine, 0);
    assert(engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[0].desired);
    assert(engine.rows[0].diagnostic.code == VCO_OK && engine.rows[0].acknowledged == 1);
    assert(device.writes == 1 && device.bytes[0] == 8 && !device.bytes[4]);
    assert(device.locks == device.unlocks);
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    assert(device.writes == 1 && engine.rows[0].state == VCO_ENTRY_PAUSED);
    device.revoke_copy = false;
    device.foreground = true;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 2);
    assert(engine.rows[0].state == VCO_ENTRY_ON && device.writes == 3 && device.bytes[4] == 9);
    device.revoke_copy = true;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 3);
    assert(engine.rows[0].state == VCO_ENTRY_PAUSED);
    assert(!vco_engine_submit(&engine, command(0, 2, false)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 4);
    assert(engine.rows[0].state == VCO_ENTRY_OFF && !engine.rows[0].desired);
    device.revoke_copy = false;
    device.foreground = true;
    unsigned before = device.writes;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 5);
    assert(device.writes == before);
    /* A real copy failure is terminal even if foreground is later valid. */
    device.fault = 4;
    assert(!vco_engine_submit(&engine, command(0, 3, true)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 6);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR && !engine.rows[0].desired &&
           engine.rows[0].diagnostic.code == VCO_E_IO);
    device.fault = 0;
    before = device.writes;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 7);
    assert(device.writes == before);
    assert(!vco_engine_submit(&engine, command(0, 4, true)));
    device.foreground = false;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 8);
    assert(engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[0].desired);
    VcoIdentity next = identity;
    ++next.instance;
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &next);
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 9);
    assert(!engine.bound && !engine.queued && device.writes == before);
}
static void test_engine(void)
{
    setup();
    vco_engine_init(&engine);
    bind_file("_V1 A\n$0200 81000000 11223344\n_V0 B\n$0100 81000000 00005566\n_V0 Patch\n$A200 "
              "81000010 E3A00001\n");
    assert(engine.rows[0].state == VCO_ENTRY_OFF && !device.writes);
    assert(engine.rows[2].state == VCO_ENTRY_UNSUPPORTED); /* adapter lacks injection */
    assert(vco_engine_submit(&engine, command(0, 1, true)) == VCO_OK);
    assert(engine.rows[0].state == VCO_ENTRY_ENABLING && !device.writes);
    assert(vco_engine_submit(&engine, command(0, 1, true)) == VCO_OK);
    assert(vco_engine_submit(&engine, command(0, 1, false)) == VCO_E_ARGUMENT);
    vco_engine_step(&engine, 100);
    assert(engine.rows[0].state == VCO_ENTRY_ON && engine.rows[0].acknowledged == 1);
    assert(device.writes == 1 && device.bytes[0] == 0x44 && device.bytes[3] == 0x11);
    vco_engine_step(&engine, 200);
    assert(device.writes == 1);
    vco_engine_step(&engine, 50100);
    assert(device.writes == 2);
    assert(vco_engine_submit(&engine, command(1, 1, true)) == VCO_OK);
    vco_engine_step(&engine, 50101);
    assert(engine.rows[1].state == VCO_ENTRY_ERROR && device.bytes[0] == 0x44);
    assert(engine.rows[1].diagnostic.code == VCO_E_PATCH);
    VcoExecutionCommand stale = command(0, 2, false);
    stale.revision++;
    assert(vco_engine_submit(&engine, stale) == VCO_E_SESSION);
    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    vco_engine_step(&engine, 50200);
    assert(engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[0].desired);
    unsigned writes = device.writes;
    vco_engine_step(&engine, 100000);
    assert(device.writes == writes);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    vco_engine_step(&engine, 100001);
    assert(engine.rows[0].state == VCO_ENTRY_ON && device.writes == writes + 1);
    assert(vco_engine_submit(&engine, command(0, 2, false)) == VCO_OK);
    vco_engine_step(&engine, 200000);
    assert(engine.rows[0].state == VCO_ENTRY_OFF && device.writes == writes + 1);
    assert(device.bytes[0] == 0x44); /* OFF does not restore ordinary data. */
    assert(vco_engine_bind(&engine, &session, engine.revision, &file, memory, ops, patches, 256) ==
           VCO_E_ARGUMENT);
    assert(engine.bound);
    assert(vco_engine_submit(&engine, command(0, 3, true)) == VCO_OK);
    VcoIdentity replacement = identity;
    ++replacement.instance;
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &replacement);
    vco_engine_step(&engine, 300000);
    assert(!engine.bound && !engine.queued && device.writes == writes + 1);
    setup();
    vco_engine_init(&engine);
    char list[2048];
    size_t n = 0;
    for (unsigned i = 0; i < 18; ++i)
        n += (size_t)snprintf(list + n, sizeof(list) - n, "_V1 E%u\n$0200 %08X 00000001\n", i,
                              BASE + i * 4);
    bind_file(list);
    for (unsigned i = 0; i < VCO_ENGINE_QUEUE; ++i)
        assert(vco_engine_submit(&engine, command(i, 1, true)) == VCO_OK);
    assert(vco_engine_submit(&engine, command(16, 1, true)) == VCO_E_LIMIT);
    vco_engine_step(&engine, 0);
    assert(device.writes == 16);
    assert(vco_engine_submit(&engine, command(16, 1, true)) == VCO_OK);
    vco_engine_step(&engine, 1);
    assert(engine.rows[16].state == VCO_ENTRY_ERROR &&
           engine.rows[16].diagnostic.code == VCO_E_LIMIT);
    assert(vco_engine_unbind(&engine) == VCO_OK);
    bind_file("_V0 Partial\n$0200 81000000 00000008\n$0200 81001000 00000009\n");
    assert(vco_engine_submit(&engine, command(0, 1, true)) == VCO_OK);
    vco_engine_step(&engine, 2);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR && device.bytes[0] == 8);
    writes = device.writes;
    vco_engine_step(&engine, 500002);
    assert(device.writes == writes);
    assert(vco_engine_unbind(&engine) == VCO_OK);
}
static unsigned injections, releases;
static bool patch_live, release_failure;
static uint8_t original_patch[4];
static uint64_t patch_session;
static int fake_inject(void *ctx, uint64_t token, uint32_t a, const uint8_t *in, size_t n,
                       int32_t *h)
{
    (void)ctx;
    if (a != BASE + 16 || n != 4 || patch_live || !vco_session_matches(&session, token, true))
        return -1;
    memcpy(original_patch, device.bytes + 16, 4);
    memcpy(device.bytes + 16, in, 4);
    ++injections;
    patch_live = true;
    patch_session = token;
    *h = 0;
    return 0;
}
static int fake_release(void *ctx, uint64_t token, int32_t h)
{
    (void)ctx;
    assert(h == 0 && patch_live && token == patch_session);
    patch_live = false;
    ++releases;
    if (release_failure)
        return -1;
    /* The native adapter must similarly distinguish a departed instance. */
    if (vco_session_matches(&session, token, false))
        memcpy(device.bytes + 16, original_patch, 4);
    return 0;
}
static int fake_pad(void *ctx, uint64_t token, uint32_t *controller, uint32_t *buttons)
{
    (void)ctx;
    assert(token == session.token);
    *controller = 1;
    *buttons = device.bytes[8];
    return 0;
}
static void test_guarded_claims(void)
{
    for (unsigned condition = 0; condition < 3; ++condition) {
        setup();
        injections = releases = 0;
        patch_live = release_failure = false;
        memory.inject = fake_inject;
        memory.release = fake_release;
        memory.pad = fake_pad;
        vco_engine_init(&engine);
        memset(device.bytes + 16, 0xaa, 4);
        bind_file(condition == 1 ? "_V0 Conditional patch\n$D201 81000000 00000001\n"
                                   "$A200 81000010 E3A00001\n_V0 Other\n$0200 81000010 00000009\n"
                                 : "_V0 Button patch\n$C201 00000000 00000001\n"
                                   "$A200 81000010 E3A00001\n_V0 Other\n$0200 81000010 00000009\n");
        put_root(1);
        device.bytes[8] = 1;
        assert(!vco_engine_submit(&engine, command(0, 1, true)));
        vco_engine_step(&engine, 0);
        assert(patch_live && injections == 1);
        put_root(0);
        device.bytes[8] = 0;
        release_failure = condition == 2;
        vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
        if (condition == 2) {
            assert(engine.blocked_restore && engine.rows[0].diagnostic.code == VCO_E_RESTORE);
            assert(vco_engine_submit(&engine, command(1, 1, true)) == VCO_E_RESTORE);
            assert(vco_engine_unbind(&engine) == VCO_E_RESTORE && releases == 1);
            continue;
        }
        assert(condition == 1 ? patch_live && !releases : !patch_live && releases == 1);
        assert(!vco_engine_submit(&engine, command(1, 1, true)));
        vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 1);
        if (condition == 1)
            assert(engine.rows[1].state == VCO_ENTRY_ERROR &&
                   engine.rows[1].diagnostic.code == VCO_E_PATCH && device.bytes[16] == 1);
        else
            assert(engine.rows[1].state == VCO_ENTRY_ON && device.bytes[16] == 9);
        assert(!vco_engine_unbind(&engine));
    }
}
static void test_guarded_data_claims(void)
{
    setup();
    memory.pad = fake_pad;
    vco_engine_init(&engine);
    bind_file("_V0 Button write\n$C201 00000000 00000001\n$0200 81000010 00000008\n"
              "_V0 Other\n$0200 81000010 00000009\n");
    device.bytes[8] = 1;
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 0);
    assert(device.bytes[16] == 8 && engine.rows[0].state == VCO_ENTRY_ON);
    device.bytes[8] = 0;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    assert(device.bytes[16] == 8 && engine.rows[0].state == VCO_ENTRY_ON);
    assert(!vco_engine_submit(&engine, command(1, 1, true)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 1);
    assert(device.bytes[16] == 9 && engine.rows[1].state == VCO_ENTRY_ON);
    device.bytes[8] = 1;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 2);
    assert(device.bytes[16] == 9 && engine.rows[0].state == VCO_ENTRY_ERROR &&
           engine.rows[0].diagnostic.code == VCO_E_PATCH);
    assert(!vco_engine_unbind(&engine));
}
static void test_patch_authority_pause(void)
{
    for (unsigned restore_error = 0; restore_error < 2; ++restore_error) {
        setup();
        injections = releases = 0;
        patch_live = false;
        release_failure = restore_error != 0;
        memory.inject = fake_inject;
        memory.release = fake_release;
        vco_engine_init(&engine);
        memset(device.bytes + 16, 0xaa, 4);
        bind_file("_V0 Patch and data\n$A200 81000010 E3A00001\n$0200 81000000 00000008\n");
        device.revoke_copy = true;
        assert(!vco_engine_submit(&engine, command(0, 1, true)));
        vco_engine_step(&engine, 0);
        assert(injections == 1 && releases == 1 && !patch_live);
        if (restore_error) {
            assert(engine.rows[0].state == VCO_ENTRY_ERROR && !engine.rows[0].desired &&
                   engine.rows[0].diagnostic.code == VCO_E_RESTORE && engine.blocked_restore);
            device.foreground = true;
            device.revoke_copy = false;
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
            assert(injections == 1 && releases == 1);
            assert(vco_engine_unbind(&engine) == VCO_E_RESTORE);
        } else {
            assert(engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[0].desired &&
                   device.bytes[16] == 0xaa);
            device.foreground = true;
            device.revoke_copy = false;
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
            assert(injections == 2 && releases == 1 && patch_live &&
                   engine.rows[0].state == VCO_ENTRY_ON);
            assert(!vco_engine_unbind(&engine));
            assert(releases == 2 && !patch_live);
        }
    }
}
static void test_paused_capacity(void)
{
    setup();
    vco_engine_init(&engine);
    char text[2048];
    size_t used = 0;
    for (unsigned i = 0; i <= VCO_ENGINE_ACTIVE; ++i)
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "_V0 Item %u\n$0200 %08X 00000001\n", i, BASE + i * 4);
    bind_file(text);
    for (unsigned i = 0; i < VCO_ENGINE_ACTIVE; ++i)
        assert(!vco_engine_submit(&engine, command(i, 1, true)));
    vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL);
    vco_engine_step(&engine, 0);
    for (unsigned i = 0; i < VCO_ENGINE_ACTIVE; ++i)
        assert(engine.rows[i].state == VCO_ENTRY_PAUSED && engine.rows[i].desired);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    assert(!vco_engine_submit(&engine, command(VCO_ENGINE_ACTIVE, 1, true)));
    vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL);
    vco_engine_step(&engine, 1);
    assert(engine.rows[VCO_ENGINE_ACTIVE].state == VCO_ENTRY_ERROR &&
           engine.rows[VCO_ENGINE_ACTIVE].diagnostic.code == VCO_E_LIMIT &&
           !engine.rows[VCO_ENGINE_ACTIVE].desired && !device.writes);
    assert(!vco_engine_unbind(&engine));
}
static void test_patches(void)
{
    setup();
    vco_engine_init(&engine);
    memory.inject = fake_inject;
    memory.release = fake_release;
    memset(device.bytes + 16, 0xaa, 4);
    bind_file("_V0 Patch\n$A200 81000010 E3A00001\n_V0 Collision\n$0200 81000010 00000000\n");
    assert(vco_engine_submit(&engine, command(0, 1, true)) == VCO_OK);
    vco_engine_step(&engine, 0);
    assert(injections == 1 && patch_live && device.bytes[16] == 1);
    vco_engine_step(&engine, 50000);
    assert(injections == 1);
    assert(vco_engine_submit(&engine, command(1, 1, true)) == VCO_OK);
    vco_engine_step(&engine, 50001);
    assert(engine.rows[1].state == VCO_ENTRY_ERROR && device.bytes[16] == 1);
    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    vco_engine_step(&engine, 50002);
    assert(releases == 1 && device.bytes[16] == 0xaa && engine.rows[0].state == VCO_ENTRY_PAUSED);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    vco_engine_step(&engine, 50003);
    assert(injections == 2);
    assert(vco_engine_submit(&engine, command(0, 2, false)) == VCO_OK);
    release_failure = true;
    vco_engine_step(&engine, 50004);
    assert(releases == 2 && engine.blocked_restore && engine.rows[0].state == VCO_ENTRY_ERROR);
    assert(engine.rows[0].diagnostic.code == VCO_E_RESTORE);
    assert(vco_engine_submit(&engine, command(1, 2, true)) == VCO_E_RESTORE);
    assert(vco_engine_unbind(&engine) == VCO_E_RESTORE && releases == 2);
    assert(vco_engine_bind(&engine, &session, 8, &file, memory, ops, patches, 256) ==
           VCO_E_RESTORE);
}
static unsigned preparations;
static void prepare_metadata(void *context, uint64_t token)
{
    assert(context == &preparations && token == session.token);
    ++preparations;
}
static void test_idle_prepare(void)
{
    setup();
    vco_engine_init(&engine);
    assert(vco_engine_set_idle_prepare(&engine, prepare_metadata, &preparations) == VCO_E_ARGUMENT);
    bind_file("_V1 First\n$0200 81000000 00000001\n"
              "_V0 Second\n$0200 81000004 00000002\n");
    preparations = 0;
    assert(!vco_engine_set_idle_prepare(&engine, prepare_metadata, &preparations));
    vco_engine_step(&engine, 0);
    assert(!preparations && !device.writes); /* File flags never prepare or enable. */
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    assert(vco_engine_set_idle_prepare(&engine, NULL, NULL) == VCO_E_ARGUMENT);
    assert(!vco_engine_submit(&engine, command(1, 1, true)));
    vco_engine_step(&engine, 1);
    assert(preparations == 1 && device.writes == 2 && engine.rows[0].desired &&
           engine.rows[1].desired); /* One idle boundary for two queued ONs. */
    assert(vco_engine_set_idle_prepare(&engine, NULL, NULL) == VCO_E_ARGUMENT);
    assert(engine.idle_prepare == prepare_metadata);
    assert(!vco_engine_submit(&engine, command(0, 1, true))); /* Duplicate receipt. */
    assert(!vco_engine_submit(&engine, command(0, 2, true))); /* Reaffirm live ON. */
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    assert(preparations == 1);

    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 1);
    assert(engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[1].state == VCO_ENTRY_PAUSED);
    unsigned writes = device.writes;
    assert(vco_engine_submit(&engine, command(0, 3, true)) == VCO_E_SESSION);
    assert(vco_engine_set_idle_prepare(&engine, NULL, NULL) == VCO_E_ARGUMENT);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    assert(!vco_engine_submit(&engine, command(0, 3, true)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 2);
    assert(preparations == 1 && device.writes == writes + 2); /* Intent survives pause. */

    assert(!vco_engine_submit(&engine, command(0, 4, false)));
    assert(!vco_engine_submit(&engine, command(1, 2, false)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 3);
    assert(preparations == 1 && !engine.rows[0].desired && !engine.rows[1].desired);
    assert(!vco_engine_submit(&engine, command(0, 5, true)));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 4);
    assert(preparations == 2 && engine.rows[0].desired && !engine.rows[1].desired);
    assert(!vco_engine_unbind(&engine));
    assert(!engine.idle_prepare && !engine.idle_prepare_context);
    bind_file("_V0 Replacement\n$0200 81000000 00000003\n");
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 0);
    assert(preparations == 2); /* Rebind cannot retain a borrowed provider context. */
    assert(!vco_engine_unbind(&engine));

    setup();
    vco_engine_init(&engine);
    memory.inject = fake_inject;
    memory.release = fake_release;
    bind_file("_V0 Patch\n$A200 81000010 E3A00001\n");
    preparations = 0;
    injections = releases = 0;
    patch_live = release_failure = false;
    assert(!vco_engine_set_idle_prepare(&engine, prepare_metadata, &preparations));
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 0);
    assert(preparations == 1 && patch_live);
    assert(!vco_engine_submit(&engine, command(0, 2, false)));
    release_failure = true;
    vco_engine_step(&engine, 1);
    assert(engine.blocked_restore && preparations == 1);
    assert(vco_engine_set_idle_prepare(&engine, NULL, NULL) == VCO_E_ARGUMENT);
    assert(vco_engine_submit(&engine, command(0, 3, true)) == VCO_E_RESTORE);
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    assert(preparations == 1); /* Failed restoration cannot trigger a remap. */
}
static void test_budget(void)
{
    setup();
    vco_engine_init(&engine);
    bind_file("_V0 Short repeat\n$4201 81000000 00000001\n$FFFF 00000000 00000000\n");
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 0);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR &&
           engine.rows[0].diagnostic.code == VCO_E_BUDGET && device.writes == 255 &&
           device.locks == device.unlocks);
    assert(!vco_engine_unbind(&engine));

    /* A long list may grant more work; a compressed count must never grant an
     * unbounded number of writes. Use the same target to avoid claim pressure. */
    setup();
    vco_engine_init(&engine);
    char text[8192];
    size_t used = (size_t)snprintf(text, sizeof(text), "_V0 Long repeat\n");
    for (unsigned i = 0; i < 198; ++i)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "$0200 81000000 00000001\n");
    assert(used < sizeof(text));
    snprintf(text + used, sizeof(text) - used,
             "$4201 81000000 00000002\n$FFFF 00000000 00000000\n");
    bind_file(text);
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 0);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR &&
           engine.rows[0].diagnostic.code == VCO_E_BUDGET && device.writes == 401 &&
           device.locks == device.unlocks);
    unsigned writes = device.writes;
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    assert(device.writes == writes); /* Exhaustion stops subsequent ticks. */
    assert(!vco_engine_unbind(&engine));

    /* The portable engine may receive a larger arena than the production
     * upload permits. Scalar copies must still stop at the absolute work cap. */
    setup();
    vco_engine_init(&engine);
    static VcoCode large_codes[9000];
    static VcoCheatOp large_ops[9000];
    static VcoPatch large_patches[9000];
    VcoCheatEntry large_entry = {.count = 9000};
    for (unsigned i = 0; i < 9000; ++i)
        large_codes[i] = (VcoCode){0x5200, BASE, BASE, i + 2};
    VcoCheatFile large = {&large_entry, 1, 1, large_codes, 9000, 9000, names, sizeof(names), 0};
    assert(!vco_engine_bind(&engine, &session, 1, &large, memory, large_ops, large_patches, 9000));
    assert(!vco_engine_submit(&engine, command(0, 1, true)));
    vco_engine_step(&engine, 0);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR &&
           engine.rows[0].diagnostic.code == VCO_E_BUDGET &&
           engine.rows[0].diagnostic.line == 8194 && device.reads == 8192 &&
           device.writes == 8192 && device.locks == device.unlocks);
    assert(!vco_engine_unbind(&engine));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "session"))
        test_session();
    else if (!strcmp(argv[1], "memory"))
        test_memory();
    else if (!strcmp(argv[1], "engine"))
        test_engine();
    else if (!strcmp(argv[1], "dynamic_claims"))
        test_dynamic_claims();
    else if (!strcmp(argv[1], "claim_pressure"))
        test_claim_pressure();
    else if (!strcmp(argv[1], "guarded_claims"))
        test_guarded_claims();
    else if (!strcmp(argv[1], "guarded_data_claims"))
        test_guarded_data_claims();
    else if (!strcmp(argv[1], "authority_pause"))
        test_authority_pause();
    else if (!strcmp(argv[1], "patch_authority_pause"))
        test_patch_authority_pause();
    else if (!strcmp(argv[1], "paused_capacity"))
        test_paused_capacity();
    else if (!strcmp(argv[1], "patches"))
        test_patches();
    else if (!strcmp(argv[1], "idle_prepare"))
        test_idle_prepare();
    else if (!strcmp(argv[1], "budget"))
        test_budget();
    else
        assert(0);
    return 0;
}
