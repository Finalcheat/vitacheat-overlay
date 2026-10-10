#ifndef VCO_HARNESS_FIXTURE_H
#define VCO_HARNESS_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#define VCO_FIXTURE_ABI UINT32_C(0x00010000)
/* Start arguments may be copied by Modulemgr. Return through the explicit
 * same-process pointer, never by mutating the copied start envelope. */
typedef struct {
    uint32_t abi, size, generation, variant;
    struct VcoFixtureReply *reply;
} VcoFixtureStart;
typedef struct VcoFixtureReply {
    uint32_t abi, size, generation, variant;
    uint32_t data_address, function_address, data_initial, function_initial;
    int32_t module_uid;
} VcoFixtureReply;
typedef struct {
    uint32_t base, size, permissions;
} VcoFixtureSegment;
typedef struct {
    int32_t uid;
    uint32_t state, index;
    VcoFixtureSegment text, data;
} VcoFixtureInfo;
typedef struct {
    void *context;
    int32_t (*load)(void *, unsigned variant);
    int (*start)(void *, int32_t uid, const VcoFixtureStart *, int *status);
    int (*info)(void *, int32_t uid, VcoFixtureInfo *);
    int (*stop_unload)(void *, int32_t uid, int *status);
} VcoFixtureOps;
typedef enum {
    VCO_FIXTURE_EMPTY,
    VCO_FIXTURE_LOADING,
    VCO_FIXTURE_READY,
    VCO_FIXTURE_UNLOADING,
    VCO_FIXTURE_FAULT
} VcoFixtureState;
typedef enum {
    VCO_FIXTURE_ERROR_NONE,
    VCO_FIXTURE_ERROR_LOAD,
    VCO_FIXTURE_ERROR_START,
    VCO_FIXTURE_ERROR_INFO,
    VCO_FIXTURE_ERROR_CONTRACT,
    VCO_FIXTURE_ERROR_UNLOAD
} VcoFixtureError;
typedef struct {
    VcoFixtureOps ops;
    VcoFixtureReply reply;
    VcoFixtureInfo info;
    int32_t uid, native_result, module_status;
    uint32_t generation;
    unsigned next_variant, loads, unloads;
    VcoFixtureState state;
    VcoFixtureError error;
} VcoFixtureController;
/* Single app-thread ownership. Do not reinitialize after native ownership was
 * acquired. A failed start/info/unload retains UID and enters FAULT: no retry,
 * no further module calls/function pointers. Exit this test app to retire it. */
void vco_fixture_init(VcoFixtureController *, VcoFixtureOps);
int vco_fixture_load(VcoFixtureController *);
int vco_fixture_unload(VcoFixtureController *);
/* One explicit stress step: unload when ready, load the other variant when
 * empty. No cheat request, sleep, file write or implicit recovery occurs. */
int vco_fixture_step(VcoFixtureController *);
/* Complete alternative .psv with three dynamic-module tests, all _V0. Only a
 * READY instance can generate it; never keep an address from an unloaded UID.
 * Returns positive complete length or -1; clears writable output on failure. */
int vco_fixture_codes(const VcoFixtureController *, char *, size_t);
#endif
