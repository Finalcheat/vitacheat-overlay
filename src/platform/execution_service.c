#include "../storage_paths.h"
#include "execution_service.h"
#include "foreground_probe.h"
#include "../execution/service_protocol.h"
#include "../execution/transport.h"
#include "../execution/trace.h"
#include "../diagnostics.h"
#include "resource_profile.h"
#include <psp2/kernel/threadmgr.h>
#include <string.h>

/* The observer never waits for this service's locks, uploads, or file I/O.
 * Its 50 ms cadence renews the kernel's independently checked 250 ms lease. */
#define OBSERVE_US 50000u
#define CONTROL_US 20000u
#define TRACE_TICKS 5u
#define NATIVE_LOG_PATH VCO_NATIVE_LOG
typedef struct {
    uint64_t generation, clear_generation;
    uint32_t catalog_revision, catalog_state, count;
    VcoError error;
    size_t size;
    bool ready;
    uint8_t packet[VCO_UPLOAD_MAX];
} CatalogStage;
typedef struct {
    uint64_t session, revision;
    unsigned entry;
    bool enabled;
} QueuedCommand;
typedef struct {
    uint64_t ticket;
    VcoControlRequest request;
} ResumePacket;
static SceUID state_lock = -1, catalog_lock = -1;
static SceUID observer_thread = -1, control_thread = -1;
static uint32_t started, stopping;
static VcoExecutionView published, building;
static uint64_t published_generation;
static CatalogStage staged, desired;
static VcoControlTransport transport;
static QueuedCommand queue[VCO_ENGINE_QUEUE], flight;
static unsigned queue_head, queue_count;
static bool pending[VCO_ENGINE_ENTRIES], flight_active;
static VcoError command_error;
static uint64_t upload_sequence, completed_generation, cleared_generation;
static uint64_t assigned_request;
static uint64_t bound_session, bound_revision;
static bool upload_sent, clear_sent;
static VcoError upload_error;
static unsigned query_first;
/* Only the control worker touches this diagnostic state. It never determines
 * execution readiness or row state. A storage failure disables collection for
 * this user module's lifetime. Shared-writer contention leaves the record and
 * cursor intact for the next collection, without waiting or spinning. */
static uint64_t trace_cursor;
static uint32_t trace_dropped;
static unsigned trace_ticks;
static bool trace_disabled;
enum {
    TRACE_HEALTH_READY = 1u,
    TRACE_HEALTH_INVALID = 2u,
    TRACE_HEALTH_WRITE = 4u,
    TRACE_HEALTH_FORMAT = 8u,
    TRACE_HEALTH_RPC_UNKNOWN = 16u,
    TRACE_HEALTH_RPC_SHIFT = 5u
};
static uint32_t trace_health_reported;
static void trace_health(uint32_t kind, const char *tag, unsigned value)
{
    if (trace_health_reported & kind)
        return;
    /* One health record per kind, in the existing Shell log. Contention has
     * performed no I/O and can defer this report. Actual storage errors are
     * not retried. Health logging cannot disable or change execution/trace. */
    if (vco_diagnostics_write(VCO_SHELL_LOG, tag, value, 0) != VCO_DIAGNOSTICS_BUSY)
        trace_health_reported |= kind;
}

