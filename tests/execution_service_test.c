#include "platform/execution_service.h"
#include "execution/service_protocol.h"
#include "execution/trace.h"
#include "diagnostics.h"
#include "platform/resource_profile.h"
#include <psp2/kernel/threadmgr.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* Execute the real user service, upload codec, transport, mailbox and engine.
 * Only native threads/syscall boundaries are deterministic fakes. Every delay
 * yields after the service has dropped its locks, so tests can interleave UI
 * input, catalog publication and lifecycle changes without private-state access. */
static SceKernelThreadEntry threads[2];
static unsigned mutex_count, thread_count, held[2], running_thread, delays;
static bool fail_try_lock;
#ifdef VCO_RESOURCE_PROFILE
static unsigned profile_calls;
void vco_resource_profile_tick(int32_t observer, int32_t control, uint32_t state,
                               unsigned enabled, uint64_t session)
{
    (void)session;
    assert(running_thread == 1 && !held[0] && !held[1]);
    assert(observer == 11 && control == 12 && state <= VCO_EXECUTION_UNAVAILABLE);
    assert(enabled <= VCO_ENGINE_ENTRIES);
    ++profile_calls;
}
#endif
static jmp_buf schedule;
SceUID sceKernelCreateMutex(const char *name, unsigned attr, int initial, const void *opt)
{
    assert(name && !attr && !initial && !opt && mutex_count < 2);
    return (SceUID)++mutex_count;
}
int sceKernelDeleteMutex(SceUID id)
{
    assert(id >= 1 && id <= 2);
    return 0;
}
int sceKernelLockMutex(SceUID id, int count, SceUInt *timeout)
{
    assert(id >= 1 && id <= 2 && count == 1 && !timeout && !held[id - 1]);
    held[id - 1] = 1;
    return 0;
}
int sceKernelTryLockMutex(SceUID id, int count)
{
    if (fail_try_lock || held[id - 1])
        return -1;
    return sceKernelLockMutex(id, count, NULL);
}
int sceKernelUnlockMutex(SceUID id, int count)
{
    assert(id >= 1 && id <= 2 && count == 1 && held[id - 1]);
    held[id - 1] = 0;
    return 0;
}
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority,
                             SceSize stack, unsigned attr, int cpu, const void *opt)
{
    assert(name && entry && priority > 0 && stack >= 8192 && !attr && !cpu && !opt);
    assert(thread_count < 2);
    threads[thread_count++] = entry;
    return (SceUID)(thread_count + 10);
}
int sceKernelStartThread(SceUID id, SceSize size, void *args)
{
    assert(id >= 11 && id <= 12 && !size && !args);
    return 0;
}
int sceKernelDeleteThread(SceUID id)
{
    assert(id >= 11 && id <= 12);
    return 0;
}
int sceKernelWaitThreadEnd(SceUID id, int *status, SceUInt *timeout)
{
    assert(id >= 11 && id <= 12 && !status && !timeout);
    return 0;
}
int sceKernelExitDeleteThread(int result)
{
    return result;
}
int sceKernelDelayThread(SceUInt usec)
{
    assert(!held[0] && !held[1]);
    assert(usec == (running_thread ? 20000u : 50000u));
    ++delays;
    longjmp(schedule, 1);
}
static void run(unsigned which, unsigned count)
{
    running_thread = which;
    for (unsigned i = 0; i < count; ++i)
        if (!setjmp(schedule)) {
            threads[which](0, NULL);
            assert(!"worker unexpectedly exited");
        }
}

