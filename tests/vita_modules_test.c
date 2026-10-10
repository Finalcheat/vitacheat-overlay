#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psp2kern/kernel/modulemgr.h>
#include <taihen.h>
#include "platform/vita/modules.h"
#include "execution/engine.h"

static VcoSession session;
static VcoMemoryAdapter data;
static VcoVitaModules provider;
static VcoVitaModuleRecord arena[VCO_VITA_MODULE_LIMIT];
static SceKernelModuleInfo native[VCO_VITA_MODULE_LIMIT + 1];
static SceUID kernel_uids[VCO_VITA_MODULE_LIMIT + 1];
static SceUID raw_uids[VCO_VITA_MODULE_LIMIT + 1];
static unsigned native_count, lists, infos, mains, validations, operations, fault_at;
static unsigned lookups[3], change_info_at, replace_list_at, revoke_validate_at;
static int firmware[3], change_kind;
static int32_t main_module;
static int32_t info_error;
static bool reordered, authorized, zero_export, bad_info_uid;
static bool invalid_reported_count;

static int failure(void)
{
    return ++operations == fault_at;
}

static int validate(void *context, const VcoIdentity *identity, bool foreground)
{
    assert(context == &session && identity->pid == 42 && identity->instance == 5);
    ++validations;
    if (failure())
        return -1;
    if (!authorized || (foreground && !session.runnable))
        return VCO_ACCESS_SESSION;
    if (validations == revoke_validate_at)
        session.runnable = false;
    return 0;
}

static int module_list(SceUID pid, int flags1, int flags2, SceUID *uids, SceSize *count)
{
    assert(pid == 42 && flags1 == 0xff && flags2 == 1);
    assert(*count == provider.capacity + 1);
    ++lists;
    if (failure())
        return -1;
    if (lists == replace_list_at)
        raw_uids[0] = 999;
    unsigned n = native_count < *count ? native_count : *count;
    for (unsigned i = 0; i < n; ++i)
        uids[i] = raw_uids[reordered && (lists & 1) ? native_count - 1 - i : i];
    *count = invalid_reported_count ? UINT32_MAX : n;
    return 0;
}

static int module_info(SceUID pid, SceUID uid, SceKernelModuleInfo *out)
{
    assert(pid == 42 && out->size == sizeof(*out));
    ++infos;
    if (failure())
        return -1;
    if (info_error)
        return info_error;
    for (unsigned i = 0; i < native_count; ++i) {
        if (kernel_uids[i] != uid)
            continue;
        *out = native[i];
        if (bad_info_uid)
            out->modid = 0;
        if (infos == change_info_at) {
            switch (change_kind) {
            case 0:
                native[i].module_name[0] ^= 1;
                break;
            case 1:
                native[i].path[0] ^= 1;
                break;
            case 2:
                native[i].segments[0].vaddr = (void *)(uintptr_t)0x83000000u;
                break;
            case 3:
                ++native[i].segments[0].memsz;
                break;
            case 4:
                native[i].segments[0].perms ^= 1;
                break;
            case 5:
                native[i].state = SCE_KERNEL_MODULE_STATE_ENDED;
                break;
            case 6:
                ++native[i].modver[0];
                break;
            case 7:
                ++native[i].modattr;
                break;
            case 8:
                ++native[i].modid;
                break;
            }
        }
        return 0;
    }
    return -1;
}

static SceUID main_uid(SceUID pid)
{
    assert(pid == 42);
    ++mains;
    return failure() ? -1 : main_module;
}

int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address)
{
    const uint32_t libraries[] = {0xC445FA63, 0x92C9FFC2};
    const uint32_t functions[3][2] = {
        {0x97CF7B4E, 0xB72C75A4}, {0xD269F915, 0xDAA90093}, {0x20A27FA9, 0x679F5144}};
    assert(pid == KERNEL_PID && !strcmp(module, "SceKernelModulemgr"));
    for (unsigned kind = 0; kind < 3; ++kind) {
        if (function != functions[kind][0] && function != functions[kind][1])
            continue;
        unsigned index = lookups[kind]++;
        assert(index < 2 && library == libraries[index] && function == functions[kind][index]);
        if (firmware[kind] != (int)index)
            return -1;
        *address = zero_export ? 0
                   : kind == 0 ? (uintptr_t)module_list
                   : kind == 1 ? (uintptr_t)module_info
                               : (uintptr_t)main_uid;
        return 0;
    }
    assert(0);
    return -1;
}

