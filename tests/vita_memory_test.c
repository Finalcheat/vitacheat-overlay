#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2/kernel/error.h>
#include <taihen.h>
#include "platform/vita/memory.h"
#include "platform/vita/memblock_lookup.h"
#include "platform/vita/patches.h"
#include "execution/engine.h"
#include "execution/trace.h"
#define BASE 0x81000000u
#define FAKE_PATCH_BASE 1000
#define FAKE_PATCH_LIMIT (VCO_ENGINE_CLAIMS + 16u)
static SceClass cls;
static SceObjectBase object, replacement;
static SceObjectBase *current;
static unsigned memtype;
static uint8_t bytes[32];
static int references, locks, reads, writes, unmaps, firmware, lookups;
static bool foreground, authorized, mapped;
static unsigned fault, operations;
static unsigned type_lookups;
static unsigned injected, restored;
static bool fail_inject, fail_restore, revoke_after_inject;
static int32_t fail_inject_result = -1;
static bool revoke_after_unlock;
static unsigned revoke_inject_at;
static bool deny_private_restore;
static VcoAdmission *close_on_inject, *close_on_restore, *close_on_copy;
static VcoAdmission *observe_closing;
static unsigned inject_mutation;
static unsigned guard_calls, guard_fail_at;
static unsigned guard_revoke_at;
static bool guard_invalid, guard_wrong_generation, guard_scripted;
static uint64_t guard_script[8];
static unsigned close_uid_on_guard;
static unsigned replace_uid_on_guard, restore_uid_mutation;
static unsigned fail_uid_query_on_guard;
static VcoVitaPatchSupport support;
static unsigned guid_ops, guid_fault, probe_refs;
static bool probe_open, closed_referenceable, null_guid_object, changing_closed_error;
static SceObjectBase probe_object;
static int closed_error;
static SceUID block_uid;
/* Sparse game address space for the exact device-supplied PCSB00031 fixture.
 * Roots belong to a read-only module block, objects to a separate RW block. */
static bool pcsb_mode;
static bool pcsb_patch_mode;
static struct {
    uint32_t address;
    uint8_t bytes[4];
} pcsb_instructions[VCO_ENGINE_CLAIMS + 1u];
static unsigned pcsb_instruction_count;
static uint32_t locked_address;
static struct {
    uint32_t address, value;
} pcsb_cells[] = {{0x8147C5F4, 0x81000100}, {0x81000100, 0x81000200}, {0x81000300, 0x81000400},
                  {0x81000404, 0x81000500}, {0x8100050C, 9},          {0x81000520, 9},
                  {0x8147C258, 0x81001000}, {0x81001000, 0x81002000}, {0x8100214C, 0x81003000},
                  {0x810032D8, 0x81004000}, {0x81004004, 0},          {0x81D80774, 0x81010000},
                  {0x81010268, 0x81020000}, {0x81020010, 0x81030000}, {0x8103000C, 0x81040000},
                  {0x81040024, 0x81050000}, {0x81050010, 9},          {0x8105000C, 0x12345678}};
static uint8_t *pcsb_instruction(uintptr_t address, size_t size)
{
    if (pcsb_patch_mode)
        for (unsigned i = 0; i < pcsb_instruction_count; ++i)
            if (address >= pcsb_instructions[i].address &&
                address + size <= (uint64_t)pcsb_instructions[i].address + 4)
                return pcsb_instructions[i].bytes + (address - pcsb_instructions[i].address);
    return NULL;
}
static uint8_t *pcsb_cell(uintptr_t address, size_t size)
{
    for (unsigned i = 0; i < sizeof(pcsb_cells) / sizeof(pcsb_cells[0]); ++i)
        if (address >= pcsb_cells[i].address &&
            address + size <= (uint64_t)pcsb_cells[i].address + 4)
            return (uint8_t *)&pcsb_cells[i].value + (address - pcsb_cells[i].address);
    return pcsb_instruction(address, size);
}
static SceObjectBase block_objects[3], block_replacement;
static SceObjectBase *block_current[3];
static unsigned block_references[3];
static bool fail_find, fail_unlock;
static unsigned find_calls, nonzero_find_calls;
static uint32_t split_block_at, fail_find_at;
static bool fail_patch_reference_drop, fail_block_reference_drop;
enum {
    BLOCK_CHANGE_NONE,
    BLOCK_CHANGE_UID,
    BLOCK_CHANGE_OBJECT,
    BLOCK_CHANGE_FIND,
    BLOCK_CHANGE_REFER,
    BLOCK_CHANGE_RELEASE
};
enum {
    BLOCK_BOUNDARY_UNLOCK,
    BLOCK_BOUNDARY_GUARD,
    BLOCK_BOUNDARY_NATIVE_QUERY,
    BLOCK_BOUNDARY_NATIVE_RELEASE
};
static unsigned publication_block_change, publication_block_boundary;
static bool publication_block_changed, fail_temp_block_reference, fail_temp_block_release;
static struct {
    uint8_t saved[4];
    uint32_t address;
    size_t size;
    bool active;
    bool uid_open;
    unsigned references;
    SceObjectBase object;
    SceObjectBase *current;
} patches[FAKE_PATCH_LIMIT];
static void change_publication_block(unsigned boundary)
{
    if (!publication_block_change || publication_block_changed ||
        boundary != publication_block_boundary)
        return;
    assert(injected == 1 && !locks && patches[0].references == 1 && block_references[0] == 1);
    publication_block_changed = true;
    if (publication_block_change == BLOCK_CHANGE_UID)
        block_uid = 78;
    else if (publication_block_change == BLOCK_CHANGE_OBJECT)
        block_current[0] = &block_replacement;
    else if (publication_block_change == BLOCK_CHANGE_FIND)
        fail_find = true;
    else if (publication_block_change == BLOCK_CHANGE_REFER)
        fail_temp_block_reference = true;
    else {
        assert(publication_block_change == BLOCK_CHANGE_RELEASE);
        fail_temp_block_release = true;
    }
    memset(bytes, 0xcc, sizeof(bytes)); /* New/unknown occupant after the range unlock. */
}
static bool replace_after_lock, revoke_after_copy;
static bool fail_preflight_copy, replace_after_read;
static int preflight_copy_error;
static int failure(void)
{
    return ++operations == fault;
}
static SceClass *get_class(void)
{
    return &cls;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address)
{
    assert(pid == KERNEL_PID);
    bool type = !strcmp(module, "SceSysmem");
    assert(type || !strcmp(module, "SceProcessmgr"));
    unsigned index = type ? type_lookups++ : (unsigned)lookups++;
    assert(index < 2);
    const uint32_t libraries[2][2] = {{0x7A69DE86, 0xEB1F8EF7}, {0x63A519E5, 0x02451F0F}};
    const uint32_t functions[2][2] = {{0xC6820972, 0x98AE4BC8}, {0x289BE3EC, 0xD44FE44B}};
    assert(library == libraries[type][index] && function == functions[type][index]);
    if ((int)index != firmware || failure())
        return -1;
    *address = type ? (uintptr_t)ksceKernelGetMemBlockType : (uintptr_t)get_class;
    return 0;
}

