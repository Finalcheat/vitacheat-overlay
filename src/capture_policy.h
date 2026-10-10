#ifndef VCO_CAPTURE_POLICY_H
#define VCO_CAPTURE_POLICY_H
#include <stdbool.h>
#include "bridge.h"
/* Closed overlays cannot extend their capture deadline through polling. */
static inline bool vco_capture_expired(uint32_t mode, uint32_t elapsed)
{
    return mode == VCO_CAPTURE_OFF ||
           elapsed >= (mode == VCO_CAPTURE_DRAIN ? VCO_DRAIN_US : VCO_LEASE_US);
}
static inline bool vco_capture_renewable(uint32_t mode)
{
    return mode == VCO_CAPTURE_ACTIVE;
}
#endif