static VcoSession session;
static VcoEngine engine;
static VcoControlMailbox mailbox;
static VcoCheatEntry entries[32];
static VcoCode codes[32];
static VcoCheatOp ops[32];
static VcoPatch patches[32];
static VcoCheatFile decoded;
static VcoServiceStatus kernel_status;
static VcoCatalog catalog;
static VcoCatalogWorkspace workspace;
static VcoExecutionView view;
static uint8_t pending_packet[VCO_UPLOAD_MAX], game[128], saved_patch[4];
static size_t pending_length;
static bool load_pending, clear_pending, hold_load, patch_live, malformed_reply;
static unsigned upload_calls, upload_processed, clear_calls, clear_processed, writes, injections;
static unsigned releases, observations, submit_copy, receive_copy, load_copy, query_mask;
static uint64_t last_set_request;
static VcoMailboxStatus load_failure;
static VcoMailboxStatus status_failure;
static VcoError fail_upload;
static uint64_t now_us, next_list_revision;
static uint64_t trace_first, trace_last, trace_after;
static uint32_t trace_dropped_count;
static unsigned trace_calls, trace_log_attempts, trace_log_count, trace_log_fail;
static unsigned trace_log_busy, trace_log_busy_at;
static unsigned health_attempts, health_log_count, health_busy, health_fail;
static char health_tags[32][VCO_TRACE_TAG_SIZE];
static unsigned health_values[32];
static VcoMailboxStatus trace_failure;
static bool trace_override;
static VcoTraceBatch trace_invalid_batch;
static char trace_tags[64][VCO_TRACE_TAG_SIZE];
static unsigned trace_values[64];
static const VcoIdentity identity = {77, 1, "PCSE00001"};
static VcoTraceRecord trace_record(uint64_t sequence)
{
    return (VcoTraceRecord){sequence,
                            sequence * 10,
                            65,
                            -(int32_t)sequence,
                            0x81000000u + (uint32_t)sequence,
                            0xa5000000u + (uint32_t)sequence};
}
static VcoTraceBatch trace_response(uint64_t after)
{
    VcoTraceBatch batch = {.abi = VCO_TRACE_ABI,
                           .size = sizeof(batch),
                           .oldest_sequence = trace_first,
                           .latest_sequence = trace_last,
                           .dropped = trace_dropped_count};
    if (after < trace_last) {
        uint64_t first = after + 1;
        if (first < trace_first)
            first = trace_first;
        uint64_t count = trace_last - first + 1;
        batch.count = count > VCO_TRACE_BATCH ? VCO_TRACE_BATCH : (unsigned)count;
        for (unsigned i = 0; i < batch.count; ++i)
            batch.records[i] = trace_record(first + i);
    }
    return batch;
}
int vcoServiceTrace(uint64_t after, VcoTraceBatch *out, uint32_t length)
{
    assert(running_thread == 1 && !held[0] && !held[1] && length == sizeof(*out));
    trace_after = after;
    ++trace_calls;
    if (trace_failure) {
        /* Simulate a failed half-copy: no field may become a trusted cursor. */
        memset(out, 0xa5, sizeof(*out) / 2);
        return trace_failure;
    }
    *out = trace_override ? trace_invalid_batch : trace_response(after);
    return VCO_MAILBOX_OK;
}
int vco_diagnostics_write(const char *path, const char *tag, unsigned value, int reset)
{
    assert(running_thread == 1 && !held[0] && !held[1] && !reset);
    assert(strlen(tag) < VCO_TRACE_TAG_SIZE);
    if (!strcmp(path, VCO_SHELL_LOG)) {
        ++health_attempts;
        if (health_busy) {
            --health_busy;
            return VCO_DIAGNOSTICS_BUSY;
        }
        if (health_attempts == health_fail)
            return VCO_DIAGNOSTICS_ERROR;
        assert(health_log_count < 32);
        strcpy(health_tags[health_log_count], tag);
        health_values[health_log_count++] = value;
        return VCO_DIAGNOSTICS_OK;
    }
    assert(!strcmp(path, VCO_NATIVE_LOG));
    if (++trace_log_attempts == trace_log_fail)
        return VCO_DIAGNOSTICS_ERROR;
    if (trace_log_attempts == trace_log_busy_at || trace_log_busy) {
        if (trace_log_busy)
            --trace_log_busy;
        return VCO_DIAGNOSTICS_BUSY;
    }
    assert(trace_log_count < 64);
    strcpy(trace_tags[trace_log_count], tag);
    trace_values[trace_log_count++] = value;
    return 0;
}
static int alive(void *ctx, uint64_t token)
{
    (void)ctx;
    return vco_session_matches(&session, token, true);
}
static bool range(uint32_t address, size_t length)
{
    return address >= 0x81000000u && (uint64_t)address + length <= UINT64_C(0x81000080);
}
static int write_memory(void *ctx, uint64_t token, uint32_t address, const uint8_t *in,
                        size_t length)
{
    if (!alive(ctx, token) || !range(address, length))
        return -1;
    memcpy(game + (address - 0x81000000u), in, length);
    ++writes;
    return 0;
}
static int inject(void *ctx, uint64_t token, uint32_t address, const uint8_t *in, size_t length,
                  int32_t *handle)
{
    assert(alive(ctx, token) && address == 0x81000004u && length == 4 && !patch_live);
    memcpy(saved_patch, game + 4, 4);
    memcpy(game + 4, in, 4);
    *handle = 9;
    patch_live = true;
    ++injections;
    return 0;
}
static int release(void *ctx, uint64_t token, int32_t handle)
{
    (void)ctx;
    (void)token;
    assert(handle == 9 && patch_live);
    memcpy(game + 4, saved_patch, 4);
    patch_live = false;
    ++releases;
    return 0;
}
static void refresh_status(void)
{
    kernel_status.session = session.present ? session.token : 0;
    kernel_status.pid = session.present ? session.identity.pid : 0;
    memset(kernel_status.title_id, 0, sizeof(kernel_status.title_id));
    if (session.present)
        memcpy(kernel_status.title_id, session.identity.title_id, sizeof(kernel_status.title_id));
    kernel_status.revision = engine.bound ? engine.revision : 0;
    kernel_status.count = engine.bound ? engine.count : 0;
    if (kernel_status.state != VCO_SERVICE_CONFLICT)
        kernel_status.state = kernel_status.error ? VCO_SERVICE_ERROR
                              : !session.present  ? VCO_SERVICE_NO_GAME
                              : !engine.bound     ? VCO_SERVICE_LOADING
                              : session.runnable  ? VCO_SERVICE_READY
                                                  : VCO_SERVICE_BACKGROUND;
}
static void kernel_step(void)
{
    now_us += 20000;
    if (!hold_load && load_pending) {
        assert(!vco_engine_unbind(&engine));
        kernel_status.catalog_revision = 0;
        if (clear_pending) {
            ++clear_processed;
            kernel_status.content_key = 0;
            kernel_status.error = 0;
        } else {
            ++upload_processed;
            VcoUploadHeader header;
            assert(!vco_upload_decode(pending_packet, pending_length, &decoded, &header));
            kernel_status.load_request = header.request;
            kernel_status.content_key = header.content_key;
            kernel_status.error = fail_upload;
            if (!session.present || header.target_session != session.token)
                kernel_status.error = VCO_E_SESSION;
            if (!kernel_status.error) {
                assert(session.present && header.target_session == session.token &&
                       header.pid == (uint32_t)session.identity.pid);
                VcoCheatMemory memory = {
                    .alive = alive, .write = write_memory, .inject = inject, .release = release};
                assert(!vco_engine_bind(&engine, &session, ++next_list_revision, &decoded, memory,
                                        ops, patches, 32));
                kernel_status.catalog_revision = header.catalog_revision;
            }
        }
        load_pending = clear_pending = false;
    }
    assert(vco_mailbox_pump(&mailbox, now_us));
    refresh_status();
}
int vcoServiceStatus(VcoServiceStatus *out, uint32_t length)
{
    assert(length == sizeof(*out));
    kernel_step();
    if (status_failure) {
        memset(out, 0xa5, sizeof(*out) / 2);
        return status_failure;
    }
    *out = kernel_status;
    return VCO_MAILBOX_OK;
}
int vcoServiceLoad(const void *packet, uint32_t length)
{
    assert((packet && length >= sizeof(VcoUploadHeader)) || (!packet && !length));
    if (load_failure)
        return load_failure;
    if (load_pending) {
        if (packet && !clear_pending && pending_length == length &&
            !memcmp(packet, pending_packet, length))
            return VCO_MAILBOX_OK;
        return VCO_MAILBOX_BUSY;
    }
    if (packet) {
        ++upload_calls;
        assert(length <= sizeof(pending_packet));
        memcpy(pending_packet, packet, length);
        pending_length = length;
    } else
        ++clear_calls;
    load_pending = true;
    clear_pending = !packet;
    if (load_copy) {
        --load_copy;
        return VCO_MAILBOX_COPY;
    }
    return VCO_MAILBOX_OK;
}
int vcoServiceSubmit(const VcoControlRequest *request, uint32_t length, uint64_t *out)
{
    uint64_t ticket;
    int result = vco_mailbox_submit(&mailbox, 88, request, length, &ticket);
    if (!result) {
        if (request->operation == VCO_CONTROL_QUERY)
            query_mask |= 1u << (request->entry / 16);
        else
            last_set_request = request->request;
        if (submit_copy) {
            --submit_copy;
            return VCO_MAILBOX_COPY;
        }
        *out = ticket;
    }
    return result;
}
int vcoServiceReceive(uint64_t ticket, VcoControlReply *out, uint32_t length)
{
    assert(length == sizeof(*out));
    VcoControlReply reply;
    int result = vco_mailbox_receive(&mailbox, 88, ticket, &reply);
    if (!result) {
        if (receive_copy) {
            --receive_copy;
            memcpy(out, &reply, sizeof(reply) / 2);
            return VCO_MAILBOX_COPY;
        }
        if (malformed_reply)
            reply.rows[0].desired = 7;
        *out = reply;
    }
    return result;
}
int vcoServiceAck(uint64_t ticket)
{
    return vco_mailbox_ack(&mailbox, 88, ticket);
}
int vcoServiceResume(void *out, uint32_t length)
{
    struct {
        uint64_t ticket;
        VcoControlRequest request;
    } packet;
    assert(length == sizeof(packet));
    int result = vco_mailbox_resume(&mailbox, 88, &packet.request, &packet.ticket);
    if (!result)
        memcpy(out, &packet, length);
    return result;
}
void vco_foreground_sample(VcoForegroundSample *out)
{
    assert(!held[0] && !held[1]);
    memset(out, 0, sizeof(*out));
    out->abi = VCO_FOREGROUND_PROBE_ABI;
    out->size = sizeof(*out);
}
int vcoServiceObserve(const VcoForegroundSample *sample, uint32_t length)
{
    assert(sample->abi == VCO_FOREGROUND_PROBE_ABI && length == sizeof(*sample));
    assert(!held[0] && !held[1]);
    ++observations;
    return VCO_MAILBOX_OK;
}
static const char source[] = "_V1 Money\n$0200 81000000 00000064\n"
                             "_V0 Instruction\n$A200 81000004 12345678\n";
