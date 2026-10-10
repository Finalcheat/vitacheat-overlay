#include "execution/upload.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static VcoCheatEntry entries[8], decoded_entries[8];
static VcoCode codes[16], decoded_codes[16];
static char names[256];
static VcoCheatFile source, decoded;
static unsigned char packet[VCO_UPLOAD_MAX];
static size_t length;
static VcoUploadHeader header;
static void setup(void)
{
    source = (VcoCheatFile){entries, 8, 0, codes, 16, 0, names, sizeof(names), 0};
    decoded = (VcoCheatFile){decoded_entries, 8, 0, decoded_codes, 16, 0, NULL, 0, 0};
    const char text[] = "_V1 Money\n$0200 81000000 00000064\n"
                        "_V0 Ammo\n$0100 81000004 00000019\n";
    VcoDiagnostic diagnostic;
    assert(vco_cheat_parse(text, strlen(text), &source, &diagnostic) == VCO_OK);
    assert(vco_upload_encode(packet, sizeof(packet), 9, 1, 3, 77, "PCSE00001", &source, &length) ==
           VCO_OK);
}
static void roundtrip(void)
{
    assert(vco_upload_decode(packet, length, &decoded, &header) == VCO_OK);
    assert(header.request == 9 && header.target_session == 1 && header.catalog_revision == 3 &&
           header.pid == 77);
    assert(decoded.entry_count == 2 && decoded.code_count == 2);
    assert(decoded.entries[0].file_enabled == 0 && source.entries[0].file_enabled == 1);
    for (unsigned i = 0; i < 2; ++i) {
        assert(decoded.codes[i].code == source.codes[i].code &&
               decoded.codes[i].a == source.codes[i].a && decoded.codes[i].b == source.codes[i].b &&
               decoded.codes[i].line == source.codes[i].line);
        assert(decoded.entries[i].first == source.entries[i].first);
    }
    /* Protocol may be copied to an unaligned transport buffer. */
    unsigned char unaligned[512];
    memcpy(unaligned + 1, packet, length);
    assert(vco_upload_decode(unaligned + 1, length, &decoded, &header) == VCO_OK);
}
static void corrupt(void)
{
    header.request = 101;
    for (size_t i = 0; i < length; ++i) {
        /* Every payload mutation must be caught by content identity. */
        if (i < sizeof(VcoUploadHeader))
            continue;
        packet[i] ^= 0x80;
        decoded.entry_count = decoded.code_count = 99;
        assert(vco_upload_decode(packet, length, &decoded, &header) != VCO_OK);
        assert(!decoded.entry_count && !decoded.code_count && header.request == 101);
        packet[i] ^= 0x80;
    }
    for (size_t size = 0; size < length; ++size)
        assert(vco_upload_decode(packet, size, &decoded, &header) != VCO_OK);
    VcoUploadHeader saved;
    memcpy(&saved, packet, sizeof(saved));
    VcoUploadHeader invalid = saved;
    invalid.reserved[3] = 1;
    memcpy(packet, &invalid, sizeof(invalid));
    assert(vco_upload_decode(packet, length, &decoded, &header) == VCO_E_ARGUMENT);
    invalid = saved;
    invalid.codes = UINT32_MAX;
    memcpy(packet, &invalid, sizeof(invalid));
    assert(vco_upload_decode(packet, length, &decoded, &header) == VCO_E_ARGUMENT);
    invalid = saved;
    invalid.target_session = 0;
    memcpy(packet, &invalid, sizeof(invalid));
    assert(vco_upload_decode(packet, length, &decoded, &header) == VCO_E_ARGUMENT);
    invalid = saved;
    memcpy(invalid.title_id, "NPXS10015", 10);
    memcpy(packet, &invalid, sizeof(invalid));
    assert(vco_upload_decode(packet, length, &decoded, &header) == VCO_E_ARGUMENT);
}
static void limits(void)
{
    assert(!vco_upload_size(257, 1) && !vco_upload_size(1, 8193));
    assert(vco_upload_size(256, 8192) == VCO_UPLOAD_MAX);
    decoded.code_capacity = 1;
    assert(vco_upload_decode(packet, length, &decoded, &header) == VCO_E_LIMIT);
    source.entry_count = source.entry_capacity + 1;
    size_t unchanged = 99;
    assert(vco_upload_encode(packet, sizeof(packet), 9, 1, 3, 77, "PCSE00001", &source,
                             &unchanged) == VCO_E_ARGUMENT);
    assert(unchanged == 99);
    setup();
    entries[1].first = 0;
    assert(vco_upload_encode(packet, sizeof(packet), 9, 1, 3, 77, "PCSE00001", &source,
                             &unchanged) == VCO_E_ARGUMENT);
    setup();
    codes[0].line = 0;
    assert(vco_upload_encode(packet, sizeof(packet), 9, 1, 3, 77, "PCSE00001", &source,
                             &unchanged) == VCO_E_ARGUMENT);
    setup();
    assert(vco_upload_encode(packet, sizeof(packet), 9, 0, 3, 77, "PCSE00001", &source,
                             &unchanged) == VCO_E_ARGUMENT);
    setup();
    assert(vco_upload_encode(packet, length - 1, 9, 1, 3, 77, "PCSE00001", &source, &unchanged) ==
           VCO_E_LIMIT);
    setup();
    source.entry_count = source.code_count = 0;
    assert(!vco_upload_encode(packet, sizeof(packet), 10, 1, 4, 77, "PCSE00001", &source, &length));
    assert(length == sizeof(VcoUploadHeader));
    assert(!vco_upload_decode(packet, length, &decoded, &header));
    assert(!decoded.entry_count && !decoded.code_count);
}
static uint8_t game[8];
static int alive(void *ctx, uint64_t token)
{
    (void)ctx;
    return token == 1;
}
static int write_memory(void *ctx, uint64_t token, uint32_t address, const uint8_t *data,
                        size_t size)
{
    (void)ctx;
    if (token != 1 || address < 0x81000000u || (uint64_t)address + size > UINT64_C(0x81000008))
        return -1;
    memcpy(game + (address - 0x81000000u), data, size);
    return 0;
}
static void pipeline(void)
{
    static VcoEngine engine;
    VcoSession session;
    vco_session_init(&session);
    VcoIdentity identity = {.pid = 77, .instance = 1, .title_id = "PCSE00001"};
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    VcoCheatOp ops[16];
    VcoPatch patches[16];
    VcoCheatMemory memory = {.alive = alive, .write = write_memory};
    assert(!vco_upload_decode(packet, length, &decoded, &header));
    vco_engine_init(&engine);
    assert(!vco_engine_bind(&engine, &session, 1, &decoded, memory, ops, patches, 16));
    vco_engine_step(&engine, 50000);
    assert(game[0] == 0 && engine.rows[0].state == VCO_ENTRY_OFF);
    assert(!vco_engine_submit(&engine, (VcoExecutionCommand){1, session.token, 1, 0, true}));
    vco_engine_step(&engine, 100000);
    assert(game[0] == 100 && engine.rows[0].state == VCO_ENTRY_ON);
    /* Identical source is a new explicit list: starts OFF, old commands stale. */
    assert(!vco_engine_bind(&engine, &session, 2, &decoded, memory, ops, patches, 16));
    assert(engine.rows[0].state == VCO_ENTRY_OFF);
    assert(vco_engine_submit(&engine, (VcoExecutionCommand){2, session.token, 1, 0, true}) ==
           VCO_E_SESSION);
    game[0] = 12;
    vco_engine_step(&engine, 150000);
    assert(game[0] == 12);
    assert(!vco_engine_unbind(&engine));
    /* A catalog's explicit game/version mismatch survives the user/kernel
     * copy. Even a direct enable request cannot bypass the read-only warning. */
    entries[0].diagnostic = (VcoDiagnostic){VCO_E_METADATA, 2};
    assert(!vco_upload_encode(packet, sizeof(packet), 10, 1, 4, 77, "PCSE00001", &source, &length));
    assert(!vco_upload_decode(packet, length, &decoded, &header));
    assert(!vco_engine_bind(&engine, &session, 3, &decoded, memory, ops, patches, 16));
    assert(engine.rows[0].state == VCO_ENTRY_UNSUPPORTED &&
           engine.rows[0].diagnostic.code == VCO_E_METADATA && engine.rows[0].diagnostic.line == 2);
    assert(vco_engine_submit(&engine, (VcoExecutionCommand){3, session.token, 3, 0, true}) ==
           VCO_E_METADATA);
    vco_engine_step(&engine, 200000);
    assert(game[0] == 12 && engine.rows[1].state == VCO_ENTRY_OFF);
    assert(!vco_engine_unbind(&engine));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "roundtrip"))
        roundtrip();
    else if (!strcmp(argv[1], "corrupt"))
        corrupt();
    else if (!strcmp(argv[1], "limits"))
        limits();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else
        assert(!"unknown case");
    printf("upload %s passed\n", argv[1]);
    return 0;
}
