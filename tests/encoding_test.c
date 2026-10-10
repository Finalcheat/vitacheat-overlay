#include "encoding.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *input, size_t input_size, VcoEncoding policy, const char *expected,
                  size_t expected_size, VcoEncoding encoding)
{
    char data[128];
    unsigned char scratch[16];
    assert(input_size <= sizeof(data) && expected_size <= sizeof(data));
    memset(data, 0x5a, sizeof(data));
    memcpy(data, input, input_size);
    size_t size = input_size;
    VcoDiagnostic diagnostic = {VCO_E_IO, 999};
    VcoEncoding detected = VCO_ENCODING_AUTO;
    assert(vco_encoding_decode(data, &size, sizeof(data), policy, scratch, sizeof(scratch),
                               &detected, &diagnostic) == VCO_OK);
    assert(detected == encoding && !diagnostic.code && !diagnostic.line && size == expected_size &&
           !memcmp(data, expected, size));
    for (size_t i = size; i < sizeof(data); ++i)
        assert(data[i] == 0x5a);
}
static void utf8(void)
{
    const char *strings[] = {
        "", "abc\r\n", "中文😀", "\xef\xbb\xbf中文\n",
        "\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"};
    for (unsigned i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i)
        for (unsigned mode = VCO_ENCODING_AUTO; mode <= VCO_ENCODING_UTF8; ++mode)
            check(strings[i], strlen(strings[i]), (VcoEncoding)mode, strings[i], strlen(strings[i]),
                  VCO_ENCODING_UTF8);
    char text[] = "abc";
    size_t size = 3;
    /* UTF-8 does not need scratch. */
    assert(!vco_encoding_decode(text, &size, size, VCO_ENCODING_AUTO, NULL, 0, NULL, NULL));
}
static void gbk(void)
{
    const char *input = "_V1 \xce\xd2\xb7\xbd\n$0200 81000000 11223344\r\n";
    const char *expected = "_V1 我方\n$0200 81000000 11223344\r\n";
    check(input, strlen(input), VCO_ENCODING_AUTO, expected, strlen(expected), VCO_ENCODING_GBK);
    check(input, strlen(input), VCO_ENCODING_GBK, expected, strlen(expected), VCO_ENCODING_GBK);
    /* ASCII trails, high trails (including 80), two/three-byte UTF-8 and Euro. */
    check("\x81\x40\xa1\xe8\x81\x80\x80", 7, VCO_ENCODING_GBK,
          "\xe4\xb8\x82\xc2\xa4\xe4\xba\x90\xe2\x82\xac", 11, VCO_ENCODING_GBK);
    check("ASCII only", 10, VCO_ENCODING_GBK, "ASCII only", 10, VCO_ENCODING_GBK);
    /* C2 A1 is valid in both encodings: explicit selection must win. */
    check("\xc2\xa1", 2, VCO_ENCODING_AUTO, "¡", 2, VCO_ENCODING_UTF8);
    check("\xc2\xa1", 2, VCO_ENCODING_GBK, "隆", 3, VCO_ENCODING_GBK);
}
static void rejected(const char *input, size_t input_size, size_t capacity, VcoEncoding policy,
                     size_t scratch_capacity, VcoError error, uint32_t line)
{
    char data[128], before[128];
    unsigned char scratch[32];
    assert(input_size <= sizeof(data) && capacity <= sizeof(data) &&
           scratch_capacity <= sizeof(scratch) - 2);
    memset(data, 0x5a, sizeof(data));
    memcpy(data, input, input_size);
    memcpy(before, data, sizeof(data));
    memset(scratch, 0xa5, sizeof(scratch));
    VcoEncoding encoding = VCO_ENCODING_GBK;
    VcoDiagnostic diagnostic = {0, 0};
    size_t size = input_size;
    assert(vco_encoding_decode(data, &size, capacity, policy, scratch + 1, scratch_capacity,
                               &encoding, &diagnostic) == error);
    assert(size == input_size && !memcmp(data, before, sizeof(data)) &&
           encoding == VCO_ENCODING_AUTO && diagnostic.code == error && diagnostic.line == line &&
           scratch[0] == 0xa5 && scratch[scratch_capacity + 1] == 0xa5);
}
static void invalid(void)
{
    const char *utf8_bad[] = {"\xc0\xaf",         "\xc1\xbf",         "\xc2",
                              "\xe0\x9f\xbf",     "\xed\xa0\x80",     "\xf0\x8f\xbf\xbf",
                              "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\x80",
                              "\xe4\xb8",         "\xe4\x41\x80"};
    for (unsigned i = 0; i < sizeof(utf8_bad) / sizeof(utf8_bad[0]); ++i)
        rejected(utf8_bad[i], strlen(utf8_bad[i]), 128, VCO_ENCODING_UTF8, 16, VCO_E_ENCODING, 1);
    const char *gbk_bad[] = {"\x81", "\x81\x7f", "\x81\x30", "\xff", "\xfe\xfe"};
    for (unsigned i = 0; i < sizeof(gbk_bad) / sizeof(gbk_bad[0]); ++i)
        for (unsigned mode = VCO_ENCODING_AUTO; mode <= VCO_ENCODING_GBK; ++mode)
            rejected(gbk_bad[i], strlen(gbk_bad[i]), 128, (VcoEncoding)mode, 16, VCO_E_ENCODING, 1);
    const char *bom_bad[] = {"\xff\xfe", "\xfe\xff", "\0\0\xfe\xff", "\xef\xbb\xbf\x81"};
    const size_t sizes[] = {2, 2, 4, 4};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned mode = VCO_ENCODING_AUTO; mode <= VCO_ENCODING_GBK; ++mode)
            rejected(bom_bad[i], sizes[i], 128, (VcoEncoding)mode, 16, VCO_E_ENCODING, 1);
    rejected("\xef\xbb\xbf"
             "abc",
             6, 128, VCO_ENCODING_GBK, 16, VCO_E_ENCODING, 1);
    for (unsigned mode = VCO_ENCODING_AUTO; mode <= VCO_ENCODING_GBK; ++mode)
        rejected("abc\n\0def", 8, 128, (VcoEncoding)mode, 16, VCO_E_ENCODING, 2);
    rejected("abc\r\n\x81\x30", 7, 128, VCO_ENCODING_GBK, 16, VCO_E_ENCODING, 2);
    rejected("abc", 3, 128, (VcoEncoding)99, 16, VCO_E_ARGUMENT, 0);
    size_t size = 1;
    assert(vco_encoding_decode(NULL, &size, 1, VCO_ENCODING_AUTO, NULL, 0, NULL, NULL) ==
           VCO_E_ARGUMENT);
    assert(vco_encoding_decode("a", NULL, 1, VCO_ENCODING_AUTO, NULL, 0, NULL, NULL) ==
           VCO_E_ARGUMENT);
}
static void limits(void)
{
    for (size_t i = 0; i < 300; ++i)
        assert(vco_encoding_scratch_size(i) == (i + 7) / 8);
    assert(vco_encoding_scratch_size(SIZE_MAX) == SIZE_MAX / 8 + 1);
    rejected("abc", 3, 2, VCO_ENCODING_AUTO, 16, VCO_E_LIMIT, 0);
    rejected("\xce\xd2", 2, 2, VCO_ENCODING_GBK, 1, VCO_E_LIMIT, 1);
    rejected("\x80", 1, 2, VCO_ENCODING_GBK, 1, VCO_E_LIMIT, 1);
    rejected("\xce\xd2", 2, 128, VCO_ENCODING_GBK, 0, VCO_E_LIMIT, 0);
    rejected("1234567\xce\xd2", 9, 128, VCO_ENCODING_GBK, 1, VCO_E_LIMIT, 0);
    char data[10] = {0};
    memcpy(data + 1, "\xce\xd2", 2);
    unsigned char bits = 0;
    size_t size = 2;
    assert(!vco_encoding_decode(data + 1, &size, 3, VCO_ENCODING_GBK, &bits, 1, NULL, NULL) &&
           size == 3 && !memcmp(data + 1, "我", 3) && !data[0] && !data[4]);
}
static void maximum(void)
{
    const size_t capacity = 256u * 1024u;
    char *data = malloc(capacity + 2), *before = malloc(capacity);
    unsigned char *bits = malloc(vco_encoding_scratch_size(capacity) + 2);
    assert(data && before && bits);
    memset(data, 0x5a, capacity + 2);
    memset(bits, 0xa5, vco_encoding_scratch_size(capacity) + 2);
    size_t pairs = capacity / 3, input = pairs * 2;
    for (size_t i = 0; i < pairs; ++i)
        memcpy(data + 1 + i * 2, "\xce\xd2", 2);
    size_t size = input;
    assert(!vco_encoding_decode(data + 1, &size, capacity, VCO_ENCODING_GBK, bits + 1,
                                vco_encoding_scratch_size(capacity), NULL, NULL));
    assert(size == pairs * 3 && data[0] == 0x5a && data[capacity + 1] == 0x5a && bits[0] == 0xa5 &&
           bits[vco_encoding_scratch_size(capacity) + 1] == 0xa5);
    for (size_t i = 0; i < pairs; ++i)
        assert(!memcmp(data + 1 + i * 3, "我", 3));
    memset(data + 1, 0x80, capacity);
    memcpy(before, data + 1, capacity);
    size = capacity;
    assert(vco_encoding_decode(data + 1, &size, capacity, VCO_ENCODING_GBK, bits + 1,
                               vco_encoding_scratch_size(capacity), NULL, NULL) == VCO_E_LIMIT &&
           size == capacity && !memcmp(data + 1, before, capacity));
    /* Full-size ASCII and shrinking-forbidden two-byte mappings are accepted. */
    for (size_t i = 0; i < capacity / 2; ++i)
        memcpy(data + 1 + i * 2, "\xa1\xe8", 2);
    size = capacity;
    assert(!vco_encoding_decode(data + 1, &size, capacity, VCO_ENCODING_GBK, bits + 1,
                                vco_encoding_scratch_size(capacity), NULL, NULL) &&
           size == capacity);
    for (size_t i = 0; i < capacity / 2; ++i)
        assert(!memcmp(data + 1 + i * 2, "¤", 2));
    free(bits);
    free(before);
    free(data);
}
static uint32_t random_word(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}
static void mutations(void)
{
    uint32_t state = 17;
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        char input[32], data[100], reference[100];
        unsigned char scratch[16];
        size_t length = random_word(&state) % sizeof(input), expected_size = 0;
        int valid = 1;
        for (size_t i = 0; i < length; ++i)
            input[i] = (char)(random_word(&state) >> 24);
        /* Independent forward decoding using the public converter on one
         * token at a time. Checks reverse traversal and bitset boundary logic. */
        for (size_t i = 0; i < length;) {
            unsigned c = (unsigned char)input[i];
            size_t width = c >= 0x81 && c <= 0xfe ? 2 : 1;
            char token[4] = {0};
            size_t size = width;
            if (width > length - i) {
                valid = 0;
                break;
            }
            memcpy(token, input + i, width);
            if (vco_encoding_decode(token, &size, sizeof(token), VCO_ENCODING_GBK, scratch,
                                    sizeof(scratch), NULL, NULL)) {
                valid = 0;
                break;
            }
            memcpy(reference + expected_size, token, size);
            expected_size += size;
            i += width;
        }
        size_t capacity = random_word(&state) % sizeof(data);
        memset(data, 0x5a, sizeof(data));
        memcpy(data, input, length);
        size_t size = length;
        VcoError error = vco_encoding_decode(data, &size, capacity, VCO_ENCODING_GBK, scratch,
                                             sizeof(scratch), NULL, NULL);
        if (valid && length <= capacity && expected_size <= capacity) {
            assert(!error && size == expected_size && !memcmp(data, reference, size));
        } else
            assert(error && size == length && !memcmp(data, input, length));
        for (size_t i = error ? length : size; i < sizeof(data); ++i)
            assert(data[i] == 0x5a);
    }
}
static void oracle(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    unsigned char record[6], scratch[1];
    unsigned count = 0, accepted = 0;
    while (fread(record, sizeof(record), 1, file) == 1) {
        char data[4] = {(char)record[0], (char)record[1], 0x5a, 0x5a};
        size_t size = 2;
        VcoError error =
            vco_encoding_decode(data, &size, 3, VCO_ENCODING_GBK, scratch, 1, NULL, NULL);
        if (record[2]) {
            assert(!error && size == record[2] && !memcmp(data, record + 3, size));
            ++accepted;
        } else
            assert(error == VCO_E_ENCODING && size == 2 && (unsigned char)data[0] == record[0] &&
                   (unsigned char)data[1] == record[1]);
        assert(data[3] == 0x5a);
        ++count;
    }
    assert(!ferror(file) && feof(file) && !fclose(file) && count == 126u * 190u &&
           accepted == 21791);
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (!strcmp(argv[1], "utf8"))
        utf8();
    else if (!strcmp(argv[1], "gbk"))
        gbk();
    else if (!strcmp(argv[1], "invalid"))
        invalid();
    else if (!strcmp(argv[1], "limits"))
        limits();
    else if (!strcmp(argv[1], "maximum"))
        maximum();
    else if (!strcmp(argv[1], "mutations"))
        mutations();
    else if (!strcmp(argv[1], "oracle") && argc == 3)
        oracle(argv[2]);
    else
        assert(!"unknown test");
    return 0;
}
