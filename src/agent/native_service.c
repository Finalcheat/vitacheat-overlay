#include "native_service.h"
#include "broker.h"
#include "../platform/vita/service.h"
#include "../platform/vita/shell_owner.h"
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/proc_event.h>
#include <taihen.h>
#include <string.h>

/* These objects have kernel lifetime. No executor, game reference, patch
 * handle, range lock, worker thread or file I/O lives in this service. */
enum { NEW, STARTING, ACTIVE, FAILED };
enum { TRACE_START = 160, TRACE_GAME, TRACE_LOAD, TRACE_OWNER, TRACE_OBSERVE };
enum {
    TRACE_MODULE_BEGIN = 165,
    TRACE_MODULE_ACK,
    TRACE_MODULE_END,
    TRACE_STOPPED,
    TRACE_MODULE_CALL = 176
};
#define TERMINAL UINT32_C(0x80000000)
#define LEASE_US UINT64_C(250000)
static uint32_t phase, gate, watched, event_epoch, event_lost;
static int startup_error;
static VcoAgentBroker broker;
static VcoVitaShellOwner shell_owner;
static uint8_t upload[VCO_UPLOAD_MAX], incoming[VCO_UPLOAD_MAX];
static VcoTraceRing trace;
static uint64_t received_us, last_clock, shell_list_session;
static VcoUploadHeader last_processed;
static uint32_t received_epoch;
static bool observation_live, clock_seen;
static bool owner_reset_required = true, owner_clearing;
static uint32_t load_failure_state;
static uint32_t game_failure_state;
static struct {
    uint64_t session;
    int32_t pid;
    uint32_t error;
} game_failure;
static struct {
    uint64_t session;
    int32_t pid, module, result;
} load_failure;
static int (*get_title)(int32_t, char *, uint32_t);
static VcoAgentStatus game_fault_record(int32_t caller, uint64_t session, uint32_t error);
extern int vco_kernel_input_ready(void);
extern void vco_kernel_capture_reset(void);
int module_get_export_func(SceUID, const char *, uint32_t, uint32_t, uintptr_t *);

