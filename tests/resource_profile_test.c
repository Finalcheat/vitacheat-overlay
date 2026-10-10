#include "platform/resource_profile.h"
#include "platform/execution_service.h"
#include "diagnostics.h"
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char tag[112];
    uint32_t value;
    bool reset;
} Record;
static Record records[304];
static unsigned writes, attempts, clocks, queries, pools, failed_attempt, initial_busy;
static int64_t now, finish_offset;
static int io_result, thread_error, memory_error;
static bool invalid_thread, invalid_memory;
int64_t sceKernelGetSystemTimeWide(void)
{
    ++clocks;
    return now + (clocks % 2 == 0 ? finish_offset : 0);
}
int sceKernelGetThreadInfo(SceUID id, SceKernelThreadInfo *info)
{
    assert((id == 11 || id == 12) && info->size == sizeof(*info));
    ++queries;
    info->runClocks = UINT64_C(0x1234567800000000) + queries;
    info->stackSize = id == 11 ? 8192 : 32768;
    if (invalid_thread)
        info->size = 0;
    return thread_error;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info)
{
    assert(info->size == sizeof(*info));
    ++pools;
    info->size_user = 12345678;
    info->size_cdram = 4567890;
    info->size_phycont = invalid_memory ? -1 : 12345;
    return memory_error;
}
int vco_diagnostics_write(const char *path, const char *tag, unsigned value, int reset)
{
    assert(!strcmp(path, VCO_RESOURCE_LOG) && strlen(tag) <= 110);
    ++attempts;
    if (attempts <= initial_busy)
        return VCO_DIAGNOSTICS_BUSY;
    if (failed_attempt && attempts == failed_attempt)
        return io_result;
    assert(writes < sizeof(records) / sizeof(records[0]));
    Record *record = &records[writes++];
    strcpy(record->tag, tag);
    record->value = value;
    record->reset = reset;
    return VCO_DIAGNOSTICS_OK;
}
static void tick(void)
{
    vco_resource_profile_tick(11, 12, 1, 2, UINT64_C(0xabcdef0123456789));
}
static void snapshot(void)
{
    tick();
    assert(writes == 6 && records[0].reset && records[0].value == 60);
    for (unsigned i = 1; i < writes; ++i)
        assert(!records[i].reset);
    assert(!strcmp(records[1].tag,
                   "rp_sample 0001 0000000000000000 000000000000007b 00000000 00000002 "
                   "00000001 abcdef0123456789"));
    assert(!strcmp(records[2].tag, "rp_observer 0001 0000000b 1234567800000001 00002000"));
    assert(!strcmp(records[3].tag, "rp_control 0001 0000000c 1234567800000002 00008000"));
    assert(!strcmp(records[4].tag, "rp_memory 0001 00bc614e 0045b352 00003039"));
    assert(!strcmp(records[5].tag, "rp_commit 0001") && !records[5].value);
}
static void cadence(void)
{
    tick();
    for (now = 20000; now < 5000000; now += 20000)
        tick();
    assert(writes == 6 && queries == 2 && pools == 1);
    tick();
    assert(writes == 11 && queries == 4 && pools == 2);
    now = 20000000; /* No catch-up burst after a delayed worker. */
    tick();
    assert(writes == 16 && queries == 6);
}
static void window(void)
{
    for (unsigned i = 0; i <= 60; ++i) {
        now = (int64_t)i * 5000000;
        tick();
    }
    assert(writes == 302 && queries == 120 && pools == 60);
    assert(!strncmp(records[301].tag, "rp_end 003c ", 12) && records[301].value == 1);
    unsigned calls = clocks;
    for (unsigned i = 0; i < 100; ++i)
        tick();
    assert(clocks == calls && writes == 302);
}
static void clock_failure(const char *kind)
{
    if (!strcmp(kind, "clock_start")) {
        now = -1;
        tick();
        assert(writes == 2 && records[0].reset && records[1].value == 2 && !queries);
    } else {
        now = 5000000;
        tick();
        if (!strcmp(kind, "clock_reverse"))
            now = 4999999;
        else if (!strcmp(kind, "clock_error"))
            now = -1;
        else {
            now = 10000000;
            finish_offset = -1;
        }
        tick();
        assert(writes == 7 && !strncmp(records[6].tag, "rp_end", 6) && records[6].value == 2);
        assert(queries == (!strcmp(kind, "clock_finish") ? 4 : 2));
    }
    unsigned calls = clocks;
    tick();
    assert(clocks == calls);
}
static void io_failure(unsigned position, int failure)
{
    failed_attempt = position;
    io_result = failure;
    tick();
    assert(attempts == position && writes == position - 1);
    unsigned calls = clocks;
    now = 5000000;
    tick();
    if (position == 1 && failure == VCO_DIAGNOSTICS_BUSY) {
        /* A busy header performed no I/O. Retry once on the next worker turn,
         * then acquire a fresh sample; no failed sample is replayed. */
        assert(attempts == 7 && writes == 6 && clocks == 2);
        assert(records[0].reset && !strcmp(records[5].tag, "rp_commit 0001"));
    } else if (failure == VCO_DIAGNOSTICS_BUSY) {
        /* Drop the interrupted sample, then acquire a fresh one at the next
         * interval. A busy shared logger must not end the whole capture. */
        assert(attempts == position + 5 && writes == position + 4);
        assert(queries == 4 && pools == 2);
        assert(!strncmp(records[position - 1].tag, "rp_sample 0002 ", 15));
        assert(!strcmp(records[position + 3].tag, "rp_commit 0002"));
    } else
        assert(attempts == position && clocks == calls); /* Real I/O failures stop. */
}
static void end_busy(const char *kind)
{
    tick();
    assert(writes == 6 && queries == 2);
    bool exhausted = !strcmp(kind, "end_busy_exhausted");
    bool clock_error = !strcmp(kind, "clock_end_busy");
    now = clock_error ? -1 : 300000000;
    if (exhausted)
        initial_busy = 100;
    else {
        failed_attempt = 7;
        io_result = !strcmp(kind, "end_io_error") ? VCO_DIAGNOSTICS_ERROR : VCO_DIAGNOSTICS_BUSY;
    }
    tick();
    assert(attempts == 7 && writes == 6 && queries == 2);
    if (!strcmp(kind, "end_io_error")) {
        tick();
        assert(attempts == 7 && queries == 2);
        return;
    }
    if (exhausted) {
        for (unsigned i = 1; i < 8; ++i) {
            now += 20000;
            tick();
        }
        assert(attempts == 14 && writes == 6 && queries == 2);
        initial_busy = 0;
        for (unsigned i = 0; i < 100; ++i)
            tick();
        assert(attempts == 14 && writes == 6 && queries == 2);
    } else {
        now = clock_error ? 5000000 : 300020000;
        tick();
        assert(attempts == 8 && writes == 7 && queries == 2);
        assert(!strncmp(records[6].tag, "rp_end 0001 ", 12));
        assert(records[6].value == (clock_error ? 2u : 1u));
        tick();
        assert(attempts == 8 && writes == 7 && queries == 2);
    }
}
static void startup_busy(unsigned count)
{
    initial_busy = count;
    unsigned limit = 8;
    for (unsigned i = 0; i < count && i < limit; ++i) {
        tick();
        assert(attempts == i + 1 && !writes && !clocks && !queries && !pools);
        now += 20000;
    }
    if (count < limit) {
        tick();
        assert(attempts == count + 6 && writes == 6 && clocks == 2);
        assert(records[0].reset && records[0].value == 60);
        assert(!strcmp(records[5].tag, "rp_commit 0001"));
        now += 5000000;
        tick();
        assert(writes == 11 && !records[6].reset);
    } else {
        for (unsigned i = 0; i < 100; ++i)
            tick();
        assert(attempts == limit && !writes && !clocks && !queries && !pools);
    }
}
static void faults(bool invalid)
{
    invalid_thread = invalid_memory = invalid;
    thread_error = invalid ? 0 : (int)UINT32_C(0x80028025);
    memory_error = invalid ? 0 : (int)UINT32_C(0x80020005);
    tick();
    assert(writes == 6);
    uint32_t error = invalid ? UINT32_C(0xffff0001) : (uint32_t)thread_error;
    assert(records[2].value == error && records[3].value == error);
    assert(!strcmp(records[2].tag, "rp_observer 0001 0000000b 0000000000000000 00000000"));
    assert(records[4].value == (invalid ? UINT32_C(0xffff0001) : (uint32_t)memory_error));
    assert(!strcmp(records[4].tag, "rp_memory 0001 00000000 00000000 00000000"));
    thread_error = memory_error = 0;
    invalid_thread = invalid_memory = false;
    now = 5000000;
    tick();
    assert(!records[7].value && !records[8].value && !records[9].value);
}
static void visibility(void)
{
    vco_resource_profile_visible(true);
    tick();
    assert(strstr(records[1].tag, " 00000003 00000002 "));
    vco_resource_profile_visible(true);
    vco_resource_profile_visible(false);
    vco_resource_profile_visible(true);
    now = 5000000;
    tick();
    assert(strstr(records[6].tag, " 00000007 00000002 "));
}
static void wait_for_game(void)
{
    /* Boot and menu time must neither consume the capture window nor erase a
     * prior log. Only a ready, bound game starts this one-shot measurement. */
    const uint32_t waiting[] = {VCO_EXECUTION_LOADING,  VCO_EXECUTION_BACKGROUND,
                                VCO_EXECUTION_NO_GAME,  VCO_EXECUTION_ERROR,
                                VCO_EXECUTION_CONFLICT, VCO_EXECUTION_UNAVAILABLE};
    for (unsigned i = 0; i < sizeof(waiting) / sizeof(waiting[0]); ++i) {
        now += INT64_C(600000000);
        vco_resource_profile_tick(11, 12, waiting[i], 0, 1);
        vco_resource_profile_tick(11, 12, VCO_EXECUTION_READY, 0, 0);
        assert(!attempts && !writes && !clocks && !queries && !pools);
    }
    int64_t start_time = now;
    tick();
    assert(writes == 6 && queries == 2 && pools == 1);
    for (unsigned i = 1; i <= 60; ++i) {
        now = start_time + (int64_t)i * 5000000;
        vco_resource_profile_tick(11, 12, VCO_EXECUTION_NO_GAME, 0, 0);
    }
    assert(writes == 302 && queries == 120 && pools == 60);
    assert(records[301].value == 1);
    unsigned calls = clocks;
    now += INT64_C(600000000);
    tick();
    assert(clocks == calls && writes == 302); /* No restart on another game. */
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (!strcmp(argv[1], "snapshot")) {
        finish_offset = 123;
        snapshot();
    } else if (!strcmp(argv[1], "cadence"))
        cadence();
    else if (!strcmp(argv[1], "window"))
        window();
    else if (!strncmp(argv[1], "end_", 4) || !strcmp(argv[1], "clock_end_busy"))
        end_busy(argv[1]);
    else if (!strcmp(argv[1], "startup_busy")) {
        assert(argc == 3);
        startup_busy((unsigned)atoi(argv[2]));
    } else if (!strncmp(argv[1], "clock_", 6))
        clock_failure(argv[1]);
    else if (!strcmp(argv[1], "io")) {
        assert(argc == 4);
        io_failure((unsigned)atoi(argv[2]), atoi(argv[3]));
    } else if (!strcmp(argv[1], "faults"))
        faults(false);
    else if (!strcmp(argv[1], "invalid"))
        faults(true);
    else if (!strcmp(argv[1], "visibility"))
        visibility();
    else if (!strcmp(argv[1], "wait_for_game"))
        wait_for_game();
    else if (!strcmp(argv[1], "emit") || !strcmp(argv[1], "emit_busy")) {
        bool busy = !strcmp(argv[1], "emit_busy");
        if (busy) {
            assert(argc == 3);
            failed_attempt = (unsigned)atoi(argv[2]);
            io_result = VCO_DIAGNOSTICS_BUSY;
        }
        tick();
        vco_resource_profile_visible(true);
        now = 5000000;
        tick();
        now = 10000000;
        tick();
        now = 300000000;
        if (busy)
            failed_attempt = attempts + 1;
        tick();
        if (busy) {
            now += 20000;
            tick();
        }
        for (unsigned i = 0; i < writes; ++i)
            printf("%s 0x%08x\n", records[i].tag, records[i].value);
        return 0;
    } else
        assert(!"unknown case");
    puts("bounded resource profile passed");
    return 0;
}
