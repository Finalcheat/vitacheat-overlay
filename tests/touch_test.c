#include <assert.h>
#include <string.h>
#include "input/touch.h"
#include "input_state.h"
static VcoTouchState state;
static VcoTouchSample sample;
static VcoTouchLayout layout;
static uint64_t context;
static void setup(void)
{
    vco_touch_reset(&state);
    sample = (VcoTouchSample){0};
    sample.abi = VCO_TOUCH_ABI;
    sample.available = sample.valid = 3;
    sample.tick = 0xffff0000u;
    layout = (VcoTouchLayout){false, false, 0, 6};
    context = 1;
}
static VcoTouchEvent feed(unsigned count, unsigned x, unsigned y)
{
    sample.tick += 16000;
    sample.front_count = count;
    sample.x = (uint16_t)x;
    sample.y = (uint16_t)y;
    return vco_touch_step(&state, &sample, layout, context);
}
static void arm(void)
{
    assert(feed(0, 0, 0).action == VCO_TOUCH_NONE);
}
static void mapping(void)
{
    uint16_t pixel;
    assert(vco_touch_map(0, 0, 1919, 960, &pixel) && pixel == 0);
    assert(vco_touch_map(1919, 0, 1919, 960, &pixel) && pixel == 959);
    assert(vco_touch_map(1087, 0, 1087, 544, &pixel) && pixel == 543);
    assert(vco_touch_map(40, 40, 1919, 960, &pixel) && pixel == 0);
    assert(!vco_touch_map(-1, 0, 1919, 960, &pixel));
    assert(!vco_touch_map(1920, 0, 1919, 960, &pixel));
    assert(!vco_touch_map(0, 0, 0, 960, &pixel));
    assert(!vco_touch_map(0, 0, 1919, 0, &pixel));
    setup();
    for (unsigned row = 0; row < 6; ++row) {
        unsigned y = VCO_ROW_TOP + row * VCO_ROW_STEP;
        VcoTouchEvent event = vco_touch_hit(60, y, layout);
        assert(event.action == VCO_TOUCH_SELECT && event.row == row);
        event = vco_touch_hit(850, y + 45, layout);
        assert(event.action == VCO_TOUCH_TOGGLE && event.row == row);
        assert(vco_touch_hit(850, y + 46, layout).action == VCO_TOUCH_NONE);
    }
    layout.count = 2;
    assert(vco_touch_hit(850, VCO_ROW_TOP + 2 * VCO_ROW_STEP, layout).action == VCO_TOUCH_NONE);
    assert(vco_touch_hit(600, 20, layout).action == VCO_TOUCH_SETTINGS);
    assert(vco_touch_hit(900, 20, layout).action == VCO_TOUCH_BACK_ACTION);
    assert(vco_touch_hit(750, 110, layout).action == VCO_TOUCH_PREV);
    assert(vco_touch_hit(900, 110, layout).action == VCO_TOUCH_NEXT);
    assert(vco_touch_hit(960, 150, layout).action == VCO_TOUCH_NONE);
    layout = (VcoTouchLayout){true, false, 0, VCO_SETTING_COUNT};
    assert(vco_touch_hit(810, 170, layout).action == VCO_TOUCH_DECREASE);
    assert(vco_touch_hit(900, 170, layout).action == VCO_TOUCH_INCREASE);
    layout.first = VCO_SETTING_RESET - 5;
    VcoTouchEvent event = vco_touch_hit(100, VCO_ROW_TOP + 5 * VCO_ROW_STEP, layout);
    assert(event.action == VCO_TOUCH_ACCEPT && event.row == VCO_SETTING_RESET);
    layout.first = VCO_SETTING_ENCODING;
    event = vco_touch_hit(810, VCO_ROW_TOP, layout);
    assert(event.action == VCO_TOUCH_DECREASE && event.row == VCO_SETTING_ENCODING);
    event = vco_touch_hit(900, VCO_ROW_TOP, layout);
    assert(event.action == VCO_TOUCH_INCREASE && event.row == VCO_SETTING_ENCODING);
    layout = (VcoTouchLayout){true, true, 0, 3};
    assert(vco_touch_hit(100, VCO_ROW_TOP + VCO_ROW_STEP, layout).action == VCO_TOUCH_ACCEPT);
    assert(vco_touch_hit(900, 110, layout).action == VCO_TOUCH_NONE);
}
static void gestures(void)
{
    setup();
    assert(feed(1, 850, 170).action == VCO_TOUCH_NONE); /* held when opening */
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    assert(feed(1, 850, 170).action == VCO_TOUCH_NONE);
    assert(feed(0, 850, 170).action == VCO_TOUCH_TOGGLE);
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    assert(feed(1, 850, 170).action == VCO_TOUCH_NONE);
    assert(feed(2, 850, 170).action == VCO_TOUCH_NONE);
    assert(feed(1, 850, 170).action == VCO_TOUCH_NONE);
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    feed(1, 850, 170);
    ++context;
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    feed(1, 850, 170);
    ++sample.id;
    feed(1, 850, 170);
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    feed(1, 850, 170);
    feed(1, 850, 220);
    feed(1, 850, 170);
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE); /* out and back is not a tap */
    feed(1, 600, 220);
    feed(1, 490, 220);
    assert(feed(0, 490, 220).action == VCO_TOUCH_NEXT);
    feed(1, 600, 220);
    feed(1, 710, 220);
    assert(feed(0, 710, 220).action == VCO_TOUCH_PREV);
    feed(1, 850, 170);
    for (unsigned i = 0; i < 51; ++i)
        feed(1, 850, 170);
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    feed(1, 850, 170);
    sample.tick += 500000;
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    arm();
    feed(1, 850, 170);
    sample.valid = 0;
    assert(feed(0, 850, 170).action == VCO_TOUCH_NONE);
    sample.valid = 3;
    arm();
    layout = (VcoTouchLayout){true, false, 0, VCO_SETTING_COUNT};
    feed(1, 400, 350);
    feed(1, 400, 260);
    assert(feed(0, 400, 260).action == VCO_TOUCH_DOWN);
    feed(1, 400, 260);
    feed(1, 400, 350);
    assert(feed(0, 400, 350).action == VCO_TOUCH_UP);
}
static void release_guard(void)
{
    setup();
    assert(vco_touch_neutral(&sample));
    sample.front_count = 1;
    assert(!vco_touch_neutral(&sample));
    sample.front_count = 0;
    sample.back_count = 1;
    assert(!vco_touch_neutral(&sample));
    VcoInput raw = {
        .abi = VCO_ABI, .tick = 1, .captured = 1, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
    VcoInputState input = {.seeded = true, .draining = true, .last_tick = 1};
    VcoSettings settings = vco_settings_defaults();
    assert(vco_input_step_with_touch(&input, &raw, false, &settings, vco_touch_neutral(&sample)) ==
           VCO_NONE);
    assert(input.draining);
    sample.back_count = 0;
    assert(vco_input_step_with_touch(&input, &raw, false, &settings, vco_touch_neutral(&sample)) ==
           VCO_RELEASE);
    input.open = true;
    input.wait_neutral = true;
    sample.front_count = 1;
    assert(vco_input_step_with_touch(&input, &raw, false, &settings, vco_touch_neutral(&sample)) ==
           VCO_NONE);
    assert(input.wait_neutral);
    sample.front_count = 0;
    sample.valid = 1;
    assert(!vco_touch_neutral(&sample));
    raw.buttons = VCO_PS;
    assert(vco_input_step_with_touch(&input, &raw, false, &settings, false) == VCO_NONE);
    raw.buttons = 0;
    raw.tick += 16000;
    assert(vco_input_step_with_touch(&input, &raw, false, &settings, false) == VCO_CLOSE);
    sample.available = sample.valid = 0;
    assert(vco_touch_neutral(&sample)); /* no panel */
}
static void slider(void)
{
    setup();
    layout = (VcoTouchLayout){true, false, 0, VCO_SETTING_COUNT};
    unsigned y = (unsigned)vco_touch_row_center(1);
    /* Touch and drawing share endpoints; the quantizer rounds to five and
     * clamps captured drags past either endpoint without unsigned overflow. */
    unsigned previous = 0;
    for (unsigned x = 0; x < VCO_SCREEN_WIDTH; ++x) {
        unsigned value = vco_touch_opacity(x);
        assert(value <= 100 && value % 5 == 0 && value >= previous);
        previous = value;
    }
    assert(vco_touch_opacity(520) == 0 && vco_touch_opacity(644) == 50 &&
           vco_touch_opacity(768) == 100 && vco_touch_opacity(UINT32_MAX) == 100);
    for (unsigned value = 0; value <= 100; value += 5) {
        unsigned x = VCO_OPACITY_LEFT + (VCO_OPACITY_RIGHT - VCO_OPACITY_LEFT) * value / 100;
        assert(vco_touch_opacity(x) == value);
        VcoTouchEvent hit = vco_touch_hit(x, y, layout);
        assert(hit.action == VCO_TOUCH_OPACITY && hit.row == VCO_SETTING_OPACITY &&
               hit.value == value);
    }
    assert(vco_touch_hit(495, y, layout).action == VCO_TOUCH_SELECT);
    assert(vco_touch_hit(790, y, layout).action == VCO_TOUCH_DECREASE);
    assert(vco_touch_hit(900, y, layout).action == VCO_TOUCH_INCREASE);
    assert(feed(1, 644, y).action == VCO_TOUCH_NONE); /* held on entry */
    arm();
    VcoTouchEvent hit = feed(1, 644, y);
    assert(hit.action == VCO_TOUCH_OPACITY && hit.value == 50);
    assert(feed(1, 644, y).action == VCO_TOUCH_NONE); /* no redraw at rest */
    hit = feed(1, 100, y);
    assert(hit.action == VCO_TOUCH_OPACITY && hit.value == 0);
    for (unsigned i = 0; i < 100; ++i)
        assert(feed(1, 100, y).action == VCO_TOUCH_NONE); /* not a timed tap */
    hit = feed(1, 940, y);
    assert(hit.action == VCO_TOUCH_OPACITY && hit.value == 100);
    assert(feed(0, 940, y).action == VCO_TOUCH_NONE); /* no duplicate / swipe */
    assert(feed(0, 940, y).action == VCO_TOUCH_NONE);
    layout.first = 1;
    hit = feed(1, 644, vco_touch_row_center(0));
    assert(hit.action == VCO_TOUCH_OPACITY && hit.value == 50 && state.first == 1);
    assert(feed(0, 644, vco_touch_row_center(0)).action == VCO_TOUCH_NONE);
    layout.confirm = true;
    assert(vco_touch_hit(644, y, layout).action == VCO_TOUCH_ACCEPT);
}
static void slider_cancel(void)
{
    for (unsigned fault = 0; fault < 8; ++fault) {
        setup();
        layout = (VcoTouchLayout){true, false, 0, VCO_SETTING_COUNT};
        unsigned y = (unsigned)vco_touch_row_center(1);
        arm();
        assert(feed(1, 644, y).action == VCO_TOUCH_OPACITY);
        if (fault == 0)
            ++context;
        else if (fault == 1)
            ++sample.id;
        else if (fault == 2)
            assert(feed(2, 644, y).action == VCO_TOUCH_NONE);
        else if (fault == 3)
            sample.valid = 0;
        else if (fault == 4)
            sample.tick += 500000;
        else if (fault == 5)
            assert(feed(1, 644, y + 80).action == VCO_TOUCH_NONE);
        else if (fault == 6)
            layout.confirm = true;
        else
            assert(feed(1, 960, y).action == VCO_TOUCH_NONE);
        assert(feed(1, 768, y).action == VCO_TOUCH_NONE);
        sample.valid = 3;
        layout.confirm = false;
        assert(feed(1, 520, y).action == VCO_TOUCH_NONE);
        assert(feed(0, 520, y).action == VCO_TOUCH_NONE);
        assert(feed(1, 520, y).action == VCO_TOUCH_OPACITY);
        assert(feed(0, 520, y).action == VCO_TOUCH_NONE);
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "mapping"))
        mapping();
    else if (!strcmp(argv[1], "gestures"))
        gestures();
    else if (!strcmp(argv[1], "release"))
        release_guard();
    else if (!strcmp(argv[1], "slider"))
        slider();
    else if (!strcmp(argv[1], "slider_cancel"))
        slider_cancel();
    else
        assert(0);
    return 0;
}