static void setup(void)
{
    memset(&provider, 0, sizeof(provider));
    memset(arena, 0, sizeof(arena));
    memset(native, 0, sizeof(native));
    memset(lookups, 0, sizeof(lookups));
    memset(firmware, 0, sizeof(firmware));
    lists = infos = mains = validations = operations = fault_at = 0;
    change_info_at = replace_list_at = revoke_validate_at = 0;
    change_kind = 0;
    reordered = zero_export = bad_info_uid = invalid_reported_count = false;
    authorized = true;
    info_error = 0;
    native_count = 2;
    main_module = 200; /* Main module deliberately is NOT the first SDK result. */
    for (unsigned i = 0; i < VCO_VITA_MODULE_LIMIT + 1; ++i) {
        kernel_uids[i] = raw_uids[i] = (SceUID)(100 + i * 100);
        native[i].size = sizeof(native[i]);
        /* flags2=1 enumerates kernel GUIDs, but GetModuleInfo for a user
         * process returns its process-local UID in modid. These are different
         * namespaces in the captured firmware, not interchangeable IDs. */
        native[i].modid = (SceUID)(20000 + i * 100);
        snprintf(native[i].module_name, sizeof(native[i].module_name), "module%u", i);
        snprintf(native[i].path, sizeof(native[i].path), "app0:/module%u.suprx", i);
        native[i].segments[0] =
            (SceKernelSegmentInfo){sizeof(SceKernelSegmentInfo),
                                   5,
                                   (void *)(uintptr_t)(0x81000000u + i * 0x20000u),
                                   0x10000,
                                   0x10000,
                                   0};
        native[i].segments[1] =
            (SceKernelSegmentInfo){sizeof(SceKernelSegmentInfo),
                                   6,
                                   (void *)(uintptr_t)(0x81010000u + i * 0x20000u),
                                   0x10000,
                                   0x10000,
                                   0};
        native[i].state = SCE_KERNEL_MODULE_STATE_STARTED;
    }
    vco_session_init(&session);
    VcoIdentity identity = {42, 5, "PCSB00031"};
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    data = (VcoMemoryAdapter){.session = &session,
                              .transport = {.context = &session, .validate = validate}};
}

static void initialize(unsigned capacity)
{
    assert(!vco_vita_modules_init(&provider, &data, arena, capacity));
}

static uint64_t snapshot(void)
{
    uint64_t generation = 999;
    unsigned count = 999;
    assert(!vco_vita_modules_snapshot(&provider, session.token, &generation, &count));
    assert(generation == provider.generation && count == native_count && generation);
    return generation;
}

static void prepare(void)
{
    initialize(4);
    uint64_t generation = snapshot();
    const int32_t mapping[] = {200, 100};
    assert(!vco_vita_modules_confirm_mapping(&provider, session.token, generation, mapping, 2));
}

static void resolve_failed(uint64_t token, unsigned index, unsigned segment)
{
    uint32_t base = 0xaabbccddu, size = 0x11223344u;
    assert(vco_vita_modules_resolve(&provider, token, index, segment, &base, &size) < 0);
    assert(base == 0xaabbccddu && size == 0x11223344u);
}

static void snapshot_failed(void)
{
    uint64_t generation = 999;
    unsigned count = 999;
    assert(vco_vita_modules_snapshot(&provider, session.token, &generation, &count) < 0);
    assert(generation == 999 && count == 999 && !provider.generation && !provider.count &&
           !provider.mapped);
}

static void firmware_case(void)
{
    for (int fw = 0; fw < 2; ++fw) {
        setup();
        for (unsigned i = 0; i < 3; ++i)
            firmware[i] = fw;
        prepare();
        for (unsigned i = 0; i < 3; ++i)
            assert(lookups[i] == (unsigned)fw + 1);
        uint32_t base, size;
        assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
        assert(base == 0x81020000u && size == 0x10000u && provider.main_uid == 200);
    }
    for (unsigned missing = 0; missing < 3; ++missing) {
        setup();
        firmware[missing] = -1;
        int result = vco_vita_modules_init(&provider, &data, arena, 4);
        if (missing < 2) {
            assert(result < 0);
            snapshot_failed();
        } else {
            assert(!result);
            snapshot();
            assert(provider.main_uid == -1);
        }
    }
    setup();
    zero_export = true;
    assert(vco_vita_modules_init(&provider, &data, arena, 4) < 0);
    snapshot_failed();
}

static void snapshots(void)
{
    initialize(2); /* Exact capacity is valid because native reads get a spare slot. */
    raw_uids[0] = 200;
    raw_uids[1] = 100;
    uint64_t generation = snapshot();
    assert(arena[0].uid == 100 && arena[1].uid == 200 && provider.main_uid == 200);
    assert(arena[0].process_uid == 20000 && arena[1].process_uid == 20100);
    assert(provider.enumeration[0] == 200 && provider.enumeration[1] == 100);
    assert(!strcmp(arena[0].name, "module0") && !strcmp(arena[1].path, "app0:/module1.suprx"));
    assert(!provider.mapped);
    resolve_failed(session.token, 0, 0);
    assert(snapshot() > generation);
    reordered = true; /* Changing order within a snapshot is now invalid. */
    snapshot_failed();
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        setup();
        initialize(2);
        if (invalid == 0)
            native_count = 3;
        else if (invalid == 1)
            native_count = 0;
        else if (invalid == 2)
            raw_uids[1] = raw_uids[0];
        else if (invalid == 3)
            raw_uids[0] = 0;
        else if (invalid == 4)
            raw_uids[0] = -1;
        else if (invalid == 5)
            invalid_reported_count = true;
        else if (invalid == 6)
            main_module = 999;
        else
            main_module = -1;
        snapshot_failed();
    }
    setup();
    initialize(VCO_VITA_MODULE_LIMIT);
    native_count = VCO_VITA_MODULE_LIMIT;
    assert(snapshot());
    native_count++;
    snapshot_failed();
}

