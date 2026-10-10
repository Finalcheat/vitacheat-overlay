/* No SELF. Keep the native user taiHEN calls while the VCO resident syscall
 * stubs await formal exports. These placeholders provide no runtime behavior. */
#include "agent/native_access.h"
#include "agent/native_service.h"
int vcoAgentPermission(uint64_t s, uint32_t f)
{
    (void)s;
    (void)f;
    return VCO_AGENT_CLOSED;
}
int vcoAgentTrace(const VcoAgentTraceRequest *p, uint32_t n)
{
    (void)p;
    (void)n;
    return VCO_AGENT_CLOSED;
}
int vcoAgentRead(const VcoAgentMemoryRequest *p, uint32_t n)
{
    (void)p;
    (void)n;
    return VCO_AGENT_CLOSED;
}
int vcoAgentWrite(const VcoAgentMemoryRequest *p, uint32_t n)
{
    (void)p;
    (void)n;
    return VCO_AGENT_CLOSED;
}
int vcoAgentModules(uint64_t s, VcoGameModuleList *p, uint32_t n)
{
    (void)s;
    (void)p;
    (void)n;
    return VCO_AGENT_CLOSED;
}
int vcoAgentModuleInfo(const VcoAgentModuleRequest *p, uint32_t n)
{
    (void)p;
    (void)n;
    return VCO_AGENT_CLOSED;
}
int vcoAgentPad(uint64_t s, VcoGamePadSample *p, uint32_t n)
{
    (void)s;
    (void)p;
    (void)n;
    return VCO_AGENT_CLOSED;
}
int module_start(unsigned n, const void *p)
{
    (void)n;
    (void)p;
    return 2;
}
int module_stop(unsigned n, const void *p)
{
    (void)n;
    (void)p;
    return 1;
}
