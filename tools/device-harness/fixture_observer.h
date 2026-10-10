#ifndef VCO_HARNESS_FIXTURE_OBSERVER_H
#define VCO_HARNESS_FIXTURE_OBSERVER_H
#include "fixture.h"
#include <stdbool.h>

typedef struct {
    uint32_t generation, data_address, function_address, data;
    int32_t uid, function;
    uint16_t instruction, return_instruction;
    bool called;
} VcoFixtureReading;
typedef enum {
    VCO_FIXTURE_OBSERVE_NONE,
    VCO_FIXTURE_OBSERVE_IDENTITY,
    VCO_FIXTURE_OBSERVE_DATA,
    VCO_FIXTURE_OBSERVE_CODE,
    VCO_FIXTURE_OBSERVE_RETURN,
    VCO_FIXTURE_OBSERVE_LIMIT
} VcoFixtureObserveError;
typedef struct {
    VcoFixtureReading first_failure;
    uint64_t samples;
    uint32_t origin_generation, last_generation, origin_data, origin_function;
    uint32_t expected_data, expected_function;
    unsigned generations, reused_data, reused_function;
    VcoFixtureObserveError error;
    bool active;
} VcoFixtureObserver;

/* App-thread owned, no native calls or allocations. Initialize once. A failure
 * is latched until app exit; stop/begin never clear it. Successful samples are
 * observations, not evidence of a module lifetime lock or complete coverage. */
void vco_fixture_observer_init(VcoFixtureObserver *);
int vco_fixture_observer_begin(VcoFixtureObserver *, const VcoFixtureController *);
void vco_fixture_observer_stop(VcoFixtureObserver *);
/* Check the copied identity/data/opcodes before calling the owned test target.
 * The initial READY instance may already have the known A patches (88/9).
 * All later generations must retain their own initial values and opcodes.
 * While inactive, known current-instance patches are allowed for manual tests.
 * This does not make observation and subsequent execution atomic. */
bool vco_fixture_observer_callable(const VcoFixtureObserver *, const VcoFixtureController *,
                                   const VcoFixtureReading *);
/* A complete reading includes the actual function result. On the first bad
 * reading, keep its exact values and expected baseline, stop monitoring, and
 * reject future observations/begin attempts without altering the evidence. */
int vco_fixture_observer_sample(VcoFixtureObserver *, const VcoFixtureController *,
                                const VcoFixtureReading *);
#endif
