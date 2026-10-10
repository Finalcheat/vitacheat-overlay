#include "shell_owner.h"
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysroot.h>
#include <taihen.h>
#include <string.h>

int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address);

static SceClass *process_class(void)
{
    /* ProcessmgrForKernel on 3.60 and 3.63+; neither is a mandatory import. */
    const uint32_t libraries[] = {0x7A69DE86, 0xEB1F8EF7};
    const uint32_t functions[] = {0xC6820972, 0x98AE4BC8};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = 0;
        if (module_get_export_func(KERNEL_PID, "SceProcessmgr", libraries[i], functions[i],
                                   &address) >= 0 &&
            address)
            return ((SceClass * (*)(void)) address)();
    }
    return NULL;
}

static bool poisoned(const VcoVitaShellOwner *owner)
{
    return __atomic_load_n(&owner->poisoned, __ATOMIC_ACQUIRE) != 0;
}

static int poison(VcoVitaShellOwner *owner, int error)
{
    int empty = 0;
    __atomic_compare_exchange_n(&owner->cleanup_error, &empty, error, false, __ATOMIC_RELAXED,
                                __ATOMIC_RELAXED);
    __atomic_store_n(&owner->poisoned, 1, __ATOMIC_RELEASE);
    return -1;
}

void vco_vita_shell_owner_init(VcoVitaShellOwner *owner)
{
    if (!owner)
        return;
    memset(owner, 0, sizeof(*owner));
    owner->pid = -1;
}

int vco_vita_shell_owner_bind(VcoVitaShellOwner *owner)
{
    if (!owner || owner->bound || poisoned(owner))
        return -1;
    SceUID pid = ksceKernelSysrootGetShellPid();
    if (pid <= 0 || pid == KERNEL_PID)
        return -1;
    SceClass *cls = process_class();
    SceObjectBase *object = NULL;
    if (!cls || ksceGUIDReferObjectWithClass(pid, cls, &object) < 0)
        return -1;
    /* Shell can restart during lookup. Keep no binding to a departed Shell. */
    if (!object || ksceKernelSysrootGetShellPid() != pid) {
        int result = ksceGUIDReleaseObject(pid);
        return result < 0 ? poison(owner, result) : -1;
    }
    owner->pid = pid;
    owner->process_class = cls;
    owner->process_object = object;
    owner->bound = true;
    return 0;
}

int vco_vita_shell_owner_authorize(void *context, int32_t caller)
{
    VcoVitaShellOwner *owner = context;
    if (!owner || !owner->bound || poisoned(owner) || caller != owner->pid ||
        ksceKernelSysrootGetShellPid() != owner->pid)
        return -1;
    SceObjectBase *current = NULL;
    if (ksceGUIDReferObjectWithClass(caller, owner->process_class, &current) < 0)
        return -1;
    bool match = current && current == owner->process_object;
    int result = ksceGUIDReleaseObject(caller);
    if (result < 0)
        return poison(owner, result);
    /* Recheck after releasing the temporary reference; concurrent calls may
     * also have discovered an ambiguous release and poisoned this binding. */
    return match && ksceKernelSysrootGetShellPid() == owner->pid && !poisoned(owner) ? 0 : -1;
}

int vco_vita_shell_owner_unbind(VcoVitaShellOwner *owner)
{
    if (!owner)
        return -1;
    if (!owner->bound)
        return poisoned(owner) ? -1 : 0;
    int result = ksceGUIDReleaseObject(owner->pid);
    owner->bound = false;
    owner->pid = -1;
    owner->process_class = owner->process_object = NULL;
    /* Exactly once for the retained reference, even if release is ambiguous. */
    if (result < 0)
        return poison(owner, result);
    return poisoned(owner) ? -1 : 0;
}

int vco_vita_shell_owner_replacement(VcoVitaShellOwner *owner)
{
    if (!owner || !owner->bound || poisoned(owner))
        return -1;
    SceUID pid = ksceKernelSysrootGetShellPid();
    if (pid <= 0 || pid == KERNEL_PID)
        return -1;
    if (pid != owner->pid)
        return ksceKernelSysrootGetShellPid() == pid && !poisoned(owner) ? 1 : -1;
    SceObjectBase *current = NULL;
    if (ksceGUIDReferObjectWithClass(pid, owner->process_class, &current) < 0)
        return -1;
    int status = !current ? -1 : current == owner->process_object ? 0 : 1;
    int result = ksceGUIDReleaseObject(pid);
    if (result < 0)
        return poison(owner, result);
    return ksceKernelSysrootGetShellPid() == pid && !poisoned(owner) ? status : -1;
}
