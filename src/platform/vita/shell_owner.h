#ifndef VCO_VITA_SHELL_OWNER_H
#define VCO_VITA_SHELL_OWNER_H
#include <stdbool.h>
#include <stdint.h>
/* Kernel-owned RPC channel identity, independent of game-write authority.
 * Initialize exactly once; bind/unbind only with all RPC users quiescent.
 * The retained process object prevents a recycled PID from gaining access.
 * Fields stay immutable during authorization except atomic poison/error state.
 * Failed releases permanently quarantine this context: do not reinitialize it,
 * retry an ambiguous release, or discard it on an unbind error. */
typedef struct {
    void *process_class, *process_object;
    int32_t pid;
    int cleanup_error;
    uint32_t poisoned;
    bool bound;
} VcoVitaShellOwner;
void vco_vita_shell_owner_init(VcoVitaShellOwner *owner);
/* The PID comes from Sysroot, never from the caller or UI catalog. */
int vco_vita_shell_owner_bind(VcoVitaShellOwner *owner);
/* VcoVitaRpcAuthorize-compatible: zero authorizes, negative denies. */
int vco_vita_shell_owner_authorize(void *context, int32_t caller);
/* Observation only: 1=stable Sysroot PID differs or same PID now denotes a
 * different process object, 0=still the retained Shell, -1=unknown/fault.
 * A replacement observation never authorizes a caller or releases the binding.
 * Stop admission and quiesce all users before unbind/bind. */
int vco_vita_shell_owner_replacement(VcoVitaShellOwner *owner);
int vco_vita_shell_owner_unbind(VcoVitaShellOwner *owner);
#endif
