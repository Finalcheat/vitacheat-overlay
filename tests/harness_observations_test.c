#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "../tools/device-harness/observations.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static VcoHarnessObservation sample(void)
{
    VcoHarnessObservation o = {0};
    o.run = 1000;
    o.tick = 1200;
    o.sequence = 1;
    o.pid = 37;
    o.source = 1;
    o.fixture_generated = -1;
    o.rx[0] = 0x2007;
    o.rx[1] = 0x4770;
    o.thumb32[0] = 0xbf00;
    o.thumb32[1] = 0xf04f;
    o.thumb32[2] = 7;
    o.thumb32[3] = 0x4770;
    o.rx_result = o.thumb32_result = 7;
    o.bulk[0] = 117;
    o.fixture.uid = -1;
    o.reading.function = -1;
    return o;
}
static void bounds(void)
{
    char output[VCO_HARNESS_OBSERVATION_BYTES];
    VcoHarnessObservation o = sample();
    int length = vco_harness_observation(output, sizeof(output), &o);
    assert(length > 0 && output[length - 1] == '\n');
    assert((size_t)length == strlen(output));
    for (unsigned capacity = 1; capacity <= (unsigned)length; ++capacity) {
        memset(output, '?', sizeof(output));
        assert(vco_harness_observation(output, capacity, &o) == -1);
        assert(!output[0] && output[capacity] == '?');
    }
    assert(vco_harness_observation(NULL, 0, &o) == -1);
    assert(vco_harness_observation(output, sizeof(output), NULL) == -1 && !output[0]);
    const VcoHarnessObservation original = o;
    for (unsigned invalid = 0; invalid < 11; ++invalid) {
        o = original;
        switch (invalid) {
        case 0:
            o.run = 0;
            break;
        case 1:
            o.tick = o.run - 1;
            break;
        case 2:
            o.pid = -1;
            break;
        case 3:
            o.sequence = 0;
            break;
        case 4:
            o.sequence = 61;
            break;
        case 5:
            o.page = 2;
            break;
        case 6:
            o.source = 3;
            break;
        case 7:
            o.witness.error = VCO_FIXTURE_OBSERVE_LIMIT + 1;
            break;
        case 8:
            o.fixture.state = (VcoFixtureState)-1;
            break;
        case 9:
            o.fixture.error = VCO_FIXTURE_ERROR_UNLOAD + 1;
            break;
        default:
            o.witness.error = (VcoFixtureObserveError)-1;
            break;
        }
        assert(vco_harness_observation(output, sizeof(output), &o) == -1 && !output[0]);
    }
}
static void io(void)
{
    char directory[] = "/tmp/vco-harness-observations-XXXXXX", path[160], buffer[2048];
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/log.jsonl", directory);
    VcoHarnessObservation o = sample();
    int length = vco_harness_observation(buffer, sizeof(buffer), &o);
    assert(length > 0);
    assert(!vco_harness_observation_save(path, &o));
    ++o.sequence;
    ++o.tick;
    assert(!vco_harness_observation_save(path, &o));
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long before = ftell(file);
    assert(before > length && !fclose(file));
    file = fopen(path, "wb");
    assert(file && fputs("{", file) >= 0 && !fclose(file));
    assert(vco_harness_observation_save(path, &o) == -1);
    file = fopen(path, "rb");
    assert(file && fgetc(file) == '{' && fgetc(file) == EOF && !fclose(file));
    file = fopen(path, "wb");
    assert(file);
    int current_length = vco_harness_observation(buffer, sizeof(buffer), &o);
    for (unsigned i = 0; i < VCO_HARNESS_LOG_LIMIT - (unsigned)current_length; ++i)
        assert(fputc('\n', file) != EOF);
    assert(!fclose(file));
    assert(!vco_harness_observation_save(path, &o));
    assert(vco_harness_observation_save(path, &o) == -1);
    file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    assert(ftell(file) == VCO_HARNESS_LOG_LIMIT && !fclose(file));
    assert(!unlink(path));
    o.sequence = 0;
    assert(vco_harness_observation_save(path, &o) == -1 && access(path, F_OK));
    assert(!rmdir(directory));
    assert(vco_harness_observation_save(NULL, &o) == -1);
}
static void emit(bool extremes)
{
    VcoHarnessObservation o = sample();
    if (extremes) {
        for (unsigned i = 0; i < VCO_HARNESS_DATA_VALUES; ++i)
            o.data[i] = UINT32_MAX;
        o.run = UINT64_MAX - 1;
        o.tick = UINT64_MAX;
        o.pid = INT32_MAX;
        o.fixture.native_result = INT32_MIN;
        o.fixture.module_status = INT32_MIN;
        o.fixture.uid = INT32_MIN;
        o.fixture.state = VCO_FIXTURE_FAULT;
        o.fixture.error = VCO_FIXTURE_ERROR_UNLOAD;
        o.fixture.generation = o.fixture.loads = o.fixture.unloads = UINT32_MAX;
        o.fixture.reply.variant = o.fixture.info.index = UINT32_MAX;
        o.fixture.reply.data_address = o.fixture.reply.function_address = UINT32_MAX;
        o.reading.data = UINT32_MAX;
        o.reading.function = INT32_MIN;
        o.reading.instruction = o.reading.return_instruction = UINT16_MAX;
        o.reading.called = true;
        for (unsigned i = 0; i < 3; ++i)
            o.bulk[i] = UINT32_MAX;
        for (unsigned i = 0; i < 2; ++i)
            o.rx[i] = UINT16_MAX;
        for (unsigned i = 0; i < 4; ++i)
            o.thumb32[i] = UINT16_MAX;
        o.witness.error = VCO_FIXTURE_OBSERVE_RETURN;
        o.witness.samples = UINT64_MAX;
        o.witness.active = true;
        o.witness.generations = o.witness.reused_data = o.witness.reused_function = UINT32_MAX;
        o.witness.origin_generation = o.witness.origin_data = o.witness.origin_function =
            UINT32_MAX;
        o.witness.first_failure.generation = o.witness.first_failure.data = UINT32_MAX;
        o.witness.first_failure.data_address = o.witness.first_failure.function_address =
            UINT32_MAX;
        o.witness.first_failure.instruction = o.witness.first_failure.return_instruction =
            UINT16_MAX;
        o.witness.first_failure.called = true;
        o.witness.first_failure.uid = INT32_MIN;
        o.witness.first_failure.function = -1;
        o.witness.expected_data = UINT32_MAX;
        o.witness.expected_function = UINT32_MAX;
    }
    char output[VCO_HARNESS_OBSERVATION_BYTES];
    assert(vco_harness_observation(output, sizeof(output), &o) > 0);
    fputs(output, stdout);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "bounds"))
        bounds();
    else if (!strcmp(argv[1], "io"))
        io();
    else if (!strcmp(argv[1], "emit"))
        emit(false);
    else if (!strcmp(argv[1], "extremes"))
        emit(true);
    else
        assert(false);
    return 0;
}
