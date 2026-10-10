#ifndef VCO_SETTINGS_H
#define VCO_SETTINGS_H
#include <stdbool.h>
#include "localization.h"
#include "file_encoding.h"
#include "input/binding_capture.h"
typedef enum { VCO_BUTTON_ASIAN, VCO_BUTTON_WESTERN } VcoButtonMode;
typedef struct {
    int language;
    unsigned opacity;
    VcoEncoding encoding;
    VcoBindings bindings;
    VcoButtonMode button_mode;
} VcoSettings;
typedef struct {
    VcoSettings applied, draft;
    unsigned selected;
    bool open, confirm, apply_pending;
    VcoBindingCapture capture;
    VcoBindingError capture_error;
} VcoSettingsState;
static inline VcoSettings vco_settings_defaults(void)
{
    VcoSettings value = {-1, 70, VCO_ENCODING_AUTO, vco_bindings_defaults(), VCO_BUTTON_ASIAN};
    value.bindings.toggle = VCO_SQUARE;
    value.bindings.close = VCO_CROSS;
    return value;
}
static inline uint32_t vco_settings_confirm_button(const VcoSettings *s)
{
    return s->button_mode == VCO_BUTTON_WESTERN ? VCO_CROSS : VCO_CIRCLE;
}
static inline uint32_t vco_settings_back_button(const VcoSettings *s)
{
    return s->button_mode == VCO_BUTTON_WESTERN ? VCO_CIRCLE : VCO_CROSS;
}
/* Fixed actions and the regional pair are derived together. Only the opening
 * chord remains freely configurable; legacy fields are kept for migration. */
