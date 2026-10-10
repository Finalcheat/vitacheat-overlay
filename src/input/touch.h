#ifndef VCO_TOUCH_H
#define VCO_TOUCH_H
#include <stdbool.h>
#include <stdint.h>
#define VCO_TOUCH_ABI 0x00010000u
#define VCO_TOUCH_FRONT 1u
#define VCO_TOUCH_BACK 2u
#define VCO_SCREEN_WIDTH 960
#define VCO_SCREEN_HEIGHT 544
#define VCO_ROW_TOP 145
#define VCO_ROW_HEIGHT 46
#define VCO_ROW_STEP 52
#define VCO_VISIBLE_ROWS 6u
/* Shared screen-space geometry for the settings slider and its touch target. */
#define VCO_OPACITY_LEFT 520u
#define VCO_OPACITY_RIGHT 768u
#define VCO_OPACITY_HIT_LEFT 496u
#define VCO_OPACITY_HIT_RIGHT 790u
static inline unsigned vco_touch_opacity(unsigned x)
{
    if (x <= VCO_OPACITY_LEFT)
        return 0;
    if (x >= VCO_OPACITY_RIGHT)
        return 100;
    unsigned width = VCO_OPACITY_RIGHT - VCO_OPACITY_LEFT;
    return ((x - VCO_OPACITY_LEFT) * 20u + width / 2u) / width * 5u;
}
static inline int vco_touch_row_center(unsigned row)
{
    return VCO_ROW_TOP + VCO_ROW_HEIGHT / 2 + (int)row * VCO_ROW_STEP;
}
typedef struct {
    uint32_t abi, tick, available, valid, front_count, back_count;
    uint16_t x, y;
    uint8_t id, reserved[3];
} VcoTouchSample;
typedef char VcoTouchSampleSize[(sizeof(VcoTouchSample) == 32) ? 1 : -1];
typedef enum {
    VCO_TOUCH_NONE,
    VCO_TOUCH_SELECT,
    VCO_TOUCH_TOGGLE,
    VCO_TOUCH_PREV,
    VCO_TOUCH_NEXT,
    VCO_TOUCH_SETTINGS,
    VCO_TOUCH_BACK_ACTION,
    VCO_TOUCH_DECREASE,
    VCO_TOUCH_INCREASE,
    VCO_TOUCH_ACCEPT,
    VCO_TOUCH_UP,
    VCO_TOUCH_DOWN,
    VCO_TOUCH_OPACITY
} VcoTouchAction;
typedef struct {
    VcoTouchAction action;
    unsigned row;
    unsigned value; /* OPACITY only: 0..100, in steps of five */
} VcoTouchEvent;
typedef struct {
    bool settings, confirm;
    unsigned first, count; /* first visible absolute setting; visible list count */
} VcoTouchLayout;
typedef struct {
    bool armed, down, cancelled, moved;
    uint8_t id;
    uint16_t x, y, last_x, last_y;
    uint32_t began, previous_tick;
    unsigned first; /* viewport at gesture start */
    uint64_t context;
    VcoTouchEvent target;
} VcoTouchState;
#ifdef __cplusplus
extern "C" {
#endif
bool vco_touch_neutral(const VcoTouchSample *sample);
void vco_touch_reset(VcoTouchState *state);
VcoTouchEvent vco_touch_hit(unsigned x, unsigned y, VcoTouchLayout layout);
VcoTouchEvent vco_touch_step(VcoTouchState *state, const VcoTouchSample *sample,
                             VcoTouchLayout layout, uint64_t context);
bool vco_touch_map(int value, int min, int max, unsigned pixels, uint16_t *out);
int vcoPollTouch(VcoTouchSample *out);
#ifdef __cplusplus
}
#endif
#endif
