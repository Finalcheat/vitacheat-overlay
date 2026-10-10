#include "execution/admission.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>

#define WORKERS 8
static VcoAdmission gate = VCO_ADMISSION_INITIALIZER;
static unsigned ready, release_workers;
static unsigned results[WORKERS];
static void *worker(void *arg)
{
    unsigned id = *(unsigned *)arg;
    assert(vco_admission_enter(&gate));
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_workers, __ATOMIC_ACQUIRE))
        sched_yield();
    results[id] = id + 1;
    assert(vco_admission_leave(&gate));
    /* Close has returned before release_workers was published. */
    assert(!vco_admission_enter(&gate));
    return 0;
}
static void *racer(void *arg)
{
    VcoAdmission *g = arg;
    while (vco_admission_enter(g))
        assert(vco_admission_leave(g));
    return 0;
}
int main(void)
{
    assert(vco_admission_closed(0));
    assert(!vco_admission_closed(&gate));
    assert(!vco_admission_enter(0));
    assert(!vco_admission_leave(0));
    assert(!vco_admission_drained(0));
    vco_admission_close(0);
    assert(!vco_admission_drained(&gate));
    assert(!vco_admission_leave(&gate));
    pthread_t threads[WORKERS];
    unsigned ids[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) {
        ids[i] = i;
        assert(!pthread_create(&threads[i], 0, worker, &ids[i]));
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != WORKERS)
        sched_yield();
    vco_admission_close(&gate);
    vco_admission_close(&gate);
    assert(!vco_admission_drained(&gate));
    assert(vco_admission_closed(&gate));
    assert(!vco_admission_enter(&gate));
    __atomic_store_n(&release_workers, 1, __ATOMIC_RELEASE);
    while (!vco_admission_drained(&gate))
        sched_yield();
    /* Read BEFORE join: visibility must come from drain, not pthread_join. */
    for (unsigned i = 0; i < WORKERS; ++i)
        assert(results[i] == i + 1);
    for (unsigned i = 0; i < WORKERS; ++i)
        assert(!pthread_join(threads[i], 0));
    assert(!vco_admission_leave(&gate));
    assert(vco_admission_drained(&gate));
    VcoAdmission full = {UINT32_C(0x7fffffff)};
    assert(!vco_admission_enter(&full));
    vco_admission_close(&full);
    assert(!vco_admission_drained(&full));
    assert(vco_admission_leave(&full));
    assert(!vco_admission_enter(&full));
    for (unsigned round = 0; round < 100; ++round) {
        VcoAdmission racing = VCO_ADMISSION_INITIALIZER;
        for (unsigned i = 0; i < WORKERS; ++i)
            assert(!pthread_create(&threads[i], 0, racer, &racing));
        vco_admission_close(&racing);
        for (unsigned i = 0; i < WORKERS; ++i)
            assert(!pthread_join(threads[i], 0));
        assert(vco_admission_drained(&racing));
        assert(!vco_admission_enter(&racing));
    }
    return 0;
}
