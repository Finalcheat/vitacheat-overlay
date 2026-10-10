#ifndef TEST_TOUCH_SDK_H
#define TEST_TOUCH_SDK_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceUInt32;
#define SCE_TOUCH_MAX_REPORT 8
typedef struct {
    int16_t minAaX, minAaY, maxAaX, maxAaY;
    int16_t minDispX, minDispY, maxDispX, maxDispY;
    uint8_t minForce, maxForce, reserved[30];
} SceTouchPanelInfo;
typedef struct {
    uint8_t id, force;
    int16_t x, y;
    uint8_t reserved[8];
    uint16_t info;
} SceTouchReport;
typedef struct {
    uint64_t timeStamp;
    uint32_t status, reportNum;
    SceTouchReport report[8];
} SceTouchData;
#endif