int ksceGUIDReferObjectWithClass(SceUID pid, SceClass *type, SceObjectBase **out)
{
    assert(pid == 42 && type == &cls);
    if (failure())
        return -1;
    ++references;
    *out = current;
    return 0;
}
int ksceGUIDReleaseObject(SceUID pid)
{
    if (observe_closing)
        assert(!vco_admission_drained(observe_closing));
    if (pid >= 77 && pid <= 79) {
        unsigned i = (unsigned)pid - 77;
        assert(block_references[i]);
        --block_references[i];
        bool temporary_failure = fail_temp_block_release && !locks && block_references[i] == 1;
        if (temporary_failure)
            fail_temp_block_release = false;
        return ++guid_ops == guid_fault || temporary_failure ||
                       (fail_block_reference_drop && !block_references[i])
                   ? -1
                   : 0;
    }
    if (pid == 500) {
        assert(probe_refs);
        --probe_refs;
        return ++guid_ops == guid_fault ? -1 : 0;
    }
    if (pid >= FAKE_PATCH_BASE && pid < FAKE_PATCH_BASE + (int)FAKE_PATCH_LIMIT) {
        unsigned i = (unsigned)(pid - FAKE_PATCH_BASE);
        assert(patches[i].references);
        --patches[i].references;
        if (!locks)
            change_publication_block(BLOCK_BOUNDARY_NATIVE_RELEASE);
        return ++guid_ops == guid_fault || (fail_patch_reference_drop && !patches[i].uid_open) ? -1
                                                                                               : 0;
    }
    assert(pid == 42 && references > 0);
    --references;
    return failure() ? -1 : 0;
}
int ksceGUIDReferObject(SceUID uid, SceObjectBase **out)
{
    if (++guid_ops == guid_fault)
        return -1;
    if (uid >= 77 && uid <= 79) {
        unsigned i = (unsigned)uid - 77;
        if (fail_temp_block_reference && !locks) {
            fail_temp_block_reference = false;
            return -1;
        }
        ++block_references[i];
        *out = block_current[i];
        return 0;
    }
    if (uid == 500) {
        if (!probe_open && !closed_referenceable)
            return changing_closed_error && guid_ops % 2 ? (int)SCE_KERNEL_ERROR_UNKNOWN_UID
                                                         : closed_error;
        ++probe_refs;
        *out = null_guid_object ? NULL : &probe_object;
        return 0;
    }
    assert(uid >= FAKE_PATCH_BASE && uid < FAKE_PATCH_BASE + (int)FAKE_PATCH_LIMIT);
    unsigned i = (unsigned)(uid - FAKE_PATCH_BASE);
    if (!patches[i].uid_open)
        return closed_error;
    if (!locks)
        change_publication_block(BLOCK_BOUNDARY_NATIVE_QUERY);
    ++patches[i].references;
    *out = null_guid_object ? NULL : patches[i].current;
    return 0;
}
SceUID ksceKernelCreateEventFlag(const char *name, int attributes, int bits,
                                 SceKernelEventFlagOptParam *options)
{
    assert(!strcmp(name, "VCOGuidProbe") && !attributes && !bits && !options);
    assert(!probe_open && !probe_refs);
    if (++guid_ops == guid_fault)
        return -1;
    probe_open = true;
    return 500;
}
int ksceKernelDeleteEventFlag(SceUID uid)
{
    assert(uid == 500 && probe_open);
    if (++guid_ops == guid_fault)
        return -1;
    probe_open = false;
    return 0;
}
static bool range(const void *addr, unsigned length)
{
    if (pcsb_mode)
        return pcsb_cell((uintptr_t)addr, length) != NULL;
    return (uintptr_t)addr >= BASE && (uintptr_t)addr + length <= BASE + sizeof(bytes);
}
int ksceKernelLockRangeProc(SceUID pid, void *addr, SceSize size)
{
    assert(pid == 42 && !locks);
    if (failure() || !mapped || !range(addr, size))
        return -1;
    ++locks;
    locked_address = (uint32_t)(uintptr_t)addr;
    if (replace_after_lock)
        current = &replacement;
    if (restore_uid_mutation && injected) {
        assert(patches[0].uid_open && patches[0].references == 1);
        if (restore_uid_mutation == 1) {
            patches[0].uid_open = false;
            patches[0].active = false;
        } else
            patches[0].current = &replacement;
        memset(bytes, 0xcc, sizeof(bytes));
    }
    return 0;
}
int ksceKernelUnlockRangeProc(SceUID pid, void *addr, SceSize size)
{
    if (observe_closing)
        assert(!vco_admission_drained(observe_closing));
    assert(pid == 42 && locks == 1 && range(addr, size));
    --locks;
    ++unmaps;
    change_publication_block(BLOCK_BOUNDARY_UNLOCK);
    if (revoke_after_unlock && injected && !restored)
        foreground = false;
    return failure() || fail_unlock ? -1 : 0;
}
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    assert(pid == 42);
    ++find_calls;
    nonzero_find_calls += size != 0;
    if (failure() || fail_find || !mapped || !range(addr, size ? size : 1) ||
        (fail_find_at && (uintptr_t)addr == fail_find_at))
        return -1;
    /* Captured Sysmem aca5dc: zero selects a containing block; nonzero
     * requires the allocation's exact base AND extent, not a byte range. */
    if (size && (pcsb_mode || (uintptr_t)addr != BASE || size != sizeof(bytes)))
        return (int32_t)UINT32_C(0x80020001);
    if (split_block_at && (uintptr_t)addr >= split_block_at)
        return 79;
    if (pcsb_instruction((uintptr_t)addr, 1))
        return 79;
    return pcsb_mode && (uintptr_t)addr < 0x81400000 ? 78 : block_uid;
}
int ksceKernelGetMemBlockType(SceUID block, unsigned *out)
{
    assert((block == block_uid || (pcsb_mode && block == 78) || (pcsb_patch_mode && block == 79)) &&
           locks == 1);
    if (failure())
        return -1;
    *out = block == 79                       ? SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RX
           : pcsb_mode && block == block_uid ? SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R
                                             : memtype;
    return 0;
}
int ksceKernelCopyFromUserProc(SceUID pid, void *out, const void *addr, SceSize size)
{
    if (close_on_copy) {
        vco_admission_close(close_on_copy);
        assert(!vco_admission_drained(close_on_copy));
    }

    assert(pid == 42 && locks == 1 && range(addr, size));
    ++reads;
    if (revoke_after_copy)
        foreground = false;
    if (failure() || fail_preflight_copy) {
        memset(out, 0xee, size);
        return fail_preflight_copy ? preflight_copy_error : -1;
    }
    memcpy(out, pcsb_mode ? pcsb_cell((uintptr_t)addr, size) : bytes + ((uintptr_t)addr - BASE),
           size);
    if (replace_after_read)
        current = &replacement;
    return 0;
}
int ksceKernelCopyToUserProc(SceUID pid, void *addr, const void *in, SceSize size)
{
    if (close_on_copy) {
        vco_admission_close(close_on_copy);
        assert(!vco_admission_drained(close_on_copy));
    }

    assert(pid == 42 && locks == 1 && range(addr, size));
    assert(memtype == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW);
    assert(!pcsb_mode || locked_address < 0x81400000);
    ++writes;
    if (failure())
        return -1;
    memcpy(pcsb_mode ? pcsb_cell((uintptr_t)addr, size) : bytes + ((uintptr_t)addr - BASE), in,
           size);
    if (revoke_after_copy)
        foreground = false;
    return 0;
}
int32_t taiInjectAbsForKernel(int32_t pid, void *dest, const void *src, size_t size)
{
    assert(pid == 42 && locks == 1 && range(dest, (unsigned)size) && size <= 4);
    if (close_on_inject) {
        vco_admission_close(close_on_inject);
        assert(!vco_admission_drained(close_on_inject));
    }
    if (fail_inject)
        return fail_inject_result;
    unsigned i = injected++;
    assert(i < FAKE_PATCH_LIMIT);
    patches[i].address = (uint32_t)(uintptr_t)dest;
    patches[i].size = size;
    patches[i].active = true;
    patches[i].uid_open = true;
    patches[i].current = &patches[i].object;
    uint8_t *destination =
        pcsb_mode ? pcsb_cell(patches[i].address, size) : bytes + (patches[i].address - BASE);
    assert(destination);
    memcpy(patches[i].saved, destination, size);
    memcpy(destination, src, size);
    if (revoke_after_inject || injected == revoke_inject_at)
        foreground = false;
    if (inject_mutation) {
        if (inject_mutation == 1)
            block_uid = 78;
        else if (inject_mutation == 2)
            block_current[0] = &block_replacement;
        else
            fail_find = true;
        memset(bytes, 0xcc, sizeof(bytes)); /* New/unknown occupant after native injection. */
    }
    return (int32_t)i + FAKE_PATCH_BASE;
}
int taiInjectReleaseForKernel(int32_t handle)
{
    assert(handle >= FAKE_PATCH_BASE && handle < FAKE_PATCH_BASE + (int)FAKE_PATCH_LIMIT &&
           patches[handle - FAKE_PATCH_BASE].active && locks == 1);
    if (close_on_restore) {
        vco_admission_close(close_on_restore);
        assert(!vco_admission_drained(close_on_restore));
    }
    unsigned i = (unsigned)(handle - FAKE_PATCH_BASE);
    ++restored;
    patches[i].active = false;
    patches[i].uid_open = false;
    if (fail_restore)
        return -1;
    uint8_t *destination = pcsb_mode ? pcsb_cell(patches[i].address, patches[i].size)
                                     : bytes + (patches[i].address - BASE);
    assert(destination);
    memcpy(destination, patches[i].saved, patches[i].size);
    return 0;
}
static int authority(void *ctx, const VcoIdentity *id, bool require_foreground)
{
    assert(ctx == &cls && id->pid == 42 && !strcmp(id->title_id, "PCSB00031"));
    return failure() || !authorized || (require_foreground && !foreground) ||
                   (deny_private_restore && injected && !require_foreground)
               ? -1
               : 0;
}
static void reset(void)
{
    assert(!references && !locks);
    assert(!probe_refs && !probe_open);
    reads = writes = unmaps = lookups = 0;
    operations = fault = type_lookups = 0;
    firmware = 0;
    pcsb_mode = false;
    pcsb_patch_mode = false;
    pcsb_instruction_count = 0;
    current = &object;
    memtype = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW;
    authorized = foreground = mapped = true;
    replace_after_lock = revoke_after_copy = false;
    fail_preflight_copy = replace_after_read = false;
    preflight_copy_error = -1;
    memset(bytes, 0x5a, sizeof(bytes));
    injected = restored = 0;
    fail_inject = fail_restore = revoke_after_inject = false;
    revoke_after_unlock = deny_private_restore = false;
    revoke_inject_at = 0;
    inject_mutation = guard_calls = guard_fail_at = 0;
    guard_revoke_at = 0;
    close_uid_on_guard = 0;
    replace_uid_on_guard = restore_uid_mutation = 0;
    fail_uid_query_on_guard = 0;
    guard_invalid = guard_wrong_generation = guard_scripted = false;
    memset(guard_script, 0, sizeof(guard_script));
    memset(patches, 0, sizeof(patches));
    block_uid = 77;
    fail_find = fail_unlock = false;
    find_calls = nonzero_find_calls = 0;
    split_block_at = fail_find_at = 0;
    fail_patch_reference_drop = fail_block_reference_drop = false;
    publication_block_change = publication_block_boundary = 0;
    publication_block_changed = fail_temp_block_reference = fail_temp_block_release = false;
    for (unsigned i = 0; i < 3; ++i) {
        block_references[i] = 0;
        block_current[i] = &block_objects[i];
    }
    guid_ops = guid_fault = 0;
    closed_error = (int)SCE_KERNEL_ERROR_INVALID_UID;
    closed_referenceable = null_guid_object = changing_closed_error = false;
    vco_vita_patch_support_init(&support);
    assert(!vco_vita_patch_support_probe(&support));
    assert(!vco_vita_patch_support_dispose(&support));
}
static VcoVitaMemory native;
static VcoMemoryAdapter adapter;
static VcoSession session;
static VcoCheatMemory callbacks;
static int bind_traced(VcoVitaMemoryTrace callback, void *context)
{
    vco_vita_memory_init(&native, authority, &cls);
    if (callback)
        assert(!vco_vita_memory_set_trace(&native, callback, context));
    VcoIdentity id;
    if (vco_vita_memory_bind(&native, 42, "PCSB00031", &id))
        return -1;
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &id));
    adapter = (VcoMemoryAdapter){&session, vco_vita_memory_transport(&native)};
    callbacks = vco_memory_callbacks(&adapter);
    return 0;
}
static int bind(void)
{
    return bind_traced(NULL, NULL);
}
static void finish(void)
{
    bool poisoned = native.poisoned;
    assert(!locks && !block_references[0] && !block_references[1] && !block_references[2]);
    int result = vco_vita_memory_unbind(&native);
    assert(poisoned ? result < 0 : !result);
    assert(!references);
}
static void transfer(void)
{
    for (firmware = 0; firmware < 2;) {
        int version = firmware;
        assert(!bind());
        assert(lookups == version + 1);
        uint8_t value[4] = {1, 2, 3, 4}, out[4] = {0};
        for (size_t size = 1; size <= 4; size *= 2) {
            assert(!callbacks.write(callbacks.context, session.token, BASE + 1, value, size));
            assert(!callbacks.read(callbacks.context, session.token, BASE + 1, out, size));
            assert(!memcmp(out, value, size));
        }
        finish();
        reset();
        firmware = version + 1;
    }
}
static void memblock_contract(void)
{
    assert(ksceKernelFindProcMemBlockByAddr(42, (void *)(uintptr_t)(BASE + 1), 0) == 77);
    assert(ksceKernelFindProcMemBlockByAddr(42, (void *)(uintptr_t)BASE, sizeof(bytes)) == 77);
    for (unsigned width = 1; width <= 4; width *= 2)
        assert((uint32_t)ksceKernelFindProcMemBlockByAddr(42, (void *)(uintptr_t)(BASE + 1),
                                                          width) == UINT32_C(0x80020001));
    find_calls = nonzero_find_calls = 0;
    for (unsigned width = 1; width <= 4; width *= 2) {
        assert(vco_vita_find_memblock(42, BASE + 1, width) == 77);
        assert(vco_vita_find_memblock(42, BASE + sizeof(bytes) - width, width) == 77);
    }
    assert(!nonzero_find_calls);
    unsigned calls = find_calls;
    assert(vco_vita_find_memblock(42, BASE, 0) < 0);
    assert(vco_vita_find_memblock(42, BASE, 3) < 0);
    assert(vco_vita_find_memblock(42, BASE, SIZE_MAX) < 0);
    assert(vco_vita_find_memblock(42, 0x1000, 1) < 0);
    assert(vco_vita_find_memblock(42, UINT32_MAX, 2) < 0);
    assert(vco_vita_find_memblock(42, UINT32_MAX - 2, 4) < 0);
    assert(find_calls == calls);
    /* The final representable byte is valid; missing mapping still rejects. */
    assert(vco_vita_find_memblock(42, UINT32_MAX, 1) < 0 && find_calls == calls + 1);
}
static void memblock_spans(void)
{
    assert(!bind());
    uint8_t out[4], value[4] = {1, 2, 3, 4};
    split_block_at = BASE + 8; /* Adjacent block is deliberately a different UID/type. */
    for (unsigned width = 2; width <= 4; width *= 2) {
        uint32_t start = split_block_at - width + 1;
        memset(out, 0xaa, sizeof(out));
        assert(callbacks.read(callbacks.context, session.token, start, out, width) < 0);
        assert(callbacks.write(callbacks.context, session.token, start, value, width) < 0);
        assert(!reads && !writes && out[0] == 0xaa && !locks);
    }
    assert(!callbacks.write(callbacks.context, session.token, split_block_at - 4, value, 4));
    assert(writes == 1 && !locks && !nonzero_find_calls);
    split_block_at = 0;
    fail_find_at = BASE + 3;
    int before_reads = reads, before_writes = writes;
    assert(callbacks.read(callbacks.context, session.token, BASE, out, 4) < 0);
    assert(callbacks.write(callbacks.context, session.token, BASE, value, 4) < 0);
    assert(reads == before_reads && writes == before_writes && !locks);
    fail_find_at = 0;
    finish();
}
static void denied(void)
{
    assert(!bind());
    uint8_t value[4] = {0};
    unsigned rejected[] = {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R,
                           SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RX, 0xdeadbeef};
    for (unsigned i = 0; i < 3; ++i) {
        memtype = rejected[i];
        assert(callbacks.write(callbacks.context, session.token, BASE, value, 4) < 0);
    }
    assert(!writes);
    assert(callbacks.read(callbacks.context, session.token, 0x1000, value, 4) < 0);
    assert(callbacks.read(callbacks.context, session.token, 0xffffffffu, value, 4) < 0);
    assert(callbacks.read(callbacks.context, session.token, BASE + 31, value, 4) < 0);
    assert(callbacks.read(callbacks.context, session.token, BASE, value, 3) < 0);
    assert(callbacks.read(callbacks.context, session.token + 1, BASE, value, 4) < 0);
    memtype = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW;
    foreground = false;
    assert(callbacks.write(callbacks.context, session.token, BASE, value, 4) < 0);
    foreground = true;
    current = &replacement;
    assert(callbacks.write(callbacks.context, session.token, BASE, value, 4) < 0);
    current = &object;
    replace_after_lock = true;
    assert(callbacks.write(callbacks.context, session.token, BASE, value, 4) < 0);
    assert(!writes && !locks);
    finish();
    reset();
    assert(!bind());
    revoke_after_copy = true;
    memset(value, 0xaa, sizeof(value));
    assert(callbacks.read(callbacks.context, session.token, BASE, value, 4) < 0);
    assert(value[0] == 0xaa && !locks);
    finish();
}
static void lifecycle(void)
{
    VcoIdentity first, second;
    vco_vita_memory_init(&native, NULL, &cls);
    assert(vco_vita_memory_bind(&native, 42, "PCSB00031", &first) < 0);
    vco_vita_memory_init(&native, authority, &cls);
    assert(vco_vita_memory_bind(&native, KERNEL_PID, "PCSB00031", &first) < 0);
    assert(vco_vita_memory_bind(&native, 42, "NPXS10000", &first) < 0);
    assert(!vco_vita_memory_bind(&native, 42, "PCSB00031", &first));
    assert(vco_vita_memory_bind(&native, 42, "PCSB00031", &second) < 0);
    assert(!vco_vita_memory_unbind(&native));
    lookups = 0;
    type_lookups = 0;
    assert(!vco_vita_memory_bind(&native, 42, "PCSB00031", &second));
    assert(first.instance != second.instance);
    VcoMemoryTransport transport = vco_vita_memory_transport(&native);
    assert(transport.validate(&native, &first, true) < 0);
    assert(!transport.lock(&native, &second, BASE, 4));
    assert(vco_vita_memory_unbind(&native) < 0);
    assert(transport.lock(&native, &second, BASE, 4) < 0);
    assert(transport.unlock(&native, &second, BASE + 1, 4) < 0);
    assert(!transport.unlock(&native, &second, BASE, 4));
    finish();
    native.sequence = UINT64_MAX;
    assert(vco_vita_memory_bind(&native, 42, "PCSB00031", &second) < 0);
}
static int transfer_once(bool writing, uint8_t *data)
{
    return writing ? callbacks.write(callbacks.context, session.token, BASE, data, 4)
                   : callbacks.read(callbacks.context, session.token, BASE, data, 4);
}
static void faults(void)
{
    /* Inject once at every native/authority boundary in a successful transfer.
     * References must balance even when cleanup reports failure. */
    for (unsigned writing = 0; writing < 2; ++writing) {
        reset();
        assert(!bind());
        operations = 0;
        uint8_t data[4] = {0};
        assert(!transfer_once(writing, data));
        unsigned total = operations;
        finish();
        for (unsigned fail = 1; fail <= total; ++fail) {
            reset();
            assert(!bind());
            operations = 0;
            fault = fail;
            memset(data, 0xaa, sizeof(data));
            assert(transfer_once(writing, data) < 0);
            if (!writing)
                assert(data[0] == 0xaa);
            fault = 0;
            finish();
        }
    }
    reset();
    firmware = 2;
    assert(bind() < 0);
    assert(!references);
    reset();
    authorized = false;
    assert(bind() < 0);
    assert(!references);
    reset();
    assert(!bind());
    fault = operations + 1;
    assert(vco_vita_memory_unbind(&native) < 0 && native.poisoned);
    assert(vco_vita_memory_unbind(&native) < 0 && !references);
}
static VcoVitaExecution executor;
static VcoTraceRecord memory_records[16];
static unsigned memory_record_count;
static void capture_memory_trace(void *context, uint32_t event, int32_t result, uint32_t subject,
                                 uint32_t detail)
{
    assert(context == memory_records && memory_record_count < 16);
    memory_records[memory_record_count++] = (VcoTraceRecord){0, 0, event, result, subject, detail};
}
static void memory_trace_test(void)
{
    vco_vita_memory_init(&native, authority, &cls);
    assert(!vco_vita_memory_set_trace(&native, capture_memory_trace, memory_records));
    VcoIdentity id;
    assert(!vco_vita_memory_bind(&native, 42, "PCSB00031", &id));
    assert(vco_vita_memory_set_trace(&native, NULL, NULL) < 0);
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &id));
    VcoVitaPatchSupport diagnostic_support;
    vco_vita_patch_support_init(&diagnostic_support);
    assert(!vco_vita_patch_support_set_trace(&diagnostic_support, capture_memory_trace,
                                             memory_records));
    assert(!vco_vita_patch_support_probe(&diagnostic_support));
    vco_vita_execution_init(&executor, &native, &session, &diagnostic_support);
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    uint8_t out[4] = {0xAA, 0xAA, 0xAA, 0xAA}, value[4] = {1, 2, 3, 4};
    memory_record_count = 0;
    assert(!memory.read(memory.context, session.token, BASE, out, 4));
    assert(!memory.write(memory.context, session.token, BASE, value, 4));
    assert(!memory_record_count); /* No normal-transfer log traffic. */
    mapped = false;
    assert(memory.read(memory.context, session.token, BASE, out, 4) == VCO_MEMORY_LOCK);
    assert(memory_record_count == 2 && memory_records[0].event == VCO_MEMORY_TRACE_LOCK_FAILURE &&
           memory_records[0].subject == BASE && memory_records[0].detail == 4 &&
           memory_records[1].event == VCO_MEMORY_TRACE_READ_FAILURE &&
           memory_records[1].result == VCO_MEMORY_LOCK);
    mapped = true;
    memory_record_count = 0;
    memtype = 0xDEADBEEF;
    assert(memory.read(memory.context, session.token, BASE, out, 4) == VCO_MEMORY_INSPECT);
    assert(memory_record_count == 2 && memory_records[0].event == VCO_MEMORY_TRACE_TYPE_DENIED &&
           memory_records[0].detail == 0xDEADBEEF && !locks);
    memory_record_count = 0;
    memtype = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RX;
    int before = writes;
    assert(memory.write(memory.context, session.token, BASE, value, 4) == VCO_MEMORY_PERMISSION);
    assert(memory_record_count == 1 && writes == before && !locks &&
           memory_records[0].event == VCO_MEMORY_TRACE_WRITE_FAILURE &&
           memory_records[0].result == VCO_MEMORY_PERMISSION);
    memory_record_count = 0;
    memtype = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW;
    fail_preflight_copy = true;
    preflight_copy_error = (int32_t)UINT32_C(0x8002FF01);
    memset(out, 0xAA, sizeof(out));
    assert(memory.read(memory.context, session.token, BASE, out, 4) == VCO_MEMORY_COPY);
    assert(memory_record_count == 2 &&
           memory_records[0].event == VCO_MEMORY_TRACE_COPY_READ_FAILURE &&
           memory_records[0].result == preflight_copy_error && out[0] == 0xAA && !locks);
    fail_preflight_copy = false;
    memory_record_count = 0;
    foreground = false;
    assert(memory.read(memory.context, session.token, BASE, out, 4) == VCO_MEMORY_AUTHORITY);
    assert(memory_record_count == 2 &&
           memory_records[0].event == VCO_MEMORY_TRACE_AUTHORITY_FAILURE &&
           memory_records[0].subject == 42 && !locks);
    foreground = true;
    assert(!vco_vita_execution_dispose(&executor));
    finish();
    assert(!vco_vita_patch_support_dispose(&diagnostic_support));
}
static unsigned feature_calls;
static int feature_result;
static bool feature_revoke;
static uint32_t feature_base, feature_size;
static int feature_module_guard(void *ctx, uint64_t token, uint32_t address, uint32_t size,
                                uint64_t *generation)
{
    assert(ctx == &feature_base && token == session.token && address == BASE && size == 4);
    ++guard_calls;
    if (guard_calls == guard_revoke_at)
        foreground = false;
    if (guard_calls == 4)
        change_publication_block(BLOCK_BOUNDARY_GUARD);
    if (guard_calls == close_uid_on_guard) {
        assert(injected == 1 && patches[0].uid_open && patches[0].references == 1);
        patches[0].uid_open = false;
        patches[0].active = false;
    }
    if (guard_calls == replace_uid_on_guard) {
        assert(injected == 1 && patches[0].uid_open && patches[0].references == 1);
        patches[0].current = &replacement;
    }
    if (guard_calls == 6 && fail_uid_query_on_guard)
        guid_fault = guid_ops + fail_uid_query_on_guard;
    if (guard_invalid || guard_calls == guard_fail_at)
        return -1;
    if (guard_scripted) {
        assert(guard_calls <= sizeof(guard_script) / sizeof(guard_script[0]));
        *generation = guard_script[guard_calls - 1];
    } else
        *generation = guard_wrong_generation ? 18 : 17;
    return 0;
}
static int feature_module(void *ctx, uint64_t token, unsigned index, unsigned segment,
                          uint32_t *base, uint32_t *size)
{
    assert(ctx == &feature_base && token == session.token && index == 7 && segment == 1);
    ++feature_calls;
    *base = feature_base;
    *size = feature_size;
    if (feature_revoke)
        foreground = false;
    return feature_result;
}
static int feature_pad(void *ctx, uint64_t token, uint32_t *device, uint32_t *buttons)
{
    assert(ctx == &feature_size && token == session.token);
    ++feature_calls;
    *device = 1;
    *buttons = 0x300;
    if (feature_revoke)
        foreground = false;
    return feature_result;
}
static void pcsb00031(const char *fixture)
{
    FILE *input = fopen(fixture, "rb");
    assert(input);
    char text[8192];
    size_t length = fread(text, 1, sizeof(text), input);
    assert(feof(input) && !ferror(input) && !fclose(input));
    VcoCheatEntry entries[16];
    VcoCode codes[256];
    char names[2048];
    VcoCheatFile file = {entries, 16, 0, codes, 256, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    assert(vco_cheat_parse(text, length, &file, &diagnostic) == VCO_OK);
    assert(file.entry_count == 15 && file.code_count == 128);
    pcsb_mode = true;
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    assert(!vco_vita_execution_set_features(
        &executor, (VcoVitaExecutionFeatures){.pad_context = &feature_size, .pad = feature_pad}));
    VcoCheatOp ops[128];
    VcoPatch receipts[128];
    static VcoEngine engine;
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, vco_vita_execution_callbacks(&executor),
                            ops, receipts, 128));
    assert(!reads && !writes); /* Neither file metadata nor binding executes. */
    uint64_t request = 0, now = 0;
    const unsigned links[] = {0, 1, 2, 3};
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t saved = pcsb_cells[links[i]].value;
        pcsb_cells[links[i]].value = 0;
        pcsb_cells[4].value = 9;
        int before = writes;
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){++request, session.token, 1, 0, true}));
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        assert(engine.rows[0].state == VCO_ENTRY_ON && engine.rows[0].desired &&
               engine.rows[0].diagnostic.code == VCO_OK && writes == before &&
               pcsb_cells[4].value == 9 && !locks && references == 1);
        pcsb_cells[links[i]].value = saved;
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        assert(engine.rows[0].state == VCO_ENTRY_ON && pcsb_cells[4].value == 3);
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){++request, session.token, 1, 0, false}));
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        assert(engine.rows[0].state == VCO_ENTRY_OFF);
    }
    for (unsigned entry = 0; entry < file.entry_count; ++entry) {
        pcsb_cells[4].value = pcsb_cells[5].value = pcsb_cells[10].value = pcsb_cells[16].value = 9;
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){++request, session.token, 1, entry, true}));
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        if (engine.rows[entry].state != VCO_ENTRY_ON)
            fprintf(stderr, "PCSB00031 entry %u: state %u, error %u, line %u\n", entry,
                    engine.rows[entry].state, engine.rows[entry].diagnostic.code,
                    engine.rows[entry].diagnostic.line);
        assert(engine.rows[entry].state == VCO_ENTRY_ON &&
               engine.rows[entry].diagnostic.code == VCO_OK);
        if (entry == 14)
            assert(pcsb_cells[16].value == 0x12345678);
        else if (entry == 2 || entry % 3 == 2)
            assert(pcsb_cells[10].value == 0x3F800000);
        else if (entry % 3 == 1)
            assert(pcsb_cells[5].value == 0);
        else
            assert(pcsb_cells[4].value == 3);
        assert(!vco_engine_submit(
            &engine, (VcoExecutionCommand){++request, session.token, 1, entry, false}));
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        int before = writes;
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        assert(engine.rows[entry].state == VCO_ENTRY_OFF && writes == before);
    }
    mapped = false;
    assert(
        !vco_engine_submit(&engine, (VcoExecutionCommand){++request, session.token, 1, 0, true}));
    vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR && engine.rows[0].diagnostic.code == VCO_E_IO);
    mapped = true;
    assert(!vco_engine_unbind(&engine) && !vco_vita_execution_dispose(&executor));
    finish();
}
/* New device file: two data entries plus eleven B-relative A patch groups.
 * Exercise the real parser/compiler/engine/native backend. Only SDK memory and
 * module metadata are modeled; no fake interpreter executes the instructions. */
