#include "cheats.h"
#include "metadata.h"
#include "../tools/device-harness/codes.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define BASE UINT32_C(0x81000000)
static union {
    uint64_t alignment;
    unsigned char bytes[4096];
} region;
#define PATCHES (VCO_HARNESS_BULK_PATCHES + 8u)
static struct {
    uint8_t original[4];
    uint32_t address;
    size_t size;
    int live;
} injected[PATCHES];
static unsigned issued, live_patches;
static int alive(void *ctx, uint64_t token)
{
    (void)ctx;
    return token == 7;
}
static int read_memory(void *ctx, uint64_t token, uint32_t address, uint8_t *out, size_t size)
{
    (void)ctx;
    assert(token == 7 && address >= BASE &&
           (uint64_t)address + size <= (uint64_t)BASE + sizeof(region.bytes));
    memcpy(out, region.bytes + (address - BASE), size);
    return 0;
}
static int write_memory(void *ctx, uint64_t token, uint32_t address, const uint8_t *in, size_t size)
{
    (void)ctx;
    assert(token == 7 && address >= BASE &&
           (uint64_t)address + size <= (uint64_t)BASE + sizeof(region.bytes));
    memcpy(region.bytes + (address - BASE), in, size);
    return 0;
}
static int inject(void *ctx, uint64_t token, uint32_t address, const uint8_t *in, size_t size,
                  int32_t *handle)
{
    assert(issued < PATCHES && size <= sizeof(injected[0].original));
    unsigned index = issued++;
    read_memory(ctx, token, address, injected[index].original, size);
    injected[index].address = address;
    injected[index].size = size;
    injected[index].live = 1;
    ++live_patches;
    *handle = (int32_t)index + 1;
    return write_memory(ctx, token, address, in, size);
}
static int release(void *ctx, uint64_t token, int32_t handle)
{
    assert(handle > 0 && (unsigned)handle <= issued && injected[handle - 1].live);
    unsigned index = (unsigned)handle - 1;
    injected[index].live = 0;
    --live_patches;
    return write_memory(ctx, token, injected[index].address, injected[index].original,
                        injected[index].size);
}
static int module(void *ctx, uint64_t token, unsigned index, unsigned segment, uint32_t *base,
                  uint32_t *size)
{
    (void)ctx;
    assert(token == 7 && index == 3 && segment <= 1);
    *base = BASE;
    *size = sizeof(region.bytes);
    return 0;
}
static int pad(void *ctx, uint64_t token, uint32_t *device, uint32_t *buttons)
{
    (void)ctx;
    assert(token == 7);
    *device = 1;
    *buttons = 0x200;
    return 0;
}
static int generate(char *text, size_t size)
{
    return vco_harness_codes(text, size, BASE + 0x100, BASE + 0x800, BASE + 0x820, 3, BASE,
                             BASE + 0x900, BASE);
}
static void vectors(void)
{
    char text[8192], names[2048];
    VcoCheatEntry entries[16];
    VcoCode codes[256];
    VcoCheatOp operations[256];
    VcoPatch patches[256];
    VcoCheatFile file = {entries, 16, 0, codes, 256, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    int length = generate(text, sizeof(text));
    assert(length > 0 && (size_t)length < sizeof(text));
    assert(!vco_cheat_parse(text, (size_t)length, &file, &diagnostic));
    VcoFileMetadata metadata;
    vco_metadata_parse(text, (size_t)length, &metadata);
    assert(vco_metadata_compatibility(&metadata, "VCOCH0001", VCO_HARNESS_VERSION) ==
           VCO_COMPAT_MATCH);
    assert(vco_metadata_compatibility(&metadata, "VCOCH0001", "99.99") == VCO_COMPAT_MISMATCH);
    assert(file.entry_count == 16 && file.entries[0].file_enabled);
    for (unsigned i = 1; i < file.entry_count; ++i)
        assert(!file.entries[i].file_enabled);
    const uint8_t thumb32[8] = {0x00, 0xBF, 0x4F, 0xF0, 0x07, 0x00, 0x70, 0x47};
    VcoCheatMemory memory = {NULL, alive, read_memory, write_memory, module, pad, inject, release};
    for (size_t i = 0; i < file.entry_count; ++i) {
        memset(&region, 0, sizeof(region));
        memset(injected, 0, sizeof(injected));
        issued = live_patches = 0;
        VcoHarnessData *data = (VcoHarnessData *)(void *)(region.bytes + 0x100);
        data->source = 1234;
        data->patch = 5;
        data->pointer = BASE + 0x100 + offsetof(VcoHarnessData, target_a);
        data->source_pointer = BASE + 0x100 + offsetof(VcoHarnessData, source);
        region.bytes[0x800] = 7;
        region.bytes[0x801] = 0x20;
        memcpy(region.bytes + 0x820, thumb32, sizeof(thumb32));
        for (unsigned n = 0; n < VCO_HARNESS_BULK_PATCHES; ++n)
            data->bulk[n] = VCO_HARNESS_BULK_ORIGINAL + n;
        VcoCheatProgram program = {operations, 256, 0, NULL, 0};
        VcoCheatRuntime runtime;
        assert(!vco_cheat_compile(&file, i, &program, &diagnostic) && !program.needs_review);
        assert(!vco_cheat_runtime_init(&runtime, &program, memory, 7, patches, 256, 256, 0));
        assert(!vco_cheat_enable(&runtime));
        switch (i) {
        case 0:
            assert(data->direct32 == 100);
            break;
        case 1:
            assert(data->direct16 == 25);
            break;
        case 2:
            assert(data->direct8 == 7);
            break;
        case 3:
            assert(data->copy == 1234);
            break;
        case 4:
            assert(data->repeat[0] == 10 && data->repeat[1] == 11 && data->repeat[2] == 12);
            break;
        case 5:
            assert(data->target_a[0] == 55);
            break;
        case 6:
            assert(data->target_a[0] == 1234);
            break;
        case 7:
            assert(data->target_a[0] == 20 && data->target_a[1] == 21 && data->target_a[2] == 22);
            break;
        case 8:
            assert(data->condition == 66);
            break;
        case 9:
            assert(data->pad == 77);
            break;
        case 10:
            assert(data->patch == 88);
            break;
        case 11:
        case 12:
            assert(region.bytes[0x800] == 9 && region.bytes[0x801] == 0x20);
            break;
        case 13:
            assert(region.bytes[0x900] == 99);
            break;
        case 14:
            assert(program.count == 1 && program.ops[0].a == BASE + 0x822 &&
                   program.ops[0].width == 4 && (program.ops[0].a & 3) == 2);
            assert(region.bytes[0x824] == 9 && !memcmp(region.bytes + 0x820, thumb32, 4) &&
                   !memcmp(region.bytes + 0x825, thumb32 + 5, 3));
            break;
        case 15:
            assert(program.count == 117 && issued == 117 && live_patches == 117);
            for (unsigned n = 0; n < VCO_HARNESS_BULK_PATCHES; ++n)
                assert(data->bulk[n] == VCO_HARNESS_BULK_PATCHED + n);
            break;
        default:
            assert(0);
        }
        unsigned before = issued;
        assert(!vco_cheat_tick(&runtime) && issued == before);
        assert(!vco_cheat_disable(&runtime));
        assert(data->patch == 5 && region.bytes[0x800] == 7 && !live_patches &&
               !memcmp(region.bytes + 0x820, thumb32, sizeof(thumb32)));
        for (unsigned n = 0; n < VCO_HARNESS_BULK_PATCHES; ++n)
            assert(data->bulk[n] == VCO_HARNESS_BULK_ORIGINAL + n);
    }
    puts("all generated device harness cases passed parser/compiler/runtime");
}
static void bounds(void)
{
    char text[8192];
    int required = generate(text, sizeof(text));
    assert(required > 0 && required < (int)sizeof(text) && text[required] == '\0');
    for (size_t size = 1; size <= (size_t)required; ++size) {
        memset(text, 0x5a, sizeof(text));
        assert(generate(text, size) == -1 && !text[0]);
        for (size_t i = size; i < sizeof(text); ++i)
            assert(text[i] == 0x5a);
    }
    assert(generate(text, (size_t)required + 1) == required);
    assert(generate(NULL, 0) == -1);
    assert(generate(text, 0) == -1);
    const uint32_t addresses[][3] = {{0, BASE + 0x800, BASE + 0x820},
                                     {UINT32_MAX - 3, BASE + 0x800, BASE + 0x820},
                                     {BASE + 0x100, UINT32_MAX - 1, BASE + 0x820},
                                     {BASE + 0x100, BASE + 0x800, UINT32_MAX - 3},
                                     {BASE + 0x100, BASE + 0x800, BASE + 0x822}};
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        memset(text, 0x5a, sizeof(text));
        assert(vco_harness_codes(text, sizeof(text), addresses[i][0], addresses[i][1],
                                 addresses[i][2], 3, BASE, BASE + 0x900, BASE) == -1);
        assert(!text[0]);
    }
}
static void observations(void)
{
    VcoHarnessData data = {0};
    unsigned counts[3];
    for (unsigned i = 0; i < VCO_HARNESS_BULK_PATCHES; ++i)
        data.bulk[i] = VCO_HARNESS_BULK_ORIGINAL + i;
    vco_harness_bulk_counts(&data, counts);
    assert(counts[0] == 117 && !counts[1] && !counts[2]);
    data.bulk[116] = VCO_HARNESS_BULK_PATCHED + 116;
    data.bulk[0] = UINT32_MAX;
    vco_harness_bulk_counts(&data, counts);
    assert(counts[0] == 115 && counts[1] == 1 && counts[2] == 1);
    for (unsigned i = 0; i < VCO_HARNESS_BULK_PATCHES; ++i)
        data.bulk[i] = VCO_HARNESS_BULK_PATCHED + i;
    vco_harness_bulk_counts(&data, counts);
    assert(!counts[0] && counts[1] == 117 && !counts[2]);
    uint16_t words[4] = {0xBF00, 0xF04F, 7, 0x4770};
    assert(vco_harness_thumb32_expected(words) == 7);
    words[2] = 9;
    assert(vco_harness_thumb32_expected(words) == 9);
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t saved = words[i];
        words[i] = 0;
        assert(vco_harness_thumb32_expected(words) == -1);
        words[i] = saved;
    }
    assert(vco_harness_thumb32_expected(NULL) == -1);
}
int main(int argc, char **argv)
{
    if (argc == 1)
        vectors();
    else if (argc == 2 && !strcmp(argv[1], "bounds"))
        bounds();
    else if (argc == 2 && !strcmp(argv[1], "observations"))
        observations();
    else
        assert(0);
    return 0;
}
