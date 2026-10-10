#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
/* Compile the production callback and boot path unchanged. Only SDK, view and
 * worker boundaries are faked; input/settings policy stays real. */
#define __dso_handle vco_shell_test_dso_handle
#include "../src/shell.cpp"
#undef __dso_handle

static uint64_t clock_us;
static VcoInput next_input;
static VcoTouchSample next_touch;
static VcoCatalog next_catalog;
static bool catalog_pending, fail_poll, save_accept;
static unsigned poll_calls, touch_calls, save_calls, command_calls, refresh_calls, render_calls,
    close_calls, capture_calls, registered, rendered_first;
static int last_capture, system_language, rendered_language;
static VcoSettings saved, loaded;
static void (*registered_tick)(void *);
static bool frame(bool visible = true);

extern "C" uint64_t sceKernelGetSystemTimeWide(void)
{
    return clock_us;
}
extern "C" int sceKernelDelayThread(unsigned)
{
    return 0;
}
extern "C" int sceKernelExitDeleteThread(int value)
{
    return value;
}
extern "C" SceUID sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, unsigned, int,
                                        int, void *)
{
    return 7;
}
extern "C" int sceKernelStartThread(SceUID, unsigned, void *)
{
    return 0;
}
extern "C" int sceKernelDeleteThread(SceUID)
{
    return 0;
}
extern "C" int sceIoOpen(const char *, int, int)
{
    return 9;
}
extern "C" int sceIoClose(SceUID)
{
    return 0;
}
extern "C" int sceTouchSetSamplingState(int, int)
{
    return 0;
}
void paf::common::MainThreadCallList::Register(void (*callback)(void *), void *)
{
    ++registered;
    registered_tick = callback;
}
extern "C" int vco_diagnostics_write(const char *, const char *, unsigned, int)
{
    return 0;
}
extern "C" int vcoPoll(VcoInput *out)
{
    ++poll_calls;
    if (fail_poll)
        return -1;
    *out = next_input;
    return 0;
}
extern "C" int vcoPollTouch(VcoTouchSample *out)
{
    ++touch_calls;
    *out = next_touch;
    return 0;
}
extern "C" int vcoCapture(int mode)
{
    ++capture_calls;
    last_capture = mode;
    return 0;
}
extern "C" int vco_catalog_service_start(void)
{
    return 0;
}
extern "C" void vco_catalog_service_refresh(void)
{
    ++refresh_calls;
}
extern "C" void vco_catalog_service_encoding(VcoEncoding) {}
extern "C" void vco_catalog_service_check_source(void) {}
extern "C" int vco_catalog_service_source_copy(VcoCatalogSourceView *)
{
    return 0;
}
extern "C" int vco_catalog_service_copy(VcoCatalog *out)
{
    if (!catalog_pending)
        return 0;
    catalog_pending = false;
    *out = next_catalog;
    return 1;
}
extern "C" int vco_catalog_service_language(void)
{
    return system_language;
}
extern "C" int vco_settings_service_start(VcoSettings *out)
{
    *out = loaded;
    return 0;
}
extern "C" bool vco_settings_service_request(const VcoSettings *value)
{
    ++save_calls;
    assert(vco_settings_valid(value));
    if (save_accept)
        saved = *value;
    return save_accept;
}
extern "C" VcoSaveStatus vco_settings_service_status(void)
{
    return VCO_SAVE_OK;
}
extern "C" int vco_execution_service_start(void)
{
    return 0;
}
extern "C" int vco_execution_service_copy(VcoExecutionView *)
{
    return 0;
}
extern "C" int vco_execution_service_set(uint64_t session, uint64_t list, unsigned entry, bool)
{
    assert(session == execution.session && list == execution.list_revision &&
           entry < execution.count);
    ++command_calls;
    return 1;
}
bool vco_view_open(uint32_t)
{
    return true;
}
void vco_view_close(uint32_t)
{
    ++close_calls;
}
void vco_view_render(const VcoCatalog &, const VcoExecutionView &, const VcoExecutionUi &,
                     const VcoCatalogSourceView &, unsigned, unsigned, int language,
                     VcoExecutionNotice)
{
    rendered_language = language;
    ++render_calls;
}
void vco_view_update_header_layout(void) {}
void vco_view_opacity(unsigned) {}
void vco_view_settings(const VcoSettingsState &, const VcoCatalog &, int, bool, unsigned first)
{
    rendered_first = first;
}
void vco_view_bindings(const VcoSettings &, int) {}
void vco_view_save_status(VcoSaveStatus, int, bool) {}

