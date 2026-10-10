#ifndef VCO_VITA_MEMORY_H
#define VCO_VITA_MEMORY_H
#include "../../execution/memory.h"
#include "memory_trace.h"
/* Kernel-only, single-worker state. Initialize once, then bind/unbind; never
 * reset an object that owns references. No pointer in this type crosses a syscall.
 * Authority must verify title/liveness and (when requested) foreground status.
 * A catalog snapshot is NOT sufficient authority. Missing authority denies bind. */
typedef int (*VcoVitaAuthority)(void *, const VcoIdentity *, bool foreground);
typedef struct {
    VcoIdentity identity;
    uint64_t sequence;
    void *process_class, *process_object;
    int (*get_block_type)(int32_t, unsigned *);
    void *authority_context;
    VcoVitaAuthority authority;
    VcoVitaMemoryTrace trace;
    void *trace_context;
    uint32_t address, length;
    int32_t block;
    int cleanup_error;
    bool bound, locked, poisoned;
} VcoVitaMemory;
void vco_vita_memory_init(VcoVitaMemory *memory, VcoVitaAuthority authority, void *context);
/* Configure before binding. Callback/context survive all bindings. */
int vco_vita_memory_set_trace(VcoVitaMemory *, VcoVitaMemoryTrace, void *);
/* output is assigned only on success; instance is generated, never supplied. */
int vco_vita_memory_bind(VcoVitaMemory *memory, int32_t pid, const char title_id[10],
                         VcoIdentity *output);
int vco_vita_memory_unbind(VcoVitaMemory *memory);
VcoMemoryTransport vco_vita_memory_transport(VcoVitaMemory *memory);
#endif
