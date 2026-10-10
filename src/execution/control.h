#ifndef VCO_EXECUTION_CONTROL_H
#define VCO_EXECUTION_CONTROL_H
#include "engine.h"
#define VCO_CONTROL_ABI 0x00010001u
#define VCO_CONTROL_PAGE 16u
#define VCO_CONTROL_SET 1u
#define VCO_CONTROL_QUERY 2u
#define VCO_CONTROL_FOREGROUND 1u
#define VCO_CONTROL_BLOCKED 2u
/* Wire structures contain no pointers, bool, size_t or native enums. 64-bit
 * fields precede 32-bit fields so ARM and host layouts match without packing. */
typedef struct {
    uint64_t request, session, revision;
    uint32_t abi, size, operation, entry, enabled, count, reserved[2];
} VcoControlRequest;
typedef struct {
    uint64_t acknowledged, pending;
    uint32_t state, error, line, desired;
} VcoControlRow;
typedef struct {
    uint64_t request, session, revision;
    uint32_t abi, size, status, error, first, count, total, flags;
    VcoControlRow rows[VCO_CONTROL_PAGE];
} VcoControlReply;
typedef enum {
    VCO_CONTROL_OK = 0,
    VCO_CONTROL_PACKET = 1,
    VCO_CONTROL_OWNER = 2,
    VCO_CONTROL_UNBOUND = 3,
    VCO_CONTROL_STALE = 4,
    VCO_CONTROL_REJECTED = 5
} VcoControlStatus;
/* Called on the engine worker after CopyFromUser/IPC copy, or while holding its
 * lock. caller/owner are trusted transport identities, NOT packet fields.
 * No syscall or cross-thread lock is implemented by this portable dispatcher.
 * OK for SET means accepted/idempotent; rows.pending/acknowledged carry progress.
 * Caller must allocate a complete reply; input may be unaligned. */
VcoControlStatus vco_control_dispatch(VcoEngine *engine, int32_t owner, int32_t caller,
                                      const void *packet, size_t length, VcoControlReply *reply);
#endif
