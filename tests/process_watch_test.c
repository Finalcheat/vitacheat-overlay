#include <assert.h>
#include <string.h>
#include "platform/vita/process_watch.h"
#include "execution/engine.h"
static VcoProcessEvent queue[64];
static unsigned queued, head, polls, state_calls;
static uint32_t generation, phase;
static bool busy, change_on_poll, stop_on_state;
static VcoSession session;
static VcoVitaProcessWatch watch;
static VcoEngine engine;
static unsigned writes, injections, releases;
uint32_t vco_process_events_invalidation(void)
{
    return generation;
}
void vco_process_events_state(VcoProcessEventsState *out)
{
    if (stop_on_state && ++state_calls == 2)
        phase = VCO_PROCESS_EVENTS_STOPPED;
    *out = (VcoProcessEventsState){phase, generation, 0, 0};
}
VcoProcessEventsPoll vco_process_events_poll(VcoProcessEvent *out)
{
    ++polls;
    if (change_on_poll) {
        change_on_poll = false;
        ++generation;
    }
    if (busy)
        return VCO_PROCESS_EVENTS_BUSY;
    if (head == queued)
        return VCO_PROCESS_EVENTS_EMPTY;
    *out = queue[head++];
    return VCO_PROCESS_EVENTS_OK;
}
static void foreground(void)
{
    VcoIdentity id = {42, 7, "PCSB00031"};
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &id);
}
static void setup(void)
{
    queued = head = polls = state_calls = 0;
    generation = 1;
    phase = VCO_PROCESS_EVENTS_ACTIVE;
    busy = change_on_poll = stop_on_state = false;
    vco_session_init(&session);
    foreground();
    vco_vita_process_watch_init(&watch, &session);
    assert(!session.runnable && session.present && watch.revocations == 1);
    vco_vita_process_watch_update(&watch, 1);
    assert(vco_vita_process_watch_unchanged(&watch));
}
static void event(unsigned kind, int32_t pid)
{
    assert(queued < 64);
    queue[queued++] = (VcoProcessEvent){++generation, kind, pid, 0, 0, 0, -1, 0};
}
static void transitions(void)
{
    uint64_t token = session.token;
    for (unsigned kind = VCO_PROCESS_EVENT_CREATE; kind <= VCO_PROCESS_EVENT_SWITCH; ++kind) {
        foreground();
        event(kind, kind == VCO_PROCESS_EVENT_EXIT ? 42 : 77);
        assert(!vco_vita_process_watch_unchanged(&watch)); /* before worker wakes */
        vco_vita_process_watch_update(&watch, kind);
        assert(!session.runnable && session.present && session.token == token);
        assert(watch.last_event.kind == kind && watch.records == kind);
        assert(!vco_vita_process_watch_unchanged(&watch));
        vco_vita_process_watch_update(&watch, kind);
        assert(!session.runnable && vco_vita_process_watch_unchanged(&watch));
    }
    assert(watch.terminal_seen && watch.revocations == 7);
    foreground();
    phase = VCO_PROCESS_EVENTS_STOPPED;
    assert(!vco_vita_process_watch_unchanged(&watch));
    vco_vita_process_watch_update(&watch, 9);
    assert(!session.runnable && session.present);
}
static void faults(void)
{
    foreground();
    busy = true;
    vco_vita_process_watch_update(&watch, 1);
    assert(!session.runnable && !vco_vita_process_watch_unchanged(&watch));
    busy = false;
    vco_vita_process_watch_update(&watch, 2);
    foreground();
    change_on_poll = true;
    vco_vita_process_watch_update(&watch, 3);
    assert(!session.runnable && !vco_vita_process_watch_unchanged(&watch));
    vco_vita_process_watch_update(&watch, 4);
    foreground();
    stop_on_state = true;
    vco_vita_process_watch_update(&watch, 5);
    assert(!session.runnable && !vco_vita_process_watch_unchanged(&watch));
    phase = VCO_PROCESS_EVENTS_ACTIVE;
    stop_on_state = false;
    generation |= VCO_PROCESS_EVENTS_LOST;
    vco_vita_process_watch_update(&watch, 6);
    assert(watch.lost && !vco_vita_process_watch_unchanged(&watch));
    generation = 2; /* Even a broken source cannot clear locally latched loss. */
    foreground();
    vco_vita_process_watch_update(&watch, 7);
    assert(!session.runnable && !vco_vita_process_watch_unchanged(&watch));
    vco_vita_process_watch_init(NULL, NULL);
    vco_vita_process_watch_update(NULL, 0);
    assert(!vco_vita_process_watch_unchanged(NULL));
    vco_vita_process_watch_init(&watch, NULL);
    vco_vita_process_watch_update(&watch, 0);
    assert(!vco_vita_process_watch_unchanged(&watch));
}
static void bounded(void)
{
    for (unsigned i = 0; i < 64; ++i)
        event(VCO_PROCESS_EVENT_START, 77);
    watch.records = UINT32_MAX - 1;
    watch.revocations = UINT32_MAX;
    foreground();
    unsigned before = polls;
    vco_vita_process_watch_update(&watch, 1);
    assert(polls - before == VCO_PROCESS_EVENTS_CAPACITY && head == VCO_PROCESS_EVENTS_CAPACITY);
    assert(!session.runnable && watch.records == UINT32_MAX && watch.revocations == UINT32_MAX);
    assert(!vco_vita_process_watch_unchanged(&watch));
    vco_vita_process_watch_update(&watch, 2);
    assert(head == 64 && !vco_vita_process_watch_unchanged(&watch));
    vco_vita_process_watch_update(&watch, 3);
    assert(vco_vita_process_watch_unchanged(&watch) && !session.runnable);
}
static int alive(void *ctx, uint64_t token)
{
    assert(ctx == &session);
    return vco_session_matches(&session, token, true) && vco_vita_process_watch_unchanged(&watch);
}
static int write_memory(void *ctx, uint64_t token, uint32_t address, const uint8_t *data,
                        size_t size)
{
    assert(alive(ctx, token) && address == 0x81000000u && data[0] == 1 && size == 4);
    ++writes;
    return 0;
}
static int inject(void *ctx, uint64_t token, uint32_t address, const uint8_t *data, size_t size,
                  int32_t *handle)
{
    assert(alive(ctx, token) && address == 0x81000010u && data[0] == 1 && size == 4);
    *handle = (int32_t)++injections;
    return 0;
}
static int release(void *ctx, uint64_t token, int32_t handle)
{
    assert(ctx == &session && vco_session_matches(&session, token, false) && handle > 0);
    ++releases;
    return 0;
}
static void pipeline(void)
{
    VcoCheatEntry entries[2];
    VcoCode codes[2];
    VcoCheatOp ops[2];
    VcoPatch patches[2];
    char names[32];
    VcoCheatFile file = {entries, 2, 0, codes, 2, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    const char text[] = "_V1 Data\n$0200 81000000 00000001\n_V0 Patch\n$A200 81000010 00000001";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    VcoCheatMemory memory = {0};
    memory.context = &session;
    memory.alive = alive;
    memory.write = write_memory;
    memory.inject = inject;
    memory.release = release;
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &file, memory, ops, patches, 2));
    foreground(); /* Only this test's independent authority grants execution. */
    for (unsigned i = 0; i < 2; ++i)
        assert(
            !vco_engine_submit(&engine, (VcoExecutionCommand){i + 1, session.token, 1, i, true}));
    vco_engine_step(&engine, 1);
    assert(writes == 1 && injections == 1 && !releases);
    event(VCO_PROCESS_EVENT_STOP, 42);
    assert(!alive(&session, session.token));
    vco_vita_process_watch_update(&watch, 50001);
    vco_engine_step(&engine, 50001);
    assert(writes == 1 && injections == 1 && releases == 1);
    assert(engine.rows[0].state == VCO_ENTRY_PAUSED && engine.rows[1].state == VCO_ENTRY_PAUSED);
    vco_vita_process_watch_update(&watch, 100001);
    vco_engine_step(&engine, 100001);
    assert(writes == 1 && injections == 1 && !session.runnable);
    /* START is evidence to revoke again, not an implicit re-enable. */
    event(VCO_PROCESS_EVENT_START, 42);
    vco_vita_process_watch_update(&watch, 150001);
    vco_engine_step(&engine, 150001);
    assert(writes == 1 && injections == 1 && !session.runnable);
    assert(!vco_engine_unbind(&engine) && releases == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "transitions"))
        transitions();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else if (!strcmp(argv[1], "bounded"))
        bounded();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else
        assert(0);
    return 0;
}
