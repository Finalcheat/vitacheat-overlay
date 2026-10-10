#ifndef VCO_GAME_NATIVE_ACCESS_H
#define VCO_GAME_NATIVE_ACCESS_H
#include "protocol.h"
#define VCO_GAME_MODULE_LIMIT 256u
/* Fixed-width SDK metadata, never native pointers or object references. The
 * extra list slot distinguishes exact capacity from a truncated SDK list. */
typedef struct {
    uint32_t count;
    int32_t ids[VCO_GAME_MODULE_LIMIT + 1u];
} VcoGameModuleList;
typedef struct {
    uint32_t base, size, permissions;
} VcoGameModuleSegment;
typedef struct {
    int32_t uid, process_uid;
    uint32_t attributes, version, state, reserved;
    char name[28];
    VcoGameModuleSegment segments[4];
} VcoGameModuleInfo;
typedef struct {
    uint64_t stamp;
    uint32_t buttons, device, capture_epoch, capturing;
} VcoGamePadSample;
/* Resident loader calls once; firmware exports are resolved, not imported with
 * a 3.60-only NID. No game object/patch/range reference is acquired. */
int vco_agent_access_start(void);
int vcoAgentRead(const VcoAgentMemoryRequest *, uint32_t length);
int vcoAgentWrite(const VcoAgentMemoryRequest *, uint32_t length);
int vcoAgentModules(uint64_t, VcoGameModuleList *, uint32_t length);
int vcoAgentModuleInfo(const VcoAgentModuleRequest *, uint32_t length);
int vcoAgentPad(uint64_t, VcoGamePadSample *, uint32_t length);
#endif
