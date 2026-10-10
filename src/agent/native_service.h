#ifndef VCO_GAME_NATIVE_SERVICE_H
#define VCO_GAME_NATIVE_SERVICE_H
#include "protocol.h"
#include "broker.h"

/* Resident kernel startup/loader calls, not user exports. Callback terminal is
 * lock-free and only revokes. No game thread/patch/object is retained or joined. */
int vco_agent_kernel_start(void);
void vco_agent_kernel_loader_failed(int error);
int vco_agent_kernel_title(int32_t pid, char title[32]);
VcoAgentStatus vco_agent_kernel_begin(int32_t pid, const char title[10], VcoAgentBinding *);
void vco_agent_kernel_load_result(const VcoAgentBinding *, int32_t module, int32_t start_result);
void vco_agent_kernel_terminal(int32_t pid);
/* Called only by public user-module syscall hooks, before the original body.
 * begin has a 500ms clock deadline and 500 attempts, followed by a bounded
 * claim release on failure. No broker gate is held during a delay. A zero
 * claim means an unrelated/unattached process passes through. end runs after
 * the SDK body, including negative results. Never use in a teardown callback. */
VcoAgentStatus vco_agent_kernel_module_begin(VcoModuleCall *);
VcoAgentStatus vco_agent_kernel_module_end(const VcoModuleCall *);
void vco_agent_kernel_module_trace(unsigned operation, int32_t result, const VcoModuleCall *);
void vco_agent_kernel_file_trace(int32_t pid, int32_t result, bool fallback);
void vco_agent_kernel_loader_context(int32_t pid, uint32_t state);
void vco_agent_kernel_loader_scope(int32_t pid, unsigned step, int32_t result);
void vco_agent_kernel_binding_result(int32_t pid, VcoAgentStatus result, unsigned attempts);
void vco_agent_kernel_loader_stage(int32_t pid, unsigned step, int32_t result, uint32_t detail);
/* Internal short current-game SDK call scope. Success owns the broker gate;
 * check/end require that scope. Never call module entry or taiHEN under it. */
VcoAgentStatus vco_agent_game_call_begin(uint64_t session, bool foreground);
VcoAgentStatus vco_agent_game_call_check(uint64_t session, bool foreground);
void vco_agent_game_call_end(void);

/* Current-game syscalls. Native caller identity is never taken from a packet. */
int vcoAgentAttach(const VcoAgentBinding *, uint32_t length);
int vcoAgentPoll(const VcoAgentPollRequest *, uint32_t length);
int vcoAgentPublish(const VcoAgentResult *, uint32_t length);
int vcoAgentPermission(uint64_t session, uint32_t foreground);
/* Bounded fatal receipt; actual caller PID is recorded by the syscall, then
 * matched with its session under the broker gate. No game pointer or wait. */
int vcoAgentFault(uint64_t session, uint32_t error);
/* Copy-only diagnostic admission, at most 64 per game launch. No retry/wait,
 * execution state change or native pointer retention. */
int vcoAgentTrace(const VcoAgentTraceRequest *, uint32_t length);
#endif
