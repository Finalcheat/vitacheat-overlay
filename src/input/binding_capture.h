#ifndef VCO_BINDING_CAPTURE_H
#define VCO_BINDING_CAPTURE_H
#include "bindings.h"
typedef struct {
    unsigned action;
    uint32_t seen;
    bool active, ready, simultaneous;
} VcoBindingCapture;
typedef enum {
    VCO_BIND_CAPTURE_NONE,
    VCO_BIND_CAPTURE_CHANGED,
    VCO_BIND_CAPTURE_CANCELLED,
    VCO_BIND_CAPTURE_REJECTED
} VcoBindingCaptureResult;
static inline void vco_binding_capture_begin(VcoBindingCapture *c, unsigned action)
{
    VcoBindingCapture value = {action, 0, action < VCO_BIND_COUNT, false, false};
    *c = value;
}
static inline void vco_binding_capture_cancel(VcoBindingCapture *c)
{
    c->active = false;
    c->ready = false;
    c->seen = 0;
    c->simultaneous = false;
}
/* neutral includes buttons, both sticks and both touch panels. Each captured
 * chord must have been simultaneously present in at least one raw sample;
 * rolling between keys while holding one is not a larger chord. */
static inline VcoBindingCaptureResult vco_binding_capture_step(VcoBindingCapture *c,
                                                               VcoBindings *draft, uint32_t buttons,
                                                               bool neutral, VcoBindingError *error)
{
    if (!c->active)
        return VCO_BIND_CAPTURE_NONE;
    if (buttons & VCO_PS) {
        vco_binding_capture_cancel(c);
        return VCO_BIND_CAPTURE_CANCELLED;
    }
    if (!c->ready) {
        if (neutral)
            c->ready = true;
        return VCO_BIND_CAPTURE_NONE;
    }
    if (buttons) {
        uint32_t seen = c->seen | buttons;
        if (seen != c->seen)
            c->simultaneous = false;
        c->seen = seen;
        if (buttons == seen)
            c->simultaneous = true;
        return VCO_BIND_CAPTURE_NONE;
    }
    if (!neutral || !c->seen)
        return VCO_BIND_CAPTURE_NONE;
    if (c->seen == VCO_DOWN) {
        vco_binding_capture_cancel(c);
        return VCO_BIND_CAPTURE_CANCELLED;
    }
    VcoBindingError result = vco_binding_check(draft, c->action, c->seen);
    if (result == VCO_BINDING_OK && !c->simultaneous)
        result = VCO_BINDING_NOT_SIMULTANEOUS;
    if (error)
        *error = result;
    if (result != VCO_BINDING_OK) {
        c->seen = 0;
        c->ready = false;
        c->simultaneous = false;
        return VCO_BIND_CAPTURE_REJECTED;
    }
    (void)vco_binding_set(draft, c->action, c->seen);
    vco_binding_capture_cancel(c);
    return VCO_BIND_CAPTURE_CHANGED;
}
#endif
