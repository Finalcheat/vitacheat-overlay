#include <assert.h>
#include <stdio.h>
#include "input_state.h"
#include "capture_policy.h"

typedef struct {
    VcoInputState state;
    VcoInput raw;
    VcoSettings mapping;
} Rig;
static Rig rig(bool open, uint32_t tick)
{
    Rig value = {0};
    value.state.seeded = open;
    value.state.open = open;
    value.state.last_tick = tick;
    value.raw = (VcoInput){
        .abi = VCO_ABI, .tick = tick, .captured = open, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
    value.mapping = vco_settings_defaults();
    return value;
}
static VcoAction sample(Rig *r, uint32_t buttons, bool settings, bool touch_neutral, bool paused)
{
    r->raw.buttons = buttons;
    r->raw.tick += 16000;
    return vco_input_step_with_capture(&r->state, &r->raw, settings, &r->mapping, touch_neutral,
                                       paused);
}
static VcoAction step(Rig *r, uint32_t buttons)
{
    return sample(r, buttons, false, true, false);
}
static void default_actions(void)
{
    Rig r = rig(false, 0xffff0000u);
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_COMBO) == VCO_OPEN && r.state.wait_neutral);
    r.state.open = true;
    r.raw.captured = 1;
    assert(step(&r, VCO_COMBO | VCO_R) == VCO_NONE);
    assert(step(&r, VCO_L) == VCO_NONE);
    assert(step(&r, 0) == VCO_NONE && !r.state.wait_neutral);
    assert(step(&r, VCO_SQUARE) == VCO_TOGGLE);
    assert(step(&r, VCO_SQUARE) == VCO_NONE); /* Held keys do not repeat, including timer wrap. */
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_L) == VCO_NONE && r.state.pending_button == VCO_L);
    assert(step(&r, 0) == VCO_PAGE_PREV);
    assert(step(&r, VCO_R) == VCO_PAGE_NEXT);
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_START) == VCO_NONE);
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_TRIANGLE) == VCO_SETTINGS);
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_CIRCLE) == VCO_NONE);
    assert(step(&r, VCO_CROSS) == VCO_CLOSE && !r.state.open && r.state.draining);
    assert(step(&r, VCO_CROSS) == VCO_NONE);
    assert(step(&r, 0) == VCO_RELEASE && !r.state.draining);
}
static void opening_gate(void)
{
    Rig r = rig(false, 1000000);
    r.state.wait_neutral = true; /* The caller also uses this on startup/new settings. */
    assert(step(&r, VCO_COMBO) == VCO_NONE);
    assert(step(&r, VCO_L) == VCO_NONE);
    r.raw.lx = 0;
    assert(step(&r, 0) == VCO_NONE && r.state.wait_neutral);
    r.raw.lx = 128;
    assert(sample(&r, 0, false, false, false) == VCO_NONE && r.state.wait_neutral);
    assert(step(&r, 0) == VCO_NONE && !r.state.wait_neutral);
    assert(step(&r, VCO_L) == VCO_NONE);
    assert(step(&r, VCO_COMBO) == VCO_OPEN && r.state.wait_neutral);
    r.state.open = true;
    r.raw.captured = 1;
    assert(step(&r, VCO_COMBO | VCO_SQUARE) == VCO_NONE);
    assert(step(&r, VCO_SQUARE) == VCO_NONE);
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_SQUARE) == VCO_TOGGLE);
    r = rig(false, 0);
    assert(step(&r, VCO_COMBO) == VCO_NONE); /* First sample only seeds edges. */
    assert(step(&r, VCO_COMBO) == VCO_NONE);
    assert(step(&r, VCO_COMBO | VCO_PS) == VCO_NONE && !r.state.open);
}
static void regional_actions(void)
{
    for (unsigned mode = 0; mode <= VCO_BUTTON_WESTERN; ++mode) {
        VcoSettings mapping = vco_settings_defaults();
        vco_settings_set_button_mode(&mapping, (VcoButtonMode)mode);
        mapping.bindings.open = VCO_R | VCO_SELECT;
        assert(vco_settings_valid(&mapping));
        const uint32_t bound[] = {VCO_L, VCO_R, mapping.bindings.toggle, mapping.bindings.close,
                                  VCO_TRIANGLE};
        const VcoAction actions[] = {VCO_PAGE_PREV, VCO_PAGE_NEXT, VCO_TOGGLE, VCO_CLOSE,
                                     VCO_SETTINGS};
        for (unsigned i = 0; i < 5; ++i) {
            Rig r = rig(true, 0);
            r.mapping = mapping;
            if (bound[i] & mapping.bindings.open) {
                assert(step(&r, bound[i]) == VCO_NONE);
                assert(step(&r, 0) == actions[i]);
            } else
                assert(step(&r, bound[i]) == actions[i]);
        }
        Rig r = rig(true, 0);
        r.mapping = mapping;
        assert(sample(&r, vco_settings_confirm_button(&mapping), true, true, false) == VCO_TOGGLE);
        assert(sample(&r, 0, true, true, false) == VCO_NONE);
        assert(sample(&r, mapping.bindings.close, true, true, false) == VCO_BACK && r.state.open);
        assert(sample(&r, 0, true, true, false) == VCO_NONE);
        assert(sample(&r, VCO_SQUARE, true, true, false) == VCO_NONE);
        assert(sample(&r, VCO_START, true, true, false) == VCO_NONE);
        assert(sample(&r, VCO_LEFT, true, true, false) == VCO_ADJUST_LEFT);
        assert(sample(&r, VCO_RIGHT, true, true, false) == VCO_ADJUST_RIGHT);
    }
}
static void combo_members(void)
{
    const uint32_t buttons[] = {VCO_L, VCO_R, VCO_SQUARE, VCO_CROSS, VCO_TRIANGLE};
    const VcoAction actions[] = {VCO_PAGE_PREV, VCO_PAGE_NEXT, VCO_TOGGLE, VCO_CLOSE, VCO_SETTINGS};
    for (unsigned member = 0; member < 5; ++member) {
        Rig r = rig(true, 0);
        r.mapping.bindings.open = buttons[member] | VCO_SELECT;
        assert(step(&r, buttons[member]) == VCO_NONE && r.state.pending_button == buttons[member]);
        assert(step(&r, r.mapping.bindings.open) == VCO_CLOSE); /* No constituent action leaked. */
        assert(!r.state.pending_button && !r.state.open && r.state.draining);
        assert(step(&r, 0) == VCO_RELEASE);
        r = rig(true, 0);
        r.mapping.bindings.open = buttons[member] | VCO_SELECT;
        assert(step(&r, buttons[member]) == VCO_NONE);
        assert(sample(&r, 0, false, false, false) == VCO_NONE && r.state.pending_button);
        r.raw.ry = 0;
        assert(step(&r, 0) == VCO_NONE && r.state.pending_button);
        r.raw.ry = 128;
        assert(step(&r, 0) == actions[member] && !r.state.pending_button);
        assert(step(&r, 0) == (actions[member] == VCO_CLOSE ? VCO_RELEASE : VCO_NONE));
    }
    const uint32_t fixed[] = {VCO_UP, VCO_DOWN, VCO_LEFT, VCO_RIGHT, VCO_CIRCLE, VCO_CROSS};
    const VcoAction settings_actions[] = {VCO_MOVE_UP,      VCO_MOVE_DOWN, VCO_ADJUST_LEFT,
                                          VCO_ADJUST_RIGHT, VCO_TOGGLE,    VCO_BACK};
    for (unsigned member = 0; member < 6; ++member) {
        Rig r = rig(true, 0);
        r.mapping.bindings.open = fixed[member] | VCO_SELECT;
        assert(vco_settings_valid(&r.mapping));
        assert(sample(&r, fixed[member], true, true, false) == VCO_NONE);
        assert(sample(&r, r.mapping.bindings.open, true, true, false) == VCO_BACK);
        assert(r.state.open && r.state.wait_neutral && !r.state.pending_button);
        assert(sample(&r, fixed[member], true, true, false) == VCO_NONE);
        assert(sample(&r, 0, true, true, false) == VCO_NONE && !r.state.wait_neutral);
        assert(sample(&r, fixed[member], true, true, false) == VCO_NONE);
        assert(sample(&r, 0, true, true, false) == settings_actions[member]);
        assert(sample(&r, 0, true, true, false) == VCO_NONE);
    }
    Rig r = rig(true, 0);
    r.mapping.bindings.open = VCO_SQUARE | VCO_CROSS;
    assert(step(&r, VCO_SQUARE) == VCO_NONE);
    assert(step(&r, VCO_SQUARE | VCO_CROSS) == VCO_CLOSE);
    assert(!r.state.pending_button); /* Both toggle and close may be combo constituents. */
}
static void invalid_chords(void)
{
    Rig r = rig(true, 0);
    r.mapping.bindings.open = VCO_SQUARE | VCO_SELECT;
    assert(step(&r, VCO_SQUARE) == VCO_NONE);
    assert(step(&r, VCO_SQUARE | VCO_CROSS) == VCO_NONE && r.state.chord_blocked);
    assert(step(&r, VCO_SQUARE) == VCO_NONE && !r.state.pending_button);
    assert(step(&r, 0) == VCO_NONE &&
           !r.state.chord_blocked); /* No toggle/close from partial chord. */
    assert(step(&r, VCO_SQUARE) == VCO_NONE);
    assert(step(&r, VCO_CROSS) == VCO_NONE && r.state.chord_blocked); /* No observed all-release. */
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_SQUARE) == VCO_NONE);
    assert(step(&r, 0) == VCO_TOGGLE);
    assert(step(&r, VCO_SQUARE | VCO_CROSS) == VCO_NONE);
    assert(step(&r, VCO_CROSS) == VCO_NONE);
    assert(step(&r, 0) == VCO_NONE);
    assert(step(&r, VCO_SQUARE | VCO_CROSS) == VCO_NONE);
    assert(step(&r, VCO_SQUARE | VCO_SELECT) == VCO_CLOSE); /* Complete combo still wins. */
}
static void paused_dispatch(void)
{
    Rig r = rig(true, 0);
    r.mapping.bindings.open = VCO_SQUARE | VCO_SELECT;
    assert(step(&r, VCO_SQUARE) == VCO_NONE && r.state.pending_button);
    assert(sample(&r, VCO_SQUARE, true, true, true) == VCO_NONE);
    assert(!r.state.pending_button && r.state.wait_neutral);
    assert(sample(&r, r.mapping.bindings.open, true, true, true) == VCO_NONE && r.state.open);
    assert(sample(&r, VCO_CROSS, true, true, true) == VCO_NONE && r.state.open);
    assert(sample(&r, VCO_LEFT, true, true, true) == VCO_NONE);
    assert(sample(&r, 0, true, true, true) == VCO_NONE && r.state.wait_neutral);
    vco_settings_set_button_mode(&r.mapping, VCO_BUTTON_WESTERN);
    assert(sample(&r, VCO_CROSS, false, true, false) == VCO_NONE);
    assert(step(&r, 0) == VCO_NONE && !r.state.wait_neutral);
    assert(step(&r, VCO_SQUARE) == VCO_NONE && r.state.pending_button);
    assert(step(&r, 0) == VCO_TOGGLE);
    assert(r.state.previous == 0 && r.state.last_tick == r.raw.tick);
}
static void forced_exits(void)
{
    for (unsigned waiting = 0; waiting < 3; ++waiting)
        for (unsigned reason = 0; reason < 3; ++reason) {
            Rig r = rig(true, 1000000);
            r.state.wait_neutral = waiting == 0;
            r.state.pending_button = waiting == 1 ? VCO_L : 0;
            if (reason == 1)
                r.raw.captured = 0;
            if (reason == 2)
                r.raw.tick += VCO_LEASE_US - 16000;
            if (reason == 0)
                assert(sample(&r, VCO_PS, true, false, waiting == 2) == VCO_NONE);
            assert(sample(&r, reason == 0 ? 0 : VCO_L, true, false, waiting == 2) == VCO_CLOSE);
            assert(!r.state.open && r.state.draining && !r.state.wait_neutral &&
                   !r.state.pending_button);
            r.raw.captured = 0;
            assert(sample(&r, 0, true, false, waiting == 2) == VCO_RELEASE && !r.state.draining);
        }
    Rig r = rig(true, 0);
    r.state.seeded = false;
    assert(sample(&r, VCO_PS, true, true, true) == VCO_NONE);
    assert(sample(&r, 0, true, true, true) == VCO_CLOSE); /* Solo PS release also covers startup. */
    r = rig(true, 0);
    r.state.seeded = false;
    r.raw.captured = 0;
    assert(sample(&r, 0, true, true, true) == VCO_CLOSE);
    r = rig(true, 0);
    r.state.pending_button = VCO_L;
    r.state.chord_blocked = true;
    vco_input_forced_closed(&r.state);
    assert(!r.state.open && !r.state.draining && r.state.wait_neutral && !r.state.seeded);
    assert(!r.state.pending_button && !r.state.chord_blocked);
    assert(step(&r, VCO_COMBO) == VCO_NONE);
    assert(step(&r, 0) == VCO_NONE && !r.state.wait_neutral);
    assert(step(&r, VCO_COMBO) == VCO_OPEN);
}
static void drain_deadlines(void)
{
    for (unsigned wrap = 0; wrap < 2; ++wrap) {
        uint32_t start = wrap ? 0xffff0000u : 1000000u;
        Rig r = rig(true, start);
        r.state.wait_neutral = true;
        r.state.pending_button = VCO_L;
        vco_input_closed(&r.state);
        assert(!r.state.open && !r.state.wait_neutral && !r.state.pending_button);
        r.raw.buttons = VCO_CIRCLE;
        r.raw.lx = 0;
        for (uint32_t elapsed = 0; elapsed < VCO_DRAIN_US; elapsed += 10000) {
            r.raw.tick = start + elapsed;
            assert(vco_input_step_with_capture(&r.state, &r.raw, false, &r.mapping, false, true) ==
                   VCO_NONE);
            assert(!vco_capture_expired(VCO_CAPTURE_DRAIN, elapsed));
            assert(!vco_capture_renewable(VCO_CAPTURE_DRAIN));
        }
        r.raw.tick = start + VCO_DRAIN_US;
        assert(vco_input_step_with_capture(&r.state, &r.raw, false, &r.mapping, false, true) ==
               VCO_RELEASE);
        assert(!r.state.draining && !r.state.wait_neutral);
        assert(vco_capture_expired(VCO_CAPTURE_DRAIN, VCO_DRAIN_US));
        assert(!vco_capture_expired(VCO_CAPTURE_ACTIVE, VCO_DRAIN_US));
        assert(vco_capture_expired(VCO_CAPTURE_ACTIVE, VCO_LEASE_US));
    }
    Rig r = rig(true, 0);
    vco_input_closed(&r.state);
    assert(sample(&r, VCO_PS, false, false, true) == VCO_RELEASE);
    vco_input_closed(&r.state);
    r.raw.captured = 0;
    assert(sample(&r, VCO_CIRCLE, false, false, true) == VCO_RELEASE);
    assert(vco_capture_renewable(VCO_CAPTURE_ACTIVE));
    assert(!vco_capture_renewable(VCO_CAPTURE_OFF));
}
static void screenshots(void)
{
    for (unsigned settings = 0; settings < 2; ++settings)
        for (unsigned paused = 0; paused < 2; ++paused)
            for (unsigned order = 0; order < 3; ++order) {
                Rig r = rig(true, 0xffff0000u);
                if (order)
                    assert(sample(&r, order == 1 ? VCO_PS : VCO_START, settings, true, paused) ==
                           VCO_NONE);
                assert(sample(&r, VCO_PS | VCO_START, settings, true, paused) == VCO_NONE &&
                       r.state.open);
                for (unsigned i = 0; i < 40; ++i)
                    assert(sample(&r, VCO_PS | VCO_START, settings, true, paused) == VCO_NONE &&
                           r.state.open);
                assert(sample(&r, VCO_START, settings, true, paused) == VCO_NONE);
                assert(sample(&r, 0, settings, true, paused) == VCO_NONE);
                assert(!r.state.system_pending && r.state.open);
                assert(sample(&r, 0, settings, true, false) == VCO_NONE);
                assert(sample(&r, VCO_TRIANGLE, false, true, false) == VCO_SETTINGS);
            }
    Rig r = rig(true, 0);
    assert(step(&r, VCO_PS) == VCO_NONE);
    for (unsigned i = 0; i < 15; ++i)
        assert(step(&r, VCO_PS) == VCO_NONE);
    assert(step(&r, VCO_PS) == VCO_CLOSE);
    r = rig(true, 0);
    assert(step(&r, VCO_PS | VCO_START) == VCO_NONE);
    r.raw.captured = 0;
    assert(step(&r, VCO_PS | VCO_START) == VCO_CLOSE); /* Screenshot cannot bypass lease loss. */
}
int main(void)
{
    screenshots();
    default_actions();
    opening_gate();
    regional_actions();
    combo_members();
    invalid_chords();
    paused_dispatch();
    forced_exits();
    drain_deadlines();
    puts("PASS: dynamic actions, combo arbitration, neutral gate, paused dispatch, forced exits, "
         "drain bounds");
    return 0;
}
