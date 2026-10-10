#include "control_rpc.h"
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
typedef char resume_size_check[sizeof(VcoControlResume) == 64 ? 1 : -1];
enum { SUBMIT, RECEIVE, ACK, RESUME };
static bool user_range(const void *address, uint32_t length)
{
    uintptr_t start = (uintptr_t)address;
    return length && start >= 0x40000000u && start <= UINT32_MAX &&
           (uint64_t)start + length <= UINT64_C(0x100000000);
}
static bool authorized(VcoVitaControlRpc *rpc, int32_t caller)
{
    return rpc && rpc->worker && rpc->owner > 0 && caller == rpc->owner && rpc->authorize &&
           rpc->authorize(rpc->context, caller) == 0;
}
static VcoMailboxStatus transfer(VcoVitaControlRpc *rpc, unsigned operation, const void *input,
                                 uint32_t input_length, uint64_t ticket, void *output,
                                 uint32_t output_length)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int32_t caller = ksceKernelGetProcessId();
    VcoMailboxStatus result = VCO_MAILBOX_OWNER;
    if (!authorized(rpc, caller))
        goto done;
    result = VCO_MAILBOX_ARGUMENT;
    if (operation == SUBMIT) {
        if (input_length != sizeof(VcoControlRequest) || !user_range(input, input_length) ||
            output_length != sizeof(uint64_t) || !user_range(output, output_length))
            goto done;
        VcoControlRequest request = {0};
        if (ksceKernelCopyFromUser(&request, input, sizeof(request)) < 0) {
            result = VCO_MAILBOX_COPY;
            goto done;
        }
        if (!authorized(rpc, caller)) {
            result = VCO_MAILBOX_OWNER;
            goto done;
        }
        uint64_t assigned = 0;
        result = vco_vita_control_worker_submit(rpc->worker, caller, &request, sizeof(request),
                                                &assigned);
        if (result == VCO_MAILBOX_OK) {
            if (!authorized(rpc, caller))
                result = VCO_MAILBOX_OWNER;
            else if (ksceKernelCopyToUser(output, &assigned, sizeof(assigned)) < 0)
                result = VCO_MAILBOX_COPY;
        }
    } else if (operation == RECEIVE) {
        if (!ticket || output_length != sizeof(VcoControlReply) ||
            !user_range(output, output_length))
            goto done;
        VcoControlReply reply = {0};
        result = vco_vita_control_worker_receive(rpc->worker, caller, ticket, &reply);
        if (result == VCO_MAILBOX_OK) {
            if (!authorized(rpc, caller))
                result = VCO_MAILBOX_OWNER;
            else if (ksceKernelCopyToUser(output, &reply, sizeof(reply)) < 0)
                result = VCO_MAILBOX_COPY;
        }
    } else if (operation == ACK) {
        if (!ticket)
            goto done;
        result = vco_vita_control_worker_ack(rpc->worker, caller, ticket);
    } else if (operation == RESUME) {
        if (output_length != sizeof(VcoControlResume) || !user_range(output, output_length))
            goto done;
        VcoControlResume resume = {0};
        result =
            vco_vita_control_worker_resume(rpc->worker, caller, &resume.request, &resume.ticket);
        if (result == VCO_MAILBOX_OK) {
            if (!authorized(rpc, caller))
                result = VCO_MAILBOX_OWNER;
            else if (ksceKernelCopyToUser(output, &resume, sizeof(resume)) < 0)
                result = VCO_MAILBOX_COPY;
        }
    }
done:
    EXIT_SYSCALL(syscall_state);
    return result;
}
VcoMailboxStatus vco_vita_rpc_submit(VcoVitaControlRpc *rpc, const void *request, uint32_t length,
                                     uint64_t *ticket)
{
    return transfer(rpc, SUBMIT, request, length, 0, ticket, sizeof(*ticket));
}
VcoMailboxStatus vco_vita_rpc_receive(VcoVitaControlRpc *rpc, uint64_t ticket, void *reply,
                                      uint32_t length)
{
    return transfer(rpc, RECEIVE, NULL, 0, ticket, reply, length);
}
VcoMailboxStatus vco_vita_rpc_ack(VcoVitaControlRpc *rpc, uint64_t ticket)
{
    return transfer(rpc, ACK, NULL, 0, ticket, NULL, 0);
}
VcoMailboxStatus vco_vita_rpc_resume(VcoVitaControlRpc *rpc, VcoControlResume *resume,
                                     uint32_t length)
{
    return transfer(rpc, RESUME, NULL, 0, 0, resume, length);
}