static void reset(void)
{
    memset(&input, 0, sizeof(input));
    memset(&input_bridge, 0, sizeof(input_bridge));
    memset(&catalog, 0, sizeof(catalog));
    memset(&catalog_source, 0, sizeof(catalog_source));
    memset(&lifecycle, 0, sizeof(lifecycle));
    memset(&execution, 0, sizeof(execution));
    memset(&commands, 0, sizeof(commands));
    memset(&settings, 0, sizeof(settings));
    settings.applied = loaded = vco_settings_defaults();
    saved = settings.applied;
    storage_available = save_accept = true;
    save_requested = reloading = catalog_pending = fail_poll = false;
    save_status = VCO_SAVE_OK;
    page_index = selected = 0;
    reload_revision = 0;
    notice = VCO_NOTICE_NONE;
    poll_calls = touch_calls = save_calls = command_calls = refresh_calls = render_calls =
        close_calls = capture_calls = registered = rendered_first = 0;
    last_capture = -1;
    system_language = rendered_system_language = 0;
    rendered_language = -1;
    registered_tick = NULL;
    clock_us = 1000;
    input.open = input.seeded = true;
    input.last_tick = (uint32_t)clock_us;
    catalog.state = VCO_LIST_READY;
    catalog.revision = 1;
    catalog.execution_session = 3;
    catalog.count = 2;
    catalog.game.pid = 42;
    catalog.game.app_id = 8;
    strcpy(catalog.game.title_id, "PCSA00001");
    execution.state = VCO_EXECUTION_READY;
    execution.catalog_revision = catalog.revision;
    execution.session = catalog.execution_session;
    execution.list_revision = 4;
    execution.count = catalog.count;
    execution.known[0] = execution.known[1] = true;
    (void)vco_ui_lifecycle_catalog(&lifecycle, &catalog);
    (void)vco_ui_lifecycle_execution(&lifecycle, &catalog, &execution);
    memset(&next_input, 0, sizeof(next_input));
    next_input.abi = VCO_ABI;
    next_input.captured = 1;
    next_input.lx = next_input.ly = next_input.rx = next_input.ry = 128;
    memset(&next_touch, 0, sizeof(next_touch));
    next_touch.abi = VCO_TOUCH_ABI;
    next_touch.available = next_touch.valid = VCO_TOUCH_FRONT | VCO_TOUCH_BACK;
}
static bool frame(bool visible)
{
    clock_us += 16000;
    next_input.tick = next_touch.tick = (uint32_t)clock_us;
    tick(NULL);
    assert(input.open == visible);
    return input.open;
}
static void button(uint32_t buttons)
{
    next_input.buttons = buttons;
    (void)frame();
}
static void begin_capture(unsigned row)
{
    vco_settings_begin(&settings);
    settings.selected = row;
    button(0); /* settle any previous capture or apply transaction */
    button(vco_settings_confirm_button(&settings.applied));
    assert(settings.capture.active && input.wait_neutral);
    button(0);
    assert(settings.capture.active);
}
static void pending_apply(void)
{
    vco_settings_begin(&settings);
    settings.draft.opacity = 35;
    settings.selected = VCO_SETTING_APPLY;
    button(vco_settings_confirm_button(&settings.applied));
    assert(settings.apply_pending && settings.open && settings.applied.opacity == 70);
    assert(!save_calls && input.wait_neutral);
}
static void safety_test(void)
{
    for (unsigned phase = 0; phase < 2; ++phase) {
        for (unsigned fault = 0; fault < 5; ++fault) {
            reset();
            if (phase)
                pending_apply();
            else
                begin_capture(VCO_SETTING_OPEN);
            if (!fault) {
                next_input.buttons = VCO_PS;
                frame();
                next_input.buttons = 0;
            } else if (fault == 1)
                next_input.captured = 0;
            else if (fault == 2)
                clock_us += VCO_LEASE_US;
            else if (fault == 3) {
                unsigned before = poll_calls;
                next_catalog = catalog;
                next_catalog.state = VCO_LIST_NO_GAME;
                catalog_pending = true;
                frame(false);
                assert(poll_calls == before && last_capture == VCO_CAPTURE_OFF);
            } else
                fail_poll = true;
            if (fault != 3)
                frame(false);
            assert(!settings.open && !settings.capture.active && !settings.apply_pending);
            assert(settings.applied.opacity == 70 && !save_calls && !command_calls && close_calls);
            assert(capture_calls && last_capture != VCO_CAPTURE_ACTIVE);
        }
    }
}
static void capture_test(void)
{
    reset();
    begin_capture(VCO_SETTING_OPEN);
    button(VCO_CROSS);
    button(0);
    assert(settings.capture.active && settings.capture_error == VCO_BINDING_COMBO_REQUIRED);
    assert(settings.applied.bindings.open == VCO_COMBO && !command_calls && !save_calls);
    button(0);
    button(VCO_R | VCO_SELECT);
    button(VCO_R);
    assert(settings.capture.active && settings.draft.bindings.open == VCO_COMBO);
    button(0);
    assert(!settings.capture.active && settings.draft.bindings.open == (VCO_R | VCO_SELECT));
    assert(settings.applied.bindings.open == VCO_COMBO && !command_calls && !save_calls);
    assert(input.wait_neutral);
    button(VCO_CROSS);
    assert(!command_calls && settings.open);
    button(0);
    begin_capture(VCO_SETTING_OPEN);
    button(VCO_DOWN);
    button(0);
    assert(!settings.capture.active && settings.open && !command_calls);
}
static void touch_test(void)
{
    reset();
    frame();
    for (unsigned i = 0; i < 100; ++i) {
        next_touch.front_count = next_touch.back_count = 1;
        next_touch.x = (uint16_t)(i * 9);
        next_touch.y = (uint16_t)(i * 5);
        frame();
    }
    assert(!touch_calls && !command_calls && selected == 0 && !settings.open);
    begin_capture(VCO_SETTING_OPEN);
    button(VCO_R | VCO_SELECT);
    button(0);
    assert(!settings.capture.active && settings.draft.bindings.open == (VCO_R | VCO_SELECT));
    assert(!touch_calls && !command_calls);
}
static void apply_test(void)
{
    reset();
    pending_apply();
    next_input.buttons = vco_settings_confirm_button(&settings.applied);
    frame();
    assert(settings.apply_pending && !save_calls);
    next_input.buttons = 0;
    next_input.lx = 160;
    frame();
    assert(settings.apply_pending && settings.applied.opacity == 70 && !save_calls);
    next_input.lx = 128;
    next_touch.front_count = 1;
    next_touch.x = 500;
    next_touch.y = 300;
    frame();
    assert(!settings.apply_pending && !settings.open && !touch_calls && input.wait_neutral);
    next_touch.front_count = 0;
    frame();
    assert(!settings.open && !settings.apply_pending && settings.applied.opacity == 35);
    assert(saved.opacity == 35 && save_calls == 1 && !command_calls && !input.wait_neutral);
    frame();
    frame();
    assert(save_calls == 1 && !command_calls);

    /* Worker BUSY preserves the requested value without changing the map again. */
    reset();
    pending_apply();
    save_accept = false;
    button(0);
    assert(!settings.open && save_requested && save_calls == 1 && settings.applied.opacity == 35);
    save_accept = true;
    frame();
    assert(!save_requested && save_calls == 2 && saved.opacity == 35);
    frame();
    assert(save_calls == 2);
}
static void mapping_test(void)
{
    for (VcoButtonMode initial : {VCO_BUTTON_ASIAN, VCO_BUTTON_WESTERN}) {
        reset();
        vco_settings_set_button_mode(&settings.applied, initial);
        const uint32_t old_confirm = vco_settings_confirm_button(&settings.applied);
        const uint32_t old_back = vco_settings_back_button(&settings.applied);
        vco_settings_begin(&settings);
        settings.selected = VCO_SETTING_BUTTON_MODE;
        button(VCO_RIGHT);
        button(0);
        const VcoSettings mapping = settings.draft;
        assert(mapping.button_mode != initial && settings.applied.button_mode == initial);
        /* Draft changes cannot switch the controls or bypass the unsaved prompt. */
        button(old_back);
        assert(settings.confirm);
        button(0);
        button(old_back);
        assert(!settings.confirm && settings.open);
        button(0);
        settings.selected = VCO_SETTING_APPLY;
        button(old_confirm);
        assert(settings.apply_pending && settings.applied.button_mode == initial);
        button(old_back); /* old Back is also the new Confirm: consume it. */
        assert(settings.apply_pending && !command_calls && !save_calls);
        button(0);
        assert(!settings.open && vco_settings_equal(&settings.applied, &mapping));
        assert(save_calls == 1 && vco_settings_equal(&saved, &mapping));
        const uint32_t confirm = vco_settings_confirm_button(&mapping);
        const uint32_t back = vco_settings_back_button(&mapping);
        button(confirm); /* commit still requires a fresh neutral frame */
        assert(!command_calls);
        button(0);
        button(confirm);
        button(0);
        button(VCO_START);
        button(0);
        assert(!command_calls && !settings.open);
        button(VCO_SQUARE);
        assert(command_calls == 1);
        button(VCO_SQUARE);
        assert(command_calls == 1); /* Holding Square never repeats the toggle. */
        button(0);
        button(VCO_TRIANGLE);
        assert(settings.open);
        button(0);
        settings.selected = VCO_SETTING_BUTTON_MODE;
        button(VCO_SQUARE);
        assert(settings.draft.button_mode == mapping.button_mode && command_calls == 1);
        button(0);
        button(confirm);
        assert(settings.draft.button_mode == initial);
        button(0);
        button(back);
        assert(settings.confirm);
        button(0);
        settings.selected = 1; /* Discard uses the same Confirm. */
        button(confirm);
        assert(!settings.open && settings.applied.button_mode == mapping.button_mode);
        button(0);
        next_input.buttons = back;
        frame(false);
        assert(last_capture == VCO_CAPTURE_DRAIN && close_calls == 1);
    }
}
static void startup_test(void)
{
    reset();
    input.open = false;
    loaded.bindings.open = VCO_R | VCO_START;
    assert(vco_settings_valid(&loaded));
    assert(boot(0, NULL) == 0);
    assert(registered == 1 && registered_tick == tick && input.wait_neutral);
    assert(settings.applied.bindings.open == loaded.bindings.open);
    next_input.captured = 0;
    next_input.buttons = loaded.bindings.open;
    frame(false);
    assert(input.wait_neutral && !capture_calls);
    next_input.buttons = 0;
    frame(false);
    assert(!input.wait_neutral && !capture_calls);
    next_input.buttons = loaded.bindings.open;
    frame();
    assert(last_capture == VCO_CAPTURE_ACTIVE && capture_calls == 1 && !command_calls);
}
static void screenshot_test(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        reset();
        if (mode == 1)
            begin_capture(VCO_SETTING_OPEN);
        if (mode == 2)
            pending_apply();
        button(VCO_PS);
        button(VCO_PS | VCO_START);
        for (unsigned i = 0; i < 40; ++i)
            frame();
        assert(!command_calls && !close_calls && !save_calls);
        if (mode == 1)
            assert(settings.capture.active);
        if (mode == 2)
            assert(settings.apply_pending);
        button(VCO_START);
        button(0);
        assert(input.open && !close_calls && !command_calls);
        if (!mode) {
            button(0);
            button(VCO_TRIANGLE);
            assert(settings.open);
        }
    }
}
static void opacity_test(void)
{
    reset();
    vco_settings_begin(&settings);
    settings.selected = VCO_SETTING_OPACITY;
    next_touch.front_count = next_touch.back_count = 1;
    for (unsigned i = 0; i < 14; ++i) {
        button(VCO_LEFT);
        button(0);
    }
    assert(settings.draft.opacity == 0 && settings.applied.opacity == 70);
    for (unsigned i = 0; i < 20; ++i) {
        button(VCO_RIGHT);
        button(0);
    }
    assert(settings.draft.opacity == 100 && !save_calls && !touch_calls);
    settings.selected = VCO_SETTING_APPLY;
    button(vco_settings_confirm_button(&settings.applied));
    button(0);
    assert(!settings.open && settings.applied.opacity == 100 && save_calls == 1);
    assert(!touch_calls && !command_calls);
}
static void language_test(void)
{
    reset();
    system_language = 1;
    frame();
    assert(rendered_language == 1);
    unsigned before = render_calls;
    system_language = 2;
    frame();
    assert(render_calls == before + 1 && rendered_language == 2);
    system_language = 3;
    frame();
    assert(rendered_language == 3);
    system_language = 4;
    frame();
    assert(rendered_language == 4);
    settings.applied.language = 1;
    render();
    assert(rendered_language == 1);
    system_language = 0;
    frame();
    assert(rendered_language == 1); /* Explicit choice wins over the system. */
    assert(!command_calls && !save_calls);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "language"))
        language_test();
    else if (!strcmp(argv[1], "safety"))
        safety_test();
    else if (!strcmp(argv[1], "capture"))
        capture_test();
    else if (!strcmp(argv[1], "touch"))
        touch_test();
    else if (!strcmp(argv[1], "apply"))
        apply_test();
    else if (!strcmp(argv[1], "startup"))
        startup_test();
    else if (!strcmp(argv[1], "mapping"))
        mapping_test();
    else if (!strcmp(argv[1], "screenshot"))
        screenshot_test();
    else if (!strcmp(argv[1], "opacity"))
        opacity_test();
    else
        assert(false);
    puts("production Shell binding integration passed");
    return 0;
}
