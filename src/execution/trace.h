#ifndef VCO_TRACE_H
#define VCO_TRACE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VCO_TRACE_ABI UINT32_C(0x00010000)
#define VCO_TRACE_CAPACITY 128u
#define VCO_TRACE_BATCH 8u
#define VCO_TRACE_TAG_SIZE 76u /* 75 ASCII bytes plus NUL. */
typedef struct {
    uint64_t sequence, tick;
    uint32_t event;
    int32_t result;
    uint32_t subject, detail;
} VcoTraceRecord;
typedef struct {
    uint32_t abi, size;
    uint64_t oldest_sequence, latest_sequence;
    uint32_t dropped, count;
    VcoTraceRecord records[VCO_TRACE_BATCH];
} VcoTraceBatch;
typedef enum {
    VCO_TRACE_OK = 0,
    VCO_TRACE_BUSY = 1,
    VCO_TRACE_INVALID = 2,
    VCO_TRACE_EXHAUSTED = 3,
    VCO_TRACE_FROZEN = 4
} VcoTraceResult;
/* Caller-owned permanent storage. Static zero initialization is valid. Fields
 * are private to these functions; never reset while any caller can access it.
 * No allocation, SDK calls, clock lookup, I/O or waiting occurs here. */
typedef struct {
    uint32_t gate, dropped, head, count, frozen;
    uint64_t latest;
    VcoTraceRecord records[VCO_TRACE_CAPACITY];
} VcoTraceRing;
void vco_trace_init(VcoTraceRing *);
/* Event zero is invalid. Contention/exhaustion loses this record and increases
 * dropped. The counter saturates conservatively if its bounded atomic update
 * cannot complete; UINT32_MAX therefore means the exact count is unavailable.
 * Overwriting old records is separately visible through oldest_sequence. */
VcoTraceResult vco_trace_append(VcoTraceRing *, uint64_t tick, uint32_t event, int32_t result,
                                uint32_t subject, uint32_t detail);
/* Append a final diagnostic and freeze under the same gate. Later appends
 * return FROZEN without changing records/counters; snapshots remain available.
 * This freezes diagnostics only, never execution. Busy/exhausted does not freeze. */
VcoTraceResult vco_trace_freeze(VcoTraceRing *, uint64_t tick, uint32_t event, int32_t result,
                                uint32_t subject, uint32_t detail);
/* Non-consuming, copied snapshot of up to eight records after the cursor.
 * Old cursors resume at the retained oldest record. A cursor beyond latest is
 * invalid. Empty rings report oldest=latest=0. after==latest reports count=0
 * while retaining the current range. Every unused byte is zero; all errors
 * clear the complete output. out must not alias the ring. Snapshot contention
 * does not increment dropped. The drop counter may change during a snapshot. */
VcoTraceResult vco_trace_snapshot(VcoTraceRing *, uint64_t after, VcoTraceBatch *out);
/* Formats sequence/tick/event/subject/detail as a short ASCII diagnostic tag.
 * Pass (uint32_t)record.result separately as diagnostics_write's value. No
 * snprintf/locale dependency. Returns 75 on success, zero on invalid input or
 * insufficient capacity; clears out[0] when writable. Buffers must not alias. */
size_t vco_trace_format(const VcoTraceRecord *, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
