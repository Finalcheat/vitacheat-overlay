#ifndef VCO_INPUT_STATE_H
#define VCO_INPUT_STATE_H
#include <stdbool.h>
#include <stdint.h>
#include "bridge.h"
#include "settings.h"
#include "input/buttons.h"
typedef enum {
    VCO_NONE,
    VCO_OPEN,
    VCO_CLOSE,
    VCO_MOVE_UP,
    VCO_MOVE_DOWN,
    VCO_PAGE_PREV,
    VCO_PAGE_NEXT,
    VCO_TOGGLE,
    VCO_RELEASE,
    VCO_SETTINGS,
    VCO_BACK,
    VCO_ADJUST_LEFT,
    VCO_ADJUST_RIGHT
} VcoAction;
typedef struct {
    uint32_t previous, last_tick, drain_started, pending_button, system_began;
    bool seeded, open, draining, wait_neutral, chord_blocked, system_pending, screenshot;
} VcoInputState;
static inline bool vco_neutral(const VcoInput *in)
{
    return !in->buttons && in->lx >= 112 && in->lx <= 144 && in->ly >= 112 && in->ly <= 144 &&
           in->rx >= 112 && in->rx <= 144 && in->ry >= 112 && in->ry <= 144;
}
static inline void vco_input_closed(VcoInputState *state)
{
    state->open = false;
    state->draining = true;
    state->wait_neutral = false;
    state->system_pending = state->screenshot = false;
    state->pending_button = 0;
    state->chord_blocked = false;
    state->drain_started = state->last_tick;
}
/* Lifecycle departure has no game to drain into: the caller queues OFF and
 * waits for fresh neutral input before permitting another explicit open. */
static inline void vco_input_forced_closed(VcoInputState *state)
{
    state->open = false;
    state->draining = false;
    state->wait_neutral = true;
    state->seeded = false;
    state->system_pending = state->screenshot = false;
    state->pending_button = 0;
    state->chord_blocked = false;
}
/* Square toggles the main list. Settings and unsaved-change prompts use the
 * applied regional Confirm/Back pair; the list shares only its Back button.
 * Draft changes take effect after Apply and full input neutrality. */
static inline VcoAction vco_input_button_action(uint32_t button, bool settings,
                                                const VcoSettings *mapping)
{
    if (button == VCO_UP)
        return VCO_MOVE_UP;
    if (button == VCO_DOWN)
        return VCO_MOVE_DOWN;
    if (settings) {
        if (button == vco_settings_back_button(mapping))
            return VCO_BACK;
        if (button == VCO_LEFT)
            return VCO_ADJUST_LEFT;
        if (button == VCO_RIGHT)
            return VCO_ADJUST_RIGHT;
        if (button == vco_settings_confirm_button(mapping))
            return VCO_TOGGLE;
    } else {
        if (button == mapping->bindings.close)
            return VCO_CLOSE;
        if (button == mapping->bindings.previous)
            return VCO_PAGE_PREV;
        if (button == mapping->bindings.next)
            return VCO_PAGE_NEXT;
        if (button == mapping->bindings.toggle)
            return VCO_TOGGLE;
    }
    return button == mapping->bindings.settings ? VCO_SETTINGS : VCO_NONE;
}
/* Paused dispatch is used during binding capture and a pending Apply. It still
 * observes raw input and every forced exit, so it never bypasses the lease or
 * permits an invisible capture to outlive its usual release policy. */
