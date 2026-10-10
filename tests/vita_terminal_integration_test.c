#include "platform/vita/authority.h"
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Real event capture and authority together; only SDK objects are substituted. */
static const SceProcEventHandler *handler;
static SceClass process_class;
static SceObjectBase process, replacement;
static int replaced, fail_title;
static unsigned refs;
static int exit_on_title;
static SceProcEventInvokeParam1 param = {.size = sizeof(param)};
static SceClass *get_class(void)
{
    return &process_class;
}
static int get_title(int32_t pid, char *out, uint32_t size)
{
    assert(pid == 42 && size == 32);
    memset(out, 0, size);
    memcpy(out, "PCSB00031", 10);
    if (exit_on_title) {
        exit_on_title = 0;
        assert(!handler->kill(pid, &param, 0));
    }
    return fail_title ? -17 : 0;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *out)
{
    assert(pid == KERNEL_PID);
    (void)library;
    (void)function;
    *out = !strcmp(module, "SceProcessmgr") ? (uintptr_t)get_class : (uintptr_t)get_title;
    return 0;
}
int64_t ksceKernelGetSystemTimeWide(void)
{
    return 1000000;
}
int ksceGUIDReferObjectWithClass(SceUID pid, SceClass *cls, SceObjectBase **out)
{
    assert(pid == 42 && cls == &process_class);
    ++refs;
    *out = replaced ? &replacement : &process;
    return 0;
}
int ksceGUIDReleaseObject(SceUID pid)
{
    assert(pid == 42 && refs);
    --refs;
    return 0;
}
SceUID ksceKernelRegisterProcEventHandler(const char *name, const SceProcEventHandler *h, int arg)
{
    assert(name && !arg);
    handler = h;
    return 77;
}
int ksceKernelUnregisterProcEventHandler(SceUID uid)
{
    assert(uid == 77);
    return 0;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(!vco_process_events_start());
    VcoVitaGameAuthority a;
    vco_vita_authority_init(&a);
    VcoForegroundSample sample = {.abi = VCO_FOREGROUND_PROBE_ABI,
                                  .size = sizeof(sample),
                                  .status = VCO_FOREGROUND_CANDIDATE,
                                  .active_app_id = 22,
                                  .game_app_id = 22,
                                  .active_pid = 42,
                                  .game_pid = 42,
                                  .game_title_id = "PCSB00031"};
    assert(!vco_vita_authority_accept(&a, &sample, sizeof(sample), 1000000,
                                      vco_process_events_invalidation(), 1));
    if (!strcmp(argv[1], "prepare")) {
        exit_on_title = 1;
        assert(vco_vita_authority_prepare(&a) < 0);
        assert(!refs && !a.held && !vco_process_events_allow(42));
    } else {
        assert(!vco_vita_authority_prepare(&a));
        VcoIdentity id = a.identity;
        id.instance = 1;
        assert(!vco_vita_authority_authorize(&a, &id, false));
        if (!strcmp(argv[1], "overflow") || !strcmp(argv[1], "retirement")) {
            for (unsigned i = 0; i < VCO_PROCESS_EVENTS_CAPACITY; ++i)
                assert(!handler->stop(43, 0, &param, 0));
            assert(!handler->exit(42, &param, 0));
            assert(vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST);
        } else if (!strcmp(argv[1], "during")) {
            exit_on_title = 1;
        } else {
            assert(!strcmp(argv[1], "queued"));
            assert(!handler->kill(42, &param, 0));
        }
        /* No poll or authority_event; object and title remain unchanged. */
        assert(vco_vita_authority_authorize(&a, &id, false) < 0);
        assert(!a.retired && a.held && refs == 1);
        if (!strcmp(argv[1], "retirement")) {
            /* A failed read is not retirement. Later replacement is independent
             * evidence even though the terminal record was lost to overflow. */
            fail_title = 1;
            assert(vco_vita_authority_authorize(&a, &id, false) < 0);
            assert(!a.retired && refs == 1);
            fail_title = 0;
            replaced = 1;
            assert(vco_vita_authority_authorize(&a, &id, false) < 0);
            assert(a.retired && refs == 1);
        }
        assert(!vco_vita_authority_unbind(&a));
        assert(!refs && !vco_process_events_allow(42));
    }
    assert(!vco_process_events_stop());
    puts("terminal callback/authority integration passed");
    return 0;
}