static int pcsb_patch_module(void *context, uint64_t token, unsigned index, unsigned segment,
                             uint32_t *base, uint32_t *size)
{
    assert(context == pcsb_instructions && token == session.token && !index && !segment);
    *base = BASE;
    *size = 0x00480000;
    return 0;
}
static int pcsb_patch_guard(void *context, uint64_t token, uint32_t address, uint32_t size,
                            uint64_t *generation)
{
    assert(context == pcsb_instructions && token == session.token &&
           pcsb_instruction(address, size));
    assert(!*generation || *generation == 55);
    *generation = 55;
    return 0;
}
static void pcsb00031_patches(const char *fixture)
{
    FILE *input = fopen(fixture, "rb");
    assert(input);
    char text[16384], names[4096];
    size_t length = fread(text, 1, sizeof(text), input);
    assert(feof(input) && !ferror(input) && !fclose(input));
    VcoCheatEntry entries[16];
    VcoCode codes[512];
    VcoCheatFile file = {entries, 16, 0, codes, 512, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    assert(vco_cheat_parse(text, length, &file, &diagnostic) == VCO_OK);
    assert(file.entry_count == 13 && file.code_count == 425);
    pcsb_mode = pcsb_patch_mode = true;
    for (size_t i = 0; i < file.code_count; ++i) {
        if ((codes[i].code & 0xF000) != 0xA000)
            continue;
        assert(pcsb_instruction_count < VCO_ENGINE_CLAIMS);
        unsigned index = pcsb_instruction_count++;
        pcsb_instructions[index].address = BASE + codes[i].a;
        memset(pcsb_instructions[index].bytes, 0x5a, 4);
    }
    assert(pcsb_instruction_count == 404 && !bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    assert(!vco_vita_execution_set_features(
        &executor, (VcoVitaExecutionFeatures){.module_context = pcsb_instructions,
                                              .module = pcsb_patch_module,
                                              .module_guard = pcsb_patch_guard}));
    VcoCheatOp ops[512];
    VcoPatch receipts[512];
    static VcoEngine engine;
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, vco_vita_execution_callbacks(&executor),
                            ops, receipts, 512));
    for (unsigned i = 0; i < file.entry_count; ++i)
        assert(engine.rows[i].state == VCO_ENTRY_OFF && engine.rows[i].diagnostic.code == VCO_OK &&
               !engine.rows[i].desired);
    assert(!reads && !writes && !injected);
    uint64_t request = 0, now = 0;
    for (unsigned entry = 0; entry < file.entry_count; ++entry) {
        const VcoCheatEntry *source = &file.entries[entry];
        unsigned before = injected, patch_count = 0;
        for (size_t i = source->first; i < source->first + source->count; ++i)
            patch_count += (codes[i].code & 0xF000) == 0xA000;
        if (entry == 12)
            assert(patch_count == 117);
        pcsb_cells[4].value = pcsb_cells[5].value = 9;
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){++request, session.token, 1, entry, true}));
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        if (engine.rows[entry].state != VCO_ENTRY_ON)
            fprintf(stderr,
                    "PCSB00031 patches entry %u (%s): state %u, error %u, line %u, "
                    "injected %u of %u\n",
                    entry, names + source->name, engine.rows[entry].state,
                    engine.rows[entry].diagnostic.code, engine.rows[entry].diagnostic.line,
                    injected - before, patch_count);
        assert(engine.rows[entry].state == VCO_ENTRY_ON &&
               engine.rows[entry].diagnostic.code == VCO_OK && injected - before == patch_count &&
               !locks && !executor.blocked);
        for (size_t i = source->first; i < source->first + source->count; ++i) {
            const VcoCode *code = &codes[i];
            if ((code->code & 0xF000) != 0xA000)
                continue;
            unsigned width = code->code == 0xA100 ? 2 : 4;
            const uint8_t *actual = pcsb_instruction(BASE + code->a, width);
            assert(actual);
            for (unsigned byte = 0; byte < width; ++byte)
                assert(actual[byte] == (uint8_t)(code->b >> (8 * byte)));
        }
        if (entry == 2)
            assert(pcsb_cells[4].value == 3 && writes > 0);
        else if (entry == 3)
            assert(!pcsb_cells[5].value);
        unsigned applied = injected;
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        assert(engine.rows[entry].state == VCO_ENTRY_ON && injected == applied);
        assert(!vco_engine_submit(
            &engine, (VcoExecutionCommand){++request, session.token, 1, entry, false}));
        vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
        assert(engine.rows[entry].state == VCO_ENTRY_OFF && restored == injected && !locks);
        for (unsigned i = 0; i < pcsb_instruction_count; ++i)
            for (unsigned byte = 0; byte < 4; ++byte)
                assert(pcsb_instructions[i].bytes[byte] == 0x5a);
        assert(!block_references[0] && !block_references[1] && !block_references[2]);
    }
    assert(injected == 404 && restored == 404);
    /* The native pool is shared by the whole list. Every non-overlapping group
     * must coexist, rather than merely succeeding when tested in isolation. */
    for (unsigned entry = 0; entry < file.entry_count; ++entry)
        assert(!vco_engine_submit(&engine,
                                  (VcoExecutionCommand){++request, session.token, 1, entry, true}));
    vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
    for (unsigned entry = 0; entry < file.entry_count; ++entry)
        assert(engine.rows[entry].state == VCO_ENTRY_ON &&
               engine.rows[entry].diagnostic.code == VCO_OK);
    assert(injected == 808 && restored == 404 && block_references[2] == 404);
    vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
    assert(injected == 808);
    for (unsigned entry = 0; entry < file.entry_count; ++entry)
        assert(!vco_engine_submit(
            &engine, (VcoExecutionCommand){++request, session.token, 1, entry, false}));
    vco_engine_step(&engine, now += VCO_ENGINE_PERIOD_US);
    for (unsigned entry = 0; entry < file.entry_count; ++entry)
        assert(engine.rows[entry].state == VCO_ENTRY_OFF);
    assert(restored == 808 && !block_references[2] && !vco_engine_unbind(&engine));
    for (unsigned i = 0; i < pcsb_instruction_count; ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            assert(pcsb_instructions[i].bytes[byte] == 0x5a);
    assert(!vco_vita_execution_dispose(&executor));
    finish();
}
/* Preserve the downloaded file and its physical lines, including reported L978.
 * Only the experimental entry is enabled; the SDK memory is a byte model, not
 * proof that these injected game instructions are correct on real hardware. */
