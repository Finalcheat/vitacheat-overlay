#ifndef VCO_BINDINGS_H
#define VCO_BINDINGS_H
#include "buttons.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct {
    uint32_t previous, next, toggle, close, settings, open;
} VcoBindings;
enum {
    VCO_BIND_PREVIOUS,
    VCO_BIND_NEXT,
    VCO_BIND_TOGGLE,
    VCO_BIND_CLOSE,
    VCO_BIND_SETTINGS,
    VCO_BIND_OPEN,
    VCO_BIND_COUNT
};
typedef enum {
    VCO_BINDING_OK,
    VCO_BINDING_RESERVED,
    VCO_BINDING_SINGLE_REQUIRED,
    VCO_BINDING_COMBO_REQUIRED,
    VCO_BINDING_CONFLICT,
    VCO_BINDING_OPPOSITE_DIRECTIONS,
    VCO_BINDING_NOT_SIMULTANEOUS
} VcoBindingError;
static inline VcoBindings vco_bindings_defaults(void)
{
    VcoBindings value = {VCO_L, VCO_R, VCO_SQUARE, VCO_CIRCLE, VCO_TRIANGLE, VCO_COMBO};
    return value;
}
static inline uint32_t vco_binding_value(const VcoBindings *b, unsigned action)
{
    if (!b)
        return 0;
    switch (action) {
    case VCO_BIND_PREVIOUS:
        return b->previous;
    case VCO_BIND_NEXT:
        return b->next;
    case VCO_BIND_TOGGLE:
        return b->toggle;
    case VCO_BIND_CLOSE:
        return b->close;
    case VCO_BIND_SETTINGS:
        return b->settings;
    case VCO_BIND_OPEN:
        return b->open;
    default:
        return 0;
    }
}
static inline bool vco_binding_set(VcoBindings *b, unsigned action, uint32_t mask)
{
    if (!b)
        return false;
    switch (action) {
    case VCO_BIND_PREVIOUS:
        b->previous = mask;
        break;
    case VCO_BIND_NEXT:
        b->next = mask;
        break;
    case VCO_BIND_TOGGLE:
        b->toggle = mask;
        break;
    case VCO_BIND_CLOSE:
        b->close = mask;
        break;
    case VCO_BIND_SETTINGS:
        b->settings = mask;
        break;
    case VCO_BIND_OPEN:
        b->open = mask;
        break;
    default:
        return false;
    }
    return true;
}
static inline VcoBindingError vco_binding_check(const VcoBindings *b, unsigned action,
                                                uint32_t mask)
{
    if (!b || action >= VCO_BIND_COUNT || (mask & ~VCO_BUTTON_MASK))
        return VCO_BINDING_RESERVED;
    unsigned count = vco_button_count(mask);
    if (action == VCO_BIND_OPEN) {
        if (count < 2)
            return VCO_BINDING_COMBO_REQUIRED;
        if ((mask & (VCO_UP | VCO_DOWN)) == (VCO_UP | VCO_DOWN) ||
            (mask & (VCO_LEFT | VCO_RIGHT)) == (VCO_LEFT | VCO_RIGHT))
            return VCO_BINDING_OPPOSITE_DIRECTIONS;
    } else {
        if (count != 1)
            return VCO_BINDING_SINGLE_REQUIRED;
        if ((mask & (VCO_UP | VCO_DOWN)) || (action == VCO_BIND_SETTINGS && mask == VCO_START))
            return VCO_BINDING_RESERVED;
        for (unsigned i = 0; i < VCO_BIND_OPEN; ++i)
            if (i != action && vco_binding_value(b, i) == mask)
                return VCO_BINDING_CONFLICT;
    }
    return VCO_BINDING_OK;
}
/* Upgrade the old START settings action without changing other choices.
 * Triangle may already belong to Toggle/Close, so choose an unused key. */
static inline bool vco_bindings_migrate_settings(VcoBindings *b)
{
    if (b->settings != VCO_START)
        return true;
    static const uint32_t candidates[] = {VCO_TRIANGLE, VCO_SELECT, VCO_CROSS,
                                          VCO_LEFT,     VCO_RIGHT,  VCO_L,
                                          VCO_R,        VCO_SQUARE, VCO_CIRCLE};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (vco_binding_check(b, VCO_BIND_SETTINGS, candidates[i]) == VCO_BINDING_OK) {
            b->settings = candidates[i];
            return true;
        }
    }
    return false;
}
static inline bool vco_bindings_valid(const VcoBindings *b)
{
    for (unsigned i = 0; i < VCO_BIND_COUNT; ++i)
        if (vco_binding_check(b, i, vco_binding_value(b, i)) != VCO_BINDING_OK)
            return false;
    return true;
}
static inline bool vco_bindings_equal(const VcoBindings *a, const VcoBindings *b)
{
    for (unsigned i = 0; i < VCO_BIND_COUNT; ++i)
        if (vco_binding_value(a, i) != vco_binding_value(b, i))
            return false;
    return true;
}
/* Returns full UTF-8 byte length. Truncation is at whole button names, never
 * inside a glyph. Callers can summarize a long chord in a narrow row. */
static inline size_t vco_binding_name(uint32_t mask, char *out, size_t capacity)
{
    static const uint32_t bits[] = {VCO_L,      VCO_R,     VCO_SELECT, VCO_START,
                                    VCO_UP,     VCO_DOWN,  VCO_LEFT,   VCO_RIGHT,
                                    VCO_SQUARE, VCO_CROSS, VCO_CIRCLE, VCO_TRIANGLE};
    static const char *const names[] = {"L", "R", "SELECT", "START", "↑", "↓",
                                        "←", "→", "□",      "×",     "○", "△"};
    size_t length = 0, written = 0;
    bool truncated = false;
    if (out && capacity)
        out[0] = 0;
    if (!mask || (mask & ~VCO_BUTTON_MASK)) {
        if (out && capacity > 1) {
            out[0] = '?';
            out[1] = 0;
        }
        return 1;
    }
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        if (!(mask & bits[i]))
            continue;
        size_t unit = strlen(names[i]), separator = length ? 1 : 0;
        length += separator + unit;
        if (!out || !capacity || truncated || separator + unit >= capacity - written) {
            truncated = true;
            continue;
        }
        if (separator)
            out[written++] = '+';
        memcpy(out + written, names[i], unit);
        written += unit;
        out[written] = 0;
    }
    return length;
}
#endif