static inline void vco_settings_set_button_mode(VcoSettings *s, VcoButtonMode mode)
{
    s->button_mode = mode;
    s->bindings.previous = VCO_L;
    s->bindings.next = VCO_R;
    s->bindings.settings = VCO_TRIANGLE;
    s->bindings.toggle = VCO_SQUARE;
    s->bindings.close = vco_settings_back_button(s);
}
enum {
    VCO_SETTING_LANGUAGE,
    VCO_SETTING_OPACITY,
    VCO_SETTING_BUTTON_MODE,
    VCO_SETTING_OPEN,
    VCO_SETTING_ENCODING,
    VCO_SETTING_APPLY,
    VCO_SETTING_RESET,
    VCO_SETTING_COUNT
};
static inline int vco_settings_binding_row(unsigned row)
{
    return row == VCO_SETTING_OPEN ? VCO_BIND_OPEN : -1;
}
static inline bool vco_settings_valid(const VcoSettings *s)
{
    return s && s->language >= -1 && s->language < VCO_LANGUAGE_COUNT && s->opacity <= 100 &&
           s->opacity % 5 == 0 && (unsigned)s->encoding <= VCO_ENCODING_GBK &&
           (unsigned)s->button_mode <= VCO_BUTTON_WESTERN && s->bindings.previous == VCO_L &&
           s->bindings.next == VCO_R && s->bindings.settings == VCO_TRIANGLE &&
           s->bindings.toggle == VCO_SQUARE && s->bindings.close == vco_settings_back_button(s) &&
           vco_bindings_valid(&s->bindings);
}
static inline bool vco_settings_equal(const VcoSettings *a, const VcoSettings *b)
{
    return a->language == b->language && a->opacity == b->opacity && a->encoding == b->encoding &&
           a->button_mode == b->button_mode && vco_bindings_equal(&a->bindings, &b->bindings);
}
static inline int vco_settings_language(const VcoSettings *value, int system)
{
    int language = value->language < 0 ? system : value->language;
    return vco_language_normalize(language);
}
static inline void vco_settings_begin(VcoSettingsState *s)
{
    s->draft = s->applied;
    s->selected = 0;
    s->confirm = false;
    s->apply_pending = false;
    vco_binding_capture_begin(&s->capture, VCO_BIND_COUNT);
    s->capture_error = VCO_BINDING_OK;
    s->open = true;
}
static inline void vco_settings_capture_cancel(VcoSettingsState *s)
{
    vco_binding_capture_cancel(&s->capture);
    s->capture_error = VCO_BINDING_OK;
}
static inline void vco_settings_cancel(VcoSettingsState *s)
{
    s->draft = s->applied;
    s->open = false;
    s->confirm = false;
    s->apply_pending = false;
    vco_settings_capture_cancel(s);
}
static inline bool vco_settings_capture_begin(VcoSettingsState *s, unsigned action)
{
    if (!s->open || s->confirm || s->apply_pending || action != VCO_BIND_OPEN)
        return false;
    vco_binding_capture_begin(&s->capture, action);
    s->capture_error = VCO_BINDING_OK;
    return true;
}
static inline VcoBindingCaptureResult vco_settings_capture_step(VcoSettingsState *s,
                                                                uint32_t buttons, bool neutral)
{
    return vco_binding_capture_step(&s->capture, &s->draft.bindings, buttons, neutral,
                                    &s->capture_error);
}
static inline void vco_settings_back(VcoSettingsState *s)
{
    if (s->capture.active) {
        vco_settings_capture_cancel(s);
        return;
    }
    if (s->apply_pending)
        return;
    if (s->confirm) {
        s->confirm = false;
        s->selected = 0;
        return;
    }
    if (vco_settings_equal(&s->draft, &s->applied))
        vco_settings_cancel(s);
    else {
        s->confirm = true;
        s->selected = 0;
    }
}
static inline void vco_settings_move(VcoSettingsState *s, int direction)
{
    if (s->capture.active || s->apply_pending)
        return;
    unsigned last = s->confirm ? 2 : VCO_SETTING_COUNT - 1;
    if (direction < 0 && s->selected)
        --s->selected;
    if (direction > 0 && s->selected < last)
        ++s->selected;
}
static inline void vco_settings_adjust(VcoSettingsState *s, int direction)
{
    if (s->confirm || s->capture.active || s->apply_pending || (direction != -1 && direction != 1))
        return;
    int binding = vco_settings_binding_row(s->selected);
    if (binding >= 0) {
        (void)vco_settings_capture_begin(s, (unsigned)binding);
    } else if (s->selected == VCO_SETTING_LANGUAGE) {
        int language = s->draft.language + direction;
        if (language >= -1 && language < VCO_LANGUAGE_COUNT)
            s->draft.language = language;
    } else if (s->selected == VCO_SETTING_OPACITY) {
        int opacity = (int)s->draft.opacity + direction * 5;
        if (opacity >= 0 && opacity <= 100)
            s->draft.opacity = (unsigned)opacity;
    } else if (s->selected == VCO_SETTING_ENCODING) {
        s->draft.encoding = (VcoEncoding)(((int)s->draft.encoding + direction + 3) % 3);
    } else if (s->selected == VCO_SETTING_BUTTON_MODE) {
        vco_settings_set_button_mode(&s->draft, s->draft.button_mode == VCO_BUTTON_ASIAN
                                                    ? VCO_BUTTON_WESTERN
                                                    : VCO_BUTTON_ASIAN);
    }
}
static inline void vco_settings_accept(VcoSettingsState *s)
{
    if (s->capture.active || s->apply_pending)
        return;
    int binding = vco_settings_binding_row(s->selected);
    if ((s->confirm && s->selected == 0) || (!s->confirm && s->selected == VCO_SETTING_APPLY)) {
        if (vco_settings_valid(&s->draft))
            s->apply_pending = true;
    } else if (s->confirm && s->selected == 1)
        vco_settings_cancel(s);
    else if (s->confirm) {
        s->confirm = false;
        s->selected = 0;
    } else if (s->selected == VCO_SETTING_RESET) {
        s->draft = vco_settings_defaults();
        s->capture_error = VCO_BINDING_OK;
    } else if (binding >= 0)
        (void)vco_settings_capture_begin(s, (unsigned)binding);
    else if (s->selected == VCO_SETTING_BUTTON_MODE)
        vco_settings_adjust(s, 1);
}
/* UI owner commits the complete snapshot after raw pad neutrality.
 * No save request or new mapping is published merely by pressing Apply. */
static inline bool vco_settings_commit(VcoSettingsState *s, bool neutral)
{
    if (!s->apply_pending || !neutral || !vco_settings_valid(&s->draft))
        return false;
    s->applied = s->draft;
    vco_settings_cancel(s);
    return true;
}
#endif
