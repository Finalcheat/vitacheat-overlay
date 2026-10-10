#include <assert.h>
#include <string.h>
#include "execution/transport.h"
#include "platform/vita/control_rpc.h"
#define USER_BASE 0x81000000u
#define USER_REQUEST ((void *)(uintptr_t)USER_BASE)
#define USER_REPLY ((void *)(uintptr_t)(USER_BASE + 128))
#define USER_TICKET ((uint64_t *)(uintptr_t)(USER_BASE + 1024))
static unsigned char user[2048];
static int32_t caller;
static unsigned entered, exited, depth, from_calls, to_calls, auth_calls, worker_calls;
static unsigned fail_authorize;
static bool fail_from, fail_to;
static VcoControlMailbox mailbox;
static VcoVitaControlWorker worker;
static VcoVitaControlRpc rpc;
uint32_t fake_enter_syscall(void)
{
    assert(!depth);
    ++depth;
    ++entered;
    return 0x1234;
}
void fake_exit_syscall(uint32_t state)
{
    assert(depth == 1 && state == 0x1234);
    --depth;
    ++exited;
}
int32_t ksceKernelGetProcessId(void)
{
    assert(depth == 1);
    return caller;
}
static unsigned char *translate(const void *pointer, size_t size)
{
    uintptr_t address = (uintptr_t)pointer;
    assert(depth == 1 && address >= USER_BASE && address + size <= USER_BASE + sizeof(user));
    return user + (address - USER_BASE);
}
int ksceKernelCopyFromUser(void *out, const void *in, size_t size)
{
    ++from_calls;
    if (fail_from) {
        memcpy(out, translate(in, size), size / 2);
        return -1;
    }
    memcpy(out, translate(in, size), size);
    return 0;
}
int ksceKernelCopyToUser(void *out, const void *in, size_t size)
{
    ++to_calls;
    if (fail_to) {
        memcpy(translate(out, size), in, size / 2);
        return -1;
    }
    memcpy(translate(out, size), in, size);
    return 0;
}
static int authorize(void *context, int32_t pid)
{
    assert(context == &rpc && pid == 88 && depth == 1);
    return ++auth_calls == fail_authorize ? -1 : 0;
}
/* Scheduling already has independent native-worker tests. Keep the real
 * mailbox/protocol here and replace only the thread wrapper boundary. */
