#include "catalog/catalog.h"
#include "execution/upload.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VcoFileMetadata metadata;
static void parse(const char *text)
{
    memset(&metadata, 0xa5, sizeof(metadata));
    vco_metadata_parse(text, text ? strlen(text) : 0, &metadata);
}
static VcoCompatibility compatibility(void)
{
    return vco_metadata_compatibility(&metadata, "PCSH00042", "01.00");
}
static int blocked(VcoCompatibility result)
{
    return result == VCO_COMPAT_MISMATCH || result == VCO_COMPAT_CONFLICT;
}
static void versions(void)
{
    const char *same[] = {"1.00", "01.00", "001.00"};
    for (unsigned i = 0; i < sizeof(same) / sizeof(*same); ++i)
        for (unsigned j = 0; j < sizeof(same) / sizeof(*same); ++j)
            assert(vco_version_equal(same[i], same[j]) == 1);
    assert(vco_version_equal("0.00", "000.00") == 1);
    assert(vco_version_equal("999.99", "999.99") == 1);
    assert(vco_version_equal("1.01", "1.00") == 0);
    assert(vco_version_equal("2.00", "1.00") == 0);
    const char *bad[] = {"",     "1",     "1.",      "1.0",   "1.000",    "1.00x",
                         ".00",  "-1.00", "+1.00",   " 1.00", "1.00 ",    "1,00",
                         "1.0a", "1.a0",  "0001.00", "v1.00", "1.00-2.00"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        /* Exact allocation exposes short-string reads beyond the terminator. */
        size_t size = strlen(bad[i]) + 1;
        char *short_string = malloc(size);
        assert(short_string);
        memcpy(short_string, bad[i], size);
        assert(vco_version_equal(short_string, "1.00") == -1);
        assert(vco_version_equal("1.00", short_string) == -1);
        free(short_string);
    }
    assert(vco_version_equal(NULL, "1.00") == -1);
    assert(vco_version_equal("1.00", NULL) == -1);
}
static void headers(void)
{
    parse("# cheats tested on version 1.00\n# GAMEPLAY: notes\n# arbitrary words\n"
          "_V0 entry\n# Version: 2.00\n$0200 81000000 00000001\n");
    assert(!metadata.fields && !metadata.invalid && compatibility() == VCO_COMPAT_UNVERIFIED);
    parse("# TITLE_ID: PCSH00042\n# Game Name: 中文名称\n# APP_VER: 001.00\n"
          "_V0 entry\n# Title ID: PCSE00001\n# Version: 2.00\n");
    assert(metadata.fields == (VCO_META_TITLE_ID | VCO_META_TITLE | VCO_META_VERSION));
    assert(!strcmp(metadata.title, "中文名称") && !strcmp(metadata.version, "001.00"));
    assert(metadata.title_id_line == 1 && metadata.version_line == 3);
    assert(compatibility() == VCO_COMPAT_MATCH);
    parse("\xef\xbb\xbf \t# Game ID: PCSH00042\r\n\r\n\t# Version: 1.00 \r\n"
          "_V0 test\r\n# Version: 2.00\r\n");
    assert(metadata.title_id_line == 1 && metadata.version_line == 3);
    assert(compatibility() == VCO_COMPAT_MATCH);
    const char raw[] = "# Version: 1.00ignored";
    vco_metadata_parse(raw, sizeof("# Version: 1.00") - 1, &metadata);
    assert(!strcmp(metadata.version, "1.00") && compatibility() == VCO_COMPAT_MATCH);
    parse("# Title ID: PCSH00042\n");
    assert(compatibility() == VCO_COMPAT_UNVERIFIED);
    parse("# Version: 1.00\n");
    assert(vco_metadata_compatibility(&metadata, "PCSH00042", NULL) == VCO_COMPAT_UNVERIFIED);
    assert(vco_metadata_compatibility(&metadata, "PCSH00042", "1.0") == VCO_COMPAT_UNVERIFIED);
    parse(NULL);
    assert(!metadata.fields && !metadata.invalid && !metadata.conflicts && !metadata.title[0]);
    assert(vco_metadata_compatibility(NULL, "PCSH00042", "1.00") == VCO_COMPAT_UNVERIFIED);
    assert(vco_metadata_compatibility(&metadata, NULL, "1.00") == VCO_COMPAT_UNVERIFIED);
    vco_metadata_parse(NULL, 0, NULL);
}
static void compact(void)
{
    parse("# PCSH00042 Final Fantasy X 1.00\n_V0 cheat\n");
    assert(!strcmp(metadata.title_id, "PCSH00042") && !strcmp(metadata.title, "Final Fantasy X"));
    assert(!strcmp(metadata.version, "1.00") && compatibility() == VCO_COMPAT_MATCH);
    parse("# PCSH00042 中文名称 2.00\n");
    assert(!strcmp(metadata.title, "中文名称") && compatibility() == VCO_COMPAT_MISMATCH);
    parse("# PCSH00042 Game: Subtitle 2.00\n");
    assert(!strcmp(metadata.title, "Game: Subtitle") && compatibility() == VCO_COMPAT_MISMATCH);
    parse("# PCSH00042 1.00\n");
    assert(!metadata.title[0] && compatibility() == VCO_COMPAT_MATCH);
    parse("# PCSH00042 Game 1.0\n");
    assert(!strcmp(metadata.title, "Game 1.0") && !(metadata.fields & VCO_META_VERSION));
    assert(compatibility() == VCO_COMPAT_UNVERIFIED);
    parse("# IMPORTANT text 1.00\n");
    assert(!metadata.fields && compatibility() == VCO_COMPAT_UNVERIFIED);
}
static void conflicts(void)
{
    parse("# Version: 01.00\n# APP_VER: 1.00\n# Title ID: PCSH00042\n"
          "# Game ID: PCSH00042\n");
    assert(!metadata.invalid && !metadata.conflicts && compatibility() == VCO_COMPAT_MATCH);
    parse("# Title ID: PCSH00042\n# Title ID: PCSE00001\n# Version: 1.00\n");
    assert(metadata.conflicts & VCO_META_TITLE_ID);
    assert(compatibility() == VCO_COMPAT_CONFLICT);
    parse("# Version: 1.00\n# Version: 2.00\n");
    assert(metadata.conflicts & VCO_META_VERSION);
    assert(compatibility() == VCO_COMPAT_CONFLICT);
    parse("# Title: First\n# Title: Second\n# Version: 1.00\n");
    assert(metadata.conflicts == VCO_META_TITLE && compatibility() == VCO_COMPAT_MATCH);
    /* Invalid duplicate evidence cannot erase an already known mismatch. */
    const char *mismatch[] = {
        "# Version: 2.00\n# Version: unknown\n",
        "# Version: unknown\n# Version: 2.00\n",
        "# Title ID: PCSE00001\n# Title ID: invalid\n# Version: 1.00\n",
        "# Title ID: invalid\n# Title ID: PCSE00001\n# Version: 1.00\n",
        "# Title ID: invalid\n# Version: 2.00\n",
    };
    for (unsigned i = 0; i < sizeof(mismatch) / sizeof(*mismatch); ++i) {
        parse(mismatch[i]);
        assert(blocked(compatibility()));
    }
}
static void bounds(void)
{
    const char *bad_ids[] = {"", "PCSH0004", "PCSH00042X", "pcsh00042", "../a.psv"};
    char text[512];
    for (unsigned i = 0; i < sizeof(bad_ids) / sizeof(*bad_ids); ++i) {
        snprintf(text, sizeof(text), "# Title ID: %s\n", bad_ids[i]);
        parse(text);
        assert(metadata.invalid & VCO_META_TITLE_ID);
        assert(!(metadata.fields & VCO_META_TITLE_ID) && compatibility() == VCO_COMPAT_UNVERIFIED);
    }
    const char *prefix = "# Title: ";
    size_t length = strlen(prefix);
    memcpy(text, prefix, length);
    memset(text + length, 'x', 255);
    text[length + 255] = 0;
    parse(text);
    assert((metadata.fields & VCO_META_TITLE) && strlen(metadata.title) == 255);
    text[length + 255] = 'x';
    text[length + 256] = 0;
    parse(text);
    assert((metadata.invalid & VCO_META_TITLE) && !metadata.title[0]);
    parse("# Version: 1234567890123456\n");
    assert(metadata.invalid & VCO_META_VERSION);
    parse("# Version: \n");
    assert(metadata.invalid & VCO_META_VERSION);
    parse("# this_unknown_key_exceeds_buffer_size: 1.00\n");
    assert(!metadata.fields && !metadata.invalid);
    vco_metadata_parse("# Version: 1.00", 0, &metadata);
    assert(!metadata.fields && !metadata.invalid);
}
static VcoCatalog catalog;
static VcoCatalogWorkspace work;
static unsigned read_calls;
static VcoFileResult read_file(void *context, const char *path, char *out, size_t capacity,
                               size_t *length)
{
    const char *text = context;
    assert(!strcmp(path, "ux0:vitacheat/db/PCSH00042.psv"));
    ++read_calls;
    *length = strlen(text);
    assert(*length < capacity);
    memcpy(out, text, *length);
    return VCO_FILE_OK;
}
static void catalog_load(const char *text, const char *title)
{
    VcoGame game = {1, 42, "PCSH00042", "", "01.00", 0};
    if (title)
        memcpy(game.title, title, strlen(title) + 1);
    read_calls = 0;
    vco_catalog_load(&catalog, &game, &work, read_file, (void *)text);
    assert(read_calls == 1 && catalog.state == VCO_LIST_READY &&
           catalog.source == VCO_SOURCE_VITACHEAT);
    assert(catalog.count == 1 && work.entry_count == 1 && work.code_count == 1);
}
static void catalog_case(void)
{
    const char *matching = "# PCSH00042 File title 1.00\n_V0 entry\n$0200 81000000 00000001\n";
    catalog_load(matching, NULL);
    assert(catalog.compatibility == VCO_COMPAT_MATCH && !strcmp(catalog.game.title, "File title"));
    assert(catalog.rows[0].diagnostic.code == VCO_OK && work.entries[0].diagnostic.code == VCO_OK);
    catalog_load(matching, "SFO title");
    assert(!strcmp(catalog.game.title, "SFO title"));
    catalog_load("# Title: First\n# Title: Second\n# Version: 1.00\n"
                 "_V0 entry\n$0200 81000000 00000001\n",
                 NULL);
    assert(!catalog.game.title[0] && catalog.rows[0].diagnostic.code == VCO_OK);
    catalog_load("# Title ID: PCSH00042\n# Title ID: PCSE00001\n# Title: Wrong identity\n"
                 "_V0 entry\n$0200 81000000 00000001\n",
                 NULL);
    assert(catalog.compatibility == VCO_COMPAT_CONFLICT && !catalog.game.title[0]);
    assert(catalog.rows[0].diagnostic.code == VCO_E_METADATA);
    catalog_load("# Title ID: invalid\n# Title: Invalid identity\n# Version: 1.00\n"
                 "_V0 entry\n$0200 81000000 00000001\n",
                 NULL);
    assert(catalog.compatibility == VCO_COMPAT_UNVERIFIED && !catalog.game.title[0]);
    catalog_load("# Title ID: PCSH00042\n# Title ID: invalid\n# Title: Invalid duplicate\n"
                 "# Version: 1.00\n_V0 entry\n$0200 81000000 00000001\n",
                 NULL);
    assert(catalog.compatibility == VCO_COMPAT_UNVERIFIED && !catalog.game.title[0]);
    catalog_load("# Version unspecified\n_V0 entry\n# Version: 2.00\n"
                 "$0200 81000000 00000001\n",
                 NULL);
    assert(catalog.compatibility == VCO_COMPAT_UNVERIFIED && !catalog.rows[0].diagnostic.code);
    const char *wrong[] = {
        "# PCSE00001 Other game 1.00\n",      "# Version: 2.00\n",
        "# Version: 1.00\n# Version: 2.00\n", "# Version: 2.00\n# Version: unknown\n",
        "# PCSH00042 Game: Subtitle 2.00\n",
    };
    for (unsigned i = 0; i < sizeof(wrong) / sizeof(*wrong); ++i) {
        char text[512];
        snprintf(text, sizeof(text), "%s_V0 entry\n$0200 81000000 00000001\n", wrong[i]);
        catalog_load(text, NULL);
        assert(blocked(catalog.compatibility));
        assert(catalog.rows[0].diagnostic.code == VCO_E_METADATA);
        assert(work.entries[0].diagnostic.code == VCO_E_METADATA);
        assert(work.entries[0].diagnostic.line > 0);
        /* The refusal survives the actual canonical wire representation and
         * a second compiler invocation, rather than existing only in the UI. */
        VcoCheatFile source = {work.entries, VCO_CATALOG_ENTRIES, work.entry_count,
                               work.codes,   VCO_CATALOG_CODES,   work.code_count,
                               work.names,   sizeof(work.names),  work.name_size};
        uint8_t packet[256];
        size_t size = 0;
        assert(vco_upload_encode(packet, sizeof(packet), 1, 9, 1, 42, "PCSH00042", &source,
                                 &size) == VCO_OK);
        VcoCheatEntry entry[1];
        VcoCode code[1];
        VcoCheatOp op[1];
        VcoCheatFile decoded = {entry, 1, 0, code, 1, 0, NULL, 0, 0};
        VcoUploadHeader header;
        assert(vco_upload_decode(packet, size, &decoded, &header) == VCO_OK);
        VcoCheatProgram program = {op, 1, 0, NULL, 0};
        VcoDiagnostic diagnostic;
        assert(vco_cheat_compile(&decoded, 0, &program, &diagnostic) == VCO_E_METADATA);
        assert(diagnostic.line == work.entries[0].diagnostic.line && !program.count);
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "versions"))
        versions();
    else if (!strcmp(argv[1], "headers"))
        headers();
    else if (!strcmp(argv[1], "compact"))
        compact();
    else if (!strcmp(argv[1], "conflicts"))
        conflicts();
    else if (!strcmp(argv[1], "bounds"))
        bounds();
    else if (!strcmp(argv[1], "catalog"))
        catalog_case();
    else
        assert(0);
    printf("PASS metadata %s\n", argv[1]);
    return 0;
}
