#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/execution_view.h"
#include "localization.h"
static VcoCatalog catalog;
static VcoExecutionView view;
static VcoExecutionUi ui;
static void setup(void)
{
    memset(&catalog, 0, sizeof(catalog));
    memset(&view, 0, sizeof(view));
    memset(&ui, 0, sizeof(ui));
    catalog.revision = 7;
    catalog.state = VCO_LIST_READY;
    catalog.count = 4;
    view.revision = 10;
    view.catalog_revision = catalog.revision;
    view.session = 22;
    catalog.execution_session = view.session;
    view.list_revision = 31;
    view.count = catalog.count;
    view.state = VCO_EXECUTION_READY;
    vco_execution_ui_sync(&ui, &view);
}
static VcoExecutionDisplay display(unsigned row)
{
    return vco_execution_display(&catalog, &view, &ui, row);
}
static bool target(unsigned row, bool *enabled)
{
    return vco_execution_toggle_target(&catalog, &view, &ui, row, enabled);
}
static void rows(void)
{
    bool enabled = true;
    catalog.rows[0].file_enabled = 1;
    assert(display(0) == VCO_DISPLAY_UNKNOWN && !target(0, &enabled));
    view.known[0] = true;
    const VcoExecutionDisplay expected[] = {
        VCO_DISPLAY_OFF,    VCO_DISPLAY_ENABLING, VCO_DISPLAY_ON,         VCO_DISPLAY_DISABLING,
        VCO_DISPLAY_PAUSED, VCO_DISPLAY_ERROR,    VCO_DISPLAY_UNSUPPORTED};
    for (unsigned state = VCO_ENTRY_OFF; state <= VCO_ENTRY_UNSUPPORTED; ++state) {
        view.rows[0].state = state;
        assert(display(0) == expected[state]);
    }
    view.rows[0].state = 99;
    assert(display(0) == VCO_DISPLAY_UNKNOWN && !target(0, &enabled));
    view.rows[0].state = VCO_ENTRY_OFF;
    assert(target(0, &enabled) && enabled);
    view.rows[0].state = VCO_ENTRY_ON;
    assert(target(0, &enabled) && !enabled);
    view.rows[0].state = VCO_ENTRY_PAUSED;
    assert(target(0, &enabled) && !enabled);
    view.rows[0].state = VCO_ENTRY_ERROR;
    view.rows[0].error = VCO_E_IO;
    assert(target(0, &enabled) && enabled);
    view.rows[0].error = VCO_E_RESTORE;
    assert(!target(0, &enabled));
    view.rows[0].error = VCO_E_METADATA;
    assert(display(0) == VCO_DISPLAY_ERROR && !target(0, &enabled));
    view.rows[0].state = VCO_ENTRY_OFF;
    for (unsigned compatibility = VCO_COMPAT_MISMATCH; compatibility <= VCO_COMPAT_CONFLICT;
         ++compatibility) {
        catalog.compatibility = (VcoCompatibility)compatibility;
        assert(!target(0, &enabled));
        view.rows[0].state = VCO_ENTRY_ON;
        assert(target(0, &enabled) && !enabled); /* Never block stopping a known active row. */
        view.rows[0].state = VCO_ENTRY_OFF;
    }
    catalog.compatibility = VCO_COMPAT_UNVERIFIED;
    assert(target(0, &enabled) && enabled); /* Absence of evidence is shown, not invented. */
    view.rows[0].state = VCO_ENTRY_UNSUPPORTED;
    assert(!target(0, &enabled));
    view.rows[0].state = VCO_ENTRY_ON;
    view.rows[0].pending = 123;
    assert(!target(0, &enabled));
    view.rows[0].pending = 0;
    view.state = VCO_EXECUTION_BACKGROUND;
    assert(display(0) == VCO_DISPLAY_PAUSED); /* Confirmed authority supersedes an older row. */
    assert(target(0, &enabled) && !enabled);
    view.rows[0].state = VCO_ENTRY_OFF;
    assert(!target(0, &enabled));
    view.state = VCO_EXECUTION_ERROR;
    view.rows[0].state = VCO_ENTRY_ON;
    assert(display(0) == VCO_DISPLAY_UNKNOWN && !target(0, &enabled));
    view.rows[0].state = VCO_ENTRY_ERROR;
    assert(display(0) == VCO_DISPLAY_ERROR);
    view.known[0] = false;
    catalog.rows[0].needs_review = VCO_REVIEW_CONDITIONS;
    assert(display(0) == VCO_DISPLAY_UNSUPPORTED);
    catalog.rows[0].needs_review = 0;
    catalog.rows[0].diagnostic.code = VCO_E_SYNTAX;
    assert(display(0) == VCO_DISPLAY_ERROR);
    catalog.rows[0].diagnostic.code = VCO_E_UNVERIFIED;
    assert(display(0) == VCO_DISPLAY_UNSUPPORTED);
}
static void pending(void)
{
    bool enabled;
    view.known[0] = true;
    assert(target(0, &enabled) && enabled);
    vco_execution_ui_submitted(&ui, &view, 0);
    assert(display(0) == VCO_DISPLAY_QUEUED && !target(0, &enabled));
    assert(view.rows[0].state == VCO_ENTRY_OFF); /* Never optimistic ON. */
    vco_execution_ui_sync(&ui, &view);
    assert(ui.pending[0]);
    --view.revision;
    vco_execution_ui_sync(&ui, &view);
    assert(ui.pending[0]); /* A late old view cannot release the submission guard. */
    view.revision += 2;
    view.queued[0] = true;
    vco_execution_ui_sync(&ui, &view);
    assert(!ui.pending[0] && display(0) == VCO_DISPLAY_QUEUED && !target(0, &enabled));
    ++view.revision;
    view.queued[0] = false;
    view.rows[0].state = VCO_ENTRY_ENABLING;
    view.rows[0].pending = 42;
    vco_execution_ui_sync(&ui, &view);
    assert(display(0) == VCO_DISPLAY_ENABLING && !target(0, &enabled));
    ++view.revision;
    view.rows[0].state = VCO_ENTRY_ON;
    view.rows[0].pending = 0;
    view.rows[0].acknowledged = 42;
    vco_execution_ui_sync(&ui, &view);
    assert(display(0) == VCO_DISPLAY_ON && target(0, &enabled) && !enabled);
    vco_execution_ui_submitted(&ui, &view, 0);
    ++view.revision;
    view.rows[0].state = VCO_ENTRY_ERROR;
    view.rows[0].error = VCO_E_IO;
    vco_execution_ui_sync(&ui, &view);
    assert(!ui.pending[0] && display(0) == VCO_DISPLAY_ERROR && target(0, &enabled) && enabled);
    /* Fast rejection may leave the old confirmed state: newer service snapshot
     * with queued=false ends the local guard, while UI keeps that old state. */
    view.rows[0].state = VCO_ENTRY_OFF;
    vco_execution_ui_submitted(&ui, &view, 0);
    ++view.revision;
    vco_execution_ui_sync(&ui, &view);
    assert(!ui.pending[0] && display(0) == VCO_DISPLAY_OFF);
}
static void binding(void)
{
    bool enabled;
    view.known[0] = true;
    assert(vco_execution_view_matches(&catalog, &view));
    ++catalog.revision;
    assert(!vco_execution_view_matches(&catalog, &view) && !target(0, &enabled));
    assert(display(0) == VCO_DISPLAY_UNKNOWN);
    view.catalog_revision = catalog.revision;
    vco_execution_ui_submitted(&ui, &view, 0);
    uint64_t old_session = view.session;
    ++view.session;
    ++view.list_revision;
    ++view.revision;
    vco_execution_ui_sync(&ui, &view);
    assert(!ui.pending[0] && ui.session == view.session);
    vco_execution_ui_submitted(&ui, &view, 0);
    uint64_t current_session = view.session;
    view.session = old_session;
    --view.revision;
    vco_execution_ui_sync(&ui, &view);
    assert(ui.session == current_session && ui.pending[0]);
    view.session = current_session;
    ++view.revision;
    view.count = VCO_ENGINE_ENTRIES + 1;
    assert(!vco_execution_view_matches(&catalog, &view) && !target(0, &enabled));
    view.count = catalog.count;
    catalog.state = VCO_LIST_LOADING;
    assert(!vco_execution_view_matches(&catalog, &view) && !target(0, &enabled));
    catalog.state = VCO_LIST_READY;
    view.session = 0;
    assert(!vco_execution_view_matches(&catalog, &view));
    assert(display(UINT32_MAX) == VCO_DISPLAY_UNKNOWN && !target(UINT32_MAX, &enabled));
    assert(!vco_execution_view_matches(NULL, &view));
    assert(!vco_execution_view_matches(&catalog, NULL));
    assert(!vco_execution_toggle_target(&catalog, &view, &ui, 0, NULL));
}
static void wrapping(void)
{
    memset(&ui, 0, sizeof(ui));
    view.revision = UINT32_MAX - 1;
    view.known[0] = true;
    vco_execution_ui_sync(&ui, &view);
    vco_execution_ui_submitted(&ui, &view, 0);
    view.revision = UINT32_MAX;
    view.queued[0] = true;
    vco_execution_ui_sync(&ui, &view);
    assert(!ui.pending[0] && display(0) == VCO_DISPLAY_QUEUED);
    vco_execution_ui_submitted(&ui, &view, 1);
    view.revision = 1;
    view.queued[0] = false;
    vco_execution_ui_sync(&ui, &view);
    assert(!ui.pending[1] && ui.view_revision == 1);
    vco_execution_ui_submitted(&ui, &view, 0);
    view.revision = UINT32_MAX;
    vco_execution_ui_sync(&ui, &view);
    assert(ui.pending[0] && ui.view_revision == 1);
}
static void localization(void)
{
    for (int lang = 0; lang < VCO_LANGUAGE_COUNT; ++lang) {
        for (int state = VCO_DISPLAY_UNKNOWN; state <= VCO_DISPLAY_QUEUED; ++state)
            assert(vco_execution_display_text((VcoExecutionDisplay)state, lang)[0]);
        for (unsigned state = VCO_EXECUTION_LOADING; state <= VCO_EXECUTION_UNAVAILABLE; ++state)
            assert(vco_execution_state_text(state, lang)[0]);
        for (unsigned error = VCO_E_ARGUMENT; error <= VCO_E_RESTORE + 1; ++error)
            assert(vco_execution_error_text(error, lang)[0]);
        for (unsigned compatibility = VCO_COMPAT_UNVERIFIED; compatibility <= VCO_COMPAT_CONFLICT;
             ++compatibility)
            assert(vco_catalog_compatibility_text((VcoCompatibility)compatibility, lang)[0]);
        assert(vco_catalog_source_text(VCO_SOURCE_CHECKING, lang)[0]);
        assert(vco_catalog_source_text(VCO_SOURCE_CHANGED, lang)[0]);
        assert(vco_catalog_source_text(VCO_SOURCE_UNAVAILABLE, lang)[0]);
        assert(!vco_catalog_source_text(VCO_SOURCE_UNCHANGED, lang)[0]);
        assert(!vco_catalog_source_text(VCO_SOURCE_UNCHECKED, lang)[0]);
        for (int notice = VCO_NOTICE_BUSY; notice <= VCO_NOTICE_RELOADING; ++notice)
            assert(vco_execution_notice_text((VcoExecutionNotice)notice, lang)[0]);
        /* Reserved old ABI state no longer tells users to disable VitaCheat. */
        assert(!strstr(vco_execution_state_text(VCO_EXECUTION_CONFLICT, lang), "VitaCheat"));
    }
    assert(!strcmp(vco_execution_display_text(VCO_DISPLAY_ON, -1), "ON"));
    assert(!strcmp(vco_execution_display_text(VCO_DISPLAY_ON, 99), "ON"));
    assert(!strcmp(vco_execution_display_text((VcoExecutionDisplay)99, 0), "UNKNOWN"));
    assert(vco_execution_error_text(UINT32_MAX, 0)[0]);
    assert(!strcmp(vco_execution_error_text(VCO_E_METADATA, 0),
                   "Cheat file game/version mismatch or conflicting metadata"));
    assert(!strcmp(vco_catalog_compatibility_text((VcoCompatibility)99, 0), "Version unverified"));
    char header[128];
    strcpy(catalog.game.title_id, "PCSE12345");
    strcpy(catalog.game.version, "01.02");
    strcpy(catalog.metadata.version, "1.99");
    vco_catalog_identity_text(&catalog, header, sizeof(header));
    assert(!strcmp(header, "PCSE12345 · v01.02"));
    catalog.game.version[0] = 0;
    vco_catalog_identity_text(&catalog, header, sizeof(header));
    assert(
        !strcmp(header, "PCSE12345")); /* Never present file metadata as the installed version. */
    catalog.game.title_id[0] = 0;
    vco_catalog_identity_text(&catalog, header, sizeof(header));
    assert(!header[0]);
    memset(catalog.game.title_id, 'X', sizeof(catalog.game.title_id));
    memset(catalog.game.version, 'Y', sizeof(catalog.game.version));
    memset(header, 0x55, sizeof(header));
    vco_catalog_identity_text(&catalog, header, 8);
    assert(!memcmp(header, "XXXXXXX", 7) && !header[7] && header[8] == 0x55);
    vco_catalog_identity_text(&catalog, header, 1);
    assert(!header[0]);
    vco_catalog_identity_text(&catalog, header, 0);
    vco_catalog_identity_text(&catalog, NULL, sizeof(header));
    vco_catalog_identity_text(NULL, header, sizeof(header));
    assert(!header[0]);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "rows"))
        rows();
    else if (!strcmp(argv[1], "pending"))
        pending();
    else if (!strcmp(argv[1], "binding"))
        binding();
    else if (!strcmp(argv[1], "wrapping"))
        wrapping();
    else if (!strcmp(argv[1], "localization"))
        localization();
    else
        assert(!"unknown execution view test");
    puts("execution view passed");
    return 0;
}
