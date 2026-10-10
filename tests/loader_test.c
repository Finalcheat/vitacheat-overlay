#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>
static struct {
    unsigned delays, loads, starts, exports;
    int ready_after, missing, start_error, load_error, delay_error, exits, exit_status;
    int (*entry)(SceSize, void *);
} fake;
int module_start(SceSize, const void *);
int sceKernelDelayThread(unsigned time)
{
    assert(time == 250000);
    ++fake.delays;
    return fake.delay_error;
}
int sceKernelExitDeleteThread(int result)
{
    ++fake.exits;
    fake.exit_status = result;
    return 0;
}
int sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int priority,
                          unsigned stack, unsigned a, unsigned b, void *option)
{
    assert(!strcmp(name, "vco_loader") && priority > 0 && stack >= 16384 && !a && !b && !option);
    fake.entry = entry;
    return 7;
}
int sceKernelStartThread(int id, unsigned length, void *argument)
{
    assert(id == 7 && !length && !argument);
    return fake.entry(length, argument);
}
int sceKernelDeleteThread(int id)
{
    assert(id == 7);
    return 0;
}
int taiGetModuleInfo(const char *name, tai_module_info_t *info)
{
    assert(!strcmp(name, "ScePaf") && info->size == sizeof(*info));
    return (int)fake.delays >= fake.ready_after ? 0 : -1;
}
int taiGetModuleExportFunc(const char *name, unsigned library, unsigned function, uintptr_t *out)
{
    assert(!strcmp(name, "ScePaf") && library == 1 && (function == 11 || function == 12));
    ++fake.exports;
    if (fake.missing && function == 12)
        return -1;
    *out = 0x81000001;
    return 0;
}
int sceKernelLoadModule(const char *path, int flags, void *options)
{
    assert(!strcmp(path, "ux0:vitacheat/VitaCheatOverlayUI.suprx") && !flags && !options);
    ++fake.loads;
    return fake.load_error ? fake.load_error : 8;
}
int sceKernelStartModule(int module, unsigned size, void *args, int flags, void *options,
                         int *status)
{
    assert(module == 8 && !size && !args && !flags && !options);
    ++fake.starts;
    *status = fake.start_error;
    return fake.start_error;
}
int sceIoOpen(const char *path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    return -1;
}
int sceIoWrite(int fd, const void *data, unsigned length)
{
    (void)fd;
    (void)data;
    return (int)length;
}
int sceIoClose(int fd)
{
    (void)fd;
    return 0;
}
int64_t sceIoLseek(int fd, int64_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    assert(0); /* This fixture refuses log-file opens. */
    return -1;
}
int main(void)
{
    assert(module_start(0, NULL) == SCE_KERNEL_START_SUCCESS);
    assert(fake.loads == 1 && fake.starts == 1 && fake.exports == 2 && !fake.delays);
    memset(&fake, 0, sizeof(fake));
    fake.ready_after = 8;
    assert(!module_start(0, NULL));
    assert(fake.delays == 8 && fake.loads == 1 && fake.starts == 1);
    memset(&fake, 0, sizeof(fake));
    fake.missing = 1;
    assert(!module_start(0, NULL));
    assert(fake.delays == 120 && !fake.loads && !fake.starts && fake.exit_status < 0);
    memset(&fake, 0, sizeof(fake));
    fake.start_error = -1;
    assert(!module_start(0, NULL));
    assert(fake.loads == 1 && fake.starts == 1); /* No repeat after ambiguous start. */
    memset(&fake, 0, sizeof(fake));
    fake.load_error = -1;
    assert(!module_start(0, NULL));
    assert(fake.loads == 1 && !fake.starts);
    memset(&fake, 0, sizeof(fake));
    fake.ready_after = 8;
    fake.delay_error = -1;
    assert(!module_start(0, NULL));
    assert(fake.delays == 1 && !fake.loads && fake.exit_status < 0);
    puts("bounded loader readiness passed");
    return 0;
}
