#ifndef VCO_INPUT_BRIDGE_POLICY_H
#define VCO_INPUT_BRIDGE_POLICY_H
#include "../bridge.h"
#include "touch.h"
#include <string.h>

typedef struct {
    int (*poll)(VcoInput *);
    int (*touch)(VcoTouchSample *);
    int (*capture)(int);
} VcoInputBridgeOps;
typedef struct {
    uint32_t release_started;
    int release_mode;
    bool release_pending, touch_first;
} VcoInputBridge;
typedef enum { VCO_INPUT_READY, VCO_INPUT_WAIT, VCO_INPUT_FAILED } VcoInputBridgeResult;

/* At most one attempt per frame. Pending release blocks Poll, whose successful
 * ACTIVE sampling renews the kernel lease. A failed release therefore cannot
 * keep an invisible overlay capturing forever. OFF always supersedes DRAIN. */
static inline bool vco_input_bridge_retry(VcoInputBridge *state, uint32_t now,
                                          VcoInputBridgeOps ops)
{
    if (!state->release_pending)
        return true;
    if (state->release_mode == VCO_CAPTURE_DRAIN &&
        (uint32_t)(now - state->release_started) >= VCO_DRAIN_US)
        state->release_mode = VCO_CAPTURE_OFF;
    if (ops.capture(state->release_mode) != 0)
        return false;
    state->release_pending = false;
    return true;
}
static inline void vco_input_bridge_release(VcoInputBridge *state, int mode, uint32_t now)
{
    if (mode != VCO_CAPTURE_OFF && mode != VCO_CAPTURE_DRAIN)
        return;
    if (!state->release_pending) {
        state->release_pending = true;
        state->release_started = now;
        state->release_mode = mode;
    } else if (mode == VCO_CAPTURE_OFF)
        state->release_mode = VCO_CAPTURE_OFF;
    state->touch_first = false;
}
static inline bool vco_input_bridge_open(const VcoInputBridge *state, VcoInputBridgeOps ops)
{
    return !state->release_pending && ops.capture(VCO_CAPTURE_ACTIVE) == 0;
}
static inline bool vco_input_bridge_touch_valid(const VcoTouchSample *sample)
{
    return sample->abi == VCO_TOUCH_ABI && (sample->valid & sample->available) == sample->available;
}
static inline VcoInputBridgeResult vco_input_bridge_poll(VcoInputBridge *state, uint32_t now,
                                                         bool need_touch, VcoInputBridgeOps ops,
                                                         VcoInput *raw, VcoTouchSample *sample)
{
    memset(raw, 0, sizeof(*raw));
    memset(sample, 0, sizeof(*sample));
    sample->abi = VCO_TOUCH_ABI;
    if (!vco_input_bridge_retry(state, now, ops))
        return VCO_INPUT_WAIT;
    bool have_touch = false;
    if (state->touch_first && need_touch) {
        int result = ops.touch(sample);
        if (result == VCO_INPUT_BUSY)
            return VCO_INPUT_WAIT;
        if (result != 0 || !vco_input_bridge_touch_valid(sample))
            return VCO_INPUT_FAILED;
        have_touch = true;
    }
    state->touch_first = false;
    int result = ops.poll(raw);
    if (result == VCO_INPUT_BUSY)
        return VCO_INPUT_WAIT;
    if (result != 0 || raw->abi != VCO_ABI)
        return VCO_INPUT_FAILED;
    if (need_touch && !have_touch) {
        result = ops.touch(sample);
        if (result == VCO_INPUT_BUSY) {
            /* Retry touch before the next Poll. Sustained touch contention
             * cannot continually renew ACTIVE while no input is processed. */
            state->touch_first = true;
            return VCO_INPUT_WAIT;
        }
        if (result != 0 || !vco_input_bridge_touch_valid(sample))
            return VCO_INPUT_FAILED;
    }
    return VCO_INPUT_READY;
}
#endif