static void mapping(void)
{
    initialize(4);
    uint64_t generation = snapshot();
    int32_t map[VCO_VITA_MODULE_LIMIT] = {0};
    map[255] = 200;
    assert(!vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 256));
    uint32_t base, size;
    assert(!vco_vita_modules_resolve(&provider, session.token, 255, 1, &base, &size));
    assert(base == 0x81030000u && size == 0x10000u);
    resolve_failed(session.token, 0, 0);
    resolve_failed(session.token, 256, 0);
    resolve_failed(session.token, 255, 2);
    assert(provider.mapped);
    assert(vco_vita_modules_resolve(&provider, session.token, 255, 0, &base, &base) < 0);
    assert(vco_vita_modules_resolve(&provider, session.token, 255, 0, NULL, &size) < 0);
    assert(vco_vita_modules_confirm_mapping(&provider, session.token, generation + 1, map, 256) <
           0);
    assert(!provider.mapped); /* Failed replacement never keeps an older mapping active. */
    map[0] = 200;
    assert(vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 256) < 0);
    map[0] = 999;
    assert(vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 256) < 0);
    memset(map, 0, sizeof(map));
    assert(vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 256) < 0);
    map[0] = 100;
    assert(!vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 1));
    assert(snapshot() != generation && !provider.mapped);
    assert(vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 1) < 0);
    provider.sequence = UINT64_MAX;
    snapshot_failed();
}

static void auto_mapping(void)
{
    initialize(4);
    native_count = 3;
    raw_uids[0] = 200;
    raw_uids[1] = 100;
    raw_uids[2] = 300;
    uint64_t generation = snapshot();
    assert(arena[0].uid == 100 && arena[1].uid == 200 && arena[2].uid == 300);
    assert(!vco_vita_modules_map_z06(&provider, session.token, generation));
    assert(provider.mapping_count == 3);
    const uint32_t expected[] = {0x81040000u, 0x81000000u, 0x81020000u};
    uint32_t base, size;
    for (unsigned i = 0; i < 3; ++i) {
        assert(!vco_vita_modules_resolve(&provider, session.token, i, 0, &base, &size));
        assert(base == expected[i] && size == 0x10000u);
    }
    resolve_failed(session.token, 3, 0);
    assert(provider.main_uid == 200 && provider.mapping[0] == 2); /* Main is not forced to B0. */
    assert(vco_vita_modules_map_z06(&provider, session.token, generation + 1) < 0);
    assert(!provider.mapped);
    assert(!vco_vita_modules_map_z06(&provider, session.token, generation));

    /* Same UID set and unchanged records, but reversed order means different
     * B targets. Stop instead of retargeting a running cheat silently. */
    raw_uids[0] = 300;
    raw_uids[2] = 200;
    resolve_failed(session.token, 0, 0);
    assert(!provider.mapped && !provider.generation);
    uint64_t newer = snapshot();
    assert(newer > generation);
    assert(vco_vita_modules_map_z06(&provider, session.token, generation) < 0);
    assert(!vco_vita_modules_map_z06(&provider, session.token, newer));
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
    assert(base == 0x81020000u);
    ++native_count; /* Dynamic load invalidates count/order before any access. */
    resolve_failed(session.token, 0, 0);
    newer = snapshot();
    assert(!vco_vita_modules_map_z06(&provider, session.token, newer));
    --native_count; /* Dynamic unload also invalidates even an unaffected UID. */
    resolve_failed(session.token, 0, 0);

    setup();
    initialize(VCO_VITA_MODULE_LIMIT);
    native_count = VCO_VITA_MODULE_LIMIT;
    generation = snapshot();
    assert(!vco_vita_modules_map_z06(&provider, session.token, generation));
    assert(!vco_vita_modules_resolve(&provider, session.token, 255, 0, &base, &size));
    assert(base == 0x81000000u);
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
    assert(base == 0x81000000u + 255u * 0x20000u);
}

static void boundaries(void)
{
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        setup();
        initialize(4);
        if (invalid == 0)
            native[0].segments[0].vaddr = (void *)(uintptr_t)0x1000;
        else if (invalid == 1)
            native[0].segments[0].vaddr = (void *)(uintptr_t)0xfffffff0u;
        else if (invalid == 2)
            native[0].segments[0].memsz = UINT32_MAX; /* Nonempty overflow stays invalid. */
        else if (invalid == 3)
            native[0].segments[0].vaddr = NULL;
        else if (invalid == 4)
            memset(native[0].module_name, 'x', sizeof(native[0].module_name));
        else if (invalid == 5)
            memset(native[0].path, 'x', sizeof(native[0].path));
        else if (invalid == 6)
            native[0].module_name[0] = 0;
        else
            bad_info_uid = true;
        snapshot_failed();
    }
    setup();
    native[1].segments[0].vaddr = (void *)(uintptr_t)0xfffffffcu;
    native[1].segments[0].memsz = 4;
    native[1].segments[1] = (SceKernelSegmentInfo){0};
    prepare();
    uint32_t base, size;
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
    assert(base == 0xfffffffcu && size == 4);
    resolve_failed(session.token, 0, 1);
    setup();
    native[1].state = SCE_KERNEL_MODULE_STATE_READY;
    prepare();
    resolve_failed(session.token, 0, 0);
    if (UINTPTR_MAX > UINT32_MAX) {
        setup();
        initialize(4);
        native[0].segments[0].vaddr = (void *)(uintptr_t)UINT64_C(0x100000000);
        snapshot_failed();
    }
}