static bool trace_valid(const VcoTraceBatch *batch)
{
    if (batch->abi != VCO_TRACE_ABI || batch->size != sizeof(*batch) ||
        batch->count > VCO_TRACE_BATCH || batch->dropped < trace_dropped ||
        batch->latest_sequence < trace_cursor)
        return false;
    if (!batch->latest_sequence) {
        if (batch->oldest_sequence || batch->count)
            return false;
    } else {
        if (!batch->oldest_sequence || batch->oldest_sequence > batch->latest_sequence ||
            batch->latest_sequence - batch->oldest_sequence >= VCO_TRACE_CAPACITY)
            return false;
        uint64_t available = 0, first = 0;
        if (trace_cursor < batch->latest_sequence) {
            first = trace_cursor + 1;
            if (first < batch->oldest_sequence)
                first = batch->oldest_sequence;
            available = batch->latest_sequence - first + 1;
            if (available > VCO_TRACE_BATCH)
                available = VCO_TRACE_BATCH;
        }
        if (batch->count != available)
            return false;
        for (unsigned i = 0; i < batch->count; ++i)
            if (batch->records[i].sequence != first + i || !batch->records[i].event)
                return false;
    }
    const VcoTraceRecord empty = {0};
    for (unsigned i = batch->count; i < VCO_TRACE_BATCH; ++i)
        if (memcmp(&batch->records[i], &empty, sizeof(empty)))
            return false;
    return true;
}
static bool trace_save(const char *tag, unsigned value)
{
    int result = vco_diagnostics_write(NATIVE_LOG_PATH, tag, value, 0);
    if (result == VCO_DIAGNOSTICS_BUSY)
        return false;
    if (result != VCO_DIAGNOSTICS_OK) {
        trace_disabled = true;
        trace_health(TRACE_HEALTH_WRITE, "native_trace_write_failed", (unsigned)result);
        return false;
    }
    return true;
}
static void collect_trace(void)
{
    if (trace_disabled || ++trace_ticks < TRACE_TICKS)
        return;
    trace_ticks = 0;
    VcoTraceBatch batch = {0};
    int result = vcoServiceTrace(trace_cursor, &batch, sizeof(batch));
    if (result != VCO_MAILBOX_OK) {
        uint32_t kind = result > VCO_MAILBOX_OK && result <= VCO_MAILBOX_COPY
                            ? 1u << (TRACE_HEALTH_RPC_SHIFT + (unsigned)result)
                            : TRACE_HEALTH_RPC_UNKNOWN;
        trace_health(kind, "native_trace_rpc_failure", (unsigned)result);
        return;
    }
    if (!trace_valid(&batch)) {
        trace_health(TRACE_HEALTH_INVALID, "native_trace_invalid", batch.abi);
        return;
    }
    trace_health(TRACE_HEALTH_READY, "native_trace_ready", batch.abi);
    /* Report ring overwrite separately from events that never entered it.
     * Counts larger than the log value saturate; each record retains its full
     * 64-bit sequence in the formatted tag. */
    if (batch.count && batch.records[0].sequence - trace_cursor > 1) {
        uint64_t missing = batch.records[0].sequence - trace_cursor - 1;
        if (!trace_save("native_overwritten",
                        missing > UINT32_MAX ? UINT32_MAX : (unsigned)missing))
            return;
    }
    if (batch.dropped != trace_dropped) {
        if (!trace_save("native_dropped_total", batch.dropped))
            return;
        trace_dropped = batch.dropped;
    }
    for (unsigned i = 0; i < batch.count; ++i) {
        char tag[VCO_TRACE_TAG_SIZE];
        if (!vco_trace_format(&batch.records[i], tag, sizeof(tag))) {
            trace_disabled = true;
            trace_health(TRACE_HEALTH_FORMAT, "native_trace_format_failed", batch.records[i].event);
            return;
        }
        if (!trace_save(tag, (uint32_t)batch.records[i].result))
            return;
        trace_cursor = batch.records[i].sequence;
    }
}

