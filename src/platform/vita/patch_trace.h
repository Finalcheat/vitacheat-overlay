#ifndef VCO_VITA_PATCH_TRACE_H
#define VCO_VITA_PATCH_TRACE_H
#include <stdint.h>
/* Optional worker-side diagnostics. The callback must return promptly, perform
 * no I/O and never reenter/mutate this backend. Its context outlives support and
 * every executor. Results preserve native signed return values. Subjects and
 * details are numeric UIDs/keys/flags; no kernel object address is published. */
typedef void (*VcoVitaPatchTrace)(void *context, uint32_t event, int32_t result, uint32_t subject,
                                  uint32_t detail);
/* Stable event numbers reserved in 1..31. Unless stated otherwise subject is
 * the native UID and detail is the public patch key. No event is emitted for
 * ordinary memory operations or a repeated successful live-UID retirement poll. */
typedef enum {
    VCO_PATCH_TRACE_PROBE_CREATE = 1,       /* result=UID/error; subject/detail=0 */
    VCO_PATCH_TRACE_PROBE_REFERENCE = 2,    /* detail=object present (0/1) */
    VCO_PATCH_TRACE_PROBE_DELETE = 3,       /* detail=0 */
    VCO_PATCH_TRACE_PROBE_CLOSED_QUERY = 4, /* detail=query index (0/1) */
    VCO_PATCH_TRACE_PROBE_RELEASE = 5,      /* detail=0 temporary, 1 retained */
    /* result=0 ready/-1 unavailable; subject=observed closed_error bit pattern;
     * detail bits: 1 ready, 2 quarantined, 4 referenced, 8 UID still retained. */
    VCO_PATCH_TRACE_PROBE_RESULT = 6,
    VCO_PATCH_TRACE_BLOCK_REFERENCE = 7, /* detail=patch width */
    VCO_PATCH_TRACE_INJECT = 8,          /* result=UID/error; subject=key, detail=width */
    VCO_PATCH_TRACE_PATCH_REFERENCE = 9,
    /* Negative query failure or live UID/object mismatch; never means closed.
     * A verified exact closed error uses NATIVE_CLOSED instead. */
    VCO_PATCH_TRACE_NATIVE_QUERY_FAILURE = 10,
    VCO_PATCH_TRACE_NATIVE_CLOSED = 11,
    VCO_PATCH_TRACE_NATIVE_RELEASE = 12, /* raw taiInjectRelease return */
    VCO_PATCH_TRACE_PATCH_RELEASE_REFERENCE = 13,
    VCO_PATCH_TRACE_BLOCK_RELEASE_REFERENCE = 14,
    VCO_PATCH_TRACE_BLOCK_MAPPING_FAILURE = 15, /* result=found UID/error */
    VCO_PATCH_TRACE_BLOCK_QUERY = 16,           /* raw GUID refer; failed/mismatched or retired */
    VCO_PATCH_TRACE_TEMP_RELEASE_FAILURE = 17,
    VCO_PATCH_TRACE_RESTORE_LOCK = 18,   /* subject=key, detail=width */
    VCO_PATCH_TRACE_RESTORE_UNLOCK = 19, /* subject=key, detail=width */
    VCO_PATCH_TRACE_QUARANTINE = 20,     /* subject=key, detail=VcoVitaPatchTraceReason */
    VCO_PATCH_TRACE_POISON = 21,         /* subject=key, detail=0 */
    VCO_PATCH_TRACE_RETIRED_CLEARED = 22,
    VCO_PATCH_TRACE_INJECT_UNLOCK = 23,  /* subject=key, detail=width */
    VCO_PATCH_TRACE_BLOCKED = 24,        /* subject=key, detail=VcoVitaPatchTraceReason */
    VCO_PATCH_TRACE_UNPUBLISHED_CLEANUP, /* raw background cleanup result, subject=UID, detail=key
                                          */
    /* First failing predicate/call, not the generic inject callback return.
     * subject=user target address; detail=stage << 8 | width. Expected SESSION
     * denials do not emit this record. No ownership or SDK behavior changes. */
    VCO_PATCH_TRACE_INJECT_FAILURE
} VcoVitaPatchTraceEvent;
typedef enum {
    VCO_PATCH_STAGE_ARGUMENT = 1,
    VCO_PATCH_STAGE_AUTHORITY,
    VCO_PATCH_STAGE_OVERLAP,
    VCO_PATCH_STAGE_CAPACITY,
    VCO_PATCH_STAGE_MODULE_CAPTURE,
    VCO_PATCH_STAGE_LOCK,
    VCO_PATCH_STAGE_INSPECT,
    VCO_PATCH_STAGE_READ,
    VCO_PATCH_STAGE_BLOCK_REFERENCE,
    VCO_PATCH_STAGE_BLOCK_MAPPING,
    VCO_PATCH_STAGE_MODULE_INJECT,
    VCO_PATCH_STAGE_TAI_INJECT,
    VCO_PATCH_STAGE_PATCH_REFERENCE,
    VCO_PATCH_STAGE_MODULE_CONFIRM,
    VCO_PATCH_STAGE_UID_CONFIRM,
    VCO_PATCH_STAGE_UNLOCK,
    VCO_PATCH_STAGE_MODULE_PUBLICATION,
    VCO_PATCH_STAGE_UID_PUBLICATION,
    VCO_PATCH_STAGE_BLOCK_PUBLICATION,
    VCO_PATCH_STAGE_PRIVATE_CLEANUP,
    VCO_PATCH_STAGE_ADMISSION,
    VCO_PATCH_STAGE_PERMISSION
} VcoVitaPatchFailureStage;
typedef enum {
    VCO_PATCH_TRACE_REASON_UNCERTAIN = 1,
    VCO_PATCH_TRACE_REASON_RESTORE_GUARD = 2,
    VCO_PATCH_TRACE_REASON_RESTORE_FAILED = 3,
    VCO_PATCH_TRACE_REASON_RETIRED = 4,
    VCO_PATCH_TRACE_REASON_INJECT_UNLOCK = 5,
    VCO_PATCH_TRACE_REASON_INJECT_AUTHORITY = 6,
    VCO_PATCH_TRACE_REASON_INJECT_GUARD = 7
} VcoVitaPatchTraceReason;
#endif
