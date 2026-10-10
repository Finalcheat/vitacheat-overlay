#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "config/config.h"
typedef struct {
    char bytes[3][VCO_CONFIG_CAPACITY];
    size_t lengths[3];
    bool exists[3];
    int fail_at, operation;
} Disk;
static int fail(Disk *disk)
{
    return disk->fail_at && ++disk->operation == disk->fail_at;
}
static int read_file(void *ctx, unsigned id, char *data, size_t cap, size_t *length)
{
    Disk *d = ctx;
    if (fail(d))
        return -1;
    if (!d->exists[id])
        return 1;
    assert(d->lengths[id] < cap);
    memcpy(data, d->bytes[id], d->lengths[id]);
    *length = d->lengths[id];
    return 0;
}
static int write_file(void *ctx, unsigned id, const char *data, size_t length)
{
    Disk *d = ctx;
    if (fail(d))
        return -1;
    memcpy(d->bytes[id], data, length);
    d->lengths[id] = length;
    d->exists[id] = true;
    return 0;
}
static int remove_file(void *ctx, unsigned id)
{
    Disk *d = ctx;
    if (fail(d))
        return -1;
    if (!d->exists[id])
        return 1;
    d->exists[id] = false;
    return 0;
}
static int rename_file(void *ctx, unsigned from, unsigned to)
{
    Disk *d = ctx;
    if (fail(d) || !d->exists[from] || d->exists[to])
        return -1;
    memcpy(d->bytes[to], d->bytes[from], d->lengths[from]);
    d->lengths[to] = d->lengths[from];
    d->exists[to] = true;
    d->exists[from] = false;
    return 0;
}
static uint32_t face(unsigned index)
{
    const uint32_t buttons[] = {VCO_SQUARE, VCO_CROSS, VCO_CIRCLE, VCO_TRIANGLE};
    return buttons[index];
}
int main(void)
{
    VcoSettings original = vco_settings_defaults(), result, changed = original;
    char data[VCO_CONFIG_CAPACITY];
    for (int language = -1; language <= 4; ++language)
        for (unsigned opacity = 0; opacity <= 100; opacity += 5)
            for (unsigned mode = 0; mode <= VCO_BUTTON_WESTERN; ++mode) {
                changed = original;
                changed.language = language;
                changed.opacity = opacity;
                vco_settings_set_button_mode(&changed, (VcoButtonMode)mode);
                changed.bindings.open = VCO_R | VCO_SELECT;
                size_t length = vco_config_encode(&changed, data, sizeof(data));
                assert(length && vco_config_decode(data, length, &result));
                assert(vco_settings_equal(&changed, &result));
                assert(strstr(data, "version=6\n") && strstr(data, "button_mode=") &&
                       !strstr(data, "previous=") && !strstr(data, "toggle=") &&
                       !strstr(data, "close=") && !strstr(data, "settings="));
            }
    const char *bad[] = {
        "",
        "version=2\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\nclose=2\n",
        "version=1\nlanguage=0\nopacity=101\npaging=0\ntoggle=0\nclose=2\n",
        "version=1\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\nclose=0\n",
        "version=1\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\nclose=2\nclose=1\n",
        "version=1\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\n",
        "version=1\nlanguage=999999999999999999\n",
        "version=1\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\nclose=2\nencoding=0\n",
        "version=2\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\nclose=2\nencoding=3\n",
        "version=2\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\nclose=2\nencoding=1\nencoding=2\n",
        "version=3\nlanguage=0\nopacity=70\npaging=0\ntoggle=0\nclose=2\nencoding=0\n",
        "version=5\nlanguage=0\nopacity=70\nbutton_mode=2\nopen=257\nencoding=0\n",
        "version=5\nlanguage=0\nopacity=70\nbutton_mode=0\nopen=256\nencoding=0\n",
        "version=5\nlanguage=0\nopacity=70\nbutton_mode=0\nopen=257\nencoding=3\n",
        "version=5\nlanguage=0\nopacity=70\nbutton_mode=0\nopen=257\nencoding=0\nclose=8192\n",
        "version=5\nlanguage=0\nopacity=70\nbutton_mode=0\nopen=257\nencoding=0\nbutton_mode=1\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        result = original;
        assert(!vco_config_decode(bad[i], strlen(bad[i]), &result));
        assert(vco_settings_equal(&result, &original));
    }
    for (unsigned version = 3; version <= 4; ++version)
        for (unsigned close = 0; close < 4; ++close) {
            uint32_t close_button = face(close);
            uint32_t toggle_button = close_button == VCO_SQUARE ? VCO_CIRCLE : VCO_SQUARE;
            int n = snprintf(data, sizeof(data),
                             "version=%u\nlanguage=2\nopacity=35\nprevious=128\nnext=32\ntoggle=%"
                             "u\nclose=%u\nsettings=1\nopen=520\nencoding=2\n",
                             version, toggle_button, close_button);
            assert(n > 0 && vco_config_decode(data, (size_t)n, &result));
            assert(result.language == 1 && result.opacity == 35 &&
                   result.encoding == VCO_ENCODING_GBK);
            assert(result.bindings.open == (VCO_R | VCO_START));
            assert(result.bindings.previous == VCO_L && result.bindings.next == VCO_R &&
                   result.bindings.settings == VCO_TRIANGLE);
            assert(result.button_mode ==
                   (close_button == VCO_CIRCLE ? VCO_BUTTON_WESTERN : VCO_BUTTON_ASIAN));
            assert(result.bindings.close == (close_button == VCO_CIRCLE ? VCO_CIRCLE : VCO_CROSS));
        }
    const char *v3 = "version=3\nlanguage=0\nopacity=70\nprevious=256\nnext=512\ntoggle="
                     "32768\nclose=8192\nsettings=8\nopen=257\nencoding=0\n";
    assert(vco_config_decode(v3, strlen(v3), &result) && result.button_mode == VCO_BUTTON_WESTERN &&
           result.bindings.toggle == VCO_SQUARE && result.bindings.close == VCO_CIRCLE);
    strcpy(data, v3);
    data[8] = '4';
    result = original;
    assert(!vco_config_decode(data, strlen(data), &result) &&
           vco_settings_equal(&result, &original));
    const char *fields[] = {"previous=256", "next=512",   "toggle=32768",
                            "close=8192",   "settings=8", "open=257"};
    const char *invalid[] = {"previous=16",         "next=256", "toggle=65536", "close=3",
                             "settings=4294967296", "open=256"};
    for (unsigned i = 0; i < 6; ++i) {
        const char *at = strstr(v3, fields[i]);
        assert(at);
        size_t before = (size_t)(at - v3), old = strlen(fields[i]),
               replacement = strlen(invalid[i]);
        memcpy(data, v3, before);
        memcpy(data + before, invalid[i], replacement);
        strcpy(data + before + replacement, at + old);
        result = original;
        assert(!vco_config_decode(data, strlen(data), &result));
        assert(vco_settings_equal(&result, &original));
    }
    for (size_t i = 0; i < strlen(v3) - strlen("encoding=0\n"); ++i) {
        result = original;
        assert(!vco_config_decode(v3, i, &result) && vco_settings_equal(&result, &original));
    }
    for (unsigned paging = 0; paging < 2; ++paging)
        for (unsigned toggle = 0; toggle < 4; ++toggle)
            for (unsigned close = 0; close < 4; ++close) {
                if (toggle == close)
                    continue;
                int n = snprintf(data, sizeof(data),
                                 "version=2\nlanguage=0\nopacity=70\npaging=%u\ntoggle=%u\nclose=%"
                                 "u\nencoding=2\n",
                                 paging, toggle, close);
                assert(n > 0 && (size_t)n < sizeof(data) &&
                       vco_config_decode(data, (size_t)n, &result));
                assert(result.bindings.previous == VCO_L && result.bindings.next == VCO_R &&
                       result.bindings.settings == VCO_TRIANGLE &&
                       result.bindings.open == VCO_COMBO &&
                       result.button_mode ==
                           (face(close) == VCO_CIRCLE ? VCO_BUTTON_WESTERN : VCO_BUTTON_ASIAN) &&
                       result.encoding == VCO_ENCODING_GBK);
            }
    /* v5 keeps its original IDs; v6 adds Japanese and Korean. */
    for (unsigned stored = 0; stored <= 3; ++stored) {
        int n = snprintf(data, sizeof(data),
                         "version=5\nlanguage=%u\nopacity=70\n"
                         "button_mode=1\nopen=257\nencoding=0\n",
                         stored);
        assert(n > 0 && vco_config_decode(data, (size_t)n, &result));
        assert(result.language == (int)stored - 1 && result.button_mode == VCO_BUTTON_WESTERN);
    }
    const char *unsupported[] = {
        "version=5\nlanguage=4\nopacity=70\nbutton_mode=0\nopen=257\nencoding=0\n",
        "version=6\nlanguage=6\nopacity=70\nbutton_mode=0\nopen=257\nencoding=0\n"};
    for (unsigned i = 0; i < 2; ++i)
        assert(!vco_config_decode(unsupported[i], strlen(unsupported[i]), &result));
    Disk disk = {0};
    VcoConfigIO io = {&disk, read_file, write_file, remove_file, rename_file};
    assert(vco_config_load(&io, &result) == VCO_CONFIG_DEFAULT);
    const char *v1 = "close=3\nversion=1\nlanguage=2\nopacity=35\npaging=1\ntoggle=1\n";
    assert(vco_config_decode(v1, strlen(v1), &result) && result.language == 1 &&
           result.opacity == 35 && result.bindings.previous == VCO_L &&
           result.bindings.next == VCO_R && result.bindings.toggle == VCO_SQUARE &&
           result.bindings.close == VCO_CROSS && result.bindings.settings == VCO_TRIANGLE &&
           result.bindings.open == VCO_COMBO && result.encoding == VCO_ENCODING_AUTO);
    memcpy(disk.bytes[0], v1, strlen(v1));
    disk.lengths[0] = strlen(v1);
    disk.exists[0] = true;
    for (unsigned encoding = VCO_ENCODING_AUTO; encoding <= VCO_ENCODING_GBK; ++encoding) {
        changed = result;
        changed.encoding = (VcoEncoding)encoding;
        size_t length = vco_config_encode(&changed, data, sizeof(data));
        assert(!memcmp(data, "version=6\n", 10) && vco_config_decode(data, length, &result) &&
               vco_settings_equal(&changed, &result));
        assert(vco_config_save(&io, &changed) &&
               vco_config_load(&io, &result) == VCO_CONFIG_LOADED &&
               vco_settings_equal(&changed, &result));
    }
    memset(&disk, 0, sizeof(disk));
    assert(vco_config_save(&io, &original));
    Disk baseline = disk;
    changed = original;
    changed.opacity = 35;
    vco_settings_set_button_mode(&changed, VCO_BUTTON_WESTERN);
    changed.bindings.open = VCO_R | VCO_START;
    changed.encoding = VCO_ENCODING_GBK;
    for (int fault = 1; fault <= 9; ++fault) {
        disk = baseline;
        disk.fail_at = fault;
        bool saved = vco_config_save(&io, &changed);
        disk.fail_at = 0;
        VcoConfigLoad loaded = vco_config_load(&io, &result);
        assert(loaded == VCO_CONFIG_LOADED || loaded == VCO_CONFIG_RECOVERED);
        assert(vco_settings_equal(&result, &original) || vco_settings_equal(&result, &changed));
        if (saved)
            assert(vco_settings_equal(&result, &changed));
    }
    disk = baseline;
    assert(vco_config_save(&io, &changed));
    disk.bytes[0][0] = '!';
    assert(vco_config_load(&io, &result) == VCO_CONFIG_RECOVERED);
    assert(vco_settings_equal(&result, &original));
    /* Crash after current -> backup: ignore a partial staging file. */
    disk.exists[0] = false;
    disk.exists[1] = true;
    disk.lengths[1] = 1;
    disk.bytes[1][0] = '!';
    assert(vco_config_load(&io, &result) == VCO_CONFIG_RECOVERED);
    assert(vco_settings_equal(&result, &original));
    assert(vco_config_save(&io, &changed));
    assert(vco_config_load(&io, &result) == VCO_CONFIG_LOADED);
    assert(vco_settings_equal(&result, &changed));
    return 0;
}
