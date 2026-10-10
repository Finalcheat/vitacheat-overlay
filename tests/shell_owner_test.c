#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include "platform/vita/shell_owner.h"

static SceClass cls;
static SceObjectBase original, replacement;
static SceObjectBase *current;
static int32_t shell_pid;
static int firmware, lookups, references, refers, releases;
static bool zero_export, null_class, fail_refer, fail_release, release_consumes;
static bool restart_on_refer, restart_on_release;
static unsigned shell_reads, change_on_shell_read;

static SceClass *get_class(void)
{
    return null_class ? NULL : &cls;
}

int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address)
{
    const uint32_t libraries[] = {0x7A69DE86, 0xEB1F8EF7};
    const uint32_t functions[] = {0xC6820972, 0x98AE4BC8};
    assert(pid == KERNEL_PID && !strcmp(module, "SceProcessmgr"));
    int index = lookups++;
    assert(index >= 0 && index < 2);
    assert(library == libraries[index] && function == functions[index]);
    if (index != firmware)
        return -1;
    *address = zero_export ? 0 : (uintptr_t)get_class;
    return 0;
}

SceUID ksceKernelSysrootGetShellPid(void)
{
    if (++shell_reads == change_on_shell_read)
        ++shell_pid;
    return shell_pid;
}

int ksceGUIDReferObjectWithClass(SceUID pid, SceClass *type, SceObjectBase **out)
{
    assert(pid == 42 && type == &cls);
    ++refers;
    if (fail_refer)
        return -1;
    ++references;
    *out = current;
    if (restart_on_refer)
        shell_pid = 43;
    return 0;
}

int ksceGUIDReleaseObject(SceUID pid)
{
    assert(pid == 42 && references > 0);
    ++releases;
    if (!fail_release || release_consumes)
        --references;
    if (restart_on_release)
        shell_pid = 43;
    return fail_release ? -27 : 0;
}

static void reset(void)
{
    assert(!references);
    shell_pid = 42;
    firmware = lookups = refers = releases = 0;
    current = &original;
    zero_export = null_class = fail_refer = fail_release = false;
    restart_on_refer = restart_on_release = false;
    release_consumes = true;
    shell_reads = change_on_shell_read = 0;
}

static void bind(VcoVitaShellOwner *owner)
{
    vco_vita_shell_owner_init(owner);
    assert(!vco_vita_shell_owner_bind(owner));
    assert(owner->bound && owner->pid == 42 && references == 1);
    assert(owner->process_object == &original && owner->process_class == &cls);
}

static void binding(void)
{
    VcoVitaShellOwner owner;
    assert(vco_vita_shell_owner_bind(NULL) < 0);
    assert(vco_vita_shell_owner_authorize(NULL, 42) < 0);
    assert(vco_vita_shell_owner_unbind(NULL) < 0);
    for (int fw = 0; fw < 2; ++fw) {
        reset();
        firmware = fw;
        bind(&owner);
        assert(lookups == fw + 1);
        assert(vco_vita_shell_owner_bind(&owner) < 0);
        assert(!vco_vita_shell_owner_authorize(&owner, 42));
        assert(references == 1);
        assert(!vco_vita_shell_owner_unbind(&owner));
        assert(!owner.bound && !owner.process_object && owner.pid == -1);
        assert(!vco_vita_shell_owner_unbind(&owner) && releases == 2);
        assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
        /* A clean lifecycle permits another bind, without a new init. */
        lookups = 0;
        assert(!vco_vita_shell_owner_bind(&owner));
        assert(!vco_vita_shell_owner_unbind(&owner));
    }
    const int invalid[] = {-1, 0, KERNEL_PID};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset();
        shell_pid = invalid[i];
        vco_vita_shell_owner_init(&owner);
        assert(vco_vita_shell_owner_bind(&owner) < 0);
        assert(!lookups && !refers && !owner.bound);
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset();
        vco_vita_shell_owner_init(&owner);
        if (failure == 0)
            firmware = 2;
        else if (failure == 1)
            zero_export = true;
        else if (failure == 2)
            null_class = true;
        else if (failure == 3)
            fail_refer = true;
        else
            current = NULL;
        assert(vco_vita_shell_owner_bind(&owner) < 0);
        assert(!owner.bound && !owner.poisoned && !references);
        assert(!vco_vita_shell_owner_unbind(&owner));
    }
    reset();
    restart_on_refer = true;
    vco_vita_shell_owner_init(&owner);
    assert(vco_vita_shell_owner_bind(&owner) < 0);
    assert(!references && !owner.bound && releases == 1);
}

