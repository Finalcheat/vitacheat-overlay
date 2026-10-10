#include "catalog.h"
#include "text.h"
#include "../storage_paths.h"
#include <string.h>

int vco_title_id_valid(const char *s)
{
    if (!s || strlen(s) != 9)
        return 0;
    for (size_t i = 0; i < 9; ++i)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')))
            return 0;
    return 1;
}
VcoListState vco_game_select(const VcoGame *games, size_t count, VcoGame *out)
{
    if (!out)
        return VCO_LIST_DISCOVERY_ERROR;
    memset(out, 0, sizeof(*out));
    if (!games && count)
        return VCO_LIST_DISCOVERY_ERROR;
    size_t found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (games[i].pid <= 0 || games[i].app_id <= 0 || !vco_title_id_valid(games[i].title_id) ||
            memcmp(games[i].title_id, "NPXS", 4) == 0)
            continue;
        if (found && out->pid == games[i].pid && out->app_id == games[i].app_id)
            continue;
        *out = games[i];
        ++found;
    }
    if (found != 1)
        memset(out, 0, sizeof(*out));
    return !found ? VCO_LIST_NO_GAME : found == 1 ? VCO_LIST_READY : VCO_LIST_AMBIGUOUS;
}
void vco_catalog_load(VcoCatalog *out, const VcoGame *game, VcoCatalogWorkspace *work,
                      VcoReadFile read_file, void *context)
{
    vco_catalog_load_encoded(out, game, work, read_file, context, VCO_ENCODING_AUTO);
}
void vco_catalog_load_encoded(VcoCatalog *out, const VcoGame *game, VcoCatalogWorkspace *work,
                              VcoReadFile read_file, void *context, VcoEncoding policy)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->encoding_policy = policy;
    if (work)
        work->entry_count = work->code_count = work->name_size = 0;
    if (!game) {
        out->state = VCO_LIST_DISCOVERY_ERROR;
        return;
    }
    out->game = *game;
    if (!vco_title_id_valid(game->title_id) || !work || !read_file) {
        out->state = VCO_LIST_DISCOVERY_ERROR;
        return;
    }
    char path[80];
    size_t length = 0;
    const char *root = VCO_CHEAT_DB_DIR;
    size_t root_length = strlen(root);
    memcpy(path, root, root_length);
    memcpy(path + root_length, game->title_id, 9);
    memcpy(path + root_length + 9, ".psv", 5);
    VcoFileResult result = read_file(context, path, work->text, sizeof(work->text), &length);
    if (result != VCO_FILE_MISSING)
        out->source = VCO_SOURCE_VITACHEAT;
    if (result != VCO_FILE_OK) {
        out->state = result == VCO_FILE_MISSING     ? VCO_LIST_MISSING
                     : result == VCO_FILE_TOO_LARGE ? VCO_LIST_LIMIT
                                                    : VCO_LIST_IO_ERROR;
        return;
    }
    if (length > sizeof(work->text)) {
        out->state = VCO_LIST_LIMIT;
        return;
    }
    /* Compiler ops are unused until parsing completes: reuse that arena for the
     * in-place decoder bitset, without another full-size text buffer. The read
     * callback already fingerprints original bytes before this conversion. */
    VcoError error = vco_encoding_decode(work->text, &length, sizeof(work->text), policy, work->ops,
                                         sizeof(work->ops), &out->encoding, &out->diagnostic);
    if (error) {
        out->state = error == VCO_E_LIMIT ? VCO_LIST_LIMIT : VCO_LIST_PARSE_ERROR;
        return;
    }
    VcoCheatFile file = {work->entries, VCO_CATALOG_ENTRIES, 0, work->codes, VCO_CATALOG_CODES, 0,
                         work->names,   sizeof(work->names), 0};
    error = vco_cheat_parse(work->text, length, &file, &out->diagnostic);
    if (error) {
        out->state = error == VCO_E_LIMIT ? VCO_LIST_LIMIT : VCO_LIST_PARSE_ERROR;
        return;
    }
    vco_metadata_parse(work->text, length, &out->metadata);
    out->compatibility = vco_metadata_compatibility(&out->metadata, game->title_id, game->version);
    if (!out->game.title[0] && (out->metadata.fields & VCO_META_TITLE) &&
        !(out->metadata.conflicts & (VCO_META_TITLE | VCO_META_TITLE_ID)) &&
        !(out->metadata.invalid & (VCO_META_TITLE | VCO_META_TITLE_ID)) &&
        (!(out->metadata.fields & VCO_META_TITLE_ID) ||
         !strcmp(out->metadata.title_id, game->title_id)))
        vco_text_copy(out->game.title, sizeof(out->game.title), out->metadata.title);
    out->count = (uint32_t)file.entry_count;
    work->entry_count = file.entry_count;
    work->code_count = file.code_count;
    work->name_size = file.name_size;
    for (size_t i = 0; i < file.entry_count; ++i) {
        if ((out->compatibility == VCO_COMPAT_MISMATCH ||
             out->compatibility == VCO_COMPAT_CONFLICT) &&
            !work->entries[i].diagnostic.code) {
            uint32_t line = out->metadata.source_line;
            if (out->compatibility == VCO_COMPAT_MISMATCH)
                line = (out->metadata.fields & VCO_META_TITLE_ID) &&
                               strcmp(out->metadata.title_id, game->title_id)
                           ? out->metadata.title_id_line
                           : out->metadata.version_line;
            /* Travels through the canonical upload, so the kernel compiler also
             * rejects enable requests. This is not merely a disabled UI switch. */
            work->entries[i].diagnostic = (VcoDiagnostic){VCO_E_METADATA, line};
        }
        VcoCatalogRow *row = &out->rows[i];
        vco_text_copy(row->name, sizeof(row->name), file.names + file.entries[i].name);
        row->file_enabled = (unsigned)file.entries[i].file_enabled;
        VcoCheatProgram program = {work->ops, VCO_CATALOG_CODES, 0, NULL, 0};
        (void)vco_cheat_compile(&file, i, &program, &row->diagnostic);
        row->needs_review = program.needs_review;
    }
    out->state = VCO_LIST_READY;
}
