#include "../tools/device-harness/fixture_observer.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

#define BASE UINT32_C(0x83000000)
static VcoFixtureController controller;
static VcoFixtureObserver observer;
static VcoFixtureReading reading;
static void instance(uint32_t generation, unsigned variant, uint32_t offset)
{
    memset(&controller, 0, sizeof(controller));
    controller.state = VCO_FIXTURE_READY;
    controller.generation = generation;
    controller.uid = 100 + (int32_t)variant;
    controller.info.uid = controller.uid;
    controller.reply.generation = generation;
    controller.reply.variant = variant;
    controller.reply.module_uid = controller.uid;
    controller.reply.data_initial = 5 + variant;
    controller.reply.function_initial = variant ? 11 : 7;
    controller.reply.data_address = BASE + offset;
    controller.reply.function_address = BASE + 0x1001 + offset;
    memset(&reading, 0, sizeof(reading));
    reading.generation = generation;
    reading.uid = controller.uid;
    reading.data_address = controller.reply.data_address;
    reading.function_address = controller.reply.function_address;
    reading.data = controller.reply.data_initial;
    reading.instruction = (uint16_t)(0x2000u | controller.reply.function_initial);
    reading.return_instruction = 0x4770;
    reading.function = (int32_t)controller.reply.function_initial;
    reading.called = true;
}
static void initial(void)
{
    for (unsigned variant = 0; variant < 2; ++variant) {
        for (unsigned patches = 0; patches < 8; ++patches) {
            vco_fixture_observer_init(&observer);
            instance(40, variant, 0);
            assert(vco_fixture_observer_callable(&observer, &controller, &reading));
            assert(!vco_fixture_observer_begin(&observer, &controller));
            if (patches & 1)
                reading.data = 88;
            if (patches & 2)
                reading.instruction = 0x2009;
            if (patches & 4)
                reading.function = 9; /* Toggle can occur between reading code and calling it. */
            assert(vco_fixture_observer_callable(&observer, &controller, &reading));
            assert(!vco_fixture_observer_sample(&observer, &controller, &reading));
            assert(observer.samples == 1 && !observer.generations && !observer.error);
            assert(vco_fixture_observer_begin(&observer, &controller) < 0);
            vco_fixture_observer_stop(&observer);
            assert(observer.samples == 1 && !observer.active);
            assert(!vco_fixture_observer_begin(&observer, &controller));
            assert(!observer.samples && !observer.error);
        }
    }
}
static void newer(void)
{
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        vco_fixture_observer_init(&observer);
        instance(1, 0, 0);
        assert(!vco_fixture_observer_begin(&observer, &controller));
        instance(2, 1, 0);
        static const VcoFixtureObserveError errors[] = {
            VCO_FIXTURE_OBSERVE_NONE, VCO_FIXTURE_OBSERVE_DATA,   VCO_FIXTURE_OBSERVE_CODE,
            VCO_FIXTURE_OBSERVE_CODE, VCO_FIXTURE_OBSERVE_RETURN, VCO_FIXTURE_OBSERVE_RETURN};
        if (mutation == 1)
            reading.data = 5; /* Original A's saved value contaminates newly loaded B. */
        if (mutation == 2)
            reading.instruction = 0x2007;
        if (mutation == 3)
            reading.instruction = 0x2009; /* Even a known patch is forbidden on a new lifetime. */
        if (mutation == 4)
            reading.function = 7; /* Bytes say 11, executed value still belongs to A. */
        if (mutation == 5)
            reading.called = false;
        assert(vco_fixture_observer_callable(&observer, &controller, &reading) ==
               (mutation == 0 || mutation >= 4));
        assert((vco_fixture_observer_sample(&observer, &controller, &reading) < 0) ==
               (mutation != 0));
        assert(observer.error == errors[mutation]);
        if (mutation) {
            assert(!observer.active && !observer.samples && !observer.generations &&
                   observer.expected_data == 6 && observer.expected_function == 11 &&
                   observer.first_failure.generation == 2);
        } else
            assert(observer.samples == 1 && observer.generations == 1 &&
                   observer.reused_data == 1 && observer.reused_function == 1);
    }
}
static void reuse(void)
{
    for (unsigned empty = 0; empty < 2; ++empty) {
        vco_fixture_observer_init(&observer);
        instance(1, 0, 0);
        if (empty) {
            controller.state = VCO_FIXTURE_EMPTY;
            controller.uid = -1;
        }
        assert(!vco_fixture_observer_begin(&observer, &controller));
        for (unsigned i = 0; i < 100; ++i) {
            instance(i + 2, i & 1, 0);
            for (unsigned frame = 0; frame < 3; ++frame) {
                assert(vco_fixture_observer_callable(&observer, &controller, &reading));
                assert(!vco_fixture_observer_sample(&observer, &controller, &reading));
            }
        }
        assert(observer.samples == 300 && observer.generations == 100 &&
               observer.reused_data == 100 - empty && observer.reused_function == 100 - empty &&
               observer.last_generation == 101);
        /* A shifted text address is counted independently from data reuse. */
        instance(102, 0, 0x2000);
        controller.reply.data_address = reading.data_address = observer.origin_data;
        assert(!vco_fixture_observer_sample(&observer, &controller, &reading));
        assert(observer.generations == 101 && observer.reused_data == 101 - empty &&
               observer.reused_function == 100 - empty);
    }
}
static void latch(void)
{
    vco_fixture_observer_init(&observer);
    instance(1, 0, 0);
    assert(!vco_fixture_observer_begin(&observer, &controller));
    instance(2, 1, 0);
    reading.return_instruction = 0xdead;
    assert(!vco_fixture_observer_callable(&observer, &controller, &reading));
    assert(vco_fixture_observer_sample(&observer, &controller, &reading) < 0);
    assert(observer.error == VCO_FIXTURE_OBSERVE_CODE &&
           observer.first_failure.return_instruction == 0xdead);
    unsigned char original[sizeof(observer)];
    memcpy(original, &observer, sizeof(observer));
    vco_fixture_observer_stop(&observer);
    for (unsigned i = 3; i < 20; ++i) {
        instance(i, i & 1, 0x1000);
        assert(vco_fixture_observer_begin(&observer, &controller) < 0);
        assert(!vco_fixture_observer_callable(&observer, &controller, &reading));
        assert(vco_fixture_observer_sample(&observer, &controller, &reading) < 0);
        assert(!memcmp(original, &observer, sizeof(observer)));
    }
}
static void invalid(void)
{
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        vco_fixture_observer_init(&observer);
        instance(2, 0, 0);
        assert(!vco_fixture_observer_begin(&observer, &controller));
        switch (mutation) {
        case 0:
            reading.uid++;
            break;
        case 1:
            reading.generation++;
            break;
        case 2:
            reading.data_address += 4;
            break;
        case 3:
            reading.function_address += 2;
            break;
        case 4:
            controller.state = VCO_FIXTURE_UNLOADING;
            break;
        case 5:
            controller.reply.module_uid++;
            break;
        case 6:
            controller.reply.variant = 2;
            break;
        case 7:
            controller.info.uid++;
            break;
        default:
            instance(1, 0, 0);
            break; /* Matching receipt, regressed lifetime. */
        }
        assert(!vco_fixture_observer_callable(&observer, &controller, &reading));
        assert(vco_fixture_observer_sample(&observer, &controller, &reading) < 0 &&
               observer.error == VCO_FIXTURE_OBSERVE_IDENTITY);
    }
    vco_fixture_observer_init(&observer);
    instance(3, 0, 0);
    assert(!vco_fixture_observer_begin(&observer, &controller));
    assert(vco_fixture_observer_sample(&observer, &controller, NULL) < 0 && observer.active);
    assert(!vco_fixture_observer_callable(NULL, &controller, &reading));
    assert(!vco_fixture_observer_callable(&observer, NULL, &reading));
    assert(vco_fixture_observer_begin(NULL, &controller) < 0);
    assert(vco_fixture_observer_sample(NULL, &controller, &reading) < 0);
    vco_fixture_observer_init(NULL);
    vco_fixture_observer_stop(NULL);
}
static void limits(void)
{
    vco_fixture_observer_init(&observer);
    instance(1, 0, 0);
    assert(!vco_fixture_observer_begin(&observer, &controller));
    observer.samples = UINT64_MAX - 1;
    assert(!vco_fixture_observer_sample(&observer, &controller, &reading));
    assert(observer.samples == UINT64_MAX);
    assert(vco_fixture_observer_sample(&observer, &controller, &reading) < 0 &&
           observer.error == VCO_FIXTURE_OBSERVE_LIMIT && observer.samples == UINT64_MAX);
    vco_fixture_observer_init(&observer);
    assert(!vco_fixture_observer_begin(&observer, &controller));
    observer.generations = UINT_MAX;
    instance(2, 1, 0);
    assert(vco_fixture_observer_sample(&observer, &controller, &reading) < 0 &&
           observer.error == VCO_FIXTURE_OBSERVE_LIMIT && observer.generations == UINT_MAX &&
           !observer.samples);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "initial"))
        initial();
    else if (!strcmp(argv[1], "newer"))
        newer();
    else if (!strcmp(argv[1], "reuse"))
        reuse();
    else if (!strcmp(argv[1], "latch"))
        latch();
    else if (!strcmp(argv[1], "invalid"))
        invalid();
    else if (!strcmp(argv[1], "limits"))
        limits();
    else
        assert(0);
    return 0;
}
