#include <stdint.h>
/* No SELF is generated. Only provide the input readiness/capture callbacks
 * supplied by the resident kernel when the new backend is integrated. */
int vco_kernel_input_ready(void)
{
    return 0;
}
void vco_kernel_capture_reset(void) {}
int vco_kernel_capturing(void)
{
    return 0;
}
uint32_t vco_kernel_capture_epoch(void)
{
    return 0;
}
int module_start(unsigned size, const void *arguments)
{
    (void)size;
    (void)arguments;
    return 2;
}
int module_stop(unsigned size, const void *arguments)
{
    (void)size;
    (void)arguments;
    return 1;
}
