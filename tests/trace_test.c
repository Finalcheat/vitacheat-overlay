#include "execution/trace.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static void zero(const void *memory, size_t size)
{
    const unsigned char *bytes = memory;
    for (size_t i = 0; i < size; ++i)
        assert(!bytes[i]);
}
static void append(VcoTraceRing *ring, unsigned value)
{
    assert(vco_trace_append(ring, value + 100, 7, -(int32_t)value, value, value + 1) ==
           VCO_TRACE_OK);
}
static void tail(const VcoTraceBatch *batch)
{
    assert(batch->abi == VCO_TRACE_ABI && batch->size == sizeof(*batch));
    assert(batch->count <= VCO_TRACE_BATCH);
    zero(batch->records + batch->count,
         (VCO_TRACE_BATCH - batch->count) * sizeof(batch->records[0]));
}
static void snapshot(void)
{
    static VcoTraceRing ring; /* No init call is required for permanent static storage. */
    VcoTraceBatch batch;
    memset(&batch, 0xa5, sizeof(batch));
    assert(vco_trace_snapshot(&ring, 0, &batch) == VCO_TRACE_OK);
    assert(!batch.oldest_sequence && !batch.latest_sequence && !batch.count && !batch.dropped);
    tail(&batch);
    for (unsigned i = 0; i < 13; ++i)
        append(&ring, i);
    assert(vco_trace_snapshot(&ring, 0, &batch) == VCO_TRACE_OK);
    assert(batch.oldest_sequence == 1 && batch.latest_sequence == 13 && batch.count == 8);
    for (unsigned i = 0; i < batch.count; ++i) {
        VcoTraceRecord expected = {i + 1, i + 100, 7, -(int32_t)i, i, i + 1};
        assert(!memcmp(&batch.records[i], &expected, sizeof(expected)));
    }
    VcoTraceBatch retry;
    assert(vco_trace_snapshot(&ring, 0, &retry) == VCO_TRACE_OK);
    assert(!memcmp(&batch, &retry, sizeof(batch))); /* A failed downstream copy can reread. */
    assert(vco_trace_snapshot(&ring, 8, &batch) == VCO_TRACE_OK);
    assert(batch.count == 5 && batch.records[0].sequence == 9 && batch.records[4].sequence == 13);
    tail(&batch);
    assert(vco_trace_snapshot(&ring, 13, &batch) == VCO_TRACE_OK);
    assert(!batch.count && batch.oldest_sequence == 1 && batch.latest_sequence == 13);
    tail(&batch);
    append(&ring, 13);
    assert(retry.records[0].sequence == 1 && retry.latest_sequence == 13);
    assert(vco_trace_snapshot(&ring, 13, &batch) == VCO_TRACE_OK);
    assert(batch.count == 1 && batch.records[0].sequence == 14);
    tail(&batch);
}
static void wrap(void)
{
    VcoTraceRing ring;
    vco_trace_init(&ring);
    for (unsigned i = 0; i < VCO_TRACE_CAPACITY + 2; ++i)
        append(&ring, i);
    VcoTraceBatch batch;
    assert(vco_trace_snapshot(&ring, 0, &batch) == VCO_TRACE_OK);
    assert(batch.oldest_sequence == 3 && batch.latest_sequence == 130 && !batch.dropped);
    assert(batch.count == 8 && batch.records[0].sequence == 3 && batch.records[7].sequence == 10);
    VcoTraceBatch retained = batch;
    assert(vco_trace_snapshot(&ring, 4, &batch) == VCO_TRACE_OK);
    assert(batch.records[0].sequence == 5 && batch.records[7].sequence == 12);
    assert(vco_trace_snapshot(&ring, 128, &batch) == VCO_TRACE_OK);
    assert(batch.count == 2 && batch.records[0].sequence == 129 &&
           batch.records[1].sequence == 130);
    tail(&batch);
    assert(vco_trace_snapshot(&ring, 130, &batch) == VCO_TRACE_OK);
    assert(!batch.count && batch.oldest_sequence == 3 && batch.latest_sequence == 130);
    tail(&batch);
    for (unsigned i = 0; i < VCO_TRACE_CAPACITY; ++i)
        append(&ring, i);
    assert(vco_trace_snapshot(&ring, 10, &batch) == VCO_TRACE_OK);
    assert(batch.oldest_sequence == 131 && batch.records[0].sequence == 131);
    assert(retained.records[0].sequence == 3 && retained.records[0].subject == 2);
}
static void limits(void)
{
    VcoTraceRing ring;
    vco_trace_init(&ring);
    vco_trace_init(NULL);
    VcoTraceBatch batch;
    memset(&batch, 0xa5, sizeof(batch));
    assert(vco_trace_snapshot(NULL, 0, &batch) == VCO_TRACE_INVALID);
    zero(&batch, sizeof(batch));
    assert(vco_trace_snapshot(&ring, 0, NULL) == VCO_TRACE_INVALID);
    assert(vco_trace_append(NULL, 0, 1, 0, 0, 0) == VCO_TRACE_INVALID);
    assert(vco_trace_append(&ring, 0, 0, 0, 0, 0) == VCO_TRACE_INVALID);
    assert(vco_trace_snapshot(&ring, 1, &batch) == VCO_TRACE_INVALID);
    zero(&batch, sizeof(batch));
    /* Single-thread fault setup: exercise the lifetime boundary without 2^64 writes. */
    ring.latest = UINT64_MAX - 1;
    assert(vco_trace_append(&ring, UINT64_MAX, UINT32_MAX, INT32_MIN, UINT32_MAX, UINT32_MAX) ==
           VCO_TRACE_OK);
    assert(vco_trace_append(&ring, 0, 1, 0, 0, 0) == VCO_TRACE_EXHAUSTED);
    assert(vco_trace_snapshot(&ring, UINT64_MAX - 1, &batch) == VCO_TRACE_OK);
    assert(batch.latest_sequence == UINT64_MAX && batch.oldest_sequence == UINT64_MAX &&
           batch.count == 1 && batch.dropped == 1);
    assert(batch.records[0].tick == UINT64_MAX && batch.records[0].result == INT32_MIN);
    tail(&batch);
    assert(vco_trace_snapshot(&ring, UINT64_MAX, &batch) == VCO_TRACE_OK && !batch.count);
    assert(batch.latest_sequence == UINT64_MAX && batch.dropped == 1);
    tail(&batch);
}
static void contention(void)
{
    VcoTraceRing ring = {0};
    VcoTraceBatch batch;
    __atomic_store_n(&ring.gate, 1, __ATOMIC_RELEASE);
    assert(vco_trace_append(&ring, 0, 1, 0, 0, 0) == VCO_TRACE_BUSY);
    assert(ring.dropped == 1 && !ring.latest && !ring.count);
    memset(&batch, 0xa5, sizeof(batch));
    assert(vco_trace_snapshot(&ring, 0, &batch) == VCO_TRACE_BUSY);
    zero(&batch, sizeof(batch));
    assert(ring.dropped == 1);
    __atomic_store_n(&ring.dropped, UINT32_MAX - 1, __ATOMIC_RELAXED);
    assert(vco_trace_append(&ring, 0, 1, 0, 0, 0) == VCO_TRACE_BUSY);
    assert(vco_trace_append(&ring, 0, 1, 0, 0, 0) == VCO_TRACE_BUSY);
    assert(ring.dropped == UINT32_MAX);
    __atomic_store_n(&ring.gate, 0, __ATOMIC_RELEASE);
    append(&ring, 0);
    assert(vco_trace_snapshot(&ring, 0, &batch) == VCO_TRACE_OK);
    assert(batch.dropped == UINT32_MAX && batch.records[0].sequence == 1);
}
static void format(void)
{
    VcoTraceRecord record = {UINT64_C(0x123456789abcdef0), UINT64_MAX,
                             UINT32_C(0x87654321),         INT32_MIN,
                             UINT32_C(0xfedcba98),         0};
    char tag[VCO_TRACE_TAG_SIZE + 1];
    memset(tag, 0xa5, sizeof(tag));
    assert(vco_trace_format(&record, tag, VCO_TRACE_TAG_SIZE) == 75);
    assert(!strcmp(tag,
                   "tr s=123456789abcdef0 t=ffffffffffffffff e=87654321 sub=fedcba98 d=00000000"));
    assert((unsigned char)tag[VCO_TRACE_TAG_SIZE] == 0xa5 && record.result == INT32_MIN);
    for (size_t size = 1; size < VCO_TRACE_TAG_SIZE; ++size) {
        memset(tag, 0xa5, sizeof(tag));
        assert(!vco_trace_format(&record, tag, size) && !tag[0]);
        assert((unsigned char)tag[size] == 0xa5);
    }
    tag[0] = 'z';
    assert(!vco_trace_format(&record, tag, 0) && tag[0] == 'z');
    assert(!vco_trace_format(&record, NULL, sizeof(tag)));
    assert(!vco_trace_format(NULL, tag, sizeof(tag)) && !tag[0]);
    record.sequence = 0;
    assert(!vco_trace_format(&record, tag, sizeof(tag)) && !tag[0]);
    record.sequence = 1;
    record.event = 0;
    assert(!vco_trace_format(&record, tag, sizeof(tag)) && !tag[0]);
}

