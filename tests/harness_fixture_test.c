#include "../tools/device-harness/fixture.h"
#include "../tools/device-harness/codes.h"
#include "cheats.h"
#include "metadata.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BASE UINT32_C(0x83000000)
static VcoFixtureController controller;
static unsigned calls[4], fail_at, malformed, range_fault;
static bool consumed;
static int32_t load(void *ctx, unsigned variant)
{
    assert(ctx == &controller && variant < 2 && controller.state == VCO_FIXTURE_LOADING);
    ++calls[0];
    assert(vco_fixture_load(&controller) < 0); /* Native callback cannot reenter admission. */
    return fail_at == 1 ? -201 : 100 + (int32_t)variant;
}
static int start(void *ctx, int32_t uid, const VcoFixtureStart *args, int *status)
{
    assert(ctx == &controller && uid == 100 + (int32_t)args->variant);
    ++calls[1];
    /* Match Modulemgr's copy semantics: output must use a contained pointer. */
    VcoFixtureStart copy = *args;
    assert(copy.abi == VCO_FIXTURE_ABI && copy.size == sizeof(copy) && copy.generation);
    *copy.reply = (VcoFixtureReply){VCO_FIXTURE_ABI,  sizeof(VcoFixtureReply), copy.generation,
                                    copy.variant,     BASE + 0x2000,           BASE + 0x1011,
                                    5 + copy.variant, copy.variant ? 11 : 7,   uid};
    switch (malformed) {
    case 1:
        copy.reply->abi++;
        break;
    case 2:
        copy.reply->size--;
        break;
    case 3:
        copy.reply->generation--;
        break;
    case 4:
        copy.reply->variant ^= 1;
        break;
    case 5:
        copy.reply->data_initial++;
        break;
    case 6:
        copy.reply->function_initial++;
        break;
    case 7:
        copy.reply->function_address &= ~1u;
        break;
    case 8:
        copy.reply->data_address++;
        break;
    case 9:
        copy.reply->function_address = 1;
        break;
    case 10:
        copy.reply->data_address = 0;
        break;
    case 11:
        copy.reply->module_uid++;
        break;
    default:
        break;
    }
    *status = fail_at == 3 ? 2 : 0;
    return fail_at == 2 ? -202 : 0;
}
static int info(void *ctx, int32_t uid, VcoFixtureInfo *out)
{
    assert(ctx == &controller && uid > 0);
    ++calls[2];
    *out = (VcoFixtureInfo){uid, 6, 3, {BASE + 0x1000, 0x100, 5}, {BASE + 0x2000, 0x1000, 6}};
    switch (range_fault) {
    case 1:
        out->uid++;
        break;
    case 2:
        out->state = 2;
        break;
    case 3:
        out->index = 256;
        break;
    case 4:
        out->text.size = 0;
        break;
    case 5:
        out->data.size = 0;
        break;
    case 6:
        out->text.base = 0x1000;
        break;
    case 7:
        out->text.size = UINT32_MAX;
        break;
    case 8:
        out->data.size = UINT32_MAX;
        break;
    case 9:
        out->text.permissions = 7;
        break;
    case 10:
        out->data.permissions = 4;
        break;
    case 11:
        out->text.size = 0x11;
        break;
    case 12:
        out->data.base += 4;
        break;
    case 13:
        out->data.size = 3;
        break;
    default:
        break;
    }
    return fail_at == 4 ? -204 : 0;
}
static int stop(void *ctx, int32_t uid, int *status)
{
    assert(ctx == &controller && uid > 0 && controller.state == VCO_FIXTURE_UNLOADING);
    ++calls[3];
    assert(!controller.reply.function_address && !controller.reply.data_address &&
           !controller.info.uid);
    assert(vco_fixture_unload(&controller) < 0);
    consumed = true; /* A failed return may already have consumed the module. */
    *status = fail_at == 6 ? 1 : 0;
    return fail_at == 5 ? -205 : 0;
}
static void reset(void)
{
    memset(calls, 0, sizeof(calls));
    fail_at = malformed = range_fault = 0;
    consumed = false;
    vco_fixture_init(&controller, (VcoFixtureOps){&controller, load, start, info, stop});
}
static void inert_fault(void)
{
    assert(controller.state == VCO_FIXTURE_FAULT && !controller.reply.function_address &&
           !controller.info.uid);
    unsigned before[4];
    memcpy(before, calls, sizeof(calls));
    assert(vco_fixture_load(&controller) < 0 && vco_fixture_unload(&controller) < 0 &&
           vco_fixture_step(&controller) < 0);
    char text[32] = "old";
    assert(vco_fixture_codes(&controller, text, sizeof(text)) < 0 && !text[0]);
    assert(!memcmp(before, calls, sizeof(calls)));
}
static void lifecycle(void)
{
    assert(!vco_fixture_load(&controller));
    assert(controller.state == VCO_FIXTURE_READY && controller.uid == 100 &&
           controller.generation == 1);
    assert(controller.loads == 1 && !controller.unloads && controller.reply.data_initial == 5);
    assert(vco_fixture_load(&controller) < 0 && calls[0] == 1);
    assert(!vco_fixture_unload(&controller));
    assert(consumed && controller.state == VCO_FIXTURE_EMPTY && controller.uid == -1 &&
           !controller.reply.function_address);
    assert(vco_fixture_unload(&controller) < 0 && calls[3] == 1);
    assert(!vco_fixture_load(&controller));
    assert(controller.uid == 101 && controller.generation == 2 &&
           controller.reply.data_initial == 6 && controller.reply.function_initial == 11);
    assert(!vco_fixture_unload(&controller));
}
static void contract(void)
{
    for (unsigned i = 1; i <= 11; ++i) {
        reset();
        malformed = i;
        assert(vco_fixture_load(&controller) < 0 && controller.uid == 100);
        assert(calls[0] == 1 && calls[1] == 1 && calls[2] == 1 && !calls[3]);
        inert_fault();
    }
}
static void ranges(void)
{
    for (unsigned i = 1; i <= 13; ++i) {
        reset();
        range_fault = i;
        assert(vco_fixture_load(&controller) < 0 && controller.uid == 100);
        inert_fault();
    }
}
static void faults(void)
{
    for (unsigned i = 1; i <= 6; ++i) {
        reset();
        if (i <= 4) {
            fail_at = i;
            assert(vco_fixture_load(&controller) < 0);
            assert(controller.uid == (i == 1 ? -201 : 100));
            assert(controller.native_result == (i == 1 ? -201 : i == 2 ? -202 : i == 4 ? -204 : 0));
        } else {
            assert(!vco_fixture_load(&controller));
            fail_at = i;
            assert(vco_fixture_unload(&controller) < 0 && consumed && controller.uid == 100);
            assert(controller.native_result == (i == 5 ? -205 : 0));
            assert(!controller.unloads);
        }
        inert_fault();
    }
}
static void cycles(void)
{
    for (unsigned i = 0; i < 200; ++i) {
        assert(!vco_fixture_step(&controller));
        if (!(i & 1))
            assert(controller.state == VCO_FIXTURE_READY && controller.generation == i / 2 + 1 &&
                   controller.reply.variant == (i / 2 & 1));
        else
            assert(controller.state == VCO_FIXTURE_EMPTY);
    }
    assert(controller.loads == 100 && controller.unloads == 100 && calls[0] == 100 &&
           calls[3] == 100);
}
static uint8_t bytes[0x3000], saved[4];
static uint32_t patched;
static size_t width;
static bool active;
static int alive(void *ctx, uint64_t token)
{
    (void)ctx;
    return token == 7;
}
static int read_data(void *ctx, uint64_t token, uint32_t addr, uint8_t *out, size_t n)
{
    (void)ctx;
    assert(token == 7 && addr >= BASE && (uint64_t)addr + n <= BASE + sizeof(bytes));
    memcpy(out, bytes + (addr - BASE), n);
    return 0;
}
static int write_data(void *ctx, uint64_t token, uint32_t addr, const uint8_t *in, size_t n)
{
    (void)ctx;
    assert(token == 7 && addr >= BASE && (uint64_t)addr + n <= BASE + sizeof(bytes));
    memcpy(bytes + (addr - BASE), in, n);
    return 0;
}
static int inject(void *ctx, uint64_t token, uint32_t addr, const uint8_t *in, size_t n,
                  int32_t *key)
{
    assert(!active && n <= sizeof(saved));
    read_data(ctx, token, addr, saved, n);
    active = true;
    patched = addr;
    width = n;
    *key = 1;
    return write_data(ctx, token, addr, in, n);
}
static int release(void *ctx, uint64_t token, int32_t key)
{
    assert(active && key == 1);
    active = false;
    return write_data(ctx, token, patched, saved, width);
}
static int module(void *ctx, uint64_t token, unsigned index, unsigned segment, uint32_t *base,
                  uint32_t *size)
{
    (void)ctx;
    assert(token == 7 && index == 3 && !segment);
    *base = controller.info.text.base;
    *size = controller.info.text.size;
    return 0;
}
static void codes(void)
{
    for (unsigned variant = 0; variant < 2; ++variant) {
        assert(!vco_fixture_load(&controller));
        char text[1024], names[512];
        int n = vco_fixture_codes(&controller, text, sizeof(text));
        assert(n > 0);
        VcoCheatEntry entries[3];
        VcoCode lines[4];
        VcoCheatOp operations[4];
        VcoPatch patches[2];
        VcoCheatFile file = {entries, 3, 0, lines, 4, 0, names, sizeof(names), 0};
        VcoDiagnostic diagnostic;
        assert(!vco_cheat_parse(text, (size_t)n, &file, &diagnostic) && file.entry_count == 3);
        VcoFileMetadata metadata;
        vco_metadata_parse(text, (size_t)n, &metadata);
        assert(vco_metadata_compatibility(&metadata, "VCOCH0001", VCO_HARNESS_VERSION) ==
               VCO_COMPAT_MATCH);
        for (unsigned entry = 0; entry < 3; ++entry) {
            assert(!entries[entry].file_enabled);
            memset(bytes, 0, sizeof(bytes));
            uint32_t original_data = 5 + variant;
            uint16_t original_instruction = (uint16_t)(0x2000 + (variant ? 11 : 7));
            memcpy(bytes + 0x2000, &original_data, 4);
            memcpy(bytes + 0x1010, &original_instruction, 2);
            VcoCheatProgram program = {operations, 4, 0, NULL, 0};
            VcoCheatRuntime runtime;
            VcoCheatMemory memory = {NULL,   alive, read_data, write_data,
                                     module, NULL,  inject,    release};
            assert(!vco_cheat_compile(&file, entry, &program, &diagnostic));
            assert(!vco_cheat_runtime_init(&runtime, &program, memory, 7, patches, 2, 256, 0));
            assert(!vco_cheat_enable(&runtime));
            if (!entry)
                assert(bytes[0x2000] == 88);
            else
                assert(bytes[0x1010] == 9 && bytes[0x1011] == 0x20);
            assert(!vco_cheat_disable(&runtime) && !active);
            assert(!memcmp(bytes + 0x2000, &original_data, 4));
            assert(!memcmp(bytes + 0x1010, &original_instruction, 2));
        }
        assert(!vco_fixture_unload(&controller));
        assert(vco_fixture_codes(&controller, text, sizeof(text)) < 0 && !text[0]);
    }
}
static void bounds(void)
{
    char text[1024], full[1024];
    assert(vco_fixture_load(NULL) < 0 && vco_fixture_unload(NULL) < 0 &&
           vco_fixture_step(NULL) < 0);
    assert(vco_fixture_codes(&controller, text, sizeof(text)) < 0 && !text[0]);
    controller.generation = UINT32_MAX;
    assert(vco_fixture_load(&controller) < 0 && !calls[0]);
    reset();
    assert(!vco_fixture_load(&controller));
    int n = vco_fixture_codes(&controller, full, sizeof(full));
    assert(n > 0);
    for (size_t capacity = 1; capacity <= (size_t)n; ++capacity) {
        memset(text, 0xa5, sizeof(text));
        assert(vco_fixture_codes(&controller, text, capacity) < 0 && !text[0]);
        assert((unsigned char)text[capacity] == 0xa5);
    }
    assert(vco_fixture_codes(&controller, text, (size_t)n + 1) == n && !strcmp(text, full));
    assert(vco_fixture_codes(NULL, text, sizeof(text)) < 0 && !text[0]);
    assert(vco_fixture_codes(&controller, NULL, 100) < 0);
    text[0] = 'q';
    assert(vco_fixture_codes(&controller, text, 0) < 0 && text[0] == 'q');
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    reset();
    if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "contract"))
        contract();
    else if (!strcmp(argv[1], "ranges"))
        ranges();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else if (!strcmp(argv[1], "cycles"))
        cycles();
    else if (!strcmp(argv[1], "codes"))
        codes();
    else if (!strcmp(argv[1], "bounds"))
        bounds();
    else
        assert(0);
    puts("dynamic harness fixture passed");
    return 0;
}
