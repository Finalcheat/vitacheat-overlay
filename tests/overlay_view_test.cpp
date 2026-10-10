#include <cassert>
#include <cmath>
#include "../src/ui/overlay_view.cpp"
unsigned paf::Plugin::creations, paf::Plugin::fail_on;
bool paf::Plugin::template_failure;
unsigned paf::ui::Widget::lookups, paf::ui::Widget::fail_lookup;
extern "C" int text_set_string(ui::Widget *widget, const paf::wstring &value, int)
{
    widget->value = value.value;
    return 0;
}
extern "C" int vco_diagnostics_write(const char *, const char *, unsigned, int)
{
    return 0;
}
static void same(float actual, float expected)
{
    assert(std::fabs(actual - expected) < 0.001f);
}
static void hidden(void)
{
    for (unsigned i = 0; i < OPACITY_PART_COUNT; ++i)
        same(opacity_parts[i]->color.w, 0);
    assert(opacity_preview->value.empty());
}
static void focus_at(unsigned row, bool visible)
{
    float y = 272 - vco_ui_row_center(row);
    same(highlight->pos.y, y);
    same(focus_marker->pos.y, y);
    assert((highlight->color.w > 0) == visible);
    assert((focus_marker->color.w > 0) == visible);
}
static void list(void)
{
    VcoCatalog catalog = {};
    VcoExecutionView execution = {};
    VcoExecutionUi commands = {};
    VcoCatalogSourceView source = {};
    vco_view_render(catalog, execution, commands, source, 0, 0, 0, VCO_NOTICE_NONE);
}
static void appearance(void)
{
    assert(vco_view_open(1000));
    hidden();
    VcoCatalog catalog = {};
    for (int language = 0; language < VCO_LANGUAGE_COUNT; ++language) {
        for (unsigned opacity = 0; opacity <= 100; opacity += 5) {
            for (unsigned first = 0; first <= 1; ++first) {
                VcoSettingsState settings = {};
                settings.applied = vco_settings_defaults();
                vco_settings_begin(&settings);
                settings.draft.language = language;
                settings.draft.opacity = opacity;
                settings.selected = VCO_SETTING_OPACITY;
                vco_view_opacity(opacity);
                vco_view_settings(settings, catalog, 0, false, first);
                focus_at(settings.selected - first, true);
                same(background->color.w, 1);
                same(opacity_parts[OPACITY_TINT]->color.w, opacity / 100.0f);
                same(opacity_parts[OPACITY_LIGHT]->color.w, 1);
                same(opacity_parts[OPACITY_DARK]->color.w, 1);
                same(opacity_preview->color.w, 1);
                same(opacity_parts[OPACITY_THUMB]->pos.x, 520 + 248 * opacity / 100.0f - 480);
                same(opacity_parts[OPACITY_THUMB]->pos.y, 272 - vco_ui_row_center(1 - first));
                same(opacity_parts[OPACITY_FILL]->size.x, 248 * opacity / 100.0f);
                same(names[1 - first]->size.x, 244);
                assert(!opacity_preview->value.empty() && !message->value.empty());
                for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i)
                    same(states[i]->color.w, 1);
                /* Confirm/scroll/list transitions must remove every preview
                 * plane and restore ordinary label widths. */
                settings.confirm = true;
                settings.selected = 0;
                vco_view_settings(settings, catalog, 0, false, 0);
                hidden();
                same(background->color.w, 1);
                settings.confirm = false;
                settings.selected = VCO_SETTING_OPEN;
                vco_view_settings(settings, catalog, 0, false, 2);
                hidden();
                list();
                vco_view_opacity(opacity);
                hidden();
                same(background->color.w, opacity / 100.0f);
                for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i)
                    same(names[i]->size.x, 668);
            }
        }
    }
    vco_view_close(2000);
    assert(plugin->widgets.empty());
}
static void allocation_failures(void)
{
    assert(vco_view_open(1000));
    unsigned count = Plugin::creations;
    vco_view_close(2000);
    unsigned tick = 1000000;
    /* Fail each added plane/text allocation in the actual PageOpen path. */
    for (unsigned i = 1; i <= count; ++i) {
        Plugin::fail_on = i;
        assert(!vco_view_open(tick));
        assert(!page && plugin->widgets.empty());
        tick += 1000000;
    }
    Plugin::fail_on = 0;
    assert(vco_view_open(tick));
    hidden();
    vco_view_close(tick + 1);
    Plugin::template_failure = true;
    tick += 1000000;
    assert(!vco_view_open(tick));
    assert(!page && plugin->widgets.empty());
    Plugin::template_failure = false;
    // Missing group and each of the 18 expected template planes must close
    // the page before any partially bound switch is dereferenced.
    for (unsigned i = 1; i <= 19; ++i) {
        ui::Widget::fail_lookup = i;
        tick += 1000000;
        assert(!vco_view_open(tick));
        assert(!page && plugin->widgets.empty());
    }
    ui::Widget::fail_lookup = 0;
}
static void switches(void)
{
    assert(vco_view_open(1000));
    VcoCatalog catalog = {};
    VcoExecutionView execution = {};
    VcoExecutionUi commands = {};
    VcoCatalogSourceView source = {};
    catalog.state = VCO_LIST_READY;
    catalog.count = 2;
    catalog.revision = 5;
    catalog.execution_session = 7;
    catalog.source = VCO_SOURCE_VITACHEAT;
    catalog.encoding = VCO_ENCODING_UTF8;
    std::strcpy(catalog.game.title, "A long game title that must fit its own header column");
    std::strcpy(catalog.game.title_id, "PCSB00031");
    std::strcpy(catalog.game.version, "01.02");
    execution.session = 7;
    execution.list_revision = 9;
    execution.catalog_revision = 5;
    execution.count = 2;
    execution.state = VCO_EXECUTION_READY;
    execution.known[0] = execution.known[1] = true;
    execution.rows[0].state = VCO_ENTRY_ON;
    execution.rows[1].state = VCO_ENTRY_OFF;
    unsigned focused = 0;
    auto render = [&]() {
        vco_view_render(catalog, execution, commands, source, 0, focused, 1, VCO_NOTICE_NONE);
    };
    render();
    focus_at(0, true);
    unsigned widgets_before_navigation = Plugin::creations;
    for (ui::Widget *header : {title, game_version, eyebrow, identity}) {
        same(static_cast<ui::Text *>(header)->font_size, 18);
        assert(!static_cast<ui::Text *>(header)->bold);
        assert(header->style == VCO_HEADER_STYLE);
        assert(!static_cast<ui::Text *>(header)->wrap);
        assert(static_cast<ui::Text *>(header)->ellipsis);
        same(header->color.x, title->color.x);
        same(header->color.y, title->color.y);
        same(header->color.z, title->color.z);
        same(header->color.w, 1);
    }
    same(static_cast<ui::Text *>(names[0])->font_size, 24);
    assert(static_cast<ui::Text *>(names[0])->bold);
    same(static_cast<ui::Text *>(numbers[0])->font_size, 16);
    assert(!static_cast<ui::Text *>(numbers[0])->bold);
    assert(numbers[0]->style == VCO_INDEX_STYLE);
    same(enabled_background[0]->color.w, 0.60f);
    same(enabled_background[1]->color.w, 0);
    assert(names[0]->color.x < names[1]->color.x);
    assert(names[0]->color.y > names[0]->color.z);
    same(focus_marker->color.w, 1);
    assert(focus_marker->pos.x + focus_marker->size.x / 2 <
           numbers[0]->pos.x - numbers[0]->size.x / 2);
    same(switch_on[0]->color.w, 1);
    same(switch_off[0]->color.w, 0);
    same(switch_thumb[0]->pos.x, 434);
    same(switch_on[1]->color.w, 0);
    same(switch_off[1]->color.w, 1);
    same(switch_thumb[1]->pos.x, 398);
    assert(switch_on[0]->texture_id == switch_on[1]->texture_id);
    assert(switch_on[0]->texture_id == "vco_texture_track_on");
    assert(switch_off[0]->texture_id == "vco_texture_track_off");
    assert(switch_thumb[0]->texture_id == "vco_texture_thumb");
    focused = 1;
    render();
    focus_at(1, true);
    assert(Plugin::creations == widgets_before_navigation);
    same(enabled_background[0]->color.w, 0.60f);
    same(enabled_background[1]->color.w, 0);
    same(focus_marker->pos.y, 272 - vco_ui_row_center(1));
    /* A submitted command is never presented as a confirmed binary state.
     * Both directions, pauses, errors and stale identities clear old visuals. */
    for (unsigned row = 0; row < 2; ++row) {
        execution.queued[row] = true;
        render();
        same(switch_thumb[row]->color.w, 0);
        same(enabled_background[row]->color.w, 0);
        same(names[row]->color.x, 0.95f);
        same(states[row]->size.x, 140);
        execution.queued[row] = false;
    }
    for (VcoEntryState state : {VCO_ENTRY_ENABLING, VCO_ENTRY_DISABLING, VCO_ENTRY_PAUSED,
                                VCO_ENTRY_ERROR, VCO_ENTRY_UNSUPPORTED}) {
        execution.rows[0].state = state;
        render();
        same(switch_thumb[0]->color.w, 0);
        same(enabled_background[0]->color.w, 0);
        assert(!states[0]->value.empty());
    }
    execution.rows[0].state = VCO_ENTRY_ON;
    execution.state = VCO_EXECUTION_BACKGROUND;
    render();
    same(switch_thumb[0]->color.w, 0);
    same(enabled_background[0]->color.w, 0);
    execution.state = VCO_EXECUTION_READY;
    render();
    VcoSettingsState settings = {};
    settings.applied = vco_settings_defaults();
    vco_settings_begin(&settings);
    vco_view_settings(settings, catalog, 0, false, 0);
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i) {
        same(switch_thumb[i]->color.w, 0);
        same(enabled_background[i]->color.w, 0);
        same(names[i]->color.x, 0.95f);
        same(states[i]->size.x, i == VCO_SETTING_BUTTON_MODE ? 300 : 140);
    }
    /* Actual decoded encoding belongs in settings; draft policy changes do
     * not relabel the already loaded file. Error/apply prompts keep priority. */
    for (int language = 0; language < VCO_LANGUAGE_COUNT; ++language) {
        settings.draft.language = language;
        settings.draft.encoding = VCO_ENCODING_GBK;
        settings.selected = VCO_SETTING_ENCODING;
        vco_view_settings(settings, catalog, 0, false, 3);
        assert(message->value.find(u"UTF-8") != std::u16string::npos);
        assert(message->value.find(u"GBK") == std::u16string::npos);
        settings.apply_pending = true;
        vco_view_settings(settings, catalog, 0, false, 3);
        assert(message->value.find(u"UTF-8") == std::u16string::npos);
        assert(footer->value.empty());
        settings.apply_pending = false;
    }
    render();
    assert(message->value.find(u"UTF-8") == std::u16string::npos);
    assert(message->value.find(u"VitaCheat") == std::u16string::npos);
    execution.rows[1].error = VCO_E_IO;
    execution.rows[1].line = 996;
    render();
    assert(message->value.find(u"L996") != std::u16string::npos);
    assert(message->value.find(u"UTF-8") == std::u16string::npos);
    vco_view_settings(settings, catalog, 0, false, 3);
    same(message->color.x, title->color.x);
    same(message->color.y, title->color.y);
    same(message->color.z, title->color.z);
    execution.rows[1].error = 0;
    render();
    ++execution.session;
    render();
    same(switch_thumb[0]->color.w, 0);
    same(enabled_background[0]->color.w, 0);
    list();
    same(switch_thumb[1]->color.w, 0);
    same(enabled_background[1]->color.w, 0);
    same(focus_marker->color.w, 0);
    focus_at(0, false);
    vco_view_close(2000);
    assert(plugin->widgets.empty());
}
static void regional(void)
{
    assert(vco_view_open(1000));
    VcoCatalog catalog = {};
    for (int language = 0; language < VCO_LANGUAGE_COUNT; ++language) {
        for (VcoButtonMode mode : {VCO_BUTTON_ASIAN, VCO_BUTTON_WESTERN}) {
            VcoSettingsState settings = {};
            settings.applied = vco_settings_defaults();
            vco_settings_set_button_mode(&settings.applied, mode);
            vco_settings_begin(&settings);
            settings.draft.language = language;
            settings.selected = VCO_SETTING_BUTTON_MODE;
            vco_settings_adjust(&settings, 1);
            vco_view_settings(settings, catalog, 0, false, 0);
            assert(pagination->value == u"3 / 8");
            assert(names[VCO_SETTING_BUTTON_MODE]->value.find(u"/") != std::u16string::npos);
            assert(states[VCO_SETTING_BUTTON_MODE]->value[2] ==
                   (mode == VCO_BUTTON_ASIAN ? u'×' : u'○'));
            same(states[VCO_SETTING_BUTTON_MODE]->size.x, 300);
            static const char16_t *const confirmations[VCO_LANGUAGE_COUNT][2] = {
                {u"○ Confirm", u"× Confirm"},
                {u"○ 确认", u"× 确认"},
                {u"○ 確認", u"× 確認"},
                {u"○ 決定", u"× 決定"},
                {u"○ 확인", u"× 확인"}};
            const char16_t *confirm = confirmations[language][mode];
            assert(footer->value.find(confirm) != std::u16string::npos);
            assert(footer->value.find(u"□") == std::u16string::npos);
            settings.confirm = true;
            vco_view_settings(settings, catalog, 0, false, 0);
            assert(footer->value.find(confirm) != std::u16string::npos);
            settings.confirm = false;
            settings.applied = settings.draft;
            vco_view_bindings(settings.applied, language);
            static const char16_t *const toggles[VCO_LANGUAGE_COUNT] = {
                u"□ Toggle", u"□ 切换", u"□ 切換", u"□ 切替", u"□ 전환"};
            static const char16_t *const backs[VCO_LANGUAGE_COUNT][2] = {{u"○ Back", u"× Back"},
                                                                         {u"○ 返回", u"× 返回"},
                                                                         {u"○ 返回", u"× 返回"},
                                                                         {u"○ 戻る", u"× 戻る"},
                                                                         {u"○ 뒤로", u"× 뒤로"}};
            const char16_t *toggle = toggles[language];
            const char16_t *back = backs[language][mode];
            assert(footer->value.find(toggle) != std::u16string::npos);
            assert(footer->value.find(back) != std::u16string::npos);
            assert(footer->value.find(confirm) == std::u16string::npos);
        }
    }
    list();
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i)
        same(states[i]->size.x, 140);
    vco_view_close(2000);
}
static void localization(void)
{
    assert(vco_view_open(1000));
    const char16_t *const headings[VCO_LANGUAGE_COUNT] = {u"SETTINGS", u"设置", u"設定", u"設定",
                                                          u"설정"};
    const char16_t *const native_names[VCO_LANGUAGE_COUNT] = {u"English", u"简体中文", u"繁體中文",
                                                              u"日本語", u"한국어"};
    for (int language = 0; language < VCO_LANGUAGE_COUNT; ++language) {
        VcoCatalog catalog = {};
        VcoSettingsState settings = {};
        settings.applied = vco_settings_defaults();
        settings.applied.language = language;
        vco_settings_begin(&settings);
        vco_view_settings(settings, catalog, 0, false, 0);
        assert(title->value == headings[language]);
        assert(states[0]->value.find(native_names[language]) != std::u16string::npos);
        for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i)
            assert(!names[i]->value.empty());
        for (unsigned status = 0; status <= VCO_SAVE_INVALID; ++status) {
            vco_view_save_status((VcoSaveStatus)status, language, true);
            assert(!message->value.empty() &&
                   message->value.find(u'\ufffd') == std::u16string::npos);
        }
        settings.capture.active = true;
        settings.selected = VCO_SETTING_OPEN;
        settings.capture_error = VCO_BINDING_RESERVED;
        vco_view_settings(settings, catalog, 0, false, 0);
        assert(!message->value.empty() && footer->value.find(u"↓") != std::u16string::npos);
        settings.capture.active = false;
        settings.confirm = true;
        vco_view_settings(settings, catalog, 0, false, 0);
        for (unsigned i = 0; i < 3; ++i)
            assert(!names[i]->value.empty());
        catalog.state = VCO_LIST_PARSE_ERROR;
        catalog.diagnostic.code = VCO_E_ENCODING;
        VcoExecutionView execution = {};
        VcoExecutionUi commands = {};
        VcoCatalogSourceView source = {};
        vco_view_render(catalog, execution, commands, source, 0, 0, language, VCO_NOTICE_NONE);
        assert(!empty_state->value.empty() &&
               empty_state->value.find(u'\ufffd') == std::u16string::npos);
        if (language == VCO_LANGUAGE_JAPANESE)
            assert(empty_state->value.find(u"文字コード") != std::u16string::npos);
        if (language == VCO_LANGUAGE_KOREAN)
            assert(empty_state->value.find(u"인코딩") != std::u16string::npos);
    }
    vco_view_close(2000);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!std::strcmp(argv[1], "localization"))
        localization();
    else if (!std::strcmp(argv[1], "appearance"))
        appearance();
    else if (!std::strcmp(argv[1], "allocation"))
        allocation_failures();
    else if (!std::strcmp(argv[1], "regional"))
        regional();
    else if (!std::strcmp(argv[1], "switches"))
        switches();
    else
        assert(false);
    return 0;
}
