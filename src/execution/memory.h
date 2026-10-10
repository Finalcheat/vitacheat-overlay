#ifndef VCO_EXECUTION_MEMORY_H
#define VCO_EXECUTION_MEMORY_H
#include "session.h"
#include "../cheats/cheats.h"
/* Permission and lifetime checks are adapters, independently fault-injectable.
 * The lock pins the process instance and range until unlock. Inspect/copy must
 * target that pinned instance. No adapter may silently fall back to current PID. */
#define VCO_MEM_READ 1u
#define VCO_MEM_WRITE 2u
#define VCO_MEM_EXECUTE 4u
/* Lost authority maps to VCO_E_SESSION. Other memory failures map to VCO_E_IO;
 * native diagnostics preserve their finer stage. */
typedef enum {
    VCO_MEMORY_ARGUMENT = -2,
    VCO_MEMORY_AUTHORITY = VCO_ACCESS_SESSION,
    VCO_MEMORY_CAPABILITY = -4,
    VCO_MEMORY_LOCK = -5,
    VCO_MEMORY_INSPECT = -6,
    VCO_MEMORY_REGION = -7,
    VCO_MEMORY_PERMISSION = -8,
    VCO_MEMORY_COPY = -9,
    VCO_MEMORY_UNLOCK = -10
} VcoMemoryFailure;
typedef struct {
    uint32_t base, size, permissions;
    uint64_t instance;
} VcoMemoryRegion;
typedef struct {
    void *context;
    int (*lock)(void *, const VcoIdentity *, uint32_t, size_t);
    int (*inspect)(void *, const VcoIdentity *, uint32_t, size_t, VcoMemoryRegion *);
    int (*read)(void *, const VcoIdentity *, uint32_t, uint8_t *, size_t);
    int (*write)(void *, const VcoIdentity *, uint32_t, const uint8_t *, size_t);
    int (*unlock)(void *, const VcoIdentity *, uint32_t, size_t);
    int (*validate)(void *, const VcoIdentity *, bool foreground);
} VcoMemoryTransport;
typedef struct {
    const VcoSession *session;
    VcoMemoryTransport transport;
} VcoMemoryAdapter;
/* Only data access is exposed here. A patches require a separate taiHEN adapter. */
VcoCheatMemory vco_memory_callbacks(VcoMemoryAdapter *adapter);
#endif