static void lifecycle(void)
{
    for (int change = 0; change < 9; ++change) {
        setup();
        prepare();
        change_kind = change;
        change_info_at = infos + 1;
        resolve_failed(session.token, 0, 0);
        assert(!provider.mapped && !provider.generation);
        resolve_failed(session.token, 0, 0);
    }
    setup();
    prepare();
    reordered = true;
    uint32_t base, size;
    resolve_failed(session.token, 0, 0);
    assert(!provider.mapped && !provider.generation);
    setup();
    prepare();
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
    replace_list_at = lists + 2; /* Membership changes after selected info was sampled. */
    resolve_failed(session.token, 0, 0);
    assert(!provider.mapped);
    for (unsigned change = 0; change < 5; ++change) {
        setup();
        prepare();
        if (change == 0)
            authorized = false; /* Same PID, revoked retained process instance. */
        else if (change == 1)
            session.runnable = false;
        else if (change == 2)
            ++session.identity.instance;
        else if (change == 3)
            revoke_validate_at = validations + 1;
        else
            main_module = 100;
        resolve_failed(session.token, 0, 0);
        if (change == 1 || change == 3) {
            assert(provider.mapped);
            session.runnable = true;
            uint32_t base, size;
            assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
        } else
            assert(!provider.mapped);
    }
    setup();
    prepare();
    resolve_failed(session.token + 1, 0, 0);
    assert(!provider.mapped);
    setup();
    initialize(4);
    change_info_at = 1;
    snapshot_failed();
    setup();
    initialize(4);
    uint64_t generation = snapshot();
    change_info_at = infos + 1;
    const int32_t map[] = {100};
    assert(vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 1) < 0);
    assert(!provider.generation);
}

static void faults(void)
{
    initialize(4);
    operations = 0;
    snapshot();
    unsigned total = operations;
    for (unsigned fail = 1; fail <= total; ++fail) {
        setup();
        initialize(4);
        fault_at = fail;
        snapshot_failed();
    }
    setup();
    prepare();
    operations = 0;
    uint32_t base, size;
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
    total = operations;
    for (unsigned fail = 1; fail <= total; ++fail) {
        setup();
        prepare();
        operations = 0;
        fault_at = fail;
        resolve_failed(session.token, 0, 0);
        assert(!provider.mapped && !provider.generation);
    }
    setup();
    assert(vco_vita_modules_init(NULL, &data, arena, 4) < 0);
    assert(vco_vita_modules_init(&provider, NULL, arena, 4) < 0);
    assert(vco_vita_modules_init(&provider, &data, arena, 0) < 0);
    assert(vco_vita_modules_init(&provider, &data, arena, 257) < 0);
    assert(vco_vita_modules_init(&provider, &data, NULL, 4) < 0);
    data.transport.validate = NULL;
    assert(vco_vita_modules_init(&provider, &data, arena, 4) < 0);
}