static void authorization(void)
{
    VcoVitaShellOwner owner;
    reset();
    vco_vita_shell_owner_init(&owner);
    assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
    assert(vco_vita_shell_owner_authorize(&owner, 123) < 0);
    assert(!refers); /* First caller never acquires ownership. */
    assert(!vco_vita_shell_owner_bind(&owner));
    assert(vco_vita_shell_owner_authorize(&owner, 123) < 0);
    assert(vco_vita_shell_owner_authorize(&owner, KERNEL_PID) < 0);
    assert(refers == 1 && owner.pid == 42);
    assert(!vco_vita_shell_owner_authorize(&owner, 42));
    shell_pid = 43;
    assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
    assert(vco_vita_shell_owner_authorize(&owner, 43) < 0);
    shell_pid = 42;
    current = &replacement; /* A recycled PID must fail the object comparison. */
    assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
    assert(references == 1 && !owner.poisoned);
    current = NULL;
    assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
    assert(references == 1);
    current = &original;
    fail_refer = true;
    int old_releases = releases;
    assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
    assert(releases == old_releases && references == 1);
    fail_refer = false;
    restart_on_refer = true;
    assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
    assert(references == 1);
    restart_on_refer = false;
    shell_pid = 42;
    restart_on_release = true;
    assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
    assert(references == 1);
    assert(!vco_vita_shell_owner_unbind(&owner) && !references);
}

static void faults(void)
{
    VcoVitaShellOwner owner;
    for (unsigned consumed = 0; consumed < 2; ++consumed) {
        for (unsigned boundary = 0; boundary < 3; ++boundary) {
            reset();
            vco_vita_shell_owner_init(&owner);
            if (boundary > 0)
                assert(!vco_vita_shell_owner_bind(&owner));
            fail_release = true;
            release_consumes = consumed;
            if (boundary == 0) {
                restart_on_refer = true;
                assert(vco_vita_shell_owner_bind(&owner) < 0);
            } else if (boundary == 1) {
                assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
            } else {
                assert(vco_vita_shell_owner_unbind(&owner) < 0);
            }
            assert(owner.poisoned && owner.cleanup_error == -27);
            int old_refers = refers, old_releases = releases;
            fail_release = restart_on_refer = false;
            shell_pid = 42;
            assert(vco_vita_shell_owner_authorize(&owner, 42) < 0);
            assert(refers == old_refers && releases == old_releases);
            assert(vco_vita_shell_owner_bind(&owner) < 0);
            /* Only the separately retained bind reference may be released.
             * Never retry the temporary/failed bind/failed unbind release. */
            assert(vco_vita_shell_owner_unbind(&owner) < 0);
            assert(releases == old_releases + (boundary == 1 ? 1 : 0));
            old_releases = releases;
            assert(vco_vita_shell_owner_unbind(&owner) < 0);
            assert(releases == old_releases && references == (consumed ? 0 : 1));
            assert(vco_vita_shell_owner_bind(&owner) < 0 && owner.cleanup_error == -27);
            /* Simulated kernel shutdown of an intentionally quarantined
             * context. Production must retain it after any cleanup error. */
            references = 0;
        }
    }
}

static void replacement_test(void)
{
    VcoVitaShellOwner owner;
    assert(vco_vita_shell_owner_replacement(NULL) < 0);
    bind(&owner);
    assert(!vco_vita_shell_owner_replacement(&owner) && references == 1);
    current = &replacement;
    assert(vco_vita_shell_owner_replacement(&owner) == 1 && references == 1);
    assert(owner.bound && owner.process_object == &original);
    current = NULL;
    assert(vco_vita_shell_owner_replacement(&owner) < 0 && references == 1);
    current = &original;
    fail_refer = true;
    int old_releases = releases;
    assert(vco_vita_shell_owner_replacement(&owner) < 0 && releases == old_releases);
    fail_refer = false;
    shell_pid = 0;
    assert(vco_vita_shell_owner_replacement(&owner) < 0);
    shell_pid = 43;
    int old_refers = refers;
    assert(vco_vita_shell_owner_replacement(&owner) == 1 && refers == old_refers);
    change_on_shell_read = shell_reads + 2;
    assert(vco_vita_shell_owner_replacement(&owner) < 0);
    shell_pid = 42;
    change_on_shell_read = 0;
    restart_on_refer = true;
    assert(vco_vita_shell_owner_replacement(&owner) < 0 && references == 1);
    shell_pid = 42;
    restart_on_refer = false;
    fail_release = true;
    assert(vco_vita_shell_owner_replacement(&owner) < 0 && owner.poisoned && references == 1);
    fail_release = false;
    assert(vco_vita_shell_owner_replacement(&owner) < 0);
    assert(vco_vita_shell_owner_unbind(&owner) < 0 && !references);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    reset();
    if (!strcmp(argv[1], "binding"))
        binding();
    else if (!strcmp(argv[1], "authorization"))
        authorization();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else if (!strcmp(argv[1], "replacement"))
        replacement_test();
    else
        assert(0);
    assert(!references);
    return 0;
}
