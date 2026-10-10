#include "session.h"
#include <string.h>
void vco_session_init(VcoSession *s)
{
    memset(s, 0, sizeof(*s));
}
bool vco_identity_valid(const VcoIdentity *id)
{
    if (!id || id->pid <= 0 || !id->instance || id->title_id[9] || !memcmp(id->title_id, "NPXS", 4))
        return false;
    for (unsigned i = 0; i < 9; ++i)
        if (!((id->title_id[i] >= 'A' && id->title_id[i] <= 'Z') ||
              (id->title_id[i] >= '0' && id->title_id[i] <= '9')))
            return false;
    return true;
}
bool vco_session_observe(VcoSession *s, VcoObservation observation, const VcoIdentity *id)
{
    if (!s)
        return false;
    bool was_runnable = s->runnable;
    s->runnable = false;
    if (observation == VCO_OBSERVATION_UNKNOWN)
        return was_runnable;
    if (observation == VCO_OBSERVATION_ABSENT) {
        bool changed = s->present;
        s->present = false;
        s->token = 0;
        memset(&s->identity, 0, sizeof(s->identity));
        return changed;
    }
    if ((observation != VCO_OBSERVATION_BACKGROUND && observation != VCO_OBSERVATION_FOREGROUND) ||
        !vco_identity_valid(id))
        return was_runnable;
    bool changed = !s->present || s->identity.pid != id->pid ||
                   s->identity.instance != id->instance ||
                   memcmp(s->identity.title_id, id->title_id, 10);
    if (changed) {
        if (s->sequence == UINT64_MAX) {
            s->token = 0;
            s->present = false;
            return true;
        }
        s->token = ++s->sequence;
        s->identity = *id;
        s->present = true;
    }
    s->runnable = observation == VCO_OBSERVATION_FOREGROUND;
    return changed || was_runnable != s->runnable;
}
bool vco_session_matches(const VcoSession *s, uint64_t token, bool foreground)
{
    return s && token && s->present && s->token == token && (!foreground || s->runnable);
}
