#ifndef VCO_GAME_WORKER_H
#define VCO_GAME_WORKER_H
#include "executor.h"
#define VCO_GAME_WORKER_PERIOD_US 10000u
enum {
    VCO_GAME_WORKER_ARGUMENT = -100,
    VCO_GAME_WORKER_STATE = -101,
    VCO_GAME_WORKER_SESSION = -102,
    VCO_GAME_WORKER_PROTOCOL = -103,
    VCO_GAME_WORKER_CLEANUP = -104,
    VCO_GAME_WORKER_CLOCK = -105
};
typedef struct {
    void *context;
    VcoAgentStatus (*poll)(void *, uint64_t, VcoAgentMessage *, void *, size_t);
    VcoAgentStatus (*publish)(void *, const VcoAgentResult *);
    VcoAgentStatus (*fault)(void *, uint64_t, VcoError); /* optional fatal receipt */
} VcoGameChannel;
/* Game-owned state/buffers, never borrowed from Shell or kernel. Initialize
 * once with no live thread. stop is called by a different user thread. */
typedef struct {
    VcoGameExecutor *executor;
    VcoGameChannel channel;
    void *payload;
    size_t capacity;
    int32_t thread;
    uint32_t stop_requested, completed;
    bool started, joined;
    int run_error;
    VcoError cleanup;
} VcoGameWorker;
void vco_game_worker_init(VcoGameWorker *);
int vco_game_worker_start(VcoGameWorker *, VcoGameExecutor *, VcoGameChannel, void *, size_t);
void vco_game_worker_request_stop(VcoGameWorker *);
/* Successful WaitThreadEnd precedes DeleteThread. Timeout preserves ownership
 * and permits another join. Nonzero cleanup prohibits unloading the module. */
int vco_game_worker_stop(VcoGameWorker *, uint32_t timeout_us);
#endif
