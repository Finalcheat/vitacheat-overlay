#include "cheats.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BASE 0x81000000u
#define N 1024u
static VcoCheatEntry entries[16];
static VcoCode codes[N];
static char names[1024];
static VcoCheatOp ops[N];
static VcoPatch patches[N];
static VcoCheatFile file;
static VcoCheatProgram program;
static VcoCheatRuntime runtime;
static VcoDiagnostic diag;
/* A byte-addressed fake process: exercises endian conversion independently of
 * the host CPU. Injectable faults cover partial writes and consumed handles. */
static struct {
    uint8_t bytes[4096], saved[N][4];
    uint32_t addresses[N], sizes[N], device, buttons;
    unsigned reads, writes, injected, released, pad_calls, live[N];
    int fail_write, fail_inject, fail_release, pad_result, access_result;
    uint64_t session;
} mem;
static size_t offset(uint32_t address, size_t n)
{
    if (address < BASE || (uint64_t)address + n > (uint64_t)BASE + sizeof(mem.bytes))
        return sizeof(mem.bytes);
    return address - BASE;
}
static int alive(void *ctx, uint64_t session)
{
    (void)ctx;
    return session == mem.session;
}
static int read_mem(void *ctx, uint64_t session, uint32_t a, uint8_t *v, size_t n)
{
    size_t o = offset(a, n);
    (void)ctx;
    ++mem.reads;
    if (mem.access_result)
        return mem.access_result;
    if (!alive(ctx, session) || o == sizeof(mem.bytes))
        return -1;
    memcpy(v, mem.bytes + o, n);
    return 0;
}
static int write_mem(void *ctx, uint64_t session, uint32_t a, const uint8_t *v, size_t n)
{
    size_t o = offset(a, n);
    ++mem.writes;
    if (mem.access_result)
        return mem.access_result;
    if (!alive(ctx, session) || o == sizeof(mem.bytes) ||
        (mem.fail_write > 0 && mem.writes == (unsigned)mem.fail_write))
        return -1;
    memcpy(mem.bytes + o, v, n);
    return 0;
}
static int module(void *ctx, uint64_t session, unsigned index, unsigned seg, uint32_t *base,
                  uint32_t *size)
{
    if (mem.access_result)
        return mem.access_result;
    if (!alive(ctx, session) || index != 0 || seg > 1)
        return -1;
    *base = BASE + seg * 2048;
    *size = 2048;
    return 0;
}
static int pad(void *ctx, uint64_t session, uint32_t *device, uint32_t *buttons)
{
    ++mem.pad_calls;
    if (mem.access_result)
        return mem.access_result;
    if (!alive(ctx, session))
        return -1;
    if (mem.pad_result)
        return mem.pad_result; /* Outputs deliberately untouched on suppression. */
    *device = mem.device;
    *buttons = mem.buttons;
    return 0;
}
static int inject(void *ctx, uint64_t session, uint32_t a, const uint8_t *v, size_t n,
                  int32_t *handle)
{
    if (mem.access_result)
        return mem.access_result;
    unsigned i = mem.injected++;
    size_t o = offset(a, n);
    if (!alive(ctx, session) || i >= N || o == sizeof(mem.bytes) ||
        (mem.fail_inject > 0 && i + 1 == (unsigned)mem.fail_inject))
        return -1;
    for (unsigned j = 0; j < i; ++j)
        if (mem.live[j] && (uint64_t)a < (uint64_t)mem.addresses[j] + mem.sizes[j] &&
            (uint64_t)mem.addresses[j] < (uint64_t)a + n)
            return -1;
    memcpy(mem.saved[i], mem.bytes + o, n);
    mem.addresses[i] = a;
    mem.sizes[i] = (uint32_t)n;
    mem.live[i] = 1;
    memcpy(mem.bytes + o, v, n);
    *handle = (int32_t)i;
    return 0; /* zero is valid */
}
static int release(void *ctx, uint64_t session, int32_t h)
{
    ++mem.released;
    assert(h >= 0 && h < (int32_t)N && mem.live[h]);
    mem.live[h] = 0;
    if (!alive(ctx, session) || mem.fail_release)
        return -1;
    memcpy(mem.bytes + offset(mem.addresses[h], mem.sizes[h]), mem.saved[h], mem.sizes[h]);
    return 0;
}
static VcoCheatMemory backend(void)
{
    VcoCheatMemory m = {NULL, alive, read_mem, write_mem, module, pad, inject, release};
    return m;
}
static VcoError parse_n(const char *s, size_t n)
{
    file = (VcoCheatFile){entries, 16, 0, codes, N, 0, names, sizeof(names), 0};
    return vco_cheat_parse(s, n, &file, &diag);
}
static VcoError parse(const char *s)
{
    return parse_n(s, strlen(s));
}
static VcoError compile(size_t e)
{
    program = (VcoCheatProgram){ops, N, 0, NULL, 0};
    return vco_cheat_compile(&file, e, &program, &diag);
}
static void setup(const char *s)
{
    memset(&mem, 0, sizeof(mem));
    mem.session = 7;
    mem.device = 1;
    assert(parse(s) == VCO_OK);
    assert(compile(0) == VCO_OK);
    assert(program.needs_review == 0);
    assert(vco_cheat_runtime_init(&runtime, &program, backend(), 7, patches, N, 10000, 0) ==
           VCO_OK);
    assert(runtime.state == VCO_CHEAT_OFF);
}
static void access_session_tests(void)
{
    const char *texts[] = {
        "_V0 Read\n$D001 81000000 00000000\n", "_V0 Write\n$0200 81000000 11223344\n",
        "_V0 Module\n$B200 00000000 00000000\n", "_V0 Pad\n$C201 00000000 00000000\n",
        "_V0 Patch\n$A200 81000000 11223344\n"};
    const VcoError faults[] = {VCO_E_IO, VCO_E_IO, VCO_E_ADDRESS, VCO_E_IO, VCO_E_PATCH};
    for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        for (unsigned expired = 0; expired < 2; ++expired) {
            setup(texts[i]);
            mem.access_result = expired ? VCO_ACCESS_SESSION : -1;
            VcoError expected = expired ? VCO_E_SESSION : faults[i];
            assert(vco_cheat_enable(&runtime) == expected);
            assert(runtime.diagnostic.code == expected && runtime.diagnostic.line == 2 &&
                   runtime.state == VCO_CHEAT_ERROR);
            assert(!mem.injected && !mem.released && !mem.bytes[0]);
            assert(!vco_cheat_disable(&runtime));
        }
    }
}
static void put32(size_t o, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        mem.bytes[o + i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(size_t o)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= (uint32_t)mem.bytes[o + i] << (8 * i);
    return v;
}
static void parser_tests(void)
{
    assert(parse("\xef\xbb\xbf# game\r\n_V1 无限生命\r\n$0200 81000000 00000063\r\n_V0 second\n") ==
           VCO_OK);
    assert(file.entry_count == 2 && file.code_count == 1 && entries[0].file_enabled);
    assert(strcmp(names + entries[0].name, "无限生命") == 0);
    assert(parse("$0200 81000000 00000001\n") == VCO_E_SYNTAX);
    assert(file.entry_count == 0 && file.code_count == 0);
    assert(parse("_V0 bad\n$0200 8100000Z 00000001\n_V0 good\n$0000 81000001 00000009") == VCO_OK);
    assert(compile(0) == VCO_E_SYNTAX && compile(1) == VCO_OK);
    assert(parse("_V2 not valid\n") == VCO_E_SYNTAX);
    assert(parse("_V0 \xff") == VCO_E_ENCODING);
    assert(parse_n("_V0 a\0b", 8) == VCO_E_ENCODING);
    assert(parse("_V0 a\n$0200 81000000 00000001 trailing") == VCO_OK);
    assert(compile(0) == VCO_E_SYNTAX);
    assert(parse("_V0 a\n$0200 81000000 00000001") == VCO_OK);
    file.code_capacity = 0;
    assert(vco_cheat_parse("_V0 a\n$0200 81000000 00000001",
                           strlen("_V0 a\n$0200 81000000 00000001"), &file, &diag) == VCO_E_LIMIT);
}
static void write_tests(void)
{
    setup("_V1 widths\n$0000 81000000 000000AB\n$0100 81000002 00001234\n$0200 81000004 89ABCDEF");
    assert(mem.writes == 0);
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.writes == 0);
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    assert(mem.bytes[0] == 0xAB && mem.bytes[2] == 0x34 && mem.bytes[3] == 0x12);
    assert(get32(4) == 0x89ABCDEFu);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 3);
    mem.bytes[0] = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.bytes[0] == 0xAB);
    assert(vco_cheat_disable(&runtime) == VCO_OK);
    mem.bytes[0] = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.bytes[0] == 0);
    setup("_V0 copy\n$5200 81000008 81000004");
    put32(4, 0x87654321);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(8) == 0x87654321);
    setup("_V0 compression\n$4101 81000000 000000FF\n$0004 00000006 00000001");
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    for (unsigned i = 0; i < 4; ++i)
        assert(((unsigned)mem.bytes[i * 6] | (unsigned)mem.bytes[i * 6 + 1] << 8) == 0xFF + i);
}
static void pointer_tests(void)
{
    setup("_V0 pointer\n$3203 81000000 00000010\n$3200 00000000 00000004\n$3200 00000000 "
          "00000008\n$3300 00000000 0001869F");
    put32(0, BASE + 0x100);
    put32(0x110, BASE + 0x200);
    put32(0x204, BASE + 0x300);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x308) == 99999);
    put32(0x204, BASE + 0x400);
    assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x408) == 99999);
    setup("_V0 pointer copy\n$8101 81000000 00000002\n$8800 00000000 00000000\n$8501 81000004 "
          "00000004\n$8900 00000000 00000000");
    put32(0, BASE + 0x100);
    put32(4, BASE + 0x200);
    mem.bytes[0x204] = 0xCD;
    mem.bytes[0x205] = 0xAB;
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[0x102] == 0xCD &&
           mem.bytes[0x103] == 0xAB);
    /* Compress intermediate level: each iteration follows a DIFFERENT child. */
    setup("_V0 nested repeat\n$7202 81000000 00000010\n$7200 00000000 00000008\n$7701 00000000 "
          "00000060\n$0003 00000004 00000001");
    put32(0, BASE + 0x100);
    for (unsigned i = 0; i < 3; ++i)
        put32(0x110 + i * 4, BASE + 0x200 + i * 0x100);
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    for (unsigned i = 0; i < 3; ++i)
        assert(get32(0x208 + i * 0x100) == 0x60 + i);
    setup("_V0 root repeat\n$7101 81000000 00000008\n$7700 00000000 00000063\n$0002 00000004 "
          "00000000");
    put32(0, BASE + 0x100);
    put32(4, BASE + 0x200);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[0x108] == 99 &&
           mem.bytes[0x208] == 99);
}
static void dormant_pointer_tests(void)
{
    /* A game may publish or clear these pointers when entering/leaving a
     * match. Z06 skips an unavailable chain and retries it on the next tick. */
    static const char chain[] = "_V0 dormant\n$3204 81000000 00000000\n$3200 00000000 00000100\n"
                                "$3200 00000000 00000004\n$3200 00000000 0000000C\n"
                                "$3300 00000000 00000003\n$0200 81000020 00000077\n";
    const size_t links[] = {0, 0x100, 0x300, 0x404};
    const uint32_t pointees[] = {BASE + 0x100, BASE + 0x200, BASE + 0x400, BASE + 0x500};
    for (unsigned level = 0; level < 4; ++level) {
        setup(chain);
        for (unsigned i = 0; i < 4; ++i)
            put32(links[i], pointees[i]);
        put32(links[level], 0);
        assert(vco_cheat_enable(&runtime) == VCO_OK);
        assert(runtime.state == VCO_CHEAT_ON && get32(0x50C) == 0 && get32(0x20) == 0x77);
        assert(mem.reads == level + 1 && mem.writes == 1);
        put32(links[level], pointees[level]);
        assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x50C) == 3);
        put32(links[level], 0);
        put32(0x50C, 9);
        assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x50C) == 9);
        put32(links[level], pointees[level]);
        assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x50C) == 3);
        assert(vco_cheat_disable(&runtime) == VCO_OK);
        put32(0x50C, 9);
        assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x50C) == 9);
    }
    setup("_V0 sparse repeat\n$7201 81000000 00000000\n$7700 00000000 00000011\n"
          "$0003 00000004 00000001\n$0200 81000020 00000077");
    put32(4, BASE + 0x100);
    put32(8, BASE + 0x200);
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    assert(get32(0x100) == 0x12 && get32(0x200) == 0x13 && get32(0x20) == 0x77);
    for (unsigned source = 0; source < 2; ++source) {
        setup("_V0 dormant copy\n$8201 81000000 00000000\n$8800 00000000 00000000\n"
              "$8501 81000004 00000000\n$8900 00000000 00000000\n"
              "$0200 81000020 00000077");
        put32(0, source ? BASE + 0x100 : 0);
        put32(4, source ? 0 : BASE + 0x200);
        put32(0x100, 9);
        put32(0x200, 0x12345678);
        assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x100) == 9);
        assert(get32(0x20) == 0x77 && mem.writes == 1);
        put32(0, BASE + 0x100);
        put32(4, BASE + 0x200);
        assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x100) == 0x12345678);
    }
    /* A plausible pointer into an unmapped region is still a real read error. */
    setup(chain);
    put32(0, BASE + 0x10000);
    assert(vco_cheat_enable(&runtime) == VCO_E_IO && runtime.state == VCO_CHEAT_ERROR);
    assert(!mem.writes);
}
static void copy_header_tests(void)
{
    for (unsigned relative = 0; relative < 2; ++relative)
        for (unsigned width = 0; width < 3; ++width)
            for (unsigned source = 4; source < 7; ++source) {
                char text[512];
                uint32_t root = relative ? 0 : BASE;
                snprintf(text, sizeof(text),
                         "_V0 mixed copy headers\n%s$8%X02 %08X 00000008\n"
                         "$8%X00 00000000 00000010\n$8800 00000000 00000000\n"
                         "$8%X02 %08X 00000008\n$8%X00 00000000 00000010\n"
                         "$8900 00000000 00000000\n",
                         relative ? "$B200 00000000 00000000\n" : "", width, root, width, source,
                         root + 4, source);
                setup(text);
                put32(0, BASE + 0x100);
                put32(4, BASE + 0x200);
                put32(0x108, BASE + 0x300);
                put32(0x208, BASE + 0x400);
                put32(0x310, 0xABCDEF01);
                put32(0x410, 0x12345678);
                assert(vco_cheat_enable(&runtime) == VCO_OK);
                uint32_t mask = width == 2 ? UINT32_MAX : (1u << (8u << width)) - 1;
                assert(get32(0x310) == ((0xABCDEF01 & ~mask) | (0x12345678 & mask)));
            }
    for (unsigned source = 3; source <= 7; source += 4) {
        char text[256];
        snprintf(text, sizeof(text),
                 "_V0 invalid source\n$8201 81000000 00000000\n$8800 00000000 00000000\n"
                 "$8%X01 81000004 00000000\n$8900 00000000 00000000",
                 source);
        assert(parse(text) == VCO_OK && compile(0) == VCO_E_FORMAT);
    }
}
static void patch_tests(void)
{
    setup("_V0 patch\n$B200 00000000 00000000\n$A100 00000004 0000BF00\n$A200 00000008 12345678");
    memset(mem.bytes, 0xA5, 32);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.injected == 2);
    assert(mem.bytes[4] == 0 && mem.bytes[5] == 0xBF && get32(8) == 0x12345678);
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.injected == 2);
    assert(vco_cheat_disable(&runtime) == VCO_OK && mem.released == 2 && get32(8) == 0xA5A5A5A5);
    assert(vco_cheat_disable(&runtime) == VCO_OK && mem.released == 2);
    mem.fail_inject = 4;
    assert(vco_cheat_enable(&runtime) == VCO_E_PATCH && mem.released == 3);
    assert(get32(4) == 0xA5A5A5A5);
    mem.fail_inject = 0;
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    mem.fail_release = 1;
    assert(vco_cheat_disable(&runtime) == VCO_E_RESTORE);
    unsigned releases = mem.released;
    assert(vco_cheat_disable(&runtime) == VCO_E_RESTORE && mem.released == releases);
    assert(vco_cheat_enable(&runtime) == VCO_E_RESTORE);
}
static void control_tests(void)
{
    char text[256];
    for (unsigned kind = 0; kind < 12; ++kind) {
        snprintf(text, sizeof(text),
                 "_V0 condition\n$D%X01 81000000 00000005\n$0000 81000008 00000063\n$0000 81000009 "
                 "00000007",
                 kind);
        setup(text);
        put32(0, 5);
        assert(vco_cheat_enable(&runtime) == VCO_OK);
        assert(mem.bytes[8] == (kind < 3 ? 99 : 0) && mem.bytes[9] == 7);
    }
    setup("_V0 pad\n$C201 00000001 00000300\n$0000 81000000 00000063");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 0);
    mem.buttons = 0x300;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.bytes[0] == 99);
    /* Suppression is a false condition even for a mask that matches neutral
     * input. Skip the complete multi-line instruction, then continue outside
     * the guarded block; it is not a fatal I/O error or a pause of all cheats. */
    setup("_V0 suppressed\n$C202 00000000 00000000\n$4201 81000000 11223344\n"
          "$0002 00000004 00000000\n$0000 81000008 00000055");
    mem.pad_result = VCO_PAD_SUPPRESSED;
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 1 && !get32(0) && !get32(4) &&
           mem.bytes[8] == 0x55);
    mem.pad_result = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0) == 0x11223344 && get32(4) == 0x11223344);
    mem.pad_result = 2; /* Undocumented positive statuses are errors as well. */
    unsigned writes = mem.writes;
    assert(vco_cheat_tick(&runtime) == VCO_E_IO && mem.writes == writes);
    setup("_V0 gate\n$D001 81000000 00000000\n$0000 81000008 00000063");
    program.needs_review = VCO_REVIEW_CONDITIONS; /* Future, unverified extension. */
    assert(vco_cheat_enable(&runtime) == VCO_E_UNVERIFIED && mem.writes == 0);
}
static void failure_tests(void)
{
    const char *bad[] = {
        "_V0 bad\n$4101 81000000 00000001",
        "_V0 bad\n$3202 81000000 00000000\n$3300 00000000 00000001",
        "_V0 bad\n$B200 00000002 00000000",
        "_V0 bad\n$D001 81000000 00000000\n$4101 81000000 00000001\n$0002 00000004 00000000",
        "_V0 bad\n$2000 81000000 00000001",
        "_V0 bad\n$0101 81000000 00010000",
        "_V0 bad\n$7201 81000000 00000000\n$7702 00000000 00000001\n$0002 00000004 00000000"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(parse(bad[i]) == VCO_OK);
        assert(compile(0) == VCO_E_FORMAT);
        assert(program.count == 0);
    }
    setup("_V0 invalid\n$0200 FFFFFFFF 00000001");
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && mem.writes == 0);
    setup("_V0 budget\n$4201 81000000 00000001\n$FFFF 00000004 00000000");
    runtime.budget = 2;
    assert(vco_cheat_enable(&runtime) == VCO_E_BUDGET && mem.writes <= 2 &&
           runtime.state == VCO_CHEAT_ERROR);
    setup("_V0 stale\n$0000 81000000 00000001");
    mem.session = 8;
    assert(vco_cheat_enable(&runtime) == VCO_E_SESSION && mem.writes == 0);
    setup("_V0 partial\n$0000 81000000 00000001\n$0000 81000001 00000002");
    mem.fail_write = 2;
    assert(vco_cheat_enable(&runtime) == VCO_E_IO && mem.bytes[0] == 1 && mem.bytes[1] == 0);
    assert(runtime.state == VCO_CHEAT_ERROR);
}
static void width_matrix_tests(void)
{
    char text[512];
    for (unsigned x = 0; x < 3; ++x) {
        unsigned width = 1u << x;
        uint32_t mask = x == 2 ? UINT32_MAX : (1u << (width * 8)) - 1;
        uint32_t value = 0x89ABCDEFu & mask;
        snprintf(text, sizeof(text),
                 "_V0 widths\n$0%X00 81000010 %08X\n$5%X00 81000020 81000010\n$4%X01 81000030 "
                 "%08X\n$0003 00000008 00000001",
                 x, value, x, x, value);
        setup(text);
        assert(vco_cheat_enable(&runtime) == VCO_OK);
        for (unsigned byte = 0; byte < width; ++byte) {
            assert(mem.bytes[0x10 + byte] == (uint8_t)(value >> (8 * byte)));
            assert(mem.bytes[0x20 + byte] == (uint8_t)(value >> (8 * byte)));
            for (unsigned i = 0; i < 3; ++i)
                assert(mem.bytes[0x30 + i * 8 + byte] == (uint8_t)((value + i) >> (8 * byte)));
        }
        snprintf(text, sizeof(text),
                 "_V0 pointer widths\n$3%X01 81000000 00000008\n$3300 00000000 %08X", x, value);
        setup(text);
        put32(0, BASE + 0x100);
        assert(vco_cheat_enable(&runtime) == VCO_OK);
        for (unsigned byte = 0; byte < width; ++byte)
            assert(mem.bytes[0x108 + byte] == (uint8_t)(value >> (8 * byte)));
        snprintf(text, sizeof(text),
                 "_V0 pointer copy widths\n$8%X01 81000000 00000008\n$8800 00000000 "
                 "00000000\n$8%X01 81000004 00000008\n$8900 00000000 00000000",
                 x, x + 4);
        setup(text);
        put32(0, BASE + 0x100);
        put32(4, BASE + 0x200);
        put32(0x208, value);
        assert(vco_cheat_enable(&runtime) == VCO_OK);
        for (unsigned byte = 0; byte < width; ++byte)
            assert(mem.bytes[0x108 + byte] == (uint8_t)(value >> (8 * byte)));
    }
    /* Last-level compression is contiguous; unlike the intermediate-level
     * case it must not dereference a different child for each element. */
    setup("_V0 last level\n$7202 81000000 00000010\n$7200 00000000 00000008\n$7702 00000000 "
          "00000060\n$0003 00000004 00000001");
    put32(0, BASE + 0x100);
    put32(0x110, BASE + 0x200);
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    assert(get32(0x208) == 0x60 && get32(0x20C) == 0x61 && get32(0x210) == 0x62);
    /* Z06 example structure, relocated into the fake module's segment 1. */
    setup("_V0 all items\n$B200 00000001 00000000\n$4101 00000000 00002000\n$006F 00000002 "
          "00000001\n$4001 00000100 00000063\n$006F 00000001 00000000");
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    for (unsigned i = 0; i < 0x6F; ++i) {
        assert(((unsigned)mem.bytes[2048 + i * 2] | (unsigned)mem.bytes[2049 + i * 2] << 8) ==
               0x2000 + i);
        assert(mem.bytes[2304 + i] == 99);
    }
    /* Comparison table tests both outcomes, not just the equal case. */
    for (unsigned kind = 0; kind < 12; ++kind)
        for (unsigned value = 4; value <= 6; ++value) {
            snprintf(text, sizeof(text),
                     "_V0 comparison\n$D%X01 81000000 00000005\n$0000 81000008 00000063", kind);
            setup(text);
            put32(0, value);
            assert(vco_cheat_enable(&runtime) == VCO_OK);
            unsigned relation = kind / 3;
            int expected = relation == 0   ? value == 5
                           : relation == 1 ? value != 5
                           : relation == 2 ? value > 5
                                           : value < 5;
            assert(mem.bytes[8] == (expected ? 99 : 0));
        }
}
static void unaligned_patch_tests(void)
{
    for (unsigned relative = 0; relative < 2; ++relative) {
        for (unsigned selector = 0; selector < 3; ++selector) {
            unsigned width = 1u << selector;
            for (unsigned residue = 0; residue < 4; ++residue) {
                char text[256];
                unsigned target = 16 + residue;
                snprintf(text, sizeof(text), "_V0 unaligned patch\n%s$A%X00 %08X 12345678\n",
                         relative ? "$B200 00000000 00000000\n" : "", selector,
                         relative ? target : BASE + target);
                setup(text);
                memset(mem.bytes, 0x5a, 32);
                assert(vco_cheat_enable(&runtime) == VCO_OK && mem.injected == 1);
                for (unsigned byte = 0; byte < 32; ++byte) {
                    uint8_t expected =
                        byte >= target && byte < target + width
                            ? (uint8_t)(UINT32_C(0x12345678) >> (8 * (byte - target)))
                            : 0x5a;
                    assert(mem.bytes[byte] == expected);
                }
                assert(vco_cheat_tick(&runtime) == VCO_OK && mem.injected == 1);
                assert(vco_cheat_disable(&runtime) == VCO_OK && mem.released == 1);
                for (unsigned byte = 0; byte < 32; ++byte)
                    assert(mem.bytes[byte] == 0x5a);
            }
        }
    }
    setup("_V0 address wrap\n$A200 FFFFFFFE 12345678");
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && !mem.injected);
    setup("_V0 relative overrun\n$B200 00000000 00000000\n$A200 000007FE 12345678");
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && !mem.injected);
}
static void boundary_tests(void)
{
    setup("_V0 segment\n$B200 00000001 00000000\n$0200 00000000 12345678");
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(2048) == 0x12345678 && get32(0) == 0);
    setup("_V0 segment overflow\n$B200 00000001 00000000\n$0200 000007FF 12345678");
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && mem.writes == 0);
    setup("_V0 missing module\n$B201 00000000 00000000\n$0000 00000000 00000001");
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && mem.writes == 0);
    setup("_V0 unavailable pointer\n$3201 81000000 00000010\n$3300 00000000 00000001");
    put32(0, UINT32_MAX - 8);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 0);
    setup("_V0 pointer arithmetic overflow\n$3201 81000000 7FFFFFFF\n$3300 00000000 00000001");
    put32(0, BASE);
    assert(vco_cheat_enable(&runtime) == VCO_E_ADDRESS && mem.writes == 0);
    setup("_V0 null pointer\n$3201 81000000 00000000\n$3300 00000000 00000001");
    assert(vco_cheat_enable(&runtime) == VCO_OK && !mem.writes);
    setup("_V0 overlap\n$A200 81000000 12345678\n$A100 81000002 0000BF00");
    memset(mem.bytes, 0xAB, 4);
    assert(vco_cheat_enable(&runtime) == VCO_E_PATCH && get32(0) == 0xABABABAB);
    setup("_V0 unaligned\n$A100 81000001 0000BF00");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.injected == 1 &&
           get32(0) == UINT32_C(0x00BF0000));
    assert(vco_cheat_disable(&runtime) == VCO_OK && get32(0) == 0);
    setup("_V0 departed\n$A100 81000000 0000BF00");
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    mem.session = 8;
    assert(vco_cheat_tick(&runtime) == VCO_E_RESTORE && !mem.live[0]);
    assert(mem.bytes[1] == 0xBF); /* adapter MUST NOT restore into the new process */
    setup("_V0 nesting\n$D003 81000000 00000001\n$D001 81000001 00000001\n$0000 81000008 "
          "00000063\n$0000 81000009 00000064\n$0000 8100000A 00000065");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[8] == 0 && mem.bytes[9] == 0 &&
           mem.bytes[10] == 101);
    mem.bytes[0] = 1;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.bytes[8] == 0 && mem.bytes[9] == 100);
    mem.bytes[1] = 1;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.bytes[8] == 99);
    assert(parse("_V0 conditional patch\n$D001 81000000 00000001\n$A100 81000002 0000BF00") ==
           VCO_OK);
    assert(compile(0) == VCO_OK);
    assert(parse("_V0 crossing blocks\n$D002 81000000 00000001\n$D002 81000001 00000001\n$0000 "
                 "81000008 00000001\n$0000 81000009 00000001") == VCO_OK);
    assert(compile(0) == VCO_OK);
    setup("_V0 A000\n$A000 81000000 000000FF");
    runtime.review_opt_in = 0;
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.injected == 1 && mem.bytes[0] == 0xFF);
}
/* These expectations are grounded in the original Z06 executor, including its
 * differences from ambiguous Z04 prose. See fixtures/z06 and the compatibility
 * document; the original binary is never linked into production or host tests. */
