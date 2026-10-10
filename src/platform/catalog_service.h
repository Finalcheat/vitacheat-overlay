#ifndef VCO_CATALOG_SERVICE_H
#define VCO_CATALOG_SERVICE_H
#include "catalog.h"
#include "source.h"
#ifdef __cplusplus
extern "C" {
#endif
int vco_catalog_service_start(void);
void vco_catalog_service_refresh(void);
/* Policy affects the next file load only; never implicitly reload/clear/enable. */
void vco_catalog_service_encoding(VcoEncoding policy);
/* Only queues an asynchronous source check. No file I/O on the UI thread and
 * no implicit reload/clear/upload, even when the current file has changed. */
void vco_catalog_service_check_source(void);
int vco_catalog_service_source_copy(VcoCatalogSourceView *out);
/* Try-lock only: PAF never waits for disk or a worker lock. 1 new snapshot,
 * 0 unchanged/busy. The destination is owned exclusively by the UI thread. */
int vco_catalog_service_copy(VcoCatalog *out);
int vco_catalog_service_language(void); /* Stable VcoLanguage ID from localization.h. */
#ifdef __cplusplus
}
#endif
#endif
