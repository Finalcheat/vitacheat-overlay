#ifndef VCO_AGENT_USER_BRIDGE_H
#define VCO_AGENT_USER_BRIDGE_H
#include "native_service.h"
#include "native_access.h"

/* Ordinary user C helpers marshal the raw two-word syscall requests. Keep the
 * kernel-facing declarations separate so a plain C call cannot accidentally
 * hide stack arguments in a host-only test. */
static inline int vco_game_bridge_poll(uint64_t session, VcoAgentMessage *message, uint32_t length,
                                       void *payload, uint32_t capacity)
{
    VcoAgentPollRequest request = {.session = session,
                                   .abi = VCO_AGENT_ABI,
                                   .message = (uintptr_t)message,
                                   .payload = (uintptr_t)payload,
                                   .message_length = length,
                                   .capacity = capacity};
    return vcoAgentPoll(&request, sizeof(request));
}
static inline int vco_game_bridge_read(uint64_t session, uint32_t address, void *buffer,
                                       uint32_t width)
{
    VcoAgentMemoryRequest request = {.session = session,
                                     .abi = VCO_AGENT_ABI,
                                     .address = address,
                                     .buffer = (uintptr_t)buffer,
                                     .width = width};
    return vcoAgentRead(&request, sizeof(request));
}
static inline int vco_game_bridge_write(uint64_t session, uint32_t address, const void *buffer,
                                        uint32_t width)
{
    VcoAgentMemoryRequest request = {.session = session,
                                     .abi = VCO_AGENT_ABI,
                                     .address = address,
                                     .buffer = (uintptr_t)buffer,
                                     .width = width};
    return vcoAgentWrite(&request, sizeof(request));
}
static inline int vco_game_bridge_module_info(uint64_t session, int32_t uid,
                                              VcoGameModuleInfo *output, uint32_t length)
{
    VcoAgentModuleRequest request = {.session = session,
                                     .abi = VCO_AGENT_ABI,
                                     .uid = uid,
                                     .output = (uintptr_t)output,
                                     .length = length};
    return vcoAgentModuleInfo(&request, sizeof(request));
}
#endif
