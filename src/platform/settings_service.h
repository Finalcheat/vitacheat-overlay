#ifndef VCO_SETTINGS_SERVICE_H
#define VCO_SETTINGS_SERVICE_H
#include "../settings.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    VCO_SAVE_READY,
    VCO_SAVE_PENDING,
    VCO_SAVE_OK,
    VCO_SAVE_FAILED,
    VCO_SAVE_RECOVERED,
    VCO_SAVE_INVALID
} VcoSaveStatus;
/* Boot thread only; loads before registering the UI callback. */
int vco_settings_service_start(VcoSettings *out);
/* Nonblocking UI calls; false means busy/unavailable, caller retains retry. */
bool vco_settings_service_request(const VcoSettings *value);
VcoSaveStatus vco_settings_service_status(void);
#ifdef __cplusplus
}
#endif
#endif
