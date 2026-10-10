#include "catalog_service.h"
#include "../localization.h"
#include "execution_service.h"
#include "foreground_probe.h"
#include "../execution/service_protocol.h"
#include "game_probe.h"
#include "sfo.h"
#include "text.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/registrymgr.h>
#include <psp2/system_param.h>
#include <string.h>
#include "../storage_paths.h"
#define VCO_LOG_PATH VCO_CATALOG_LOG
#include "diagnostics.h"

static SceUID snapshot_lock = -1;
static uint32_t refresh_request;
static uint32_t source_request;
static uint32_t encoding_policy = VCO_ENCODING_AUTO;
static VcoCatalog published, pending;
static VcoCatalogSourceView published_source;
static VcoSourceStamp loaded_source;
static bool have_source;
static int language;
static void refresh_language(void)
{
    int system_language = -1;
    /* Read the same persistent key used by system settings, including in the
     * Shell plugin context. Failed reads preserve the last known language. */
    if (sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "language", &system_language) < 0 ||
        system_language < 0 || system_language > SCE_SYSTEM_PARAM_LANG_TURKISH)
        return;
    int selected = system_language == SCE_SYSTEM_PARAM_LANG_CHINESE_S   ? VCO_LANGUAGE_CHINESE_S
                   : system_language == SCE_SYSTEM_PARAM_LANG_CHINESE_T ? VCO_LANGUAGE_CHINESE_T
                   : system_language == SCE_SYSTEM_PARAM_LANG_JAPANESE  ? VCO_LANGUAGE_JAPANESE
                   : system_language == SCE_SYSTEM_PARAM_LANG_KOREAN    ? VCO_LANGUAGE_KOREAN
                                                                        : VCO_LANGUAGE_ENGLISH;
    __atomic_store_n(&language, selected, __ATOMIC_RELEASE);
}
/* Vita's errno encoding for ENOENT. Other errors MUST NOT trigger fallback. */
#define VCO_SCE_ENOENT ((int)0x80010002u)
static VcoFileResult read_file(void *ctx, const char *path, char *out, size_t capacity,
                               size_t *length)
{
    (void)ctx;
    *length = 0;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return fd == VCO_SCE_ENOENT ? VCO_FILE_MISSING : VCO_FILE_ERROR;
    VcoFileResult result = VCO_FILE_OK;
    while (*length < capacity) {
        int n = sceIoRead(fd, out + *length, capacity - *length);
        if (n < 0) {
            result = VCO_FILE_ERROR;
            break;
        }
        if (!n)
            break;
        *length += (size_t)n;
    }
    if (result == VCO_FILE_OK && *length == capacity) {
        char extra;
        int n = sceIoRead(fd, &extra, 1);
        if (n > 0)
            result = VCO_FILE_TOO_LARGE;
        if (n < 0)
            result = VCO_FILE_ERROR;
    }
    if (sceIoClose(fd) < 0)
        result = VCO_FILE_ERROR;
    return result;
}
typedef struct {
    VcoSourceStamp stamp;
    bool attempted;
} SourceRead;
static VcoFileResult read_catalog(void *ctx, const char *path, char *out, size_t capacity,
                                  size_t *length)
{
    SourceRead *read = ctx;
    read->attempted = true;
    VcoFileResult result = read_file(NULL, path, out, capacity, length);
    vco_source_begin(&read->stamp, VCO_SOURCE_VITACHEAT);
    if (result == VCO_FILE_OK)
        (void)vco_source_add(&read->stamp, out, *length);
    else
        read->stamp.result = result;
    if (result == VCO_FILE_MISSING)
        read->stamp.source = VCO_SOURCE_NONE;
    return result;
}
static void fingerprint_file(const char *path, VcoListSource source, VcoSourceStamp *stamp)
{
    vco_source_begin(stamp, source);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        stamp->result = fd == VCO_SCE_ENOENT ? VCO_FILE_MISSING : VCO_FILE_ERROR;
        if (stamp->result == VCO_FILE_MISSING)
            stamp->source = VCO_SOURCE_NONE;
        return;
    }
    char buffer[2048];
    for (;;) {
        int amount = sceIoRead(fd, buffer, sizeof(buffer));
        if (amount < 0 || amount > (int)sizeof(buffer)) {
            stamp->result = VCO_FILE_ERROR;
            break;
        }
        if (!amount || !vco_source_add(stamp, buffer, (size_t)amount))
            break;
    }
    if (sceIoClose(fd) < 0)
        stamp->result = VCO_FILE_ERROR;
}
static void source_publish(VcoSourceCheckState state)
{
    sceKernelLockMutex(snapshot_lock, 1, NULL);
    ++published_source.revision;
    if (!published_source.revision)
        ++published_source.revision;
    published_source.catalog_revision = published.revision;
    published_source.state = state;
    sceKernelUnlockMutex(snapshot_lock, 1);
}
static void check_source(void)
{
    if (!have_source || !vco_title_id_valid(pending.game.title_id)) {
        source_publish(VCO_SOURCE_UNAVAILABLE);
        return;
    }
    source_publish(VCO_SOURCE_CHECKING);
    char path[80];
    VcoSourceStamp observed;
    sceClibSnprintf(path, sizeof(path), VCO_CHEAT_DB_DIR "%s.psv", pending.game.title_id);
    fingerprint_file(path, VCO_SOURCE_VITACHEAT, &observed);
    source_publish(vco_source_compare(&loaded_source, &observed));
}
static VcoListState discover(VcoGame *game)
{
    VcoForegroundSample sample;
    vco_foreground_sample(&sample);
    memset(game, 0, sizeof(*game));
    /* The same GAME selector as the independent execution observer identifies
     * presentation in foreground and background. Never choose an unrelated
     * process from a running-application enumeration. UNKNOWN is not absence. */
    if (sample.abi != VCO_FOREGROUND_PROBE_ABI || sample.size != sizeof(sample) || sample.failure ||
        (sample.status != VCO_FOREGROUND_CANDIDATE && sample.status != VCO_BACKGROUND_CANDIDATE) ||
        sample.game_app_id <= 0 || sample.game_pid <= 0 || sample.game_title_id[9] ||
        !vco_title_id_valid(sample.game_title_id)) {
        VcoServiceStatus execution = {0};
        if (vcoServiceStatus(&execution, sizeof(execution)) == VCO_MAILBOX_OK &&
            execution.abi == VCO_SERVICE_ABI && execution.size == sizeof(execution) &&
            execution.state == VCO_SERVICE_NO_GAME && !execution.session && !execution.revision &&
            !execution.count && !execution.pid)
            return VCO_LIST_NO_GAME;
        return VCO_LIST_DISCOVERY_ERROR;
    }
    game->app_id = sample.game_app_id;
    game->pid = sample.game_pid;
    vco_text_copy(game->title_id, sizeof(game->title_id), sample.game_title_id);
    VcoListState selected = VCO_LIST_READY;
    if (selected == VCO_LIST_READY) {
        VcoGameProbe probe = {0};
        static VcoGameProbe last_probe;
        static int last_result = 1;
        int result = vcoProbeGame(game->pid, &probe);
        if (result != last_result || memcmp(&probe, &last_probe, sizeof(probe))) {
            vco_log("probe_pid", (unsigned)game->pid, 0);
            vco_log("probe_result", (unsigned)result, 0);
            if (!result) {
                vco_log("process_info", (unsigned)probe.info_result, 0);
                vco_log("status_result", (unsigned)probe.status_result, 0);
                vco_log("process_status", (unsigned)probe.process_status, 0);
                vco_log("main_thread_hint", (unsigned)probe.main_thread, 0);
                vco_log("modules_result", (unsigned)probe.modules_result, 0);
                vco_log("module_count", probe.module_count, 0);
                vco_log("execution_capabilities", probe.verified_capabilities, 0);
            }
            last_probe = probe;
            last_result = result;
        }
        if (!result && probe.abi == VCO_PROBE_ABI && probe.size == sizeof(probe) &&
            probe.pid == game->pid && probe.info_result >= 0 && probe.main_thread > 0)
            game->main_thread_hint = probe.main_thread;
    }
    return selected;
}
static void metadata(VcoGame *game)
{
    char data[16384], path[96], id[32], title[256], version[32];
    size_t length;
    const char *roots[] = {"ux0:patch/", "ux0:app/", "gro0:app/"};
    for (size_t i = 0; i < 3; ++i) {
        sceClibSnprintf(path, sizeof(path), "%s%s/sce_sys/param.sfo", roots[i], game->title_id);
        if (read_file(NULL, path, data, sizeof(data), &length) != VCO_FILE_OK)
            continue;
        if (vco_sfo_string(data, length, "TITLE_ID", id, sizeof(id)) != 1 ||
            strcmp(id, game->title_id))
            continue;
        if (vco_sfo_string(data, length, "TITLE", title, sizeof(title)) != 1 || !title[0])
            continue;
        vco_text_copy(game->title, sizeof(game->title), title);
        if (vco_sfo_string(data, length, "APP_VER", version, sizeof(version)) == 1)
            vco_text_copy(game->version, sizeof(game->version), version);
        return;
    }
}
static void publish(const VcoCatalogWorkspace *work, const VcoSourceStamp *source)
{
    sceKernelLockMutex(snapshot_lock, 1, NULL);
    uint32_t revision = published.revision + 1;
    published = pending;
    published.revision = revision ? revision : 1;
    pending.revision = published.revision;
    have_source = source != NULL;
    if (source)
        loaded_source = *source;
    ++published_source.revision;
    if (!published_source.revision)
        ++published_source.revision;
    published_source.catalog_revision = published.revision;
    published_source.state = source ? vco_source_compare(source, source) : VCO_SOURCE_UNCHECKED;
    sceKernelUnlockMutex(snapshot_lock, 1);
    /* This bounded copy owns the parsed data before its temporary memblock is
     * freed. The UI lock is never held while encoding or staging an upload. */
    int result = vco_execution_service_catalog(&pending, work);
    if (result < 0)
        vco_log("execution_catalog_error", (unsigned)result, 0);
}
static int same_game(const VcoGame *a, const VcoGame *b)
{
    return a->pid == b->pid && a->app_id == b->app_id && !strcmp(a->title_id, b->title_id);
}
static uint64_t candidate_session(const VcoGame *game)
{
    VcoServiceStatus status = {0};
    if (vcoServiceStatus(&status, sizeof(status)) != VCO_MAILBOX_OK ||
        status.abi != VCO_SERVICE_ABI || status.size != sizeof(status) || !status.session ||
        status.pid != game->pid || status.title_id[9] || memcmp(status.title_id, game->title_id, 9))
        return 0;
    return status.session;
}
static int worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    VcoGame previous = {0};
    VcoListState previous_state = VCO_LIST_LOADING;
    uint64_t previous_session = 0;
    uint32_t seen = 0, source_seen = 0;
    unsigned ticks = 0;
    for (;;) {
        uint32_t request = __atomic_load_n(&refresh_request, __ATOMIC_ACQUIRE);
        uint32_t check = __atomic_load_n(&source_request, __ATOMIC_ACQUIRE);
        if (request == seen && check == source_seen && ticks++ % 4) {
            sceKernelDelayThread(250000);
            continue;
        }
        refresh_language();
        VcoGame game = {0};
        VcoListState state = discover(&game);
        if (state == VCO_LIST_DISCOVERY_ERROR && previous_state == VCO_LIST_READY) {
            /* A transient AppMgr failure pauses the kernel lease; it must not
             * erase desired ON state or reload the same session as all OFF.
             * Keep an explicit refresh pending until a target is confirmed. */
            if (check != source_seen) {
                source_seen = check;
                check_source();
            }
            sceKernelDelayThread(250000);
            continue;
        }
        uint64_t session = state == VCO_LIST_READY ? candidate_session(&game) : 0;
        bool identity_changed = !same_game(&game, &previous);
        if (identity_changed || state == VCO_LIST_NO_GAME)
            previous_session = 0;
        int changed =
            state != previous_state || identity_changed || (session && session != previous_session);
        if (changed || request != seen) {
            seen = request;
            memset(&pending, 0, sizeof(pending));
            pending.game = game;
            pending.state = state == VCO_LIST_READY ? VCO_LIST_LOADING : state;
            publish(NULL, NULL);
            if (state == VCO_LIST_READY) {
                VcoEncoding policy =
                    (VcoEncoding)__atomic_load_n(&encoding_policy, __ATOMIC_ACQUIRE);
                metadata(&game);
                SceSize bytes = (sizeof(VcoCatalogWorkspace) + 4095u) & ~4095u;
                SceUID block = sceKernelAllocMemBlock("vco_parse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                                      bytes, NULL);
                VcoCatalogWorkspace *work = NULL;
                SourceRead source = {0};
                if (block >= 0 && sceKernelGetMemBlockBase(block, (void **)&work) >= 0)
                    vco_catalog_load_encoded(&pending, &game, work, read_catalog, &source, policy);
                else {
                    pending.game = game;
                    pending.state = VCO_LIST_LIMIT;
                }
                VcoGame check = {0};
                VcoListState after = discover(&check);
                uint64_t after_session = after == VCO_LIST_READY ? candidate_session(&check) : 0;
                if (after != VCO_LIST_DISCOVERY_ERROR &&
                    (after != VCO_LIST_READY || !same_game(&check, &game) ||
                     (session && after_session && session != after_session))) {
                    memset(&pending, 0, sizeof(pending));
                    pending.state = after == VCO_LIST_READY ? VCO_LIST_LOADING : after;
                    /* Retry next pass rather than publish a departed game's rows. */
                    previous_state = VCO_LIST_LOADING;
                    previous = check;
                    previous_session = 0;
                    publish(NULL, NULL);
                    if (block >= 0)
                        sceKernelFreeMemBlock(block);
                    sceKernelDelayThread(250000);
                    continue;
                }
                /* Both observations must identify the same instance. A file
                 * loaded before the first token is display-only; token arrival
                 * forces a fresh read before any upload can execute its data. */
                pending.execution_session = session && session == after_session ? session : 0;
                session = pending.execution_session;
                previous_session = session;
                publish(pending.state == VCO_LIST_READY ? work : NULL,
                        source.attempted ? &source.stamp : NULL);
                if (block >= 0)
                    sceKernelFreeMemBlock(block);
            }
            vco_log("list_state", pending.state, 0);
            vco_log("list_count", pending.count, 0);
            vco_log("parse_error", pending.diagnostic.code, 0);
            vco_log("parse_line", pending.diagnostic.line, 0);
            vco_log("file_encoding_policy", pending.encoding_policy, 0);
            vco_log("file_encoding", pending.encoding, 0);
            vco_log("game_pid", (unsigned)game.pid, 0);
            previous = game;
            previous_state = state;
        }
        /* Keep an established token through transient zero-token observations.
         * A file read without matching before/after tokens deliberately resets
         * its baseline above so the first valid token triggers a new read. */
        if (session)
            previous_session = session;
        if (check != source_seen) {
            source_seen = check;
            check_source();
        }
        sceKernelDelayThread(250000);
    }
    return 0;
}
int vco_catalog_service_start(void)
{
    if (snapshot_lock >= 0)
        return -1;
    refresh_language();
    published.state = VCO_LIST_LOADING;
    snapshot_lock = sceKernelCreateMutex("vco_catalog", 0, 0, NULL);
    if (snapshot_lock < 0)
        return snapshot_lock;
    SceUID thread = sceKernelCreateThread("vco_catalog", worker, 160, 32768, 0, 0, NULL);
    int error = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (error < 0) {
        if (thread >= 0)
            sceKernelDeleteThread(thread);
        sceKernelDeleteMutex(snapshot_lock);
        snapshot_lock = -1;
        return error;
    }
    __atomic_store_n(&refresh_request, 1, __ATOMIC_RELEASE);
    vco_log("catalog_start_execution", sizeof(VcoCatalogWorkspace), 1);
    return 0;
}
void vco_catalog_service_refresh(void)
{
    __atomic_add_fetch(&refresh_request, 1, __ATOMIC_RELEASE);
}
void vco_catalog_service_encoding(VcoEncoding policy)
{
    if ((unsigned)policy <= VCO_ENCODING_GBK)
        __atomic_store_n(&encoding_policy, (uint32_t)policy, __ATOMIC_RELEASE);
}
void vco_catalog_service_check_source(void)
{
    __atomic_add_fetch(&source_request, 1, __ATOMIC_RELEASE);
}
int vco_catalog_service_source_copy(VcoCatalogSourceView *out)
{
    if (!out || snapshot_lock < 0 || sceKernelTryLockMutex(snapshot_lock, 1) < 0)
        return 0;
    int changed = out->revision != published_source.revision;
    if (changed)
        *out = published_source;
    sceKernelUnlockMutex(snapshot_lock, 1);
    return changed;
}
int vco_catalog_service_copy(VcoCatalog *out)
{
    if (!out || snapshot_lock < 0 || sceKernelTryLockMutex(snapshot_lock, 1) < 0)
        return 0;
    int changed = out->revision != published.revision;
    if (changed)
        *out = published;
    sceKernelUnlockMutex(snapshot_lock, 1);
    return changed;
}
int vco_catalog_service_language(void)
{
    return __atomic_load_n(&language, __ATOMIC_ACQUIRE);
}
