#ifndef VCO_VITA_PROCESS_EVENTS_H
#define VCO_VITA_PROCESS_EVENTS_H
#include <stdint.h>

#define VCO_PROCESS_EVENTS_CAPACITY 32u
#define VCO_PROCESS_EVENTS_LOST 0x80000000u
#define VCO_PROCESS_EVENTS_GENERATION_MASK 0x7fffffffu
typedef enum {
    VCO_PROCESS_EVENTS_NEW,
    VCO_PROCESS_EVENTS_STARTING,
    VCO_PROCESS_EVENTS_ACTIVE,
    VCO_PROCESS_EVENTS_STOPPING,
    VCO_PROCESS_EVENTS_STOPPED,
    VCO_PROCESS_EVENTS_REGISTER_FAILED,
    VCO_PROCESS_EVENTS_UNREGISTER_FAILED
} VcoProcessEventsPhase;
typedef enum {
    VCO_PROCESS_EVENT_CREATE = 1,
    VCO_PROCESS_EVENT_EXIT,
    VCO_PROCESS_EVENT_KILL,
    VCO_PROCESS_EVENT_STOP,
    VCO_PROCESS_EVENT_START,
    VCO_PROCESS_EVENT_SWITCH
} VcoProcessEventKind;
enum {
    VCO_PROCESS_EVENT_PARAM_PRESENT = 1u,
    VCO_PROCESS_EVENT_PARAM_PID = 2u,
    VCO_PROCESS_EVENT_PARAM_INVALID = 4u,
    VCO_PROCESS_EVENT_TRANSITION = 8u,
    VCO_PROCESS_EVENT_LATE = 16u
};
/* 32-byte evidence record. event_id is supplied only for SWITCH; event_type
 * only for STOP/START/SWITCH. No inferred meaning for undocumented parameters.
 * pid is the callback PID, or documented parameter PID for SWITCH, else -1. */
typedef struct {
    uint32_t invalidation, kind;
    int32_t pid, event_id, event_type;
    uint32_t parameter_size;
    int32_t parameter_pid;
    uint32_t flags;
} VcoProcessEvent;
typedef struct {
    uint32_t phase, invalidation, late_callback_seen;
    int32_t native_result;
} VcoProcessEventsState;
typedef enum {
    VCO_PROCESS_EVENTS_OK,
    VCO_PROCESS_EVENTS_EMPTY,
    VCO_PROCESS_EVENTS_BUSY,
    VCO_PROCESS_EVENTS_ARGUMENT
} VcoProcessEventsPoll;

/* One static instance, one registration attempt, at most one unregister
 * attempt. Return 0 on success, negative native error on SDK failure, -1 for
 * an invalid lifecycle operation. Failure details remain in state().
 * start/stop invoke SDK and are NOT callback-safe. They do not reset evidence.
 * After any registration attempt, keep this code and static storage resident;
 * successful unregister does not prove callback quiescence. No unload/reset
 * or re-registration API is provided. */
int vco_process_events_start(void);
int vco_process_events_stop(void);
/* Nonblocking dequeue, one worker consumer. BUSY/EMPTY do not alter *out. */
VcoProcessEventsPoll vco_process_events_poll(VcoProcessEvent *out);
/* Lock-free revocation evidence. Every callback invalidates before inspecting
 * metadata/queue. LOST is permanent and never cleared by dequeue or stop.
 * A changed value or LOST must revoke candidates; unchanged is NOT authority. */
uint32_t vco_process_events_invalidation(void);
/* One worker-owned target watch, separate from the lossy diagnostic queue.
 * Arm only against a fresh kernel epoch, while retaining the process object.
 * Returns 0 on success; failure acquires nothing. No rearm until unwatch.
 * A terminal callback revokes before queue access; allow() never grants
 * authority or proves retirement. It cannot drain an already running write.
 * Delayed callbacks across a same-PID rebind may conservatively revoke the new
 * watch; they must not be used as proof that the new instance has exited. */
int vco_process_events_watch(int32_t pid, uint32_t epoch);
void vco_process_events_unwatch(int32_t pid);
int vco_process_events_allow(int32_t pid);
/* Individually atomic diagnostic fields, not a transaction across fields.
 * Callers must not use ACTIVE or a stable generation to grant execution. */
void vco_process_events_state(VcoProcessEventsState *out);
#endif
