#include "catalog/source.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static VcoSourceStamp stamp(const void *text, size_t length, VcoListSource source)
{
    VcoSourceStamp result;
    vco_source_begin(&result, source);
    assert(vco_source_add(&result, text, length));
    return result;
}
static void hash(void)
{
    VcoSourceStamp empty = stamp(NULL, 0, VCO_SOURCE_OWN);
    assert(empty.hash == UINT64_C(0xcbf29ce484222325) && !empty.length);
    assert(empty.result == VCO_FILE_OK && empty.source == VCO_SOURCE_OWN);
    VcoSourceStamp full = stamp("hello", 5, VCO_SOURCE_OWN);
    assert(full.hash == UINT64_C(0xa430d84680aabd0b) && full.length == 5);
    for (size_t split = 0; split <= 5; ++split) {
        VcoSourceStamp parts;
        vco_source_begin(&parts, VCO_SOURCE_OWN);
        assert(vco_source_add(&parts, "hello", split));
        assert(vco_source_add(&parts, NULL, 0));
        assert(vco_source_add(&parts, &"hello"[split], 5 - split));
        assert(vco_source_compare(&full, &parts) == VCO_SOURCE_UNCHANGED);
    }
    const unsigned char binary[] = {0, 0xff, 0x80, 'x', '\r', '\n', 0};
    VcoSourceStamp binary_full = stamp(binary, sizeof(binary), VCO_SOURCE_OWN);
    VcoSourceStamp bytewise;
    vco_source_begin(&bytewise, VCO_SOURCE_OWN);
    for (size_t i = 0; i < sizeof(binary); ++i)
        assert(vco_source_add(&bytewise, binary + i, 1));
    assert(binary_full.length == sizeof(binary));
    assert(vco_source_compare(&binary_full, &bytewise) == VCO_SOURCE_UNCHANGED);
}
static void changes(void)
{
    VcoSourceStamp loaded = stamp("abcdef", 6, VCO_SOURCE_OWN);
    VcoSourceStamp observed = stamp("abcdeg", 6, VCO_SOURCE_OWN);
    assert(loaded.length == observed.length && loaded.hash != observed.hash);
    assert(vco_source_compare(&loaded, &observed) == VCO_SOURCE_CHANGED);
    observed = stamp("abcdefg", 7, VCO_SOURCE_OWN);
    assert(vco_source_compare(&loaded, &observed) == VCO_SOURCE_CHANGED);
    observed = stamp("abcde", 5, VCO_SOURCE_OWN);
    assert(vco_source_compare(&loaded, &observed) == VCO_SOURCE_CHANGED);
    observed = loaded;
    assert(vco_source_compare(&loaded, &observed) == VCO_SOURCE_UNCHANGED);
    observed.source = VCO_SOURCE_VITACHEAT;
    assert(vco_source_compare(&loaded, &observed) == VCO_SOURCE_CHANGED);
    assert(vco_source_compare(&observed, &loaded) == VCO_SOURCE_CHANGED);
    loaded = stamp(NULL, 0, VCO_SOURCE_OWN);
    observed = stamp(NULL, 0, VCO_SOURCE_OWN);
    assert(vco_source_compare(&loaded, &observed) == VCO_SOURCE_UNCHANGED);
}
static void missing(void)
{
    VcoSourceStamp absent;
    vco_source_begin(&absent, VCO_SOURCE_NONE);
    absent.result = VCO_FILE_MISSING;
    VcoSourceStamp empty = stamp(NULL, 0, VCO_SOURCE_OWN);
    assert(vco_source_compare(&absent, &absent) == VCO_SOURCE_UNCHANGED);
    assert(vco_source_compare(&empty, &absent) == VCO_SOURCE_CHANGED);
    assert(vco_source_compare(&absent, &empty) == VCO_SOURCE_CHANGED);
    VcoSourceStamp legacy = stamp("legacy", 6, VCO_SOURCE_VITACHEAT);
    assert(vco_source_compare(&legacy, &absent) == VCO_SOURCE_CHANGED);
    assert(vco_source_compare(&absent, &legacy) == VCO_SOURCE_CHANGED);
    VcoSourceStamp large = legacy;
    large.result = VCO_FILE_TOO_LARGE;
    assert(vco_source_compare(&absent, &large) == VCO_SOURCE_CHANGED);
    assert(vco_source_compare(&large, &absent) == VCO_SOURCE_CHANGED);
}
static void failures(void)
{
    VcoSourceStamp valid = stamp("data", 4, VCO_SOURCE_OWN);
    VcoSourceStamp invalid = valid;
    invalid.result = VCO_FILE_ERROR;
    assert(vco_source_compare(&valid, &invalid) == VCO_SOURCE_UNAVAILABLE);
    assert(vco_source_compare(&invalid, &valid) == VCO_SOURCE_UNAVAILABLE);
    assert(vco_source_compare(&invalid, &invalid) == VCO_SOURCE_UNAVAILABLE);
    invalid.result = VCO_FILE_TOO_LARGE;
    assert(vco_source_compare(&valid, &invalid) == VCO_SOURCE_CHANGED);
    assert(vco_source_compare(&invalid, &valid) == VCO_SOURCE_CHANGED);
    assert(vco_source_compare(&invalid, &invalid) == VCO_SOURCE_UNAVAILABLE);
    invalid.result = (VcoFileResult)99;
    assert(vco_source_compare(&invalid, &valid) == VCO_SOURCE_UNAVAILABLE);
    assert(vco_source_compare(&valid, &invalid) == VCO_SOURCE_UNAVAILABLE);
    invalid.result = (VcoFileResult)-1;
    assert(vco_source_compare(&invalid, &valid) == VCO_SOURCE_UNAVAILABLE);
    invalid = valid;
    invalid.source = (VcoListSource)99;
    assert(vco_source_compare(&invalid, &valid) == VCO_SOURCE_UNAVAILABLE);
    assert(vco_source_compare(&valid, &invalid) == VCO_SOURCE_UNAVAILABLE);
    invalid.source = (VcoListSource)-1;
    assert(vco_source_compare(&invalid, &valid) == VCO_SOURCE_UNAVAILABLE);
    invalid = (VcoSourceStamp){0};
    assert(vco_source_compare(&invalid, &invalid) == VCO_SOURCE_UNAVAILABLE);
    VcoSourceStamp absent = valid;
    absent.result = VCO_FILE_MISSING;
    absent.source = VCO_SOURCE_NONE;
    assert(vco_source_compare(&invalid, &absent) == VCO_SOURCE_UNAVAILABLE);
    assert(vco_source_compare(&absent, &invalid) == VCO_SOURCE_UNAVAILABLE);
    invalid = valid;
    invalid.source = VCO_SOURCE_NONE;
    assert(vco_source_compare(&invalid, &valid) == VCO_SOURCE_UNAVAILABLE);
    assert(vco_source_compare(&valid, NULL) == VCO_SOURCE_UNAVAILABLE);
    assert(vco_source_compare(NULL, &valid) == VCO_SOURCE_UNAVAILABLE);
    invalid = valid;
    invalid.length = VCO_CATALOG_TEXT + 1u;
    assert(vco_source_compare(&invalid, &invalid) == VCO_SOURCE_UNAVAILABLE);
}
static void bounds(void)
{
    static unsigned char bytes[VCO_CATALOG_TEXT];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (unsigned char)(i * 17u + 3u);
    VcoSourceStamp full = stamp(bytes, sizeof(bytes), VCO_SOURCE_OWN);
    VcoSourceStamp parts;
    vco_source_begin(&parts, VCO_SOURCE_OWN);
    for (size_t i = 0; i < sizeof(bytes); i += 2048)
        assert(vco_source_add(&parts, bytes + i, 2048));
    assert(vco_source_compare(&full, &parts) == VCO_SOURCE_UNCHANGED);
    assert(vco_source_add(&parts, NULL, 0));
    assert(!vco_source_add(&parts, bytes, 1));
    assert(parts.result == VCO_FILE_TOO_LARGE && parts.length == full.length &&
           parts.hash == full.hash);
    assert(!vco_source_add(&parts, NULL, 0));
    assert(vco_source_compare(&full, &parts) == VCO_SOURCE_CHANGED);
    vco_source_begin(&parts, VCO_SOURCE_OWN);
    uint64_t initial = parts.hash;
    assert(!vco_source_add(&parts, bytes, SIZE_MAX));
    assert(parts.result == VCO_FILE_TOO_LARGE && !parts.length && parts.hash == initial);
    vco_source_begin(&parts, VCO_SOURCE_OWN);
    assert(!vco_source_add(&parts, NULL, 1));
    assert(parts.result == VCO_FILE_ERROR && !parts.length && parts.hash == initial);
    assert(!vco_source_add(&parts, bytes, 1));
    vco_source_begin(&parts, VCO_SOURCE_OWN);
    parts.length = VCO_CATALOG_TEXT + 1u;
    assert(!vco_source_add(&parts, NULL, 0) && parts.result == VCO_FILE_ERROR);
    vco_source_begin(NULL, VCO_SOURCE_OWN);
    assert(!vco_source_add(NULL, bytes, 1));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "hash"))
        hash();
    else if (!strcmp(argv[1], "changes"))
        changes();
    else if (!strcmp(argv[1], "missing"))
        missing();
    else if (!strcmp(argv[1], "failures"))
        failures();
    else if (!strcmp(argv[1], "bounds"))
        bounds();
    else
        assert(0);
    printf("PASS source %s\n", argv[1]);
    return 0;
}
