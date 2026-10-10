#ifndef VCO_HARNESS_OBSERVATIONS_H
#define VCO_HARNESS_OBSERVATIONS_H
#include "codes.h"
#include "fixture_observer.h"

#define VCO_HARNESS_OBSERVATION_BYTES 2048u
#define VCO_HARNESS_OBSERVATION_LIMIT 60u
#define VCO_HARNESS_LOG_LIMIT 65536u
#define VCO_HARNESS_OBSERVATION_LOG "ux0:vitacheat/vco-harness-observations.jsonl"
/* data[] order: direct32/16/8, copy, repeat[3], pointer, source,
 * target_a[3], target_b[3], condition, pad, patch, module_marker.
 * These are observations, never an inferred VCO/VitaCheat ON/OFF status. */
#define VCO_HARNESS_DATA_VALUES 19u
typedef struct {
    uint64_t run, tick;
    uint32_t sequence, buttons, page, source;
    int32_t pid, generated, fixture_generated;
    uint32_t data[VCO_HARNESS_DATA_VALUES], bulk[3];
    uint16_t rx[2], thumb32[4];
    int32_t rx_result, thumb32_result;
    VcoFixtureController fixture;
    VcoFixtureReading reading;
    VcoFixtureObserver witness;
} VcoHarnessObservation;
/* Complete self-contained JSON line, or -1 with a cleared writable output.
 * No SDK calls, allocations, target reads/calls or state changes occur here. */
int vco_harness_observation(char *, size_t, const VcoHarnessObservation *);
/* Append one complete observation to a bounded file. An incomplete old tail,
 * a full file or any I/O error rejects further appends; never erase evidence. */
int vco_harness_observation_save(const char *, const VcoHarnessObservation *);
#endif
