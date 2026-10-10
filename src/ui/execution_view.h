#ifndef VCO_EXECUTION_VIEW_H
#define VCO_EXECUTION_VIEW_H
#include "../catalog/catalog.h"
#include "../catalog/source.h"
#include "../platform/execution_service.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    VCO_DISPLAY_UNKNOWN,
    VCO_DISPLAY_OFF,
    VCO_DISPLAY_ENABLING,
    VCO_DISPLAY_ON,
    VCO_DISPLAY_DISABLING,
    VCO_DISPLAY_PAUSED,
    VCO_DISPLAY_ERROR,
    VCO_DISPLAY_UNSUPPORTED,
    VCO_DISPLAY_QUEUED
} VcoExecutionDisplay;
typedef enum {
    VCO_NOTICE_NONE,
    VCO_NOTICE_BUSY,
    VCO_NOTICE_UNAVAILABLE,
    VCO_NOTICE_RELOADING
} VcoExecutionNotice;
/* Covers the interval between a successful nonblocking SET and the next service
 * snapshot. These bits never turn a row ON; service queued/pending owns progress. */
typedef struct {
    uint64_t session, list_revision;
    uint32_t view_revision;
    uint32_t submitted_revision[VCO_ENGINE_ENTRIES];
    bool pending[VCO_ENGINE_ENTRIES];
    bool seeded;
} VcoExecutionUi;
bool vco_execution_view_matches(const VcoCatalog *, const VcoExecutionView *);
void vco_execution_ui_sync(VcoExecutionUi *, const VcoExecutionView *);
void vco_execution_ui_submitted(VcoExecutionUi *, const VcoExecutionView *, unsigned entry);
VcoExecutionDisplay vco_execution_display(const VcoCatalog *, const VcoExecutionView *,
                                          const VcoExecutionUi *, unsigned entry);
bool vco_execution_toggle_target(const VcoCatalog *, const VcoExecutionView *,
                                 const VcoExecutionUi *, unsigned entry, bool *enabled);
const char *vco_execution_display_text(VcoExecutionDisplay, int language);
const char *vco_execution_state_text(uint32_t state, int language);
const char *vco_execution_error_text(uint32_t error, int language);
const char *vco_execution_notice_text(VcoExecutionNotice, int language);
/* Header presentation uses the observed game version, never the file's claim. */
void vco_catalog_identity_text(const VcoCatalog *, char *out, size_t capacity);
const char *vco_catalog_compatibility_text(VcoCompatibility, int language);
const char *vco_catalog_source_text(VcoSourceCheckState, int language);
#ifdef __cplusplus
}
#endif
#endif
