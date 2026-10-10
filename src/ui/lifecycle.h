#ifndef VCO_UI_LIFECYCLE_H
#define VCO_UI_LIFECYCLE_H
#include "../platform/execution_service.h"
#include <string.h>
typedef struct {
    int32_t pid, app_id;
    char title_id[16];
    uint64_t session;
    bool have_game, present;
} VcoUiLifecycle;
/* Discovery failures are not evidence of departure. Retain the last trusted
 * identity through UNKNOWN and through same-game explicit list reloads. */
static inline bool vco_ui_lifecycle_catalog(VcoUiLifecycle *state, const VcoCatalog *catalog)
{
    if (catalog->state == VCO_LIST_NO_GAME) {
        bool departed = state->present;
        memset(state, 0, sizeof(*state));
        return departed;
    }
    const VcoGame *game = &catalog->game;
    if (catalog->state == VCO_LIST_DISCOVERY_ERROR || game->pid <= 0 || game->app_id <= 0 ||
        !game->title_id[0])
        return false;
    if (state->have_game && state->pid == game->pid && state->app_id == game->app_id &&
        !strcmp(state->title_id, game->title_id))
        return false;
    bool departed = state->present;
    state->pid = game->pid;
    state->app_id = game->app_id;
    memcpy(state->title_id, game->title_id, sizeof(state->title_id));
    state->session = 0;
    state->have_game = state->present = true;
    return departed;
}
static inline bool vco_ui_lifecycle_execution(VcoUiLifecycle *state, const VcoCatalog *catalog,
                                              const VcoExecutionView *view)
{
    if (view->state == VCO_EXECUTION_NO_GAME) {
        if (!state->session || view->catalog_revision != catalog->revision)
            return false;
        bool departed = state->present;
        state->present = false;
        state->session = 0;
        return departed;
    }
    if (!view->session || !catalog->revision || view->catalog_revision != catalog->revision ||
        catalog->state == VCO_LIST_NO_GAME || !state->have_game)
        return false;
    /* On first discovery / a different catalog target, LOADING can still carry
     * the old kernel target while its clear drains. Establish the baseline only
     * from a ready binding; afterwards retain it across zero-session loading. */
    if (!state->session && view->state != VCO_EXECUTION_READY &&
        view->state != VCO_EXECUTION_BACKGROUND)
        return false;
    /* A reused PID/title is still a new process when the kernel session changes.
     * Zero-session LOADING publications must not erase this comparison base. */
    bool departed = state->present && state->session && state->session != view->session;
    state->session = view->session;
    state->present = true;
    return departed;
}
#endif
