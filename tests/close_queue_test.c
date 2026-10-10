#include "execution/close_queue.h"
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
static VcoCloseQueue queue;
static VcoEngineCloseKey key = {.identity = {42, 99, "PCSB00031"},
                                .session = 1,
                                .revision = 2,
                                .transaction = 3,
                                .scope = VCO_CLOSE_PROCESS,
                                .process_object = 0x1234};
static unsigned calls, payload;
static VcoError execute(void *context, const VcoEngineCloseKey *target, VcoEngineCloseResult *out)
{
    assert(context == &queue && vco_close_key_equal(target, &key));
    VcoCloseReply reply;
    assert(vco_close_queue_receive(&queue, 1, 1, &reply) == VCO_CLOSE_WAIT);
    assert(vco_close_queue_release(&queue, 1, 1) == VCO_CLOSE_WAIT);
    ++calls;
    *out = (VcoEngineCloseResult){VCO_OK, VCO_E_RESTORE};
    return VCO_E_RESTORE;
}
static void delivery(void)
{
    vco_close_queue_init(&queue);
    uint64_t ticket = 0, duplicate = 0;
    assert(!vco_close_queue_pump(&queue, execute, NULL, &queue));
    assert(!vco_close_queue_submit(&queue, 1, &key, &ticket) && ticket == 1);
    assert(!vco_close_queue_submit(&queue, 1, &key, &duplicate) && duplicate == ticket);
    assert(!vco_close_queue_submit(&queue, 2, &key, &duplicate));
    VcoCloseReply reply, copy;
    memset(&reply, 0x5a, sizeof(reply));
    copy = reply;
    assert(vco_close_queue_receive(&queue, 1, ticket, &reply) == VCO_CLOSE_WAIT);
    assert(!memcmp(&reply, &copy, sizeof(reply)));
    assert(vco_close_queue_pump(&queue, execute, NULL, &queue) && calls == 1);
    assert(!vco_close_queue_receive(&queue, 1, ticket, &reply));
    assert(reply.ticket == ticket && vco_close_key_equal(&reply.key, &key) &&
           reply.error == VCO_E_RESTORE && reply.cleanup.native == VCO_E_RESTORE);
    assert(!vco_close_queue_receive(&queue, 2, ticket, &copy));
    assert(copy.error == reply.error && vco_close_key_equal(&copy.key, &reply.key));
    assert(vco_close_queue_pump(&queue, execute, NULL, &queue) && calls == 1);
    assert(!vco_close_queue_release(&queue, 1, ticket));
    assert(vco_close_queue_submit(&queue, 1, &key, &duplicate) == VCO_CLOSE_TICKET);
    assert(vco_close_queue_release(&queue, 1, ticket) == VCO_CLOSE_TICKET);
    assert(vco_close_queue_pump(&queue, execute, NULL, &queue));
    assert(!vco_close_queue_release(&queue, 2, ticket));
    assert(!vco_close_queue_pump(&queue, execute, NULL, &queue));
    assert(!vco_close_queue_submit(&queue, 1, &key, &duplicate) && duplicate > ticket);
    assert(vco_close_queue_receive(&queue, 1, ticket, &reply) == VCO_CLOSE_TICKET);
}
static void bounds(void)
{
    vco_close_queue_init(&queue);
    uint64_t ticket = 123, next;
    VcoEngineCloseKey bad = key;
    bad.transaction = 0;
    assert(vco_close_queue_submit(&queue, 1, &bad, &ticket) == VCO_CLOSE_ARGUMENT && ticket == 123);
    assert(vco_close_queue_submit(NULL, 1, &key, &ticket) == VCO_CLOSE_ARGUMENT);
    assert(vco_close_queue_submit(&queue, 0, &key, &ticket) == VCO_CLOSE_ARGUMENT);
    assert(!vco_close_queue_submit(&queue, 1, &key, &ticket));
    for (unsigned variant = 0; variant < 6; ++variant) {
        bad = key;
        if (variant == 0)
            ++bad.identity.instance;
        if (variant == 1)
            ++bad.identity.pid;
        if (variant == 2)
            ++bad.process_object;
        if (variant == 3)
            ++bad.session;
        if (variant == 4)
            ++bad.revision;
        if (variant == 5)
            ++bad.transaction;
        next = 99;
        assert(vco_close_queue_submit(&queue, 2, &bad, &next) == VCO_CLOSE_BUSY && next == 99);
    }
    for (unsigned i = 2; i <= VCO_CLOSE_PEERS; ++i)
        assert(!vco_close_queue_submit(&queue, i, &key, &next) && next == ticket);
    assert(vco_close_queue_submit(&queue, 99, &key, &next) == VCO_CLOSE_LIMIT);
    assert(vco_close_queue_pump(&queue, execute, NULL, &queue));
    for (unsigned i = 1; i <= VCO_CLOSE_PEERS; ++i)
        assert(!vco_close_queue_release(&queue, i, ticket));
    assert(!vco_close_queue_pump(&queue, execute, NULL, &queue));
    queue.sequence = UINT64_MAX; /* Explicit bounded-counter fault. */
    next = 99;
    assert(vco_close_queue_submit(&queue, 1, &key, &next) == VCO_CLOSE_EXHAUSTED && next == 99);
    __atomic_store_n(&queue.gate, 1, __ATOMIC_RELEASE); /* A preempted short-copy producer. */
    assert(vco_close_queue_submit(&queue, 1, &key, &next) == VCO_CLOSE_BUSY);
    assert(vco_close_queue_pump(&queue, execute, NULL, &queue));
    __atomic_store_n(&queue.gate, 0, __ATOMIC_RELEASE);
}
static void shutdown(void)
{
    vco_close_queue_init(&queue);
    uint64_t ticket;
    assert(!vco_close_queue_submit(&queue, 1, &key, &ticket));
    assert(vco_close_queue_abandon_after_stop(&queue, 1, ticket) == VCO_CLOSE_WAIT);
    vco_close_queue_stop(&queue);
    assert(vco_close_queue_submit(&queue, 2, &key, &ticket) == VCO_CLOSE_CLOSED);
    assert(vco_close_queue_pump(&queue, execute, NULL, &queue));
    VcoCloseReply reply;
    assert(!vco_close_queue_receive(&queue, 1, ticket, &reply) && reply.error == VCO_E_RESTORE);
    assert(!vco_close_queue_release(&queue, 1, ticket));
    assert(!vco_close_queue_pump(&queue, execute, NULL, &queue));
    assert(!vco_close_queue_open(&queue));
    /* Separate owner lifetime, explicitly stopped before processing. This
     * fixture asserts owner quiescence; the worker wrapper verifies its ACK. */
    vco_close_queue_init(&queue);
    assert(!vco_close_queue_submit(&queue, 1, &key, &ticket));
    vco_close_queue_stop(&queue);
    assert(!vco_close_queue_abandon_after_stop(&queue, 1, ticket));
    assert(vco_close_queue_receive(&queue, 1, ticket, &reply) == VCO_CLOSE_TICKET);
    assert(!calls || calls == 1);
}
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool entered, finish;
static VcoError blocking_execute(void *context, const VcoEngineCloseKey *target,
                                 VcoEngineCloseResult *out)
{
    assert(context == &queue && vco_close_key_equal(target, &key));
    ++calls;
    assert(!pthread_mutex_lock(&mutex));
    entered = true;
    assert(!pthread_cond_broadcast(&cond));
    while (!finish)
        assert(!pthread_cond_wait(&cond, &mutex));
    assert(!pthread_mutex_unlock(&mutex));
    payload = 0x1234;
    *out = (VcoEngineCloseResult){VCO_OK, VCO_E_RESTORE};
    return VCO_E_RESTORE;
}
static void *owner(void *unused)
{
    (void)unused;
    assert(vco_close_queue_pump(&queue, blocking_execute, NULL, &queue));
    return NULL;
}
typedef struct {
    uint64_t id, ticket;
} Producer;
static void *producer(void *argument)
{
    Producer *p = argument;
    VcoCloseStatus status;
    do {
        status = vco_close_queue_submit(&queue, p->id, &key, &p->ticket);
        if (status == VCO_CLOSE_BUSY)
            sched_yield();
    } while (status == VCO_CLOSE_BUSY);
    assert(status == VCO_CLOSE_OK);
    return NULL;
}
static void concurrent(void)
{
    vco_close_queue_init(&queue);
    Producer peers[4] = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
    pthread_t producers[4], owning;
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&producers[i], NULL, producer, &peers[i]));
    for (unsigned i = 0; i < 4; ++i) {
        assert(!pthread_join(producers[i], NULL));
        assert(peers[i].ticket == 1);
    }
    assert(!pthread_create(&owning, NULL, owner, NULL));
    assert(!pthread_mutex_lock(&mutex));
    while (!entered)
        assert(!pthread_cond_wait(&cond, &mutex));
    assert(!pthread_mutex_unlock(&mutex));
    uint64_t ticket;
    assert(!vco_close_queue_submit(&queue, 5, &key, &ticket) && ticket == 1);
    VcoCloseReply reply;
    assert(vco_close_queue_receive(&queue, 5, ticket, &reply) == VCO_CLOSE_WAIT);
    assert(vco_close_queue_release(&queue, 5, ticket) == VCO_CLOSE_WAIT);
    vco_close_queue_stop(&queue);
    assert(!pthread_mutex_lock(&mutex));
    finish = true;
    assert(!pthread_cond_broadcast(&cond));
    assert(!pthread_mutex_unlock(&mutex));
    VcoCloseStatus status;
    do {
        status = vco_close_queue_receive(&queue, 5, ticket, &reply);
        if (status == VCO_CLOSE_WAIT || status == VCO_CLOSE_BUSY)
            sched_yield();
    } while (status == VCO_CLOSE_WAIT || status == VCO_CLOSE_BUSY);
    assert(status == VCO_CLOSE_OK && payload == 0x1234 && reply.cleanup.native == VCO_E_RESTORE);
    assert(!pthread_join(owning, NULL) && calls == 1);
    for (unsigned i = 1; i <= 5; ++i)
        assert(!vco_close_queue_release(&queue, i, ticket));
    assert(!vco_close_queue_pump(&queue, blocking_execute, NULL, &queue));
}
static unsigned finish_calls, native_payload[2];
static void complete(void *context, const VcoCloseReply *reply)
{
    assert(context == &queue && !queue.active && calls == 1);
    assert(reply->ticket == 1 && vco_close_key_equal(&reply->key, &key) &&
           reply->error == VCO_E_RESTORE && reply->cleanup.native == VCO_E_RESTORE);
    VcoCloseReply retired;
    uint64_t next = 99;
    assert(vco_close_queue_receive(&queue, 1, 1, &retired) == VCO_CLOSE_TICKET);
    /* The callback is outside the gate, but no new invocation may enter yet. */
    assert(vco_close_queue_submit(&queue, 3, &key, &next) == VCO_CLOSE_BUSY && next == 99);
    ++finish_calls;
}
static void completion(void)
{
    vco_close_queue_init(&queue);
    uint64_t first, second;
    assert(!vco_close_queue_submit(&queue, 1, &key, &first));
    assert(!vco_close_queue_submit(&queue, 2, &key, &second) && first == second);
    assert(vco_close_queue_pump(&queue, execute, complete, &queue));
    assert(!vco_close_queue_release(&queue, 1, first));
    assert(vco_close_queue_pump(&queue, execute, complete, &queue) && !finish_calls);
    VcoCloseReply original;
    assert(!vco_close_queue_receive(&queue, 2, second, &original));
    assert(!vco_close_queue_release(&queue, 2, second) && !finish_calls);
    uint64_t next = 99;
    assert(vco_close_queue_submit(&queue, 3, &key, &next) == VCO_CLOSE_BUSY && next == 99);
    assert(!vco_close_queue_pump(&queue, execute, complete, &queue) && finish_calls == 1);
    assert(!vco_close_queue_pump(&queue, execute, complete, &queue) && finish_calls == 1);
    assert(original.error == VCO_E_RESTORE && original.cleanup.native == VCO_E_RESTORE);
    VcoEngineCloseKey newer = key;
    ++newer.transaction;
    assert(!vco_close_queue_submit(&queue, 3, &newer, &next) && next > first);
}
static void completion_stop(void)
{
    for (unsigned unserved = 0; unserved < 2; ++unserved) {
        vco_close_queue_init(&queue);
        calls = 0;
        uint64_t ticket;
        assert(!vco_close_queue_submit(&queue, 1, &key, &ticket));
        if (!unserved) {
            assert(vco_close_queue_pump(&queue, execute, complete, &queue));
            assert(!vco_close_queue_release(&queue, 1, ticket));
        }
        vco_close_queue_stop(&queue);
        if (unserved)
            assert(!vco_close_queue_abandon_after_stop(&queue, 1, ticket));
        assert(!vco_close_queue_pump(&queue, execute, complete, &queue) && !finish_calls);
        assert(calls == !unserved);
    }
}
static void blocking_complete(void *context, const VcoCloseReply *reply)
{
    /* Acquire all ordinary writes from completed native invocations, without
     * pthread_join or a second synchronization mechanism for that payload. */
    assert(native_payload[0] == 42 && native_payload[1] == 84);
    complete(context, reply);
    assert(!pthread_mutex_lock(&mutex));
    entered = true;
    assert(!pthread_cond_broadcast(&cond));
    while (!finish)
        assert(!pthread_cond_wait(&cond, &mutex));
    assert(!pthread_mutex_unlock(&mutex));
}
static void *completion_owner(void *unused)
{
    (void)unused;
    while (vco_close_queue_pump(&queue, execute, blocking_complete, &queue))
        sched_yield();
    return NULL;
}
static void *end_native(void *argument)
{
    Producer *p = argument;
    native_payload[p->id - 1] = (unsigned)p->id * 42;
    VcoCloseStatus result;
    do {
        result = vco_close_queue_release(&queue, p->id, p->ticket);
        if (result == VCO_CLOSE_BUSY)
            sched_yield();
    } while (result == VCO_CLOSE_BUSY);
    assert(result == VCO_CLOSE_OK);
    return NULL;
}
static void completion_concurrent(void)
{
    vco_close_queue_init(&queue);
    Producer peers[2] = {{1, 0}, {2, 0}};
    for (unsigned i = 0; i < 2; ++i)
        assert(!vco_close_queue_submit(&queue, peers[i].id, &key, &peers[i].ticket));
    assert(vco_close_queue_pump(&queue, execute, blocking_complete, &queue));
    pthread_t owning, ends[2];
    assert(!pthread_create(&owning, NULL, completion_owner, NULL));
    for (unsigned i = 0; i < 2; ++i)
        assert(!pthread_create(&ends[i], NULL, end_native, &peers[i]));
    assert(!pthread_mutex_lock(&mutex));
    while (!entered)
        assert(!pthread_cond_wait(&cond, &mutex));
    assert(!pthread_mutex_unlock(&mutex));
    assert(finish_calls == 1);
    uint64_t next = 99;
    assert(vco_close_queue_submit(&queue, 3, &key, &next) == VCO_CLOSE_BUSY);
    vco_close_queue_stop(&queue); /* Stop can race an already admitted finish. */
    assert(!pthread_mutex_lock(&mutex));
    finish = true;
    assert(!pthread_cond_broadcast(&cond));
    assert(!pthread_mutex_unlock(&mutex));
    assert(!pthread_join(owning, NULL));
    for (unsigned i = 0; i < 2; ++i)
        assert(!pthread_join(ends[i], NULL));
    assert(!vco_close_queue_pump(&queue, execute, blocking_complete, &queue) && finish_calls == 1);
    assert(vco_close_queue_submit(&queue, 3, &key, &next) == VCO_CLOSE_CLOSED);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "delivery"))
        delivery();
    else if (!strcmp(argv[1], "bounds"))
        bounds();
    else if (!strcmp(argv[1], "shutdown"))
        shutdown();
    else if (!strcmp(argv[1], "concurrent"))
        concurrent();
    else if (!strcmp(argv[1], "completion"))
        completion();
    else if (!strcmp(argv[1], "completion_stop"))
        completion_stop();
    else if (!strcmp(argv[1], "completion_concurrent"))
        completion_concurrent();
    else
        assert(0);
    return 0;
}
