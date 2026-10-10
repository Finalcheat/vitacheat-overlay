#include "platform/catalog_service.h"
#include "platform/execution_service.h"
#include "platform/game_probe.h"
#include "execution/service_protocol.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static SceKernelThreadEntry entry;
static bool held, allocated, fail_try, file_open;
static VcoCatalogWorkspace arena;
static VcoCatalog view;
static VcoForegroundSample sample;
static uint32_t kernel_state = VCO_SERVICE_READY;
static uint64_t kernel_session;
static unsigned delays, uploads, clears, hints = 10;
static unsigned readonly_publications;
static unsigned own_opens, legacy_opens, expected_legacy_opens;
static bool own_present = true, legacy_present = true, read_error, close_error, legacy_error;
static bool desired_on;
static uint32_t check_revision, expected_address = 0x81000000u;
static VcoCatalogSourceView source_view;
static size_t file_offset;
static bool flip_encoding_during_read;
static jmp_buf finish;
static uint32_t last_revision;
static int registry_language = SCE_SYSTEM_PARAM_LANG_CHINESE_S;
static bool system_param_failure, registry_failure, language_test;
static unsigned system_param_calls, registry_calls;
static const char source[] = "_V1 Data\n$0200 81000000 00000064\n";
static const char equal_length_edit[] = "_V1 Data\n$0200 81000000 00000065\n";
static const char longer_edit[] = "# changed\n_V1 Data\n$0200 81000000 00000065\n";
static const char new_session_file[] = "_V1 Data\n$0200 81001000 00000064\n";
static const char gbk_file[] = "_V1 \xce\xd2\xb7\xbd\n$0200 81001000 00000064\n";
static const char *own_data = source, *legacy_data = source, *open_data;
SceUID sceKernelCreateMutex(const char *name, unsigned attr, int initial, const void *opt)
{
    assert(name && !attr && !initial && !opt);
    return 1;
}
int sceKernelLockMutex(SceUID id, int count, SceUInt *timeout)
{
    assert(id == 1 && count == 1 && !timeout && !held);
    held = true;
    return 0;
}
int sceKernelTryLockMutex(SceUID id, int count)
{
    return fail_try ? -1 : sceKernelLockMutex(id, count, NULL);
}
int sceKernelUnlockMutex(SceUID id, int count)
{
    assert(id == 1 && count == 1 && held);
    held = false;
    return 0;
}
int sceKernelDeleteMutex(SceUID id)
{
    assert(id == 1);
    return 0;
}
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry fn, int priority, SceSize size,
                             unsigned attr, int cpu, const void *opt)
{
    assert(name && fn && priority > 0 && size >= 32768 && !attr && !cpu && !opt && !entry);
    entry = fn;
    return 2;
}
int sceKernelStartThread(SceUID id, SceSize size, void *arg)
{
    assert(id == 2 && !size && !arg);
    return 0;
}
int sceKernelDeleteThread(SceUID id)
{
    assert(id == 2);
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, SceSize size, const void *opt)
{
    assert(name && type == 1 && size >= sizeof(arena) && !opt && !allocated);
    allocated = true;
    return 3;
}
int sceKernelGetMemBlockBase(SceUID id, void **out)
{
    assert(id == 3 && allocated && out);
    *out = &arena;
    return 0;
}
int sceKernelFreeMemBlock(SceUID id)
{
    assert(id == 3 && allocated);
    allocated = false;
    memset(&arena, 0xa5, sizeof(arena));
    return 0;
}
int sceIoOpen(const char *path, int flags, int mode)
{
    (void)flags;
    (void)mode;
    if (!strstr(path, ".psv"))
        return (int)0x80010002u;
    bool own = strstr(path, "ux0:data/") != NULL;
    if (own) {
        ++own_opens;
    } else {
        assert(!strncmp(path, "ux0:vitacheat/db/", 16));
        ++legacy_opens;
        if (legacy_error)
            return (int)0x80010005u;
    }
    if (!(own ? own_present : legacy_present))
        return (int)0x80010002u;
    assert(!file_open);
    file_open = true;
    file_offset = 0;
    open_data = own ? own_data : legacy_data;
    return 4;
}
int sceIoRead(int fd, void *out, size_t length)
{
    assert(fd == 4 && file_open);
    if (flip_encoding_during_read) {
        flip_encoding_during_read = false;
        vco_catalog_service_encoding(VCO_ENCODING_GBK);
    }
    if (read_error && file_offset)
        return -1;
    size_t amount = strlen(open_data) - file_offset;
    if (amount > length)
        amount = length;
    memcpy(out, open_data + file_offset, amount);
    file_offset += amount;
    return (int)amount;
}
int sceIoWrite(int fd, const void *in, size_t length)
{
    (void)fd;
    (void)in;
    (void)length;
    assert(!"logging descriptor should not be available");
    return -1;
}
int sceIoClose(int fd)
{
    assert(fd == 4 && file_open);
    file_open = false;
    return close_error ? -1 : 0;
}
int64_t sceIoLseek(int fd, int64_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    assert(!"logging descriptor should not be available");
    return -1;
}
int sceRegMgrSystemParamGetInt(int id, int *out)
{
    ++system_param_calls;
    assert(id == SCE_SYSTEM_PARAM_ID_LANG);
    if (system_param_failure)
        return -1;
    *out = SCE_SYSTEM_PARAM_LANG_CHINESE_S;
    return 0;
}
int sceRegMgrGetKeyInt(const char *category, const char *key, int *out)
{
    assert(!strcmp(category, "/CONFIG/SYSTEM") && !strcmp(key, "language") && !held);
    ++registry_calls;
    if (registry_failure)
        return -1;
    *out = registry_language;
    return 0;
}
void vco_foreground_sample(VcoForegroundSample *out)
{
    assert(!held);
    *out = sample;
}
int vcoProbeGame(int32_t pid, VcoGameProbe *out)
{
    assert(pid == sample.game_pid);
    *out = (VcoGameProbe){
        .abi = VCO_PROBE_ABI, .size = sizeof(*out), .pid = pid, .main_thread = (int32_t)hints};
    return 0;
}
int vcoServiceStatus(VcoServiceStatus *out, uint32_t length)
{
    assert(!held && length == sizeof(*out));
    *out = (VcoServiceStatus){.abi = VCO_SERVICE_ABI, .size = sizeof(*out), .state = kernel_state};
    if (kernel_state != VCO_SERVICE_NO_GAME) {
        out->session = kernel_session;
        out->pid = sample.game_pid;
        memcpy(out->title_id, sample.game_title_id, sizeof(out->title_id));
    }
    return VCO_MAILBOX_OK;
}
int vco_execution_service_catalog(const VcoCatalog *catalog, const VcoCatalogWorkspace *work)
{
    assert(!held && catalog->revision > last_revision);
    last_revision = catalog->revision;
    VcoCatalog snapshot = {0};
    assert(vco_catalog_service_copy(&snapshot) && snapshot.revision == catalog->revision);
    if (work) {
        assert(allocated && work == &arena && catalog->state == VCO_LIST_READY &&
               catalog->count == 1 && work->entry_count == 1 && work->code_count == 1 &&
               work->codes[0].a == expected_address && work->entries[0].file_enabled);
        if (catalog->execution_session) {
            assert(catalog->execution_session == kernel_session);
            ++uploads;
        } else
            ++readonly_publications;
    } else {
        assert(!catalog->count);
        ++clears;
    }
    desired_on = false; /* Every actual bridge publication clears the old selection. */
    return 0;
}
static void candidate(int32_t pid, const char *title, bool background)
{
    sample = (VcoForegroundSample){.abi = VCO_FOREGROUND_PROBE_ABI,
                                   .size = sizeof(sample),
                                   .status = background ? VCO_BACKGROUND_CANDIDATE
                                                        : VCO_FOREGROUND_CANDIDATE,
                                   .game_app_id = pid + 100,
                                   .game_pid = pid};
    memcpy(sample.game_title_id, title, sizeof(sample.game_title_id));
}
static void unknown(void)
{
    sample = (VcoForegroundSample){.abi = VCO_FOREGROUND_PROBE_ABI,
                                   .size = sizeof(sample),
                                   .failure = VCO_FOREGROUND_FAILURE_QUERY};
}
static void check_async(void)
{
    unsigned reads = own_opens + legacy_opens;
    vco_catalog_service_check_source();
    assert(reads == own_opens + legacy_opens); /* The UI only enqueues, never reads. */
}
static void checked(VcoSourceCheckState state, unsigned expected_uploads, unsigned expected_clears)
{
    assert(source_view.state == state && source_view.catalog_revision == view.revision);
    assert(view.revision == check_revision && uploads == expected_uploads &&
           clears == expected_clears && desired_on);
}
int sceKernelDelayThread(SceUInt usec)
{
    assert(usec == 250000 && !held && !allocated && !file_open);
    ++delays;
    if (language_test) {
        if (delays == 4) {
            assert(vco_catalog_service_language() == 0);
            registry_failure = false;
        } else if (delays == 8) {
            assert(vco_catalog_service_language() == 1);
            registry_language = SCE_SYSTEM_PARAM_LANG_CHINESE_T;
        } else if (delays == 12) {
            assert(vco_catalog_service_language() == 2);
            registry_failure = true;
        } else if (delays == 16) {
            assert(vco_catalog_service_language() == 2);
            registry_failure = false;
            registry_language = 999; /* Invalid values do not erase known Chinese. */
        } else if (delays == 20) {
            assert(vco_catalog_service_language() == 2);
            registry_language = 1; /* English */
        } else if (delays == 24) {
            assert(vco_catalog_service_language() == 0 && registry_calls >= 7 &&
                   registry_calls <= 8 && !system_param_calls);
            longjmp(finish, 1);
        }
        return 0;
    }
    (void)vco_catalog_service_copy(&view);
    (void)vco_catalog_service_source_copy(&source_view);
    if (delays == 1) {
        assert(!uploads && clears == 1 && readonly_publications == 1 && !view.execution_session &&
               view.count == 1 && view.game.pid == 77);
        unknown();
        kernel_state = VCO_SERVICE_BACKGROUND;
    } else if (delays == 8) {
        assert(!uploads && clears == 1 && readonly_publications == 1 && view.count == 1 &&
               view.game.pid == 77);
        candidate(77, "PCSE00001", true);
        kernel_session = 100; /* First token requires a new file read bound to this instance. */
        ++hints;              /* A diagnostic-only hint cannot force an all-OFF reload. */
    } else if (delays == 16) {
        assert(uploads == 1 && clears == 2 && readonly_publications == 1 &&
               view.execution_session == 100 && view.game.pid == 77);
        unknown();
        kernel_state = VCO_SERVICE_NO_GAME;
    } else if (delays == 24) {
        assert(uploads == 1 && clears == 3 && view.state == VCO_LIST_NO_GAME && !view.count);
        candidate(78, "PCSE00002", false);
        kernel_session = 101;
        kernel_state = VCO_SERVICE_LOADING;
    } else if (delays == 32) {
        assert(uploads == 2 && clears == 4 && view.state == VCO_LIST_READY && view.count == 1 &&
               view.game.pid == 78 && !strcmp(view.game.title_id, "PCSE00002"));
        unknown();
        kernel_state = VCO_SERVICE_BACKGROUND;
        vco_catalog_service_refresh();
    } else if (delays == 36) {
        assert(uploads == 2 && clears == 4 && view.game.pid == 78);
        candidate(78, "PCSE00002", false);
    } else if (delays == 44) {
        assert(uploads == 3 && clears == 5 && view.game.pid == 78 && view.count == 1);
        fail_try = true;
        assert(!vco_catalog_service_copy(&view));
        assert(!vco_catalog_service_source_copy(&source_view));
        fail_try = false;
        check_revision = view.revision;
        desired_on = true;
        legacy_data = equal_length_edit;
        check_async();
    } else if (delays == 45) {
        checked(VCO_SOURCE_CHANGED, 3, 5);
        read_error = true;
        check_async();
    } else if (delays == 46) {
        checked(VCO_SOURCE_UNAVAILABLE, 3, 5); /* Partial read cannot publish unchanged. */
        read_error = false;
        legacy_data = source;
        check_async();
    } else if (delays == 47) {
        checked(VCO_SOURCE_UNCHANGED, 3, 5);
        legacy_data = longer_edit;
        check_async();
    } else if (delays == 48) {
        checked(VCO_SOURCE_CHANGED, 3, 5);
        vco_catalog_service_refresh();
    } else if (delays == 49) {
        assert(uploads == 4 && clears == 6 && !desired_on &&
               source_view.state == VCO_SOURCE_UNCHANGED && view.revision != check_revision);
        desired_on = true;
        check_revision = view.revision;
        own_present = false;
        own_data = legacy_data;
        check_async();
    } else if (delays == 50) {
        checked(VCO_SOURCE_UNCHANGED, 4, 6); /* Obsolete private file is ignored. */
        vco_catalog_service_refresh();
    } else if (delays == 51) {
        assert(uploads == 5 && clears == 7 && !desired_on && view.source == VCO_SOURCE_VITACHEAT &&
               source_view.state == VCO_SOURCE_UNCHANGED);
        desired_on = true;
        check_revision = view.revision;
        own_present = true;
        check_async();
    } else if (delays == 52) {
        checked(VCO_SOURCE_UNCHANGED, 5, 7); /* Private directory cannot override DB. */
        legacy_error = true;
        expected_legacy_opens = legacy_opens;
        check_async();
    } else if (delays == 53) {
        checked(VCO_SOURCE_UNAVAILABLE, 5, 7);
        assert(!own_opens &&
               legacy_opens == expected_legacy_opens + 1); /* DB failure has no fallback. */
        legacy_error = false;
        close_error = true;
        check_async();
    } else if (delays == 54) {
        checked(VCO_SOURCE_UNAVAILABLE, 5, 7);
        close_error = false;
        own_present = false;
        check_async();
    } else if (delays == 55) {
        checked(VCO_SOURCE_UNCHANGED, 5, 7);
        legacy_present = false;
        check_async();
    } else if (delays == 56) {
        checked(VCO_SOURCE_CHANGED, 5, 7); /* Successfully confirmed removal. */
        legacy_present = true;
        check_async();
    } else if (delays == 57) {
        checked(VCO_SOURCE_UNCHANGED, 5, 7);
        kernel_session = 0;
        check_async();
    } else if (delays == 58) {
        checked(VCO_SOURCE_UNCHANGED, 5, 7);
        kernel_session = 123; /* Same PID/app/title, verified different process instance. */
        legacy_data = new_session_file;
        expected_address = 0x81001000u;
        check_async();
    } else if (delays == 59) {
        assert(uploads == 6 && clears == 8 && !desired_on && view.game.pid == 78 &&
               !strcmp(view.game.title_id, "PCSE00002") &&
               source_view.state == VCO_SOURCE_UNCHANGED);
        desired_on = true;
        check_revision = view.revision;
        kernel_session = 0;
        check_async();
    } else if (delays == 60) {
        checked(VCO_SOURCE_UNCHANGED, 6, 8);
        kernel_session = 123;
        check_async();
    } else if (delays == 61) {
        checked(VCO_SOURCE_UNCHANGED, 6, 8); /* Zero-token gap cannot cause another reload. */
        vco_catalog_service_refresh();
    } else if (delays == 62) {
        assert(uploads == 7 && clears == 9 && !desired_on &&
               source_view.state == VCO_SOURCE_UNCHANGED && view.revision != check_revision);
        legacy_data = gbk_file;
        desired_on = true;
        check_revision = view.revision;
        unsigned reads = own_opens + legacy_opens;
        vco_catalog_service_encoding(VCO_ENCODING_UTF8);
        vco_catalog_service_encoding((VcoEncoding)99); /* Invalid policy is ignored. */
        assert(reads == own_opens + legacy_opens);
        check_async();
    } else if (delays == 63) {
        checked(VCO_SOURCE_CHANGED, 7, 9); /* Apply does not clear active entries. */
        vco_catalog_service_refresh();
    } else if (delays == 64) {
        assert(view.state == VCO_LIST_PARSE_ERROR && view.encoding_policy == VCO_ENCODING_UTF8 &&
               view.encoding == VCO_ENCODING_AUTO && view.diagnostic.code == VCO_E_ENCODING &&
               uploads == 7 && clears == 11 && !desired_on);
        check_revision = view.revision;
        vco_catalog_service_encoding(VCO_ENCODING_GBK);
        check_async();
    } else if (delays == 65) {
        assert(view.revision == check_revision && view.state == VCO_LIST_PARSE_ERROR &&
               uploads == 7 && clears == 11 && source_view.state == VCO_SOURCE_UNCHANGED);
        vco_catalog_service_refresh();
    } else if (delays == 66) {
        assert(view.state == VCO_LIST_READY && view.encoding_policy == VCO_ENCODING_GBK &&
               view.encoding == VCO_ENCODING_GBK && !strcmp(view.rows[0].name, "我方") &&
               uploads == 8 && clears == 12 && !desired_on);
        desired_on = true;
        check_revision = view.revision;
        check_async();
    } else if (delays == 67) {
        checked(VCO_SOURCE_UNCHANGED, 8, 12); /* Fingerprint is of GBK bytes, not decoded UTF-8. */
        vco_catalog_service_encoding(VCO_ENCODING_UTF8);
        check_async();
    } else if (delays == 68) {
        checked(VCO_SOURCE_UNCHANGED, 8, 12);
        assert(view.encoding_policy == VCO_ENCODING_GBK); /* Snapshot describes loaded data. */
        flip_encoding_during_read = true;
        vco_catalog_service_refresh();
    } else if (delays == 69) {
        assert(view.state == VCO_LIST_PARSE_ERROR && view.encoding_policy == VCO_ENCODING_UTF8 &&
               view.diagnostic.code == VCO_E_ENCODING && uploads == 8 && clears == 14 &&
               !desired_on && !flip_encoding_during_read);
        vco_catalog_service_refresh();
    } else if (delays == 70) {
        assert(view.state == VCO_LIST_READY && view.encoding_policy == VCO_ENCODING_GBK &&
               view.encoding == VCO_ENCODING_GBK && uploads == 9 && clears == 15 && !desired_on);
        longjmp(finish, 1);
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        system_param_failure = true; /* The deprecated startup path must not matter. */
        candidate(77, "PCSE00001", false);
        if (!strcmp(argv[1], "language_registry")) {
            assert(!vco_catalog_service_start() && vco_catalog_service_language() == 1);
        } else if (!strcmp(argv[1], "language_traditional")) {
            registry_language = SCE_SYSTEM_PARAM_LANG_CHINESE_T;
            assert(!vco_catalog_service_start() && vco_catalog_service_language() == 2);
        } else if (!strcmp(argv[1], "language_japanese")) {
            registry_language = SCE_SYSTEM_PARAM_LANG_JAPANESE;
            assert(!vco_catalog_service_start() && vco_catalog_service_language() == 3);
        } else if (!strcmp(argv[1], "language_korean")) {
            registry_language = SCE_SYSTEM_PARAM_LANG_KOREAN;
            assert(!vco_catalog_service_start() && vco_catalog_service_language() == 4);
        } else if (!strcmp(argv[1], "language_recovery")) {
            language_test = registry_failure = true;
            assert(!vco_catalog_service_start() && vco_catalog_service_language() == 0);
            if (!setjmp(finish))
                entry(0, NULL);
            assert(delays == 24);
        } else
            assert(0);
        assert(!system_param_calls);
        return 0;
    }
    assert(argc == 1);
    candidate(77, "PCSE00001", false);
    assert(!vco_catalog_service_start() && vco_catalog_service_language() == 1);
    if (!setjmp(finish))
        entry(0, NULL);
    assert(delays == 70 && !held && !allocated && !own_opens);
    puts("catalog service lifecycle, raw source checks, explicit encoding policy and replacement "
         "passed");
    return 0;
}