static void compatibility_tests(void)
{
    char text[512];
    const unsigned devices[] = {0, 1, 2, 4, 8, 3, 0xFFFF};
    const unsigned buttons[] = {0, 0x300, 0x301, 0x1300, UINT32_MAX};
    for (size_t d = 0; d < sizeof(devices) / sizeof(*devices); ++d)
        for (size_t b = 0; b < sizeof(buttons) / sizeof(*buttons); ++b) {
            snprintf(text, sizeof(text),
                     "_V0 exact pad\n$C201 %08X 00000300\n$0000 81000000 000000AB\n"
                     "$0000 81000001 000000CD",
                     devices[d]);
            setup(text);
            mem.device = 8; /* The original ignores the code's device field. */
            mem.buttons = buttons[b];
            assert(vco_cheat_enable(&runtime) == VCO_OK);
            assert(mem.bytes[0] == (buttons[b] == 0x300 ? 0xAB : 0) && mem.bytes[1] == 0xCD);
        }
    setup("_V0 sample once\n$C201 00000001 00000000\n$0000 81000000 00000001\n"
          "$C201 00000002 00000000\n$0000 81000001 00000002");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.pad_calls == 1 && mem.bytes[1] == 2);
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.pad_calls == 2);
    setup("_V0 empty guards\n$C200 00000000 00000300\n$D000 81000000 00000001\n"
          "$0000 81000008 00000077");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[8] == 0x77);
    setup("_V0 long C\n$C2FF 00000000 00000300\n$0000 81000008 00000077");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 0);
    setup("_V0 long D\n$D0FF 81000000 00000001\n$0000 81000008 00000077");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 0);
    /* Branch ranges are physical forward skips, not lexical nested scopes. */
    for (unsigned outer = 0; outer < 2; ++outer)
        for (unsigned inner = 0; inner < 2; ++inner) {
            setup("_V0 crossing\n$D002 81000000 00000001\n$D002 81000001 00000001\n"
                  "$0000 81000008 00000011\n$0000 81000009 00000022\n"
                  "$0000 8100000A 00000033");
            mem.bytes[0] = (uint8_t)outer;
            mem.bytes[1] = (uint8_t)inner;
            assert(vco_cheat_enable(&runtime) == VCO_OK);
            assert(mem.bytes[8] == (outer && inner ? 0x11 : 0));
            assert(mem.bytes[9] == (!outer || inner ? 0x22 : 0));
            assert(mem.bytes[10] == 0x33);
        }
    /* All D widths zero-extend loads and truncate narrow constants. Test the
     * sign bit so signed comparisons cannot accidentally pass this matrix. */
    for (unsigned kind = 0; kind < 12; ++kind) {
        unsigned width = 1u << (kind % 3);
        uint32_t high = UINT32_C(1) << (width * 8 - 1);
        uint32_t mask = width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1;
        const uint32_t values[] = {0, high - 1, high, mask};
        for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i) {
            snprintf(text, sizeof(text),
                     "_V0 unsigned\n$D%X01 81000000 %08X\n$0000 81000008 000000AB", kind,
                     high | ~mask);
            setup(text);
            put32(0, values[i]);
            unsigned comparison = kind / 3;
            int match = comparison == 0   ? values[i] == high
                        : comparison == 1 ? values[i] != high
                        : comparison == 2 ? values[i] > high
                                          : values[i] < high;
            assert(vco_cheat_enable(&runtime) == VCO_OK);
            assert(mem.bytes[8] == (match ? 0xAB : 0));
        }
    }
    setup("_V0 zero repeat\n$4201 81000000 00000001\n$0000 00000004 00000001\n"
          "$7201 81000004 00000000\n$7700 00000000 00000001\n"
          "$0000 00000004 00000001\n$0000 81000008 000000AB");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.reads == 0 && mem.writes == 1 &&
           mem.bytes[8] == 0xAB);
    /* The original parser accepts the complete value word. 0/3/4/7/A stores
     * truncate at the final width, including B-relative and stepped values. */
    const unsigned families[] = {0, 3, 4, 7, 10};
    const uint32_t values[] = {0x12345678, UINT32_MAX};
    for (unsigned relative = 0; relative < 2; ++relative)
        for (unsigned x = 0; x < 2; ++x)
            for (unsigned v = 0; v < sizeof(values) / sizeof(*values); ++v)
                for (unsigned f = 0; f < sizeof(families) / sizeof(*families); ++f) {
                    unsigned family = families[f];
                    const char *prefix = relative ? "$B200 00000000 00000000\n" : "";
                    uint32_t root = relative ? 0 : BASE;
                    unsigned n = 1, destination = family == 3 || family == 7 ? 0x110 : 0x10;
                    if (family == 3 || family == 7) {
                        snprintf(text, sizeof(text),
                                 "_V0 narrow\n%s$%X%X01 %08X 00000010\n$%04X 00000000 %08X\n%s",
                                 prefix, family, x, root, family == 3 ? 0x3300 : 0x7701, values[v],
                                 family == 7 ? "$0003 00000004 FFFFFFFF\n" : "");
                    } else {
                        snprintf(text, sizeof(text), "_V0 narrow\n%s$%X%X%02X %08X %08X\n%s",
                                 prefix, family, x, family == 4 ? 1 : 0, root + 0x10, values[v],
                                 family == 4 ? "$0003 00000004 FFFFFFFF\n" : "");
                    }
                    if (family == 4 || family == 7)
                        n = 3;
                    setup(text);
                    put32(0, BASE + 0x100);
                    for (unsigned i = 0; i < n; ++i)
                        put32(destination + i * 4, 0xAABBCCDD);
                    uint32_t mask = x ? 0xFFFF : 0xFF;
                    assert(vco_cheat_enable(&runtime) == VCO_OK);
                    for (unsigned i = 0; i < n; ++i)
                        assert(get32(destination + i * 4) ==
                               ((0xAABBCCDD & ~mask) | ((values[v] - i) & mask)));
                    assert(vco_cheat_disable(&runtime) == VCO_OK);
                    if (family == 10)
                        assert(get32(destination) == 0xAABBCCDD && mem.released == 1);
                }
}
static void conditional_patch_tests(void)
{
    setup("_V0 held patch\n$C201 00000001 00000300\n$A100 81000010 0000BF00\n"
          "$0000 81000020 000000AB");
    put32(0x10, 0xAABBCCDD);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.injected == 0 && mem.bytes[0x20] == 0xAB);
    mem.buttons = 0x300;
    assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x10) == 0xAABBBF00 && mem.injected == 1);
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.injected == 1);
    mem.buttons = 0x301;
    assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x10) == 0xAABBCCDD && mem.released == 1);
    put32(0x10, 0x11223344);
    mem.buttons = 0x300;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.injected == 2);
    mem.pad_result = VCO_PAD_SUPPRESSED;
    assert(vco_cheat_tick(&runtime) == VCO_OK && get32(0x10) == 0x11223344 && mem.released == 2);
    assert(vco_cheat_disable(&runtime) == VCO_OK && mem.released == 2);
    setup("_V0 latched D\n$D001 81000000 00000001\n$A000 81000010 000000AB");
    mem.bytes[0] = 1;
    mem.bytes[0x10] = 0xCD;
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[0x10] == 0xAB);
    mem.bytes[0] = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.released == 0 && mem.bytes[0x10] == 0xAB);
    assert(vco_cheat_disable(&runtime) == VCO_OK && mem.bytes[0x10] == 0xCD);
    setup("_V0 nested C\n$C202 00000000 00000300\n$D001 81000000 00000001\n"
          "$A200 81000010 12345678");
    mem.buttons = 0x300;
    mem.bytes[0] = 1;
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.injected == 1);
    mem.bytes[0] = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.released == 0);
    mem.buttons = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.released == 1 && get32(0x10) == 0);
    /* D can skip C itself. A skipped condition is not evaluated, so it cannot
     * release a patch even when the controller changed in the meantime. */
    setup("_V0 nested D\n$D002 81000000 00000001\n$C201 00000000 00000300\n"
          "$A200 81000010 12345678");
    mem.buttons = 0x300;
    mem.bytes[0] = 1;
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    mem.buttons = 0;
    mem.bytes[0] = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.released == 0);
    assert(vco_cheat_disable(&runtime) == VCO_OK && mem.released == 1);
    setup("_V0 release failure\n$C201 00000000 00000300\n$A000 81000010 00000011\n"
          "$A000 81000011 00000022");
    mem.buttons = 0x300;
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.injected == 2);
    mem.buttons = 0;
    mem.fail_release = 1;
    assert(vco_cheat_tick(&runtime) == VCO_E_RESTORE && mem.released == 2 && !mem.live[0] &&
           !mem.live[1]);
    assert(vco_cheat_disable(&runtime) == VCO_E_RESTORE && mem.released == 2);
    setup("_V0 release budget\n$C201 00000000 00000300\n$A000 81000010 00000011");
    mem.buttons = 0x300;
    assert(vco_cheat_enable(&runtime) == VCO_OK);
    mem.buttons = 0;
    runtime.budget = 1;
    assert(vco_cheat_tick(&runtime) == VCO_E_BUDGET && mem.released == 1 && !mem.bytes[0x10]);
}
static void module_combination_tests(void)
{
    setup("_V0 B sources\n$B200 00000001 00000000\n$0200 00000010 11223344\n"
          "$5200 00000020 00000010\n$D202 00000020 11223344\n"
          "$3201 00000000 00000008\n$3300 00000000 00000077");
    put32(2048, BASE + 0x100); /* Root is relative; its pointee is absolute. */
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(2080) == 0x11223344 &&
           get32(0x108) == 0x77 && get32(0x908) == 0);
    setup("_V0 B copy pointers\n$B200 00000001 00000000\n"
          "$8202 00000000 00000010\n$8200 00000000 00000008\n$8800 00000000 00000000\n"
          "$8602 00000004 00000010\n$8600 00000000 00000008\n$8900 00000000 00000000");
    put32(2048, BASE + 0x100);
    put32(2052, BASE + 0x200);
    put32(0x110, BASE + 0x300);
    put32(0x210, BASE + 0x400);
    put32(0x408, 0x12345678);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x308) == 0x12345678);
    setup("_V0 B root repeat\n$B200 00000001 00000000\n$7201 00000000 00000008\n"
          "$7700 00000000 00000011\n$0002 00000004 00000001");
    put32(2048, BASE + 0x100);
    put32(2052, BASE + 0x200);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x108) == 0x11 && get32(0x208) == 0x12);
    setup("_V0 B replacement\n$B200 00000001 00000000\n$0000 00000010 00000011\n"
          "$B200 00000000 00000000\n$0000 00000010 00000022");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[2064] == 0x11 &&
           mem.bytes[16] == 0x22);
    setup("_V0 B tick reset\n$C201 00000000 00000001\n$B200 00000001 00000000\n"
          "$C201 00000000 00000001\n$0000 00000010 00000011\n"
          "$C201 00000000 00000000\n$0000 81000020 00000022");
    mem.buttons = 1;
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[2064] == 0x11 && !mem.bytes[32]);
    mem.buttons = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && mem.bytes[32] == 0x22);
    setup("_V0 B entry\n$B200 00000001 00000000\n$0000 00000010 00000011\n"
          "_V0 absolute entry\n$0000 81000020 00000022");
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[2064] == 0x11);
    assert(vco_cheat_disable(&runtime) == VCO_OK);
    assert(compile(1) == VCO_OK);
    assert(vco_cheat_runtime_init(&runtime, &program, backend(), 7, patches, N, 1000, 0) == VCO_OK);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.bytes[32] == 0x22);
    setup("_V0 B held patch\n$B200 00000001 00000000\n$C201 00000000 00000300\n"
          "$A100 00000010 0000BF00");
    mem.buttons = 0x300;
    put32(2064, 0xAABBCCDD);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(2064) == 0xAABBBF00);
    mem.buttons = 0;
    assert(vco_cheat_tick(&runtime) == VCO_OK && get32(2064) == 0xAABBCCDD);
}
static void offset_tests(void)
{
    setup("_V0 backward repeat\n$4201 81000020 00000003\n$0003 FFFFFFFC FFFFFFFF");
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x20) == 3 && get32(0x1C) == 2 &&
           get32(0x18) == 1);
    setup("_V0 backward chain\n$3203 81000000 FFFFFFFC\n$3200 00000000 00000008\n"
          "$3200 00000000 FFFFFFF8\n$3300 00000000 00000077");
    put32(0, BASE + 0x104);
    put32(0x100, BASE + 0x200);
    put32(0x208, BASE + 0x308);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x300) == 0x77);
    setup("_V0 backward roots\n$B200 00000001 00000000\n$7201 00000008 FFFFFFFC\n"
          "$7700 00000000 00000003\n$0003 FFFFFFFC FFFFFFFF");
    put32(2056, BASE + 0x104);
    put32(2052, BASE + 0x204);
    put32(2048, BASE + 0x304);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x100) == 3 && get32(0x200) == 2 &&
           get32(0x300) == 1);
    setup("_V0 backward intermediate\n$7202 81000000 FFFFFFFC\n$7200 00000000 FFFFFFF8\n"
          "$7701 00000000 00000003\n$0003 FFFFFFFC FFFFFFFF");
    put32(0, BASE + 0x10C);
    put32(0x108, BASE + 0x208);
    put32(0x104, BASE + 0x308);
    put32(0x100, BASE + 0x408);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x200) == 3 && get32(0x300) == 2 &&
           get32(0x400) == 1);
    setup("_V0 backward leaf\n$7201 81000000 FFFFFFFC\n$7701 00000000 00000003\n"
          "$0003 FFFFFFFC FFFFFFFF");
    put32(0, BASE + 0x10C);
    assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x108) == 3 && get32(0x104) == 2 &&
           get32(0x100) == 1);
    setup("_V0 underflow\n$3201 81000000 FFFFFFFC\n$3300 00000000 00000001");
    put32(0, 3);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 0);
    setup("_V0 overflow\n$3201 81000000 00000004\n$3300 00000000 00000001");
    put32(0, UINT32_MAX - 3);
    assert(vco_cheat_enable(&runtime) == VCO_OK && mem.writes == 0);
}
static void append_code(char *text, size_t capacity, size_t *length, unsigned code, uint32_t a,
                        uint32_t b)
{
    int n = snprintf(text + *length, capacity - *length, "$%04X %08X %08X\n", code, a, b);
    assert(n > 0 && (size_t)n < capacity - *length);
    *length += (size_t)n;
}
static void depth_tests(void)
{
    char text[32768];
    for (unsigned family = 3; family <= 8; family += family == 3 ? 4 : 1) {
        strcpy(text, "_V0 maximum depth\n");
        size_t length = strlen(text);
        unsigned parts = family == 8 ? 2 : 1;
        for (unsigned part = 0; part < parts; ++part) {
            unsigned opcode = family * 0x1000 + (2 + part * 4) * 0x100;
            append_code(text, sizeof(text), &length, opcode | 255, BASE + part * 4, 0);
            for (unsigned depth = 1; depth < 255; ++depth)
                append_code(text, sizeof(text), &length, opcode, 0, depth == 254 ? 8 : 0);
            unsigned marker = family == 3 ? 0x3300 : family == 7 ? 0x77FF : 0x8800 + part * 0x100;
            append_code(text, sizeof(text), &length, marker, 0, family == 8 ? 0 : 0x12345678);
        }
        if (family == 7)
            append_code(text, sizeof(text), &length, 2, 4, 1);
        setup(text);
        put32(0, BASE + 0x100);
        put32(4, BASE + 0x200);
        put32(0x100, BASE + 0x100);
        put32(0x200, BASE + 0x200);
        put32(0x208, 0x12345678);
        assert(vco_cheat_enable(&runtime) == VCO_OK && get32(0x108) == 0x12345678);
        if (family == 7)
            assert(get32(0x10C) == 0x12345679);
        assert(mem.reads == (family == 3 ? 255u : family == 7 ? 510u : 511u));
    }
}
static void capacity_tests(void)
{
    const char *valid = "_V0 a\n$0200 81000000 00000001";
    assert(parse(valid) == VCO_OK);
    file.entry_capacity = 0;
    assert(vco_cheat_parse(valid, strlen(valid), &file, &diag) == VCO_E_LIMIT && !file.entry_count);
    assert(parse(valid) == VCO_OK);
    file.name_capacity = 1;
    assert(vco_cheat_parse(valid, strlen(valid), &file, &diag) == VCO_E_LIMIT && !file.code_count);
    assert(parse(valid) == VCO_OK);
    program = (VcoCheatProgram){ops, 0, 0, NULL, 0};
    assert(vco_cheat_compile(&file, 0, &program, &diag) == VCO_E_LIMIT && !program.count);
    assert(compile(0) == VCO_OK);
    assert(vco_cheat_runtime_init(&runtime, &program, backend(), 7, patches, 0, 100, 0) ==
           VCO_E_ARGUMENT);
    assert(parse("_V0 a\n$0000 81000000 00000001\n_V0 a\n$0000 81000001 00000002") == VCO_OK);
    assert(file.entry_count == 2 && entries[0].first != entries[1].first);
    const char *invalid[] = {"_V0 \xC0\x80", "_V0 \xED\xA0\x80", "_V0 \xF4\x90\x80\x80",
                             "_V0 \xE4\xB8"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        assert(parse(invalid[i]) == VCO_E_ENCODING);
}
static uint32_t random_word(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}
static void mutation_tests(void)
{
    static const char *const seeds[] = {
        "_V0 a\n$0200 81000000 12345678\n",
        ("_V1 a\n$7202 81000000 00000000\n$7200 00000000 00000000\n$7701 00000000 00000001\n$0002 "
         "00000004 00000000\n"),
        "_V0 a\n$D002 81000000 00000001\n$4101 81000004 00000002\n$0002 00000002 00000001\n",
        "_V0 a\n$B200 00000000 00000000\n$A100 00000010 0000BF00\n"};
    uint32_t seed = 0x56434Fu;
    char buffer[512];
    for (unsigned test = 0; test < 10000; ++test) {
        const char *source = seeds[test % 4];
        size_t length = strlen(source);
        memcpy(buffer, source, length);
        for (unsigned j = 0; j < 1 + test % 4; ++j) {
            size_t position = random_word(&seed) % length;
            buffer[position] = (char)(random_word(&seed) >> 24);
        }
        if (test % 3 == 0)
            length = random_word(&seed) % (length + 1);
        if (parse_n(buffer, length) != VCO_OK)
            continue;
        for (size_t e = 0; e < file.entry_count; ++e) {
            if (compile(e) != VCO_OK)
                continue;
            memset(&mem, 0, sizeof(mem));
            mem.session = 7;
            assert(vco_cheat_runtime_init(&runtime, &program, backend(), 7, patches, N, 64, 7) ==
                   VCO_OK);
            (void)vco_cheat_enable(&runtime);
            (void)vco_cheat_disable(&runtime);
            for (size_t i = 0; i < N; ++i)
                assert(!mem.live[i]);
        }
    }
}

int main(int argc, char **argv)
{
    struct {
        const char *name;
        void (*run)(void);
    } cases[] = {{"parser", parser_tests},
                 {"write", write_tests},
                 {"pointer", pointer_tests},
                 {"dormant_pointer", dormant_pointer_tests},
                 {"copy_headers", copy_header_tests},
                 {"access_session", access_session_tests},
                 {"patch", patch_tests},
                 {"unaligned_patch", unaligned_patch_tests},
                 {"control", control_tests},
                 {"failure", failure_tests},
                 {"widths", width_matrix_tests},
                 {"boundaries", boundary_tests},
                 {"capacity", capacity_tests},
                 {"mutations", mutation_tests},
                 {"compatibility", compatibility_tests},
                 {"conditional_patch", conditional_patch_tests},
                 {"module_combinations", module_combination_tests},
                 {"offsets", offset_tests},
                 {"depth", depth_tests}};
    int ran = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0)
            continue;
        cases[i].run();
        printf("PASS: %s\n", cases[i].name);
        ++ran;
    }
    return ran ? 0 : 1;
}
