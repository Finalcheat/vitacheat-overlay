#ifndef VCO_VITA_MEMORY_TRACE_H
#define VCO_VITA_MEMORY_TRACE_H
#include <stdint.h>
/* Worker-owned failure diagnostics; no kernel pointers or successful transfers.
 * Callback must not do I/O, block, or reenter/mutate execution. */
typedef void (*VcoVitaMemoryTrace)(void *, uint32_t, int32_t, uint32_t, uint32_t);
enum {
    /* result=VcoMemoryFailure; subject=user address; detail=byte width. */
    VCO_MEMORY_TRACE_READ_FAILURE = 32,
    VCO_MEMORY_TRACE_WRITE_FAILURE,
    /* Raw SDK return; subject=user address; detail=width or memblock UID. */
    VCO_MEMORY_TRACE_LOCK_FAILURE,
    VCO_MEMORY_TRACE_FIND_FAILURE,
    VCO_MEMORY_TRACE_BLOCK_CHANGED,
    VCO_MEMORY_TRACE_TYPE_FAILURE,
    /* result=-1; subject=user address; detail=observed type. */
    VCO_MEMORY_TRACE_TYPE_DENIED,
    VCO_MEMORY_TRACE_COPY_READ_FAILURE,
    VCO_MEMORY_TRACE_COPY_WRITE_FAILURE,
    VCO_MEMORY_TRACE_UNLOCK_FAILURE,
    /* Raw SDK/authority result; subject=PID; detail=foreground requirement. */
    VCO_MEMORY_TRACE_PROCESS_REFER_FAILURE,
    VCO_MEMORY_TRACE_PROCESS_CHANGED,
    VCO_MEMORY_TRACE_PROCESS_RELEASE_FAILURE,
    VCO_MEMORY_TRACE_AUTHORITY_FAILURE
};
#endif
