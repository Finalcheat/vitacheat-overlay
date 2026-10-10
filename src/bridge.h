#ifndef VCO_BRIDGE_H
#define VCO_BRIDGE_H
#include <stdint.h>
#define VCO_ABI 0x00020004u
/* Input bridge calls: 0 success, BUSY transient (output untouched), negative
 * actual failure. BUSY never means controller sampling failed. */
#define VCO_INPUT_BUSY 1
#define VCO_LEASE_US 500000u
#define VCO_DRAIN_US 250000u
#define VCO_CAPTURE_OFF 0
#define VCO_CAPTURE_ACTIVE 1
#define VCO_CAPTURE_DRAIN 2
typedef struct {
    uint32_t abi;
    uint32_t buttons;
    uint32_t tick;
    uint32_t captured;
    uint8_t lx, ly, rx, ry;
} VcoInput;
#ifdef __cplusplus
extern "C" {
#endif
int vcoPoll(VcoInput *out);
int vcoCapture(int enable);
#ifdef __cplusplus
}
#endif
#endif