static uint32_t next_revision(uint32_t value)
{
    ++value;
    return value ? value : 1;
}
static VcoMailboxStatus status_code(int value)
{
    return value >= VCO_MAILBOX_OK && value <= VCO_MAILBOX_COPY ? (VcoMailboxStatus)value
                                                                : VCO_MAILBOX_CLOSED;
}
static VcoError admission_error(VcoMailboxStatus result)
{
    if (result == VCO_MAILBOX_ARGUMENT)
        return VCO_E_ARGUMENT;
    if (result == VCO_MAILBOX_EXHAUSTED)
        return VCO_E_LIMIT;
    /* Owner retirement/rebind can race an otherwise valid Status. Keep the
     * immutable upload and retry after the next Status instead of permanently
     * disabling a newly restarted Shell because admission was briefly closed. */
    if (result == VCO_MAILBOX_BUSY || result == VCO_MAILBOX_WAIT || result == VCO_MAILBOX_OWNER ||
        result == VCO_MAILBOX_CLOSED)
        return VCO_OK;
    return VCO_E_IO;
}
static VcoMailboxStatus submit(void *ctx, const VcoControlRequest *request, uint64_t *ticket)
{
    (void)ctx;
    return status_code(vcoServiceSubmit(request, sizeof(*request), ticket));
}
static VcoMailboxStatus receive(void *ctx, uint64_t ticket, VcoControlReply *reply)
{
    (void)ctx;
    return status_code(vcoServiceReceive(ticket, reply, sizeof(*reply)));
}
static VcoMailboxStatus acknowledge(void *ctx, uint64_t ticket)
{
    (void)ctx;
    return status_code(vcoServiceAck(ticket));
}
static VcoMailboxStatus resume(void *ctx, VcoControlRequest *request, uint64_t *ticket)
{
    (void)ctx;
    ResumePacket packet = {0};
    VcoMailboxStatus result = status_code(vcoServiceResume(&packet, sizeof(packet)));
    if (result == VCO_MAILBOX_OK) {
        *request = packet.request;
        *ticket = packet.ticket;
    }
    return result;
}
static void reset_requests_locked(void)
{
    queue_head = queue_count = 0;
    memset(pending, 0, sizeof(pending));
    memset(published.queued, 0, sizeof(published.queued));
    command_error = VCO_OK;
}
static void publish(uint32_t state, VcoError error, const VcoServiceStatus *status, bool rows)
{
    memset(&building, 0, sizeof(building));
    building.catalog_revision = desired.catalog_revision;
    building.count = desired.count;
    building.state = state;
    if (status) {
        building.session = status->session;
        building.list_revision = status->revision;
    }
    if (rows) {
        memcpy(building.known, transport.client.known, sizeof(building.known));
        memcpy(building.rows, transport.client.rows, sizeof(building.rows));
        for (unsigned i = 0; i < building.count; ++i)
            if (building.known[i] && building.rows[i].desired)
                ++building.enabled_count;
    }
    sceKernelLockMutex(state_lock, 1, NULL);
    if (published_generation == desired.generation) {
        building.error = error ? error : command_error;
        memcpy(building.queued, pending, sizeof(building.queued));
        building.revision = published.revision;
        if (memcmp(&building, &published, sizeof(building))) {
            building.revision = next_revision(published.revision);
            published = building;
        }
    }
    sceKernelUnlockMutex(state_lock, 1);
}
static bool status_valid(const VcoServiceStatus *status)
{
    if (status->abi != VCO_SERVICE_ABI || status->size != sizeof(*status) ||
        status->state > VCO_SERVICE_LOADING || status->error > VCO_E_METADATA ||
        status->count > VCO_ENGINE_ENTRIES || status->reserved || status->padding[0] ||
        status->padding[1])
        return false;
    if (!status->revision)
        return !status->count;
    return status->session && status->pid > 0 && status->catalog_revision && status->content_key &&
           status->load_request && memchr(status->title_id, 0, sizeof(status->title_id)) &&
           vco_title_id_valid(status->title_id);
}
static bool same_upload(const VcoServiceStatus *status)
{
    VcoUploadHeader header;
    memcpy(&header, desired.packet, sizeof(header));
    return desired.ready && status->session == header.target_session &&
           status->catalog_revision == desired.catalog_revision &&
           status->pid == (int32_t)header.pid &&
           !memcmp(status->title_id, header.title_id, sizeof(header.title_id)) &&
           status->content_key == header.content_key && status->count == header.entries;
}
static bool target_ready(const VcoServiceStatus *status)
{
    VcoUploadHeader header;
    memcpy(&header, desired.packet, sizeof(header));
    return status->session && status->session == header.target_session &&
           status->pid == (int32_t)header.pid &&
           !memcmp(status->title_id, header.title_id, sizeof(header.title_id)) &&
           (status->state == VCO_SERVICE_LOADING || status->state == VCO_SERVICE_READY ||
            status->state == VCO_SERVICE_BACKGROUND);
}
static void invalidate_binding(const VcoServiceStatus *status)
{
    if (!bound_session || (bound_session == status->session && bound_revision == status->revision))
        return;
    /* A process can disappear and return between catalog discoveries. Do not
     * send queued old SETs into the new lifecycle, even when the title, PID and
     * displayed catalog revision happen to remain identical. */
    vco_transport_reconnect(&transport);
    bound_session = bound_revision = 0;
    flight_active = false;
    sceKernelLockMutex(state_lock, 1, NULL);
    reset_requests_locked();
    published.session = published.list_revision = 0;
    published.state = VCO_EXECUTION_LOADING;
    memset(published.known, 0, sizeof(published.known));
    memset(published.rows, 0, sizeof(published.rows));
    published.enabled_count = 0;
    published.revision = next_revision(published.revision);
    sceKernelUnlockMutex(state_lock, 1);
}
static void finish_command(VcoError error)
{
    sceKernelLockMutex(state_lock, 1, NULL);
    if (flight_active && published_generation == desired.generation &&
        bound_session == flight.session && bound_revision == flight.revision &&
        flight.entry < desired.count) {
        pending[flight.entry] = false;
        command_error = error;
    }
    sceKernelUnlockMutex(state_lock, 1);
    flight_active = false;
}
static void copy_catalog(void)
{
    bool changed = false;
    sceKernelLockMutex(catalog_lock, 1, NULL);
    if (staged.generation != desired.generation) {
        desired = staged;
        changed = true;
    }
    sceKernelUnlockMutex(catalog_lock, 1);
    if (changed) {
        /* A prior SET may already have reached the mailbox. Recover/drain it,
         * never submit an unsent old command after the displayed list changes. */
        vco_transport_reconnect(&transport);
        flight_active = false;
        assigned_request = 0;
        upload_sent = false;
        upload_error = VCO_OK;
        completed_generation = 0;
        bound_session = bound_revision = 0;
        query_first = 0;
    }
}
static void pump_transport(void)
{
    if (vco_transport_idle(&transport))
        return;
    VcoMailboxStatus result = vco_transport_tick(&transport);
    if (flight_active && !transport.client.waiting &&
        (transport.phase == VCO_TRANSPORT_ACK || vco_transport_idle(&transport))) {
        VcoError error = transport.client.last_status == VCO_CONTROL_OK ? VCO_OK
                         : transport.client.last_status == VCO_CONTROL_REJECTED
                             ? (VcoError)transport.client.last_error
                             : VCO_E_SESSION;
        finish_command(error);
    }
    if (result == VCO_MAILBOX_ARGUMENT && transport.invalid_reply) {
        /* Keep the malformed reply unacknowledged. Surface the protocol failure
         * instead of advancing rows or treating a timeout as a confirmed OFF. */
        upload_error = VCO_E_ARGUMENT;
    }
}
static bool clear_catalog(const VcoServiceStatus *status)
{
    if (desired.clear_generation <= cleared_generation)
        return true;
    if (clear_sent && !status->revision && !status->catalog_revision && !status->content_key &&
        !status->error && status->state != VCO_SERVICE_ERROR) {
        cleared_generation = desired.clear_generation;
        clear_sent = false;
        return true;
    }
    VcoMailboxStatus result = status_code(vcoServiceLoad(NULL, 0));
    if (result == VCO_MAILBOX_OK || result == VCO_MAILBOX_COPY)
        clear_sent = true;
    else {
        VcoError error = admission_error(result);
        if (error)
            upload_error = error;
    }
    return false;
}
static bool upload_catalog(const VcoServiceStatus *status)
{
    if (!desired.ready || !desired.size)
        return false;
    if (upload_sequence < status->load_request)
        upload_sequence = status->load_request;
    if (completed_generation == desired.generation) {
        if (upload_error)
            return false;
        if (status->revision && same_upload(status))
            return true;
        if (!target_ready(status))
            return false;
        /* Retry only within the instance that was verified around this file
         * read. A new lifecycle waits for a freshly read catalog generation. */
        completed_generation = 0;
        assigned_request = 0;
        upload_sent = false;
    }
    if (upload_sent && status->load_request == assigned_request) {
        completed_generation = desired.generation;
        if (status->error)
            upload_error = (VcoError)status->error;
        else if (!same_upload(status))
            upload_error = VCO_E_SESSION;
        return !upload_error;
    }
    if (status->state == VCO_SERVICE_ERROR) {
        upload_error = status->error ? (VcoError)status->error : VCO_E_IO;
        completed_generation = desired.generation;
        return false;
    }
    /* Catalog discovery and authority observation run independently. The first
     * successful file read is not proof that the kernel has acquired a target. */
    if (!target_ready(status))
        return false;
    if (upload_sent && status->load_request > assigned_request) {
        upload_sent = false;
        assigned_request = 0;
    }
    if (!assigned_request || (!upload_sent && assigned_request <= status->load_request)) {
        if (upload_sequence == UINT64_MAX) {
            upload_error = VCO_E_LIMIT;
            return false;
        }
        assigned_request = ++upload_sequence;
        VcoUploadHeader header;
        memcpy(&header, desired.packet, sizeof(header));
        header.request = assigned_request;
        memcpy(desired.packet, &header, sizeof(header));
    }
    VcoMailboxStatus result = status_code(vcoServiceLoad(desired.packet, (uint32_t)desired.size));
    if (result == VCO_MAILBOX_OK || result == VCO_MAILBOX_COPY)
        upload_sent = true;
    else {
        VcoError error = admission_error(result);
        if (error)
            upload_error = error;
    }
    return false;
}
static void begin_work(void)
{
    if (!vco_transport_idle(&transport) || !transport.client.bound)
        return;
    bool have_command = false;
    sceKernelLockMutex(state_lock, 1, NULL);
    if (queue_count) {
        flight = queue[queue_head];
        queue_head = (queue_head + 1) % VCO_ENGINE_QUEUE;
        --queue_count;
        have_command = true;
    }
    sceKernelUnlockMutex(state_lock, 1);
    if (have_command) {
        flight_active = true;
        if (flight.session != transport.client.session ||
            flight.revision != transport.client.revision ||
            !vco_transport_set(&transport, flight.entry, flight.enabled))
            finish_command(VCO_E_SESSION);
        return;
    }
    if (transport.client.count) {
        if (query_first >= transport.client.count)
            query_first = 0;
        unsigned count = transport.client.count - query_first;
        if (count > VCO_CONTROL_PAGE)
            count = VCO_CONTROL_PAGE;
        if (vco_transport_query(&transport, query_first, count))
            query_first += count;
    }
}
static int observe_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
        VcoForegroundSample sample;
        vco_foreground_sample(&sample);
        (void)vcoServiceObserve(&sample, sizeof(sample));
        sceKernelDelayThread(OBSERVE_US);
    }
    return sceKernelExitDeleteThread(0);
}
static int execution_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
        copy_catalog();
        VcoServiceStatus status = {0};
        VcoMailboxStatus result = status_code(vcoServiceStatus(&status, sizeof(status)));
        if (result == VCO_MAILBOX_OK && status_valid(&status)) {
            invalidate_binding(&status);
            pump_transport();
            bool cleared = clear_catalog(&status);
            bool loaded = cleared && !upload_error && upload_catalog(&status);
            bool matched = loaded && same_upload(&status) && status.session && status.revision;
            if (matched && (!transport.client.bound || transport.client.session != status.session ||
                            transport.client.revision != status.revision ||
                            transport.client.count != status.count)) {
                vco_transport_bind(&transport, status.session, status.revision, status.count);
                bound_session = status.session;
                bound_revision = status.revision;
                query_first = 0;
            }
            uint32_t state = VCO_EXECUTION_LOADING;
            VcoError error = desired.error ? desired.error : upload_error;
            bool rows = false;
            if (status.state == VCO_SERVICE_CONFLICT)
                state = VCO_EXECUTION_CONFLICT;
            else if (status.state == VCO_SERVICE_ERROR || error) {
                state = VCO_EXECUTION_ERROR;
                if (!error)
                    error = (VcoError)status.error;
            } else if (status.state == VCO_SERVICE_NO_GAME)
                state = VCO_EXECUTION_NO_GAME;
            else if (matched && (status.state == VCO_SERVICE_READY ||
                                 status.state == VCO_SERVICE_BACKGROUND)) {
                state = status.state == VCO_SERVICE_READY ? VCO_EXECUTION_READY
                                                          : VCO_EXECUTION_BACKGROUND;
                rows = true;
                begin_work();
            }
            publish(state, error, matched ? &status : NULL, rows);
        } else if (result != VCO_MAILBOX_BUSY && result != VCO_MAILBOX_WAIT) {
            publish(result == VCO_MAILBOX_OK ? VCO_EXECUTION_ERROR : VCO_EXECUTION_UNAVAILABLE,
                    result == VCO_MAILBOX_OK ? VCO_E_ARGUMENT : VCO_E_IO, NULL, false);
        }
        collect_trace();
