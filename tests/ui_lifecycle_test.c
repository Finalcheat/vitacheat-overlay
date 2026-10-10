#include "ui/lifecycle.h"
#include "input/bridge_policy.h"
#include "input_state.h"
#include <assert.h>
#include <stdio.h>

static VcoCatalog catalog;
static VcoExecutionView execution;
static VcoUiLifecycle lifecycle;
static void ready(uint64_t session)
{
    catalog.state = VCO_LIST_READY;
    catalog.execution_session = session;
    execution = (VcoExecutionView){.session = session,
                                    .list_revision = 5,
                                    .catalog_revision = catalog.revision,
                                    .state = VCO_EXECUTION_READY};
}
static void identity(void)
{
    catalog = (VcoCatalog){.revision = 1,
                           .game = {.pid = 77, .app_id = 177, .title_id = "PCSE00001"},
                           .state = VCO_LIST_LOADING};
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &catalog)); /* First discovery never closes. */
    execution = (VcoExecutionView){.catalog_revision = 1, .state = VCO_EXECUTION_NO_GAME};
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    ready(10);
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &catalog));
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    VcoCatalog unknown = {.state = VCO_LIST_DISCOVERY_ERROR};
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &unknown));
    execution.state = VCO_EXECUTION_BACKGROUND;
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    execution.session = 0;
    execution.state = VCO_EXECUTION_LOADING;
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    assert(lifecycle.session == 10); /* UNKNOWN/loading cannot erase the instance baseline. */
    ++catalog.revision;
    catalog.state = VCO_LIST_LOADING;
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &catalog));
    ready(10);
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution)); /* Explicit reload. */
    ++catalog.revision;
    catalog.game.pid = 88;
    catalog.game.app_id = 188;
    assert(vco_ui_lifecycle_catalog(&lifecycle, &catalog));
    execution.catalog_revision = catalog.revision;
    execution.state = VCO_EXECUTION_LOADING;
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution)); /* Old target draining. */
    ready(11);
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution)); /* Already hidden. */
    execution.state = VCO_EXECUTION_NO_GAME;
    execution.session = 0;
    assert(vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    catalog.state = VCO_LIST_NO_GAME;
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &catalog)); /* No late second close in Shell. */
    ++catalog.revision;
    ready(12);
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &catalog));
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    execution.session = 0;
    execution.state = VCO_EXECUTION_LOADING;
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    execution.session = 13; /* Same PID/app/title but a verified replacement instance. */
    assert(vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    ++catalog.revision;
    ready(13);
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &catalog));
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution));
    --execution.catalog_revision;
    execution.state = VCO_EXECUTION_NO_GAME;
    assert(!vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution)); /* Late old view. */
    catalog.state = VCO_LIST_NO_GAME;
    assert(vco_ui_lifecycle_catalog(&lifecycle, &catalog));
    assert(!vco_ui_lifecycle_catalog(&lifecycle, &catalog));
}
static int capture_result, mode = VCO_CAPTURE_ACTIVE;
static unsigned polls;
static uint32_t buttons = VCO_COMBO, now = 100;
static int capture(int value)
{
    if (capture_result)
        return capture_result;
    mode = value;
    return 0;
}
static int poll_input(VcoInput *out)
{
    ++polls;
    *out = (VcoInput){VCO_ABI, buttons, now, mode != VCO_CAPTURE_OFF, 128, 128, 128, 128};
    return 0;
}
static int poll_touch(VcoTouchSample *out)
{
    *out = (VcoTouchSample){.abi = VCO_TOUCH_ABI};
    return 0;
}
static void release_input(void)
{
    VcoInputState input = {.open = true, .seeded = true, .last_tick = now};
    VcoInputBridge bridge = {0};
    VcoInputBridgeOps ops = {poll_input, poll_touch, capture};
    VcoInput raw;
    VcoTouchSample touch;
    vco_input_forced_closed(&input);
    assert(!input.open && !input.draining && input.wait_neutral && !input.seeded);
    vco_input_bridge_release(&bridge, VCO_CAPTURE_OFF, now);
    capture_result = VCO_INPUT_BUSY;
    for (unsigned i = 0; i < 40; ++i) {
        now += 16000;
        assert(vco_input_bridge_poll(&bridge, now, true, ops, &raw, &touch) == VCO_INPUT_WAIT);
        assert(!polls && bridge.release_pending && bridge.release_mode == VCO_CAPTURE_OFF);
        assert(!vco_input_bridge_open(&bridge, ops));
    }
    capture_result = 0;
    assert(vco_input_bridge_poll(&bridge, now, true, ops, &raw, &touch) == VCO_INPUT_READY);
    assert(mode == VCO_CAPTURE_OFF && !bridge.release_pending);
    assert(vco_input_step(&input, &raw) == VCO_NONE); /* Held original combo cannot reopen. */
    now += 16000;
    assert(!poll_input(&raw));
    assert(vco_input_step(&input, &raw) == VCO_NONE && input.wait_neutral);
    buttons = 0;
    now += 16000;
    assert(!poll_input(&raw));
    assert(vco_input_step(&input, &raw) == VCO_NONE && !input.wait_neutral);
    buttons = VCO_COMBO;
    now += 16000;
    assert(!poll_input(&raw));
    assert(vco_input_step(&input, &raw) == VCO_OPEN); /* A fresh explicit Shell open is allowed. */
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "identity"))
        identity();
    else if (!strcmp(argv[1], "release"))
        release_input();
    else
        assert(!"unknown UI lifecycle case");
    puts("UI lifecycle passed");
    return 0;
}
