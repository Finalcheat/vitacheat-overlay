#include "fixture_observer.h"
#include <limits.h>
#include <string.h>

static bool ready(const VcoFixtureController *c)
{
    return c && c->state == VCO_FIXTURE_READY && c->uid > 0 && c->generation &&
           c->info.uid == c->uid && c->reply.module_uid == c->uid &&
           c->reply.generation == c->generation && c->reply.variant <= 1 &&
           c->reply.data_initial == 5 + c->reply.variant &&
           c->reply.function_initial == (c->reply.variant ? 11u : 7u) && c->reply.data_address &&
           !(c->reply.data_address & 3u) && (c->reply.function_address & 1u);
}
void vco_fixture_observer_init(VcoFixtureObserver *o)
{
    if (o)
        memset(o, 0, sizeof(*o));
}
int vco_fixture_observer_begin(VcoFixtureObserver *o, const VcoFixtureController *c)
{
    if (!o || !c || o->active || o->error ||
        (!ready(c) && !(c->state == VCO_FIXTURE_EMPTY && c->uid == -1)))
        return -1;
    memset(o, 0, sizeof(*o));
    o->active = true;
    if (ready(c)) {
        o->origin_generation = o->last_generation = c->generation;
        o->origin_data = c->reply.data_address;
        o->origin_function = c->reply.function_address;
    }
    return 0;
}
void vco_fixture_observer_stop(VcoFixtureObserver *o)
{
    if (o)
        o->active = false;
}
static VcoFixtureObserveError content(const VcoFixtureObserver *o, const VcoFixtureController *c,
                                      const VcoFixtureReading *r)
{
    if (!o || !r || !ready(c) || r->generation != c->generation || r->uid != c->uid ||
        r->data_address != c->reply.data_address ||
        r->function_address != c->reply.function_address ||
        (o->active && r->generation < o->last_generation))
        return VCO_FIXTURE_OBSERVE_IDENTITY;
    bool newer = o->active && r->generation != o->origin_generation;
    if (r->data != c->reply.data_initial && (newer || r->data != 88))
        return VCO_FIXTURE_OBSERVE_DATA;
    uint16_t initial = (uint16_t)(0x2000u | c->reply.function_initial);
    if (r->return_instruction != 0x4770 ||
        (r->instruction != initial && (newer || r->instruction != 0x2009)))
        return VCO_FIXTURE_OBSERVE_CODE;
    return VCO_FIXTURE_OBSERVE_NONE;
}
bool vco_fixture_observer_callable(const VcoFixtureObserver *o, const VcoFixtureController *c,
                                   const VcoFixtureReading *r)
{
    return o && !o->error && content(o, c, r) == VCO_FIXTURE_OBSERVE_NONE;
}
static int fail(VcoFixtureObserver *o, const VcoFixtureController *c, const VcoFixtureReading *r,
                VcoFixtureObserveError error)
{
    o->first_failure = *r;
    o->expected_data = ready(c) ? c->reply.data_initial : 0;
    o->expected_function = ready(c) ? c->reply.function_initial : 0;
    o->error = error;
    o->active = false;
    return -1;
}
int vco_fixture_observer_sample(VcoFixtureObserver *o, const VcoFixtureController *c,
                                const VcoFixtureReading *r)
{
    if (!o || !r || !o->active || o->error)
        return -1;
    VcoFixtureObserveError error = content(o, c, r);
    if (error)
        return fail(o, c, r, error);
    bool newer = r->generation != o->origin_generation;
    /* A legitimate toggle can happen between opcode reading and the call.
     * On the initial instance allow either known return; never demand that
     * the two separate observations describe one atomic toggle. */
    if (!r->called ||
        (r->function != (int32_t)c->reply.function_initial && (newer || r->function != 9)))
        return fail(o, c, r, VCO_FIXTURE_OBSERVE_RETURN);
    if (o->samples == UINT64_MAX ||
        (r->generation != o->last_generation && o->generations == UINT_MAX))
        return fail(o, c, r, VCO_FIXTURE_OBSERVE_LIMIT);
    ++o->samples;
    if (r->generation != o->last_generation) {
        ++o->generations;
        if (!o->origin_data) {
            /* An EMPTY start has no old address. Keep the first new instance
             * as the reuse reference without permitting patches on it. */
            o->origin_data = r->data_address;
            o->origin_function = r->function_address;
        } else {
            if (r->data_address == o->origin_data)
                ++o->reused_data;
            if (r->function_address == o->origin_function)
                ++o->reused_function;
        }
        o->last_generation = r->generation;
    }
    return 0;
}