static void pcsb00031_trajectory(const char *fixture)
{
    FILE *input = fopen(fixture, "rb");
    assert(input);
    char text[32768], names[8192];
    size_t length = fread(text, 1, sizeof(text), input);
    assert(feof(input) && !ferror(input) && !fclose(input));
    VcoCheatEntry entries[32];
    VcoCode codes[1024];
    VcoCheatFile file = {entries, 32, 0, codes, 1024, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, length, &file, &diagnostic));
    assert(file.entry_count == 31 && file.code_count == 982);
    const VcoCheatEntry *source = &entries[30];
    assert(source->count == 199 && strstr(names + source->name, "Trajectory + landing"));
    file.entries = entries + 30;
    file.entry_count = 1;
    pcsb_mode = pcsb_patch_mode = true;
    for (size_t i = source->first + 1; i < source->first + source->count; ++i) {
        assert(codes[i].code == 0xA200);
        unsigned index = pcsb_instruction_count++;
        pcsb_instructions[index].address = BASE + codes[i].a;
        memset(pcsb_instructions[index].bytes, 0x5a, 4);
    }
    assert(pcsb_instruction_count == 198 && !bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    assert(!vco_vita_execution_set_features(
        &executor, (VcoVitaExecutionFeatures){.module_context = pcsb_instructions,
                                              .module = pcsb_patch_module,
                                              .module_guard = pcsb_patch_guard}));
    VcoCheatOp ops[199];
    VcoPatch receipts[199];
    static VcoEngine engine;
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, vco_vita_execution_callbacks(&executor),
                            ops, receipts, 199));
    assert(engine.rows[0].state == VCO_ENTRY_OFF && !injected);
    uint32_t actual_budget = engine.slots[0].runtime.budget;
    engine.slots[0].runtime.budget = 256;
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
    vco_engine_step(&engine, 0);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR &&
           engine.rows[0].diagnostic.code == VCO_E_BUDGET &&
           engine.rows[0].diagnostic.line == 978 && injected == 127 && restored == 127 &&
           !executor.blocked && !block_references[2]);
    engine.slots[0].runtime.budget = actual_budget;
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 0, true}));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
    if (engine.rows[0].state != VCO_ENTRY_ON)
        fprintf(stderr, "Trajectory: error %u at L%u, budget %u, injected %u/198\n",
                engine.rows[0].diagnostic.code, engine.rows[0].diagnostic.line, actual_budget,
                injected - 127);
    assert(engine.rows[0].state == VCO_ENTRY_ON && injected == 325 && restored == 127);
    for (size_t i = source->first + 1; i < source->first + source->count; ++i) {
        const VcoCode *code = &codes[i];
        const uint8_t *actual = pcsb_instruction(BASE + code->a, 4);
        assert(actual);
        for (unsigned byte = 0; byte < 4; ++byte)
            assert(actual[byte] == (uint8_t)(code->b >> (8 * byte)));
    }
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 2);
    assert(engine.rows[0].state == VCO_ENTRY_ON && injected == 325);
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){3, session.token, 1, 0, false}));
    vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 3);
    assert(engine.rows[0].state == VCO_ENTRY_OFF && restored == injected && !locks &&
           !block_references[2]);
    for (unsigned i = 0; i < pcsb_instruction_count; ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            assert(pcsb_instructions[i].bytes[byte] == 0x5a);
    assert(!vco_engine_unbind(&engine) && !vco_vita_execution_dispose(&executor));
    finish();
}
static void patch_capacity_test(void)
{
    pcsb_mode = pcsb_patch_mode = true;
    pcsb_instruction_count = VCO_ENGINE_CLAIMS + 1;
    for (unsigned i = 0; i < pcsb_instruction_count; ++i) {
        pcsb_instructions[i].address = 0x82000000u + 4 * i;
        memset(pcsb_instructions[i].bytes, 0x5a, 4);
    }
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    int32_t keys[VCO_ENGINE_CLAIMS];
    const uint8_t value[4] = {1, 2, 3, 4};
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i) {
        int result = memory.inject(memory.context, session.token, pcsb_instructions[i].address,
                                   value, 4, &keys[i]);
        if (result)
            fprintf(stderr, "Native patch capacity stopped after %u receipts\n", i);
        assert(!result);
    }
    int32_t refused = -99;
    unsigned acquired = guid_ops, native_calls = injected;
    int copied = reads;
    assert(memory.inject(memory.context, session.token,
                         pcsb_instructions[VCO_ENGINE_CLAIMS].address, value, 4, &refused) < 0);
    assert(refused == -99 && injected == native_calls && guid_ops == acquired && reads == copied &&
           !locks && !executor.blocked);
    assert(!memory.release(memory.context, session.token, keys[0]));
    assert(!memory.inject(memory.context, session.token,
                          pcsb_instructions[VCO_ENGINE_CLAIMS].address, value, 4, &refused));
    assert(refused > keys[VCO_ENGINE_CLAIMS - 1] && executor.patches[0].key == refused);
    assert(!vco_vita_execution_dispose(&executor));
    assert(injected == VCO_ENGINE_CLAIMS + 1 && restored == injected);
    for (unsigned i = 0; i < pcsb_instruction_count; ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            assert(pcsb_instructions[i].bytes[byte] == 0x5a);
    finish();
}
static void features(void)
{
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    assert(!memory.module && !memory.pad);
    VcoVitaExecutionFeatures providers = {.module_context = &feature_base,
                                          .module = feature_module,
                                          .pad_context = &feature_size,
                                          .pad = feature_pad};
    assert(vco_vita_execution_set_features(&executor, providers) < 0);
    /* This test never handed the previous callbacks to an engine and owns no
     * patch. Initialize a separate backend for the configured provider path. */
    static VcoVitaExecution configured;
    vco_vita_execution_init(&configured, &native, &session, &support);
    assert(!vco_vita_execution_set_features(&configured, providers));
    memory = vco_vita_execution_callbacks(&configured);
    assert(memory.module && memory.pad &&
           vco_vita_execution_set_features(&configured, providers) < 0);
    feature_base = BASE;
    feature_size = 32;
    uint32_t a = 99, b = 88;
    assert(!memory.module(memory.context, session.token, 7, 1, &a, &b) && a == BASE && b == 32);
    assert(!memory.pad(memory.context, session.token, &a, &b) && a == 1 && b == 0x300);
    for (unsigned which = 0; which < 2; ++which) {
        a = 99;
        b = 88;
        feature_result = -1;
        assert((which ? memory.pad(memory.context, session.token, &a, &b)
                      : memory.module(memory.context, session.token, 7, 1, &a, &b)) < 0);
        assert(a == 99 && b == 88);
        feature_result = 0;
        feature_revoke = true;
        assert((which ? memory.pad(memory.context, session.token, &a, &b)
                      : memory.module(memory.context, session.token, 7, 1, &a, &b)) < 0);
        assert(a == 99 && b == 88);
        foreground = true;
        feature_revoke = false;
    }
    feature_result = VCO_PAD_SUPPRESSED;
    assert(memory.pad(memory.context, session.token, &a, &b) == VCO_PAD_SUPPRESSED && a == 99 &&
           b == 88);
    feature_revoke = true;
    assert(memory.pad(memory.context, session.token, &a, &b) < 0 && a == 99 && b == 88);
    foreground = true;
    feature_revoke = false;
    feature_result = 2;
    assert(memory.pad(memory.context, session.token, &a, &b) < 0 && a == 99 && b == 88);
    feature_result = 0;
    feature_base = 0x1000;
    assert(memory.module(memory.context, session.token, 7, 1, &a, &b) < 0 && a == 99 && b == 88);
    feature_base = UINT32_MAX;
    assert(memory.module(memory.context, session.token, 7, 1, &a, &b) < 0 && a == 99 && b == 88);
    unsigned called = feature_calls;
    assert(memory.module(memory.context, session.token, 7, 1, &a, &a) < 0);
    assert(memory.pad(memory.context, session.token, &a, &a) < 0);
    assert(memory.pad(memory.context, session.token + 1, &a, &b) < 0);
    configured.blocked = true;
    assert(memory.module(memory.context, session.token, 7, 1, &a, &b) < 0);
    assert(feature_calls == called);
    assert(!vco_vita_execution_dispose(&executor) && !vco_vita_execution_dispose(&configured));
    finish();
}
static void patch_spans(void)
{
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    memtype = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RX;
    uint8_t value[4] = {1, 2, 3, 4};
    int32_t handle = -99;
    split_block_at = BASE + 8;
    for (unsigned width = 2; width <= 4; width *= 2) {
        assert(memory.inject(memory.context, session.token, split_block_at - width + 1, value,
                             width, &handle) < 0);
        assert(handle == -99 && !reads && !injected && !locks);
    }
    split_block_at = 0;
    fail_find_at = BASE + 4;
    assert(memory.inject(memory.context, session.token, BASE + 1, value, 4, &handle) < 0);
    assert(handle == -99 && !injected && !locks);
    fail_find_at = 0;
    assert(!memory.inject(memory.context, session.token, BASE + 1, value, 4, &handle));
    assert(injected == 1 && !memcmp(bytes + 1, value, 4));
    assert(!memory.release(memory.context, session.token, handle));
    assert(restored == 1 && bytes[1] == 0x5a && !nonzero_find_calls);
    assert(!vco_vita_execution_dispose(&executor));
    finish();
}
static void patch_test(void)
{
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    uint8_t value[4] = {1, 2, 3, 4};
    int32_t handle = -99, other = -99;
    memtype = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RX;
    assert(memory.write(memory.context, session.token, BASE, value, 4) < 0);
    assert(!memory.inject(memory.context, session.token, BASE, value, 4, &handle));
    assert(handle > 0 && !memcmp(bytes, value, 4));
    assert(memory.inject(memory.context, session.token, BASE + 1, value, 1, &other) < 0);
    assert(other == -99 && injected == 1);
    assert(memory.release(memory.context, session.token + 1, handle) < 0 && !restored);
    foreground = false;
    assert(vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &session.identity));
    assert(!memory.release(memory.context, session.token, handle));
    assert(bytes[0] == 0x5a && restored == 1);
    assert(memory.release(memory.context, session.token, handle) < 0 && restored == 1);
    assert(!vco_vita_execution_dispose(&executor));
    finish();

    reset();
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    memory = vco_vita_execution_callbacks(&executor);
    fail_inject = true;
    handle = -99;
    assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0);
    assert(!injected && !locks && handle == -99);
    fail_inject = false;
    revoke_after_inject = true;
    assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0);
    assert(handle == -99 && injected == 1 && restored == 1 && !executor.blocked);
    assert(!vco_vita_execution_dispose(&executor) && restored == 1 && bytes[0] == 0x5a);
    finish();

    for (unsigned stale = 0; stale < 2; ++stale) {
        reset();
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        memory = vco_vita_execution_callbacks(&executor);
        assert(!memory.inject(memory.context, session.token, BASE, value, 4, &handle));
        if (stale)
            current = &replacement;
        else
            fail_restore = true;
        assert(memory.release(memory.context, session.token, handle) < 0 && executor.blocked);
        assert(restored == (stale ? 0u : 1u));
        assert(memory.release(memory.context, session.token, handle) < 0);
        assert(vco_vita_execution_dispose(&executor) < 0);
        assert(memory.write(memory.context, session.token, BASE + 8, value, 4) < 0);
        assert(restored == (stale ? 0u : 1u));
        /* Simulate shutdown of the test kernel after quarantined ownership.
         * Production must retain this context and its process reference. */
        memset(patches, 0, sizeof(patches));
        memset(block_references, 0, sizeof(block_references));
        current = &object;
        finish();
    }
}
static void patch_probe_test(void)
{
    unsigned before = guid_ops;
    assert(!vco_vita_patch_support_probe(&support) && guid_ops == before);
    for (unsigned fail = 1; fail <= 6; ++fail) {
        VcoVitaPatchSupport trial;
        guid_ops = 0;
        guid_fault = fail;
        vco_vita_patch_support_init(&trial);
        assert(vco_vita_patch_support_probe(&trial) < 0 && !trial.ready);
        assert(!probe_refs);
        bool ambiguous = fail == 3 || fail == 6;
        assert((vco_vita_patch_support_dispose(&trial) < 0) == ambiguous);
        unsigned attempts = guid_ops;
        assert(vco_vita_patch_support_probe(&trial) < 0 && attempts == guid_ops);
        /* Fake a reboot for ambiguous native cleanup, not a product recovery. */
        probe_open = false;
        probe_refs = 0;
    }
    guid_fault = 0;
    for (unsigned variant = 0; variant < 5; ++variant) {
        VcoVitaPatchSupport trial;
        guid_ops = 0;
        closed_referenceable = variant == 0;
        null_guid_object = variant == 1;
        changing_closed_error = variant == 2;
        closed_error = variant == 3   ? -123
                       : variant == 4 ? (int)SCE_KERNEL_ERROR_UNKNOWN_UID
                                      : (int)SCE_KERNEL_ERROR_INVALID_UID;
        vco_vita_patch_support_init(&trial);
        assert((vco_vita_patch_support_probe(&trial) == 0) == (variant == 4));
        assert(!probe_refs && !probe_open && !vco_vita_patch_support_dispose(&trial));
        if (variant == 4)
            assert(trial.closed_error == (int)SCE_KERNEL_ERROR_UNKNOWN_UID);
    }
    closed_referenceable = null_guid_object = changing_closed_error = false;
    closed_error = (int)SCE_KERNEL_ERROR_INVALID_UID;
    assert(!bind());
    VcoVitaPatchSupport unavailable;
    vco_vita_patch_support_init(&unavailable);
    vco_vita_execution_init(&executor, &native, &session, &unavailable);
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    assert(!memory.inject && !memory.release && memory.write && memory.read);
    uint8_t data[1] = {0xAB};
    assert(!memory.write(memory.context, session.token, BASE, data, 1));
    assert(!vco_vita_execution_dispose(&executor));
    finish();
}
static void auto_cleanup(unsigned index)
{
    assert(index < injected && patches[index].active && patches[index].uid_open);
    /* Models tai_try_cleanup_process: remove/free saved state/Close UID.
     * It does NOT restore old process bytes or drop VCO's retained reference. */
    patches[index].active = patches[index].uid_open = false;
}
static VcoTraceRing patch_trace_ring;
static void capture_patch_trace(void *context, uint32_t event, int32_t result, uint32_t subject,
                                uint32_t detail)
{
    assert(context == &patch_trace_ring);
    assert(vco_trace_append(context, 0, event, result, subject, detail) == VCO_TRACE_OK);
}
static unsigned trace_find(uint32_t event, int32_t result)
{
    uint64_t cursor = 0;
    unsigned found = 0;
    VcoTraceBatch batch;
    do {
        assert(!vco_trace_snapshot(&patch_trace_ring, cursor, &batch));
        for (unsigned i = 0; i < batch.count; ++i) {
            found += batch.records[i].event == event && batch.records[i].result == result;
            cursor = batch.records[i].sequence;
        }
    } while (batch.count);
    return found;
}
static void patch_failure_stage_test(void)
{
    const uint32_t stages[] = {VCO_PATCH_STAGE_TAI_INJECT, VCO_PATCH_STAGE_READ,
                               VCO_PATCH_STAGE_MODULE_CAPTURE, VCO_PATCH_STAGE_READ,
                               VCO_PATCH_STAGE_LOCK};
    const int32_t raw[] = {(int32_t)0x8002d019, -1, -1, VCO_ACCESS_SESSION, VCO_ACCESS_SESSION};
    for (unsigned variant = 0; variant < 6; ++variant) {
        reset();
        vco_trace_init(&patch_trace_ring);
        assert(!bind_traced(capture_patch_trace, &patch_trace_ring));
        vco_vita_patch_support_init(&support);
        assert(!vco_vita_patch_support_set_trace(&support, capture_patch_trace, &patch_trace_ring));
        assert(!vco_vita_patch_support_probe(&support));
        vco_vita_execution_init(&executor, &native, &session, &support);
        assert(!vco_vita_execution_set_features(
            &executor, (VcoVitaExecutionFeatures){.module_context = &feature_base,
                                                  .module_guard = feature_module_guard}));
        if (!variant) {
            fail_inject = true;
            fail_inject_result = raw[0];
        } else if (variant == 1) {
            fail_preflight_copy = true;
            preflight_copy_error = -37;
        } else if (variant == 2)
            guard_fail_at = 1;
        else if (variant == 3)
            revoke_after_copy = true;
        else if (variant == 4)
            replace_after_lock = true;
        else
            foreground = false;
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        const uint8_t value[4] = {1, 2, 3, 4};
        int32_t handle = -99;
        int result = memory.inject(memory.context, session.token, BASE, value, 4, &handle);
        assert(result == (variant >= 3 ? VCO_ACCESS_SESSION : -1) && handle == -99 && !injected &&
               !restored && !locks && !block_references[0]);
        VcoTraceBatch batch;
        uint64_t cursor = 0;
        unsigned failures = 0;
        do {
            assert(!vco_trace_snapshot(&patch_trace_ring, cursor, &batch));
            for (unsigned i = 0; i < batch.count; ++i) {
                const VcoTraceRecord *r = &batch.records[i];
                if (r->event == VCO_PATCH_TRACE_INJECT_FAILURE) {
                    if (variant >= 5 || r->result != raw[variant] ||
                        r->detail != (stages[variant] << 8 | 4))
                        fprintf(stderr, "failure stage variant %u: stage %u raw %08x\n", variant,
                                r->detail >> 8, (unsigned)r->result);
                    assert(variant < 5 && r->result == raw[variant] && r->subject == BASE &&
                           r->detail == (stages[variant] << 8 | 4));
                    ++failures;
                }
                cursor = r->sequence;
            }
        } while (batch.count);
        assert(failures == (variant < 3 ? 1u : 0u));
        if (variant == 1)
            assert(trace_find(VCO_MEMORY_TRACE_COPY_READ_FAILURE, -37) == 1);
        assert(!vco_vita_execution_dispose(&executor));
        finish();
    }
    fail_inject_result = -1;
}
static void patch_preacquisition_authority_test(void)
{
    for (unsigned variant = 0; variant < 5; ++variant) {
        reset();
        assert(!bind());
        vco_trace_init(&patch_trace_ring);
        vco_vita_patch_support_init(&support);
        assert(!vco_vita_patch_support_set_trace(&support, capture_patch_trace, &patch_trace_ring));
        assert(!vco_vita_patch_support_probe(&support));
        vco_vita_execution_init(&executor, &native, &session, &support);
        assert(!vco_vita_execution_set_features(
            &executor, (VcoVitaExecutionFeatures){.module_context = &feature_base,
                                                  .module_guard = feature_module_guard}));
        VcoAdmission gate = VCO_ADMISSION_INITIALIZER;
        assert(!vco_vita_execution_set_admission(&executor, &gate));
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        /* Capture, held block, and locked read: all precede taiHEN. Failed
         * cleanup must not convert either reference/unlock ambiguity to pause. */
        guard_revoke_at = variant == 0 ? 1 : variant == 1 || variant == 3 ? 2 : 0;
        revoke_after_copy = variant == 2 || variant == 4;
        fail_block_reference_drop = variant == 3;
        fail_unlock = variant == 4;
        uint8_t value[4] = {1, 2, 3, 4};
        int32_t handle = -99;
        int result = memory.inject(memory.context, session.token, BASE, value, 4, &handle);
        assert(result == (variant < 3 ? VCO_ACCESS_SESSION : -1));
        assert(handle == -99 && !locks && !injected && !restored && !block_references[0] &&
               __atomic_load_n(&gate.state, __ATOMIC_ACQUIRE) == 0);
        for (unsigned i = 0; i < 4; ++i)
            assert(bytes[i] == 0x5a);
        if (variant < 3) {
            assert(!executor.blocked && !executor.poisoned && !native.poisoned);
            assert(!trace_find(VCO_PATCH_TRACE_INJECT_FAILURE, VCO_ACCESS_SESSION));
            foreground = true;
            guard_revoke_at = 0;
            revoke_after_copy = false;
            assert(!memory.inject(memory.context, session.token, BASE, value, 4, &handle));
            assert(handle > 0 && injected == 1 && !locks);
            assert(!memory.release(memory.context, session.token, handle) && restored == 1);
            assert(!vco_vita_execution_dispose(&executor));
        } else {
            assert(executor.blocked && vco_vita_execution_error(&executor) == VCO_E_RESTORE);
            assert(trace_find(VCO_PATCH_TRACE_INJECT_FAILURE, VCO_ACCESS_SESSION) == 1);
        }
        vco_admission_close(&gate);
        assert(vco_admission_drained(&gate));
        finish();
    }
}
static void patch_trace_test(void)
{
    unsigned baseline_guid = 0, baseline_ops = 0;
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset();
        vco_trace_init(&patch_trace_ring);
        vco_vita_patch_support_init(&support);
        assert(vco_vita_patch_support_set_trace(NULL, capture_patch_trace, NULL) < 0);
        assert(!vco_vita_patch_support_set_trace(&support, NULL, &patch_trace_ring));
        assert(!support.trace && !support.trace_context);
        if (mode)
            assert(!vco_vita_patch_support_set_trace(&support, capture_patch_trace,
                                                     &patch_trace_ring));
        guid_ops = 0;
        assert(!vco_vita_patch_support_probe(&support));
        assert(vco_vita_patch_support_set_trace(&support, NULL, NULL) < 0);
        unsigned count = guid_ops;
        assert(!vco_vita_patch_support_probe(&support) && guid_ops == count);
        if (mode) {
            assert(trace_find(VCO_PATCH_TRACE_PROBE_RESULT, 0) == 1);
            assert(trace_find(VCO_PATCH_TRACE_PROBE_CLOSED_QUERY, closed_error) == 2);
        }
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        VcoIdentity retired = session.identity;
        uint8_t data[4] = {1, 2, 3, 4};
        int32_t key;
        assert(!memory.inject(memory.context, session.token, BASE, data, 4, &key));
        if (mode < 2) {
            assert(!memory.release(memory.context, session.token, key));
            assert(restored == 1 && bytes[0] == 0x5a);
            if (!mode) {
                baseline_guid = guid_ops;
                baseline_ops = operations;
            } else {
                assert(guid_ops == baseline_guid && operations == baseline_ops);
                assert(trace_find(VCO_PATCH_TRACE_NATIVE_RELEASE, 0) == 1);
            }
            count = guid_ops;
            assert(memory.release(memory.context, session.token, key) < 0 && guid_ops == count);
        } else if (mode == 2) {
            fail_restore = true;
            assert(memory.release(memory.context, session.token, key) < 0 && restored == 1);
            assert(trace_find(VCO_PATCH_TRACE_NATIVE_RELEASE, -1) == 1);
            assert(trace_find(VCO_PATCH_TRACE_QUARANTINE, -1) == 1);
            count = guid_ops;
            assert(memory.release(memory.context, session.token, key) < 0 && guid_ops == count);
            authorized = false;
            assert(!vco_vita_execution_reap_retired(&executor, &retired));
            assert(trace_find(VCO_PATCH_TRACE_NATIVE_CLOSED, closed_error) >= 1);
        } else {
            auto_cleanup(0);
            guid_fault = guid_ops + 1;
            assert(vco_vita_execution_reap_retired(&executor, &retired) < 0 && !restored);
            assert(trace_find(VCO_PATCH_TRACE_NATIVE_QUERY_FAILURE, -1) == 1);
            assert(!trace_find(VCO_PATCH_TRACE_NATIVE_CLOSED, -1));
            assert(trace_find(VCO_PATCH_TRACE_POISON, -1) == 1);
            count = guid_ops;
            assert(vco_vita_execution_reap_retired(&executor, &retired) < 0 && guid_ops == count);
            /* Reboot model for permanently ambiguous native ownership. */
            memset(patches, 0, sizeof(patches));
            memset(block_references, 0, sizeof(block_references));
        }
        if (mode != 3)
            assert(!vco_vita_execution_dispose(&executor));
        finish();
    }
}
static void patch_lifecycle_test(void)
{
    uint8_t data[4] = {1, 2, 3, 4};
    int32_t first, second;
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    VcoIdentity retired = session.identity;
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    uint64_t token = session.token;
    assert(!memory.inject(memory.context, token, BASE, data, 4, &first));
    assert(!memory.inject(memory.context, token, BASE + 8, data, 4, &second));
    assert(patches[0].references == 1 && patches[1].references == 1);
    VcoIdentity wrong = retired;
    ++wrong.instance;
    unsigned calls = guid_ops;
    assert(vco_vita_execution_reap_retired(&executor, &wrong) < 0 && !executor.retired &&
           guid_ops == calls);
    authorized = foreground = false;
    assert(vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL));
    assert(memory.release(memory.context, token, first) < 0 && executor.blocked && !restored);
    assert(vco_vita_execution_reap_retired(&executor, &retired) == 1 && !restored);
    assert(patches[0].references == 1 && patches[1].references == 1);
    auto_cleanup(0);
    memset(bytes, 0x99, sizeof(bytes)); /* New process contents must not be touched. */
    assert(vco_vita_execution_reap_retired(&executor, &retired) == 1 && !patches[0].references &&
           patches[1].references == 1 && !restored);
    auto_cleanup(1);
    assert(!vco_vita_execution_reap_retired(&executor, &retired) && !patches[1].references);
    calls = guid_ops;
    assert(!vco_vita_execution_reap_retired(&executor, &retired) && guid_ops == calls);
    assert(!vco_vita_execution_dispose(&executor) && !restored && bytes[0] == 0x99 &&
           bytes[8] == 0x99);
    assert(memory.write(memory.context, token, BASE + 16, data, 4) < 0);
    finish();
    reset();
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    retired = session.identity;
    memory = vco_vita_execution_callbacks(&executor);
    assert(!memory.inject(memory.context, session.token, BASE, data, 4, &first));
    fail_restore = true;
    assert(memory.release(memory.context, session.token, first) < 0 && restored == 1 &&
           patches[0].references == 1);
    /* The failed restore consumed the native UID. Only retirement permits the
     * service to discard this old process's RESTORE latch afterward. */
    assert(vco_vita_execution_dispose(&executor) < 0);
    authorized = false;
    memset(bytes, 0x88, sizeof(bytes));
    assert(!vco_vita_execution_reap_retired(&executor, &retired) && !patches[0].references &&
           bytes[0] == 0x88 && restored == 1);
    finish();
}
static void patch_faults_test(void)
{
    uint8_t data[4] = {1, 2, 3, 4};
    for (unsigned variant = 0; variant < 6; ++variant) {
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        VcoIdentity retired = session.identity;
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        int32_t handle = -99;
        if (variant == 0)
            guid_fault = guid_ops + 4; /* Cannot retain the original injection UID. */
        assert((memory.inject(memory.context, session.token, BASE, data, 4, &handle) < 0) ==
               (variant == 0));
        if (variant == 0) {
            assert(handle == -99 && executor.poisoned && !patches[0].references);
        } else if (variant == 1) {
            patches[0].current = &replacement; /* Same integer, different object. */
            assert(vco_vita_execution_reap_retired(&executor, &retired) < 0);
            assert(patches[0].references == 1 && !restored);
        } else if (variant == 2) {
            guid_fault = guid_ops + 2; /* Temporary validation reference release. */
            assert(memory.release(memory.context, session.token, handle) < 0 && !restored);
            assert(patches[0].references == 1);
        } else if (variant == 3) {
            fail_patch_reference_drop = true; /* Original reference release after taiHEN. */
            assert(memory.release(memory.context, session.token, handle) < 0 && restored == 1);
            assert(!patches[0].references);
        } else if (variant == 4) {
            auto_cleanup(0);
            guid_fault = guid_ops + 1; /* Generic failure is not UID absence. */
            assert(vco_vita_execution_reap_retired(&executor, &retired) < 0);
            assert(patches[0].references == 1 && !restored);
        } else {
            auto_cleanup(0);
            guid_fault = guid_ops + 2; /* Closed observation succeeded, drop failed. */
            assert(vco_vita_execution_reap_retired(&executor, &retired) < 0);
            assert(!patches[0].references && !restored);
        }
        assert(executor.poisoned && vco_vita_execution_dispose(&executor) < 0);
        unsigned calls = guid_ops, releases = restored;
        guid_fault = 0;
        assert(vco_vita_execution_reap_retired(&executor, &retired) < 0 && guid_ops == calls &&
               restored == releases);
        /* Irrecoverable native ownership requires a reboot; model that only
         * after asserting no production API retried or discarded ownership. */
        memset(patches, 0, sizeof(patches));
        memset(block_references, 0, sizeof(block_references));
        finish();
        reset();
    }
}
static void patch_mapping_test(void)
{
    uint8_t data[4] = {1, 2, 3, 4};
    for (unsigned variant = 0; variant < 11; ++variant) {
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        VcoIdentity retired = session.identity;
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        int32_t handle = -99;
        assert(!memory.inject(memory.context, session.token, BASE, data, 4, &handle));
        assert(block_references[0] == 1 && patches[0].references == 1 && !locks);
        if (variant == 0) {
            foreground = false;
            assert(vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &retired));
            assert(!memory.release(memory.context, session.token, handle));
            assert(restored == 1 && !block_references[0] && !patches[0].references &&
                   bytes[0] == 0x5a);
        } else if (variant == 1) {
            block_uid = 78;
            memset(bytes, 0xcc, sizeof(bytes));
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(!executor.poisoned && !restored && block_references[0] == 1);
            authorized = false;
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired));
            assert(!block_references[0] && !block_references[1] && bytes[0] == 0xcc);
        } else if (variant == 2) {
            block_current[0] = &block_replacement;
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(executor.poisoned && !restored && block_references[0] == 1);
        } else if (variant == 3) {
            mapped = false; /* Range acquisition fails before any restore. */
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(!restored && !executor.poisoned && block_references[0] == 1);
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired));
        } else if (variant == 4) {
            fail_block_reference_drop = true; /* Original block reference release. */
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(restored == 1 && executor.poisoned && !block_references[0]);
        } else if (variant == 5) {
            auto_cleanup(0);
            block_current[0] = &block_replacement;
            assert(vco_vita_execution_reap_retired(&executor, &retired) < 0);
            assert(executor.poisoned && !restored && block_references[0] == 1);
        } else if (variant == 6) {
            auto_cleanup(0);
            guid_fault = guid_ops + 5; /* Closed patch, original block drop. */
            assert(vco_vita_execution_reap_retired(&executor, &retired) < 0);
            assert(executor.poisoned && !restored && !block_references[0]);
        } else if (variant == 7) {
            guid_fault = guid_ops + 3; /* Temporary block identity query. */
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(!executor.poisoned && !restored && block_references[0] == 1);
            guid_fault = 0;
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired));
        } else if (variant == 8) {
            replace_after_lock = true;
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(!restored && !executor.poisoned && block_references[0] == 1);
            current = &object;
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired));
        } else if (variant == 9) {
            fail_find = true;
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(!restored && !executor.poisoned && block_references[0] == 1);
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired));
        } else {
            fail_unlock = true;
            assert(memory.release(memory.context, session.token, handle) < 0);
            assert(restored == 1 && executor.poisoned && block_references[0] == 1);
        }
        assert(!locks);
        if (executor.poisoned) {
            unsigned calls = guid_ops;
            assert(vco_vita_execution_dispose(&executor) < 0 && calls == guid_ops);
            /* Explicit fake-kernel reboot after proving quarantine is retained. */
            memset(patches, 0, sizeof(patches));
            memset(block_references, 0, sizeof(block_references));
        } else {
            assert(!vco_vita_execution_dispose(&executor));
            assert(!block_references[0] && !block_references[1]);
        }
        guid_fault = 0;
        finish();
        reset();
    }
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    int32_t handle = -99;
    guid_fault = guid_ops + 1; /* No block reference acquired: no injection. */
    assert(memory.inject(memory.context, session.token, BASE, data, 4, &handle) < 0);
    assert(!injected && !block_references[0] && !executor.poisoned && handle == -99);
    guid_fault = 0;
    fail_inject = true;
    assert(memory.inject(memory.context, session.token, BASE, data, 4, &handle) < 0);
    assert(!injected && !block_references[0] && !executor.poisoned && handle == -99);
    assert(!vco_vita_execution_dispose(&executor));
    finish();
}
static void patch_modules_test(void)
{
    for (unsigned variant = 0; variant < 6; ++variant) {
        assert(!bind());
        VcoIdentity retired = session.identity;
        vco_vita_execution_init(&executor, &native, &session, &support);
        feature_base = BASE;
        feature_size = sizeof(bytes);
        feature_result = 0;
        feature_revoke = guard_invalid = guard_wrong_generation = false;
        guard_calls = guard_fail_at = 0;
        VcoVitaExecutionFeatures features = {.module_context = &feature_base,
                                             .module = feature_module,
                                             .module_guard = feature_module_guard};
        assert(!vco_vita_execution_set_features(&executor, features));
        VcoCode codes[] = {{0xb207, 1, 0, 1}, {0xa200, 0, 0x01020304, 2}};
        VcoCheatEntry entry = {0, 0, 2, 0, {0, 0}};
        VcoCheatFile file = {&entry, 1, 1, codes, 2, 2, NULL, 0, 0};
        VcoCheatOp ops[2];
        VcoPatch records[2];
        static VcoEngine engine;
        vco_engine_init(&engine);
        assert(!vco_engine_bind(&engine, &session, 1, &file,
                                vco_vita_execution_callbacks(&executor), ops, records, 2));
        assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
        vco_engine_step(&engine, 1);
        assert(injected == 1 && !restored && executor.patches[0].module_generation == 17);
        if (variant) {
            memset(bytes, 0xcc, sizeof(bytes)); /* same memblock, new module occupant */
            guard_invalid = variant < 4;
            if (variant == 4)
                guard_fail_at = guard_calls + 2; /* Changed after restore acquired range lock. */
            if (variant == 5)
                guard_wrong_generation = true;
        }
        if (variant == 0 || variant == 3) {
            foreground = false;
            vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &retired);
        } else if (variant == 1)
            feature_result = -1; /* Real module provider already invalidated its snapshot. */
        else
            assert(
                !vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 0, false}));
        vco_engine_step(&engine, 50001);
        if (!variant) {
            assert(restored == 1 && bytes[0] == 0x5a && engine.rows[0].state == VCO_ENTRY_PAUSED);
            assert(!vco_engine_unbind(&engine) && !vco_vita_execution_dispose(&executor));
        } else {
            assert(!restored && bytes[0] == 0xcc && engine.blocked_restore && executor.blocked);
            assert(engine.rows[0].diagnostic.code == VCO_E_RESTORE);
            assert(executor.patches[0].quarantined && patches[0].references == 1 &&
                   block_references[0] == 1 && !executor.poisoned);
            unsigned checks = guard_calls;
            assert(vco_vita_execution_dispose(&executor) < 0 && !restored);
            authorized = false;
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired));
            assert(!restored && bytes[0] == 0xcc && guard_calls == checks &&
                   !patches[0].references && !block_references[0]);
            vco_engine_init(&engine); /* Only after proven retired native ownership cleanup. */
            assert(!vco_vita_execution_dispose(&executor));
        }
        finish();
        reset();
    }

    for (unsigned call = 1; call <= 2; ++call) {
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        guard_calls = 0;
        guard_fail_at = call;
        guard_invalid = guard_wrong_generation = false;
        VcoVitaExecutionFeatures features = {.module_context = &feature_base,
                                             .module_guard = feature_module_guard};
        assert(!vco_vita_execution_set_features(&executor, features));
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        const uint8_t value[4] = {1, 2, 3, 4};
        int32_t handle = -99;
        assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0);
        assert(handle == -99 && !injected && !locks && !block_references[0]);
        assert(!vco_vita_execution_dispose(&executor));
        finish();
        reset();
    }
}
/* Exercise unlock itself and the native observations between unlock and the
 * final block check. Guard variants cover both absent and positive receipts. */
