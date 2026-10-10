#ifndef VCO_BUTTONS_H
#define VCO_BUTTONS_H
#include <stdint.h>
/* Ordinary Vita control bits; PS and non-game system bits remain reserved. */
#define VCO_START 0x00000008u
#define VCO_SELECT 0x00000001u
#define VCO_UP 0x00000010u
#define VCO_RIGHT 0x00000020u
#define VCO_DOWN 0x00000040u
#define VCO_LEFT 0x00000080u
#define VCO_L 0x00000100u
#define VCO_R 0x00000200u
#define VCO_SQUARE 0x00008000u
#define VCO_CIRCLE 0x00002000u
#define VCO_TRIANGLE 0x00001000u
#define VCO_CROSS 0x00004000u
#define VCO_PS 0x00010000u
#define VCO_BUTTON_MASK 0x0000f3f9u
#define VCO_COMBO (VCO_L | VCO_SELECT) /* Default, never the configured mapping. */
static inline unsigned vco_button_count(uint32_t mask)
{
    unsigned count = 0;
    while (mask) {
        mask &= mask - 1;
        ++count;
    }
    return count;
}
#endif
