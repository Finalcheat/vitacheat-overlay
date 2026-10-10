#ifndef VCO_ENGINE_H
#define VCO_ENGINE_H
#include "session.h"
#include "close_key.h"
#include "limits.h"
#include "../cheats/cheats.h"
/* Values are part of control ABI v1; append only or bump its ABI. */
typedef enum {
    VCO_ENTRY_OFF = 0,
    VCO_ENTRY_ENABLING = 1,
    VCO_ENTRY_ON = 2,
    VCO_ENTRY_DISABLING = 3,
    VCO_ENTRY_PAUSED = 4,
    VCO_ENTRY_ERROR = 5,
    VCO_ENTRY_UNSUPPORTED = 6
} VcoEntryState;
typedef struct {
    uint64_t request, session, revision;
    uint32_t entry;
    bool enabled;
} VcoExecutionCommand;
typedef struct {
    VcoEntryState state;
    VcoDiagnostic diagnostic;
    uint64_t acknowledged, pending;
    bool desired;
} VcoExecutionRow;
struct VcoEngine;
/* Optional worker-owned metadata preparation before a manual ON command when
 * every entry is inactive and no patch/claim remains. Never called on ticks,
 * background resume or another ON while intent/resources are retained.
 * May refresh external metadata only; no writes, reentry or engine mutation.
 * Failure stays in the provider: ordinary data codes need not depend on it. */
typedef void (*VcoEngineIdlePrepare)(void *, uint64_t session);
typedef struct {
    VcoCheatProgram program;
    VcoCheatRuntime runtime;
    struct VcoEngine *engine;
    unsigned index;
    uint64_t last_request;
    bool last_desired;
    VcoError access_error;
} VcoExecutionSlot;
typedef struct {
    uint32_t address;
    uint16_t entry;
    /* One byte keeps the 8-byte record size. Data is refreshed before a new
     * execution; patches persist until successful native release. */
    uint8_t width, flags;
} VcoWriteClaim;
/* Runs after all published runtimes have stopped, even if one failed. Must
 * account for private/unpublished ownership too, preserve ambiguous receipts,
 * and never retry a consumed native release. NULL asserts no private owner.
 * No callback may reenter the engine or change its buffers. */
typedef VcoError (*VcoEngineCloseNative)(void *);
typedef struct VcoEngine {
    const VcoSession *session;
    VcoIdentity identity; /* immutable identity captured at bind */
    VcoCheatMemory memory;
    uint64_t token, revision, last_revision, last_tick;
    uint64_t last_close_transaction;
    unsigned count, head, queued;
    bool bound, scheduled, blocked_restore, closing, close_complete;
    VcoEngineCloseKey close_key;
    VcoEngineCloseResult close_result;
    VcoEngineCloseNative close_native;
    void *close_native_context;
    VcoEngineIdlePrepare idle_prepare;
    void *idle_prepare_context;
    VcoExecutionRow rows[VCO_ENGINE_ENTRIES];
    VcoExecutionSlot slots[VCO_ENGINE_ENTRIES];
    VcoExecutionCommand commands[VCO_ENGINE_QUEUE];
    VcoWriteClaim claims[VCO_ENGINE_CLAIMS];
} VcoEngine;
/* Single worker owns all functions; callers synchronize command/snapshot handoff.
 * Compiled ops and patches are caller arenas. Parsed codes remain immutable/live
 * until unbind; no borrowed UI or syscall pointers may enter these arenas. */
void vco_engine_init(VcoEngine *engine);
VcoError vco_engine_bind(VcoEngine *engine, const VcoSession *session, uint64_t revision,
                         const VcoCheatFile *file, VcoCheatMemory memory, VcoCheatOp *ops,
                         VcoPatch *patches, size_t capacity);
VcoError vco_engine_submit(VcoEngine *engine, VcoExecutionCommand command);
/* Configure after bind and before accepting commands; an active/queued engine
 * rejects changes. unbind/bind clears the hook and its borrowed context. */
VcoError vco_engine_set_idle_prepare(VcoEngine *, VcoEngineIdlePrepare, void *);
void vco_engine_step(VcoEngine *engine, uint64_t now_us);
/* Single owning worker, between steps; these are NOT cross-thread entry points.
 * Close revokes execution before callbacks, cancels queued intent, preserves
 * the compiled list and caches a result for this exact key/provider. Validation
 * errors leave result untouched. Duplicate close never invokes cleanup again.
 * A failed close cannot reopen; its caller must retain native ownership/arenas.
 * Successful close is not proof that a firmware destruction boundary may pass. */
VcoError vco_engine_close(VcoEngine *, const VcoEngineCloseKey *, VcoEngineCloseNative, void *,
                          VcoEngineCloseResult *);
/* Caller independently validates native lifetime and rearms its empty adapter
 * first. Reopen requires a newer revision and starts every supported row OFF;
 * old requests remain stale. It neither restores bytes nor grants SDK authority. */
VcoError vco_engine_reopen(VcoEngine *, const VcoEngineCloseKey *, uint64_t revision);
VcoError vco_engine_unbind(VcoEngine *engine);
#endif
