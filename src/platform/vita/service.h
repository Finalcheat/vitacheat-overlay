#ifndef VCO_VITA_SERVICE_H
#define VCO_VITA_SERVICE_H
#include <stdint.h>
/* Kernel-only, invoked from syscall context. Binds independently verified
 * Shell identity; never grants ownership to the first arbitrary caller. */
int vco_service_authorize_caller(int32_t caller);
/* Syscall/helper scope, without ENTER/EXIT_SYSCALL. Zero admits the verified
 * Shell and must be paired with exactly one end after ALL copies/mutations.
 * Nonzero is VcoMailboxStatus, owns no scope, and needs no end. Input helpers
 * remain available even if the execution backend could not start. */
int vco_service_call_begin(int32_t caller);
void vco_service_call_end(void);
#endif
