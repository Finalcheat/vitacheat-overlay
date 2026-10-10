#include "platform/foreground_probe.h"
#include <psp2/appmgr.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { SELECTOR, PID, TITLE };
typedef struct {
    int kind, argument, result;
    char title[32];
} Step;
static Step steps[10];
static unsigned cursor;
static Step *take(int kind, int argument)
{
    assert(cursor < 10);
    Step *s = &steps[cursor++];
    assert(s->kind == kind && s->argument == argument);
    return s;
}
int32_t sceAppMgrGetAppIdByAppId(int32_t id)
{
    return take(SELECTOR, id)->result;
}
SceUID sceAppMgrGetProcessIdByAppIdForShell(SceInt32 id)
{
    return take(PID, id)->result;
}
int sceAppMgrGetNameById(SceUID pid, char *name)
{
    Step *s = take(TITLE, pid);
    memcpy(name, s->title, sizeof(s->title));
    return s->result;
}
static void setup(int foreground)
{
    memset(steps, 0, sizeof(steps));
    cursor = 0;
    for (unsigned i = 0; i < 10; i += 5) {
        steps[i] = (Step){.kind = SELECTOR, .argument = -4, .result = foreground ? 22 : 11};
        steps[i + 1] = (Step){.kind = SELECTOR, .argument = -3, .result = 22};
        steps[i + 2] =
            (Step){.kind = PID, .argument = foreground ? 22 : 11, .result = foreground ? 220 : 110};
        steps[i + 3] = (Step){.kind = PID, .argument = 22, .result = 220};
        steps[i + 4] = (Step){.kind = TITLE, .argument = 220, .title = "PCSB00031"};
    }
}
static void unknown(const VcoForegroundSample *s, uint32_t failure)
{
    VcoForegroundSample expected = {
        .abi = VCO_FOREGROUND_PROBE_ABI, .size = sizeof(*s), .failure = failure};
    assert(!memcmp(s, &expected, sizeof(*s)));
}
static void transitions(void)
{
    assert(sizeof(VcoForegroundSample) == 48 && offsetof(VcoForegroundSample, game_title_id) == 32);
    VcoForegroundSample sample;
    for (unsigned round = 0; round < 4; ++round) {
        setup(round & 1);
        vco_foreground_sample(&sample);
        assert(cursor == 10 && sample.abi == VCO_FOREGROUND_PROBE_ABI &&
               sample.size == sizeof(sample) && !sample.failure);
        assert(sample.status == (round & 1 ? VCO_FOREGROUND_CANDIDATE : VCO_BACKGROUND_CANDIDATE));
        assert(sample.game_app_id == 22 && sample.game_pid == 220);
        assert(sample.active_app_id == (round & 1 ? 22 : 11));
        assert(sample.active_pid == (round & 1 ? 220 : 110));
        assert(!strcmp(sample.game_title_id, "PCSB00031"));
        assert(!memcmp(sample.reserved, (uint8_t[6]){0}, sizeof(sample.reserved)));
        setup(0);
        steps[0].result = -1; /* Shell/no active app is ambiguous: never ABSENT. */
        vco_foreground_sample(&sample);
        unknown(&sample, VCO_FOREGROUND_FAILURE_QUERY);
    }
    cursor = 0;
    vco_foreground_sample(NULL);
    assert(!cursor);
}
static void faults(void)
{
    VcoForegroundSample sample;
    for (unsigned i = 0; i < 10; ++i) {
        setup(0);
        memset(&sample, 0xcc, sizeof(sample));
        steps[i].result = -123;
        vco_foreground_sample(&sample);
        unknown(&sample, VCO_FOREGROUND_FAILURE_QUERY);
        assert(cursor == i + 1);
        if (i % 5 != 4) {
            setup(0);
            steps[i].result = 0;
            vco_foreground_sample(&sample);
            unknown(&sample, VCO_FOREGROUND_FAILURE_QUERY);
        }
    }
    const char *bad[] = {"", "PCSB0003", "PCSB000311", "NPXS00031", "pcsb00031", "PCSB/0031"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        for (unsigned step = 4; step <= 9; step += 5) {
            setup(0);
            memset(steps[step].title, 0, sizeof(steps[step].title));
            memcpy(steps[step].title, bad[i], strlen(bad[i]));
            vco_foreground_sample(&sample);
            unknown(&sample, VCO_FOREGROUND_FAILURE_IDENTITY);
        }
    }
    setup(0);
    memset(steps[4].title, 'A', sizeof(steps[4].title));
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_IDENTITY);
}
static void races(void)
{
    VcoForegroundSample sample;
    /* Active application changed between acquisitions. */
    setup(0);
    steps[5].result = 33;
    steps[7].argument = 33;
    steps[7].result = 330;
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_CHANGED);
    /* Game application ID changed, even if PID/name stayed the same. */
    setup(0);
    steps[6].result = 33;
    steps[8].argument = 33;
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_CHANGED);
    /* Stable app IDs do not hide a different process or different metadata. */
    setup(0);
    steps[7].result = 111;
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_CHANGED);
    setup(0);
    steps[8].result = 221;
    steps[9].argument = 221;
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_CHANGED);
    setup(0);
    memcpy(steps[9].title, "PCSB00032", 10);
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_CHANGED);
    /* Same app ID with different PIDs (or converse) is not a background fact. */
    setup(1);
    steps[3].result = 221;
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_IDENTITY);
    setup(0);
    steps[2].result = 220;
    vco_foreground_sample(&sample);
    unknown(&sample, VCO_FOREGROUND_FAILURE_IDENTITY);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "transitions"))
        transitions();
    else if (!strcmp(argv[1], "faults"))
        faults();
    else if (!strcmp(argv[1], "races"))
        races();
    else
        return 1;
    puts("foreground probe tests passed");
    return 0;
}
