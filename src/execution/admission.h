#ifndef VCO_ADMISSION_H
#define VCO_ADMISSION_H
#include <stdint.h>

/* One lifetime, one irreversible close. Initialize before publishing to any
 * thread. Never reset/reuse storage until all potential callers are gone.
 * The owner must separately pin the protected resource and this gate's storage.
 * Every successful enter requires exactly one leave, after ALL resource access
 * and ownership handoff. This gate does not serialize admitted operations.
 * No waiting or scheduling is performed here. */
typedef struct {
    uint32_t state;
} VcoAdmission;
#define VCO_ADMISSION_INITIALIZER {0}
int vco_admission_enter(VcoAdmission *gate);
void vco_admission_close(VcoAdmission *gate);
/* Observation only: false never authorizes a later operation. NULL is closed. */
int vco_admission_closed(const VcoAdmission *gate);
int vco_admission_leave(VcoAdmission *gate);
/* True only when closed AND empty; acquire-observes completed operations.
 * Does not elect a cleanup owner: callers must arrange exclusive cleanup. */
int vco_admission_drained(const VcoAdmission *gate);
#endif
