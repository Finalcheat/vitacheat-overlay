#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psp2kern/ctrl.h>
#include "platform/vita/pad.h"
static VcoSession session;
static const VcoIdentity identity = {42, 7, "PCSB00031"};
static VcoMemoryAdapter adapter;
static VcoVitaPad pad;
static struct {
    SceCtrlPortInfo ports;
    unsigned port_calls, peek_calls, validation_calls, capture_calls;
    unsigned fail_port_at, fail_validation_at, fail_capture_at;
    unsigned change_port_at, transition_at, revoke_capture_at;
    unsigned revoke_peek;
    uint32_t buttons, epoch;
    uint64_t stamp;
    int result, active, transition_active;
    bool authorized, freeze_stamp, transition_peek;
} sdk;
static uint32_t out_device, out_buttons;

static void revoke(unsigned action)
{
    if (action == 1)
        sdk.authorized = false;
    else if (action == 2)
        vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL);
    else if (action == 3) {
        VcoIdentity next = identity;
        ++next.instance;
        vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &next);
    }
}
int ksceCtrlGetControllerPortInfo(SceCtrlPortInfo *out)
{
    ++sdk.port_calls;
    *out = sdk.ports;
    if (sdk.port_calls == sdk.change_port_at)
        out->port[0] = SCE_CTRL_TYPE_DS4;
    return sdk.port_calls == sdk.fail_port_at ? -1 : 0;
}
int ksceCtrlPeekBufferPositive(int port, SceCtrlData *out, int count)
{
    assert(port == 0 && count == 1);
    ++sdk.peek_calls;
    if (!sdk.freeze_stamp)
        ++sdk.stamp;
    memset(out, 0xa5, sizeof(*out));
    out->timeStamp = sdk.stamp;
    out->buttons = sdk.buttons;
    revoke(sdk.revoke_peek);
    if (sdk.transition_peek) {
        sdk.active = sdk.transition_active;
        ++sdk.epoch;
    }
    return sdk.result;
}
static int validate(void *context, const VcoIdentity *target, bool foreground)
{
    assert(context == &sdk && target->pid == identity.pid && foreground);
    ++sdk.validation_calls;
    return !sdk.authorized || sdk.validation_calls == sdk.fail_validation_at ? -1 : 0;
}
static int suppression(void *context, uint32_t *epoch)
{
    assert(context == &sdk);
    ++sdk.capture_calls;
    if (sdk.capture_calls == sdk.transition_at) {
        sdk.active = sdk.transition_active;
        ++sdk.epoch;
    }
    if (sdk.capture_calls == sdk.revoke_capture_at)
        sdk.authorized = false;
    *epoch = sdk.epoch;
    if (sdk.capture_calls == sdk.fail_capture_at) {
        *epoch = UINT32_MAX;
        return -1;
    }
    return sdk.active;
}
static void setup(void)
{
    memset(&sdk, 0, sizeof(sdk));
    sdk.ports.port[0] = SCE_CTRL_TYPE_PHY;
    sdk.authorized = true;
    sdk.epoch = 10;
    sdk.stamp = 10;
    sdk.result = 1;
    vco_session_init(&session);
    assert(vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity));
    adapter = (VcoMemoryAdapter){0};
    adapter.session = &session;
    adapter.transport.context = &sdk;
    adapter.transport.validate = validate;
    assert(vco_vita_pad_init(&pad, &adapter, suppression, &sdk));
}
static int read_pad(uint64_t token)
{
    out_device = 0xdeadbeef;
    out_buttons = 0xa5a5a5a5;
    int result = vco_vita_pad_read(&pad, token, &out_device, &out_buttons);
    if (result) {
        assert(out_device == 0xdeadbeef && out_buttons == 0xa5a5a5a5);
    } else {
        assert(out_device == sdk.ports.port[0]);
        assert(out_buttons == (sdk.buttons & 0xf3f9u));
    }
    return result;
}
static void arm(void)
{
    sdk.buttons = 0;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.buttons = SCE_CTRL_CROSS;
    assert(!read_pad(session.token));
}
static void mapping(void)
{
    assert(sizeof(SceCtrlData) == 32 && sizeof(SceCtrlPortInfo) == 16);
    arm();
    const unsigned bits[] = {1,     8,      0x10,   0x20,   0x40,   0x80,  0x100,
                             0x200, 0x1000, 0x2000, 0x4000, 0x8000, 0x300, 0xf3f9};
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        sdk.buttons = bits[i];
        assert(!read_pad(session.token) && out_buttons == bits[i]);
    }
    sdk.buttons = SCE_CTRL_HEADPHONE | SCE_CTRL_VOLUP | SCE_CTRL_VOLDOWN | SCE_CTRL_POWER;
    assert(!read_pad(session.token) && !out_buttons);
    sdk.buttons = SCE_CTRL_PSBUTTON | SCE_CTRL_CROSS;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.buttons = SCE_CTRL_CROSS;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    arm();
    sdk.buttons = 0x400; /* Ext2 L1 is not VitaCheat's documented 0x100 trigger. */
    assert(read_pad(session.token) < 0);
    sdk.buttons = SCE_CTRL_CROSS;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);

    const unsigned unsupported[] = {3, 255};
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        sdk.ports.port[0] = (uint8_t)unsupported[i];
        unsigned peeks = sdk.peek_calls;
        assert(read_pad(session.token) < 0 && sdk.peek_calls == peeks);
    }
    sdk.ports.port[0] = 1;
    for (unsigned i = 1; i < 5; ++i) {
        sdk.ports.port[i] = 255;
        assert(read_pad(session.token) < 0);
        sdk.ports.port[i] = 0;
    }
    arm();
}
static void controllers(void)
{
    /* Original Z06 uses logical port 0. Each profile below still calls plain
     * PeekBufferPositive(0,...,1), regardless of where an external pad appears. */
    const uint8_t profiles[][5] = {
        {SCE_CTRL_TYPE_PHY, SCE_CTRL_TYPE_DS4, 0, 0, 0},
        {SCE_CTRL_TYPE_VIRT, SCE_CTRL_TYPE_DS3, SCE_CTRL_TYPE_DS4, 0, 0},
        {SCE_CTRL_TYPE_DS3, 0, 0, 0, 0},
        {SCE_CTRL_TYPE_DS4, 0, 0, 0, 0},
        {SCE_CTRL_TYPE_UNPAIRED, SCE_CTRL_TYPE_DS3, 0, 0, 0},
    };
    arm();
    for (unsigned i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        memcpy(sdk.ports.port, profiles[i], sizeof(profiles[i]));
        sdk.buttons = SCE_CTRL_CROSS;
        assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
        assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
        arm();
        sdk.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
        assert(!read_pad(session.token) && out_buttons == 0x300);
        sdk.active = 1;
        ++sdk.epoch;
        assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
        sdk.active = 0;
        assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
        arm();
    }
    /* A paired-controller change between worker polls must disarm even if
     * both native topology reads in each individual poll are stable. */
    sdk.ports.port[4] = SCE_CTRL_TYPE_DS4;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    arm();
    sdk.ports.port[4] = SCE_CTRL_TYPE_UNPAIRED;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    arm();
    sdk.buttons = 0x400;
    assert(read_pad(session.token) < 0); /* No Ext2 trigger reinterpretation. */
}
static void capture_case(void)
{
    sdk.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED); /* First call held chord. */
    arm();
    sdk.active = 1;
    ++sdk.epoch;
    unsigned peeks = sdk.peek_calls;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED && sdk.peek_calls == peeks);
    sdk.active = 0; /* Capture/drain lease expires without incrementing epoch. */
    for (unsigned frame = 0; frame < 10; ++frame)
        assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.buttons = 0;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.freeze_stamp = true;
    sdk.buttons = SCE_CTRL_CROSS;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED); /* Cached release frame. */
    sdk.freeze_stamp = false;
    assert(!read_pad(session.token));

    /* Overlay opened and closed entirely between worker polls. */
    sdk.epoch += 2;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    arm();
    sdk.transition_peek = true;
    sdk.transition_active = 1;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.transition_peek = false;
    sdk.active = 0;
    arm();
    /* Both capture transitions happen during sampling; epoch still catches it. */
    sdk.transition_peek = true;
    sdk.transition_active = 0;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.transition_peek = false;
    arm();
    sdk.transition_at = sdk.capture_calls + 2;
    sdk.transition_active = 1;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.transition_at = 0;
    sdk.active = 0;
    arm();
    --sdk.epoch;
    assert(read_pad(session.token) < 0 && pad.poisoned);
    sdk.epoch += 2;
    assert(read_pad(session.token) < 0);
    assert(vco_vita_pad_init(&pad, &adapter, suppression, &sdk));
    arm();
    sdk.epoch = UINT32_MAX;
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    sdk.epoch = 0;
    assert(read_pad(session.token) < 0 && pad.poisoned);
}
static void lifecycle(void)
{
    arm();
    unsigned peeks = sdk.peek_calls;
    assert(read_pad(session.token + 1) < 0 && sdk.peek_calls == peeks);
    arm();
    vco_session_observe(&session, VCO_OBSERVATION_BACKGROUND, &identity);
    assert(read_pad(session.token) < 0);
    vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
    assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
    arm();
    for (unsigned action = 1; action <= 3; ++action) {
        setup();
        arm();
        sdk.revoke_peek = action;
        uint64_t token = session.token;
        assert(read_pad(token) < 0);
        sdk.revoke_peek = 0;
        sdk.authorized = true;
        if (action > 1)
            vco_session_observe(&session, VCO_OBSERVATION_FOREGROUND, &identity);
        assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
        arm();
    }
    sdk.revoke_capture_at = sdk.capture_calls + 2;
    assert(read_pad(session.token) < 0); /* Validation occurs after post-capture callback. */
}
static void failures(void)
{
    const int bad_results[] = {-1, 0, 2};
    for (unsigned i = 0; i < sizeof(bad_results) / sizeof(bad_results[0]); ++i) {
        setup();
        arm();
        sdk.result = bad_results[i];
        assert(read_pad(session.token) < 0);
        sdk.result = 1;
        assert(read_pad(session.token) == VCO_PAD_SUPPRESSED);
        arm();
    }
    for (unsigned stage = 1; stage <= 2; ++stage) {
        setup();
        arm();
        sdk.fail_port_at = sdk.port_calls + stage;
        assert(read_pad(session.token) < 0);
        sdk.fail_port_at = 0;
        arm();
        sdk.fail_validation_at = sdk.validation_calls + stage;
        assert(read_pad(session.token) < 0);
        sdk.fail_validation_at = 0;
        arm();
        sdk.fail_capture_at = sdk.capture_calls + stage;
        assert(read_pad(session.token) < 0 && !pad.poisoned);
        sdk.fail_capture_at = 0;
        arm();
    }
    sdk.change_port_at = sdk.port_calls + 2;
    assert(read_pad(session.token) < 0);
    sdk.change_port_at = 0;
    arm();
    sdk.active = 2;
    assert(read_pad(session.token) < 0);
    sdk.active = 0;
    arm();
    out_device = 123;
    assert(vco_vita_pad_read(&pad, session.token, &out_device, &out_device) < 0);
    assert(out_device == 123);
    assert(vco_vita_pad_read(NULL, session.token, &out_device, &out_buttons) < 0);
    assert(vco_vita_pad_read(&pad, session.token, NULL, &out_buttons) < 0);
    assert(vco_vita_pad_read(&pad, session.token, &out_device, NULL) < 0);
    assert(!vco_vita_pad_init(NULL, &adapter, suppression, &sdk));
    assert(!vco_vita_pad_init(&pad, &adapter, NULL, &sdk));
    assert(read_pad(session.token) < 0);
    assert(!vco_vita_pad_init(&pad, NULL, suppression, &sdk));
    adapter.transport.validate = NULL;
    assert(!vco_vita_pad_init(&pad, &adapter, suppression, &sdk));
}
static unsigned writes;
static int alive(void *context, uint64_t token)
{
    assert(context == &pad);
    return vco_session_matches(&session, token, true) && sdk.authorized;
}
static int write_memory(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                        size_t size)
{
    assert(alive(context, token) && address == 0x81000000u && bytes && size == 4);
    ++writes;
    return 0;
}
static void pipeline(void)
{
    VcoCheatEntry entries[1];
    VcoCode codes[2];
    VcoCheatOp operations[2];
    VcoPatch patches[2];
    char names[32];
    VcoCheatFile file = {entries, 1, 0, codes, 2, 0, names, sizeof(names), 0};
    VcoCheatProgram program = {operations, 2, 0, NULL, 0};
    VcoDiagnostic diagnostic;
    /* Overlay suppression blocks conditions until release and a fresh exact press. */
    const char text[] = "_V1 PAD\n$C201 00000000 00004000\n$0200 81000000 11223344\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    assert(!vco_cheat_compile(&file, 0, &program, &diagnostic));
    VcoCheatMemory memory = {0};
    memory.context = &pad;
    memory.alive = alive;
    memory.pad = vco_vita_pad_read;
    memory.write = write_memory;
    VcoCheatRuntime runtime;
    assert(!vco_cheat_runtime_init(&runtime, &program, memory, session.token, patches, 2, 32, 0));
    assert(!vco_cheat_enable(&runtime));
    writes = 0;
    sdk.ports.port[0] = SCE_CTRL_TYPE_VIRT;
    sdk.ports.port[1] = SCE_CTRL_TYPE_DS4;
    sdk.active = 1;
    sdk.buttons = SCE_CTRL_CROSS;
    assert(!vco_cheat_tick(&runtime) && !writes && runtime.state == VCO_CHEAT_ON);
    sdk.active = 0;
    ++sdk.epoch;
    assert(!vco_cheat_tick(&runtime) && !writes);
    sdk.buttons = 0;
    assert(!vco_cheat_tick(&runtime) && !writes);
    sdk.buttons = SCE_CTRL_CROSS;
    assert(!vco_cheat_tick(&runtime) && writes == 1);
    sdk.active = 1;
    ++sdk.epoch;
    assert(!vco_cheat_tick(&runtime) && writes == 1);
    sdk.active = 0;
    ++sdk.epoch;
    sdk.result = -1;
    assert(vco_cheat_tick(&runtime) == VCO_E_IO && writes == 1);
    assert(runtime.state == VCO_CHEAT_ERROR);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "mapping"))
        mapping();
    else if (!strcmp(argv[1], "controllers"))
        controllers();
    else if (!strcmp(argv[1], "capture"))
        capture_case();
    else if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "failures"))
        failures();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else
        assert(!"unknown pad test");
    printf("vita_pad %s passed\n", argv[1]);
    return 0;
}
