#ifndef TEST_SERVICE_FAKES_H
#define TEST_SERVICE_FAKES_H
#include "execution/service_protocol.h"
#include "platform/vita/process_events.h"
#define TEST_SHELL 88
#define TEST_GAME 42
#define TEST_BASE UINT32_C(0x81000000)
#define TEST_USER(offset) ((void *)(uintptr_t)(TEST_BASE + (offset)))
#define TEST_PACKET 0u
#define TEST_SAMPLE 150000u
#define TEST_STATUS 151000u
#define TEST_REQUEST 152000u
#define TEST_REPLY 153000u
#define TEST_TICKET 154000u
typedef struct {
    unsigned char user[160000];
    int32_t caller, shell_pid;
    int64_t now;
    unsigned depth, entered, exited, from_calls, to_calls, allocations, frees;
    unsigned live_blocks, writes, binds, unbinds, modules_snapshots, module_maps;
    unsigned fail_from_call, mutate_header_call, revoke_owner_call;
    unsigned restart_during_copy;
    unsigned replace_game_during_copy;
    unsigned fail_allocation_call;
    bool fail_to, fail_base, fail_free, fail_bind, expire_bind, replace_bind, fail_features;
    bool fail_dispose, fail_memory_unbind, fail_events, fail_worker, collision;
    bool collision_unknown, owner_replaced, owner_lookup_failure, fail_title;
    bool fail_patch_support;
    bool block_during_write, expire_during_write, fail_rearm;
    unsigned write_delay_us, observe_during_write;
    unsigned rearms;
    bool fail_module_snapshot, fail_module_map;
    int32_t module_uid;
    /* Explicit metadata-query interleaving: 1=object replacement, 2=EXIT,
     * 3=lost lifecycle events. These model SDK scheduling, not lifetime pins. */
    unsigned module_snapshot_action;
    int reap_result;
    int game_release_error;
    bool consume_failed_game_release;
    bool consume_failed_free, status_during_free;
    unsigned reap_calls, dispose_calls;
    int refs[4];
    uint32_t epoch, phase, value;
    unsigned capture_resets;
    bool input_ready, worker_pending, worker_delete_failure, worker_cleanup_failure;
    unsigned worker_reaps, worker_waits, worker_deletes;
} ServiceFake;
extern ServiceFake fake;
void service_fake_setup(void);
void service_fake_tick(void);
void service_fake_event(uint32_t kind, int32_t pid);
void service_fake_replace_game(void);
void service_fake_worker_fault(void);
void service_fake_worker_finish(void);
void service_fake_quarantine(void);
void service_fake_patch_residue(unsigned kind);
void service_fake_trace(uint32_t event, int32_t result, uint32_t subject, uint32_t detail);
VcoForegroundSample service_fake_sample(int32_t pid, uint32_t status);
#endif
