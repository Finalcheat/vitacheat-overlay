#include <assert.h>
#include "settings.h"
static void apply(VcoSettingsState *s)
{
    VcoSettings old = s->applied;
    vco_settings_accept(s);
    assert(s->apply_pending && s->open && vco_settings_equal(&s->applied, &old));
    assert(!vco_settings_commit(s, false) && vco_settings_equal(&s->applied, &old));
    assert(vco_settings_commit(s, true) && !s->open && !s->apply_pending);
    assert(!vco_settings_commit(s, true));
}
int main(void)
{
    VcoSettingsState s = {0};
    s.applied = vco_settings_defaults();
    assert(s.applied.opacity == 70 && vco_settings_language(&s.applied, 2) == 2);
    vco_settings_begin(&s);
    vco_settings_adjust(&s, 1);
    assert(s.draft.language == 0 && s.applied.language == -1);
    vco_settings_move(&s, 1);
    for (unsigned i = 0; i < 30; ++i)
        vco_settings_adjust(&s, -1);
    assert(s.draft.opacity == 0);
    for (unsigned i = 0; i < 30; ++i)
        vco_settings_adjust(&s, 1);
    assert(s.draft.opacity == 100);
    vco_settings_back(&s);
    assert(s.confirm && s.open);
    vco_settings_move(&s, 1);
    vco_settings_accept(&s);
    assert(!s.open && s.applied.opacity == 70 && s.applied.language == -1);
    vco_settings_begin(&s);
    vco_settings_adjust(&s, 1);
    vco_settings_back(&s);
    apply(&s);
    assert(s.applied.language == 0);
    vco_settings_begin(&s);
    s.selected = VCO_SETTING_RESET;
    vco_settings_accept(&s);
    assert(s.draft.language == -1 && s.applied.language == 0);
    vco_settings_back(&s);
    s.selected = 2;
    vco_settings_accept(&s);
    assert(s.open && !s.confirm);
    vco_settings_cancel(&s);
    assert(s.draft.language == 0 && !s.open);
    /* Only the opening chord is captured; fixed actions cannot be edited. */
    for (unsigned i = 0; i < VCO_BIND_OPEN; ++i)
        assert(!vco_settings_capture_begin(&s, i));
    {
        unsigned i = VCO_BIND_OPEN;
        vco_settings_begin(&s);
        s.selected = VCO_SETTING_OPEN;
        assert(vco_settings_binding_row(s.selected) == (int)i);
        vco_settings_adjust(&s, 1);
        assert(s.capture.active && s.capture.action == i);
        unsigned selected = s.selected;
        vco_settings_move(&s, 1);
        vco_settings_accept(&s);
        assert(s.capture.active && s.selected == selected && !s.apply_pending);
        vco_settings_back(&s);
        assert(s.open && !s.capture.active && !s.confirm);
        vco_settings_accept(&s);
        assert(s.capture.active && s.capture.action == i);
        vco_settings_cancel(&s);
        assert(!s.capture.active && !s.open);
    }
    assert(vco_settings_binding_row(VCO_SETTING_LANGUAGE) == -1 &&
           vco_settings_binding_row(VCO_SETTING_ENCODING) == -1);
    vco_settings_begin(&s);
    s.selected = VCO_SETTING_BUTTON_MODE;
    assert(s.applied.button_mode == VCO_BUTTON_ASIAN && s.draft.bindings.close == VCO_CROSS);
    vco_settings_adjust(&s, 1);
    assert(s.draft.button_mode == VCO_BUTTON_WESTERN && s.draft.bindings.close == VCO_CIRCLE &&
           s.applied.button_mode == VCO_BUTTON_ASIAN && !s.capture.active);
    vco_settings_adjust(&s, 1);
    assert(s.draft.button_mode == VCO_BUTTON_ASIAN);
    vco_settings_adjust(&s, -1);
    assert(s.draft.button_mode == VCO_BUTTON_WESTERN);
    vco_settings_accept(&s);
    assert(s.draft.button_mode == VCO_BUTTON_ASIAN && !s.apply_pending);
    vco_settings_cancel(&s);
    vco_settings_begin(&s);
    s.selected = VCO_SETTING_ENCODING;
    vco_settings_adjust(&s, -1);
    assert(s.draft.encoding == VCO_ENCODING_GBK && s.applied.encoding == VCO_ENCODING_AUTO &&
           !vco_settings_equal(&s.draft, &s.applied));
    vco_settings_back(&s);
    apply(&s);
    assert(s.applied.encoding == VCO_ENCODING_GBK);
    vco_settings_begin(&s);
    s.selected = VCO_SETTING_ENCODING;
    vco_settings_adjust(&s, 1);
    assert(s.draft.encoding == VCO_ENCODING_AUTO);
    vco_settings_adjust(&s, 1);
    assert(s.draft.encoding == VCO_ENCODING_UTF8);
    vco_settings_adjust(&s, 1);
    assert(s.draft.encoding == VCO_ENCODING_GBK);
    s.selected = VCO_SETTING_RESET;
    vco_settings_accept(&s);
    assert(s.draft.encoding == VCO_ENCODING_AUTO && s.applied.encoding == VCO_ENCODING_GBK);
    vco_settings_cancel(&s);
    /* Waiting never changes the committed settings or permits new edits. */
    vco_settings_begin(&s);
    s.draft.bindings.open = VCO_R | VCO_SELECT;
    s.selected = VCO_SETTING_APPLY;
    vco_settings_accept(&s);
    VcoSettings wanted = s.draft, old = s.applied;
    vco_settings_adjust(&s, -1);
    vco_settings_move(&s, -1);
    vco_settings_back(&s);
    assert(s.apply_pending && vco_settings_equal(&s.draft, &wanted) &&
           vco_settings_equal(&s.applied, &old));
    assert(!vco_settings_capture_begin(&s, VCO_BIND_TOGGLE));
    vco_settings_cancel(&s); /* Lifecycle/PS discards an uncommitted request. */
    assert(!s.apply_pending && vco_settings_equal(&s.applied, &old));
    /* Preview edits, cancel and Apply must work for every persisted language. */
    for (int language = 0; language < 5; ++language) {
        s.applied = vco_settings_defaults();
        vco_settings_begin(&s);
        s.selected = VCO_SETTING_LANGUAGE;
        for (int i = -1; i < language; ++i)
            vco_settings_adjust(&s, 1);
        assert(s.draft.language == language && s.applied.language == -1);
        assert(vco_settings_language(&s.draft, 0) == language);
        vco_settings_back(&s);
        apply(&s);
        assert(s.applied.language == language);
        vco_settings_begin(&s);
        vco_settings_adjust(&s, -1);
        vco_settings_cancel(&s);
        assert(s.applied.language == language);
    }
    s.applied.encoding = (VcoEncoding)3;
    assert(!vco_settings_valid(&s.applied));
    return 0;
}
