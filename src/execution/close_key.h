#ifndef VCO_CLOSE_KEY_H
#define VCO_CLOSE_KEY_H
#include "session.h"
#include "../cheats/cheats.h"

typedef enum { VCO_CLOSE_LIST, VCO_CLOSE_PROCESS, VCO_CLOSE_MODULE } VcoEngineCloseScope;
/* Internal worker protocol, not control ABI. Opaque object tags are compared,
 * never dereferenced or acquired here. Native callers must retain/verify the
 * objects and protect mappings independently before closing them. A module
 * boundary conservatively closes the entire list, including ordinary writes. */
typedef struct {
    VcoIdentity identity;
    uint64_t session, revision, transaction;
    VcoEngineCloseScope scope;
    uintptr_t process_object, module_object;
    int32_t module_uid;
    uint64_t module_generation;
} VcoEngineCloseKey;
typedef struct {
    VcoError entries, native;
} VcoEngineCloseResult;
/* Canonical fields, never padding bytes. These validate syntax/identity tags,
 * not the actual native objects, references, mappings, or wait context. */
int vco_close_key_valid(const VcoEngineCloseKey *);
int vco_close_key_equal(const VcoEngineCloseKey *, const VcoEngineCloseKey *);
#endif
