#ifndef VCO_TOUCH_KERNEL_H
#define VCO_TOUCH_KERNEL_H
#include "../input/touch.h"
int vco_touch_kernel_start(void);
/* Disable sampling/filtering, retaining every installed hook/ref until reboot.
 * This is not permission to unload code or re-install another set of hooks. */
void vco_touch_kernel_stop(void);
void vco_touch_kernel_sample(VcoTouchSample *out);
int vco_kernel_capturing(void);
uint32_t vco_kernel_capture_epoch(void);
#endif