static unsigned pipeline_writes;
static int pipeline_alive(void *context, uint64_t token)
{
    assert(context == &provider);
    return authorized && vco_session_matches(&session, token, true);
}
static int pipeline_write(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                          size_t size)
{
    const uint8_t expected[] = {0x44, 0x33, 0x22, 0x11};
    assert(pipeline_alive(context, token));
    /* B207 explicitly maps to UID 200, whose segment 1 starts at 0x81030000.
     * The SDK enumeration index, B index, segment and offset are all distinct. */
    assert(address == 0x81030010u && size == sizeof(expected));
    assert(!memcmp(bytes, expected, sizeof(expected)));
    ++pipeline_writes;
    return 0;
}
static void pipeline(bool automatic)
{
    initialize(4);
    uint64_t generation = snapshot();
    VcoCheatEntry entries[1];
    VcoCode codes[2];
    char names[32];
    VcoCheatOp ops[2];
    VcoPatch patches[2];
    VcoDiagnostic diagnostic;
    VcoCheatFile file = {entries, 1, 0, codes, 2, 0, names, sizeof(names), 0};
    const char *text = automatic ? "_V1 Module\n$B200 00000001 00000000\n$0200 00000010 11223344\n"
                                 : "_V1 Module\n$B207 00000001 00000000\n$0200 00000010 11223344\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    assert(file.entry_count == 1 && entries[0].file_enabled);
    VcoCheatProgram program = {ops, 2, 0, NULL, 0};
    assert(!vco_cheat_compile(&file, 0, &program, &diagnostic));
    assert(ops[0].kind == VCO_OP_MODULE && ops[0].a == (automatic ? 0u : 7u) && ops[0].b == 1);
    VcoCheatMemory callbacks = {.context = &provider,
                                .alive = pipeline_alive,
                                .write = pipeline_write,
                                .module = vco_vita_modules_resolve};
    VcoCheatRuntime runtime;
    assert(
        !vco_cheat_runtime_init(&runtime, &program, callbacks, session.token, patches, 2, 16, 0));
    pipeline_writes = 0;
    assert(!vco_cheat_tick(&runtime) && !pipeline_writes); /* _V1 never auto-enables. */
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && !pipeline_writes);

    const int32_t map[8] = {0, 0, 0, 0, 0, 0, 0, 200};
    if (automatic)
        assert(!vco_vita_modules_map_z06(&provider, session.token, generation));
    else
        assert(!vco_vita_modules_confirm_mapping(&provider, session.token, generation, map, 8));
    assert(!vco_cheat_enable(&runtime) && pipeline_writes == 1);
    assert(!vco_cheat_tick(&runtime) && pipeline_writes == 2);

    /* A real provider mismatch must stop the whole B/write sequence; the
     * writer must never receive either the old address or a newly guessed one. */
    if (automatic) {
        raw_uids[0] = 200;
        raw_uids[1] = 100;
    } else
        native[1].segments[1].vaddr = (void *)(uintptr_t)0x83000000u;
    assert(vco_cheat_tick(&runtime) == VCO_E_ADDRESS);
    assert(runtime.state == VCO_CHEAT_ERROR && runtime.diagnostic.line == 2);
    assert(pipeline_writes == 2 && !provider.mapped && !provider.generation);
    assert(!vco_cheat_tick(&runtime) && pipeline_writes == 2);
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && pipeline_writes == 2);
}
static void background_pipeline(void)
{
    session.runnable = false;
    initialize(4);
    uint64_t generation = snapshot();
    assert(!vco_vita_modules_map_z06(&provider, session.token, generation));
    /* A list compiled in the background is all OFF. Its prebuilt B mapping
     * becomes usable only after foreground authority is observed again. */
    VcoCheatEntry entry = {0, 0, 2, 1, {0, 0}};
    VcoCode codes[] = {{0xb200, 1, 0, 1}, {0x0200, 0x10, 0x11223344, 2}};
    VcoCheatFile file = {&entry, 1, 1, codes, 2, 2, NULL, 0, 0};
    VcoCheatOp ops[2];
    VcoPatch patches[2];
    VcoCheatMemory callbacks = {.context = &provider,
                                .alive = pipeline_alive,
                                .write = pipeline_write,
                                .module = vco_vita_modules_resolve};
    VcoEngine engine;
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, callbacks, ops, patches, 2));
    pipeline_writes = 0;
    assert(engine.rows[0].state == VCO_ENTRY_OFF);
    VcoExecutionCommand enable = {1, session.token, 1, 0, true};
    assert(vco_engine_submit(&engine, enable) == VCO_E_SESSION);
    vco_engine_step(&engine, 1);
    assert(!pipeline_writes && provider.mapped);
    session.runnable = true;
    assert(!vco_engine_submit(&engine, enable));
    vco_engine_step(&engine, 2);
    assert(pipeline_writes == 1 && engine.rows[0].state == VCO_ENTRY_ON);
    assert(!vco_engine_unbind(&engine));

    /* Foreground revocation pauses access while retaining the same mapping. */
    revoke_validate_at = validations + 2;
    resolve_failed(session.token, 0, 1);
    assert(provider.mapped);
    session.runnable = true;
    uint32_t base = 0, size = 0;
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 1, &base, &size));
}
static unsigned growth_writes, growth_injections, growth_releases;
static uint64_t growth_stamp;
static uint32_t growth_target;
static int growth_write(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                        size_t size)
{
    assert(pipeline_alive(context, token) && address == growth_target + 0x14 && size == 4 &&
           bytes[0] == 1);
    ++growth_writes;
    return 0;
}
static int growth_inject(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                         size_t size, int32_t *handle)
{
    assert(pipeline_alive(context, token) && address == growth_target + 0x10 && size == 4 &&
           bytes[0] == 1);
    assert(!vco_vita_modules_guard(&provider, token, address, (uint32_t)size, &growth_stamp));
    assert(growth_stamp == provider.generation);
    *handle = 123;
    ++growth_injections;
    return 0;
}
static int growth_release(void *context, uint64_t token, int32_t handle)
{
    assert(context == &provider && handle == 123 && growth_stamp == provider.generation);
    assert(!vco_vita_modules_guard(&provider, token, growth_target + 0x10, 4, &growth_stamp));
    ++growth_releases;
    growth_stamp = 0;
    return 0;
}
static void enable_growth(bool query_failure)
{
    /* Actual device sequence: initial list bind sees 17 modules, first manual
     * B200 sees 30. Exercise the real engine/provider, not a simulated remap. */
    for (unsigned reproduce_old = 0; reproduce_old < 2; ++reproduce_old) {
        setup();
        initialize(32);
        native_count = 17;
        uint64_t initial = snapshot();
        assert(!vco_vita_modules_map_z06(&provider, session.token, initial));
        VcoCheatEntry entries[2] = {{0, 0, 2, 1, {0, 0}}, {0, 2, 2, 0, {0, 0}}};
        VcoCode codes[] = {{0xb200, 0, 0, 31},
                           {0xa200, 0x10, 0xe3a00001, 32},
                           {0xb200, 0, 0, 34},
                           {0x0200, 0x14, 1, 35}};
        VcoCheatFile file = {entries, 2, 2, codes, 4, 4, NULL, 0, 0};
        VcoCheatOp ops[4];
        VcoPatch patches[4];
        VcoCheatMemory callbacks = {.context = &provider,
                                    .alive = pipeline_alive,
                                    .write = growth_write,
                                    .module = vco_vita_modules_resolve,
                                    .inject = growth_inject,
                                    .release = growth_release};
        VcoEngine engine;
        vco_engine_init(&engine);
        assert(!vco_engine_bind(&engine, &session, 1, &file, callbacks, ops, patches, 4));
        growth_writes = growth_injections = growth_releases = 0;
        growth_stamp = 0;
        native_count = 30;
        growth_target = (uint32_t)(uintptr_t)native[29].segments[0].vaddr;
        uint64_t request = 1;
        if (reproduce_old) {
            assert(!vco_engine_submit(&engine,
                                      (VcoExecutionCommand){request++, session.token, 1, 0, true}));
            vco_engine_step(&engine, 0);
            assert(engine.rows[0].state == VCO_ENTRY_ERROR &&
                   engine.rows[0].diagnostic.code == VCO_E_ADDRESS &&
                   engine.rows[0].diagnostic.line == 31 && !growth_injections && !growth_writes &&
                   !provider.mapped);
            assert(provider.failure.event == VCO_MODULE_TRACE_LIST_CHANGED);
        }
        assert(!vco_engine_set_idle_prepare(&engine, vco_vita_modules_prepare_idle, &provider));
        if (query_failure) {
            info_error = (int32_t)0x8002d082u;
            assert(!vco_engine_submit(&engine,
                                      (VcoExecutionCommand){request++, session.token, 1, 0, true}));
            vco_engine_step(&engine, 1);
            assert(engine.rows[0].state == VCO_ENTRY_ERROR &&
                   engine.rows[0].diagnostic.line == 31 && !provider.mapped && !growth_injections &&
                   !growth_writes);
            assert(provider.failure.event == VCO_MODULE_TRACE_INFO &&
                   provider.failure.result == info_error);
            info_error = 0;
        }
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){request++, session.token, 1, 0, true}));
        vco_engine_step(&engine, 2);
        assert(engine.rows[0].state == VCO_ENTRY_ON && provider.count == 30 &&
               provider.generation > initial && growth_injections == 1 && !growth_writes);
        uint64_t running = provider.generation;
        assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 1, true}));
        vco_engine_step(&engine, 3);
        assert(engine.rows[1].state == VCO_ENTRY_ON && growth_writes == 1 &&
               growth_injections == 1 && provider.generation == running);
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){request++, session.token, 1, 0, false}));
        assert(!vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 1, false}));
        vco_engine_step(&engine, 4);
        assert(growth_releases == 1 && !growth_stamp && provider.generation == running);
        /* A later manual enable can sample more newly loaded modules, after
         * restoration and OFF acknowledgements, without reusing old stamps. */
        native_count = 31;
        growth_target = (uint32_t)(uintptr_t)native[30].segments[0].vaddr;
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){request++, session.token, 1, 0, true}));
        vco_engine_step(&engine, 5);
        assert(engine.rows[0].state == VCO_ENTRY_ON && provider.generation > running &&
               growth_injections == 2 && growth_releases == 1);
        assert(!vco_engine_unbind(&engine) && growth_releases == 2 && !growth_stamp);
    }
}
static void patch_guards(void)
{
    prepare();
    uint64_t stamp = 0;
    assert(!vco_vita_modules_guard(&provider, session.token, 0x81020010, 4, &stamp));
    assert(stamp == provider.generation);
    session.runnable = false;
    assert(!vco_vita_modules_guard(&provider, session.token, 0x81020010, 4, &stamp));
    uint64_t saved = stamp;
    native[1].segments[0].vaddr = (void *)(uintptr_t)0x85000000;
    assert(vco_vita_modules_guard(&provider, session.token, 0x81020010, 4, &stamp) < 0);
    assert(stamp == saved && !provider.generation && !provider.mapped);

    setup();
    prepare();
    stamp = provider.generation;
    uint64_t old = stamp;
    uint64_t fresh = snapshot();
    assert(!vco_vita_modules_map_z06(&provider, session.token, fresh));
    unsigned calls = infos;
    assert(vco_vita_modules_guard(&provider, session.token, 0x81020010, 4, &stamp) < 0);
    assert(stamp == old && calls == infos && provider.generation == fresh);
    stamp = 0;
    assert(vco_vita_modules_guard(&provider, session.token, 0x8102fffe, 4, &stamp) < 0);
    assert(!stamp); /* Straddling module segments does not establish ownership. */
    assert(!vco_vita_modules_guard(&provider, session.token, 0x90000000, 4, &stamp));
    assert(!stamp &&
           infos == calls + 2 * provider.count); /* Negative membership verifies all records. */
    stamp = provider.generation;
    reordered = true;
    assert(vco_vita_modules_guard(&provider, session.token, 0x81020010, 4, &stamp) < 0);
    assert(!provider.mapped);

    setup();
    prepare();
    stamp = 0;
    assert(!vco_vita_modules_guard(&provider, session.token, 0x81020010, 4, &stamp));
    authorized = false;
    assert(vco_vita_modules_guard(&provider, session.token, 0x81020010, 4, &stamp) < 0);
    assert(!provider.mapped);
}
static void negative_guards(void)
{
    for (unsigned variant = 0; variant < 5; ++variant) {
        setup();
        prepare();
        uint64_t stamp = 0;
        assert(!vco_vita_modules_guard(&provider, session.token, 0x90000000, 4, &stamp) && !stamp);
        if (variant == 0)
            native[0].segments[0].vaddr = (void *)(uintptr_t)0x90000000;
        else if (variant == 1)
            native[1].segments[1].vaddr = (void *)(uintptr_t)0x90000000;
        else if (variant == 2)
            native[0].path[0] ^= 1;
        else if (variant == 3) {
            change_info_at = infos + 1; /* Changed between consecutive descriptions. */
            change_kind = 2;
        } else
            fault_at = operations + 7;
        assert(vco_vita_modules_guard(&provider, session.token, 0x90000000, 4, &stamp) < 0 &&
               !stamp && !provider.mapped && !provider.generation);
    }
}
static void ownership_segments(void)
{
    for (unsigned segment = 2; segment < VCO_VITA_MODULE_RECORD_SEGMENTS; ++segment) {
        setup();
        native[0].segments[segment] = (SceKernelSegmentInfo){
            sizeof(SceKernelSegmentInfo), 5, (void *)(uintptr_t)0x90000000u, 0x1000, 0x1000, 0};
        prepare();
        uint64_t stamp = 0;
        assert(!vco_vita_modules_guard(&provider, session.token, 0x90000010, 4, &stamp) &&
               stamp == provider.generation);
        uint32_t base = 123, size = 456;
        assert(vco_vita_modules_resolve(&provider, session.token, 1, segment, &base, &size) < 0 &&
               base == 123 && size == 456); /* Extra ownership metadata does not extend B syntax. */
        native[0].segments[segment].vaddr = (void *)(uintptr_t)0x91000000u;
        uint64_t saved = stamp;
        assert(vco_vita_modules_guard(&provider, session.token, 0x90000010, 4, &stamp) < 0 &&
               stamp == saved && !provider.mapped);

        setup();
        prepare();
        stamp = 0;
        assert(!vco_vita_modules_guard(&provider, session.token, 0x90000010, 4, &stamp) && !stamp);
        native[0].segments[segment] = (SceKernelSegmentInfo){
            sizeof(SceKernelSegmentInfo), 5, (void *)(uintptr_t)0x90000000u, 0x1000, 0x1000, 0};
        assert(vco_vita_modules_guard(&provider, session.token, 0x90000010, 4, &stamp) < 0 &&
               !stamp &&
               !provider.mapped); /* Negative ownership includes the third/fourth segment. */

        setup();
        native[0].segments[segment].vaddr = (void *)(uintptr_t)0xfffffffdu;
        native[0].segments[segment].memsz = 4;
        initialize(2);
        uint64_t generation = 123;
        unsigned count = 456;
        assert(vco_vita_modules_snapshot(&provider, session.token, &generation, &count) < 0 &&
               generation == 123 && count == 456 && !provider.generation);
    }
}