VcoMailboxStatus vco_vita_control_worker_submit(VcoVitaControlWorker *w, int32_t pid,
                                                const void *packet, size_t size, uint64_t *ticket)
{
    assert(w == &worker && depth);
    ++worker_calls;
    return vco_mailbox_submit(&mailbox, pid, packet, size, ticket);
}
VcoMailboxStatus vco_vita_control_worker_receive(VcoVitaControlWorker *w, int32_t pid,
                                                 uint64_t ticket, VcoControlReply *out)
{
    assert(w == &worker && depth);
    ++worker_calls;
    return vco_mailbox_receive(&mailbox, pid, ticket, out);
}
VcoMailboxStatus vco_vita_control_worker_ack(VcoVitaControlWorker *w, int32_t pid, uint64_t ticket)
{
    assert(w == &worker && depth);
    ++worker_calls;
    return vco_mailbox_ack(&mailbox, pid, ticket);
}
VcoMailboxStatus vco_vita_control_worker_resume(VcoVitaControlWorker *w, int32_t pid,
                                                VcoControlRequest *out, uint64_t *ticket)
{
    assert(w == &worker && depth);
    ++worker_calls;
    return vco_mailbox_resume(&mailbox, pid, out, ticket);
}
static void setup(void)
{
    memset(user, 0xa5, sizeof(user));
    entered = exited = depth = from_calls = to_calls = auth_calls = worker_calls = fail_authorize =
        0;
    fail_from = fail_to = false;
    caller = 88;
    vco_mailbox_init(&mailbox, NULL, 88);
    rpc = (VcoVitaControlRpc){&worker, 88, authorize, &rpc};
    VcoControlRequest request = {1, 1, 1, VCO_CONTROL_ABI, sizeof(request), VCO_CONTROL_QUERY,
                                 0, 0, 1, {0, 0}};
    memcpy(user, &request, sizeof(request));
}
static VcoMailboxStatus submit(void)
{
    return vco_vita_rpc_submit(&rpc, USER_REQUEST, sizeof(VcoControlRequest), USER_TICKET);
}
static uint64_t ticket(void)
{
    uint64_t value;
    memcpy(&value, user + 1024, sizeof(value));
    return value;
}
static void authorization(void)
{
    caller = 77;
    assert(submit() == VCO_MAILBOX_OWNER && !from_calls && !to_calls && !worker_calls &&
           !auth_calls);
    caller = 88;
    rpc.authorize = NULL;
    assert(submit() == VCO_MAILBOX_OWNER && !from_calls);
    rpc.authorize = authorize;
    fail_authorize = 1;
    assert(submit() == VCO_MAILBOX_OWNER && !from_calls);
    setup();
    fail_authorize = 2; /* revoke after CopyFromUser, before enqueue */
    assert(submit() == VCO_MAILBOX_OWNER && from_calls == 1 && !worker_calls);
    setup();
    fail_authorize = 3; /* enqueue happened, don't leak ticket to revoked owner */
    assert(submit() == VCO_MAILBOX_OWNER && worker_calls == 1 && !to_calls);
    fail_authorize = 0;
    assert(submit() == VCO_MAILBOX_OK && ticket() == 1); /* idempotent recovery */
    caller = 77;
    assert(vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(VcoControlReply)) == VCO_MAILBOX_OWNER);
    assert(vco_vita_rpc_ack(&rpc, 1) == VCO_MAILBOX_OWNER);
    assert(vco_vita_rpc_resume(&rpc, USER_REPLY, sizeof(VcoControlResume)) == VCO_MAILBOX_OWNER);
}
static void buffers(void)
{
    VcoControlRequest local = {0};
    assert(vco_vita_rpc_submit(&rpc, &local, sizeof(local), USER_TICKET) == VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_submit(&rpc, (void *)0x1000, sizeof(local), USER_TICKET) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_submit(&rpc, (void *)(uintptr_t)0xfffffff0u, sizeof(local), USER_TICKET) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_submit(&rpc, USER_REQUEST, sizeof(local), (void *)(uintptr_t)0xfffffffcu) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_submit(&rpc, USER_REQUEST, sizeof(local) - 1, USER_TICKET) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_receive(&rpc, 0, USER_REPLY, sizeof(VcoControlReply)) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(VcoControlReply) + 1) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_ack(&rpc, 0) == VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_rpc_resume(&rpc, USER_REPLY, sizeof(VcoControlResume) - 1) ==
           VCO_MAILBOX_ARGUMENT);
    assert(!from_calls && !to_calls && !worker_calls);
    /* Input/output overlap is safe: complete kernel copy precedes ticket write. */
    assert(!vco_vita_rpc_submit(&rpc, USER_REQUEST, sizeof(local), USER_REQUEST));
    assert(vco_mailbox_pump(&mailbox, 1));
    assert(!vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(VcoControlReply)));
    VcoControlReply reply;
    memcpy(&reply, user + 128, sizeof(reply));
    assert(reply.request == 1 && reply.status == VCO_CONTROL_UNBOUND);
}
static void copy_failures(void)
{
    fail_from = true;
    assert(submit() == VCO_MAILBOX_COPY && !worker_calls);
    fail_from = false;
    fail_to = true;
    assert(submit() == VCO_MAILBOX_COPY && worker_calls == 1);
    assert(submit() == VCO_MAILBOX_COPY && mailbox.sequence == 1);
    fail_to = false;
    assert(!submit() && ticket() == 1);
    assert(vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(VcoControlReply)) == VCO_MAILBOX_WAIT);
    assert(vco_mailbox_pump(&mailbox, 1));
    fail_to = true;
    assert(vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(VcoControlReply)) == VCO_MAILBOX_COPY);
    assert(vco_vita_rpc_resume(&rpc, USER_REPLY, sizeof(VcoControlResume)) == VCO_MAILBOX_COPY);
    fail_to = false;
    assert(!vco_vita_rpc_resume(&rpc, USER_REPLY, sizeof(VcoControlResume)));
    VcoControlResume resume;
    memcpy(&resume, user + 128, sizeof(resume));
    assert(resume.ticket == 1 && resume.request.request == 1);
    assert(!vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(VcoControlReply)));
    VcoControlReply first, second;
    memcpy(&first, user + 128, sizeof(first));
    assert(!vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(VcoControlReply)));
    memcpy(&second, user + 128, sizeof(second));
    assert(!memcmp(&first, &second, sizeof(first)));
    assert(!vco_vita_rpc_ack(&rpc, 1));
    assert(vco_vita_rpc_resume(&rpc, USER_REPLY, sizeof(VcoControlResume)) == VCO_MAILBOX_EMPTY);
    assert(vco_vita_rpc_receive(&rpc, 1, USER_REPLY, sizeof(first)) == VCO_MAILBOX_TICKET);
}

