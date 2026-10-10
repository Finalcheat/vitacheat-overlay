/* Test-only transport for the optional original-Z06 differential runner.
 * Interprets no cheat instructions: all parsing/compilation/execution is VCO.
 * Stream words are little-endian; buffers and fake process memory are bounded. */
#include "cheats.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE UINT32_C(0x81000000)
#define MEMORY_BYTES 4096u
#define CODE_LIMIT 200u
#define TEXT_LIMIT 16384u
#define SESSION UINT64_C(7)
static uint8_t memory[MEMORY_BYTES];
static uint32_t buttons, next_patch;
static struct {
    uint32_t address;
    uint8_t saved[4], width;
    int active;
} injected[CODE_LIMIT];
static void invalid(const char *message)
{
    fprintf(stderr, "differential stream: %s\n", message);
    exit(2);
}
static void input(void *out, size_t size)
{
    if (fread(out, 1, size, stdin) != size)
        invalid("truncated input");
}
static uint32_t word(void)
{
    uint8_t bytes[4];
    input(bytes, 4);
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}
static void output(const void *data, size_t size)
{
    if (fwrite(data, 1, size, stdout) != size)
        invalid("output failed");
}
static void emit(uint32_t value)
{
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(value >> (8 * i));
    output(bytes, 4);
}
static uint8_t *range(uint32_t address, size_t size)
{
    if (!size || size > 4 || address < BASE ||
        (uint64_t)address + size > (uint64_t)BASE + MEMORY_BYTES)
        return NULL;
    return memory + (address - BASE);
}
static int alive(void *context, uint64_t session)
{
    (void)context;
    return session == SESSION;
}
static int read_memory(void *context, uint64_t session, uint32_t address, uint8_t *out, size_t size)
{
    uint8_t *source = range(address, size);
    if (!alive(context, session) || !source)
        return -1;
    memcpy(out, source, size);
    return 0;
}
static int write_memory(void *context, uint64_t session, uint32_t address, const uint8_t *in,
                        size_t size)
{
    uint8_t *destination = range(address, size);
    if (!alive(context, session) || !destination)
        return -1;
    memcpy(destination, in, size);
    return 0;
}
static int module(void *context, uint64_t session, unsigned index, unsigned segment, uint32_t *base,
                  uint32_t *size)
{
    if (!alive(context, session) || index != 0 || segment > 1)
        return -1;
    *base = BASE + 2048 * segment;
    *size = 2048;
    return 0;
}
static int pad(void *context, uint64_t session, uint32_t *device, uint32_t *pressed)
{
    if (!alive(context, session))
        return -1;
    *device = 1;
    *pressed = buttons;
    return 0;
}
static int inject(void *context, uint64_t session, uint32_t address, const uint8_t *in, size_t size,
                  int32_t *handle)
{
    uint8_t *destination = range(address, size);
    if (!alive(context, session) || !destination || next_patch == CODE_LIMIT)
        return -1;
    unsigned index = next_patch++;
    injected[index].address = address;
    injected[index].width = (uint8_t)size;
    injected[index].active = 1;
    memcpy(injected[index].saved, destination, size);
    memcpy(destination, in, size);
    *handle = (int32_t)index + 1;
    return 0;
}
static int release(void *context, uint64_t session, int32_t handle)
{
    if (!alive(context, session) || handle <= 0 || (uint32_t)handle > next_patch ||
        !injected[handle - 1].active)
        return -1;
    unsigned index = (unsigned)handle - 1;
    uint8_t *destination = range(injected[index].address, injected[index].width);
    injected[index].active = 0;
    if (!destination)
        return -1;
    memcpy(destination, injected[index].saved, injected[index].width);
    return 0;
}
int main(void)
{
    if (word() != UINT32_C(0x31444356)) /* VCD1 */
        invalid("wrong input version");
    uint32_t cases = word();
    if (!cases || cases > 4096)
        invalid("case count out of range");
    emit(UINT32_C(0x31524356)); /* VCR1 */
    emit(cases);
    for (uint32_t c = 0; c < cases; ++c) {
        uint32_t length = word(), ticks = word();
        if (!length || length > TEXT_LIMIT || !ticks || ticks > 8)
            invalid("case size out of range");
        char text[TEXT_LIMIT];
        input(text, length);
        input(memory, sizeof(memory));
        next_patch = 0;
        memset(injected, 0, sizeof(injected));
        VcoCheatEntry entry[1];
        VcoCode codes[CODE_LIMIT];
        char names[TEXT_LIMIT];
        VcoCheatFile file = {entry, 1, 0, codes, CODE_LIMIT, 0, names, sizeof(names), 0};
        VcoCheatOp ops[CODE_LIMIT];
        VcoPatch receipts[CODE_LIMIT];
        VcoCheatProgram program = {ops, CODE_LIMIT, 0, NULL, 0};
        VcoCheatRuntime runtime = {0};
        VcoDiagnostic diagnostic = {0};
        VcoCheatMemory backend = {NULL,   alive, read_memory, write_memory,
                                  module, pad,   inject,      release};
        VcoError error = vco_cheat_parse(text, length, &file, &diagnostic);
        if (!error)
            error = vco_cheat_compile(&file, 0, &program, &diagnostic);
        if (!error)
            error = vco_cheat_runtime_init(&runtime, &program, backend, SESSION, receipts,
                                           CODE_LIMIT, 10000, 0);
        VcoError preparation = error;
        emit(ticks);
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            buttons = word();
            uint32_t changes = word();
            if (changes > MEMORY_BYTES / 4)
                invalid("too many memory changes");
            for (uint32_t i = 0; i < changes; ++i) {
                uint32_t address = word(), value = word();
                uint8_t *destination = range(address, 4);
                if (!destination)
                    invalid("memory change out of range");
                for (unsigned j = 0; j < 4; ++j)
                    destination[j] = (uint8_t)(value >> (8 * j));
            }
            if (!preparation)
                error = tick ? vco_cheat_tick(&runtime) : vco_cheat_enable(&runtime);
            unsigned live = 0;
            for (unsigned i = 0; i < next_patch; ++i)
                live += injected[i].active != 0;
            emit(error);
            emit(preparation ? VCO_CHEAT_ERROR : runtime.state);
            emit(preparation ? diagnostic.line : runtime.diagnostic.line);
            emit(live);
            output(memory, sizeof(memory));
        }
        if (!preparation)
            (void)vco_cheat_disable(&runtime);
    }
    if (fgetc(stdin) != EOF || ferror(stdin))
        invalid("trailing input or read failure");
    return fflush(stdout) == 0 ? 0 : 2;
}
