#ifndef VCO_AGENT_EXECUTOR_H
#define VCO_AGENT_EXECUTOR_H
#include "protocol.h"
typedef struct {
    void *context;
    void *(*allocate)(void *, size_t);
    void (*release)(void *, void *);
} VcoAgentAllocator;
/* One game worker owns this object and its arenas. Process death does not call
 * stop: the OS reclaims user storage and taiHEN cleans target-PID patches. */
typedef struct {
    VcoAgentBinding binding;
    VcoSession session;
    VcoEngine engine;
    VcoAgentAllocator allocator;
    VcoCheatMemory memory;
    VcoEngineCloseNative finalize;
    void *finalize_context;
    VcoEngineIdlePrepare prepare;
    void *prepare_context;
    void *arena;
    VcoCheatFile file;
    VcoUploadHeader upload;
    VcoAgentResult last_result;
    uint64_t last_ticket, revision;
    bool revoked, stopped;
} VcoGameExecutor;
VcoError vco_game_executor_init(VcoGameExecutor *, const VcoAgentBinding *, VcoAgentAllocator,
                                VcoCheatMemory, VcoEngineCloseNative, void *finalize_context);
/* Configure before the first LOAD, from the owning thread. Reinstalled after
 * each successful bind; never called on periodic ticks or resumed execution. */
VcoError vco_game_executor_set_prepare(VcoGameExecutor *, VcoEngineIdlePrepare, void *);
/* Only a freshly authenticated poll may advance runnable/tick. A transport
 * failure calls revoke instead; it must never run the core's restore path. */
VcoError vco_game_executor_handle(VcoGameExecutor *, const VcoAgentMessage *, const void *, size_t,
                                  uint64_t now_us, VcoAgentResult *);
void vco_game_executor_revoke(VcoGameExecutor *);
/* Explicit module unload in a live game only. On cleanup failure keep arenas
 * and refuse unloading. Do not use this as a process-termination callback. */
VcoError vco_game_executor_stop(VcoGameExecutor *);
/* Copy an all-OFF receipt only after successful stop. It contains no list,
 * arena or patch pointer, and never authorizes freeing a live thread. */
VcoError vco_game_executor_stopped_result(const VcoGameExecutor *, VcoAgentResult *);
#endif