static void configure_publication_change(unsigned scenario, unsigned change)
{
    static const unsigned boundaries[] = {BLOCK_BOUNDARY_UNLOCK, BLOCK_BOUNDARY_GUARD,
                                          BLOCK_BOUNDARY_GUARD, BLOCK_BOUNDARY_NATIVE_QUERY,
                                          BLOCK_BOUNDARY_NATIVE_RELEASE};
    assert(scenario < sizeof(boundaries) / sizeof(boundaries[0]));
    publication_block_boundary = boundaries[scenario];
    publication_block_change = change;
    if (scenario == 1 || scenario == 2) {
        VcoVitaExecutionFeatures features = {.module_context = &feature_base,
                                             .module_guard = feature_module_guard};
        assert(!vco_vita_execution_set_features(&executor, features));
        guard_scripted = true;
        for (unsigned i = 0; i < sizeof(guard_script) / sizeof(guard_script[0]); ++i)
            guard_script[i] = scenario == 2 ? 17 : 0;
    }
}
static void finish_publication_change(const VcoIdentity *retired, unsigned change)
{
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    VcoVitaPatch *p = &executor.patches[0];
    bool permanent = change == BLOCK_CHANGE_OBJECT || change == BLOCK_CHANGE_RELEASE;
    assert(publication_block_changed && !fail_temp_block_reference && !fail_temp_block_release &&
           injected == 1 && !restored && !locks && bytes[0] == 0xcc && executor.blocked &&
           executor.poisoned == permanent && p->quarantined && !p->active && p->referenced &&
           p->block_referenced && p->memblock == 77 && p->block_object == &block_objects[0] &&
           patches[0].active && patches[0].uid_open && patches[0].references == 1 &&
           block_references[0] == 1 && !block_references[1] &&
           vco_vita_execution_error(&executor) == VCO_E_RESTORE);
    unsigned calls = guid_ops;
    int ranges = unmaps;
    uint8_t data[4] = {1, 2, 3, 4};
    int32_t key = -99;
    assert(memory.release(memory.context, session.token, p->key) < 0);
    assert(memory.inject(memory.context, session.token, BASE, data, 4, &key) < 0 && key == -99);
    assert(vco_vita_execution_dispose(&executor) < 0 && guid_ops == calls && unmaps == ranges &&
           !restored && bytes[0] == 0xcc);
    if (permanent) {
        assert(vco_vita_execution_reap_retired(&executor, retired) < 0 && guid_ops == calls &&
               patches[0].references == 1 && block_references[0] == 1);
        /* Ambiguous object/release ownership: model reboot only after proving
         * that dispose, retirement and repeated requests make no native calls. */
        memset(patches, 0, sizeof(patches));
        memset(block_references, 0, sizeof(block_references));
    } else {
        authorized = false;
        assert(vco_vita_execution_reap_retired(&executor, retired) == 1 && !restored &&
               patches[0].references == 1 && block_references[0] == 1 && bytes[0] == 0xcc);
        auto_cleanup(0);
        assert(!vco_vita_execution_reap_retired(&executor, retired) && !restored &&
               !patches[0].references && !block_references[0] && bytes[0] == 0xcc &&
               vco_vita_execution_error(&executor) == VCO_OK);
        assert(!vco_vita_execution_dispose(&executor));
    }
    finish();
    reset();
}
static void patch_confirmation_test(void)
{
    for (unsigned variant = 0; variant < 9; ++variant) {
        assert(!bind());
        VcoIdentity retired = session.identity;
        vco_vita_execution_init(&executor, &native, &session, &support);
        feature_base = BASE;
        VcoVitaExecutionFeatures features = {.module_context = &feature_base,
                                             .module_guard = feature_module_guard};
        assert(!vco_vita_execution_set_features(&executor, features));
        guard_scripted = true;
        for (unsigned i = 0; i < 8; ++i)
            guard_script[i] = 17;
        if (variant < 2)
            guard_fail_at = variant + 3; /* After native write, or after range unlock. */
        else if (variant < 4)
            guard_script[variant] = 18;
        else if (variant == 4) {
            guard_script[0] = guard_script[1] = 0; /* Cannot recapture heap as a new module. */
        } else if (variant == 5)
            guard_script[2] = 0; /* Positive module receipt cannot become negative membership. */
        else if (variant == 6)
            guard_script[0] = 0; /* Changed inside lock: reject before writing anything. */
        else
            for (unsigned i = 0; i < 8; ++i)
                guard_script[i] = 0;
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        const uint8_t value[4] = {1, 2, 3, 4};
        int32_t handle = -99;
        int result = memory.inject(memory.context, session.token, BASE, value, 4, &handle);
        if (variant == 6) {
            assert(result < 0 && handle == -99 && !injected && !locks && !block_references[0] &&
                   !executor.blocked && !executor.patches[0].referenced);
            assert(!vco_vita_execution_dispose(&executor));
        } else {
            assert(injected == 1 && patches[0].references == 1 && block_references[0] == 1 &&
                   !locks && !restored);
            if (variant < 6) {
                assert(result < 0 && handle == -99 && !executor.patches[0].active &&
                       executor.patches[0].quarantined && executor.blocked && !executor.poisoned);
                unsigned calls = guid_ops;
                assert(memory.release(memory.context, session.token, executor.patches[0].key) < 0);
                assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0 &&
                       guid_ops == calls);
            } else {
                assert(!result && handle == executor.patches[0].key &&
                       executor.patches[0].module_generation == 0 && guard_calls == 4);
                memset(bytes, 0xcc, sizeof(bytes));
                if (variant == 7)
                    guard_script[4] = 17; /* Negative membership changes before restore lock. */
                else
                    guard_script[5] = 17; /* Changes inside the restore range lock. */
                assert(memory.release(memory.context, session.token, handle) < 0 && !restored &&
                       bytes[0] == 0xcc && executor.patches[0].quarantined);
            }
            assert(vco_vita_execution_error(&executor) == VCO_E_RESTORE);
            unsigned checks = guard_calls;
            assert(vco_vita_execution_dispose(&executor) < 0 && !restored && guard_calls == checks);
            authorized = false;
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired) && !restored &&
                   !patches[0].references && !block_references[0] && guard_calls == checks &&
                   vco_vita_execution_error(&executor) == VCO_OK);
        }
        finish();
        reset();
    }
    for (unsigned mutation = 1; mutation <= 3; ++mutation) {
        assert(!bind());
        VcoIdentity retired = session.identity;
        vco_vita_execution_init(&executor, &native, &session, &support);
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        inject_mutation = mutation;
        const uint8_t value[4] = {1, 2, 3, 4};
        int32_t handle = -99;
        assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0 &&
               handle == -99 && injected == 1 && !restored && bytes[0] == 0xcc &&
               executor.patches[0].quarantined && patches[0].references == 1 &&
               block_references[0] == 1 && !locks);
        assert(vco_vita_execution_dispose(&executor) < 0 && !restored && bytes[0] == 0xcc);
        if (mutation == 2) {
            assert(executor.poisoned && vco_vita_execution_reap_retired(&executor, &retired) < 0);
            /* Object mismatch is irreversible: model reboot after proving no retry/restore. */
            memset(patches, 0, sizeof(patches));
            memset(block_references, 0, sizeof(block_references));
        } else {
            authorized = false;
            auto_cleanup(0);
            assert(!vco_vita_execution_reap_retired(&executor, &retired) && !restored &&
                   bytes[0] == 0xcc && !patches[0].references && !block_references[0]);
        }
        finish();
        reset();
    }
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        for (unsigned change = BLOCK_CHANGE_UID; change <= BLOCK_CHANGE_RELEASE; ++change) {
            assert(!bind());
            VcoIdentity retired = session.identity;
            vco_vita_execution_init(&executor, &native, &session, &support);
            configure_publication_change(scenario, change);
            VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
            const uint8_t data[4] = {1, 2, 3, 4};
            int32_t key = -99;
            assert(memory.inject(memory.context, session.token, BASE, data, 4, &key) < 0 &&
                   key == -99);
            assert(executor.patches[0].module_generation == (scenario == 2 ? 17u : 0u));
            finish_publication_change(&retired, change);
        }
    }
}
static void patch_closed_confirmation_test(void)
{
    for (unsigned call = 3; call <= 4; ++call) {
        assert(!bind());
        VcoIdentity retired = session.identity;
        vco_vita_execution_init(&executor, &native, &session, &support);
        VcoVitaExecutionFeatures features = {.module_context = &feature_base,
                                             .module_guard = feature_module_guard};
        assert(!vco_vita_execution_set_features(&executor, features));
        close_uid_on_guard = call;
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        uint8_t data[4] = {1, 2, 3, 4};
        int32_t key = -99;
        assert(memory.inject(memory.context, session.token, BASE, data, 4, &key) < 0 &&
               key == -99 && injected == 1 && !restored && executor.patches[0].quarantined &&
               !executor.patches[0].active && !executor.poisoned &&
               vco_vita_execution_error(&executor) == VCO_E_RESTORE);
        unsigned queries = guid_ops;
        assert(vco_vita_execution_dispose(&executor) < 0 && guid_ops == queries && !restored);
        authorized = false;
        assert(!vco_vita_execution_reap_retired(&executor, &retired) && !restored &&
               !patches[0].references && !block_references[0]);
        finish();
        reset();
    }
}
static void patch_preflight_test(void)
{
    static const size_t sizes[] = {1, 2, 4};
    for (unsigned width = 0; width < sizeof(sizes) / sizeof(sizes[0]); ++width) {
        for (unsigned variant = 0; variant < 4; ++variant) {
            assert(!bind());
            vco_vita_execution_init(&executor, &native, &session, &support);
            VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
            fail_preflight_copy = variant == 1;
            revoke_after_copy = variant == 2;
            replace_after_read = variant == 3;
            uint8_t value[4] = {1, 2, 3, 4};
            int32_t key = -99;
            int result =
                memory.inject(memory.context, session.token, BASE, value, sizes[width], &key);
            assert(reads == 1 && !locks);
            if (!variant) {
                assert(!result && key > 0 && injected == 1 && !memcmp(bytes, value, sizes[width]));
                assert(!memory.release(memory.context, session.token, key) && restored == 1 &&
                       bytes[0] == 0x5a);
            } else {
                assert(result < 0 && key == -99 && !injected && !restored && !writes &&
                       !block_references[0] && !executor.patches[0].referenced &&
                       !executor.patches[0].block_referenced && !executor.blocked &&
                       !executor.poisoned && bytes[0] == 0x5a &&
                       vco_vita_execution_error(&executor) == VCO_OK);
                unsigned native_calls = guid_ops;
                assert(memory.release(memory.context, session.token, key) < 0 &&
                       guid_ops == native_calls);
            }
            assert(!vco_vita_execution_dispose(&executor) && !block_references[0]);
            finish();
            reset();
        }
    }
}
static void patch_restore_confirmation_test(void)
{
    for (unsigned variant = 0; variant < 8; ++variant) {
        assert(!bind());
        VcoIdentity retired = session.identity;
        vco_vita_execution_init(&executor, &native, &session, &support);
        VcoVitaExecutionFeatures features = {.module_context = &feature_base,
                                             .module_guard = feature_module_guard};
        assert(!vco_vita_execution_set_features(&executor, features));
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        uint8_t value[4] = {1, 2, 3, 4};
        int32_t key = -99;
        assert(!memory.inject(memory.context, session.token, BASE, value, 4, &key));
        assert(guard_calls == 4 && patches[0].references == 1 && block_references[0] == 1);
        if (variant < 2)
            restore_uid_mutation = variant + 1;
        else if (variant < 4)
            close_uid_on_guard = variant + 3; /* Outside or inside restore range lock. */
        else if (variant < 6)
            replace_uid_on_guard = variant + 1;
        else
            fail_uid_query_on_guard = variant - 5; /* Query or temporary release failure. */
        memset(bytes, 0xcc, sizeof(bytes));        /* New/unknown occupant must keep its bytes. */
        assert(memory.release(memory.context, session.token, key) < 0);
        assert(!restored && !locks && bytes[0] == 0xcc && executor.patches[0].quarantined &&
               !executor.patches[0].active && executor.blocked && patches[0].references == 1 &&
               block_references[0] == 1 && vco_vita_execution_error(&executor) == VCO_E_RESTORE);
        unsigned calls = guid_ops;
        int ranges = unmaps;
        assert(memory.release(memory.context, session.token, key) < 0);
        assert(vco_vita_execution_dispose(&executor) < 0 && guid_ops == calls && unmaps == ranges &&
               !restored && bytes[0] == 0xcc);
        if (variant == 1 || variant >= 4) {
            assert(executor.poisoned && vco_vita_execution_reap_retired(&executor, &retired) < 0 &&
                   guid_ops == calls);
            /* Unknown object replacement: only model a reboot after proving no retry. */
            memset(patches, 0, sizeof(patches));
            memset(block_references, 0, sizeof(block_references));
        } else {
            assert(!executor.poisoned);
            authorized = false;
            assert(!vco_vita_execution_reap_retired(&executor, &retired) && !restored &&
                   !patches[0].references && !block_references[0] && bytes[0] == 0xcc);
        }
        finish();
        reset();
    }
}
static void patch_publication_test(void)
{
    assert(!bind());
    VcoIdentity retired = session.identity;
    vco_vita_execution_init(&executor, &native, &session, &support);
    VcoVitaExecutionFeatures features = {.module_context = &feature_base,
                                         .module_guard = feature_module_guard};
    assert(!vco_vita_execution_set_features(&executor, features));
    guard_fail_at = 3;
    VcoCode code = {0xa200, BASE, 0x01020304, 1};
    VcoCheatEntry entry = {0, 0, 1, 0, {0, 0}};
    VcoCheatFile file = {&entry, 1, 1, &code, 1, 1, NULL, 0, 0};
    VcoCheatOp op;
    VcoPatch record;
    static VcoEngine engine;
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, vco_vita_execution_callbacks(&executor),
                            &op, &record, 1));
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
    vco_engine_step(&engine, 1);
    assert(engine.rows[0].state == VCO_ENTRY_ERROR && !engine.rows[0].desired &&
           engine.rows[0].diagnostic.code == VCO_E_PATCH && injected == 1 && !restored &&
           executor.patches[0].quarantined && vco_vita_execution_error(&executor) == VCO_E_RESTORE);
    /* A public OFF acknowledgement does not clear the private native residue. */
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 0, false}));
    vco_engine_step(&engine, 2);
    assert(engine.rows[0].state == VCO_ENTRY_OFF && !restored &&
           vco_vita_execution_error(&executor) == VCO_E_RESTORE);
    assert(!vco_engine_unbind(&engine) && vco_vita_execution_dispose(&executor) < 0);
    authorized = false;
    auto_cleanup(0);
    assert(!vco_vita_execution_reap_retired(&executor, &retired) && !restored &&
           vco_vita_execution_error(&executor) == VCO_OK);
    finish();
    reset();
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        for (unsigned change = BLOCK_CHANGE_UID; change <= BLOCK_CHANGE_RELEASE; ++change) {
            assert(!bind());
            retired = session.identity;
            vco_vita_execution_init(&executor, &native, &session, &support);
            configure_publication_change(scenario, change);
            vco_engine_init(&engine);
            assert(!vco_engine_bind(&engine, &session, 1, &file,
                                    vco_vita_execution_callbacks(&executor), &op, &record, 1));
            assert(
                !vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
            vco_engine_step(&engine, 1);
            assert(engine.rows[0].state == VCO_ENTRY_ERROR && !engine.rows[0].desired &&
                   engine.rows[0].diagnostic.code == VCO_E_PATCH && !record.active &&
                   !record.handle && !restored &&
                   vco_vita_execution_error(&executor) == VCO_E_RESTORE);
            assert(
                !vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 0, false}));
            vco_engine_step(&engine, 2);
            assert(engine.rows[0].state == VCO_ENTRY_OFF && !record.active && !record.handle &&
                   !restored && vco_vita_execution_error(&executor) == VCO_E_RESTORE);
            assert(!vco_engine_unbind(&engine));
            finish_publication_change(&retired, change);
        }
    }
}
static void patch_native_state_test(void)
{
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    assert(vco_vita_execution_error(NULL) == VCO_E_ARGUMENT);
    unsigned sdk = guid_ops, ops = operations;
    executor.blocked = true;
    assert(vco_vita_execution_error(&executor) == VCO_E_PATCH);
    for (unsigned slot = 0; slot < VCO_VITA_PATCH_LIMIT; ++slot) {
        VcoVitaPatch *p = &executor.patches[slot];
        for (unsigned field = 0; field < 6; ++field) {
            memset(p, 0, sizeof(*p));
            if (field == 0)
                p->active = true;
            else if (field == 1)
                p->referenced = true;
            else if (field == 2)
                p->block_referenced = true;
            else if (field == 3)
                p->object = &object;
            else if (field == 4)
                p->block_object = &block_objects[0];
            else
                p->quarantined = true;
            assert(vco_vita_execution_error(&executor) == VCO_E_RESTORE);
            executor.blocked = false;
            assert(vco_vita_execution_error(&executor) == (field == 5 ? VCO_E_RESTORE : VCO_OK));
            executor.blocked = true;
        }
        memset(p, 0, sizeof(*p));
    }
    executor.retired = true;
    assert(vco_vita_execution_error(&executor) == VCO_OK);
    executor.poisoned = true;
    assert(vco_vita_execution_error(&executor) == VCO_E_RESTORE);
    executor.poisoned = false;
    native.poisoned = true;
    assert(vco_vita_execution_error(&executor) == VCO_E_RESTORE);
    native.poisoned = false;
    assert(sdk == guid_ops && ops == operations); /* Observation is pure, no SDK release/query. */
    finish();
    assert(vco_vita_execution_error(&executor) == VCO_OK);
}
static void patch_authority_cleanup_test(void)
{
    for (unsigned variant = 0; variant < 4; ++variant) {
        reset();
        assert(!bind());
        vco_trace_init(&patch_trace_ring);
        vco_vita_patch_support_init(&support);
        assert(!vco_vita_patch_support_set_trace(&support, capture_patch_trace, &patch_trace_ring));
        assert(!vco_vita_patch_support_probe(&support));
        vco_vita_execution_init(&executor, &native, &session, &support);
        feature_base = BASE;
        assert(!vco_vita_execution_set_features(
            &executor, (VcoVitaExecutionFeatures){.module_context = &feature_base,
                                                  .module_guard = feature_module_guard}));
        VcoAdmission gate = VCO_ADMISSION_INITIALIZER;
        if (variant >= 2) {
            assert(!vco_vita_execution_set_admission(&executor, &gate));
            close_on_inject = observe_closing = &gate;
        }
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        revoke_after_inject = !(variant & 1u);
        revoke_after_unlock = (variant & 1u) != 0;
        uint8_t value[4] = {1, 2, 3, 4};
        int32_t handle = -99;
        assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) ==
               VCO_ACCESS_SESSION);
        assert(handle == -99 && injected == 1 && restored == 1 && !locks);
        for (unsigned i = 0; i < 4; ++i)
            assert(bytes[i] == 0x5a);
        assert(!patches[0].active && !patches[0].references && !block_references[0] &&
               !executor.blocked && !executor.poisoned &&
               vco_vita_execution_error(&executor) == VCO_OK);
        assert(!executor.patches[0].active && !executor.patches[0].referenced &&
               !executor.patches[0].block_referenced && !executor.patches[0].tai_handle);
        assert(trace_find(VCO_PATCH_TRACE_UNPUBLISHED_CLEANUP, 0) == 1);
        assert(!trace_find(VCO_PATCH_TRACE_BLOCKED, -1));
        if (variant >= 2)
            assert(vco_admission_drained(&gate));
        close_on_inject = observe_closing = NULL;
        assert(!memory.alive(memory.context, session.token));
        assert(!vco_vita_execution_dispose(&executor) && restored == 1);
        finish();
    }
}
static void patch_authority_failures_test(void)
{
    for (unsigned variant = 0; variant < 5; ++variant) {
        reset();
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        feature_base = BASE;
        assert(!vco_vita_execution_set_features(
            &executor, (VcoVitaExecutionFeatures){.module_context = &feature_base,
                                                  .module_guard = feature_module_guard}));
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        revoke_after_inject = true;
        fail_restore = variant == 0;
        deny_private_restore = variant == 1;
        guard_fail_at = variant == 2 ? 5 : 0; /* First cleanup ownership check. */
        fail_patch_reference_drop = variant == 3;
        fail_block_reference_drop = variant == 4;
        uint8_t value[4] = {1, 2, 3, 4};
        int32_t handle = -99;
        assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0);
        assert(handle == -99 && injected == 1 && !locks && executor.blocked &&
               vco_vita_execution_error(&executor) == VCO_E_RESTORE);
        assert(executor.patches[0].quarantined && !executor.patches[0].active);
        assert(restored == (variant == 1 || variant == 2 ? 0u : 1u));
        unsigned attempts = restored;
        assert(vco_vita_execution_dispose(&executor) < 0 && restored == attempts);
        foreground = true;
        assert(memory.inject(memory.context, session.token, BASE + 8, value, 4, &handle) < 0 &&
               injected == 1 && handle == -99);
        /* Explicit fake-kernel reboot after proving no retry/false recovery. */
        memset(patches, 0, sizeof(patches));
        memset(block_references, 0, sizeof(block_references));
        finish();
    }
}
static void patch_authority_engine_test(void)
{
    for (unsigned off_while_paused = 0; off_while_paused < 2; ++off_while_paused) {
        reset();
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        static VcoEngine engine;
        VcoCheatEntry entries[2];
        VcoCode codes[3];
        VcoCheatOp ops[3];
        VcoPatch receipts[3];
        char names[64];
        VcoCheatFile file = {entries, 2, 0, codes, 3, 0, names, sizeof(names), 0};
        VcoDiagnostic diagnostic;
        const char *text = "_V0 Two patches\n$A200 81000000 11223344\n"
                           "$A200 81000004 55667788\n_V0 Other\n$0200 81000008 00000009\n";
        assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
        vco_engine_init(&engine);
        assert(!vco_engine_bind(&engine, &session, 1, &file,
                                vco_vita_execution_callbacks(&executor), ops, receipts, 3));
        revoke_inject_at = 2; /* First public receipt plus one unpublished native patch. */
        assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
        vco_engine_step(&engine, 0);
        assert(injected == 2 && restored == 2 && !locks && !executor.blocked &&
               !engine.blocked_restore && engine.rows[0].desired &&
               engine.rows[0].state == VCO_ENTRY_PAUSED);
        for (unsigned i = 0; i < 8; ++i)
            assert(bytes[i] == 0x5a);
        assert(!receipts[0].active && !receipts[1].active && !block_references[0]);
        int32_t previous_key = executor.sequence;
        if (off_while_paused) {
            assert(
                !vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 0, false}));
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
            assert(engine.rows[0].state == VCO_ENTRY_OFF && !engine.rows[0].desired);
        }
        foreground = true;
        vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 2);
        if (off_while_paused)
            assert(injected == 2 && engine.rows[0].state == VCO_ENTRY_OFF);
        else {
            assert(injected == 4 && restored == 2 && engine.rows[0].state == VCO_ENTRY_ON &&
                   executor.sequence > previous_key);
            assert(
                !vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 0, false}));
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 3);
            assert(engine.rows[0].state == VCO_ENTRY_OFF && !engine.rows[0].desired &&
                   restored == 4 && bytes[0] == 0x5a && bytes[4] == 0x5a);
        }
        assert(!vco_engine_submit(&engine, (VcoExecutionCommand){3, session.token, 1, 1, true}));
        vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 4);
        assert(engine.rows[1].state == VCO_ENTRY_ON && bytes[8] == 9 && writes == 1);
        assert(!vco_engine_unbind(&engine) && !vco_vita_execution_dispose(&executor));
        finish();
    }
}
static void patch_rearm_test(void)
{
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    static VcoEngine engine;
    VcoCode code = {0xa200, BASE, 0x01020304, 1};
    VcoCheatEntry entry = {0, 0, 1, 0, {0, 0}};
    VcoCheatFile file = {&entry, 1, 1, &code, 1, 1, NULL, 0, 0};
    VcoCheatOp op;
    VcoPatch record;
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, vco_vita_execution_callbacks(&executor),
                            &op, &record, 1));
    revoke_after_inject = true;
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
    vco_engine_step(&engine, 1);
    assert(injected == 1 && restored == 1 && !executor.blocked &&
           engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[0].desired);
    assert(!vco_engine_unbind(&engine));
    assert(!vco_vita_execution_dispose(&executor) && restored == 1 && bytes[0] == 0x5a);
    /* An explicitly cleaned historical blocked executor may rearm; this does
     * not renew its foreground authority or reset its monotonic keys. */
    executor.blocked = true;
    int32_t sequence = executor.sequence;
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    assert(!memory.alive(memory.context, session.token));
    assert(!vco_vita_execution_rearm(&executor) && !executor.blocked);
    assert(executor.sequence == sequence && executor.sealed);
    assert(!memory.alive(memory.context, session.token)); /* No authority renewed by rearm. */
    foreground = true;
    revoke_after_inject = false;
    assert(!vco_engine_bind(&engine, &session, 2, &file, memory, &op, &record, 1));
    vco_engine_step(&engine, 50001);
    assert(injected == 1); /* Explicit rebind is always all OFF. */
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 2, 0, true}));
    vco_engine_step(&engine, 100001);
    assert(injected == 2 && executor.sequence > sequence && bytes[0] == 4);
    assert(!vco_engine_unbind(&engine) && !vco_vita_execution_dispose(&executor));
    assert(restored == 2 && bytes[0] == 0x5a);
    /* Every residual state is fail closed; no native calls occur in rearm. */
    unsigned calls = guid_ops;
    for (unsigned variant = 0; variant < 12; ++variant) {
        VcoVitaExecution saved = executor;
        VcoVitaMemory saved_native = native;
        executor.blocked = true;
        switch (variant) {
        case 0:
            executor.poisoned = true;
            break;
        case 1:
            executor.retired = true;
            break;
        case 2:
            native.poisoned = true;
            break;
        case 3:
            native.locked = true;
            break;
        case 4:
            native.bound = false;
            break;
        case 5:
            ++native.identity.instance;
            break;
        case 6:
            executor.patches[0].active = true;
            break;
        case 7:
            executor.patches[0].quarantined = true;
            break;
        case 8:
            executor.patches[0].referenced = true;
            break;
        case 9:
            executor.patches[0].block_referenced = true;
            break;
        case 10:
            executor.patches[0].object = &object;
            break;
        default:
            executor.patches[0].block_object = &object;
            break;
        }
        assert(vco_vita_execution_rearm(&executor) < 0 && executor.blocked);
        assert(guid_ops == calls && executor.sequence == saved.sequence);
        executor = saved;
        native = saved_native;
    }
    assert(!vco_vita_execution_rearm(&executor));
    finish();
}
static void authority_pause_test(void)
{
    for (unsigned variant = 0; variant < 4; ++variant) {
        reset();
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        static VcoEngine engine;
        VcoCheatEntry entry[1];
        VcoCode codes[2];
        VcoCheatOp ops[2];
        VcoPatch receipts[2];
        char names[64];
        VcoCheatFile file = {entry, 1, 0, codes, 2, 0, names, sizeof(names), 0};
        VcoDiagnostic diagnostic;
        const char *text = variant == 1 ? "_V0 Write\n$0200 81000000 11223344\n"
                                        : "_V0 Read then write\n$D202 81000000 5A5A5A5A\n"
                                          "$0200 81000004 11223344\n";
        assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
        vco_engine_init(&engine);
        assert(!vco_engine_bind(&engine, &session, 1, &file,
                                vco_vita_execution_callbacks(&executor), ops, receipts, 2));
        assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
        revoke_after_copy = true;
        fail_unlock = variant == 2;
        fail_preflight_copy = variant == 3;
        vco_engine_step(&engine, 0);
        assert(!locks);
        if (variant < 2) {
            assert(engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[0].desired &&
                   engine.rows[0].diagnostic.code == VCO_OK);
            assert(writes == (variant == 1 ? 1 : 0));
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
            assert(writes == (variant == 1 ? 1 : 0));
            revoke_after_copy = false;
            foreground = true;
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 2);
            assert(engine.rows[0].state == VCO_ENTRY_ON && writes == (variant == 1 ? 2 : 1));
        } else {
            assert(engine.rows[0].state == VCO_ENTRY_ERROR && !engine.rows[0].desired &&
                   engine.rows[0].diagnostic.code == VCO_E_IO);
            assert(variant == 2 ? native.poisoned : !native.poisoned);
            assert(variant == 2 ? vco_vita_execution_error(&executor) == VCO_E_RESTORE
                                : vco_vita_execution_error(&executor) == VCO_OK);
            revoke_after_copy = false;
            foreground = true;
            unsigned before = reads;
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US * 2);
            assert(reads == (int)before && !writes);
        }
        assert(!vco_engine_unbind(&engine));
        assert(!vco_vita_execution_dispose(&executor));
        finish();
    }
}
static void guarded_claims_test(void)
{
    for (unsigned restore_failure = 0; restore_failure < 2; ++restore_failure) {
        reset();
        assert(!bind());
        feature_result = 0;
        feature_revoke = false;
        vco_vita_execution_init(&executor, &native, &session, &support);
        assert(!vco_vita_execution_set_features(
            &executor,
            (VcoVitaExecutionFeatures){.pad_context = &feature_size, .pad = feature_pad}));
        static VcoEngine engine;
        VcoCheatEntry entries[2];
        VcoCode codes[3];
        VcoCheatOp ops[3];
        VcoPatch receipts[3];
        char names[64];
        VcoCheatFile file = {entries, 2, 0, codes, 3, 0, names, sizeof(names), 0};
        VcoDiagnostic diagnostic;
        const char *text = "_V0 Button patch\n$C201 00000000 00000300\n"
                           "$A200 81000010 E3A00001\n_V0 Other\n$0200 81000010 00000009\n";
        assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
        vco_engine_init(&engine);
        assert(!vco_engine_bind(&engine, &session, 1, &file,
                                vco_vita_execution_callbacks(&executor), ops, receipts, 3));
        assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
        vco_engine_step(&engine, 0);
        assert(engine.rows[0].state == VCO_ENTRY_ON && injected == 1 && bytes[16] == 1);
        feature_result = VCO_PAD_SUPPRESSED;
        fail_restore = restore_failure != 0;
        vco_engine_step(&engine, VCO_ENGINE_PERIOD_US);
        if (!restore_failure) {
            assert(engine.rows[0].state == VCO_ENTRY_ON && restored == 1 && bytes[16] == 0x5a);
            assert(
                !vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 1, true}));
            vco_engine_step(&engine, VCO_ENGINE_PERIOD_US + 1);
            assert(engine.rows[1].state == VCO_ENTRY_ON && bytes[16] == 9 && writes == 1);
            assert(!vco_engine_unbind(&engine) && !vco_vita_execution_dispose(&executor));
        } else {
            assert(engine.blocked_restore && engine.rows[0].diagnostic.code == VCO_E_RESTORE &&
                   vco_vita_execution_error(&executor) == VCO_E_RESTORE);
            assert(vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 1,
                                                                    true}) == VCO_E_RESTORE);
            assert(vco_engine_unbind(&engine) == VCO_E_RESTORE && !writes);
            fail_restore = false;
            unsigned attempts = restored;
            assert(vco_vita_execution_dispose(&executor) < 0 && restored == attempts);
            /* The fake failed release already closed its native UID. Only
             * independently simulated process retirement permits dropping the
             * quarantined references; no restore retry occurs. */
            authorized = false;
            assert(!vco_vita_execution_reap_retired(&executor, &session.identity));
            assert(restored == attempts);
        }
        finish();
    }
}
static VcoEngine close_engine;
static unsigned close_finalizes;
static VcoError close_native_executor(void *context)
{
    assert(context == &executor && close_engine.closing && !close_engine.close_complete);
    ++close_finalizes;
    return vco_vita_execution_dispose(context) ? VCO_E_RESTORE : VCO_OK;
}
static void scoped_close_test(void)
{
    for (unsigned variant = 0; variant < 4; ++variant) {
        reset();
        assert(!bind());
        vco_vita_execution_init(&executor, &native, &session, &support);
        VcoCheatEntry entries[2];
        VcoCode codes[3];
        VcoCheatOp ops[3];
        VcoPatch receipts[3];
        char names[64];
        VcoCheatFile file = {entries, 2, 0, codes, 3, 0, names, sizeof(names), 0};
        VcoDiagnostic diagnostic;
        const char *text = "_V0 Public patches\n$A200 81000000 11223344\n"
                           "$A200 81000004 55667788\n_V0 Pending data\n"
                           "$0200 81000008 00000009\n";
        assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
        vco_engine_init(&close_engine);
        VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
        assert(!vco_engine_bind(&close_engine, &session, 1, &file, memory, ops, receipts, 3));
        assert(
            !vco_engine_submit(&close_engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
        vco_engine_step(&close_engine, 0);
        assert(injected == 2 && !restored && receipts[0].active && receipts[1].active);
        /* A native owner absent from the public runtime must also be drained.
         * Variant 2 makes the unpublished receipt fail its immediate cleanup. */
        uint8_t value[4] = {1, 2, 3, 4};
        int32_t private_handle = -99;
        if (variant == 2)
            revoke_after_inject = fail_restore = true;
        int injected_result =
            memory.inject(memory.context, session.token, BASE + 24, value, 4, &private_handle);
        if (variant == 2) {
            assert(injected_result < 0 && private_handle == -99 && restored == 1 &&
                   executor.patches[2].quarantined);
            foreground = true;
            revoke_after_inject = fail_restore = false;
        } else
            assert(!injected_result && private_handle > 0 && executor.patches[2].active);
        assert(injected == 3);
        assert(
            !vco_engine_submit(&close_engine, (VcoExecutionCommand){2, session.token, 1, 1, true}));
        VcoEngineCloseKey closing = {.identity = close_engine.identity,
                                     .session = session.token,
                                     .revision = 1,
                                     .transaction = 1,
                                     .scope = VCO_CLOSE_PROCESS,
                                     .process_object = (uintptr_t)native.process_object};
        if (variant == 1)
            fail_restore = true;
        if (variant == 3) {
            current = &replacement;
            memset(bytes, 0xcc, sizeof(bytes));
        }
        close_finalizes = 0;
        VcoEngineCloseResult result;
        VcoError expected = variant ? VCO_E_RESTORE : VCO_OK;
        assert(vco_engine_close(&close_engine, &closing, close_native_executor, &executor,
                                &result) == expected);
        assert(close_finalizes == 1 && !writes && !locks && result.native == expected);
        assert(result.entries == (variant == 1 || variant == 3 ? VCO_E_RESTORE : VCO_OK));
        assert(restored == (variant == 3 ? 0u : 3u));
        unsigned sdk = operations, native_calls = guid_ops, attempts = restored;
        assert(vco_engine_close(&close_engine, &closing, close_native_executor, &executor,
                                &result) == expected);
        vco_engine_step(&close_engine, VCO_ENGINE_PERIOD_US);
        assert(close_finalizes == 1 && operations == sdk && guid_ops == native_calls &&
               restored == attempts && !writes);
        if (!variant) {
            for (unsigned i = 0; i < sizeof(bytes); ++i)
                assert(bytes[i] == 0x5a);
            int32_t old_sequence = executor.sequence;
            assert(!vco_vita_execution_rearm(&executor));
            assert(!vco_engine_reopen(&close_engine, &closing, 2));
            vco_engine_step(&close_engine, VCO_ENGINE_PERIOD_US * 2);
            assert(injected == 3 && !writes);
            assert(!vco_engine_submit(&close_engine,
                                      (VcoExecutionCommand){1, session.token, 2, 0, true}));
            vco_engine_step(&close_engine, VCO_ENGINE_PERIOD_US * 3);
            assert(injected == 5 && executor.sequence > old_sequence);
            assert(!vco_engine_unbind(&close_engine) && restored == 5);
            assert(!vco_vita_execution_dispose(&executor));
        } else {
            assert(vco_engine_reopen(&close_engine, &closing, 2) == VCO_E_RESTORE);
            assert(vco_engine_unbind(&close_engine) == VCO_E_RESTORE && restored == attempts);
            /* Explicit model of trusted OS retirement/UID closure; only the
             * read-only retired reaper can drop the original retained objects. */
            authorized = false;
            for (unsigned i = 0; i < injected; ++i)
                patches[i].active = patches[i].uid_open = false;
            assert(!vco_vita_execution_reap_retired(&executor, &closing.identity));
            assert(restored == attempts);
            if (variant == 3)
                for (unsigned i = 0; i < sizeof(bytes); ++i)
                    assert(bytes[i] == 0xcc); /* No stale restoration. */
        }
        finish();
    }
}
static void pipeline(void)
{
    assert(!bind());
    vco_vita_execution_init(&executor, &native, &session, &support);
    static VcoEngine engine;
    VcoCheatEntry entries[2];
    VcoCode codes[2];
    char names[128];
    VcoCheatOp ops[2];
    VcoPatch slots[2];
    VcoDiagnostic diagnostic;
    VcoCheatFile file = {entries, 2, 0, codes, 2, 0, names, sizeof(names), 0};
    const char text[] = "_V1 Data\n$0200 81000000 11223344\n_V0 Patch\n$A200 81000010 E3A00001\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, vco_vita_execution_callbacks(&executor),
                            ops, slots, 2));
    vco_engine_step(&engine, 1);
    assert(!writes && !injected); /* _V1 never auto-enables. */
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 1, true}));
    vco_engine_step(&engine, 50001);
    assert(engine.rows[0].state == VCO_ENTRY_ON && engine.rows[1].state == VCO_ENTRY_ON);
    assert(bytes[0] == 0x44 && bytes[16] == 1 && injected == 1);
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){3, session.token, 1, 0, false}));
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){4, session.token, 1, 1, false}));
    int previous_writes = writes;
    vco_engine_step(&engine, 100001);
    vco_engine_step(&engine, 150001);
    assert(engine.rows[0].state == VCO_ENTRY_OFF && engine.rows[1].state == VCO_ENTRY_OFF);
    assert(writes == previous_writes && bytes[0] == 0x44 && bytes[16] == 0x5a && restored == 1);
    assert(!vco_engine_unbind(&engine));
    assert(!vco_vita_execution_dispose(&executor));
    finish();
}
static void data_admission_test(void)
{
    for (unsigned writing = 0; writing < 2; ++writing) {
        for (unsigned unlock_failure = 0; unlock_failure < 2; ++unlock_failure) {
            reset();
            assert(!bind());
            VcoAdmission gate = VCO_ADMISSION_INITIALIZER;
            vco_vita_execution_init(&executor, &native, &session, &support);
            assert(!vco_vita_execution_set_admission(&executor, &gate));
            VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
            uint8_t value[4] = {1, 2, 3, 4}, out[4] = {0};
            close_on_copy = observe_closing = &gate;
            fail_unlock = unlock_failure != 0;
            int result = writing ? memory.write(memory.context, session.token, BASE, value, 4)
                                 : memory.read(memory.context, session.token, BASE, out, 4);
            assert(unlock_failure ? result < 0 : result == 0);
            assert(vco_admission_drained(&gate) && !locks);
            close_on_copy = observe_closing = NULL;
            if (writing)
                assert(!memcmp(bytes, value, 4));
            else if (!unlock_failure)
                assert(out[0] == 0x5a && out[3] == 0x5a);
            int previous_reads = reads, previous_writes = writes, previous_unmaps = unmaps;
            memset(out, 0xa5, sizeof(out));
            assert(memory.read(memory.context, session.token, BASE, out, 4) < 0);
            assert(memory.write(memory.context, session.token, BASE, value, 4) < 0);
            assert(out[0] == 0xa5 && out[3] == 0xa5);
            assert(reads == previous_reads && writes == previous_writes &&
                   unmaps == previous_unmaps);
            assert(!vco_vita_execution_dispose(&executor));
            finish();
        }
    }
}
static void patch_admission_test(void)
{
    assert(!bind());
    VcoAdmission gate = VCO_ADMISSION_INITIALIZER;
    vco_vita_execution_init(&executor, &native, &session, &support);
    assert(!vco_vita_execution_set_admission(&executor, &gate));
    VcoCheatMemory memory = vco_vita_execution_callbacks(&executor);
    assert(vco_vita_execution_set_admission(&executor, NULL) < 0);
    uint8_t value[4] = {1, 2, 3, 4};
    int32_t handle = -99;
    assert(!memory.inject(memory.context, session.token, BASE, value, 4, &handle));
    close_on_restore = observe_closing = &gate;
    assert(!memory.release(memory.context, session.token, handle));
    assert(vco_admission_drained(&gate));
    close_on_restore = observe_closing = NULL;
    assert(bytes[0] == 0x5a && restored == 1);
    handle = -99;
    assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0);
    assert(handle == -99 && injected == 1);
    assert(!vco_vita_execution_dispose(&executor));
    assert(vco_vita_execution_rearm(&executor) < 0);
    finish();

    reset();
    assert(!bind());
    VcoAdmission failed = VCO_ADMISSION_INITIALIZER;
    vco_vita_execution_init(&executor, &native, &session, &support);
    assert(!vco_vita_execution_set_admission(&executor, &failed));
    memory = vco_vita_execution_callbacks(&executor);
    close_on_inject = observe_closing = &failed;
    fail_inject = true;
    assert(memory.inject(memory.context, session.token, BASE, value, 4, &handle) < 0);
    assert(handle == -99 && !injected);
    assert(vco_admission_drained(&failed));
    close_on_inject = observe_closing = NULL;
    assert(!vco_vita_execution_dispose(&executor));
    finish();
    reset();
    assert(!bind());
    VcoAdmission closed = VCO_ADMISSION_INITIALIZER;
    vco_vita_execution_init(&executor, &native, &session, &support);
    assert(!vco_vita_execution_set_admission(&executor, &closed));
    memory = vco_vita_execution_callbacks(&executor);
    assert(!memory.inject(memory.context, session.token, BASE, value, 4, &handle));
    vco_admission_close(&closed);
    assert(vco_admission_drained(&closed));
    unsigned calls = guid_ops;
    int ranges = unmaps;
    assert(memory.release(memory.context, session.token, handle) < 0);
    assert(vco_vita_execution_dispose(&executor) < 0);
    assert(vco_vita_execution_error(&executor) == VCO_E_RESTORE);
    assert(!restored && guid_ops == calls && unmaps == ranges);
    assert(executor.patches[0].active && executor.patches[0].referenced &&
           executor.patches[0].block_referenced && patches[0].references == 1 &&
           block_references[0] == 1 && !memcmp(bytes, value, 4));
    assert(vco_vita_execution_rearm(&executor) < 0);
    /* Model independent process retirement and OS cleanup, never use gate
     * closure/drain itself as evidence that the process or patch has died. */
    authorized = false;
    assert(vco_vita_execution_reap_retired(&executor, &session.identity) == 1);
    assert(patches[0].references == 1 && block_references[0] == 1);
    auto_cleanup(0);
    assert(!vco_vita_execution_reap_retired(&executor, &session.identity));
    assert(!restored && !patches[0].references && !block_references[0]);
    assert(vco_vita_execution_error(&executor) == VCO_OK);
    finish();
}
int main(int argc, char **argv)
{
    assert(argc == 2 ||
           (argc == 3 && (!strcmp(argv[1], "pcsb00031") || !strcmp(argv[1], "pcsb00031_patches") ||
                          !strcmp(argv[1], "pcsb00031_trajectory"))));
    reset();
    if (!strcmp(argv[1], "pcsb00031"))
        pcsb00031(argv[2]);
    else if (!strcmp(argv[1], "pcsb00031_patches"))
        pcsb00031_patches(argv[2]);
    else if (!strcmp(argv[1], "pcsb00031_trajectory"))
        pcsb00031_trajectory(argv[2]);
    else if (!strcmp(argv[1], "patch_capacity"))
        patch_capacity_test();
    else if (!strcmp(argv[1], "data_admission"))
        data_admission_test();
    else if (!strcmp(argv[1], "patch_admission"))
        patch_admission_test();
    else if (!strcmp(argv[1], "transfer"))
        transfer();
    else if (!strcmp(argv[1], "memblock_contract"))
        memblock_contract();
    else if (!strcmp(argv[1], "memblock_spans"))
        memblock_spans();
    else if (!strcmp(argv[1], "patch_spans"))
        patch_spans();
    else if (!strcmp(argv[1], "memory_trace"))
        memory_trace_test();
    else if (!strcmp(argv[1], "denied"))
        denied();
    else if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else if (!strcmp(argv[1], "patches"))
        patch_test();
    else if (!strcmp(argv[1], "patch_trace"))
        patch_trace_test();
    else if (!strcmp(argv[1], "patch_failure_stage"))
        patch_failure_stage_test();
    else if (!strcmp(argv[1], "patch_preacquisition_authority"))
        patch_preacquisition_authority_test();
    else if (!strcmp(argv[1], "patch_probe"))
        patch_probe_test();
    else if (!strcmp(argv[1], "patch_lifecycle"))
        patch_lifecycle_test();
    else if (!strcmp(argv[1], "patch_faults"))
        patch_faults_test();
    else if (!strcmp(argv[1], "patch_mapping"))
        patch_mapping_test();
    else if (!strcmp(argv[1], "patch_modules"))
        patch_modules_test();
    else if (!strcmp(argv[1], "patch_rearm"))
        patch_rearm_test();
    else if (!strcmp(argv[1], "patch_authority_cleanup"))
        patch_authority_cleanup_test();
    else if (!strcmp(argv[1], "patch_authority_failures"))
        patch_authority_failures_test();
    else if (!strcmp(argv[1], "patch_authority_engine"))
        patch_authority_engine_test();
    else if (!strcmp(argv[1], "patch_confirmation"))
        patch_confirmation_test();
    else if (!strcmp(argv[1], "patch_closed_confirmation"))
        patch_closed_confirmation_test();
    else if (!strcmp(argv[1], "patch_restore_confirmation"))
        patch_restore_confirmation_test();
    else if (!strcmp(argv[1], "patch_preflight"))
        patch_preflight_test();
    else if (!strcmp(argv[1], "patch_publication"))
        patch_publication_test();
    else if (!strcmp(argv[1], "patch_native_state"))
        patch_native_state_test();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else if (!strcmp(argv[1], "scoped_close"))
        scoped_close_test();
    else if (!strcmp(argv[1], "authority_pause"))
        authority_pause_test();
    else if (!strcmp(argv[1], "guarded_claims"))
        guarded_claims_test();
    else if (!strcmp(argv[1], "features"))
        features();
    else
        assert(0);
    assert(!references && !locks);
    return 0;
}
