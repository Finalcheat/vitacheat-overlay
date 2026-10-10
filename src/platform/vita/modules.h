#ifndef VCO_VITA_MODULES_H
#define VCO_VITA_MODULES_H
#include "../../execution/memory.h"
#include "module_trace.h"
#define VCO_VITA_MODULE_LIMIT 256u
#define VCO_VITA_MODULE_SEGMENTS 2u        /* Original B code addresses segments 0/1 only. */
#define VCO_VITA_MODULE_RECORD_SEGMENTS 4u /* SDK ownership metadata covers every segment. */
typedef struct {
    uint32_t base, size, permissions;
} VcoVitaModuleSegment;
typedef struct {
    /* List(flags2=1)/GetModuleInfo input uses a kernel GUID; the returned
     * info.modid uses the target process's UID namespace. Retain both so a
     * process UID change still invalidates the sampled descriptor. */
    int32_t uid, process_uid;
    char name[28], path[256];
    uint32_t attributes, version, state;
    VcoVitaModuleSegment segments[VCO_VITA_MODULE_RECORD_SEGMENTS];
} VcoVitaModuleRecord;
/* Single-worker, kernel-owned provider; data/session/storage outlive it. Its
 * records are scratch until snapshot succeeds, then read-only to callers.
 * UID-sorted records are diagnostic order. enumeration retains the original
 * SDK order used by Z06's reversed B indices; neither may change in a snapshot.
 * Observed stability is NOT a module lifetime pin or an ABA guarantee. Actual
 * access must still validate/lock through the memory backend. No UI integration
 * or hardware-compatibility approval is supplied by this provider. */
typedef struct {
    const VcoMemoryAdapter *data;
    VcoVitaModuleRecord *records;
    uintptr_t list_function, info_function, main_function;
    VcoIdentity identity;
    uint64_t session, sequence, generation;
    int32_t enumeration[VCO_VITA_MODULE_LIMIT];
    uint16_t mapping[VCO_VITA_MODULE_LIMIT];
    unsigned capacity, count, mapping_count;
    int32_t main_uid;
    bool mapped;
    VcoVitaModuleTrace trace;
    void *trace_context;
    VcoVitaModuleFailure failure;
} VcoVitaModules;
/* Configure only from the owning worker. Initialization clears this hook;
 * attach it afterwards, including when init failed, to report that failure. */
static inline void vco_vita_modules_set_trace(VcoVitaModules *p, VcoVitaModuleTrace trace,
                                              void *context)
{
    if (!p)
        return;
    p->trace = trace;
    p->trace_context = context;
    if (trace && p->failure.event)
        trace(context, p->failure.event, p->failure.result, p->failure.subject, p->failure.detail);
}
/* Resolve firmware exports once. Missing List/Info fails closed. Main UID is
 * optional diagnostic metadata and is never automatically assigned to B200. */
int vco_vita_modules_init(VcoVitaModules *, const VcoMemoryAdapter *, VcoVitaModuleRecord *storage,
                          unsigned capacity);
void vco_vita_modules_invalidate(VcoVitaModules *);
/* Revokes any previous mapping first. On success, records[0..count) and main_uid
 * describe the sampled process. Caller output scalars stay unchanged on failure.
 * A full arena is accepted only when an extra UID slot proves no truncation.
 * Snapshot/mapping require retained-instance authorization, but may run while
 * background/UNKNOWN: they read metadata and cannot authorize execution. */
int vco_vita_modules_snapshot(VcoVitaModules *, uint64_t session, uint64_t *generation,
                              unsigned *count);
/* Production Z06 mapping: BXX selects enumeration[count - 1 - XX], confirmed
 * from the original implementation. Revalidates this exact snapshot/session
 * before publishing. Never rebuild mappings implicitly inside resolve(). */
int vco_vita_modules_map_z06(VcoVitaModules *, uint64_t session, uint64_t generation);
/* VcoEngineIdlePrepare-compatible. Caller must prove all-OFF and no retained
 * native patch resources. Refreshes a stale load-time snapshot at an explicit
 * manual enable boundary; never install this on a periodic/resume path.
 * SDK failure revokes mapping and stays available in provider diagnostics. */
void vco_vita_modules_prepare_idle(void *, uint64_t session);
/* Internal developer API: caller supplies an independently reviewed B-index to
 * UID mapping for this exact snapshot/session. It overrides the automatic map.
 * Zero leaves an index unmapped; nonzero UIDs must be unique members.
 * Every call revokes the old mapping, including failed replacement attempts. */
int vco_vita_modules_confirm_mapping(VcoVitaModules *, uint64_t session, uint64_t generation,
                                     const int32_t *uids, unsigned count);
/* VcoCheatMemory.module-compatible. Only indices 0..255 and segments 0/1 exist.
 * Requires foreground authority before and after metadata verification. Both
 * outputs stay unchanged on failure. No writes or patch injection occur. */
int vco_vita_modules_resolve(void *, uint64_t session, unsigned index, unsigned segment,
                             uint32_t *base, uint32_t *size);
/* Patch ownership guard, using retained-instance (not foreground) authority.
 * *generation==0 captures membership of this range in the current snapshot;
 * a module range returns its generation, a non-module range stays zero after
 * verifying every original module record (unchanged UIDs alone cannot prove
 * that no module segment moved into this range).
 * A nonzero input verifies that exact generation and original module record.
 * Changed/invalidated snapshots deny restoration rather than writing bytes
 * into a reused module range. Outputs are unchanged on failure. This does not
 * pin modules or prove heap/suballocation identity between observations. */
int vco_vita_modules_guard(void *, uint64_t session, uint32_t address, uint32_t size,
                           uint64_t *generation);
#endif
