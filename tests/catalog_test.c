#include "catalog.h"
#include "sfo.h"
#include "text.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static VcoCatalog catalog;
static VcoCatalogWorkspace work;
static struct {
    const char *own, *legacy;
    VcoFileResult own_result, legacy_result;
    unsigned calls;
} fs;
static VcoFileResult read_file(void *ctx, const char *path, char *out, size_t capacity,
                               size_t *length)
{
    (void)ctx;
    ++fs.calls;
    assert(!strcmp(path, "ux0:vitacheat/db/PCSH00042.psv"));
    VcoFileResult result = fs.legacy_result;
    if (result != VCO_FILE_OK)
        return result;
    const char *text = fs.legacy;
    *length = strlen(text);
    assert(*length <= capacity);
    memcpy(out, text, *length);
    return VCO_FILE_OK;
}
static void loading(void)
{
    VcoGame game = {1, 2, "PCSH00042", "Final Fantasy X", "1.00", 0};
    fs.legacy = "_V1 原始默认开启\n$0100 81000000 00000063\n_V0 bad\n$2000 81000000 00000001\n_V0 "
                "review\n$A000 81000000 00000001";
    fs.own = "_V0 ignored old private directory\n$0000 81000000 00000002";
    fs.own_result = fs.legacy_result = VCO_FILE_OK;
    fs.calls = 0;
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_READY && catalog.source == VCO_SOURCE_VITACHEAT &&
           catalog.count == 3 && fs.calls == 1);
    assert(catalog.rows[0].file_enabled == 1 && catalog.rows[0].diagnostic.code == VCO_OK);
    assert(catalog.rows[1].diagnostic.code == VCO_E_FORMAT);
    assert(catalog.rows[2].needs_review == 0);
    fs.legacy_result = VCO_FILE_ERROR;
    fs.calls = 0;
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_IO_ERROR && !catalog.count && fs.calls == 1);
    fs.legacy_result = VCO_FILE_OK;
    fs.legacy = "_V2 corrupt";
    fs.calls = 0;
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_PARSE_ERROR && !catalog.count && fs.calls == 1);
    /* Decode the selected source, with explicit UTF-8/GBK overrides. Never
     * substitute another source on a decoding or syntax failure. */
    fs.legacy = "_V0 \xCE\xD2\xB7\xBD\n$0000 81000000 00000001";
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_READY && catalog.encoding == VCO_ENCODING_GBK &&
           catalog.encoding_policy == VCO_ENCODING_AUTO && !strcmp(catalog.rows[0].name, "我方") &&
           work.codes[0].a == 0x81000000u && work.codes[0].b == 1);
    fs.calls = 0;
    vco_catalog_load_encoded(&catalog, &game, &work, read_file, NULL, VCO_ENCODING_UTF8);
    assert(catalog.state == VCO_LIST_PARSE_ERROR && catalog.diagnostic.code == VCO_E_ENCODING &&
           !work.entry_count && !work.code_count && !work.name_size && fs.calls == 1);
    vco_catalog_load_encoded(&catalog, &game, &work, read_file, NULL, VCO_ENCODING_GBK);
    assert(catalog.state == VCO_LIST_READY && catalog.encoding_policy == VCO_ENCODING_GBK &&
           !strcmp(catalog.rows[0].name, "我方"));
    game.title[0] = 0;
    fs.legacy = "# Title: \xCE\xD2\xB7\xBD\n# Title ID: PCSH00042\n# Version: 9.99\n"
                "_V1 \xCE\xD2\xB7\xBD\n$0200 81000000 11223344\n"
                "_V0 bad\n$2000 81000000 00000001\n";
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_READY && catalog.count == 2 &&
           catalog.compatibility == VCO_COMPAT_MISMATCH && !strcmp(catalog.game.title, "我方") &&
           catalog.rows[0].diagnostic.code == VCO_E_METADATA &&
           work.entries[0].diagnostic.code == VCO_E_METADATA && work.codes[0].b == 0x11223344u);
    fs.legacy = "";
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_READY && !catalog.count);
    fs.own_result = fs.legacy_result = VCO_FILE_MISSING;
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_MISSING && catalog.source == VCO_SOURCE_NONE);
    fs.legacy_result = VCO_FILE_TOO_LARGE;
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_LIMIT);
    strcpy(game.title_id, "../evilxx");
    fs.calls = 0;
    vco_catalog_load(&catalog, &game, &work, read_file, NULL);
    assert(catalog.state == VCO_LIST_DISCOVERY_ERROR && fs.calls == 0);
}
static void selection(void)
{
    VcoGame games[] = {{1, 10, "NPXS10015", "system", "", 0},
                       {2, 20, "PCSH00042", "game", "", 0},
                       {3, 30, "PCSE00001", "second", "", 0}},
            out;
    assert(vco_game_select(games, 1, &out) == VCO_LIST_NO_GAME);
    assert(vco_game_select(games, 2, &out) == VCO_LIST_READY && out.pid == 20);
    assert(vco_game_select(games, 3, &out) == VCO_LIST_AMBIGUOUS && out.pid == 0);
    games[2] = games[1];
    assert(vco_game_select(games, 3, &out) == VCO_LIST_READY);
    assert(vco_title_id_valid("VITASHELL") && !vco_title_id_valid("../a.psv") &&
           !vco_title_id_valid("PCSH00042X"));
}
static void unicode(void)
{
    uint16_t out[16];
    char clipped[9];
    assert(vco_utf8_to_utf16("中文A😀", out, 16) == 5);
    assert(out[0] == 0x4E2D && out[1] == 0x6587 && out[2] == 'A' && out[3] == 0xD83D &&
           out[4] == 0xDE00 && !out[5]);
    assert(vco_utf8_to_utf16("😀A", out, 3) == 1 && out[0] == 0x2026 && !out[1]);
    assert(vco_utf8_to_utf16("中文A", out, 3) == 2 && out[1] == 0x2026);
    assert(vco_utf8_to_utf16("\xF0\x9F", out, 16) == 2 && out[0] == 0xFFFD);
    assert(vco_utf8_to_utf16("A", out, 1) == 0 && !out[0]);
    vco_text_copy(clipped, sizeof(clipped), "中文测试");
    assert(strcmp(clipped, "中…") == 0);
    vco_text_copy(clipped, sizeof(clipped), "abc");
    assert(strcmp(clipped, "abc") == 0);
}
static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}
static void metadata(void)
{
    unsigned char sfo[80] = {0};
    char out[32];
    memcpy(sfo, "\0PSF", 4);
    put32(sfo + 4, 0x101);
    put32(sfo + 8, 36);
    put32(sfo + 12, 48);
    put32(sfo + 16, 1);
    sfo[22] = 4;
    sfo[23] = 2;
    put32(sfo + 24, 7);
    put32(sfo + 28, 16);
    put32(sfo + 32, 0);
    memcpy(sfo + 36, "TITLE", 6);
    memcpy(sfo + 48, "中文", 7);
    assert(vco_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == 1 &&
           !strcmp(out, "中文"));
    assert(vco_sfo_string(sfo, sizeof(sfo), "TITLE_ID", out, sizeof(out)) == 0);
    put32(sfo + 32, UINT32_MAX);
    assert(vco_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == -1);
    put32(sfo + 32, 0);
    put32(sfo + 24, 100);
    assert(vco_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == -1);
    put32(sfo + 16, UINT32_MAX);
    assert(vco_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == -1);
    for (size_t size = 0; size < 20; ++size)
        assert(vco_sfo_string(sfo, size, "TITLE", out, sizeof(out)) == -1);
}
static VcoFileResult read_fixture(void *ctx, const char *path, char *out, size_t capacity,
                                  size_t *length)
{
    (void)path;
    FILE *file = fopen(ctx, "rb");
    assert(file);
    *length = fread(out, 1, capacity, file);
    assert(!ferror(file) && feof(file) && !fclose(file));
    return VCO_FILE_OK;
}
static void compare_fixtures(const char *gbk, const char *utf8, const char *title_id,
                             const char *version)
{
    VcoCatalog *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    VcoCatalogWorkspace *wa = calloc(1, sizeof(*wa)), *wb = calloc(1, sizeof(*wb));
    assert(a && b && wa && wb);
    VcoGame game = {1, 2, "", "", "", 0};
    assert(vco_title_id_valid(title_id) && strlen(version) < sizeof(game.version));
    memcpy(game.title_id, title_id, strlen(title_id) + 1);
    memcpy(game.version, version, strlen(version) + 1);
    vco_catalog_load_encoded(a, &game, wa, read_fixture, (void *)gbk, VCO_ENCODING_AUTO);
    vco_catalog_load_encoded(b, &game, wb, read_fixture, (void *)utf8, VCO_ENCODING_UTF8);
    assert(a->state == VCO_LIST_READY && b->state == VCO_LIST_READY &&
           a->encoding == VCO_ENCODING_GBK && b->encoding == VCO_ENCODING_UTF8 &&
           a->count == b->count && wa->entry_count == wb->entry_count &&
           wa->code_count == wb->code_count && wa->name_size == wb->name_size &&
           a->compatibility == b->compatibility &&
           !memcmp(&a->metadata, &b->metadata, sizeof(a->metadata)) &&
           !memcmp(wa->names, wb->names, wa->name_size));
    unsigned errors = 0;
    for (size_t i = 0; i < a->count; ++i) {
        const VcoCheatEntry *ea = &wa->entries[i], *eb = &wb->entries[i];
        assert(!memcmp(&a->rows[i], &b->rows[i], sizeof(a->rows[i])) && ea->name == eb->name &&
               ea->first == eb->first && ea->count == eb->count &&
               ea->file_enabled == eb->file_enabled && ea->diagnostic.code == eb->diagnostic.code &&
               ea->diagnostic.line == eb->diagnostic.line);
        errors += a->rows[i].diagnostic.code != VCO_OK;
    }
    for (size_t i = 0; i < wa->code_count; ++i) {
        /* Compare semantic fields, not the indeterminate padding after code. */
        const VcoCode *ca = &wa->codes[i], *cb = &wb->codes[i];
        assert(ca->code == cb->code && ca->a == cb->a && ca->b == cb->b && ca->line == cb->line);
    }
    printf("GBK/UTF-8 fixture equality: %zu entries, %zu codes, %u isolated entry errors\n",
           wa->entry_count, wa->code_count, errors);
    free(wb);
    free(wa);
    free(b);
    free(a);
}
int main(int argc, char **argv)
{
    if (argc == 3 || argc == 5) {
        compare_fixtures(argv[1], argv[2], argc == 5 ? argv[3] : "PCSB00031",
                         argc == 5 ? argv[4] : "1.00");
        return 0;
    }
    assert(argc == 1);
    loading();
    selection();
    unicode();
    metadata();
    printf(
        "PASS: catalog sources, errors, metadata, selection, Unicode; snapshot=%zu workspace=%zu\n",
        sizeof(catalog), sizeof(work));
    return 0;
}
