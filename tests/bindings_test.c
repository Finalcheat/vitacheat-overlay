#include <assert.h>
#include <string.h>
#include "settings.h"
static void validation(void)
{
    VcoBindings b = vco_bindings_defaults();
    assert(vco_bindings_valid(&b));
    assert(vco_binding_check(&b, VCO_BIND_SETTINGS, VCO_START) == VCO_BINDING_RESERVED);
    assert(vco_binding_check(&b, VCO_BIND_OPEN, VCO_SELECT | VCO_SQUARE) == VCO_BINDING_OK);
    assert(vco_binding_check(&b, VCO_BIND_OPEN, VCO_UP | VCO_L) == VCO_BINDING_OK);
    assert(vco_binding_check(&b, VCO_BIND_OPEN, VCO_SELECT) == VCO_BINDING_COMBO_REQUIRED);
    assert(vco_binding_check(&b, VCO_BIND_TOGGLE, VCO_SELECT | VCO_CROSS) ==
           VCO_BINDING_SINGLE_REQUIRED);
    assert(vco_binding_check(&b, VCO_BIND_CLOSE, VCO_PS) == VCO_BINDING_RESERVED);
    assert(vco_binding_check(&b, VCO_BIND_CLOSE, VCO_UP) == VCO_BINDING_RESERVED);
    assert(vco_binding_check(&b, VCO_BIND_CLOSE, VCO_DOWN) == VCO_BINDING_RESERVED);
    assert(vco_binding_check(&b, VCO_BIND_OPEN, VCO_L | VCO_LEFT | VCO_RIGHT) ==
           VCO_BINDING_OPPOSITE_DIRECTIONS);
    assert(vco_binding_check(&b, VCO_BIND_OPEN, VCO_UP | VCO_DOWN) ==
           VCO_BINDING_OPPOSITE_DIRECTIONS);
    assert(vco_binding_check(&b, VCO_BIND_CLOSE, VCO_SQUARE) == VCO_BINDING_CONFLICT);
    assert(vco_binding_check(&b, VCO_BIND_CLOSE, VCO_SELECT) == VCO_BINDING_OK);
    assert(vco_binding_check(&b, VCO_BIND_OPEN, 0x20000000u) == VCO_BINDING_RESERVED);
    assert(!vco_binding_set(&b, VCO_BIND_COUNT, 1) && !vco_bindings_valid(NULL));
    static const uint32_t keys[] = {VCO_L,     VCO_R,      VCO_SELECT, VCO_START,  VCO_LEFT,
                                    VCO_RIGHT, VCO_SQUARE, VCO_CROSS,  VCO_CIRCLE, VCO_TRIANGLE};
    for (unsigned offset = 0; offset < 10; ++offset) {
        for (unsigned i = 0; i < VCO_BIND_OPEN; ++i)
            assert(vco_binding_set(&b, i, keys[(offset + i) % 10]));
        b.open = VCO_SELECT | VCO_START;
        assert(vco_bindings_migrate_settings(&b));
        assert(vco_bindings_valid(&b));
        VcoBindings duplicate = b;
        duplicate.close = b.settings;
        assert(!vco_bindings_valid(&duplicate) && !vco_bindings_equal(&b, &duplicate));
    }
}
static void capture(void)
{
    VcoSettingsState s = {0};
    s.applied = vco_settings_defaults();
    vco_settings_begin(&s);
    VcoSettings original = s.applied;
    for (unsigned action = 0; action < VCO_BIND_OPEN; ++action)
        assert(!vco_settings_capture_begin(&s, action));
    assert(vco_settings_capture_begin(&s, VCO_BIND_OPEN));
    /* Consume the confirmation key; only a fresh neutral state arms capture. */
    assert(vco_settings_capture_step(&s, VCO_CIRCLE, false) == VCO_BIND_CAPTURE_NONE);
    assert(vco_settings_capture_step(&s, 0, false) == VCO_BIND_CAPTURE_NONE && !s.capture.ready);
    assert(vco_settings_capture_step(&s, 0, true) == VCO_BIND_CAPTURE_NONE && s.capture.ready);
    (void)vco_settings_capture_step(&s, VCO_CROSS, false);
    assert(vco_settings_capture_step(&s, 0, true) == VCO_BIND_CAPTURE_REJECTED &&
           s.capture_error == VCO_BINDING_COMBO_REQUIRED);
    assert(vco_settings_equal(&s.applied, &original));
    vco_settings_capture_cancel(&s);
    assert(vco_settings_capture_begin(&s, VCO_BIND_OPEN));
    (void)vco_settings_capture_step(&s, 0, true);
    (void)vco_settings_capture_step(&s, VCO_R, false);
    assert(vco_settings_capture_step(&s, 0, true) == VCO_BIND_CAPTURE_REJECTED &&
           s.capture_error == VCO_BINDING_COMBO_REQUIRED);
    (void)vco_settings_capture_step(&s, 0, true);
    (void)vco_settings_capture_step(&s, VCO_R, false);
    (void)vco_settings_capture_step(&s, VCO_R | VCO_SELECT, false);
    (void)vco_settings_capture_step(&s, VCO_SELECT, false);
    assert(vco_settings_capture_step(&s, 0, true) == VCO_BIND_CAPTURE_CHANGED &&
           s.draft.bindings.open == (VCO_R | VCO_SELECT));
    /* Rolling chords must not capture three keys that never overlapped. */
    assert(vco_settings_capture_begin(&s, VCO_BIND_OPEN));
    (void)vco_settings_capture_step(&s, 0, true);
    (void)vco_settings_capture_step(&s, VCO_L | VCO_SELECT, false);
    (void)vco_settings_capture_step(&s, VCO_L | VCO_START, false);
    assert(vco_settings_capture_step(&s, 0, true) == VCO_BIND_CAPTURE_REJECTED &&
           s.capture_error == VCO_BINDING_NOT_SIMULTANEOUS);
    (void)vco_settings_capture_step(&s, 0, true);
    (void)vco_settings_capture_step(&s, VCO_DOWN, false);
    assert(vco_settings_capture_step(&s, 0, true) == VCO_BIND_CAPTURE_CANCELLED &&
           !s.capture.active);
    assert(vco_settings_capture_begin(&s, VCO_BIND_OPEN));
    (void)vco_settings_capture_step(&s, 0, true);
    (void)vco_settings_capture_step(&s, VCO_DOWN | VCO_L, false);
    assert(vco_settings_capture_step(&s, 0, true) == VCO_BIND_CAPTURE_CHANGED &&
           s.draft.bindings.open == (VCO_DOWN | VCO_L));
    assert(vco_settings_capture_begin(&s, VCO_BIND_OPEN));
    assert(vco_settings_capture_step(&s, VCO_PS, false) == VCO_BIND_CAPTURE_CANCELLED);
    vco_settings_cancel(&s);
    assert(vco_settings_equal(&s.applied, &original) && vco_settings_equal(&s.draft, &original));
}
static void names(void)
{
    char out[96];
    assert(vco_binding_name(VCO_COMBO, out, sizeof(out)) == 8 && !strcmp(out, "L+SELECT"));
    assert(vco_binding_name(VCO_CIRCLE, out, sizeof(out)) == strlen("○") && !strcmp(out, "○"));
    assert(vco_binding_name(VCO_L | VCO_CIRCLE, out, 3) == strlen("L+○") && !strcmp(out, "L"));
    assert(vco_binding_name(VCO_CIRCLE, out, 3) == strlen("○") && !out[0]);
    assert(vco_binding_name(VCO_R | VCO_START, NULL, 0) == strlen("R+START"));
    memset(out, 0x7f, sizeof(out));
    assert(vco_binding_name(VCO_L, out, 0) == 1 && (unsigned char)out[0] == 0x7f);
    assert(vco_binding_name(VCO_PS, out, sizeof(out)) == 1 && !strcmp(out, "?"));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "validation"))
        validation();
    else if (!strcmp(argv[1], "capture"))
        capture();
    else if (!strcmp(argv[1], "names"))
        names();
    else
        assert(0);
    return 0;
}