static VcoVitaModuleFailure diagnostics[16];
static unsigned diagnostic_count;

static void capture_diagnostic(void *context, uint32_t event, int32_t result, uint32_t subject,
                               uint32_t detail)
{
    assert(context == &diagnostic_count && diagnostic_count < 16);
    diagnostics[diagnostic_count++] = (VcoVitaModuleFailure){event, subject, detail, result};
}

static void diagnose(void)
{
    diagnostic_count = 0;
    vco_vita_modules_set_trace(&provider, capture_diagnostic, &diagnostic_count);
}

static void diagnostic_cases(void)
{
    setup();
    initialize(2);
    diagnose();
    info_error = (int32_t)0x8002d082u;
    uint64_t generation = 123;
    unsigned count = 456;
    assert(vco_vita_modules_snapshot(&provider, session.token, &generation, &count) < 0);
    assert(generation == 123 && count == 456 && !provider.mapped);
    assert(diagnostic_count == 1 && diagnostics[0].event == VCO_MODULE_TRACE_INFO &&
           diagnostics[0].result == info_error && diagnostics[0].subject == 100);
    unsigned before = operations;
    diagnostic_count = 0;
    resolve_failed(session.token, 0, 0);
    assert(operations == before && diagnostic_count == 2);
    assert(diagnostics[0].event == VCO_MODULE_TRACE_INFO && diagnostics[0].result == info_error &&
           diagnostics[0].subject == 100);
    assert(diagnostics[1].event == VCO_MODULE_TRACE_UNAVAILABLE &&
           diagnostics[1].detail == VCO_MODULE_TRACE_INFO && diagnostics[1].result == info_error);

    setup();
    initialize(2);
    diagnose();
    memset(native[0].path, 'X', sizeof(native[0].path));
    assert(vco_vita_modules_snapshot(&provider, session.token, &generation, &count) < 0);
    assert(diagnostic_count == 1 && diagnostics[0].event == VCO_MODULE_TRACE_FIELDS &&
           diagnostics[0].subject == 100 && diagnostics[0].detail == 3);

    setup();
    firmware[0] = -1;
    assert(vco_vita_modules_init(&provider, &data, arena, 2) < 0);
    diagnose();
    assert(diagnostic_count == 1 && diagnostics[0].event == VCO_MODULE_TRACE_INIT &&
           diagnostics[0].detail == 6);

    setup();
    initialize(2);
    diagnose();
    uint64_t token = snapshot();
    assert(diagnostic_count == 1 && diagnostics[0].event == VCO_MODULE_TRACE_SNAPSHOT &&
           diagnostics[0].subject == 42 && diagnostics[0].detail == 2);
    diagnostic_count = 0;
    assert(!vco_vita_modules_map_z06(&provider, session.token, token));
    uint32_t base = 123, size = 456;
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
    assert(base == 0x81020000u && size == 0x10000 && !diagnostic_count);
    /* A later successful all-OFF reload clears a previous SDK failure. */
    info_error = (int32_t)0x8002d082u;
    assert(vco_vita_modules_snapshot(&provider, session.token, &generation, &count) < 0);
    assert(provider.failure.event == VCO_MODULE_TRACE_INFO);
    info_error = 0;
    (void)snapshot();
    assert(!provider.failure.event);
}

