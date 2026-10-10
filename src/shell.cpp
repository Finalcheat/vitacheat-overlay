#include "storage_paths.h"
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/modulemgr.h>
#include <string.h>
#include <paf/std/ngp/string.h>
#include <paf/std/ngp/wchar.h>
#undef WEOF
#include <paf/common/call_list.h>
#include "bridge.h"
#include "input_state.h"
#include "input/bridge_policy.h"
#include "platform/catalog_service.h"
#include "platform/settings_service.h"
#include "platform/resource_profile.h"
#include "ui/overlay_view.h"
#include "ui/lifecycle.h"
#include "diagnostics.h"
extern "C" {
void *__dso_handle = 0;
}
static VcoInputState input;
static VcoInputBridge input_bridge;
static const VcoInputBridgeOps input_ops = {vcoPoll, NULL, vcoCapture};
static VcoCatalog catalog;
static VcoCatalogSourceView catalog_source;
static VcoUiLifecycle lifecycle;
static unsigned page_index, selected;
static VcoExecutionView execution;
static VcoExecutionUi commands;
static VcoExecutionNotice notice;
static bool reloading;
static uint32_t reload_revision;
static VcoSettingsState settings;
static bool save_requested, storage_available;
static VcoSaveStatus save_status;
static int rendered_system_language;
static unsigned settings_first(void)
{
    return !settings.confirm && settings.selected >= VCO_PAGE_SIZE
               ? settings.selected - VCO_PAGE_SIZE + 1
               : 0;
}
static void service_settings(void)
{
    vco_catalog_service_encoding(settings.applied.encoding);
    if (save_requested && vco_settings_service_request(&settings.applied))
        save_requested = false;
    VcoSaveStatus current = !storage_available ? VCO_SAVE_FAILED
                            : save_requested   ? VCO_SAVE_PENDING
                                               : vco_settings_service_status();
    save_status = current;
}
static void render()
{
    const int system_language = vco_catalog_service_language();
    rendered_system_language = system_language;
    vco_view_render(catalog, execution, commands, catalog_source, page_index, selected,
                    vco_settings_language(&settings.applied, system_language),
                    reloading ? VCO_NOTICE_RELOADING : notice);
    vco_view_bindings(settings.applied, system_language);
    unsigned entry = page_index * VCO_PAGE_SIZE + selected;
    if (!settings.open && !reloading && notice == VCO_NOTICE_NONE &&
        execution.state == VCO_EXECUTION_READY && !execution.error &&
        (catalog_source.catalog_revision != catalog.revision ||
         !vco_catalog_source_text(catalog_source.state, 0)[0]) &&
        vco_execution_view_matches(&catalog, &execution) && entry < execution.count &&
        execution.known[entry] && !execution.rows[entry].error)
        vco_view_save_status(save_status,
                             vco_settings_language(&settings.applied, system_language));
    vco_view_opacity(settings.applied.opacity);
    if (settings.open)
        vco_view_settings(settings, catalog, system_language, reloading, settings_first());
    if (settings.open && !settings.capture.active && !settings.apply_pending &&
        vco_settings_binding_row(settings.selected) < 0 &&
        (settings.confirm ||
         (settings.selected != VCO_SETTING_RELOAD && settings.selected != VCO_SETTING_ENCODING &&
          settings.selected != VCO_SETTING_OPACITY)) &&
        (vco_settings_equal(&settings.draft, &settings.applied) ||
         save_status == VCO_SAVE_PENDING || save_status >= VCO_SAVE_FAILED))
        vco_view_save_status(save_status, vco_settings_language(&settings.draft, system_language),
                             true);
}
static void clamp_selection(void)
{
    unsigned pages = catalog.count ? (catalog.count + VCO_PAGE_SIZE - 1) / VCO_PAGE_SIZE : 1;
    if (page_index >= pages)
        page_index = pages - 1;
    while (selected && page_index * VCO_PAGE_SIZE + selected >= catalog.count)
        --selected;
}
static bool update_snapshots(void)
{
    bool departed = false, redraw = false;
    int32_t old_pid = catalog.game.pid, old_app = catalog.game.app_id;
    char old_title[sizeof(catalog.game.title_id)];
    memcpy(old_title, catalog.game.title_id, sizeof(old_title));
    if (vco_catalog_service_copy(&catalog)) {
        departed = vco_ui_lifecycle_catalog(&lifecycle, &catalog);
        if (old_pid != catalog.game.pid || old_app != catalog.game.app_id ||
            memcmp(old_title, catalog.game.title_id, sizeof(old_title))) {
            vco_settings_cancel(&settings);
            page_index = 0;
            selected = 0;
            reloading = false;
        }
        if (reloading && catalog.revision != reload_revision && catalog.state != VCO_LIST_LOADING)
            reloading = false;
        notice = VCO_NOTICE_NONE;
        clamp_selection();
        redraw = true;
    }
    if (vco_execution_service_copy(&execution)) {
        departed |= vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution);
        vco_execution_ui_sync(&commands, &execution);
        redraw = true;
    }
    if (vco_catalog_service_source_copy(&catalog_source))
        redraw = true;
    if (redraw && input.open && !departed)
        render();
    return departed;
}
static void tick(void *)
{
    uint32_t now = (uint32_t)sceKernelGetSystemTimeWide();
    /* Lifecycle publications must be observed even while input RPC is BUSY.
     * Hide first and queue OFF; the release policy blocks all renewing Polls. */
    if (update_snapshots()) {
        vco_settings_cancel(&settings);
        vco_view_close(now);
        vco_input_forced_closed(&input);
        vco_input_bridge_release(&input_bridge, VCO_CAPTURE_OFF, now);
        (void)vco_input_bridge_retry(&input_bridge, now, input_ops);
        return;
    }
    if (input.open)
        vco_view_update_header_layout();
    VcoInput raw = {};
    VcoTouchSample sample = {};
    VcoInputBridgeResult result =
        vco_input_bridge_poll(&input_bridge, now, false, input_ops, &raw, &sample);
    if (result == VCO_INPUT_WAIT)
        return;
    if (result == VCO_INPUT_FAILED) {
        vco_settings_cancel(&settings);
        vco_input_bridge_release(&input_bridge, VCO_CAPTURE_OFF, now);
        (void)vco_input_bridge_retry(&input_bridge, now, input_ops);
        vco_view_close(input.last_tick);
        input.open = false;
        input.draining = false;
        input.seeded = false;
        input.wait_neutral = false;
        return;
    }
    VcoSaveStatus previous_save = save_status;
    service_settings();
    if (input.open && (previous_save != save_status ||
                       rendered_system_language != vco_catalog_service_language()))
        render();
    bool neutral = vco_neutral(&raw);
    bool system_input = input.system_pending;
    VcoAction action =
        vco_input_step_with_capture(&input, &raw, settings.open, &settings.applied, true,
                                    settings.capture.active || settings.apply_pending);
    if ((system_input || input.system_pending) && action != VCO_CLOSE && action != VCO_RELEASE)
        return;
    if (settings.open && action != VCO_CLOSE && action != VCO_RELEASE) {
        /* Changing the active map is a single neutral-frame transaction. No
         * action from the old mapping is dispatched in its commit frame. */
        if (settings.apply_pending) {
            if (vco_settings_commit(&settings, neutral)) {
                input.wait_neutral = true;
                save_requested = storage_available;
                service_settings();
                render();
            }
            return;
        }
        if (settings.capture.active) {
            if (action == VCO_BACK) {
                vco_settings_capture_cancel(&settings);
                input.wait_neutral = true;
                render();
                return;
            }
            VcoBindingCaptureResult captured =
                vco_settings_capture_step(&settings, raw.buttons, neutral);
            if (captured != VCO_BIND_CAPTURE_NONE) {
                if (captured == VCO_BIND_CAPTURE_CHANGED || captured == VCO_BIND_CAPTURE_CANCELLED)
                    input.wait_neutral = true;
                render();
            }
            return;
        }
        int binding = settings.confirm ? -1 : vco_settings_binding_row(settings.selected);
        if (binding >= 0 &&
            (action == VCO_TOGGLE || action == VCO_ADJUST_LEFT || action == VCO_ADJUST_RIGHT)) {
            if (vco_settings_capture_begin(&settings, (unsigned)binding)) {
                input.wait_neutral = true;
                render();
            }
            return;
        }
        switch (action) {
        case VCO_MOVE_UP:
            vco_settings_move(&settings, -1);
            break;
        case VCO_MOVE_DOWN:
            if (!settings.confirm && settings.selected == VCO_SETTING_COUNT - 1)
                settings.selected = VCO_SETTING_RELOAD;
            else
                vco_settings_move(&settings, 1);
            break;
        case VCO_ADJUST_LEFT:
            vco_settings_adjust(&settings, -1);
            break;
        case VCO_ADJUST_RIGHT:
            vco_settings_adjust(&settings, 1);
            break;
        case VCO_TOGGLE: {
            if (!settings.confirm && settings.selected == VCO_SETTING_RELOAD) {
                if (!reloading) {
                    reload_revision = catalog.revision;
                    reloading = true;
                    notice = VCO_NOTICE_RELOADING;
                    vco_catalog_service_refresh();
                }
                break;
            }
            vco_settings_accept(&settings);
            if (settings.apply_pending) {
                input.wait_neutral = true;
            }
            break;
        }
        case VCO_BACK:
        case VCO_SETTINGS:
            vco_settings_back(&settings);
            break;
        default:
            return;
        }
        render();
        return;
    }
    switch (action) {
    case VCO_OPEN:
        notice = VCO_NOTICE_NONE;
        /* Opening the view never rebuilds the list or replays its enabled rows. */
        if (vco_view_open(raw.tick)) {
            if (vco_input_bridge_open(&input_bridge, input_ops)) {
                input.open = true;
                vco_catalog_service_check_source();
                render();
            } else
                vco_view_close(raw.tick);
        }
        break;
    case VCO_SETTINGS:
        vco_settings_begin(&settings);
        render();
        break;
    case VCO_CLOSE:
        /* Never renew a capture whose lease has already expired. A BUSY close
         * remains queued; subsequent frames retry it before any Poll heartbeat. */
        vco_input_bridge_release(
            &input_bridge,
            (raw.buttons & VCO_PS) || !raw.captured ? VCO_CAPTURE_OFF : VCO_CAPTURE_DRAIN, now);
        (void)vco_input_bridge_retry(&input_bridge, now, input_ops);
        vco_settings_cancel(&settings);
        vco_view_close(raw.tick);
        break;
    case VCO_RELEASE:
        vco_input_bridge_release(&input_bridge, VCO_CAPTURE_OFF, now);
        (void)vco_input_bridge_retry(&input_bridge, now, input_ops);
        break;
    case VCO_MOVE_UP:
        if (selected)
            --selected;
        notice = VCO_NOTICE_NONE;
        render();
        break;
    case VCO_MOVE_DOWN:
        if (selected + 1 < VCO_PAGE_SIZE &&
            page_index * VCO_PAGE_SIZE + selected + 1 < catalog.count)
            ++selected;
        notice = VCO_NOTICE_NONE;
        render();
        break;
    case VCO_PAGE_PREV:
        if (page_index)
            --page_index;
        notice = VCO_NOTICE_NONE;
        clamp_selection();
        render();
        break;
    case VCO_PAGE_NEXT:
        if ((page_index + 1) * VCO_PAGE_SIZE < catalog.count)
            ++page_index;
        notice = VCO_NOTICE_NONE;
        clamp_selection();
        render();
        break;
    case VCO_TOGGLE: {
        unsigned entry = page_index * VCO_PAGE_SIZE + selected;
        bool enabled = false;
        notice = VCO_NOTICE_UNAVAILABLE;
        if (!reloading &&
            vco_execution_toggle_target(&catalog, &execution, &commands, entry, &enabled)) {
            int submitted = vco_execution_service_set(execution.session, execution.list_revision,
                                                      entry, enabled);
            if (submitted == 1) {
                vco_execution_ui_submitted(&commands, &execution, entry);
                notice = VCO_NOTICE_NONE;
            } else if (!submitted)
                notice = VCO_NOTICE_BUSY;
        }
        render();
        break;
    }
    default:
        break;
    }
}
static int boot(SceSize, void *)
{
    sceKernelDelayThread(5000000);
    storage_available = vco_settings_service_start(&settings.applied) >= 0;
    /* The persisted opening chord must be released before its first use. */
    input.wait_neutral = true;
    service_settings();
    int result = vco_execution_service_start();
    vco_log("execution_start_result", (unsigned)result, 0);
    if (result < 0) {
        execution.state = VCO_EXECUTION_UNAVAILABLE;
        execution.error = VCO_E_IO;
    }
    result = vco_catalog_service_start();
    vco_log("catalog_start_result", (unsigned)result, 0);
    if (result < 0)
        catalog.state = VCO_LIST_DISCOVERY_ERROR;
    paf::common::MainThreadCallList::Register(tick, NULL);
    vco_log("tick_registered_execution", 1, 0);
    return sceKernelExitDeleteThread(0);
}
extern "C" int module_start(SceSize, const void *)
{
#ifdef VCO_RESOURCE_PROFILE
    vco_log(VCO_RESOURCE_PROFILE_START_MARKER, VCO_RESOURCE_PROFILE_REVISION, 1);
#else
    vco_log("shell_start_execution", 0, 1);
#endif
    settings.applied = vco_settings_defaults();
    catalog.state = VCO_LIST_LOADING;
    execution.state = VCO_EXECUTION_LOADING;
    int fd = sceIoOpen(VCO_RESOURCE_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return SCE_KERNEL_START_FAILED;
    sceIoClose(fd);
    SceUID thread = sceKernelCreateThread("vco_boot", boot, 248, 16384, 0, 0, NULL);
    if (thread < 0)
        return SCE_KERNEL_START_FAILED;
    if (sceKernelStartThread(thread, 0, NULL) < 0) {
        sceKernelDeleteThread(thread);
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}
extern "C" int module_stop(SceSize, const void *)
{
    return SCE_KERNEL_STOP_CANCEL;
}