/* End-to-end client/RPC/protocol/engine path. Only SDK user copies, the thread
 * boundary, and final game-memory/taiHEN operations are simulated here. The
 * earlier cases separately exercise the same syscall authorization checks. */
static VcoControlTransport pipeline_transport;
static VcoEngine pipeline_engine;
static VcoSession pipeline_session;
static VcoCheatEntry pipeline_entries[2];
static VcoCode pipeline_codes[2];
static VcoCheatOp pipeline_ops[2];
static VcoPatch pipeline_patches[2];
static char pipeline_names[64];
static uint64_t pipeline_now;
static unsigned pipeline_writes, pipeline_injections, pipeline_releases;
static bool pipeline_patch_live;
static int pipeline_alive(void *context, uint64_t token)
{
    assert(context == &pipeline_session);
    return vco_session_matches(&pipeline_session, token, true);
}
static int pipeline_write(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                          size_t size)
{
    const uint8_t expected[] = {0x44, 0x33, 0x22, 0x11};
    assert(pipeline_alive(context, token) && address == 0x81000000u);
    assert(size == sizeof(expected) && !memcmp(bytes, expected, sizeof(expected)));
    ++pipeline_writes;
    return 0;
}
static int pipeline_inject(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                           size_t size, int32_t *handle)
{
    const uint8_t expected[] = {0x01, 0x00, 0xa0, 0xe3};
    assert(pipeline_alive(context, token) && address == 0x81000010u);
    assert(size == sizeof(expected) && !memcmp(bytes, expected, sizeof(expected)));
    assert(!pipeline_patch_live);
    pipeline_patch_live = true;
    ++pipeline_injections;
    *handle = 200;
    return 0;
}
static int pipeline_release(void *context, uint64_t token, int32_t handle)
{
    assert(pipeline_alive(context, token) && handle == 200 && pipeline_patch_live);
    pipeline_patch_live = false;
    ++pipeline_releases;
    return 0;
}
static VcoMailboxStatus pipeline_submit(void *context, const VcoControlRequest *request,
                                        uint64_t *out)
{
    assert(context == &rpc);
    memcpy(user, request, sizeof(*request));
    VcoMailboxStatus result = submit();
    /* Propagate even failed, half-written user outputs into callback arguments:
     * the transport must inspect status before interpreting those bytes. */
    *out = ticket();
    return result;
}
static VcoMailboxStatus pipeline_resume(void *context, VcoControlRequest *request, uint64_t *out)
{
    assert(context == &rpc);
    VcoMailboxStatus result = vco_vita_rpc_resume(&rpc, USER_REPLY, sizeof(VcoControlResume));
    VcoControlResume resumed;
    memcpy(&resumed, user + 128, sizeof(resumed));
    *request = resumed.request;
    *out = resumed.ticket;
    return result;
}
static VcoMailboxStatus pipeline_receive(void *context, uint64_t assigned, VcoControlReply *reply)
{
    assert(context == &rpc);
    VcoMailboxStatus result = vco_vita_rpc_receive(&rpc, assigned, USER_REPLY, sizeof(*reply));
    memcpy(reply, user + 128, sizeof(*reply));
    return result;
}
static VcoMailboxStatus pipeline_ack(void *context, uint64_t assigned)
{
    assert(context == &rpc);
    return vco_vita_rpc_ack(&rpc, assigned);
}
static void pipeline_tick(VcoMailboxStatus expected)
{
    unsigned before = entered;
    assert(vco_transport_tick(&pipeline_transport) == expected);
    assert(entered == before + 1 && entered == exited && !depth);
}
static void pipeline_pump(void)
{
    assert(vco_mailbox_pump(&mailbox, pipeline_now));
}
static void pipeline_query(void)
{
    assert(vco_transport_query(&pipeline_transport, 0, 2));
    pipeline_tick(VCO_MAILBOX_OK);
    pipeline_pump();
    pipeline_tick(VCO_MAILBOX_OK);
    pipeline_tick(VCO_MAILBOX_OK);
    assert(vco_transport_idle(&pipeline_transport));
}
static void pipeline_faulted_enable(unsigned entry)
{
    VcoControlClient *client = &pipeline_transport.client;
    assert(client->known[entry] && client->rows[entry].state == VCO_ENTRY_OFF);
    assert(vco_transport_set(&pipeline_transport, entry, true));
    uint64_t previous_ticket = mailbox.sequence;
    uint64_t request = client->flight.request;
    fail_to = true;
    pipeline_tick(VCO_MAILBOX_COPY); /* SET queued, ticket only half copied. */
    assert(mailbox.sequence == previous_ticket + 1 && !pipeline_transport.ticket);
    pipeline_pump(); /* The accepted SET runs even though its ticket was lost. */
    pipeline_tick(VCO_MAILBOX_COPY);
    pipeline_pump();
    assert(mailbox.sequence == previous_ticket + 1 && client->flight.request == request);
    assert(client->rows[entry].state == VCO_ENTRY_OFF && client->waiting);
    fail_to = false;
    pipeline_tick(VCO_MAILBOX_OK);
    assert(pipeline_transport.ticket == previous_ticket + 1);
    fail_to = true;
    pipeline_tick(VCO_MAILBOX_COPY); /* Partial reply must not become a snapshot. */
    assert(client->rows[entry].state == VCO_ENTRY_OFF && client->waiting);
    assert(pipeline_transport.phase == VCO_TRANSPORT_RECEIVE);
    fail_to = false;
    pipeline_tick(VCO_MAILBOX_OK);
    assert(client->rows[entry].state == VCO_ENTRY_ENABLING &&
           client->rows[entry].pending == request && !client->waiting);
    pipeline_tick(VCO_MAILBOX_OK);
    assert(!vco_transport_set(&pipeline_transport, entry, false));
    pipeline_query();
    assert(client->rows[entry].state == VCO_ENTRY_ON && !client->rows[entry].pending &&
           client->rows[entry].acknowledged == request);
}
static void pipeline_disable(unsigned entry)
{
    assert(vco_transport_set(&pipeline_transport, entry, false));
    pipeline_tick(VCO_MAILBOX_OK);
    pipeline_pump();
    pipeline_tick(VCO_MAILBOX_OK);
    assert(pipeline_transport.client.rows[entry].state == VCO_ENTRY_DISABLING);
    pipeline_tick(VCO_MAILBOX_OK);
    pipeline_query();
    assert(pipeline_transport.client.rows[entry].state == VCO_ENTRY_OFF);
}
static void pipeline(void)
{
    VcoIdentity identity = {42, 3, "PCSB00031"};
    vco_session_init(&pipeline_session);
    assert(vco_session_observe(&pipeline_session, VCO_OBSERVATION_FOREGROUND, &identity));
    vco_engine_init(&pipeline_engine);
    VcoCheatFile file = {pipeline_entries,       2, 0, pipeline_codes, 2, 0, pipeline_names,
                         sizeof(pipeline_names), 0};
    VcoDiagnostic diagnostic;
    const char text[] = "_V1 Data\n$0200 81000000 11223344\n_V1 Patch\n$A200 81000010 E3A00001\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    VcoCheatMemory memory = {0};
    memory.context = &pipeline_session;
    memory.alive = pipeline_alive;
    memory.write = pipeline_write;
    memory.inject = pipeline_inject;
    memory.release = pipeline_release;
    assert(!vco_engine_bind(&pipeline_engine, &pipeline_session, 1, &file, memory, pipeline_ops,
                            pipeline_patches, 2));
    vco_mailbox_init(&mailbox, &pipeline_engine, 88);
    pipeline_now = 1;
    pipeline_writes = pipeline_injections = pipeline_releases = 0;
    pipeline_patch_live = false;
    VcoTransportOps callbacks = {pipeline_submit, pipeline_resume, pipeline_receive, pipeline_ack};
    assert(vco_transport_init(&pipeline_transport, callbacks, &rpc));
    assert(vco_transport_bind(&pipeline_transport, pipeline_session.token, pipeline_engine.revision,
                              2));
    pipeline_tick(VCO_MAILBOX_EMPTY);
    pipeline_pump();
    assert(!pipeline_writes && !pipeline_injections);
    assert(!vco_transport_set(&pipeline_transport, 0, true));
    assert(!vco_transport_set(&pipeline_transport, 1, true));
    pipeline_query();
    assert(pipeline_transport.client.rows[0].state == VCO_ENTRY_OFF &&
           pipeline_transport.client.rows[1].state == VCO_ENTRY_OFF);
    assert(!pipeline_writes && !pipeline_injections); /* _V1 never auto-enables. */

    pipeline_faulted_enable(0);
    assert(pipeline_writes == 1 && !pipeline_injections);
    pipeline_faulted_enable(1);
    assert(pipeline_writes == 1 && pipeline_injections == 1 && pipeline_patch_live);
    pipeline_now += VCO_ENGINE_PERIOD_US;
    pipeline_pump();
    assert(pipeline_writes == 2 && pipeline_injections == 1);

    /* OFF queued on the next freeze deadline wins before another data write. */
    pipeline_now += VCO_ENGINE_PERIOD_US;
    pipeline_disable(0);
    assert(pipeline_writes == 2 && pipeline_patch_live && !pipeline_releases);
    pipeline_now += VCO_ENGINE_PERIOD_US * 2;
    pipeline_pump();
    assert(pipeline_writes == 2 && pipeline_injections == 1);
    pipeline_disable(1);
    assert(!pipeline_patch_live && pipeline_releases == 1 && pipeline_injections == 1);
    pipeline_now += VCO_ENGINE_PERIOD_US;
    pipeline_pump();
    assert(pipeline_writes == 2 && pipeline_releases == 1);
    assert(!vco_engine_unbind(&pipeline_engine));
    assert(!pipeline_patch_live && pipeline_releases == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "authorization"))
        authorization();
    else if (!strcmp(argv[1], "buffers"))
        buffers();
    else if (!strcmp(argv[1], "copy_failures"))
        copy_failures();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else
        assert(0);
    assert(entered == exited && !depth);
    return 0;
}
