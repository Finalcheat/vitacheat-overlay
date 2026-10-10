#ifndef VCO_TEST_NATIVE_CTRL_H
#define VCO_TEST_NATIVE_CTRL_H
#include <stdint.h>
/* Minimal declarations matching VitaSDK psp2common/ctrl.h and psp2kern/ctrl.h.
 * The production source is compiled unchanged against these injectable calls. */
typedef struct {
    uint64_t timeStamp;
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t up, right, down, left, lt, rt, l1, r1;
    uint8_t triangle, circle, cross, square, reserved[4];
} SceCtrlData;
typedef struct {
    uint8_t port[5], unk[11];
} SceCtrlPortInfo;
enum {
    SCE_CTRL_SELECT = 0x1,
    SCE_CTRL_START = 0x8,
    SCE_CTRL_UP = 0x10,
    SCE_CTRL_RIGHT = 0x20,
    SCE_CTRL_DOWN = 0x40,
    SCE_CTRL_LEFT = 0x80,
    SCE_CTRL_LTRIGGER = 0x100,
    SCE_CTRL_RTRIGGER = 0x200,
    SCE_CTRL_TRIANGLE = 0x1000,
    SCE_CTRL_CIRCLE = 0x2000,
    SCE_CTRL_CROSS = 0x4000,
    SCE_CTRL_SQUARE = 0x8000,
    SCE_CTRL_PSBUTTON = 0x10000,
    SCE_CTRL_HEADPHONE = 0x80000,
    SCE_CTRL_VOLUP = 0x100000,
    SCE_CTRL_VOLDOWN = 0x200000,
    SCE_CTRL_POWER = 0x40000000,
    SCE_CTRL_TYPE_UNPAIRED = 0,
    SCE_CTRL_TYPE_PHY = 1,
    SCE_CTRL_TYPE_VIRT = 2,
    SCE_CTRL_TYPE_DS3 = 4,
    SCE_CTRL_TYPE_DS4 = 8
};
int ksceCtrlGetControllerPortInfo(SceCtrlPortInfo *);
int ksceCtrlPeekBufferPositive(int, SceCtrlData *, int);
#endif
