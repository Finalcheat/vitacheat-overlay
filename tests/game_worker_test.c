#define _POSIX_C_SOURCE 200809L
#include "agent/vita_worker.h"
#include <psp2/kernel/threadmgr.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
static VcoGameWorker worker;
static VcoGameExecutor executor;
static unsigned char payload[VCO_UPLOAD_MAX];
static SceKernelThreadEntry thread_entry;
static void *thread_argument;
static unsigned creates, starts, waits, deletes, polls, publishes, delays;
static unsigned stopped_receipts, stopped_transient;
static VcoAgentStatus stopped_error;
static int create_error, start_error, wait_error, delete_error;
static unsigned mode;
static bool ran;
static pthread_t native_thread;
static pthread_mutex_t entered_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t entered_condition = PTHREAD_COND_INITIALIZER;
static bool entered;
static void *native_entry(void *unused)
{
    (void)unused;
    return (void *)(intptr_t)thread_entry(sizeof(thread_argument), &thread_argument);
}
int sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority,
                          unsigned int stack, unsigned int attributes, int affinity,
                          const void *options)
{
    (void)priority;
    (void)attributes;
    (void)affinity;
    (void)options;
    assert(!strcmp(name, "vco_game_execution") && stack == 0x6000);
    ++creates;
    thread_entry = entry;
    return create_error ? create_error : 12;
}
int sceKernelStartThread(int thread, SceSize size, void *args)
{
    assert(thread == 12 && size == sizeof(thread_argument));
    memcpy(&thread_argument, args, size);
    ++starts;
    if (mode == 6)
        assert(!pthread_create(&native_thread, NULL, native_entry, NULL));
    return start_error;
}
static int run(void)
{
    assert(!ran);
    ran = true;
    return thread_entry(sizeof(thread_argument), &thread_argument);
}
int sceKernelWaitThreadEnd(int thread, int *status, SceUInt *timeout)
{
    assert(thread == 12 && timeout);
    ++waits;
    if (wait_error)
        return wait_error;
    if (mode == 6) {
        void *result;
        assert(!pthread_join(native_thread, &result));
        *status = (int)(intptr_t)result;
        return 0;
    }
    *status = ran ? worker.run_error : run();
    return 0;
}
int sceKernelDeleteThread(int thread)
{
    assert(thread == 12);
    ++deletes;
    return delete_error;
}
int sceKernelDelayThread(SceUInt length)
{
    assert(length == VCO_GAME_WORKER_PERIOD_US || length == 1000u);
    if (length == 1000u)
        return 0;
    ++delays;
    if (mode == 6) {
        struct timespec period = {0, 1000000};
        assert(!nanosleep(&period, NULL));
        return 0;
    }
    assert(delays < 10);
    if (mode == 1 && delays == 1)
        vco_game_worker_request_stop(&worker);
    return 0;
}
SceInt64 sceKernelGetSystemTimeWide(void)
{
    return (SceInt64)polls * 50000;
}
static VcoAgentStatus poll(void *context, uint64_t session, VcoAgentMessage *out, void *bytes,
                           size_t capacity)
{
    (void)context;
    assert(session == 1 && bytes == payload && capacity == sizeof(payload));
    ++polls;
    if (mode == 6) {
        assert(!pthread_mutex_lock(&entered_mutex));
        entered = true;
        assert(!pthread_cond_signal(&entered_condition));
        assert(!pthread_mutex_unlock(&entered_mutex));
    }
    if (mode == 2)
        return VCO_AGENT_STALE;
    if (mode == 3 && polls == 1)
        return VCO_AGENT_BUSY;
    *out =
        (VcoAgentMessage){.session = 1, .abi = VCO_AGENT_ABI, .size = sizeof(*out), .runnable = 1};
    if (mode == 3)
        vco_game_worker_request_stop(&worker);
    if (mode == 4) {
        out->ticket = 1;
        out->operation = VCO_AGENT_CLEAR;
    }
    return VCO_AGENT_OK;
}
static VcoAgentStatus publish(void *context, const VcoAgentResult *result)
{
    (void)context;
    if (result->operation == VCO_AGENT_STOPPED) {
        ++stopped_receipts;
        assert(!result->ticket && !result->error && !result->status.revision &&
               !result->status.count && result->status.state == VCO_SERVICE_BACKGROUND);
        assert(executor.stopped && !executor.arena && !executor.engine.bound);
        if (stopped_transient && stopped_receipts <= stopped_transient)
            return VCO_AGENT_BUSY;
        return stopped_error;
    }
    assert(mode == 4 && result->ticket == 1 && !result->error);
    ++publishes;
    if (publishes == 1)
        return VCO_AGENT_BUSY;
    vco_game_worker_request_stop(&worker);
    return VCO_AGENT_OK;
}
static void *allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}
static void deallocate(void *context, void *bytes)
{
    (void)context;
    free(bytes);
}
static int alive(void *context, uint64_t session)
{
    (void)context;
    return session == 1;
}
static VcoError finalize(void *context)
{
    unsigned *calls = context;
    ++*calls;
    return mode == 5 ? VCO_E_RESTORE : VCO_OK;
}
static unsigned finalizations;
static void setup(void)
{
    VcoAgentBinding binding = {.session = 1,
                               .abi = VCO_AGENT_ABI,
                               .size = sizeof(binding),
                               .pid = 77,
                               .title_id = "PCSB00031"};
    VcoAgentAllocator allocator = {NULL, allocate, deallocate};
    VcoCheatMemory memory = {.alive = alive};
    assert(
        !vco_game_executor_init(&executor, &binding, allocator, memory, finalize, &finalizations));
    vco_game_worker_init(&worker);
}
static int start(void)
{
    return vco_game_worker_start(&worker, &executor, (VcoGameChannel){NULL, poll, publish, NULL},
                                 payload, sizeof(payload));
}
static void lifecycle(void)
{
    assert(!start());
    assert(vco_game_worker_start(&worker, &executor, (VcoGameChannel){NULL, poll, publish, NULL},
                                 payload, sizeof(payload)) == VCO_GAME_WORKER_STATE);
    wait_error = -7;
    assert(vco_game_worker_stop(&worker, 1000) == -7);
    assert(!deletes && worker.thread == 12 && !worker.joined && !executor.stopped);
    wait_error = 0;
    delete_error = -8;
    assert(vco_game_worker_stop(&worker, 1000) == -8);
    assert(worker.joined && executor.stopped && finalizations == 1 && worker.thread == 12);
    delete_error = 0;
    assert(!vco_game_worker_stop(&worker, 1000));
    assert(waits == 2 && deletes == 2 && worker.thread == -1);
    assert(!vco_game_worker_stop(&worker, 1000) && finalizations == 1);
}
static void execution(void)
{
    mode = 1;
    assert(!start());
    assert(!run());
    assert(polls == 1 && delays == 1 && executor.stopped && finalizations == 1);
    assert(!vco_game_worker_stop(&worker, 1000) && deletes == 1);
}
static void stale(void)
{
    mode = 2;
    assert(!start());
    assert(run() == VCO_GAME_WORKER_SESSION);
    assert(executor.revoked && !executor.stopped && !finalizations);
    assert(vco_game_worker_stop(&worker, 1000) == VCO_GAME_WORKER_SESSION);
    assert(worker.cleanup == VCO_E_SESSION && worker.thread == -1);
    assert(!stopped_receipts);
}
static void transient(void)
{
    mode = 3;
    assert(!start());
    assert(!run());
    assert(polls == 2 && delays == 1 && !publishes && finalizations == 1);
    assert(!vco_game_worker_stop(&worker, 1000));
}
static void receipt(void)
{
    mode = 4;
    assert(!start());
    assert(!run());
    assert(polls == 2 && publishes == 2 && finalizations == 2); /* CLEAR then explicit stop. */
    assert(!vco_game_worker_stop(&worker, 1000));
}
static void faults(void)
{
    create_error = -2;
    assert(start() == -2 && !starts && worker.thread == -1);
    create_error = 0;
    start_error = -3;
    delete_error = -4;
    assert(start() == -3 && worker.thread == 12 && !worker.started);
    assert(vco_game_worker_stop(&worker, 1000) == -4 && !waits && !finalizations);
    delete_error = 0;
    assert(!vco_game_worker_stop(&worker, 1000) && worker.thread == -1);
    start_error = 0;
    mode = 5;
    assert(!start());
    assert(vco_game_worker_stop(&worker, 1000) == VCO_GAME_WORKER_CLEANUP);
    assert(worker.cleanup == VCO_E_RESTORE && !executor.stopped && finalizations == 1);
    assert(vco_game_worker_stop(&worker, 1000) == VCO_GAME_WORKER_CLEANUP && finalizations == 1);
    assert(!stopped_receipts);
}
static void concurrent(void)
{
    mode = 6;
    assert(!start());
    assert(!pthread_mutex_lock(&entered_mutex));
    while (!entered)
        assert(!pthread_cond_wait(&entered_condition, &entered_mutex));
    assert(!pthread_mutex_unlock(&entered_mutex));
    assert(!vco_game_worker_stop(&worker, 100000));
    assert(worker.joined && worker.thread == -1 && executor.stopped && finalizations == 1);
    assert(polls && waits == 1 && deletes == 1);
}
static void stopped_delivery(const char *test)
{
    mode = 1;
    if (!strcmp(test, "stopped_retry"))
        stopped_transient = 2;
    else
        stopped_error = !strcmp(test, "stopped_timeout") ? VCO_AGENT_BUSY : VCO_AGENT_IO;
    assert(!start());
    int e = run();
    assert(executor.stopped && finalizations == 1);
    if (stopped_transient) {
        assert(!e && stopped_receipts == 3);
        assert(!vco_game_worker_stop(&worker, 1000));
    } else {
        assert(e == VCO_GAME_WORKER_PROTOCOL);
        assert(stopped_receipts == (stopped_error == VCO_AGENT_BUSY ? 8u : 1u));
        assert(vco_game_worker_stop(&worker, 1000) == VCO_GAME_WORKER_PROTOCOL);
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "execution"))
        execution();
    else if (!strcmp(argv[1], "stale"))
        stale();
    else if (!strcmp(argv[1], "transient"))
        transient();
    else if (!strcmp(argv[1], "receipt"))
        receipt();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else if (!strcmp(argv[1], "concurrent"))
        concurrent();
    else if (!strncmp(argv[1], "stopped_", 8))
        stopped_delivery(argv[1]);
    else
        assert(!"unknown case");
    puts("game worker passed");
    return 0;
}
