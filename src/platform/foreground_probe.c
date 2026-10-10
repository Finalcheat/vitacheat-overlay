#include "foreground_probe.h"
#include <psp2/appmgr.h>
#include <string.h>

typedef char sample_size_check[sizeof(VcoForegroundSample) == 48 ? 1 : -1];

/* VitaSDK DB: SceAppMgr lib 0x8AF17416, fn 0x332C9D98; the SDK header
 * currently omits this declaration. PSVshellPlus v1.4 source + published
 * binary establish ACTIVE=-4 and GAME=-3. See docs/FOREGROUND-RESEARCH.zh-CN.md
 * for SHA256, import table and the two call sites; these are not guessed. */
int32_t sceAppMgrGetAppIdByAppId(int32_t app_id);
enum { VCO_APP_SELECTOR_ACTIVE = -4, VCO_APP_SELECTOR_GAME = -3 };

static uint32_t observe(VcoForegroundSample *sample)
{
    sample->active_app_id = sceAppMgrGetAppIdByAppId(VCO_APP_SELECTOR_ACTIVE);
    if (sample->active_app_id <= 0)
        return VCO_FOREGROUND_FAILURE_QUERY;
    sample->game_app_id = sceAppMgrGetAppIdByAppId(VCO_APP_SELECTOR_GAME);
    if (sample->game_app_id <= 0)
        return VCO_FOREGROUND_FAILURE_QUERY;
    sample->active_pid = sceAppMgrGetProcessIdByAppIdForShell(sample->active_app_id);
    if (sample->active_pid <= 0)
        return VCO_FOREGROUND_FAILURE_QUERY;
    sample->game_pid = sceAppMgrGetProcessIdByAppIdForShell(sample->game_app_id);
    if (sample->game_pid <= 0)
        return VCO_FOREGROUND_FAILURE_QUERY;
    /* App IDs and PIDs must agree about whether this is the same application. */
    if ((sample->active_app_id == sample->game_app_id) != (sample->active_pid == sample->game_pid))
        return VCO_FOREGROUND_FAILURE_IDENTITY;
    char title[32]; /* SDK SCE_APPMGR_MAX_APP_NAME_LENGTH is 31 plus NUL. */
    memset(title, 0xff, sizeof(title));
    if (sceAppMgrGetNameById(sample->game_pid, title) < 0)
        return VCO_FOREGROUND_FAILURE_QUERY;
    if (title[9] || !memcmp(title, "NPXS", 4))
        return VCO_FOREGROUND_FAILURE_IDENTITY;
    for (unsigned i = 0; i < 9; ++i)
        if (!((title[i] >= 'A' && title[i] <= 'Z') || (title[i] >= '0' && title[i] <= '9')))
            return VCO_FOREGROUND_FAILURE_IDENTITY;
    memcpy(sample->game_title_id, title, sizeof(sample->game_title_id));
    return VCO_FOREGROUND_FAILURE_NONE;
}

void vco_foreground_sample(VcoForegroundSample *out)
{
    if (!out)
        return;
    *out = (VcoForegroundSample){.abi = VCO_FOREGROUND_PROBE_ABI, .size = sizeof(*out)};
    VcoForegroundSample first = {0}, second = {0};
    out->failure = observe(&first);
    if (out->failure)
        return;
    out->failure = observe(&second);
    if (out->failure)
        return;
    if (first.active_app_id != second.active_app_id || first.game_app_id != second.game_app_id ||
        first.active_pid != second.active_pid || first.game_pid != second.game_pid ||
        memcmp(first.game_title_id, second.game_title_id, sizeof(first.game_title_id))) {
        out->failure = VCO_FOREGROUND_FAILURE_CHANGED;
        return;
    }
    first.abi = VCO_FOREGROUND_PROBE_ABI;
    first.size = sizeof(first);
    first.status = first.active_app_id == first.game_app_id ? VCO_FOREGROUND_CANDIDATE
                                                            : VCO_BACKGROUND_CANDIDATE;
    /* Two equal observations still do not exclude an intervening switch/ABA
     * or a switch immediately after this publication. No authority is granted. */
    *out = first;
}
