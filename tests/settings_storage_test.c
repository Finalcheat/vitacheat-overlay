#include "platform/settings_service.h"
#include "config/config.h"
#include "sdk.h"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
static const char *const names[] = {"ux0:vitacheat/vco-settings.ini",
                                    "ux0:vitacheat/vco-settings.ini.tmp",
                                    "ux0:vitacheat/vco-settings.ini.bak",
                                    "ux0:data/VitaCheatOverlay/settings.ini",
                                    "ux0:data/VitaCheatOverlay/settings.ini.tmp",
                                    "ux0:data/VitaCheatOverlay/settings.ini.bak"};
static struct {
    char bytes[VCO_CONFIG_CAPACITY];
    size_t length, position;
    int present, denied;
} files[6];
static unsigned old_reads, writes, directories;
static int (*entry)(SceSize, void *);
static jmp_buf tick_end;
static unsigned index_of(const char *path)
{
    for (unsigned i = 0; i < 6; ++i)
        if (!strcmp(path, names[i]))
            return i;
    assert(!"unexpected settings path");
    return 0;
}
int sceIoOpen(const char *path, int flags, int mode)
{
    unsigned i = index_of(path);
    (void)mode;
    if (flags == SCE_O_RDONLY) {
        old_reads += i >= 3;
        if (files[i].denied)
            return -123;
        if (!files[i].present)
            return (int)0x80010002u;
    } else {
        assert(i < 3 && flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC));
        files[i].present = 1;
        files[i].length = 0;
        ++writes;
    }
    files[i].position = 0;
    return (int)i;
}
int sceIoRead(int fd, void *out, size_t cap)
{
    size_t n = files[fd].length - files[fd].position;
    if (n > cap)
        n = cap;
    memcpy(out, files[fd].bytes + files[fd].position, n);
    files[fd].position += n;
    return (int)n;
}
int sceIoWrite(int fd, const void *in, size_t n)
{
    assert(fd < 3 && n <= sizeof(files[fd].bytes));
    memcpy(files[fd].bytes, in, n);
    files[fd].length = n;
    return (int)n;
}
int sceIoClose(int fd)
{
    assert(fd >= 0 && fd < 6);
    return 0;
}
int sceIoSyncByFd(int fd, int flags)
{
    assert(fd < 3 && !flags);
    return 0;
}
int sceIoSync(const char *path, int flags)
{
    assert(!strcmp(path, "ux0:") && !flags);
    return 0;
}
int sceIoMkdir(const char *path, int mode)
{
    assert(!strcmp(path, "ux0:vitacheat") && mode == 0777);
    ++directories;
    return 0;
}
int sceIoRemove(const char *path)
{
    unsigned i = index_of(path);
    assert(i < 3);
    if (!files[i].present)
        return (int)0x80010002u;
    files[i].present = 0;
    return 0;
}
int sceIoRename(const char *from, const char *to)
{
    unsigned a = index_of(from), b = index_of(to);
    assert(a < 3 && b < 3 && files[a].present && !files[b].present);
    files[b] = files[a];
    files[a].present = 0;
    return 0;
}
int sceKernelCreateMutex(const char *name, unsigned flags, int initial, void *opts)
{
    assert(!strcmp(name, "vco_settings") && !flags && !initial && !opts);
    return 42;
}
int sceKernelLockMutex(int id, int count, void *timeout)
{
    assert(id == 42 && count == 1 && !timeout);
    return 0;
}
int sceKernelTryLockMutex(int id, int count)
{
    assert(id == 42 && count == 1);
    return 0;
}
int sceKernelUnlockMutex(int id, int count)
{
    assert(id == 42 && count == 1);
    return 0;
}
int sceKernelDeleteMutex(int id)
{
    assert(id == 42);
    return 0;
}
int sceKernelCreateThread(const char *name, int (*fn)(SceSize, void *), int priority, SceSize stack,
                          unsigned flags, int cpu, void *opts)
{
    assert(!strcmp(name, "vco_settings") && priority == 170 && stack == 8192 && !flags && !cpu &&
           !opts);
    entry = fn;
    return 43;
}
int sceKernelStartThread(int id, SceSize len, void *arg)
{
    assert(id == 43 && !len && !arg);
    return 0;
}
int sceKernelDeleteThread(int id)
{
    assert(id == 43);
    return 0;
}
int sceKernelDelayThread(unsigned delay)
{
    assert(delay == 100000);
    longjmp(tick_end, 1);
    return 0;
}
static void put(unsigned i, const VcoSettings *s)
{
    files[i].present = 1;
    files[i].length = vco_config_encode(s, files[i].bytes, sizeof(files[i].bytes));
    assert(files[i].length);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    VcoSettings defaults = vco_settings_defaults(), old = defaults, newer = defaults, out;
    old.language = 2;
    old.opacity = 35;
    newer.language = 1;
    newer.opacity = 90;
    put(3, &old);
    if (!strcmp(argv[1], "japanese") || !strcmp(argv[1], "korean")) {
        newer.language = !strcmp(argv[1], "japanese") ? VCO_LANGUAGE_JAPANESE : VCO_LANGUAGE_KOREAN;
        assert(!vco_settings_service_start(&out) && vco_settings_equal(&out, &old));
        assert(vco_settings_service_request(&newer));
        if (!setjmp(tick_end))
            entry(0, NULL);
        assert(vco_settings_service_status() == VCO_SAVE_OK);
        out = defaults;
        assert(vco_config_decode(files[0].bytes, files[0].length, &out) &&
               out.language == newer.language && vco_settings_equal(&out, &newer));
        assert(!memcmp(files[0].bytes, "version=6\n", 10));
    } else if (!strcmp(argv[1], "regional_migration")) {
        const char legacy[] = "version=4\nlanguage=0\nopacity=70\nprevious=128\nnext=32\n"
                              "toggle=32768\nclose=16384\nsettings=1\nopen=513\nencoding=0\n";
        files[0].present = 1;
        files[0].length = sizeof(legacy) - 1;
        memcpy(files[0].bytes, legacy, sizeof(legacy) - 1);
        assert(!vco_settings_service_start(&out) && out.button_mode == VCO_BUTTON_ASIAN &&
               out.language == -1 && out.bindings.open == (VCO_R | VCO_SELECT));
        assert(!writes && !directories && !old_reads &&
               !memcmp(files[0].bytes, legacy, sizeof(legacy) - 1));
        vco_settings_set_button_mode(&out, VCO_BUTTON_WESTERN);
        assert(vco_settings_service_request(&out));
        if (!setjmp(tick_end))
            entry(0, NULL);
        assert(vco_settings_service_status() == VCO_SAVE_OK &&
               files[2].length == sizeof(legacy) - 1 &&
               !memcmp(files[2].bytes, legacy, sizeof(legacy) - 1));
        assert(vco_config_decode(files[0].bytes, files[0].length, &out) &&
               out.button_mode == VCO_BUTTON_WESTERN && out.language == -1 &&
               out.bindings.open == (VCO_R | VCO_SELECT));
    } else if (!strcmp(argv[1], "migration")) {
        assert(!vco_settings_service_start(&out) && vco_settings_equal(&out, &old));
        assert(old_reads && !writes && !directories && !files[0].present);
        assert(vco_settings_service_request(&newer));
        if (!setjmp(tick_end))
            entry(0, NULL);
        assert(vco_settings_service_status() == VCO_SAVE_OK && files[0].present &&
               !files[1].present && writes == 1 && directories == 1);
        assert(vco_config_decode(files[0].bytes, files[0].length, &out) &&
               vco_settings_equal(&out, &newer));
        assert(vco_config_decode(files[3].bytes, files[3].length, &out) &&
               vco_settings_equal(&out, &old));
    } else if (!strcmp(argv[1], "preferred")) {
        put(0, &newer);
        assert(!vco_settings_service_start(&out));
        assert(vco_settings_equal(&out, &newer) && !old_reads && !writes);
    } else if (!strcmp(argv[1], "recovery")) {
        files[0].present = 1;
        files[0].length = 3;
        memcpy(files[0].bytes, "bad", 3);
        put(2, &newer);
        assert(!vco_settings_service_start(&out));
        assert(vco_settings_equal(&out, &newer) && !old_reads &&
               vco_settings_service_status() == VCO_SAVE_RECOVERED);
    } else if (!strcmp(argv[1], "corrupt") || !strcmp(argv[1], "denied")) {
        files[0].present = 1;
        files[0].length = 3;
        memcpy(files[0].bytes, "bad", 3);
        files[0].denied = !strcmp(argv[1], "denied");
        assert(!vco_settings_service_start(&out));
        assert(vco_settings_equal(&out, &defaults) && !old_reads && !writes &&
               vco_settings_service_status() == VCO_SAVE_INVALID);
    } else {
        assert(!strcmp(argv[1], "defaults"));
        files[3].present = 0;
        assert(!vco_settings_service_start(&out) && vco_settings_equal(&out, &defaults));
        assert(!writes && !directories && vco_settings_service_status() == VCO_SAVE_READY);
    }
    puts("PASS native settings shared-directory storage");
    return 0;
}
