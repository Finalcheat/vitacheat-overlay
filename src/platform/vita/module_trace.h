#ifndef VCO_VITA_MODULE_TRACE_H
#define VCO_VITA_MODULE_TRACE_H
#include <stdint.h>

/* Worker-only diagnostics in the existing RAM ring. Never perform I/O or
 * reenter the provider in this callback. Numeric subjects are PID/module UID;
 * no retained kernel object pointer is exposed. Event values are stable. */
typedef void (*VcoVitaModuleTrace)(void *, uint32_t, int32_t, uint32_t, uint32_t);
typedef enum {
    VCO_MODULE_TRACE_INIT = 96, /* detail: available exports, List=1/Info=2/Main=4 */
    VCO_MODULE_TRACE_AUTHORITY,
    VCO_MODULE_TRACE_ENUMERATE, /* raw SDK result, subject=PID, detail=count */
    VCO_MODULE_TRACE_COUNT,     /* empty/truncated list or invalid/duplicate UID */
    VCO_MODULE_TRACE_MAIN,      /* raw SDK result or missing main UID */
    VCO_MODULE_TRACE_INFO,      /* raw SDK result, subject=query GUID */
    VCO_MODULE_TRACE_FIELDS,    /* detail: 1 PUID, 2 name, 3 path, 4+segment */
    VCO_MODULE_TRACE_LIST_CHANGED,
    VCO_MODULE_TRACE_RECORD_CHANGED,
    VCO_MODULE_TRACE_SNAPSHOT, /* success only, subject=PID, detail=count */
    VCO_MODULE_TRACE_MAPPING,
    VCO_MODULE_TRACE_UNAVAILABLE,  /* detail=latched failure event; result=raw result */
    VCO_MODULE_TRACE_RANGE,        /* detail=state<<8|segment, subject=query GUID */
    VCO_MODULE_TRACE_SEGMENT_BASE, /* raw uint32 address in result, detail=segment */
    VCO_MODULE_TRACE_SEGMENT_SIZE, /* raw memory size in result, detail=segment */
    VCO_MODULE_TRACE_SEGMENT_PERMISSIONS,
    VCO_MODULE_TRACE_EMPTY /* first addressed zero-length segment per snapshot */
} VcoVitaModuleTraceEvent;
typedef struct {
    uint32_t event, subject, detail;
    int32_t result;
} VcoVitaModuleFailure;
#endif