static inline VcoAction vco_input_step_with_capture(VcoInputState *s, const VcoInput *in,
                                                    bool settings, const VcoSettings *mapping,
                                                    bool touch_neutral, bool paused)
{
    uint32_t edge = in->buttons & ~s->previous;
    bool gap = s->seeded && (uint32_t)(in->tick - s->last_tick) >= VCO_LEASE_US;
    bool seeded = s->seeded;
    s->previous = in->buttons;
    s->last_tick = in->tick;
    s->seeded = true;
    if (s->open && (gap || !in->captured)) {
        vco_input_closed(s);
        return VCO_CLOSE;
    }
    if (s->draining) {
        if ((vco_neutral(in) && touch_neutral) || !in->captured || (in->buttons & VCO_PS) ||
            (uint32_t)(in->tick - s->drain_started) >= VCO_DRAIN_US) {
            s->draining = false;
            return VCO_RELEASE;
        }
        return VCO_NONE;
    }
    /* Allow the system screenshot chord to reach Shell without navigating or
     * closing this view. A short PS grace also accepts PS-before-START; a solo
     * PS still closes on release or after 250 ms. Lease loss always wins. */
    if (s->open && (in->buttons & VCO_PS)) {
        s->pending_button = 0;
        s->chord_blocked = false;
        if (!s->system_pending) {
            s->system_pending = true;
            s->system_began = in->tick;
        }
        if (in->buttons & VCO_START)
            s->screenshot = true;
        if (!s->screenshot && (uint32_t)(in->tick - s->system_began) >= 250000u) {
            vco_input_closed(s);
            return VCO_CLOSE;
        }
        return VCO_NONE;
    }
    if (s->open && s->system_pending) {
        if (s->screenshot) {
            if (!vco_neutral(in))
                return VCO_NONE;
            s->system_pending = s->screenshot = false;
            s->wait_neutral = true;
            s->pending_button = 0;
            s->chord_blocked = false;
            return VCO_NONE;
        }
        vco_input_closed(s);
        return VCO_CLOSE;
    }
    if (in->buttons & VCO_PS) {
        s->pending_button = 0;
        s->chord_blocked = false;
        return VCO_NONE;
    }
    if (paused) {
        s->pending_button = 0;
        s->chord_blocked = false;
        s->wait_neutral = true;
        return VCO_NONE;
    }
    if (s->wait_neutral) {
        s->pending_button = 0;
        s->chord_blocked = false;
        if (vco_neutral(in) && touch_neutral)
            s->wait_neutral = false;
        return VCO_NONE;
    }
    if (!seeded)
        return VCO_NONE;
    uint32_t combo = mapping->bindings.open;
    bool combo_edge = (in->buttons & combo) == combo && (edge & combo);
    if (!s->open) {
        s->pending_button = 0;
        s->chord_blocked = false;
        if (combo_edge) {
            s->wait_neutral = true;
            return VCO_OPEN;
        }
        return VCO_NONE;
    }
    /* User-requested exits from settings share the unsaved-draft prompt. PS,
     * capture loss and lease expiry were already handled above as forced exits. */
    if (combo_edge) {
        s->pending_button = 0;
        s->chord_blocked = false;
        if (settings) {
            s->wait_neutral = true;
            return VCO_BACK;
        }
        vco_input_closed(s);
        return VCO_CLOSE;
    }
    VcoAction action = VCO_NONE;
    if (!in->buttons) {
        if (!vco_neutral(in) || !touch_neutral)
            return VCO_NONE;
        if (s->pending_button && !s->chord_blocked)
            action = vco_input_button_action(s->pending_button, settings, mapping);
        s->pending_button = 0;
        s->chord_blocked = false;
    } else if (vco_button_count(in->buttons) != 1) {
        /* Never queue constituent actions from a multi-button gesture. In
         * particular, returning to one held key cannot resurrect an action. */
        s->pending_button = 0;
        s->chord_blocked = true;
    } else if (!s->chord_blocked) {
        if (s->pending_button) {
            if (in->buttons != s->pending_button) {
                s->pending_button = 0;
                s->chord_blocked = true;
            }
        } else if (edge & in->buttons) {
            action = vco_input_button_action(in->buttons, settings, mapping);
            /* A single key that could grow into the open/close combo must not
             * toggle a cheat before the remaining combo key arrives. */
            if (action != VCO_NONE && (in->buttons & combo)) {
                s->pending_button = in->buttons;
                action = VCO_NONE;
            }
        }
    }
    if (action == VCO_CLOSE)
        vco_input_closed(s);
    return action;
}
static inline VcoAction vco_input_step_with_touch(VcoInputState *s, const VcoInput *in,
                                                  bool settings, const VcoSettings *mapping,
                                                  bool touch_neutral)
{
    return vco_input_step_with_capture(s, in, settings, mapping, touch_neutral, false);
}
static inline VcoAction vco_input_step_mapped(VcoInputState *s, const VcoInput *in, bool settings,
                                              const VcoSettings *mapping)
{
    return vco_input_step_with_touch(s, in, settings, mapping, true);
}
static inline VcoAction vco_input_step_context(VcoInputState *s, const VcoInput *in, bool settings)
{
    VcoSettings defaults = vco_settings_defaults();
    return vco_input_step_mapped(s, in, settings, &defaults);
}
static inline VcoAction vco_input_step(VcoInputState *s, const VcoInput *in)
{
    return vco_input_step_context(s, in, false);
}
#endif
