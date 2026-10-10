#ifndef VCO_OVERLAY_VIEW_H
#define VCO_OVERLAY_VIEW_H
#include "catalog.h"
#include "settings.h"
#include "layout.h"
#include "platform/settings_service.h"
#include "execution_view.h"
#include "source.h"
#include <stdint.h>
#define VCO_PAGE_SIZE VCO_UI_VISIBLE_ROWS
#define VCO_SETTING_RELOAD VCO_SETTING_COUNT
#define VCO_VIEW_SETTING_COUNT (VCO_SETTING_COUNT + 1u)
bool vco_view_open(uint32_t tick);
void vco_view_close(uint32_t tick);
void vco_view_render(const VcoCatalog &catalog, const VcoExecutionView &execution,
                     const VcoExecutionUi &commands, const VcoCatalogSourceView &source,
                     unsigned page, unsigned selected, int language, VcoExecutionNotice notice);
/* Reflow the inline title/version after PAF has resolved text metrics. */
void vco_view_update_header_layout(void);
void vco_view_opacity(unsigned opacity);
void vco_view_settings(const VcoSettingsState &settings, const VcoCatalog &catalog,
                       int system_language, bool reloading, unsigned first);
void vco_view_bindings(const VcoSettings &settings, int system_language);
void vco_view_save_status(VcoSaveStatus status, int language, bool in_settings = false);
#endif
