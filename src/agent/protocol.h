#ifndef VCO_AGENT_PROTOCOL_H
#define VCO_AGENT_PROTOCOL_H
#include "../execution/service_protocol.h"

#define VCO_AGENT_ABI 0x00010004u
enum {
    VCO_AGENT_IDLE,
    VCO_AGENT_LOAD,
    VCO_AGENT_CLEAR,
    VCO_AGENT_CONTROL,
    VCO_AGENT_MODULE_PAUSE,
    VCO_AGENT_STOPPED
}; /* STOPPED is a game-to-kernel receipt, never a polled command. */
/* A kernel loader issues this binding for one game launch. It is copied into
 * StartModule arguments. A reused PID never inherits the preceding token. */
typedef struct {
    uint64_t session;
    uint32_t abi, size;
    int32_t pid;
    uint32_t reserved;
    char title_id[10];
    uint8_t padding[6];
} VcoAgentBinding;
typedef struct {
    uint64_t session, ticket;
    uint32_t abi, size, operation, length, runnable, reserved;
} VcoAgentMessage;
typedef struct {
    uint64_t session, ticket;
    uint32_t abi, size, operation, error;
    VcoServiceStatus status;
    VcoControlReply control;
} VcoAgentResult;
typedef enum {
    VCO_AGENT_OK,
    VCO_AGENT_ARGUMENT,
    VCO_AGENT_OWNER,
    VCO_AGENT_STALE,
    VCO_AGENT_BUSY,
    VCO_AGENT_EMPTY,
    VCO_AGENT_LIMIT,
    VCO_AGENT_COPY,
    VCO_AGENT_CLOSED,
    VCO_AGENT_IO
} VcoAgentStatus;
/* Raw syscalls must not depend on C arguments on the user stack. Pass a
 * request pointer and its exact size; the kernel copies it before use.
 * Buffer addresses have a fixed 64-bit wire representation for host/device
 * layout agreement. The Vita kernel rejects values above UINT32_MAX before
 * converting them to pointers. No user pointer is retained after the call. */
typedef struct {
    uint64_t session;
    uint32_t abi, reserved;
    uint64_t message, payload;
    uint32_t message_length, capacity;
} VcoAgentPollRequest;
typedef struct {
    uint64_t session;
    uint32_t abi, address;
    uint64_t buffer;
    uint32_t width, reserved;
} VcoAgentMemoryRequest;
typedef struct {
    uint64_t session;
    uint32_t abi;
    int32_t uid;
    uint64_t output;
    uint32_t length, reserved;
} VcoAgentModuleRequest;
typedef char poll_request_size_check[sizeof(VcoAgentPollRequest) == 40 ? 1 : -1];
typedef char memory_request_size_check[sizeof(VcoAgentMemoryRequest) == 32 ? 1 : -1];
typedef char module_request_size_check[sizeof(VcoAgentModuleRequest) == 32 ? 1 : -1];
/* Best-effort game diagnostics, never a command or an execution receipt. The
 * kernel admits only this event range from the attached current game. */
enum {
    VCO_GAME_TRACE_INJECT = 0xd4,
    VCO_GAME_TRACE_GUARD,
    VCO_GAME_TRACE_PERMISSION,
    VCO_GAME_TRACE_RELEASE_GUARD,
    VCO_GAME_TRACE_RELEASE,
    VCO_GAME_TRACE_QUERY
};
typedef struct {
    uint64_t session;
    uint32_t abi, size, event;
    int32_t result;
    uint32_t subject, detail;
} VcoAgentTraceRequest;
typedef char trace_request_size_check[sizeof(VcoAgentTraceRequest) == 32 ? 1 : -1];
#endif
