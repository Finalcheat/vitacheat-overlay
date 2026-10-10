#include "touch.h"
#include "../settings.h"
#include <string.h>
static VcoTouchEvent none(void)
{
    return (VcoTouchEvent){VCO_TOUCH_NONE, 0, 0};
}
bool vco_touch_map(int value, int min, int max, unsigned pixels, uint16_t *out)
{
    if (!out || !pixels || pixels > 65536 || min < 0 || max > 32767 || max <= min || value < min ||
        value > max)
        return false;
    *out = (uint16_t)((uint32_t)(value - min) * (pixels - 1) / (unsigned)(max - min));
    return true;
}
bool vco_touch_neutral(const VcoTouchSample *s)
{
    return s && s->abi == VCO_TOUCH_ABI && (s->available & ~3u) == 0 &&
           (s->valid & s->available) == s->available && !s->front_count && !s->back_count;
}
void vco_touch_reset(VcoTouchState *s)
{
    memset(s, 0, sizeof(*s));
}
VcoTouchEvent vco_touch_hit(unsigned x, unsigned y, VcoTouchLayout layout)
{
    if (x >= VCO_SCREEN_WIDTH || y >= VCO_SCREEN_HEIGHT)
        return none();
    if (y >= 8 && y < 50) {
        if (x >= 790 && x < 932)
            return (VcoTouchEvent){VCO_TOUCH_BACK_ACTION, 0, 0};
        if (x >= 590 && x < 774 && !layout.settings)
            return (VcoTouchEvent){VCO_TOUCH_SETTINGS, 0, 0};
    }
    if (y >= 94 && y < 139 && !layout.confirm) {
        if (x >= 728 && x < 800)
            return (VcoTouchEvent){layout.settings ? VCO_TOUCH_UP : VCO_TOUCH_PREV, 0, 0};
        if (x >= 856 && x < 932)
            return (VcoTouchEvent){layout.settings ? VCO_TOUCH_DOWN : VCO_TOUCH_NEXT, 0, 0};
    }
    if (x < 28 || x >= 932 || y < VCO_ROW_TOP)
        return none();
    unsigned row = (y - VCO_ROW_TOP) / VCO_ROW_STEP;
    if (row >= VCO_VISIBLE_ROWS || (y - VCO_ROW_TOP) % VCO_ROW_STEP >= VCO_ROW_HEIGHT)
        return none();
    unsigned item = layout.settings ? layout.first + row : row;
    if (item >= layout.count)
        return none();
    if (!layout.settings)
        return (VcoTouchEvent){x >= 790 ? VCO_TOUCH_TOGGLE : VCO_TOUCH_SELECT, item, 0};
    if (layout.confirm || item >= VCO_SETTING_APPLY)
        return (VcoTouchEvent){VCO_TOUCH_ACCEPT, item, 0};
    if (item == VCO_SETTING_OPACITY && x >= VCO_OPACITY_HIT_LEFT && x < VCO_OPACITY_HIT_RIGHT)
        return (VcoTouchEvent){VCO_TOUCH_OPACITY, item, vco_touch_opacity(x)};
    return (VcoTouchEvent){x < 790   ? VCO_TOUCH_SELECT
                           : x < 860 ? VCO_TOUCH_DECREASE
                                     : VCO_TOUCH_INCREASE,
                           item, 0};
}
VcoTouchEvent vco_touch_step(VcoTouchState *s, const VcoTouchSample *in, VcoTouchLayout layout,
                             uint64_t context)
{
    VcoTouchEvent event = none();
    if (!s || !in)
        return event;
    if (in->abi != VCO_TOUCH_ABI || !(in->available & VCO_TOUCH_FRONT) ||
        !(in->valid & VCO_TOUCH_FRONT) || in->front_count > 8 ||
        (s->down && (uint32_t)(in->tick - s->previous_tick) >= 500000u)) {
        vco_touch_reset(s);
        return event;
    }
    s->previous_tick = in->tick;
    if (!s->armed) {
        if (!in->front_count)
            s->armed = true;
        return event;
    }
    if (in->front_count > 1 ||
        (in->front_count && (in->x >= VCO_SCREEN_WIDTH || in->y >= VCO_SCREEN_HEIGHT))) {
        s->cancelled = true;
        s->down = true;
        return event;
    }
    if (in->front_count == 1) {
        bool started = !s->down;
        if (!s->down) {
            s->down = true;
            s->cancelled = false;
            s->moved = false;
            s->id = in->id;
            s->x = in->x;
            s->y = in->y;
            s->began = in->tick;
            s->context = context;
            s->first = layout.first;
            s->target = vco_touch_hit(in->x, in->y, layout);
        } else if (s->id != in->id || s->context != context)
            s->cancelled = true;
        int move_x = (int)in->x - s->x, move_y = (int)in->y - s->y;
        if (move_x < -16 || move_x > 16 || move_y < -16 || move_y > 16)
            s->moved = true;
        s->last_x = in->x;
        s->last_y = in->y;
        if (s->target.action == VCO_TOUCH_OPACITY && !s->cancelled) {
            /* Capture a slider drag until release, independently of tap time
             * limits. Leaving its row cancels; it must never become a swipe. */
            if (!layout.settings || layout.confirm || layout.first > VCO_SETTING_OPACITY ||
                layout.count <= VCO_SETTING_OPACITY) {
                s->cancelled = true;
                return event;
            }
            int y = vco_touch_row_center(VCO_SETTING_OPACITY - layout.first);
            if ((int)in->y < y - VCO_ROW_HEIGHT / 2 - 16 ||
                (int)in->y > y + VCO_ROW_HEIGHT / 2 + 16) {
                s->cancelled = true;
                return event;
            }
            unsigned value = vco_touch_opacity(in->x);
            if (started || value != s->target.value) {
                s->target.value = value;
                event = s->target;
            }
        }
        return event;
    }
    if (!s->down)
        return event;
    if (!s->cancelled && s->target.action != VCO_TOUCH_OPACITY && s->context == context &&
        (uint32_t)(in->tick - s->began) <= 800000u) {
        int dx = (int)s->last_x - s->x, dy = (int)s->last_y - s->y;
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        VcoTouchEvent end = vco_touch_hit(s->last_x, s->last_y, layout);
        if (!s->moved && ax <= 16 && ay <= 16 && end.action == s->target.action &&
            end.row == s->target.row)
            event = end;
        else if (s->y >= VCO_ROW_TOP && s->y < VCO_ROW_TOP + VCO_ROW_STEP * 6) {
            if (!layout.settings && ax >= 80 && ay <= 40)
                event.action = dx < 0 ? VCO_TOUCH_NEXT : VCO_TOUCH_PREV;
            else if (layout.settings && ay >= 60 && ax <= 40)
                event.action = dy < 0 ? VCO_TOUCH_DOWN : VCO_TOUCH_UP;
        }
    }
    s->down = false;
    s->cancelled = false;
    return event;
}