static const char *source_text = source;
static VcoFileResult read_file(void *context, const char *path, char *out, size_t capacity,
                               size_t *length)
{
    (void)context;
    *length = strlen(source_text);
    assert(path && capacity >= *length);
    memcpy(out, source_text, *length);
    return VCO_FILE_OK;
}
static void publish_catalog(unsigned revision)
{
    catalog.revision = revision;
    assert(!vco_execution_service_catalog(&catalog, &workspace));
}
static void snapshot(void)
{
    (void)vco_execution_service_copy(&view);
}
static void await_ready(void)
{
    for (unsigned i = 0; i < 80; ++i) {
        run(1, 1);
        snapshot();
        if (view.state == VCO_EXECUTION_READY && view.known[0] && view.known[1])
            return;
    }
    assert(!"service did not become ready");
}
static void await_row(unsigned index, VcoEntryState state)
{
    for (unsigned i = 0; i < 80; ++i) {
        run(1, 1);
        snapshot();
        if (view.known[index] && !view.queued[index] && view.rows[index].state == (unsigned)state)
            return;
    }
    assert(!"row did not reach expected confirmed state");
}
static void setup(void)
{
    vco_session_init(&session);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    vco_engine_init(&engine);
    vco_mailbox_init(&mailbox, &engine, 88);
    decoded = (VcoCheatFile){entries, 32, 0, codes, 32, 0, NULL, 0, 0};
    kernel_status.abi = VCO_SERVICE_ABI;
    kernel_status.size = sizeof(kernel_status);
    refresh_status();
    VcoGame game_info = {.pid = 77, .title_id = "PCSE00001"};
    vco_catalog_load(&catalog, &game_info, &workspace, read_file, NULL);
    catalog.execution_session = session.token;
    assert(catalog.state == VCO_LIST_READY && catalog.count == 2 && workspace.entry_count == 2);
    assert(!vco_execution_service_start());
    assert(!vco_execution_service_start() && thread_count == 2);
    publish_catalog(1);
}
static void pipeline(void)
{
    await_ready();
    assert(!writes && !injections && view.rows[0].state == VCO_ENTRY_OFF);
    uint32_t revision = view.revision;
    assert(vco_execution_service_set(view.session, view.list_revision, 0, true) == 1);
    assert(vco_execution_service_set(view.session, view.list_revision, 0, true) == 0);
    snapshot();
    assert(view.revision != revision && view.queued[0] && view.rows[0].state == VCO_ENTRY_OFF);
    bool pending_seen = false;
    for (unsigned i = 0; i < 80; ++i) {
        run(1, 1);
        snapshot();
        pending_seen |= view.rows[0].state == VCO_ENTRY_ENABLING;
        if (view.rows[0].state == VCO_ENTRY_ON)
            break;
    }
    assert(pending_seen && view.rows[0].state == VCO_ENTRY_ON && writes && game[0] == 100);
    assert(vco_execution_service_set(view.session, view.list_revision, 0, false) == 1);
    await_row(0, VCO_ENTRY_OFF);
    unsigned previous_writes = writes;
    run(1, 12);
    assert(writes == previous_writes);
    fail_try_lock = true;
    assert(!vco_execution_service_copy(&view));
    assert(!vco_execution_service_set(view.session, view.list_revision, 0, true));
    fail_try_lock = false;
    assert(vco_execution_service_set(view.session + 1, view.list_revision, 0, true) == -1);
}
static void copies(void)
{
    load_copy = 1;
    hold_load = true;
    run(1, 12);
    assert(load_pending && upload_calls == 1 && !upload_processed);
    hold_load = false;
    await_ready();
    assert(upload_processed == 1);
    submit_copy = 2;
    receive_copy = 2;
    assert(vco_execution_service_set(view.session, view.list_revision, 1, true) == 1);
    await_row(1, VCO_ENTRY_ON);
    assert(injections == 1 && patch_live && !view.queued[1]);
    assert(vco_execution_service_set(view.session, view.list_revision, 1, false) == 1);
    await_row(1, VCO_ENTRY_OFF);
    assert(injections == 1 && releases == 1 && !patch_live);
}
static void reload(void)
{
    await_ready();
    assert(vco_execution_service_set(view.session, view.list_revision, 1, true) == 1);
    await_row(1, VCO_ENTRY_ON);
    VcoCatalog loading = {.revision = 2, .state = VCO_LIST_LOADING};
    assert(!vco_execution_service_catalog(&loading, NULL));
    hold_load = true;
    run(1, 1); /* Admit withdrawal, but delay its worker completion. */
    assert(clear_pending && clear_calls == 1);
    publish_catalog(3); /* File I/O already completed while clear is in flight. */
    unsigned previous = upload_calls;
    run(1, 20);
    snapshot();
    assert(upload_calls == previous && view.state == VCO_EXECUTION_LOADING && !view.known[0]);
    run(0, 10); /* Observer retains its cadence independently of the blocked load. */
    assert(observations == 10);
    hold_load = false;
    await_ready();
    assert(clear_processed == 1 && upload_processed == 2 && releases == 1 && !patch_live);
    assert(view.catalog_revision == 3 && view.rows[0].state == VCO_ENTRY_OFF &&
           view.rows[1].state == VCO_ENTRY_OFF);
}
static void lifecycle(void)
{
    await_ready();
    uint64_t old_session = view.session;
    assert(vco_execution_service_set(view.session, view.list_revision, 0, true) == 1);
    await_row(0, VCO_ENTRY_ON);
    vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL);
    run(1, 15);
    snapshot();
    assert(view.state == VCO_EXECUTION_BACKGROUND && view.rows[0].state == VCO_ENTRY_PAUSED &&
           view.session == old_session && upload_processed == 1);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    await_row(0, VCO_ENTRY_ON);
    assert(view.session == old_session && upload_processed == 1);
    assert(vco_execution_service_set(view.session, view.list_revision, 1, true) == 1);
    vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    assert(!vco_engine_unbind(&engine));
    kernel_status.catalog_revision = 0;
    kernel_status.content_key = 0;
    run(1, 3);
    snapshot();
    assert(view.state == VCO_EXECUTION_NO_GAME && !view.known[0] && !injections);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    run(1, 20);
    snapshot();
    assert(upload_processed == 1 && !engine.bound && !view.known[0]);
    catalog.execution_session = session.token; /* A fresh catalog read must authorize this token. */
    publish_catalog(2);
    await_ready();
    assert(view.session != old_session && view.catalog_revision == 2 && upload_processed == 2);
    assert(!view.queued[0] && !view.queued[1] && view.rows[0].state == VCO_ENTRY_OFF &&
           !injections);
    assert(vco_execution_service_set(view.session, view.list_revision, 1, true) == 1);
    await_row(1, VCO_ENTRY_ON);
    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    run(1, 15);
    snapshot();
    assert(view.state == VCO_EXECUTION_BACKGROUND && view.rows[1].state == VCO_ENTRY_PAUSED);
    assert(vco_execution_service_set(view.session, view.list_revision, 0, true) == -1);
    assert(vco_execution_service_set(view.session, view.list_revision, 1, false) == 1);
    await_row(1, VCO_ENTRY_OFF);
    assert(releases == 1);
}
static void failures(void)
{
    fail_upload = VCO_E_LIMIT;
    run(1, 20);
    snapshot();
    assert(upload_processed == 1 && view.state == VCO_EXECUTION_ERROR && view.error == VCO_E_LIMIT);
    run(1, 20);
    assert(upload_processed == 1); /* Terminal failure requires explicit reload. */
    VcoCatalog loading = {.revision = 2, .state = VCO_LIST_LOADING};
    assert(!vco_execution_service_catalog(&loading, NULL));
    hold_load = true;
    run(1, 2);
    publish_catalog(3);
    run(1, 10);
    assert(upload_processed == 1 && clear_pending);
    fail_upload = VCO_OK;
    hold_load = false;
    await_ready();
    assert(upload_processed == 2);
    status_failure = VCO_MAILBOX_COPY;
    run(1, 2);
    snapshot();
    assert(view.state == VCO_EXECUTION_UNAVAILABLE && !view.known[0]);
    status_failure = VCO_MAILBOX_OK;
    await_ready();
    malformed_reply = true;
    run(1, 20);
    snapshot();
    assert(view.state == VCO_EXECUTION_ERROR && view.error == VCO_E_ARGUMENT);
    VcoControlRequest pending_request;
    uint64_t ticket;
    assert(!vco_mailbox_resume(&mailbox, 88, &pending_request, &ticket)); /* Not auto-acked. */
}
static void readiness(void)
{
    catalog.execution_session = 0;
    publish_catalog(2);
    run(1, 10);
    snapshot();
    assert(!upload_calls && view.state == VCO_EXECUTION_LOADING && !view.known[0]);
    kernel_status.load_request = 99; /* UI restart must exceed the kernel watermark. */
    vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    run(1, 20);
    snapshot();
    assert(!upload_calls && view.state == VCO_EXECUTION_NO_GAME);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    kernel_status.state = VCO_SERVICE_CONFLICT;
    run(1, 10);
    snapshot();
    assert(!upload_calls && view.state == VCO_EXECUTION_CONFLICT);
    kernel_status.state = VCO_SERVICE_LOADING;
    catalog.execution_session = session.token;
    publish_catalog(3);
    load_failure = VCO_MAILBOX_OWNER;
    run(1, 3);
    assert(!upload_calls);
    load_failure = VCO_MAILBOX_CLOSED;
    run(1, 3);
    assert(!upload_calls);
    load_failure = VCO_MAILBOX_OK;
    await_ready();
    assert(upload_processed == 1 && kernel_status.load_request == 100);
    /* Public catalog staging owns the source before the workspace is reused. */
    memset(workspace.codes, 0, sizeof(workspace.codes));
    assert(vco_execution_service_set(view.session, view.list_revision, 0, true) == 1);
    await_row(0, VCO_ENTRY_ON);
    assert(game[0] == 100);
}
static void pagination(void)
{
    char text[2048];
    size_t length = 0;
    for (unsigned i = 0; i < 32; ++i) {
        int added = snprintf(text + length, sizeof(text) - length,
                             "_V1 Entry %u\n$0200 %08X %08X\n", i, 0x81000000u + i * 4u, i);
        assert(added > 0 && (size_t)added < sizeof(text) - length);
        length += (size_t)added;
    }
    source_text = text;
    VcoGame game_info = {.pid = 77, .title_id = "PCSE00001"};
    vco_catalog_load(&catalog, &game_info, &workspace, read_file, NULL);
    assert(catalog.count == 32 && workspace.entry_count == 32);
    catalog.execution_session = session.token;
    publish_catalog(2);
    /* The service owns a copy, not the catalog worker's temporary parse block. */
    memset(&workspace, 0xa5, sizeof(workspace));
    for (unsigned i = 0; i < 80; ++i) {
        run(1, 1);
        snapshot();
        if (view.known[31])
            break;
    }
    assert(view.count == 32 && query_mask == 3 && !writes);
    for (unsigned i = 0; i < 32; ++i)
        assert(view.known[i] && view.rows[i].state == VCO_ENTRY_OFF);
    assert(vco_execution_service_set(view.session, view.list_revision, 31, true) == 1);
    await_row(31, VCO_ENTRY_ON);
    assert(game[124] == 31);
}
static void recovery(void)
{
    /* The previous UI crashed after enqueue, before receiving its ticket. New
     * UI must drain its stale SET and keep its sequence watermark. */
    VcoControlRequest old = {700, 999, 1, VCO_CONTROL_ABI, sizeof(old), VCO_CONTROL_SET,
                             1,   1,   1, {0, 0}};
    uint64_t ticket;
    assert(!vco_mailbox_submit(&mailbox, 88, &old, sizeof(old), &ticket));
    await_ready();
    assert(!injections && !writes && view.rows[1].state == VCO_ENTRY_OFF);
    assert(vco_execution_service_set(view.session, view.list_revision, 1, true) == 1);
    await_row(1, VCO_ENTRY_ON);
    assert(injections == 1 && view.rows[1].acknowledged > 700);
    /* A temporary status failure while a SET is in flight hides stale rows.
     * Its eventual reply still clears the queue after the public binding was
     * temporarily hidden; it cannot leave the row permanently QUEUED. */
    assert(vco_execution_service_set(view.session, view.list_revision, 1, false) == 1);
    uint64_t previous_request = last_set_request;
    for (unsigned i = 0; i < 10 && last_set_request == previous_request; ++i)
        run(1, 1);
    assert(last_set_request != previous_request); /* In flight, reply not yet copied. */
    status_failure = VCO_MAILBOX_COPY;
    run(1, 10);
    snapshot();
    assert(view.state == VCO_EXECUTION_UNAVAILABLE && !view.known[1] && view.queued[1]);
    status_failure = VCO_MAILBOX_OK;
    await_row(1, VCO_ENTRY_OFF);
    assert(releases == 1 && !view.queued[1]);
}
static void instance(void)
{
    hold_load = true;
    run(1, 3);
    assert(load_pending && !clear_pending && upload_calls == 1 && !upload_processed);
    uint64_t old_token = session.token;
    vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    assert(session.token != old_token);
    hold_load = false;
    run(1, 20);
    snapshot();
    assert(upload_processed == 1 && upload_calls == 1 && !engine.bound && !writes && !injections);
    assert(view.state == VCO_EXECUTION_ERROR && view.error == VCO_E_SESSION);
    /* Re-publishing old bytes with the old token cannot retarget them just
     * because the same PID/title has acquired a new lifecycle. */
    publish_catalog(2);
    run(1, 20);
    assert(upload_calls == 1 && !engine.bound);
    VcoCatalog loading = {.revision = 3, .state = VCO_LIST_LOADING};
    assert(!vco_execution_service_catalog(&loading, NULL));
    run(1, 3);
    source_text = "_V1 New instance\n$0200 81000008 00000065\n"
                  "_V0 Instruction\n$A200 81000004 12345678\n";
    VcoGame game_info = {.pid = 77, .title_id = "PCSE00001"};
    vco_catalog_load(&catalog, &game_info, &workspace, read_file, NULL);
    catalog.execution_session = session.token;
    publish_catalog(4);
    await_ready();
    assert(upload_calls == 2 && view.rows[0].state == VCO_ENTRY_OFF && !writes);
    assert(vco_execution_service_set(view.session, view.list_revision, 0, true) == 1);
    await_row(0, VCO_ENTRY_ON);
    assert(game[8] == 101 && !game[0]);
}
static void trace_setup(void)
{
    await_ready();
    assert(vco_execution_service_set(view.session, view.list_revision, 1, true) == 1);
    await_row(1, VCO_ENTRY_ON);
    run(1, 10);
    snapshot();
    assert(view.state == VCO_EXECUTION_READY && !view.error && patch_live && injections == 1);
    assert(!trace_log_attempts);
}
static void trace_unchanged(const VcoExecutionView *before)
{
    snapshot();
    assert(!memcmp(before, &view, sizeof(view)) && patch_live && injections == 1 && !releases);
    assert(upload_processed == 1 && !writes);
}
static void trace(void)
{
    trace_setup();
    VcoExecutionView before = view;
    unsigned calls = trace_calls;
    trace_first = 1;
    trace_last = 12;
    trace_dropped_count = 2;
    run(1, 5);
    assert(trace_calls == calls + 1 && trace_after == 0 && trace_log_count == 9);
    assert(!strcmp(trace_tags[0], "native_dropped_total") && trace_values[0] == 2);
    for (unsigned i = 0; i < 8; ++i) {
        char tag[VCO_TRACE_TAG_SIZE];
        VcoTraceRecord record = trace_record(i + 1);
        assert(vco_trace_format(&record, tag, sizeof(tag)));
        assert(!strcmp(trace_tags[i + 1], tag) && trace_values[i + 1] == (uint32_t)record.result);
    }
    run(1, 5);
    assert(trace_calls == calls + 2 && trace_after == 8 && trace_log_count == 13);
    run(1, 5);
    assert(trace_after == 12 && trace_log_count == 13); /* Read is non-consuming, cursor is not. */
    trace_first = trace_last = 20;
    trace_dropped_count = 3;
    run(1, 5);
    assert(trace_after == 12 && trace_log_count == 16);
    assert(!strcmp(trace_tags[13], "native_overwritten") && trace_values[13] == 7);
    assert(!strcmp(trace_tags[14], "native_dropped_total") && trace_values[14] == 3);
    trace_unchanged(&before);
    run(0, 3);
    assert(observations == 3 && trace_calls == calls + 4); /* Never runs on the lease observer. */
    assert(vco_execution_service_set(view.session, view.list_revision, 1, false) == 1);
    await_row(1, VCO_ENTRY_OFF);
    assert(!patch_live && releases == 1 && injections == 1);
}
static void trace_invalid(void)
{
    trace_setup();
    VcoExecutionView before = view;
    trace_first = 1;
    trace_last = 2;
    trace_override = true;
    for (unsigned fault = 0; fault < 12; ++fault) {
        trace_invalid_batch = trace_response(0);
        switch (fault) {
        case 0:
            ++trace_invalid_batch.abi;
            break;
        case 1:
            --trace_invalid_batch.size;
            break;
        case 2:
            trace_invalid_batch.count = VCO_TRACE_BATCH + 1;
            break;
        case 3:
            trace_invalid_batch.oldest_sequence = 0;
            break;
        case 4:
            trace_invalid_batch.oldest_sequence = 3;
            break;
        case 5:
            trace_invalid_batch.latest_sequence = VCO_TRACE_CAPACITY + 1;
            break;
        case 6:
            --trace_invalid_batch.count;
            break;
        case 7:
            trace_invalid_batch.records[0].sequence = 0;
            break;
        case 8:
            trace_invalid_batch.records[1].sequence = 1;
            break;
        case 9:
            trace_invalid_batch.records[1].event = 0;
            break;
        case 10:
            trace_invalid_batch.records[2].detail = 1;
            break;
        case 11:
            trace_invalid_batch.latest_sequence = 0;
            break;
        }
        unsigned calls = trace_calls;
        run(1, 5);
        assert(trace_calls == calls + 1 && trace_after == 0 && !trace_log_attempts);
        trace_unchanged(&before);
    }
    trace_override = false;
    const VcoMailboxStatus failures[] = {VCO_MAILBOX_BUSY, VCO_MAILBOX_COPY, VCO_MAILBOX_CLOSED,
                                         VCO_MAILBOX_ARGUMENT};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        trace_failure = failures[i];
        run(1, 5);
        assert(trace_after == 0 && !trace_log_attempts);
        trace_unchanged(&before);
    }
    trace_failure = VCO_MAILBOX_OK;
    trace_dropped_count = 2;
    run(1, 5);
    assert(trace_after == 0 && trace_log_count == 3);
    trace_override = true;
    trace_invalid_batch = trace_response(2);
    trace_invalid_batch.dropped = 1; /* The cumulative counter cannot go backwards. */
    run(1, 5);
    assert(trace_after == 2 && trace_log_count == 3);
    trace_invalid_batch = trace_response(2);
    trace_invalid_batch.latest_sequence = 1; /* Never rewind a successfully saved cursor. */
    run(1, 5);
    assert(trace_after == 2 && trace_log_count == 3);
    trace_override = false;
    trace_last = 3;
    run(1, 5);
    assert(trace_after == 2 && trace_log_count == 4);
    trace_unchanged(&before);
}
static void trace_failure_case(void)
{
    trace_setup();
    VcoExecutionView before = view;
    trace_first = 1;
    trace_last = 4;
    trace_log_fail = 2;
    run(1, 5);
    assert(trace_after == 0 && trace_log_attempts == 2 && trace_log_count == 1);
    unsigned calls = trace_calls;
    run(1, 100);
    assert(trace_calls == calls &&
           trace_log_attempts == 2); /* Permanently bounded on I/O failure. */
    trace_unchanged(&before);
    assert(vco_execution_service_set(view.session, view.list_revision, 1, false) == 1);
    await_row(1, VCO_ENTRY_OFF);
    assert(!patch_live && releases == 1 && trace_calls == calls);
}
static void trace_writer_busy(void)
{
    trace_setup();
    VcoExecutionView before = view;
    trace_first = 1;
    trace_last = 4;
    trace_log_busy_at = 3;
    run(1, 5);
    assert(trace_after == 0 && trace_log_attempts == 3 && trace_log_count == 2);
    run(1, 5);
    assert(trace_after == 2 && trace_log_count == 4);
    for (unsigned i = 0; i < 4; ++i) {
        char tag[VCO_TRACE_TAG_SIZE];
        VcoTraceRecord record = trace_record(i + 1);
        assert(vco_trace_format(&record, tag, sizeof(tag)));
        assert(!strcmp(trace_tags[i], tag));
    }
    /* Busy while reporting an overwritten span must preserve both the cursor
     * and the cumulative dropped counter until their records are saved. */
    trace_first = trace_last = 20;
    trace_dropped_count = 3;
    trace_log_busy = 2;
    run(1, 10);
    assert(trace_after == 4 && trace_log_count == 4);
    run(1, 5);
    assert(trace_after == 4 && trace_log_count == 7);
    assert(!strcmp(trace_tags[4], "native_overwritten") && trace_values[4] == 15);
    assert(!strcmp(trace_tags[5], "native_dropped_total") && trace_values[5] == 3);
    trace_first = trace_last = 21;
    trace_log_busy = 100;
    unsigned calls = trace_calls;
    run(1, 100);
    assert(trace_calls == calls + 20 && trace_after == 20 && trace_log_count == 7);
    trace_unchanged(&before);
    /* Log contention must never prevent OFF or the corresponding restoration. */
    assert(vco_execution_service_set(view.session, view.list_revision, 1, false) == 1);
    await_row(1, VCO_ENTRY_OFF);
    assert(!patch_live && releases == 1);
    trace_log_busy = 0;
    run(1, 10);
    assert(trace_after == 21 && trace_log_count == 8);
}
static void trace_health(void)
{
    health_busy = 2;
    trace_setup();
    run(1, 20);
    VcoExecutionView before = view;
    assert(health_attempts == 3 && health_log_count == 1);
    assert(!strcmp(health_tags[0], "native_trace_ready") && health_values[0] == VCO_TRACE_ABI);
    /* The successful empty snapshot is observable without inventing a native
     * event or advancing the event cursor. Each failure kind reports once. */
    assert(!trace_log_count && trace_after == 0);
    const int failures[] = {VCO_MAILBOX_BUSY,  VCO_MAILBOX_COPY,     VCO_MAILBOX_CLOSED,
                            VCO_MAILBOX_OWNER, VCO_MAILBOX_ARGUMENT, -123};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        trace_failure = (VcoMailboxStatus)failures[i];
        run(1, 50);
        assert(health_log_count == i + 2 && trace_after == 0 && !trace_log_count);
        assert(!strcmp(health_tags[i + 1], "native_trace_rpc_failure") &&
               health_values[i + 1] == (unsigned)failures[i]);
        trace_unchanged(&before);
    }
    trace_failure = VCO_MAILBOX_OK;
    trace_override = true;
    trace_invalid_batch = trace_response(0);
    ++trace_invalid_batch.abi;
    run(1, 50);
    assert(health_log_count == 8 && !strcmp(health_tags[7], "native_trace_invalid") &&
           health_values[7] == trace_invalid_batch.abi && !trace_log_count);
    trace_override = false;
    trace_first = 1;
    trace_last = 2;
    run(1, 10);
    assert(trace_after == 2 && trace_log_count == 2 && health_log_count == 8);
    trace_unchanged(&before);
}
static void trace_health_failure(void)
{
    health_fail = 1;
    trace_setup();
    VcoExecutionView before = view;
    trace_first = 1;
    trace_last = 2;
    run(1, 100);
    assert(health_attempts == 1 && !health_log_count && trace_log_count == 2 && trace_after == 2);
    trace_unchanged(&before);
    trace_log_fail = trace_log_attempts + 1;
    trace_first = trace_last = 3;
    run(1, 5);
    assert(health_log_count == 1 && !strcmp(health_tags[0], "native_trace_write_failed") &&
           health_values[0] == (unsigned)VCO_DIAGNOSTICS_ERROR);
    unsigned calls = trace_calls, attempts = trace_log_attempts;
    run(1, 100);
    assert(trace_calls == calls && trace_log_attempts == attempts && health_log_count == 1);
    trace_unchanged(&before);
    assert(vco_execution_service_set(view.session, view.list_revision, 1, false) == 1);
    await_row(1, VCO_ENTRY_OFF);
    assert(!patch_live && releases == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else if (!strcmp(argv[1], "copies"))
        copies();
    else if (!strcmp(argv[1], "reload"))
        reload();
    else if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "failures"))
        failures();
    else if (!strcmp(argv[1], "readiness"))
        readiness();
    else if (!strcmp(argv[1], "pagination"))
        pagination();
    else if (!strcmp(argv[1], "recovery"))
        recovery();
    else if (!strcmp(argv[1], "instance"))
        instance();
    else if (!strcmp(argv[1], "trace"))
        trace();
    else if (!strcmp(argv[1], "trace_invalid"))
        trace_invalid();
    else if (!strcmp(argv[1], "trace_failure"))
        trace_failure_case();
    else if (!strcmp(argv[1], "trace_writer_busy"))
        trace_writer_busy();
    else if (!strcmp(argv[1], "trace_health"))
        trace_health();
    else if (!strcmp(argv[1], "trace_health_failure"))
        trace_health_failure();
    else
        assert(!"unknown case");
    assert(!held[0] && !held[1]);
#ifdef VCO_RESOURCE_PROFILE
    assert(profile_calls > 0);
#endif
    printf("execution service %s passed (%u bounded worker iterations)\n", argv[1], delays);
    return 0;
}