static bool take_gate(void)
{
    uint32_t empty = 0;
    return __atomic_compare_exchange_n(&gate, &empty, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void leave_gate(void)
{
    __atomic_store_n(&gate, 0, __ATOMIC_RELEASE);
}
static void invalidate(void)
{
    uint32_t value = __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < 8; ++i) {
        if (value == UINT32_MAX)
            break;
        if (__atomic_compare_exchange_n(&event_epoch, &value, value + 1, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return;
    }
    __atomic_store_n(&event_lost, 1, __ATOMIC_RELEASE);
}
void vco_agent_kernel_terminal(int32_t pid)
{
    if (pid > 0) {
        uint32_t expected = (uint32_t)pid;
        (void)__atomic_compare_exchange_n(&watched, &expected, (uint32_t)pid | TERMINAL, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    invalidate();
}
static int terminal(SceUID pid, SceProcEventInvokeParam1 *unused, int argument)
{
    (void)unused;
    (void)argument;
    vco_agent_kernel_terminal(pid);
    return 0;
}
static int transition(SceUID pid, int type, SceProcEventInvokeParam1 *unused, int argument)
{
    (void)pid;
    (void)type;
    (void)unused;
    (void)argument;
    invalidate();
    return 0;
}
static int switched(int event, int type, SceProcEventInvokeParam2 *unused, int argument)
{
    (void)event;
    (void)type;
    (void)unused;
    (void)argument;
    invalidate();
    return 0;
}
static const SceProcEventHandler handler = {.size = sizeof(handler),
                                            .exit = terminal,
                                            .kill = terminal,
                                            .stop = transition,
                                            .start = transition,
                                            .switch_process = switched};
static bool clock_now(uint64_t *out)
{
    int64_t value = ksceKernelGetSystemTimeWide();
    if (value < 0 || (clock_seen && (uint64_t)value < last_clock)) {
        observation_live = false;
        return false;
    }
    clock_seen = true;
    *out = last_clock = (uint64_t)value;
    return true;
}
static void log_event(unsigned event, int result, uint32_t subject, uint32_t detail)
{
    int64_t now = ksceKernelGetSystemTimeWide();
    (void)vco_trace_append(&trace, now < 0 ? 0 : (uint64_t)now, event, result, subject, detail);
}
static unsigned game_trace_remaining;
int vco_agent_kernel_start(void)
{
    uint32_t expected = NEW;
    if (!__atomic_compare_exchange_n(&phase, &expected, STARTING, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return expected == ACTIVE ? 0 : expected == FAILED ? startup_error : -1;
    vco_vita_shell_owner_init(&shell_owner);
    int error = vco_broker_init(&broker, INT32_MAX, upload, sizeof(upload));
    uintptr_t address = 0;
    if (!error)
        error = module_get_export_func(KERNEL_PID, "SceSysmem", 0x3691DA45, 0xEC3124A3, &address);
    if (!error && !address)
        error = -1;
    if (!error) {
        get_title = (int (*)(int32_t, char *, uint32_t))address;
        error = ksceKernelRegisterProcEventHandler("VcoGameSessions", &handler, 0);
        if (error >= 0)
            error = 0;
    }
    startup_error = error;
    log_event(TRACE_START, error, 0, 0);
    __atomic_store_n(&phase, error ? FAILED : ACTIVE, __ATOMIC_RELEASE);
    /* Handler and all storage remain resident, including after failure. */
    return error;
}
int vco_agent_kernel_title(int32_t pid, char title[32])
{
    if (!title || pid <= 0 || __atomic_load_n(&phase, __ATOMIC_ACQUIRE) != ACTIVE || !get_title)
        return -1;
    memset(title, 0xff, 32);
    return get_title(pid, title, 32);
}
void vco_agent_kernel_loader_failed(int error)
{
    if (error < 0) {
        startup_error = error;
        __atomic_store_n(&phase, FAILED, __ATOMIC_RELEASE);
        log_event(TRACE_START, error, 1, 0);
    }
}
static void synchronize(void)
{
    if (__atomic_load_n(&load_failure_state, __ATOMIC_ACQUIRE) == 2) {
        if (load_failure.session == broker.binding.session &&
            load_failure.pid == broker.binding.pid) {
            broker.attached = broker.runnable = observation_live = false;
            broker.status.state = VCO_SERVICE_ERROR;
            broker.status.error = VCO_E_IO;
        }
        __atomic_store_n(&load_failure_state, 0, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&game_failure_state, __ATOMIC_ACQUIRE) == 2) {
        if (game_failure.session == broker.binding.session &&
            game_failure.pid == broker.binding.pid) {
            broker.attached = broker.runnable = observation_live = false;
            broker.status.state = VCO_SERVICE_ERROR;
            broker.status.error = game_failure.error;
        }
        __atomic_store_n(&game_failure_state, 0, __ATOMIC_RELEASE);
    }
    if (broker.binding.session &&
        __atomic_load_n(&watched, __ATOMIC_ACQUIRE) != (uint32_t)broker.binding.pid) {
        (void)vco_broker_end(&broker, broker.binding.session);
        observation_live = false;
    }
}
static bool fresh(void)
{
    uint64_t now;
    if (vco_broker_module_busy(&broker))
        return false;
    if (owner_clearing || !observation_live || !clock_now(&now) || now < received_us ||
        now - received_us >= LEASE_US || __atomic_load_n(&event_lost, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE) != received_epoch || !shell_owner.bound ||
        vco_vita_shell_owner_authorize(&shell_owner, shell_owner.pid)) {
        observation_live = false;
        return false;
    }
    return true;
}
static int owner(int32_t caller)
{
    if (!shell_owner.bound) {
        if (vco_vita_shell_owner_bind(&shell_owner))
            return VCO_MAILBOX_OWNER;
    } else if (vco_vita_shell_owner_authorize(&shell_owner, caller)) {
        if (vco_vita_shell_owner_replacement(&shell_owner) != 1)
            return VCO_MAILBOX_OWNER;
        owner_reset_required = true;
        observation_live = false;
        if (vco_vita_shell_owner_unbind(&shell_owner) || vco_vita_shell_owner_bind(&shell_owner))
            return VCO_MAILBOX_OWNER;
        vco_kernel_capture_reset();
    }
    if (vco_vita_shell_owner_authorize(&shell_owner, caller))
        return VCO_MAILBOX_OWNER;
    if (owner_reset_required || broker.owner != shell_owner.pid) {
        if (vco_broker_rebind_owner(&broker, shell_owner.pid) != VCO_AGENT_OK)
            return VCO_MAILBOX_EXHAUSTED;
        owner_clearing = broker.attached;
        owner_reset_required = false;
        observation_live = false;
        log_event(TRACE_OWNER, 0, (uint32_t)shell_owner.pid, owner_clearing);
    }
    return VCO_MAILBOX_OK;
}
int vco_service_call_begin(int32_t caller)
{
    if (!vco_kernel_input_ready() || __atomic_load_n(&phase, __ATOMIC_ACQUIRE) == NEW ||
        __atomic_load_n(&phase, __ATOMIC_ACQUIRE) == STARTING)
        return VCO_MAILBOX_CLOSED;
    if (!take_gate())
        return VCO_MAILBOX_BUSY;
    int error = owner(caller);
    if (error)
        leave_gate();
    else
        synchronize();
    return error;
}
void vco_service_call_end(void)
{
    leave_gate();
}
int vco_service_authorize_caller(int32_t caller)
{
    if (vco_service_call_begin(caller))
        return -1;
    vco_service_call_end();
    return 0;
}
static bool range(const void *pointer, size_t size)
{
    uintptr_t address = (uintptr_t)pointer;
    return size && address >= 0x40000000u && address <= UINT32_MAX &&
           (uint64_t)address + size <= UINT64_C(0x100000000);
}
static int copy_in(void *dst, const void *src, size_t actual, size_t expected)
{
    if (actual != expected || !range(src, actual))
        return VCO_MAILBOX_ARGUMENT;
    return ksceKernelCopyFromUser(dst, src, actual) < 0 ? VCO_MAILBOX_COPY : VCO_MAILBOX_OK;
}
static int copy_out(void *dst, const void *src, size_t actual, size_t expected)
{
    if (actual != expected || !range(dst, actual))
        return VCO_MAILBOX_ARGUMENT;
    return ksceKernelCopyToUser(dst, src, actual) < 0 ? VCO_MAILBOX_COPY : VCO_MAILBOX_OK;
}
static int mailbox(VcoAgentStatus status)
{
    const int values[] = {VCO_MAILBOX_OK,        VCO_MAILBOX_ARGUMENT, VCO_MAILBOX_OWNER,
                          VCO_MAILBOX_TICKET,    VCO_MAILBOX_BUSY,     VCO_MAILBOX_EMPTY,
                          VCO_MAILBOX_EXHAUSTED, VCO_MAILBOX_COPY,     VCO_MAILBOX_CLOSED};
    return (unsigned)status < sizeof(values) / sizeof(values[0]) ? values[status]
                                                                 : VCO_MAILBOX_ARGUMENT;
}
static VcoAgentStatus game(int32_t caller, uint64_t session, bool attached)
{
    if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != ACTIVE)
        return VCO_AGENT_CLOSED;
    synchronize();
    if (!session || session != broker.binding.session)
        return VCO_AGENT_STALE;
    if (caller != broker.binding.pid)
        return VCO_AGENT_OWNER;
    if (attached && !broker.attached)
        return VCO_AGENT_ARGUMENT;
    char title[32];
    if (vco_agent_kernel_title(caller, title) < 0 || memcmp(title, broker.binding.title_id, 10))
        return VCO_AGENT_STALE;
    if (__atomic_load_n(&watched, __ATOMIC_ACQUIRE) != (uint32_t)caller)
        return VCO_AGENT_STALE;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_agent_game_call_check(uint64_t session, bool foreground)
{
    VcoAgentStatus error = game(ksceKernelGetProcessId(), session, true);
    if (!error && foreground && !fresh())
        error = VCO_AGENT_BUSY;
    return error;
}
VcoAgentStatus vco_agent_game_call_begin(uint64_t session, bool foreground)
{
    if (!take_gate())
        return VCO_AGENT_BUSY;
    VcoAgentStatus error = vco_agent_game_call_check(session, foreground);
    if (error)
        leave_gate();
    return error;
}
void vco_agent_game_call_end(void)
{
    leave_gate();
}
static VcoAgentStatus module_end(const VcoModuleCall *call)
{
    if (!call || !call->claim)
        return call ? VCO_AGENT_OK : VCO_AGENT_ARGUMENT;
    /* Each iteration has a bounded gate attempt. Never sleep or call an SDK
     * module entry with the gate held. Process invalidation cancels all claims. */
    for (unsigned i = 0; i < 500; ++i) {
        if (take_gate()) {
            synchronize();
            VcoAgentStatus error = vco_broker_module_end(&broker, call);
            log_event(TRACE_MODULE_END, error, (uint32_t)call->claim, (uint32_t)call->ticket);
            observation_live = false; /* Require a fresh post-module sample. */
            leave_gate();
            return error;
        }
        if (ksceKernelDelayThread(1000) < 0)
            break;
    }
    /* A wedged copied-call gate cannot be treated as a successful release.
     * The resident state remains paused; no module caller owns target pointers. */
    (void)game_fault_record(ksceKernelGetProcessId(), call->session, VCO_E_IO);
    return VCO_AGENT_BUSY;
}
VcoAgentStatus vco_agent_kernel_module_begin(VcoModuleCall *out)
{
    if (!out)
        return VCO_AGENT_ARGUMENT;
    *out = (VcoModuleCall){0};
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    VcoAgentStatus error = VCO_AGENT_BUSY;
    bool claimed = false;
    int64_t started = ksceKernelGetSystemTimeWide();
    for (unsigned i = 0; i < 500; ++i) {
        /* EXIT/KILL remains a lock-free revocation. Teardown calls, unrelated
         * processes and passive partially installed hooks do not wait here. */
        if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != ACTIVE ||
            __atomic_load_n(&watched, __ATOMIC_ACQUIRE) != (uint32_t)caller) {
            error = claimed ? VCO_AGENT_STALE : VCO_AGENT_OK;
            break;
        }
        if (take_gate()) {
            synchronize();
            if (!broker.attached)
                /* A loader IO fault before any catalog cannot leave execution
                 * patches. Retain the UI error, but permit the game's own
                 * module calls. Runtime/catalog and restore faults stay shut. */
                error = !broker.quiescent && broker.status.state == VCO_SERVICE_ERROR &&
                                (broker.status.revision || broker.status.error != VCO_E_IO)
                            ? VCO_AGENT_CLOSED
                            : VCO_AGENT_OK;
            else if (!claimed) {
                error = game(caller, broker.binding.session, true);
                if (!error) {
                    error = vco_broker_module_begin(&broker, caller, out);
                    claimed = !error;
                    if (claimed)
                        log_event(TRACE_MODULE_BEGIN, 0, (uint32_t)caller, (uint32_t)out->claim);
                }
            }
            if (claimed) {
                VcoError cleanup;
                error = vco_broker_module_ready(&broker, out, &cleanup);
                if (!error && cleanup)
                    error = VCO_AGENT_IO;
            }
            leave_gate();
            if (error != VCO_AGENT_BUSY && error != VCO_AGENT_EMPTY)
                break;
        }
        int64_t now = ksceKernelGetSystemTimeWide();
        if (started < 0 || now < started || now - started >= 500000 ||
            ksceKernelDelayThread(1000) < 0) {
            error = VCO_AGENT_BUSY;
            break;
        }
    }
    if (error == VCO_AGENT_EMPTY)
        error = VCO_AGENT_BUSY;
    if (error && claimed) {
        (void)module_end(out);
        *out = (VcoModuleCall){0};
    }
    EXIT_SYSCALL(state);
    return error;
}
VcoAgentStatus vco_agent_kernel_module_end(const VcoModuleCall *call)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentStatus error = module_end(call);
    EXIT_SYSCALL(state);
    return error;
}
void vco_agent_kernel_module_trace(unsigned operation, int32_t result, const VcoModuleCall *call)
{
    if (!call || operation >= 8)
        return;
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    if (caller > 0 && __atomic_load_n(&phase, __ATOMIC_ACQUIRE) == ACTIVE &&
        __atomic_load_n(&watched, __ATOMIC_ACQUIRE) == (uint32_t)caller)
        log_event(TRACE_MODULE_CALL + operation, result, (uint32_t)caller, (uint32_t)call->claim);
    EXIT_SYSCALL(state);
}
void vco_agent_kernel_file_trace(int32_t pid, int32_t result, bool fallback)
{
    log_event(fallback ? 170u : 169u, result, (uint32_t)pid, 0);
}
void vco_agent_kernel_loader_context(int32_t pid, uint32_t state)
{
    /* Keep only the context indicator, never a TLS or kernel-object address. */
    log_event(171u, (state & 0xffffu) != 0, (uint32_t)pid, 0);
}
void vco_agent_kernel_loader_scope(int32_t pid, unsigned step, int32_t result)
{
    if (step < 4)
        log_event(172u + step, result, (uint32_t)pid, 0);
}
void vco_agent_kernel_binding_result(int32_t pid, VcoAgentStatus result, unsigned attempts)
{
    log_event(0xdau, (int)result, (uint32_t)pid, attempts);
}
void vco_agent_kernel_loader_stage(int32_t pid, unsigned step, int32_t result, uint32_t detail)
{
    if (step < 3)
        log_event(208u + step, result, (uint32_t)pid, detail);
}
VcoAgentStatus vco_agent_kernel_begin(int32_t pid, const char title[10], VcoAgentBinding *out)
{
    if (!title || !out || pid <= 0 || pid == KERNEL_PID)
        return VCO_AGENT_ARGUMENT;
    if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != ACTIVE)
        return VCO_AGENT_CLOSED;
    if (!take_gate())
        return VCO_AGENT_BUSY;
    uint32_t epoch = __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE);
    char actual[32];
    VcoAgentStatus error = vco_agent_kernel_title(pid, actual) < 0 || memcmp(actual, title, 10)
                               ? VCO_AGENT_ARGUMENT
                               : vco_broker_open(&broker, pid, title, out);
    if (!error) {
        observation_live = false;
        owner_clearing = false;
        game_trace_remaining = 64u;
        shell_list_session = 0;
        memset(&last_processed, 0, sizeof(last_processed));
        __atomic_store_n(&watched, (uint32_t)pid, __ATOMIC_RELEASE);
        if (__atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE) != epoch) {
            __atomic_store_n(&watched, (uint32_t)pid | TERMINAL, __ATOMIC_RELEASE);
            synchronize();
            error = VCO_AGENT_STALE;
        }
        log_event(TRACE_GAME, error, (uint32_t)pid, (uint32_t)out->session);
    }
    leave_gate();
    return error;
}
void vco_agent_kernel_load_result(const VcoAgentBinding *binding, int32_t module, int32_t result)
{
    if (!binding)
        return;
    if (module < 0 || result != 0) {
        uint32_t empty = 0;
        if (__atomic_compare_exchange_n(&load_failure_state, &empty, 1, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            load_failure.session = binding->session;
            load_failure.pid = binding->pid;
            load_failure.module = module;
            load_failure.result = result;
            __atomic_store_n(&load_failure_state, 2, __ATOMIC_RELEASE);
        } else {
            /* No waiting while the native loader holds its own locks. Failure
             * overload conservatively revokes, never leaves a writable target. */
            vco_agent_kernel_terminal(binding->pid);
        }
    }
    log_event(TRACE_LOAD, module, (uint32_t)binding->pid, (uint32_t)result);
}
int vcoAgentAttach(const VcoAgentBinding *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentStatus error = VCO_AGENT_BUSY;
    if (take_gate()) {
        VcoAgentBinding binding;
        int copied = copy_in(&binding, input, length, sizeof(binding));
        error = copied ? copied == VCO_MAILBOX_COPY ? VCO_AGENT_COPY : VCO_AGENT_ARGUMENT
                       : game(ksceKernelGetProcessId(), binding.session, false);
        if (!error)
            error = vco_broker_attach(&broker, ksceKernelGetProcessId(), &binding);
        leave_gate();
    }
    EXIT_SYSCALL(state);
    return error;
}
static VcoAgentStatus game_fault_record(int32_t caller, uint64_t session, uint32_t error)
{
    VcoAgentStatus result = VCO_AGENT_ARGUMENT;
    if (session && error > VCO_OK && error <= VCO_E_METADATA) {
        if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != ACTIVE)
            result = VCO_AGENT_CLOSED;
        else if (caller <= 0 || __atomic_load_n(&watched, __ATOMIC_ACQUIRE) != (uint32_t)caller)
            result = VCO_AGENT_OWNER;
        else {
            uint32_t empty = 0;
            result = VCO_AGENT_BUSY;
            if (__atomic_compare_exchange_n(&game_failure_state, &empty, 1, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                game_failure.session = session;
                game_failure.pid = caller;
                game_failure.error = error;
                __atomic_store_n(&game_failure_state, 2, __ATOMIC_RELEASE);
                result = VCO_AGENT_OK;
            }
        }
    }
    return result;
}
int vcoAgentFault(uint64_t session, uint32_t error)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentStatus result = game_fault_record(ksceKernelGetProcessId(), session, error);
    EXIT_SYSCALL(state);
    return result;
}
int vcoAgentTrace(const VcoAgentTraceRequest *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentTraceRequest request;
    int copied = copy_in(&request, input, length, sizeof(request));
    VcoAgentStatus error =
        copied ? copied == VCO_MAILBOX_COPY ? VCO_AGENT_COPY : VCO_AGENT_ARGUMENT : VCO_AGENT_OK;
    if (!error && (request.abi != VCO_AGENT_ABI || request.size != sizeof(request) ||
                   request.event < VCO_GAME_TRACE_INJECT || request.event > VCO_GAME_TRACE_QUERY))
        error = VCO_AGENT_ARGUMENT;
    if (!error && !take_gate())
        error = VCO_AGENT_BUSY;
    else if (!error) {
        error = game(ksceKernelGetProcessId(), request.session, true);
        if (!error && !game_trace_remaining)
            error = VCO_AGENT_LIMIT;
        if (!error) {
            --game_trace_remaining;
            log_event(request.event, request.result, request.subject, request.detail);
        }
        leave_gate();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoAgentPoll(const VcoAgentPollRequest *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    VcoAgentPollRequest request = {0};
    int copied = copy_in(&request, input, length, sizeof(request));
    VcoAgentStatus error =
        copied ? copied == VCO_MAILBOX_COPY ? VCO_AGENT_COPY : VCO_AGENT_ARGUMENT : VCO_AGENT_OK;
    if (!error && (request.abi != VCO_AGENT_ABI || request.reserved ||
                   request.message > UINT32_MAX || request.payload > UINT32_MAX))
        error = VCO_AGENT_ARGUMENT;
    if (!error && !take_gate())
        error = VCO_AGENT_BUSY;
    else if (!error) {
        VcoAgentMessage *out = (VcoAgentMessage *)(uintptr_t)request.message;
        void *payload = (void *)(uintptr_t)request.payload;
        error = game(caller, request.session, true);
        if (!error &&
            (request.message_length != sizeof(*out) || !range(out, request.message_length)))
            error = VCO_AGENT_ARGUMENT;
        if (!error) {
            (void)vco_broker_observe(&broker, request.session, fresh());
            VcoAgentMessage message;
            error = vco_broker_poll(&broker, caller, request.session, &message, incoming,
                                    sizeof(incoming));
            if (!error && message.length &&
                (request.capacity < message.length || !range(payload, message.length)))
                error = VCO_AGENT_LIMIT;
            if (!error && message.length &&
                ksceKernelCopyToUser(payload, incoming, message.length) < 0)
                error = VCO_AGENT_COPY;
            if (!error && ksceKernelCopyToUser(out, &message, sizeof(message)) < 0)
                error = VCO_AGENT_COPY;
        }
        leave_gate();
    }
    if (error != VCO_AGENT_OK && error != VCO_AGENT_BUSY && error != VCO_AGENT_EMPTY &&
        error != VCO_AGENT_COPY)
        log_event(211u, error, (uint32_t)caller, request.capacity);
    EXIT_SYSCALL(state);
    return error;
}
int vcoAgentPublish(const VcoAgentResult *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentStatus error = VCO_AGENT_BUSY;
    if (take_gate()) {
        VcoAgentResult result;
        int copied = copy_in(&result, input, length, sizeof(result));
        int32_t caller = ksceKernelGetProcessId();
        error = copied ? copied == VCO_MAILBOX_COPY ? VCO_AGENT_COPY : VCO_AGENT_ARGUMENT
                       : game(caller, result.session, result.operation != VCO_AGENT_STOPPED);
        if (!error) {
            if (result.operation == VCO_AGENT_STOPPED) {
                error = vco_broker_publish(&broker, caller, &result);
                if (!error) {
                    log_event(TRACE_STOPPED, 0, (uint32_t)caller, (uint32_t)result.session);
                    owner_clearing = false;
                    observation_live = false;
                }
                leave_gate();
                EXIT_SYSCALL(state);
                return error;
            }
            /* Shell replacement may cancel a ticket while the game was outside
             * this gate. It must poll the replacement CLEAR, not revoke its PID. */
            if (result.ticket != broker.message.ticket)
                error = VCO_AGENT_BUSY;
            else {
                if (result.operation == VCO_AGENT_LOAD) {
                    VcoUploadHeader header;
                    memcpy(&header, broker.upload, sizeof(header));
                    result.status.load_request = header.request;
                }
                error = vco_broker_publish(&broker, caller, &result);
                if (!error && result.operation == VCO_AGENT_MODULE_PAUSE)
                    log_event(TRACE_MODULE_ACK, (int)result.error, (uint32_t)caller,
                              (uint32_t)result.ticket);
                if (!error && result.operation != VCO_AGENT_CONTROL) {
                    if (result.operation == VCO_AGENT_LOAD)
                        memcpy(&last_processed, broker.upload, sizeof(last_processed));
                    if (result.operation == VCO_AGENT_CLEAR && !result.error) {
                        owner_clearing = false;
                        observation_live = false;
                    }
                    (void)vco_broker_ack(&broker, broker.owner, result.ticket);
                }
            }
        }
        leave_gate();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoAgentPermission(uint64_t session, uint32_t foreground)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentStatus error = VCO_AGENT_BUSY;
    if (take_gate()) {
        error = foreground > 1 ? VCO_AGENT_ARGUMENT : game(ksceKernelGetProcessId(), session, true);
        if (!error && foreground && !fresh())
            error = VCO_AGENT_BUSY;
        leave_gate();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceObserve(const VcoForegroundSample *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int error = vco_service_call_begin(ksceKernelGetProcessId());
    if (!error) {
        observation_live = false;
        VcoForegroundSample sample;
        error = copy_in(&sample, input, length, sizeof(sample));
        uint64_t now;
        uint32_t epoch = __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE);
        if (!error && (!clock_now(&now) || __atomic_load_n(&event_lost, __ATOMIC_ACQUIRE)))
            error = VCO_MAILBOX_CLOSED;
        if (!error) {
            const uint8_t zero[sizeof(sample.reserved)] = {0};
            if (sample.abi != VCO_FOREGROUND_PROBE_ABI || sample.size != sizeof(sample) ||
                sample.status > VCO_BACKGROUND_CANDIDATE ||
                sample.failure > VCO_FOREGROUND_FAILURE_CHANGED ||
                memcmp(sample.reserved, zero, sizeof(zero)))
                error = VCO_MAILBOX_ARGUMENT;
            else if (sample.status == VCO_FOREGROUND_CANDIDATE) {
                char title[32];
                if (sample.failure || sample.game_pid != broker.binding.pid ||
                    sample.active_pid != sample.game_pid || sample.game_app_id <= 0 ||
                    sample.active_app_id != sample.game_app_id ||
                    memcmp(sample.game_title_id, broker.binding.title_id, 10) ||
                    vco_agent_kernel_title(sample.game_pid, title) < 0 ||
                    memcmp(title, broker.binding.title_id, 10))
                    error = VCO_MAILBOX_ARGUMENT;
                else {
                    received_us = now;
                    received_epoch = epoch;
                    observation_live = __atomic_load_n(&event_epoch, __ATOMIC_ACQUIRE) == epoch;
                }
            }
        }
        if (broker.binding.session)
            (void)vco_broker_observe(&broker, broker.binding.session, observation_live);
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceLoad(const void *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    int error = vco_service_call_begin(caller);
    if (!error) {
        uint64_t ticket;
        if (!input && !length) {
            error = !shell_list_session
                        ? VCO_MAILBOX_OK
                        : mailbox(vco_broker_clear(&broker, caller, shell_list_session, &ticket));
        } else if (length < sizeof(VcoUploadHeader) || length > sizeof(incoming))
            error = VCO_MAILBOX_ARGUMENT;
        else if ((error = copy_in(incoming, input, length, length)) == VCO_MAILBOX_OK) {
            VcoUploadHeader header;
            memcpy(&header, incoming, sizeof(header));
            if (header.target_session != broker.binding.session ||
                header.pid != (uint32_t)broker.binding.pid ||
                memcmp(header.title_id, broker.binding.title_id, 10))
                error = VCO_MAILBOX_TICKET;
            else if (header.request < last_processed.request ||
                     (header.request == last_processed.request &&
                      memcmp(&header, &last_processed, sizeof(header))))
                error = VCO_MAILBOX_ARGUMENT;
            else if (header.request == last_processed.request)
                error = VCO_MAILBOX_OK;
            else
                error = mailbox(vco_broker_load(&broker, caller, incoming, length, &ticket));
            if (!error)
                shell_list_session = broker.binding.session;
        }
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceStatus(VcoServiceStatus *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int error = vco_service_call_begin(ksceKernelGetProcessId());
    if (!error) {
        VcoServiceStatus status;
        if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) == FAILED)
            status = (VcoServiceStatus){.abi = VCO_SERVICE_ABI,
                                        .size = sizeof(status),
                                        .state = VCO_SERVICE_ERROR,
                                        .error = VCO_E_IO};
        else {
            if (broker.binding.session)
                (void)vco_broker_observe(&broker, broker.binding.session, fresh());
            error = mailbox(vco_broker_status(&broker, broker.owner, &status));
            if (!error && owner_clearing && status.state != VCO_SERVICE_ERROR)
                status.state = VCO_SERVICE_LOADING;
        }
        if (!error)
            error = copy_out(out, &status, length, sizeof(status));
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceSubmit(const VcoControlRequest *input, uint32_t length, uint64_t *out)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    int error = vco_service_call_begin(caller);
    if (!error) {
        VcoControlRequest request;
        uint64_t ticket;
        error = !range(out, sizeof(ticket)) ? VCO_MAILBOX_ARGUMENT
                                            : copy_in(&request, input, length, sizeof(request));
        if (!error)
            error = mailbox(vco_broker_submit(&broker, caller, &request, &ticket));
        if (!error)
            error = copy_out(out, &ticket, sizeof(ticket), sizeof(ticket));
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceReceive(uint64_t ticket, VcoControlReply *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int error = vco_service_call_begin(ksceKernelGetProcessId());
    if (!error) {
        VcoAgentResult result;
        error = mailbox(vco_broker_receive(&broker, broker.owner, ticket, &result));
        if (!error && result.operation != VCO_AGENT_CONTROL)
            error = VCO_MAILBOX_TICKET;
        if (!error)
            error = copy_out(out, &result.control, length, sizeof(result.control));
        if (error == VCO_MAILBOX_EMPTY)
            error = VCO_MAILBOX_WAIT;
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceAck(uint64_t ticket)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int error = vco_service_call_begin(ksceKernelGetProcessId());
    if (!error) {
        error = mailbox(vco_broker_ack(&broker, broker.owner, ticket));
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceResume(void *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int error = vco_service_call_begin(ksceKernelGetProcessId());
    if (!error) {
        struct {
            uint64_t ticket;
            VcoControlRequest request;
        } packet;
        error = mailbox(vco_broker_resume(&broker, broker.owner, &packet.request, &packet.ticket));
        if (!error)
            error = copy_out(out, &packet, length, sizeof(packet));
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoServiceTrace(uint64_t after, VcoTraceBatch *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int error = vco_service_call_begin(ksceKernelGetProcessId());
    if (!error) {
        VcoTraceBatch batch;
        error = vco_trace_snapshot(&trace, after, &batch) == VCO_TRACE_OK ? VCO_MAILBOX_OK
                                                                          : VCO_MAILBOX_ARGUMENT;
        if (!error)
            error = copy_out(out, &batch, length, sizeof(batch));
        vco_service_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
