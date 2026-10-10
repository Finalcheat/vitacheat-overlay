#ifndef VCO_CATALOG_SOURCE_H
#define VCO_CATALOG_SOURCE_H
#include "catalog.h"
#include <stdbool.h>
typedef struct {
    uint64_t hash;
    uint32_t length;
    VcoFileResult result;
    VcoListSource source;
} VcoSourceStamp;
typedef enum {
    VCO_SOURCE_UNCHECKED,
    VCO_SOURCE_CHECKING,
    VCO_SOURCE_UNCHANGED,
    VCO_SOURCE_CHANGED,
    VCO_SOURCE_UNAVAILABLE
} VcoSourceCheckState;
/* A separate presentation revision: a check never rebuilds the loaded list or
 * changes its catalog revision, binding or desired ON state. */
typedef struct {
    uint32_t revision, catalog_revision;
    VcoSourceCheckState state;
} VcoCatalogSourceView;
void vco_source_begin(VcoSourceStamp *, VcoListSource);
bool vco_source_add(VcoSourceStamp *, const void *, size_t);
VcoSourceCheckState vco_source_compare(const VcoSourceStamp *, const VcoSourceStamp *);
#endif
