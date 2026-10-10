#include "authority.h"
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include <string.h>

int module_get_export_func(SceUID, const char *, uint32_t, uint32_t, uintptr_t *);

static int poison(VcoVitaGameAuthority *a, int error)
{
    a->candidate = false;
    a->poisoned = true;
    if (!a->cleanup_error)
        a->cleanup_error = error;
    return -1;
}
static int clock_now(VcoVitaGameAuthority *a, uint64_t *out)
{
    int64_t raw = ksceKernelGetSystemTimeWide();
    if (raw < 0 || (a->clock_seen && (uint64_t)raw < a->last_clock)) {
        a->candidate = false;
        return -1;
    }
    a->clock_seen = true;
    a->last_clock = (uint64_t)raw;
    *out = (uint64_t)raw;
    return 0;
}
static int fresh(VcoVitaGameAuthority *a)
{
    uint64_t now;
    VcoProcessEventsState before, after;
    if (!a || !a->candidate || a->poisoned || clock_now(a, &now))
        return -1;
    vco_process_events_state(&before);
    uint32_t epoch = vco_process_events_invalidation();
    vco_process_events_state(&after);
    if (now < a->received_us || now - a->received_us >= VCO_VITA_AUTHORITY_LEASE_US ||
        before.phase != VCO_PROCESS_EVENTS_ACTIVE || after.phase != VCO_PROCESS_EVENTS_ACTIVE ||
        (epoch & VCO_PROCESS_EVENTS_LOST) || epoch != a->received_epoch ||
        vco_process_events_invalidation() != epoch) {
        a->candidate = false;
        return -1;
    }
    return 0;
}
static int title_valid(const char title[10], int32_t pid)
{
    VcoIdentity id = {.pid = pid, .instance = 1};
    memcpy(id.title_id, title, sizeof(id.title_id));
    return pid != KERNEL_PID && vco_identity_valid(&id);
}
static int same_target(const VcoVitaGameAuthority *a, const VcoIdentity *id)
{
    return a->sample.game_pid == id->pid &&
           !memcmp(a->sample.game_title_id, id->title_id, sizeof(id->title_id));
}
static int resolve(VcoVitaGameAuthority *a)
{
    if (!a->process_class) {
        const uint32_t libs[] = {0x7A69DE86, 0xEB1F8EF7};
        const uint32_t functions[] = {0xC6820972, 0x98AE4BC8};
        for (unsigned i = 0; i < 2; ++i) {
            uintptr_t address = 0;
            if (module_get_export_func(KERNEL_PID, "SceProcessmgr", libs[i], functions[i],
                                       &address) >= 0 &&
                address) {
                a->process_class = ((SceClass * (*)(void)) address)();
                break;
            }
        }
    }
    if (!a->get_title) {
        uintptr_t address = 0;
        /* SDK SceSysmem / SceSysrootForKernel mapping, not a user AppMgr
         * syscall called from kernel. Missing export denies preparation. */
        if (module_get_export_func(KERNEL_PID, "SceSysmem", 0x3691DA45, 0xEC3124A3, &address) >=
                0 &&
            address)
            a->get_title = (int (*)(int32_t, char *, uint32_t))address;
    }
    return a->process_class && a->get_title ? 0 : -1;
}
static int title_matches(VcoVitaGameAuthority *a, const VcoIdentity *id)
{
    char title[32];
    memset(title, 0xff, sizeof(title));
    if (!a->get_title || a->get_title(id->pid, title, sizeof(title)) < 0)
        return -1;
    return title_valid(title, id->pid) && !memcmp(title, id->title_id, sizeof(id->title_id)) ? 0
                                                                                             : 1;
}
static int verify_object(VcoVitaGameAuthority *a)
{
    if (!a->held || a->retired || a->poisoned)
        return -1;
    SceObjectBase *current = NULL;
    if (ksceGUIDReferObjectWithClass(a->identity.pid, a->process_class, &current) < 0) {
        a->candidate = false;
        return -1;
    }
    int title = -1;
    if (current && current == a->process_object)
        title = title_matches(a, &a->identity);
    else
        a->retired = true;
    int result = ksceGUIDReleaseObject(a->identity.pid);
    if (result < 0)
        return poison(a, result);
    if (title > 0)
        a->retired = true;
    if (a->retired || title < 0) {
        a->candidate = false;
        return -1;
    }
    return 0;
}
void vco_vita_authority_init(VcoVitaGameAuthority *a)
{
    if (a)
        memset(a, 0, sizeof(*a));
}
void vco_vita_authority_revoke(VcoVitaGameAuthority *a)
{
    if (a)
        a->candidate = false;
}
int vco_vita_authority_accept(VcoVitaGameAuthority *a, const void *bytes, size_t length,
                              uint64_t received, uint32_t epoch, uint64_t serial)
{
    if (!a)
        return -1;
    a->candidate = false;
    if (a->poisoned || !serial || serial <= a->serial)
        return -1;
    /* Consume new envelopes even when invalid. A malformed packet cannot be
     * resubmitted under the same serial later to extend its original lease. */
    a->serial = serial;
    if (!bytes || length != sizeof(VcoForegroundSample))
        return -1;
    VcoForegroundSample sample;
    memcpy(&sample, bytes, sizeof(sample));
    const uint8_t zero[sizeof(sample.reserved)] = {0};
    if (sample.abi != VCO_FOREGROUND_PROBE_ABI || sample.size != sizeof(sample) ||
        sample.status > VCO_BACKGROUND_CANDIDATE ||
        sample.failure > VCO_FOREGROUND_FAILURE_CHANGED ||
        memcmp(sample.reserved, zero, sizeof(zero)))
        return -1;
    if (sample.status == VCO_FOREGROUND_UNKNOWN) {
        const char empty_title[10] = {0};
        if (sample.active_app_id || sample.game_app_id || sample.active_pid || sample.game_pid ||
            memcmp(sample.game_title_id, empty_title, sizeof(empty_title)))
            return -1;
        a->sample = sample;
        return 0;
    }
    if (sample.failure || sample.active_app_id <= 0 || sample.game_app_id <= 0 ||
        sample.active_pid <= 0 || sample.active_pid == KERNEL_PID ||
        !title_valid(sample.game_title_id, sample.game_pid))
        return -1;
    bool same_app = sample.active_app_id == sample.game_app_id;
    if (same_app != (sample.active_pid == sample.game_pid) ||
        same_app != (sample.status == VCO_FOREGROUND_CANDIDATE))
        return -1;
    a->sample = sample;
    a->received_us = received;
    a->received_epoch = epoch;
    a->candidate = true;
    return fresh(a);
}
int vco_vita_authority_target(VcoVitaGameAuthority *a, int32_t *pid, char title[10])
{
    if (!pid || !title || fresh(a))
        return -1;
    *pid = a->sample.game_pid;
    memcpy(title, a->sample.game_title_id, 10);
    return 0;
}
int vco_vita_authority_prepare(VcoVitaGameAuthority *a)
{
    if (!a || a->held || a->poisoned || fresh(a) || resolve(a))
        return -1;
    VcoIdentity target = {.pid = a->sample.game_pid};
    memcpy(target.title_id, a->sample.game_title_id, sizeof(target.title_id));
    SceObjectBase *object = NULL;
    if (ksceGUIDReferObjectWithClass(target.pid, a->process_class, &object) < 0)
        return -1;
    if (!object || title_matches(a, &target) || fresh(a)) {
        int result = ksceGUIDReleaseObject(target.pid);
        a->candidate = false;
        return result < 0 ? poison(a, result) : -1;
    }
    if (vco_process_events_watch(target.pid, a->received_epoch)) {
        int result = ksceGUIDReleaseObject(target.pid);
        a->candidate = false;
        return result < 0 ? poison(a, result) : -1;
    }
    a->process_object = object;
    a->identity = target;
    a->bound_epoch = a->received_epoch;
    a->retired = false;
    a->held = true;
    return 0;
}
int vco_vita_authority_authorize(void *context, const VcoIdentity *id, bool foreground)
{
    VcoVitaGameAuthority *a = context;
    if (!a || !id || !vco_identity_valid(id) || !a->held || a->retired || a->poisoned ||
        id->pid != a->identity.pid || memcmp(id->title_id, a->identity.title_id, 10) ||
        (a->identity.instance && id->instance != a->identity.instance))
        return -1;
    bool binding = !a->identity.instance;
    if ((binding && foreground) ||
        ((foreground || binding) && (fresh(a) || !same_target(a, id) ||
                                     (foreground && a->sample.status != VCO_FOREGROUND_CANDIDATE))))
        return -1;
    /* This callback-side latch also covers background cleanup, which does not
     * require a foreground lease. Keep retirement evidence independent: a
     * revoked watch denies access but does not prove taiHEN cleanup finished. */
    if (!vco_process_events_allow(id->pid)) {
        /* A lost terminal queue entry must not prevent the service's existing
         * retained-object retirement probe. Verification only queries identity
         * and balances its temporary reference; it never authorizes access. */
        (void)verify_object(a);
        return -1;
    }
    if (verify_object(a) || !vco_process_events_allow(id->pid))
        return -1;
    if ((foreground || binding) && fresh(a))
        return -1;
    if (binding)
        a->identity.instance = id->instance;
    return 0;
}
VcoObservation vco_vita_authority_observation(VcoVitaGameAuthority *a)
{
    if (!a || !a->held || !a->identity.instance || fresh(a) || !same_target(a, &a->identity))
        return VCO_OBSERVATION_UNKNOWN;
    bool foreground = a->sample.status == VCO_FOREGROUND_CANDIDATE;
    if (vco_vita_authority_authorize(a, &a->identity, foreground) || fresh(a))
        return VCO_OBSERVATION_UNKNOWN;
    return foreground ? VCO_OBSERVATION_FOREGROUND : VCO_OBSERVATION_BACKGROUND;
}
void vco_vita_authority_event(VcoVitaGameAuthority *a, const VcoProcessEvent *event)
{
    if (!a || !event || !a->held || event->pid != a->identity.pid)
        return;
    a->candidate = false;
    if (event->kind != VCO_PROCESS_EVENT_EXIT && event->kind != VCO_PROCESS_EVENT_KILL)
        return;
    /* Queue records may be delayed/out of order. Only a terminal event known
     * to follow this retained binding retires it directly. For older/LOST
     * records, independent object/title verification detects replacements. */
    if (!(event->invalidation & VCO_PROCESS_EVENTS_LOST) && event->invalidation > a->bound_epoch)
        a->retired = true;
    else
        (void)verify_object(a);
}
int vco_vita_authority_unbind(VcoVitaGameAuthority *a)
{
    if (!a)
        return -1;
    if (!a->held)
        return a->poisoned ? -1 : 0;
    int result = ksceGUIDReleaseObject(a->identity.pid);
    vco_process_events_unwatch(a->identity.pid);
    a->held = false;
    a->process_object = NULL;
    memset(&a->identity, 0, sizeof(a->identity));
    a->retired = false;
    a->bound_epoch = 0;
    if (result < 0)
        return poison(a, result);
    return a->poisoned ? -1 : 0;
}