#ifdef VCO_RESOURCE_PROFILE
        vco_resource_profile_tick(observer_thread, control_thread, building.state,
                                  building.enabled_count, building.session);
#endif
        sceKernelDelayThread(CONTROL_US);
    }
    return sceKernelExitDeleteThread(0);
}
int vco_execution_service_start(void)
{
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE))
        return 0;
    state_lock = sceKernelCreateMutex("vco_exec_state", 0, 0, NULL);
    if (state_lock < 0)
        return -1;
    catalog_lock = sceKernelCreateMutex("vco_exec_catalog", 0, 0, NULL);
    if (catalog_lock < 0)
        goto failed;
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
    published.state = VCO_EXECUTION_LOADING;
    published.revision = 1;
    VcoTransportOps ops = {submit, resume, receive, acknowledge};
    if (!vco_transport_init(&transport, ops, NULL))
        goto failed;
    observer_thread = sceKernelCreateThread("vco_observe", observe_worker, 160, 8192, 0, 0, NULL);
    control_thread =
        sceKernelCreateThread("vco_execution", execution_worker, 165, 32768, 0, 0, NULL);
    if (observer_thread < 0 || control_thread < 0)
        goto failed;
    if (sceKernelStartThread(observer_thread, 0, NULL) < 0)
        goto failed;
    if (sceKernelStartThread(control_thread, 0, NULL) < 0) {
        __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
        sceKernelWaitThreadEnd(observer_thread, NULL, NULL);
        observer_thread = -1; /* ExitDeleteThread owns its ID. */
        goto failed;
    }
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    return 0;
failed:
    if (observer_thread >= 0)
        sceKernelDeleteThread(observer_thread);
    if (control_thread >= 0)
        sceKernelDeleteThread(control_thread);
    observer_thread = control_thread = -1;
    if (catalog_lock >= 0)
        sceKernelDeleteMutex(catalog_lock);
    sceKernelDeleteMutex(state_lock);
    state_lock = catalog_lock = -1;
    return -1;
}
int vco_execution_service_copy(VcoExecutionView *out)
{
    if (!out || !__atomic_load_n(&started, __ATOMIC_ACQUIRE) ||
        sceKernelTryLockMutex(state_lock, 1) < 0)
        return 0;
    int changed = out->revision != published.revision;
    if (changed)
        *out = published;
    sceKernelUnlockMutex(state_lock, 1);
    return changed;
}
int vco_execution_service_set(uint64_t session, uint64_t revision, unsigned entry, bool enabled)
{
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE))
        return -1;
    if (sceKernelTryLockMutex(state_lock, 1) < 0)
        return 0;
    int result = -1;
    if (session && revision && published.session == session &&
        published.list_revision == revision && entry < published.count && published.known[entry] &&
        (published.state == VCO_EXECUTION_READY ||
         (!enabled && published.state == VCO_EXECUTION_BACKGROUND)) &&
        !published.rows[entry].pending && published.rows[entry].state != VCO_ENTRY_UNSUPPORTED &&
        published.rows[entry].error != VCO_E_RESTORE) {
        result = 0;
        if (!pending[entry] && queue_count < VCO_ENGINE_QUEUE) {
            queue[(queue_head + queue_count++) % VCO_ENGINE_QUEUE] =
                (QueuedCommand){session, revision, entry, enabled};
            pending[entry] = published.queued[entry] = true;
            command_error = VCO_OK;
            published.error = VCO_OK;
            published.revision = next_revision(published.revision);
            result = 1;
        }
    }
    sceKernelUnlockMutex(state_lock, 1);
    return result;
}
int vco_execution_service_catalog(const VcoCatalog *catalog, const VcoCatalogWorkspace *work)
{
    if (!catalog || !catalog->revision || !__atomic_load_n(&started, __ATOMIC_ACQUIRE))
        return -1;
    sceKernelLockMutex(catalog_lock, 1, NULL);
    if (staged.generation == UINT64_MAX) {
        sceKernelUnlockMutex(catalog_lock, 1);
        return -1;
    }
    ++staged.generation;
    staged.catalog_revision = catalog->revision;
    staged.catalog_state = catalog->state;
    staged.count = catalog->count <= VCO_ENGINE_ENTRIES ? catalog->count : 0;
    staged.ready = false;
    staged.size = 0;
    staged.error = VCO_OK;
    if (work && catalog->state == VCO_LIST_READY && work->entry_count == catalog->count) {
        VcoCheatFile file = {(VcoCheatEntry *)work->entries,
                             VCO_CATALOG_ENTRIES,
                             work->entry_count,
                             (VcoCode *)work->codes,
                             VCO_CATALOG_CODES,
                             work->code_count,
                             (char *)work->names,
                             sizeof(work->names),
                             work->name_size};
        if (catalog->execution_session) {
            staged.error = vco_upload_encode(
                staged.packet, sizeof(staged.packet), 1, catalog->execution_session,
                catalog->revision, catalog->game.pid, catalog->game.title_id, &file, &staged.size);
            staged.ready = staged.error == VCO_OK;
        }
    } else if (work)
        staged.error = VCO_E_ARGUMENT;
    if (!staged.ready)
        staged.clear_generation = staged.generation;
    uint64_t generation = staged.generation;
    sceKernelLockMutex(state_lock, 1, NULL);
    uint32_t revision = next_revision(published.revision);
    memset(&published, 0, sizeof(published));
    published.revision = revision;
    published.catalog_revision = catalog->revision;
    published.count = catalog->count <= VCO_ENGINE_ENTRIES ? catalog->count : 0;
    published.state =
        catalog->state == VCO_LIST_NO_GAME ? VCO_EXECUTION_NO_GAME : VCO_EXECUTION_LOADING;
    published_generation = generation;
    reset_requests_locked();
    sceKernelUnlockMutex(state_lock, 1);
    sceKernelUnlockMutex(catalog_lock, 1);
    return 0;
}