static void empty_segments(void)
{
    for (unsigned segment = 0; segment < VCO_VITA_MODULE_RECORD_SEGMENTS; ++segment) {
        setup();
        initialize(2);
        /* An unrelated loaded module may have an addressed empty segment. It
         * must not prevent B200 from reaching another module's valid text. */
        native[0].segments[segment].vaddr = (void *)(uintptr_t)0x90000000u;
        native[0].segments[segment].memsz = native[0].segments[segment].filesz = 0;
        native[0].segments[segment].perms = 6;
        uint64_t generation = snapshot();
        assert(arena[0].segments[segment].base == 0x90000000u && !arena[0].segments[segment].size &&
               arena[0].segments[segment].permissions == 6);
        assert(!vco_vita_modules_map_z06(&provider, session.token, generation));
        uint32_t base = 0, size = 0;
        assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
        assert(base == 0x81020000u && size == 0x10000u);
        if (segment < VCO_VITA_MODULE_SEGMENTS) {
            resolve_failed(session.token, 1, segment);
            assert(provider.mapped); /* Reject only the requested empty range. */
        }
        uint64_t stamp = 0;
        assert(!vco_vita_modules_guard(&provider, session.token, 0x90000000u, 4, &stamp));
        assert(!stamp); /* An empty descriptor claims no address or patch ownership. */
        native[0].segments[segment].perms ^= 1;
        resolve_failed(session.token, 1, segment == 0 ? 1 : 0);
        assert(provider.failure.event == VCO_MODULE_TRACE_RECORD_CHANGED && !provider.mapped);
    }
    setup();
    initialize(2);
    native[0].segments[1].vaddr = (void *)(uintptr_t)0x1000;
    native[0].segments[1].memsz = 0;
    uint64_t generation = snapshot();
    assert(!vco_vita_modules_map_z06(&provider, session.token, generation));
    resolve_failed(session.token, 1, 1); /* A low address with zero length is still inaccessible. */
    native[0].segments[1].memsz = 4;
    snapshot_failed(); /* Positive length re-enables the original address floor. */
    assert(provider.failure.event == VCO_MODULE_TRACE_FIELDS && provider.failure.detail == 5);
}

