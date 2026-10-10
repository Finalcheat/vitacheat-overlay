#include "trace.h"
#include <string.h>

#if !defined(__GCC_ATOMIC_INT_LOCK_FREE) || __GCC_ATOMIC_INT_LOCK_FREE != 2
#error "Trace requires lock-free 32-bit integer atomics"
#endif
typedef char record_size_check[sizeof(VcoTraceRecord) == 32 ? 1 : -1];
typedef char batch_size_check[sizeof(VcoTraceBatch) == 288 ? 1 : -1];

static int enter(VcoTraceRing *ring)
{
    uint32_t free_gate = 0;
    return __atomic_compare_exchange_n(&ring->gate, &free_gate, 1, 0, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}
static void leave(VcoTraceRing *ring)
{
    __atomic_store_n(&ring->gate, 0, __ATOMIC_RELEASE);
}
static void drop(VcoTraceRing *ring)
{
    uint32_t old = __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (old == UINT32_MAX)
            return;
        if (__atomic_compare_exchange_n(&ring->dropped, &old, old + 1, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return;
    }
    /* Recording a diagnostic must not spin behind producers of diagnostics. */
    __atomic_store_n(&ring->dropped, UINT32_MAX, __ATOMIC_RELAXED);
}
void vco_trace_init(VcoTraceRing *ring)
{
    if (ring)
        memset(ring, 0, sizeof(*ring));
}
static VcoTraceResult append(VcoTraceRing *ring, uint64_t tick, uint32_t event, int32_t result,
                             uint32_t subject, uint32_t detail, int freeze)
{
    if (!ring || !event)
        return VCO_TRACE_INVALID;
    if (__atomic_load_n(&ring->frozen, __ATOMIC_ACQUIRE))
        return VCO_TRACE_FROZEN;
    if (!enter(ring)) {
        drop(ring);
        return VCO_TRACE_BUSY;
    }
    if (__atomic_load_n(&ring->frozen, __ATOMIC_RELAXED)) {
        leave(ring);
        return VCO_TRACE_FROZEN;
    }
    if (ring->latest == UINT64_MAX) {
        drop(ring);
        leave(ring);
        return VCO_TRACE_EXHAUSTED;
    }
    unsigned index = (ring->head + ring->count) % VCO_TRACE_CAPACITY;
    if (ring->count == VCO_TRACE_CAPACITY)
        ring->head = (ring->head + 1) % VCO_TRACE_CAPACITY;
    else
        ++ring->count;
    ring->records[index] = (VcoTraceRecord){++ring->latest, tick, event, result, subject, detail};
    if (freeze)
        __atomic_store_n(&ring->frozen, 1u, __ATOMIC_RELEASE);
    leave(ring);
    return VCO_TRACE_OK;
}
VcoTraceResult vco_trace_append(VcoTraceRing *ring, uint64_t tick, uint32_t event, int32_t result,
                                uint32_t subject, uint32_t detail)
{
    return append(ring, tick, event, result, subject, detail, 0);
}
VcoTraceResult vco_trace_freeze(VcoTraceRing *ring, uint64_t tick, uint32_t event, int32_t result,
                                uint32_t subject, uint32_t detail)
{
    return append(ring, tick, event, result, subject, detail, 1);
}
VcoTraceResult vco_trace_snapshot(VcoTraceRing *ring, uint64_t after, VcoTraceBatch *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!ring || !out)
        return VCO_TRACE_INVALID;
    if (!enter(ring))
        return VCO_TRACE_BUSY;
    if (after > ring->latest) {
        leave(ring);
        return VCO_TRACE_INVALID;
    }
    out->abi = VCO_TRACE_ABI;
    out->size = sizeof(*out);
    out->latest_sequence = ring->latest;
    out->oldest_sequence = ring->count ? ring->records[ring->head].sequence : 0;
    out->dropped = __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < ring->count && out->count < VCO_TRACE_BATCH; ++i) {
        const VcoTraceRecord *record = &ring->records[(ring->head + i) % VCO_TRACE_CAPACITY];
        if (record->sequence > after)
            out->records[out->count++] = *record;
    }
    leave(ring);
    return VCO_TRACE_OK;
}
static char *literal(char *out, const char *text)
{
    while (*text)
        *out++ = *text++;
    return out;
}
static char *hex(char *out, uint64_t value, unsigned digits)
{
    static const char alphabet[] = "0123456789abcdef";
    while (digits) {
        --digits;
        *out++ = alphabet[(value >> (digits * 4)) & 15];
    }
    return out;
}
size_t vco_trace_format(const VcoTraceRecord *record, char *out, size_t capacity)
{
    if (out && capacity)
        *out = 0;
    if (!record || !record->sequence || !record->event || !out || capacity < VCO_TRACE_TAG_SIZE)
        return 0;
    char *p = literal(out, "tr s=");
    p = hex(p, record->sequence, 16);
    p = hex(literal(p, " t="), record->tick, 16);
    p = hex(literal(p, " e="), record->event, 8);
    p = hex(literal(p, " sub="), record->subject, 8);
    p = hex(literal(p, " d="), record->detail, 8);
    *p = 0;
    return (size_t)(p - out);
}
