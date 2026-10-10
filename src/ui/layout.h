#ifndef VCO_UI_LAYOUT_H
#define VCO_UI_LAYOUT_H
/* Screen coordinates; PAF uses a centered origin with positive Y upwards. */
#define VCO_UI_SCREEN_WIDTH 960
#define VCO_UI_SCREEN_HEIGHT 544
#define VCO_UI_MARGIN 32
#define VCO_UI_ROW_TOP 96
#define VCO_UI_ROW_HEIGHT 48
#define VCO_UI_ROW_STEP 58
#define VCO_UI_VISIBLE_ROWS 6u
#define VCO_UI_OPACITY_LEFT 520u
#define VCO_UI_OPACITY_RIGHT 768u
static inline int vco_ui_row_center(unsigned row)
{
    return VCO_UI_ROW_TOP + VCO_UI_ROW_HEIGHT / 2 + (int)row * VCO_UI_ROW_STEP;
}
#endif
