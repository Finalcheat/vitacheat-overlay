#include "settings_service.h"
#include "../storage_paths.h"
#include "../config/config.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#define ENOENT_VITA ((int)0x80010002u)
static const char *const paths[] = {VCO_SETTINGS_PATH, VCO_SETTINGS_TEMP, VCO_SETTINGS_BACKUP};
/* Read-only migration preserves existing language and bindings. Missing new
 * primary AND backup are required; never fall back after corrupt/denied reads. */
static const char *const legacy_paths[] = {"ux0:data/VitaCheatOverlay/settings.ini",
                                           "ux0:data/VitaCheatOverlay/settings.ini.tmp",
                                           "ux0:data/VitaCheatOverlay/settings.ini.bak"};
static SceUID lock = -1;
static VcoSettings queued;
static bool pending;
static unsigned revision;
static int status = VCO_SAVE_READY;
static int read_config(void *context, unsigned id, char *data, size_t cap, size_t *length)
{
    const char *const *read_paths = context ? context : paths;
    *length = 0;
    int fd = sceIoOpen(read_paths[id], SCE_O_RDONLY, 0);
    if (fd < 0)
        return fd == ENOENT_VITA ? 1 : -1;
    int result = 0;
    while (*length < cap) {
        int n = sceIoRead(fd, data + *length, cap - *length);
        if (n < 0) {
            result = -1;
            break;
        }
        if (!n)
            break;
        *length += (size_t)n;
    }
    if (*length == cap)
        result = -1;
    if (sceIoClose(fd) < 0)
        result = -1;
    return result;
}
static int write_config(void *context, unsigned id, const char *data, size_t length)
{
    (void)context;
    sceIoMkdir(VCO_STORAGE_DIR, 0777);
    int fd = sceIoOpen(paths[id], SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    size_t done = 0;
    int result = 0;
    while (done < length) {
        int n = sceIoWrite(fd, data + done, length - done);
        if (n <= 0) {
            result = -1;
            break;
        }
        done += (size_t)n;
    }
    if (!result && sceIoSyncByFd(fd, 0) < 0)
        result = -1;
    if (sceIoClose(fd) < 0)
        result = -1;
    return result;
}
static int remove_config(void *context, unsigned id)
{
    (void)context;
    int result = sceIoRemove(paths[id]);
    return result >= 0 ? 0 : result == ENOENT_VITA ? 1 : -1;
}
static int rename_config(void *context, unsigned from, unsigned to)
{
    (void)context;
    return sceIoRename(paths[from], paths[to]) < 0 ? -1 : 0;
}
static const VcoConfigIO io = {NULL, read_config, write_config, remove_config, rename_config};
static int worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        VcoSettings value;
        unsigned generation = 0;
        bool work;
        sceKernelLockMutex(lock, 1, NULL);
        work = pending;
        if (work) {
            value = queued;
            generation = revision;
            pending = false;
        }
        sceKernelUnlockMutex(lock, 1);
        if (work) {
            bool ok = vco_config_save(&io, &value);
            /* Directory metadata synchronization is also off the UI thread. */
            if (ok && sceIoSync("ux0:", 0) < 0)
                ok = false;
            sceKernelLockMutex(lock, 1, NULL);
            if (revision == generation)
                __atomic_store_n(&status, ok ? VCO_SAVE_OK : VCO_SAVE_FAILED, __ATOMIC_RELEASE);
            sceKernelUnlockMutex(lock, 1);
        }
        sceKernelDelayThread(100000);
    }
    return 0;
}
int vco_settings_service_start(VcoSettings *out)
{
    VcoConfigLoad loaded = vco_config_load(&io, out);
    if (loaded == VCO_CONFIG_DEFAULT) {
        VcoConfigIO legacy = io;
        legacy.context = (void *)legacy_paths;
        loaded = vco_config_load(&legacy, out);
    }
    status = loaded == VCO_CONFIG_LOADED      ? VCO_SAVE_OK
             : loaded == VCO_CONFIG_RECOVERED ? VCO_SAVE_RECOVERED
             : loaded == VCO_CONFIG_INVALID   ? VCO_SAVE_INVALID
                                              : VCO_SAVE_READY;
    lock = sceKernelCreateMutex("vco_settings", 0, 0, NULL);
    if (lock < 0) {
        status = VCO_SAVE_FAILED;
        return lock;
    }
    SceUID thread = sceKernelCreateThread("vco_settings", worker, 170, 8192, 0, 0, NULL);
    int result = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (result < 0) {
        if (thread >= 0)
            sceKernelDeleteThread(thread);
        sceKernelDeleteMutex(lock);
        lock = -1;
        status = VCO_SAVE_FAILED;
    }
    return result;
}
bool vco_settings_service_request(const VcoSettings *value)
{
    if (!vco_settings_valid(value) || lock < 0 || sceKernelTryLockMutex(lock, 1) < 0)
        return false;
    queued = *value;
    ++revision;
    pending = true;
    __atomic_store_n(&status, VCO_SAVE_PENDING, __ATOMIC_RELEASE);
    sceKernelUnlockMutex(lock, 1);
    return true;
}
VcoSaveStatus vco_settings_service_status(void)
{
    return (VcoSaveStatus)__atomic_load_n(&status, __ATOMIC_ACQUIRE);
}
