#include "pad.h"
#include <psp2kern/ctrl.h>
#include <string.h>
typedef char topology_size_check
    [sizeof(((VcoVitaPad *)0)->ports) == sizeof(((SceCtrlPortInfo *)0)->port) ? 1 : -1];

#define PAD_BUTTONS                                                                                \
    (SCE_CTRL_SELECT | SCE_CTRL_START | SCE_CTRL_UP | SCE_CTRL_RIGHT | SCE_CTRL_DOWN |             \
     SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | \
     SCE_CTRL_CROSS | SCE_CTRL_SQUARE)
#define SYSTEM_BUTTONS                                                                             \
    (SCE_CTRL_PSBUTTON | SCE_CTRL_HEADPHONE | SCE_CTRL_VOLUP | SCE_CTRL_VOLDOWN | SCE_CTRL_POWER)

static void disarm(VcoVitaPad *pad)
{
    pad->armed = false;
    pad->released = false;
}
static int fail(VcoVitaPad *pad)
{
    if (pad)
        disarm(pad);
    return -1;
}
bool vco_vita_pad_init(VcoVitaPad *pad, const VcoMemoryAdapter *data,
                       VcoVitaPadSuppression suppression, void *context)
{
    if (!pad)
        return false;
    memset(pad, 0, sizeof(*pad));
    if (!data || !data->session || !data->transport.validate || !suppression)
        return false;
    pad->data = data;
    pad->suppression = suppression;
    pad->suppression_context = context;
    return true;
}
static int authorized(VcoVitaPad *pad, uint64_t token, const VcoIdentity *identity)
{
    const VcoMemoryAdapter *data = pad->data;
    if (!vco_session_matches(data->session, token, true))
        return VCO_ACCESS_SESSION;
    int result = data->transport.validate(data->transport.context, identity, true);
    if (result)
        return result;
    return vco_session_matches(data->session, token, true) &&
                   data->session->identity.pid == identity->pid &&
                   data->session->identity.instance == identity->instance &&
                   !memcmp(data->session->identity.title_id, identity->title_id,
                           sizeof(identity->title_id))
               ? 0
               : VCO_ACCESS_SESSION;
}
static int capture(VcoVitaPad *pad, uint32_t *out)
{
    uint32_t epoch = 0;
    int active = pad->suppression(pad->suppression_context, &epoch);
    if (active != 0 && active != 1)
        return -1;
    if (pad->epoch_known && epoch < pad->epoch) {
        pad->poisoned = true;
        return -1;
    }
    if (!pad->epoch_known || epoch != pad->epoch || active)
        disarm(pad);
    pad->epoch = epoch;
    pad->epoch_known = true;
    *out = epoch;
    return active;
}
static bool known_topology(const SceCtrlPortInfo *info)
{
    for (unsigned i = 0; i < sizeof(info->port); ++i)
        if (info->port[i] != SCE_CTRL_TYPE_UNPAIRED && info->port[i] != SCE_CTRL_TYPE_PHY &&
            info->port[i] != SCE_CTRL_TYPE_VIRT && info->port[i] != SCE_CTRL_TYPE_DS3 &&
            info->port[i] != SCE_CTRL_TYPE_DS4)
            return false;
    return true;
}
int vco_vita_pad_read(void *context, uint64_t token, uint32_t *device, uint32_t *buttons)
{
    VcoVitaPad *pad = context;
    if (!pad || !device || !buttons || device == buttons || !pad->data || !pad->data->session ||
        !pad->suppression || pad->poisoned)
        return fail(pad);
    VcoIdentity identity = pad->data->session->identity;
    int checked = authorized(pad, token, &identity);
    if (checked) {
        disarm(pad);
        return checked;
    }
    if (pad->token != token) {
        disarm(pad);
        pad->token = token;
    }
    uint32_t before_epoch = 0, after_epoch = 0;
    int before = capture(pad, &before_epoch);
    if (before < 0)
        return fail(pad);
    SceCtrlData sample = {0};
    SceCtrlPortInfo first = {0};
    if (!before) {
        SceCtrlPortInfo second = {0};
        if (ksceCtrlGetControllerPortInfo(&first) || !known_topology(&first) ||
            ksceCtrlPeekBufferPositive(0, &sample, 1) != 1 ||
            ksceCtrlGetControllerPortInfo(&second) || !known_topology(&second) ||
            memcmp(first.port, second.port, sizeof(first.port)))
            return fail(pad);
    }
    int after = capture(pad, &after_epoch);
    if (after < 0)
        return fail(pad);
    checked = authorized(pad, token, &identity);
    if (checked) {
        disarm(pad);
        return checked;
    }
    if (before || after || before_epoch != after_epoch) {
        disarm(pad);
        return VCO_PAD_SUPPRESSED;
    }
    if (!pad->topology_known || memcmp(pad->ports, first.port, sizeof(first.port))) {
        disarm(pad);
        memcpy(pad->ports, first.port, sizeof(first.port));
        pad->topology_known = true;
    }
    if (sample.buttons & SCE_CTRL_PSBUTTON) {
        disarm(pad);
        return VCO_PAD_SUPPRESSED;
    }
    /* Do not reinterpret Ext/Ext2 or remapper-only bits as the documented Vita
     * trigger layout. System status flags are not cheat-condition buttons. */
    if (sample.buttons & ~(PAD_BUTTONS | SYSTEM_BUTTONS))
        return fail(pad);
    uint32_t digital = sample.buttons & PAD_BUTTONS;
    if (!pad->armed) {
        if (!pad->released) {
            if (!digital) {
                pad->released = true;
                pad->release_stamp = sample.timeStamp;
            }
            return VCO_PAD_SUPPRESSED;
        }
        if (!digital || sample.timeStamp <= pad->release_stamp)
            return VCO_PAD_SUPPRESSED;
        pad->armed = true;
    }
    *device = first.port[0];
    *buttons = digital;
    return 0;
}
