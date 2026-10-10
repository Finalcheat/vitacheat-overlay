#include "input/bridge_policy.h"
#include "input_state.h"
#include "capture_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static VcoInputBridge bridge;
static VcoInput raw;
static VcoTouchSample touch;
static int poll_result, touch_result, capture_result, kernel_mode;
static unsigned polls, touches, captures;
static uint32_t now, pulse;
static bool broken_touch;
static int poll_input(VcoInput *out)
{
    ++polls;
    if (poll_result) {
        memset(out, 0xa5, sizeof(*out) / 2);
        return poll_result;
    }
    if (vco_capture_expired((uint32_t)kernel_mode, now - pulse))
        kernel_mode = VCO_CAPTURE_OFF;
    *out = (VcoInput){VCO_ABI, 0, now, kernel_mode != VCO_CAPTURE_OFF, 128, 128, 128, 128};
    if (vco_capture_renewable((uint32_t)kernel_mode))
        pulse = now;
    return 0;
}
static int poll_touch(VcoTouchSample *out)
{
    ++touches;
    if (touch_result) {
        memset(out, 0xa5, sizeof(*out) / 2);
        return touch_result;
    }
    *out = (VcoTouchSample){.abi = VCO_TOUCH_ABI,
                            .available = VCO_TOUCH_FRONT,
                            .valid = broken_touch ? 0 : VCO_TOUCH_FRONT};
    return 0;
}
static int capture(int mode)
{
    ++captures;
    if (capture_result)
        return capture_result;
    kernel_mode = mode;
    pulse = now;
    return 0;
}
static const VcoInputBridgeOps ops = {poll_input, poll_touch, capture};
static VcoInputBridgeResult sample(bool need_touch)
{
    return vco_input_bridge_poll(&bridge, now, need_touch, ops, &raw, &touch);
}
static void busy(void)
{
    assert(vco_input_bridge_open(&bridge, ops));
    VcoInputState input = {.seeded = true, .open = true, .last_tick = now};
    VcoInputState unchanged = input;
    poll_result = VCO_INPUT_BUSY;
    now += 16000;
    assert(sample(true) == VCO_INPUT_WAIT && polls == 1 && !touches);
    assert(!memcmp(&input, &unchanged, sizeof(input))); /* Caller skips the complete frame. */
    poll_result = 0;
    touch_result = VCO_INPUT_BUSY;
    assert(sample(true) == VCO_INPUT_WAIT && polls == 2 && touches == 1 && bridge.touch_first);
    now += 16000;
    assert(sample(true) == VCO_INPUT_WAIT && polls == 2 && touches == 2);
    touch_result = 0;
    now += 16000;
    assert(sample(true) == VCO_INPUT_READY && polls == 3 && touches == 3 && !bridge.touch_first);
    VcoSettings mapping = vco_settings_defaults();
    assert(vco_input_step_with_touch(&input, &raw, false, &mapping, vco_touch_neutral(&touch)) ==
               VCO_NONE &&
           input.open);
    broken_touch = true;
    assert(sample(true) == VCO_INPUT_FAILED);
    broken_touch = false;
    poll_result = -1;
    assert(sample(false) == VCO_INPUT_FAILED);
    capture_result = VCO_INPUT_BUSY;
    assert(!vco_input_bridge_open(&bridge, ops));
}
static void release_case(void)
{
    assert(vco_input_bridge_open(&bridge, ops));
    capture_result = VCO_INPUT_BUSY;
    vco_input_bridge_release(&bridge, VCO_CAPTURE_DRAIN, now);
    for (unsigned i = 0; i < 20; ++i) {
        now += 16000;
        assert(sample(true) == VCO_INPUT_WAIT && !polls && !touches);
        assert(!vco_input_bridge_open(&bridge, ops));
    }
    assert(bridge.release_pending && bridge.release_mode == VCO_CAPTURE_OFF);
    vco_input_bridge_release(&bridge, VCO_CAPTURE_DRAIN, now);
    assert(bridge.release_mode == VCO_CAPTURE_OFF); /* Never downgrade a pending OFF. */
    capture_result = -1;
    now += VCO_LEASE_US;
    assert(sample(false) == VCO_INPUT_WAIT && bridge.release_pending && !polls);
    assert(vco_capture_expired((uint32_t)kernel_mode, now - pulse));
    capture_result = 0;
    assert(sample(false) == VCO_INPUT_READY && !bridge.release_pending && !raw.captured);
    assert(vco_input_bridge_open(&bridge, ops));
    capture_result = VCO_INPUT_BUSY;
    vco_input_bridge_release(&bridge, VCO_CAPTURE_DRAIN, now);
    vco_input_bridge_release(&bridge, VCO_CAPTURE_OFF, now + 1);
    assert(bridge.release_mode == VCO_CAPTURE_OFF);
}
static void lease(void)
{
    now = UINT32_MAX - 100;
    assert(vco_input_bridge_open(&bridge, ops));
    VcoInputState input = {.seeded = true, .open = true, .last_tick = now};
    touch_result = VCO_INPUT_BUSY;
    assert(sample(true) == VCO_INPUT_WAIT);
    uint32_t original_pulse = pulse;
    for (unsigned i = 0; i < 40; ++i) {
        now += 16000;
        assert(sample(true) == VCO_INPUT_WAIT && polls == 1 && pulse == original_pulse);
    }
    touch_result = 0;
    assert(sample(true) == VCO_INPUT_READY && !raw.captured);
    assert(vco_input_step(&input, &raw) == VCO_CLOSE && !input.open);
    vco_input_bridge_release(&bridge, VCO_CAPTURE_OFF, now);
    assert(vco_input_bridge_retry(&bridge, now, ops) && kernel_mode == VCO_CAPTURE_OFF);
    /* A successful DRAIN cannot be renewed by subsequent Poll calls. */
    assert(vco_input_bridge_open(&bridge, ops));
    vco_input_bridge_release(&bridge, VCO_CAPTURE_DRAIN, now);
    assert(sample(true) == VCO_INPUT_READY && kernel_mode == VCO_CAPTURE_DRAIN);
    original_pulse = pulse;
    now += VCO_DRAIN_US;
    assert(sample(true) == VCO_INPUT_READY && !raw.captured && pulse == original_pulse);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "busy"))
        busy();
    else if (!strcmp(argv[1], "release"))
        release_case();
    else if (!strcmp(argv[1], "lease"))
        lease();
    else
        assert(!"unknown case");
    puts("input bridge policy passed");
    return 0;
}
