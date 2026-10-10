#include "observations.h"
#include <stdarg.h>
#include <stdio.h>

static int append(char *out, size_t capacity, size_t *used, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *used)
        return -1;
    *used += (size_t)count;
    return 0;
}
static int words(char *out, size_t capacity, size_t *used, const char *name, const uint32_t *values,
                 unsigned count)
{
    if (append(out, capacity, used, ",\"%s\":[", name))
        return -1;
    for (unsigned i = 0; i < count; ++i)
        if (append(out, capacity, used, "%s%u", i ? "," : "", (unsigned)values[i]))
            return -1;
    return append(out, capacity, used, "]");
}
int vco_harness_observation(char *out, size_t capacity, const VcoHarnessObservation *o)
{
    if (out && capacity)
        out[0] = 0;
    if (!out || !capacity || !o || !o->run || o->pid <= 0 || o->tick < o->run || !o->sequence ||
        o->sequence > VCO_HARNESS_OBSERVATION_LIMIT || o->page > 1 || o->source > 2 ||
        (unsigned)o->fixture.state > VCO_FIXTURE_FAULT ||
        (unsigned)o->fixture.error > VCO_FIXTURE_ERROR_UNLOAD ||
        (unsigned)o->witness.error > VCO_FIXTURE_OBSERVE_LIMIT)
        return -1;
    size_t used = 0;
    uint32_t rx[2] = {o->rx[0], o->rx[1]};
    uint32_t thumb[4] = {o->thumb32[0], o->thumb32[1], o->thumb32[2], o->thumb32[3]};
    const VcoFixtureController *f = &o->fixture;
    const VcoFixtureReading *r = &o->reading;
    const VcoFixtureObserver *w = &o->witness;
    const VcoFixtureReading *fault = &w->first_failure;
    if (append(out, capacity, &used,
               "{\"schema\":1,\"kind\":\"observation\",\"version\":\"" VCO_HARNESS_VERSION
               "\",\"title_id\":\"VCOCH0001\",\"run\":\"%016llx\",\"sequence\":%u,"
               "\"tick_us\":%llu,\"pid\":%d,\"page\":%u,\"buttons\":%u,\"source\":%u,"
               "\"codes_result\":[%d,%d]",
               (unsigned long long)o->run, (unsigned)o->sequence, (unsigned long long)o->tick,
               (int)o->pid, (unsigned)o->page, (unsigned)o->buttons, (unsigned)o->source,
               (int)o->generated, (int)o->fixture_generated) ||
        words(out, capacity, &used, "data", o->data, VCO_HARNESS_DATA_VALUES) ||
        words(out, capacity, &used, "bulk", o->bulk, 3) ||
        words(out, capacity, &used, "rx", rx, 2) ||
        words(out, capacity, &used, "thumb32", thumb, 4) ||
        append(out, capacity, &used,
               ",\"cpu\":[%d,%d],\"module\":{\"state\":%u,\"uid\":%d,\"generation\":%u,"
               "\"variant\":%u,\"loads\":%u,\"unloads\":%u,\"error\":%u,\"native\":%d,"
               "\"status\":%d,\"index\":%u,\"data_address\":%u,\"function_address\":%u,"
               "\"reading\":[%u,%d,%u,%u,%u]},\"witness\":{\"active\":%u,\"samples\":%llu,"
               "\"generations\":%u,\"reused_data\":%u,\"reused_function\":%u,\"error\":%u,"
               "\"first\":[%u,%d,%u,%d,%u,%u,%u],\"first_address\":[%u,%u],"
               "\"origin\":[%u,%u,%u],\"expected\":[%u,%u]}}\n",
               (int)o->rx_result, (int)o->thumb32_result, (unsigned)f->state, (int)f->uid,
               (unsigned)f->generation, (unsigned)f->reply.variant, f->loads, f->unloads,
               (unsigned)f->error, (int)f->native_result, (int)f->module_status,
               (unsigned)f->info.index, (unsigned)f->reply.data_address,
               (unsigned)f->reply.function_address, (unsigned)r->data, (int)r->function,
               (unsigned)r->instruction, (unsigned)r->return_instruction, (unsigned)r->called,
               (unsigned)w->active, (unsigned long long)w->samples, w->generations, w->reused_data,
               w->reused_function, (unsigned)w->error, (unsigned)fault->generation, (int)fault->uid,
               (unsigned)fault->data, (int)fault->function, (unsigned)fault->instruction,
               (unsigned)fault->return_instruction, (unsigned)fault->called,
               (unsigned)fault->data_address, (unsigned)fault->function_address,
               (unsigned)w->origin_generation, (unsigned)w->origin_data,
               (unsigned)w->origin_function, (unsigned)w->expected_data,
               (unsigned)w->expected_function)) {
        out[0] = 0;
        return -1;
    }
    return (int)used;
}
int vco_harness_observation_save(const char *path, const VcoHarnessObservation *o)
{
    char line[VCO_HARNESS_OBSERVATION_BYTES];
    int length = vco_harness_observation(line, sizeof(line), o);
    if (!path || !*path || length < 0)
        return -1;
    FILE *file = fopen(path, "a+b");
    if (!file)
        return -1;
    int result = -1;
    if (fseek(file, 0, SEEK_END))
        goto close;
    long end = ftell(file);
    if (end < 0 || (unsigned long)end > VCO_HARNESS_LOG_LIMIT - (unsigned)length)
        goto close;
    /* Never join a new JSON object to a truncated earlier observation. */
    if (end && (fseek(file, -1, SEEK_END) || fgetc(file) != '\n'))
        goto close;
    if (!fseek(file, 0, SEEK_END) && fwrite(line, 1, (size_t)length, file) == (size_t)length)
        result = 0;
close:
    if (fclose(file))
        result = -1;
    return result;
}
