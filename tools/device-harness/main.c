#include "codes.h"
#include "fixture.h"
#include "fixture_observer.h"
#include "observations.h"
#include "debugScreen.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

#define CHEATS "ux0:vitacheat/db/VCOCH0001.psv"
static volatile uint32_t module_marker;
static int rx_expected(const uint16_t words[2])
{
    return words[1] != 0x4770 ? -1 : words[0] == 0x2007 ? 7 : words[0] == 0x2009 ? 9 : -1;
}
__attribute__((naked, noinline, used)) static int patch_target(void)
{
    __asm__ volatile("movs r0, #7\n bx lr\n");
}
/* Eight bytes, with a 32-bit Thumb instruction deliberately starting at +2.
 * Keep nop/bx unchanged; only the immediate in mov.w changes from 7 to 9. */
__attribute__((naked, noinline, used, aligned(4))) static int thumb32_target(void)
{
    __asm__ volatile("nop\n mov.w r0, #7\n bx lr\n");
}
static void reset(VcoHarnessData *data)
{
    memset(data, 0, sizeof(*data));
    data->pointer = (uint32_t)(uintptr_t)&data->target_a;
    data->source_pointer = (uint32_t)(uintptr_t)&data->source;
    data->source = 1234;
    data->patch = 5;
    for (unsigned i = 0; i < VCO_HARNESS_BULK_PATCHES; ++i)
        data->bulk[i] = VCO_HARNESS_BULK_ORIGINAL + i;
    module_marker = 0;
}
static int write_codes(const char *text, size_t length)
{
    sceIoMkdir("ux0:vitacheat", 0777);
    sceIoMkdir("ux0:vitacheat/db", 0777);
    FILE *file = fopen(CHEATS ".tmp", "wb");
    if (!file)
        return -1;
    int okay = fwrite(text, 1, length, file) == length;
    if (fclose(file))
        okay = 0;
    return !okay || rename(CHEATS ".tmp", CHEATS) ? -1 : 0;
}
static int fixture_info(void *unused, int32_t uid, VcoFixtureInfo *out)
{
    (void)unused;
    SceKernelModuleInfo info = {.size = sizeof(info)};
    SceUID ids[257];
    SceSize count = 257;
    int result = sceKernelGetModuleInfo(uid, &info);
    if (result < 0)
        return result;
    result = sceKernelGetModuleList(0xff, ids, &count);
    if (result < 0)
        return result;
    if (!count || count > 256 || info.modid != uid)
        return -1;
    unsigned index = 256, matches = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (ids[count - 1 - i] == uid) {
            index = i;
            ++matches;
        }
    }
    if (matches != 1)
        return -1;
    *out = (VcoFixtureInfo){info.modid,
                            info.state,
                            index,
                            {(uint32_t)(uintptr_t)info.segments[0].vaddr, info.segments[0].memsz,
                             info.segments[0].perms},
                            {(uint32_t)(uintptr_t)info.segments[1].vaddr, info.segments[1].memsz,
                             info.segments[1].perms}};
    return 0;
}
static int32_t fixture_load(void *unused, unsigned variant)
{
    (void)unused;
    return sceKernelLoadModule(variant ? "app0:fixture-b.suprx" : "app0:fixture-a.suprx", 0, NULL);
}
static int fixture_start(void *unused, int32_t uid, const VcoFixtureStart *start, int *status)
{
    (void)unused;
    /* StartModule accepts a mutable argument pointer but owns a copy of it. */
    return sceKernelStartModule(uid, sizeof(*start), (void *)start, 0, NULL, status);
}
static int fixture_stop(void *unused, int32_t uid, int *status)
{
    (void)unused;
    return sceKernelStopUnloadModule(uid, 0, NULL, 0, NULL, status);
}
static int generate_fixture(const VcoFixtureController *fixture)
{
    char text[1024];
    int length = vco_fixture_codes(fixture, text, sizeof(text));
    return length > 0 ? write_codes(text, (size_t)length) : -1;
}
static VcoFixtureReading read_fixture(const VcoFixtureController *fixture,
                                      const VcoFixtureObserver *observer)
{
    /* App-thread ownership keeps our explicit load/unload operations out of
     * this sampling scope. No old address is read after UNLOADING/FAULT.
     * The observer checks the copied instruction pair before a target call;
     * neither this read nor the later call is an atomic module lifetime pin. */
    VcoFixtureReading reading = {0};
    reading.generation = fixture->generation;
    reading.uid = fixture->uid;
    reading.data_address = fixture->reply.data_address;
    reading.function_address = fixture->reply.function_address;
    reading.function = -1;
    reading.data = *(volatile uint32_t *)(uintptr_t)reading.data_address;
    const volatile uint16_t *code =
        (const volatile uint16_t *)(uintptr_t)(reading.function_address & ~1u);
    reading.instruction = code[0];
    reading.return_instruction = code[1];
    if (vco_fixture_observer_callable(observer, fixture, &reading)) {
        int (*volatile target)(void) = (int (*)(void))(uintptr_t)reading.function_address;
        reading.function = target();
        reading.called = true;
    }
    return reading;
}
static int generate(VcoHarnessData *data)
{
    uint32_t function = (uint32_t)(uintptr_t)patch_target & ~1u;
    uint32_t thumb32_function = (uint32_t)(uintptr_t)thumb32_target & ~1u;
    SceUID self = sceKernelGetModuleIdByAddr((void *)(uintptr_t)function);
    SceUID ids[257];
    SceSize count = 257;
    SceKernelModuleInfo info = {.size = sizeof(info)};
    if (self < 0 || sceKernelGetModuleList(0xff, ids, &count) < 0 || count > 256 ||
        sceKernelGetModuleInfo(self, &info) < 0)
        return -1;
    unsigned index;
    for (index = 0; index < count; ++index)
        if (ids[count - 1 - index] == self)
            break;
    if (index == count || info.state != SCE_KERNEL_MODULE_STATE_STARTED ||
        !info.segments[0].vaddr || info.segments[0].memsz < 4 || !info.segments[1].vaddr ||
        info.segments[1].memsz < sizeof(module_marker) ||
        function < (uintptr_t)info.segments[0].vaddr ||
        (uint64_t)function + 4 >
            (uintptr_t)info.segments[0].vaddr + (uint64_t)info.segments[0].memsz ||
        thumb32_function < (uintptr_t)info.segments[0].vaddr ||
        (uint64_t)thumb32_function + 8 >
            (uintptr_t)info.segments[0].vaddr + (uint64_t)info.segments[0].memsz ||
        (uintptr_t)&module_marker < (uintptr_t)info.segments[1].vaddr ||
        (uint64_t)(uintptr_t)&module_marker + sizeof(module_marker) >
            (uintptr_t)info.segments[1].vaddr + (uint64_t)info.segments[1].memsz)
        return -1;
    char text[8192];
    int length = vco_harness_codes(
        text, sizeof(text), (uint32_t)(uintptr_t)data, function, thumb32_function, index,
        (uint32_t)(uintptr_t)info.segments[0].vaddr, (uint32_t)(uintptr_t)&module_marker,
        (uint32_t)(uintptr_t)info.segments[1].vaddr);
    if (length < 0 || (size_t)length >= sizeof(text))
        return -1;
    return write_codes(text, (size_t)length);
}
static int save_observation(const VcoHarnessData *data, const VcoFixtureController *fixture,
                            const VcoFixtureObserver *observer, const VcoFixtureReading *reading,
                            uint64_t run, uint64_t tick, unsigned sequence, unsigned buttons,
                            unsigned page, unsigned source, int generated, int fixture_generated)
{
    VcoHarnessObservation o = {0};
    o.run = run;
    o.tick = tick;
    o.sequence = sequence;
    o.pid = sceKernelGetProcessId();
    o.buttons = buttons;
    o.page = page;
    o.source = source;
    o.generated = generated;
    o.fixture_generated = fixture_generated;
    const uint32_t values[VCO_HARNESS_DATA_VALUES] = {
        data->direct32,    data->direct16,    data->direct8,     data->copy,
        data->repeat[0],   data->repeat[1],   data->repeat[2],   data->pointer,
        data->source,      data->target_a[0], data->target_a[1], data->target_a[2],
        data->target_b[0], data->target_b[1], data->target_b[2], data->condition,
        data->pad,         data->patch,       module_marker};
    memcpy(o.data, values, sizeof(values));
    vco_harness_bulk_counts(data, o.bulk);
    const volatile uint16_t *rx = (const volatile uint16_t *)((uintptr_t)patch_target & ~1u);
    const volatile uint16_t *thumb = (const volatile uint16_t *)((uintptr_t)thumb32_target & ~1u);
    o.rx[0] = rx[0];
    o.rx[1] = rx[1];
    for (unsigned i = 0; i < 4; ++i)
        o.thumb32[i] = thumb[i];
    int (*volatile call_rx)(void) = patch_target;
    int (*volatile call_thumb)(void) = thumb32_target;
    o.rx_result = rx_expected(o.rx) < 0 ? -1 : call_rx();
    o.thumb32_result = vco_harness_thumb32_expected(o.thumb32) < 0 ? -1 : call_thumb();
    o.fixture = *fixture;
    o.reading = *reading;
    o.witness = *observer;
    return vco_harness_observation_save(VCO_HARNESS_OBSERVATION_LOG, &o);
}
int main(void)
{
    if (psvDebugScreenInit() < 0)
        return 1;
    void *base = NULL;
    SceUID block =
        sceKernelAllocMemBlock("vco_harness", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096, NULL);
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0)
        return 1;
    VcoHarnessData *data = base;
    reset(data);
    int generated = generate(data);
    unsigned saved_source = generated ? 0 : 1; /* 1=standard, 2=fixture */
    VcoFixtureController fixture;
    vco_fixture_init(
        &fixture, (VcoFixtureOps){NULL, fixture_load, fixture_start, fixture_info, fixture_stop});
    VcoFixtureObserver observer;
    vco_fixture_observer_init(&observer);
    int fixture_generated = -1;
    uint32_t generated_generation = 0;
    unsigned cycle_loads = 0;
    int cycling = 0, fixture_page = 0;
    uint64_t next_cycle = 0;
    uint32_t previous = 0;
    unsigned frames = 0;
    int64_t run_clock = sceKernelGetSystemTimeWide();
    unsigned observations_saved = 0;
    int observation_error = run_clock <= 0 ? -1 : 0;
    bool observation_pending = false, fault_recorded = false;
    uint64_t observation_after = 0;
    int (*volatile call_target)(void) = patch_target;
    int (*volatile call_thumb32)(void) = thumb32_target;
    for (;;) {
        SceCtrlData input = {0};
        sceCtrlPeekBufferPositive(0, &input, 1);
        uint32_t pressed = input.buttons & ~previous;
        previous = input.buttons;
        if (pressed & SCE_CTRL_START) {
            fixture_page = !fixture_page;
            cycling = 0;
            vco_fixture_observer_stop(&observer);
        }
        if (fixture_page && !observer.error) {
            if (pressed & SCE_CTRL_CROSS) {
                cycling = 0;
                vco_fixture_observer_stop(&observer);
                (void)vco_fixture_load(&fixture);
            }
            if (pressed & SCE_CTRL_SQUARE) {
                cycling = 0;
                vco_fixture_observer_stop(&observer);
                (void)vco_fixture_unload(&fixture);
            }
            if (pressed & SCE_CTRL_TRIANGLE) {
                cycling = 0;
                vco_fixture_observer_stop(&observer);
                fixture_generated = generate_fixture(&fixture);
                if (!fixture_generated) {
                    generated_generation = fixture.generation;
                    saved_source = 2;
                }
            }
            if ((pressed & SCE_CTRL_CIRCLE) && fixture.state != VCO_FIXTURE_FAULT) {
                if (cycling) {
                    cycling = 0;
                    vco_fixture_observer_stop(&observer);
                } else {
                    /* A completed cycle keeps observing its final instance.
                     * An explicit new cycle ends that scope before beginning
                     * another, while any latched fault remains irreversible. */
                    vco_fixture_observer_stop(&observer);
                    cycling = !vco_fixture_observer_begin(&observer, &fixture);
                }
                cycle_loads = fixture.loads;
                next_cycle = 0;
            }
            uint64_t now = sceKernelGetProcessTimeWide();
            if (cycling && now >= next_cycle) {
                if (vco_fixture_step(&fixture)) {
                    cycling = 0;
                    vco_fixture_observer_stop(&observer);
                } else if (fixture.loads - cycle_loads >= 100)
                    cycling = 0; /* Continue observing the final loaded instance. */
                next_cycle = now + 250000;
            }
        } else if (!fixture_page) {
            if (pressed & SCE_CTRL_CROSS)
                reset(data);
            if (pressed & SCE_CTRL_TRIANGLE)
                data->pointer = data->pointer == (uint32_t)(uintptr_t)&data->target_a
                                    ? (uint32_t)(uintptr_t)&data->target_b
                                    : (uint32_t)(uintptr_t)&data->target_a;
            if (pressed & SCE_CTRL_SQUARE)
                data->direct32 = 12;
            if (pressed & SCE_CTRL_CIRCLE) {
                generated = generate(data);
                if (!generated)
                    saved_source = 1;
            }
        }
        VcoFixtureReading fixture_reading = {0};
        fixture_reading.function = -1;
        /* Sample every app-loop iteration, independently of the six-frame
         * screen refresh. A one-frame fault must remain visible afterwards. */
        if (fixture.state == VCO_FIXTURE_READY && !observer.error) {
            fixture_reading = read_fixture(&fixture, &observer);
            if (observer.active &&
                vco_fixture_observer_sample(&observer, &fixture, &fixture_reading))
                cycling = 0;
        }
        /* DOWN requests an observation after it has been released for 200ms.
         * In particular, DOWN must not alter the exact R-only C-code sample.
         * Record the first fixture fault as well; no extra target call occurs
         * after the observer has latched it. Logging never changes cheats. */
        if (!observation_error && observations_saved < VCO_HARNESS_OBSERVATION_LIMIT) {
            if ((pressed & SCE_CTRL_DOWN) ||
                (!fault_recorded && (observer.error || fixture.state == VCO_FIXTURE_FAULT)))
                observation_pending = true;
            if (observation_pending) {
                int64_t tick = sceKernelGetSystemTimeWide();
                if (tick < run_clock)
                    observation_error = -1;
                else if (input.buttons & SCE_CTRL_DOWN)
                    observation_after = 0;
                else if (!observation_after)
                    observation_after = (uint64_t)tick + 200000;
                else if ((uint64_t)tick >= observation_after) {
                    observation_error = save_observation(
                        data, &fixture, &observer, &fixture_reading, (uint64_t)run_clock,
                        (uint64_t)tick, observations_saved + 1, input.buttons, fixture_page,
                        saved_source, generated, fixture_generated);
                    if (!observation_error)
                        ++observations_saved;
                    fault_recorded = observer.error || fixture.state == VCO_FIXTURE_FAULT;
                    observation_pending = false;
                    observation_after = 0;
                }
            }
        }
        if (frames++ % 6 == 0 && !fixture_page) {
            unsigned bulk_counts[3];
            vco_harness_bulk_counts(data, bulk_counts);
            const volatile uint16_t *code =
                (const volatile uint16_t *)((uintptr_t)thumb32_target & ~(uintptr_t)1);
            uint16_t words[4] = {code[0], code[1], code[2], code[3]};
            int expected = vco_harness_thumb32_expected(words);
            /* Compare raw instruction bytes with the actual CPU return value.
             * Unexpected bytes are displayed without executing the function. */
            int thumb32_result = expected < 0 ? -1 : call_thumb32();
            const volatile uint16_t *rx =
                (const volatile uint16_t *)((uintptr_t)patch_target & ~(uintptr_t)1);
            uint16_t rx_words[2] = {rx[0], rx[1]};
            int rx_result = rx_expected(rx_words) < 0 ? -1 : call_target();
            psvDebugScreenPrintf(
                "\033[H\033[2JVitaCheat Overlay - controlled harness " VCO_HARNESS_VERSION "\n\n"
                "Title ID: VCOCH0001     Generated codes: %s\n"
                "L + SELECT: overlay. Settings -> Reload list after this app starts.\n"
                "Enable ONE test at a time; disable before moving to another.\n\n"
                "01 32-bit: %u (100)    02 16-bit: %u (25)    03 8-bit: %u (7)\n"
                "04 Copy: %u (1234)\n05 Repeat: %u %u %u (10 11 12)\n"
                "06/07/08 Pointer A: %u %u %u  B: %u %u %u\nRoot: %08X\n"
                "09 Compare: %u (66)    10 Hold R only: %u (77)\n"
                "11 Data patch: %u (ON 88 / OFF 5)\n"
                "12/13 RX function: %d (ON 9 / OFF 7)\n"
                "14 Module-relative data: %u (99)\n"
                "15 Thumb-2 at +2: raw %d / CPU %d (ON 9 / OFF 7)\n"
                "16 A200 x117: original %u / patched %u / other %u\n\n"
                "CROSS resets DATA only; disable patches before reset.\n"
                "TRIANGLE swaps pointer A/B. SQUARE sets direct32=12.\n"
                "CIRCLE regenerates these 16 codes. START: dynamic-module page.\n"
                "DOWN then release: save observation (%u/60, error %d).\n"
                "Closing overlay keeps data codes active. LiveArea pauses them.\n"
                "Exit with PS. Relaunch must start every row OFF.\n"
                "Addresses belong only to this app; generated .psv changes each run.\n",
                generated           ? "FAILED"
                : saved_source == 1 ? "OK"
                                    : "DYNAMIC FILE - CIRCLE",
                (unsigned)data->direct32, (unsigned)data->direct16, (unsigned)data->direct8,
                (unsigned)data->copy, (unsigned)data->repeat[0], (unsigned)data->repeat[1],
                (unsigned)data->repeat[2], (unsigned)data->target_a[0], (unsigned)data->target_a[1],
                (unsigned)data->target_a[2], (unsigned)data->target_b[0],
                (unsigned)data->target_b[1], (unsigned)data->target_b[2], (unsigned)data->pointer,
                (unsigned)data->condition, (unsigned)data->pad, (unsigned)data->patch, rx_result,
                (unsigned)module_marker, expected, thumb32_result, bulk_counts[0], bulk_counts[1],
                bulk_counts[2], observations_saved, observation_error);
        }
        if (frames % 6 == 1 && fixture_page) {
            const char *states[] = {"EMPTY", "LOADING", "READY", "UNLOADING", "FAULT - EXIT APP"};
            const VcoFixtureReading *shown =
                observer.error ? &observer.first_failure : &fixture_reading;
            psvDebugScreenPrintf(
                "\033[H\033[2JVCO Harness - dynamic module lifecycle\n\n"
                "State: %s   UID: %08X   Generation: %u   Variant: %u\n"
                "Loads: %u   Unloads: %u   Cycle: %s (100 loads max)\n"
                "Native result: %08X   Module status: %08X   Error stage: %u\n"
                "Data: %u (ON 88 / OFF %u)   Function: %d (ON 9 / OFF %u)\n"
                "Data addr: %08X   Thumb addr: %08X   B index: %u\n"
                "Codes saved: %s   Generated generation: %u\n\n"
                "Witness samples: %llu   Clean new instances: %u\n"
                "Reused origin addresses: data %u / function %u\n"
                "First fault: %u  Gen %u  UID %08X  Expected data/function: %u/%u\n"
                "Observed Thumb: %04X %04X   Function called: %s\n\n"
                "CROSS load A/B. SQUARE unload once. TRIANGLE generate 3 codes.\n"
                "After generation: L+SELECT -> Settings -> Reload list; all OFF.\n"
                "CIRCLE starts/stops 100 load/unload cycles (250 ms per step).\n"
                "START returns to normal vectors; CIRCLE there restores 16 codes.\n"
                "DOWN then release: save observation (%u/60, error %d).\n"
                "No cheats enabled by this app. Cycle never regenerates codes.\n"
                "A first witness fault stops calls/cycles until app exit.\n"
                "READY addresses only belong to this exact module lifetime.\n"
                "An unload/start error is retained: exit/relaunch, do not retry.\n"
                "Record native.log and screenshots. Stress is not a lifetime lock.\n",
                states[fixture.state], (unsigned)fixture.uid, (unsigned)fixture.generation,
                (unsigned)fixture.reply.variant, fixture.loads, fixture.unloads,
                cycling ? "RUNNING" : "STOPPED", (unsigned)fixture.native_result,
                (unsigned)fixture.module_status, (unsigned)fixture.error, (unsigned)shown->data,
                (unsigned)fixture.reply.data_initial, (int)shown->function,
                (unsigned)fixture.reply.function_initial, (unsigned)fixture.reply.data_address,
                (unsigned)fixture.reply.function_address, (unsigned)fixture.info.index,
                fixture_generated ? "NOT READY / FAILED"
                : saved_source != 2 || fixture.state != VCO_FIXTURE_READY ||
                        generated_generation != fixture.generation
                    ? "STALE - GENERATE"
                    : "OK",
                (unsigned)generated_generation, (unsigned long long)observer.samples,
                observer.generations, observer.reused_data, observer.reused_function,
                (unsigned)observer.error, (unsigned)observer.first_failure.generation,
                (unsigned)observer.first_failure.uid, (unsigned)observer.expected_data,
                (unsigned)observer.expected_function, (unsigned)shown->instruction,
                (unsigned)shown->return_instruction, shown->called ? "YES" : "NO",
                observations_saved, observation_error);
        }
        sceKernelDelayThread(16667);
    }
}
