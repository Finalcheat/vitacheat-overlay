#ifndef VCO_VITA_PATCHES_H
#define VCO_VITA_PATCHES_H
#include "memory.h"
#include "../../execution/admission.h"
#include "../../execution/limits.h"
#include "patch_trace.h"
/* One native receipt per range admitted by the engine's protection table.
 * Fixed worker-owned storage keeps quarantine and teardown ownership unchanged. */
#define VCO_VITA_PATCH_LIMIT VCO_ENGINE_CLAIMS
/* Initialize/probe once on the startup worker, before any executor is created.
 * A successful probe is immutable and outlives its executors. Failed native
 * cleanup is retained here; never overwrite/reinitialize an attempted probe. */
typedef struct {
    int32_t probe_uid, closed_error;
    void *probe_object;
    VcoVitaPatchTrace trace;
    void *trace_context;
    bool attempted, ready, referenced, quarantined;
} VcoVitaPatchSupport;
void vco_vita_patch_support_init(VcoVitaPatchSupport *);
/* Worker-owned configuration only before the first probe attempt. NULL clears
 * the callback; no executor copies it, so this immutable support must outlive
 * all executors. Rejected changes leave both callback and context untouched. */
int vco_vita_patch_support_set_trace(VcoVitaPatchSupport *, VcoVitaPatchTrace, void *);
int vco_vita_patch_support_probe(VcoVitaPatchSupport *);
/* No native mutation/retry: reports whether startup resources were released. */
int vco_vita_patch_support_dispose(const VcoVitaPatchSupport *);
typedef struct {
    int32_t key, tai_handle, memblock;
    uint32_t address, size;
    uint64_t session, module_generation;
    void *object, *block_object;
    bool active, quarantined, referenced, block_referenced;
} VcoVitaPatch;
/* Optional module/input providers. Both contexts remain worker-owned and live
 * until the execution backend is no longer referenced. They must independently
 * validate their source; the wrapper also checks this backend's session. */
typedef struct {
    void *module_context;
    int (*module)(void *, uint64_t, unsigned, unsigned, uint32_t *, uint32_t *);
    void *pad_context;
    int (*pad)(void *, uint64_t, uint32_t *, uint32_t *);
    /* Optional module ownership stamp, captured before injection and checked
     * again after injection and on restoration, including background cleanup.
     * Zero input captures a stamp; nonzero input must verify the exact original
     * module snapshot. The executor separately requires that every check after
     * capture return the same value, including zero (sampled non-module range). */
    int (*module_guard)(void *, uint64_t, uint32_t, uint32_t, uint64_t *);
} VcoVitaExecutionFeatures;
typedef struct {
    VcoMemoryAdapter data;
    VcoAdmission *admission;
    VcoVitaExecutionFeatures features;
    const VcoVitaPatchSupport *support;
    VcoIdentity identity;
    VcoVitaPatch patches[VCO_VITA_PATCH_LIMIT];
    int32_t sequence;
    bool blocked, sealed, poisoned, retired;
} VcoVitaExecution;
/* One executor per bound memory owner, initialized once before any injection.
 * Do not unbind/reinitialize native memory until dispose succeeds. */
void vco_vita_execution_init(VcoVitaExecution *, VcoVitaMemory *, const VcoSession *,
                             const VcoVitaPatchSupport *);
/* Optional externally owned memory-operation gate, immutable after callbacks().
 * Gate storage must outlive the executor and all closing observers. A successful
 * admission spans each data read/write and inject/restore, including handoff and
 * range unlock. Closing does not cancel already admitted work. No wait, cleanup
 * owner election, resource pin, or firmware hook is supplied by this adapter.
 * A denied restore leaves ownership intact for a separately authorized cleanup
 * path; dispose cannot bypass a closed gate. rearm does not reopen the gate.
 * NULL retains the existing ungated behavior. Metadata/pad providers are not
 * covered; this gate alone is not permission to destroy process resources. */
int vco_vita_execution_set_admission(VcoVitaExecution *, VcoAdmission *);
/* Configure before the first callbacks() call. Later changes are rejected;
 * capabilities must not change underneath a bound compiled list. */
int vco_vita_execution_set_features(VcoVitaExecution *, VcoVitaExecutionFeatures);
VcoCheatMemory vco_vita_execution_callbacks(VcoVitaExecution *);
/* Restores active handles once. Ambiguous failures retain quarantine records,
 * permanently block further execution and forbid pretending teardown succeeded. */
int vco_vita_execution_dispose(VcoVitaExecution *);
/* Worker-only, readonly summary. No SDK calls, cleanup or authorization.
 * RESTORE means native ownership remains uncertain or blocked with resources;
 * PATCH means an empty live executor is blocked until explicit clear/rearm.
 * A successfully reaped retired binding reports OK, never a false new fault. */
VcoError vco_vita_execution_error(const VcoVitaExecution *);
/* Worker-only, after successful dispose and engine unbind. Reopens an empty,
 * non-poisoned executor for an explicit all-OFF list bind to the same retained
 * process. Rejects an observed closed patch gate; success is not admission.
 * Never clears native quarantine or grants foreground authority.
 * Features, sealed capability set and monotonically increasing keys survive. */
int vco_vita_execution_rearm(VcoVitaExecution *);
/* Trusted worker only, after independent authority proves this exact binding
 * retired. Never infer retirement from an unknown/background sample. Does not
 * call taiInjectRelease or access former process addresses. Returns 0 when all
 * native UIDs are proven closed and retained references released, 1 while an
 * original UID still exists (bounded polling is allowed), -1 on ambiguity.
 * After 0 the service may retire the old engine's RESTORE latch; this does not
 * forgive failed arena/memory/authority cleanup or authorize a new session. */
int vco_vita_execution_reap_retired(VcoVitaExecution *, const VcoIdentity *);
#endif