enum { PRODUCERS = 4, ATTEMPTS = 10000 };
static VcoTraceRing shared;
static uint32_t finished;
static unsigned accepted[PRODUCERS];
static void *producer(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    for (unsigned i = 0; i < ATTEMPTS; ++i) {
        VcoTraceResult result =
            vco_trace_append(&shared, ((uint64_t)id << 32) | i, id + 1, -(int32_t)i - 1, id, i);
        assert(result == VCO_TRACE_OK || result == VCO_TRACE_BUSY);
        accepted[id] += result == VCO_TRACE_OK;
    }
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void concurrent(void)
{
    pthread_t producers[PRODUCERS];
    for (unsigned i = 0; i < PRODUCERS; ++i)
        assert(!pthread_create(&producers[i], NULL, producer, (void *)(uintptr_t)i));
    uint64_t cursor = 0;
    uint32_t dropped = 0;
    do {
        VcoTraceBatch batch;
        VcoTraceResult result = vco_trace_snapshot(&shared, cursor, &batch);
        assert(result == VCO_TRACE_OK || result == VCO_TRACE_BUSY);
        if (result == VCO_TRACE_BUSY) {
            zero(&batch, sizeof(batch));
            sched_yield();
            continue;
        }
        assert(batch.dropped >= dropped);
        dropped = batch.dropped;
        uint64_t first = cursor + 1;
        if (first < batch.oldest_sequence)
            first = batch.oldest_sequence;
        unsigned count = first > batch.latest_sequence ? 0
                         : batch.latest_sequence - first + 1 > VCO_TRACE_BATCH
                             ? VCO_TRACE_BATCH
                             : (unsigned)(batch.latest_sequence - first + 1);
        assert(batch.count == count);
        for (unsigned i = 0; i < batch.count; ++i) {
            const VcoTraceRecord *record = &batch.records[i];
            assert(record->sequence == first + i && record->subject < PRODUCERS &&
                   record->detail < ATTEMPTS && record->event == record->subject + 1);
            assert(record->tick == ((uint64_t)record->subject << 32 | record->detail));
            assert(record->result == -(int32_t)record->detail - 1);
            cursor = record->sequence;
        }
        tail(&batch);
    } while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) != PRODUCERS);
    unsigned count = 0;
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        assert(!pthread_join(producers[i], NULL));
        count += accepted[i];
    }
    VcoTraceBatch batch;
    assert(vco_trace_snapshot(&shared, cursor, &batch) == VCO_TRACE_OK);
    assert(batch.latest_sequence == count);
    assert(batch.dropped == UINT32_MAX || batch.dropped + count == PRODUCERS * ATTEMPTS);
}
static void freeze(void)
{
    VcoTraceRing ring;
    vco_trace_init(&ring);
    ring.gate = 1;
    assert(vco_trace_freeze(&ring, 0, 26, -37, 0x81002000, 0x804) == VCO_TRACE_BUSY);
    assert(!ring.frozen);
    ring.gate = 0;
    for (unsigned i = 0; i < VCO_TRACE_CAPACITY * 2; ++i)
        assert(vco_trace_append(&ring, i, 8, 1000, i, 4) == VCO_TRACE_OK);
    assert(vco_trace_freeze(&ring, 300, 26, -37, 0x81002000, 0x804) == VCO_TRACE_OK);
    VcoTraceBatch before, after;
    assert(!vco_trace_snapshot(&ring, 0, &before));
    for (unsigned i = 0; i < 1000; ++i)
        assert(vco_trace_append(&ring, 400, 12, 0, i, 4) == VCO_TRACE_FROZEN);
    assert(vco_trace_freeze(&ring, 999, 26, -1, 0x81003000, 4) == VCO_TRACE_FROZEN);
    assert(!vco_trace_snapshot(&ring, 0, &after) && !memcmp(&before, &after, sizeof(before)));
    assert(!vco_trace_snapshot(&ring, before.latest_sequence - 1, &after));
    assert(after.count == 1 && after.records[0].event == 26 && after.records[0].result == -37 &&
           after.records[0].subject == 0x81002000 && after.records[0].detail == 0x804);
    assert(!vco_trace_snapshot(&ring, before.latest_sequence, &after) && !after.count);
}
int main(int argc, char **argv)
{
    assert(argc == 2 && sizeof(VcoTraceRecord) == 32 && sizeof(VcoTraceBatch) == 288);
    if (!strcmp(argv[1], "snapshot"))
        snapshot();
    else if (!strcmp(argv[1], "wrap"))
        wrap();
    else if (!strcmp(argv[1], "limits"))
        limits();
    else if (!strcmp(argv[1], "contention"))
        contention();
    else if (!strcmp(argv[1], "format"))
        format();
    else if (!strcmp(argv[1], "concurrent"))
        concurrent();
    else if (!strcmp(argv[1], "freeze"))
        freeze();
    else
        assert(0);
    puts("trace test passed");
    return 0;
}