static void segment_diagnostics(void)
{
    setup();
    initialize(2);
    diagnose();
    native[0].segments[1].vaddr = (void *)(uintptr_t)0x81010000u;
    native[0].segments[1].memsz = 0;
    uint64_t generation = snapshot();
    assert(diagnostic_count == 5);
    assert(diagnostics[0].event == VCO_MODULE_TRACE_SEGMENT_BASE &&
           (uint32_t)diagnostics[0].result == 0x81010000u && diagnostics[0].subject == 100 &&
           diagnostics[0].detail == 1);
    assert(diagnostics[1].event == VCO_MODULE_TRACE_SEGMENT_SIZE && !diagnostics[1].result);
    assert(diagnostics[2].event == VCO_MODULE_TRACE_SEGMENT_PERMISSIONS &&
           diagnostics[2].result == 6);
    assert(diagnostics[3].event == VCO_MODULE_TRACE_EMPTY &&
           diagnostics[4].event == VCO_MODULE_TRACE_SNAPSHOT);
    assert(!vco_vita_modules_map_z06(&provider, session.token, generation));
    uint32_t base = 0, size = 0;
    assert(!vco_vita_modules_resolve(&provider, session.token, 0, 0, &base, &size));
    assert(diagnostic_count == 5); /* No extra events on normal mapping/resolution. */

    setup();
    initialize(2);
    diagnose();
    native[0].segments[1].vaddr = (void *)(uintptr_t)0x1000;
    snapshot_failed();
    assert(diagnostic_count == 4 && diagnostics[0].event == VCO_MODULE_TRACE_SEGMENT_BASE &&
           diagnostics[0].result == 0x1000 && diagnostics[1].result == 0x10000 &&
           diagnostics[2].result == 6 && diagnostics[3].event == VCO_MODULE_TRACE_FIELDS &&
           diagnostics[3].detail == 5);
    resolve_failed(session.token, 0, 0);
    assert(diagnostic_count == 6 && diagnostics[4].event == VCO_MODULE_TRACE_FIELDS &&
           diagnostics[5].event == VCO_MODULE_TRACE_UNAVAILABLE);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "firmware"))
        firmware_case();
    else if (!strcmp(argv[1], "snapshots"))
        snapshots();
    else if (!strcmp(argv[1], "mapping"))
        mapping();
    else if (!strcmp(argv[1], "auto_mapping"))
        auto_mapping();
    else if (!strcmp(argv[1], "boundaries"))
        boundaries();
    else if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline(false);
    else if (!strcmp(argv[1], "auto_pipeline"))
        pipeline(true);
    else if (!strcmp(argv[1], "background_pipeline"))
        background_pipeline();
    else if (!strcmp(argv[1], "enable_growth"))
        enable_growth(false);
    else if (!strcmp(argv[1], "enable_growth_failure"))
        enable_growth(true);
    else if (!strcmp(argv[1], "patch_guards"))
        patch_guards();
    else if (!strcmp(argv[1], "negative_guards"))
        negative_guards();
    else if (!strcmp(argv[1], "ownership_segments"))
        ownership_segments();
    else if (!strcmp(argv[1], "diagnostics"))
        diagnostic_cases();
    else if (!strcmp(argv[1], "empty_segments"))
        empty_segments();
    else if (!strcmp(argv[1], "segment_diagnostics"))
        segment_diagnostics();
    else
        assert(0);
    printf("modules %s passed; provider %zu bytes, record %zu bytes\n", argv[1], sizeof(provider),
           sizeof(arena[0]));
    return 0;
}
