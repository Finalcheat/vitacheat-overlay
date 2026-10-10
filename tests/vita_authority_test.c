#include "platform/vita/authority.h"
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static SceClass process_class;
static SceObjectBase originals[2], replacement;
static SceObjectBase *objects[2];
static char titles[2][32];
static int refs[2], refers, releases, title_calls;
static int firmware, missing_class, missing_title, null_class, zero_export;
static int fail_refer, fail_release, fail_title, epoch_on_title, expire_on_refer;
static int64_t now;
static uint32_t epoch, phase;
static int32_t watched_pid;
static int terminal_seen, terminal_on_title;
static int index_of(int32_t pid)
{
    assert(pid == 42 || pid == 43);
    return pid - 42;
}
static SceClass *get_class(void)
{
    return null_class ? NULL : &process_class;
}
static int get_title(int32_t pid, char *out, uint32_t length)
{
    assert(length == 32);
    ++title_calls;
    if (terminal_on_title) {
        terminal_on_title = 0;
        terminal_seen = 1;
    }
    memcpy(out, titles[index_of(pid)], length);
    if (epoch_on_title) {
        epoch_on_title = 0;
        ++epoch;
    }
    return fail_title ? -17 : 0;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address)
{
    assert(pid == KERNEL_PID);
    *address = 0;
    if (!strcmp(module, "SceProcessmgr")) {
        assert((library == 0x7A69DE86 && function == 0xC6820972) ||
               (library == 0xEB1F8EF7 && function == 0x98AE4BC8));
        if (missing_class || (firmware ? library != 0xEB1F8EF7 : library != 0x7A69DE86))
            return -1;
        if (!zero_export)
            *address = (uintptr_t)get_class;
        return 0;
    }
    assert(!strcmp(module, "SceSysmem") && library == 0x3691DA45 && function == 0xEC3124A3);
    if (missing_title)
        return -1;
    if (!zero_export)
        *address = (uintptr_t)get_title;
    return 0;
}
int ksceGUIDReferObjectWithClass(SceUID pid, SceClass *cls, SceObjectBase **out)
{
    assert(cls == &process_class);
    ++refers;
    if (fail_refer)
        return -27;
    ++refs[index_of(pid)];
    *out = objects[index_of(pid)];
    if (expire_on_refer) {
        expire_on_refer = 0;
        now += VCO_VITA_AUTHORITY_LEASE_US;
    }
    return 0;
}
int ksceGUIDReleaseObject(SceUID pid)
{
    ++releases;
    assert(refs[index_of(pid)] > 0);
    --refs[index_of(pid)]; /* Even failure may have consumed this reference. */
    return fail_release ? -37 : 0;
}
int64_t ksceKernelGetSystemTimeWide(void)
{
    return now;
}
uint32_t vco_process_events_invalidation(void)
{
    return epoch;
}
void vco_process_events_state(VcoProcessEventsState *out)
{
    *out = (VcoProcessEventsState){phase, epoch, 0, 0};
}
int vco_process_events_watch(int32_t pid, uint32_t expected)
{
    if (watched_pid || expected != epoch || (epoch & VCO_PROCESS_EVENTS_LOST) ||
        phase != VCO_PROCESS_EVENTS_ACTIVE)
        return -1;
    watched_pid = pid;
    terminal_seen = 0;
    return 0;
}
void vco_process_events_unwatch(int32_t pid)
{
    if (pid == watched_pid)
        watched_pid = 0;
}
int vco_process_events_allow(int32_t pid)
{
    return pid == watched_pid && !terminal_seen && phase == VCO_PROCESS_EVENTS_ACTIVE;
}
static void reset(void)
{
    assert(!refs[0] && !refs[1] && !watched_pid);
    objects[0] = &originals[0];
    objects[1] = &originals[1];
    memset(titles, 0, sizeof(titles));
    memcpy(titles[0], "PCSB00031", 10);
    memcpy(titles[1], "PCSB00032", 10);
    refers = releases = title_calls = 0;
    firmware = missing_class = missing_title = null_class = zero_export = 0;
    fail_refer = fail_release = fail_title = epoch_on_title = expire_on_refer = 0;
    terminal_seen = terminal_on_title = 0;
    now = 1000000;
    epoch = 7;
    phase = VCO_PROCESS_EVENTS_ACTIVE;
}
static VcoForegroundSample sample(int32_t pid, bool foreground)
{
    VcoForegroundSample s = {.abi = VCO_FOREGROUND_PROBE_ABI,
                             .size = sizeof(s),
                             .status =
                                 foreground ? VCO_FOREGROUND_CANDIDATE : VCO_BACKGROUND_CANDIDATE,
                             .active_app_id = foreground ? 22 : 11,
                             .game_app_id = 22,
                             .active_pid = foreground ? pid : 44,
                             .game_pid = pid};
    memcpy(s.game_title_id, titles[index_of(pid)], sizeof(s.game_title_id));
    return s;
}
static void accept(VcoVitaGameAuthority *a, VcoForegroundSample s, uint64_t serial)
{
    assert(!vco_vita_authority_accept(a, &s, sizeof(s), (uint64_t)now, epoch, serial));
}
static VcoIdentity bind(VcoVitaGameAuthority *a, int32_t pid, uint64_t serial)
{
    accept(a, sample(pid, true), serial);
    assert(!vco_vita_authority_prepare(a));
    assert(a->held && !a->identity.instance && refs[index_of(pid)] == 1);
    VcoIdentity id = a->identity;
    id.instance = serial + 100; /* Exactly the memory adapter's callback contract. */
    assert(!vco_vita_authority_authorize(a, &id, false));
    assert(a->identity.instance == id.instance && refs[index_of(pid)] == 1);
    return id;
}
static void binding(void)
{
    for (int fw = 0; fw < 2; ++fw) {
        reset();
        firmware = fw;
        VcoVitaGameAuthority a;
        vco_vita_authority_init(&a);
        VcoIdentity id = bind(&a, 42, 1);
        assert(vco_vita_authority_prepare(&a) < 0);
        assert(!vco_vita_authority_authorize(&a, &id, true));
        assert(vco_vita_authority_observation(&a) == VCO_OBSERVATION_FOREGROUND);
        int32_t pid = -1;
        char title[10] = {0};
        assert(!vco_vita_authority_target(&a, &pid, title));
        assert(pid == 42 && !strcmp(title, "PCSB00031"));
        VcoIdentity wrong = id;
        ++wrong.instance;
        assert(vco_vita_authority_authorize(&a, &wrong, false) < 0);
        wrong = id;
        ++wrong.pid;
        assert(vco_vita_authority_authorize(&a, &wrong, false) < 0);
        assert(!vco_vita_authority_unbind(&a));
        assert(!a.held && !a.identity.instance && !refs[0]);
        assert(!vco_vita_authority_unbind(&a));
    }
    reset();
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    accept(&a, sample(42, true), 1);
    assert(!vco_vita_authority_prepare(&a));
    VcoIdentity id = a.identity;
    id.instance = 1;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0); /* not committed */
    assert(!a.identity.instance);
    assert(!vco_vita_authority_unbind(&a));
}
static void transitions(void)
{
    reset();
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    VcoIdentity id = bind(&a, 42, 1);
    accept(&a, sample(42, false), 2);
    assert(vco_vita_authority_observation(&a) == VCO_OBSERVATION_BACKGROUND);
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    VcoForegroundSample unknown = {.abi = VCO_FOREGROUND_PROBE_ABI,
                                   .size = sizeof(unknown),
                                   .failure = VCO_FOREGROUND_FAILURE_QUERY};
    accept(&a, unknown, 3);
    assert(vco_vita_authority_observation(&a) == VCO_OBSERVATION_UNKNOWN);
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    accept(&a, sample(42, true), 4);
    assert(!vco_vita_authority_authorize(&a, &id, true));
    vco_vita_authority_revoke(&a);
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    /* New target invalidates old writes, while the old live object can be
     * cleaned before its references are released. Latest receipt is retained. */
    accept(&a, sample(43, true), 5);
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    uint64_t receipt = a.received_us;
    assert(!vco_vita_authority_unbind(&a));
    assert(!vco_vita_authority_prepare(&a) && a.identity.pid == 43);
    assert(a.serial == 5 && a.received_us == receipt);
    VcoIdentity second = a.identity;
    second.instance = 102;
    assert(!vco_vita_authority_authorize(&a, &second, false));
    assert(!vco_vita_authority_authorize(&a, &second, true));
    assert(vco_vita_authority_authorize(&a, &id, false) < 0);
    assert(!vco_vita_authority_unbind(&a));
}
static void leases(void)
{
    reset();
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    VcoIdentity id = bind(&a, 42, 1);
    VcoForegroundSample s = sample(42, true);
    uint64_t original = (uint64_t)now;
    now += VCO_VITA_AUTHORITY_LEASE_US - 1;
    assert(!vco_vita_authority_authorize(&a, &id, true));
    ++now;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    assert(vco_vita_authority_accept(&a, &s, sizeof(s), original, epoch, 2) < 0);
    assert(!a.candidate); /* An old queued sample is not timestamped on processing. */
    accept(&a, s, 3);
    now += 100;
    assert(vco_vita_authority_accept(&a, &s, sizeof(s), (uint64_t)now, epoch, 3) < 0);
    assert(!a.candidate); /* Replay does not extend the valid observation. */
    assert(vco_vita_authority_accept(&a, &s, sizeof(s), (uint64_t)now + 1, epoch, 4) < 0);
    accept(&a, s, 5);
    --now;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    now += 2;
    accept(&a, s, 6);
    now = -1;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    assert(!vco_vita_authority_unbind(&a));
}
static void packets(void)
{
    reset();
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    VcoForegroundSample good = sample(42, true);
    for (unsigned field = 0; field < 14; ++field) {
        accept(&a, good, 2 * field + 1);
        VcoForegroundSample bad = good;
        switch (field) {
        case 0:
            ++bad.abi;
            break;
        case 1:
            --bad.size;
            break;
        case 2:
            bad.status = 3;
            break;
        case 3:
            bad.failure = VCO_FOREGROUND_FAILURE_QUERY;
            break;
        case 4:
            bad.reserved[5] = 1;
            break;
        case 5:
            bad.active_app_id = 0;
            break;
        case 6:
            bad.game_app_id = -1;
            break;
        case 7:
            bad.active_pid = 0;
            break;
        case 8:
            bad.game_pid = KERNEL_PID;
            break;
        case 9:
            memcpy(bad.game_title_id, "NPXS10001", 10);
            break;
        case 10:
            bad.game_title_id[9] = 'X';
            break;
        case 11:
            bad.game_title_id[0] = '/';
            break;
        case 12:
            bad.active_app_id = 11;
            break;
        default:
            bad.status = VCO_BACKGROUND_CANDIDATE;
            break;
        }
        assert(vco_vita_authority_accept(&a, &bad, sizeof(bad), (uint64_t)now, epoch,
                                         2 * field + 2) < 0);
        assert(!a.candidate);
    }
    assert(vco_vita_authority_accept(&a, &good, sizeof(good) - 1, (uint64_t)now, epoch, 29) < 0);
    assert(vco_vita_authority_accept(&a, NULL, sizeof(good), (uint64_t)now, epoch, 30) < 0);
    VcoForegroundSample bad_unknown = good;
    bad_unknown.status = VCO_FOREGROUND_UNKNOWN;
    assert(vco_vita_authority_accept(&a, &bad_unknown, sizeof(bad_unknown), (uint64_t)now, epoch,
                                     31) < 0);
    assert(vco_vita_authority_accept(&a, &good, sizeof(good), (uint64_t)now, epoch, 0) < 0);
    vco_vita_authority_init(NULL);
    vco_vita_authority_revoke(NULL);
    vco_vita_authority_event(NULL, NULL);
    assert(vco_vita_authority_accept(NULL, &good, sizeof(good), (uint64_t)now, epoch, 1) < 0);
    assert(vco_vita_authority_prepare(NULL) < 0 && vco_vita_authority_unbind(NULL) < 0);
    assert(vco_vita_authority_observation(NULL) == VCO_OBSERVATION_UNKNOWN);
}
static void events(void)
{
    reset();
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    VcoIdentity id = bind(&a, 42, 1);
    ++epoch;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    accept(&a, sample(42, true), 2);
    VcoProcessEvent old = {
        .invalidation = a.bound_epoch, .kind = VCO_PROCESS_EVENT_EXIT, .pid = 42};
    vco_vita_authority_event(&a, &old);
    assert(!a.candidate && !a.retired);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    accept(&a, sample(42, true), 3);
    VcoProcessEvent unrelated = {
        .invalidation = ++epoch, .kind = VCO_PROCESS_EVENT_EXIT, .pid = 43};
    vco_vita_authority_event(&a, &unrelated);
    assert(!a.retired);
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    accept(&a, sample(42, true), 4);
    VcoProcessEvent terminal = {.invalidation = ++epoch, .kind = VCO_PROCESS_EVENT_KILL, .pid = 42};
    vco_vita_authority_event(&a, &terminal);
    assert(a.retired && !a.candidate);
    assert(vco_vita_authority_authorize(&a, &id, false) < 0);
    assert(!vco_vita_authority_unbind(&a));
    reset();
    vco_vita_authority_init(&a);
    id = bind(&a, 42, 1);
    epoch |= VCO_PROCESS_EVENTS_LOST;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_authorize(&a, &id, false));
    VcoForegroundSample s = sample(42, true);
    assert(vco_vita_authority_accept(&a, &s, sizeof(s), (uint64_t)now, epoch, 2) < 0);
    assert(!vco_vita_authority_unbind(&a));
    reset();
    vco_vita_authority_init(&a);
    id = bind(&a, 42, 1);
    phase = VCO_PROCESS_EVENTS_STOPPED;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    assert(!vco_vita_authority_unbind(&a));
}
static void replacements(void)
{
    reset();
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    VcoIdentity id = bind(&a, 42, 1);
    objects[0] = &replacement; /* Same PID and Title ID, different object. */
    assert(vco_vita_authority_authorize(&a, &id, false) < 0 && a.retired);
    objects[0] = &originals[0];
    assert(vco_vita_authority_authorize(&a, &id, false) < 0); /* no resurrection */
    assert(!vco_vita_authority_unbind(&a));
    reset();
    vco_vita_authority_init(&a);
    id = bind(&a, 42, 1);
    memcpy(titles[0], "PCSB00099", 10);
    assert(vco_vita_authority_authorize(&a, &id, true) < 0 && a.retired);
    assert(!vco_vita_authority_unbind(&a));
}
static void faults(void)
{
    for (unsigned fail = 0; fail < 9; ++fail) {
        reset();
        VcoVitaGameAuthority a;
        vco_vita_authority_init(&a);
        accept(&a, sample(42, true), 1);
        if (fail == 0)
            missing_class = 1;
        if (fail == 1)
            missing_title = 1;
        if (fail == 2)
            zero_export = 1;
        if (fail == 3)
            null_class = 1;
        if (fail == 4)
            fail_refer = 1;
        if (fail == 5)
            objects[0] = NULL;
        if (fail == 6)
            fail_title = 1;
        if (fail == 7)
            epoch_on_title = 1;
        if (fail == 8)
            expire_on_refer = 1;
        assert(vco_vita_authority_prepare(&a) < 0 && !a.held && !refs[0]);
    }
    reset();
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    accept(&a, sample(42, true), 1);
    fail_title = fail_release = 1;
    assert(vco_vita_authority_prepare(&a) < 0);
    assert(a.poisoned && a.cleanup_error == -37 && !a.held && !refs[0]);
    int released = releases;
    assert(vco_vita_authority_unbind(&a) < 0 && releases == released);
    reset();
    vco_vita_authority_init(&a);
    VcoIdentity id = bind(&a, 42, 1);
    epoch_on_title = 1;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0);
    accept(&a, sample(42, true), 2);
    fail_release = 1;
    assert(vco_vita_authority_authorize(&a, &id, true) < 0 && a.poisoned);
    assert(refs[0] == 1);
    fail_release = 0;
    assert(vco_vita_authority_unbind(&a) < 0 && !refs[0]);
    released = releases;
    assert(vco_vita_authority_unbind(&a) < 0 && releases == released);
    reset();
    vco_vita_authority_init(&a);
    id = bind(&a, 42, 1);
    fail_refer = 1;
    assert(vco_vita_authority_authorize(&a, &id, false) < 0);
    assert(!a.candidate && !a.poisoned && refs[0] == 1);
    fail_refer = 0;
    fail_release = 1;
    assert(vco_vita_authority_unbind(&a) < 0 && a.poisoned && !a.held && !refs[0]);
    released = releases;
    assert(vco_vita_authority_unbind(&a) < 0 && releases == released);
}
static void terminal_latch(void)
{
    for (unsigned during = 0; during < 2; ++during) {
        reset();
        VcoVitaGameAuthority a;
        vco_vita_authority_init(&a);
        VcoIdentity id = bind(&a, 42, 1);
        accept(&a, sample(42, false), 2);
        assert(!vco_vita_authority_authorize(&a, &id, false));
        if (during)
            terminal_on_title = 1;
        else
            terminal_seen = 1;
        /* No event has been dequeued and retained object/title still match. */
        assert(vco_vita_authority_authorize(&a, &id, false) < 0);
        assert(!a.retired && a.held && refs[0] == 1);
        assert(vco_vita_authority_authorize(&a, &id, true) < 0);
        assert(!vco_vita_authority_unbind(&a));
        assert(!watched_pid && !refs[0]);
        id = bind(&a, 43, 3);
        assert(!vco_vita_authority_authorize(&a, &id, false));
        assert(!vco_vita_authority_unbind(&a));
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "terminal_latch"))
        terminal_latch();
    else if (!strcmp(argv[1], "binding"))
        binding();
    else if (!strcmp(argv[1], "transitions"))
        transitions();
    else if (!strcmp(argv[1], "leases"))
        leases();
    else if (!strcmp(argv[1], "packets"))
        packets();
    else if (!strcmp(argv[1], "events"))
        events();
    else if (!strcmp(argv[1], "replacements"))
        replacements();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else
        return 1;
    assert(!refs[0] && !refs[1]);
    puts("native authority tests passed");
    return 0;
}
