#include "fixture.h"
#include "codes.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int fault(VcoFixtureController *c, VcoFixtureError error)
{
    memset(&c->reply, 0, sizeof(c->reply));
    memset(&c->info, 0, sizeof(c->info));
    c->state = VCO_FIXTURE_FAULT;
    c->error = error;
    return -1;
}
void vco_fixture_init(VcoFixtureController *c, VcoFixtureOps ops)
{
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    c->uid = -1;
}
static int contains(VcoFixtureSegment s, uint32_t address, uint32_t width, uint32_t permissions)
{
    return s.base >= 0x40000000u && s.size && (uint64_t)s.base + s.size <= UINT64_C(0x100000000) &&
           address >= s.base && (uint64_t)address + width <= (uint64_t)s.base + s.size &&
           (s.permissions & 7u) == permissions;
}
static int valid(const VcoFixtureController *c)
{
    const VcoFixtureReply *r = &c->reply;
    return c->next_variant <= 1 && c->uid > 0 && c->info.uid == c->uid && r->module_uid == c->uid &&
           c->info.state == 6 && c->info.index <= 255 && r->abi == VCO_FIXTURE_ABI &&
           r->size == sizeof(*r) && r->generation == c->generation &&
           r->variant == c->next_variant && r->data_initial == 5 + c->next_variant &&
           r->function_initial == (c->next_variant ? 11u : 7u) && !(r->data_address & 3u) &&
           (r->function_address & 1u) && contains(c->info.text, r->function_address & ~1u, 4, 5) &&
           contains(c->info.data, r->data_address, 4, 6);
}
int vco_fixture_load(VcoFixtureController *c)
{
    if (!c || c->state != VCO_FIXTURE_EMPTY || c->uid >= 0 || !c->ops.load || !c->ops.start ||
        !c->ops.info || !c->ops.stop_unload || c->generation == UINT32_MAX ||
        c->loads == UINT_MAX || c->next_variant > 1)
        return -1;
    c->state = VCO_FIXTURE_LOADING;
    c->error = VCO_FIXTURE_ERROR_NONE;
    ++c->generation; /* Never reuse a generation after a failed start. */
    c->uid = c->ops.load(c->ops.context, c->next_variant);
    c->native_result = c->uid;
    c->module_status = INT32_MIN;
    if (c->uid <= 0)
        return fault(c, VCO_FIXTURE_ERROR_LOAD);
    VcoFixtureStart start = {VCO_FIXTURE_ABI, sizeof(start), c->generation, c->next_variant,
                             &c->reply};
    c->native_result = c->ops.start(c->ops.context, c->uid, &start, &c->module_status);
    if (c->native_result < 0 || c->module_status != 0)
        return fault(c, VCO_FIXTURE_ERROR_START);
    c->native_result = c->ops.info(c->ops.context, c->uid, &c->info);
    if (c->native_result < 0)
        return fault(c, VCO_FIXTURE_ERROR_INFO);
    if (!valid(c))
        return fault(c, VCO_FIXTURE_ERROR_CONTRACT);
    ++c->loads;
    c->state = VCO_FIXTURE_READY;
    return 0;
}
int vco_fixture_unload(VcoFixtureController *c)
{
    if (!c || c->state != VCO_FIXTURE_READY || c->uid <= 0 || c->unloads == UINT_MAX)
        return -1;
    c->state = VCO_FIXTURE_UNLOADING;
    /* Discard callable addresses before entering native stop/unload. Even a
     * negative return may have consumed the module and must not be retried. */
    memset(&c->reply, 0, sizeof(c->reply));
    memset(&c->info, 0, sizeof(c->info));
    c->module_status = INT32_MIN;
    c->native_result = c->ops.stop_unload(c->ops.context, c->uid, &c->module_status);
    if (c->native_result < 0 || c->module_status != 0)
        return fault(c, VCO_FIXTURE_ERROR_UNLOAD);
    ++c->unloads;
    c->uid = -1;
    c->next_variant ^= 1u;
    c->state = VCO_FIXTURE_EMPTY;
    return 0;
}
int vco_fixture_step(VcoFixtureController *c)
{
    if (!c)
        return -1;
    return c->state == VCO_FIXTURE_READY ? vco_fixture_unload(c) : vco_fixture_load(c);
}
int vco_fixture_codes(const VcoFixtureController *c, char *out, size_t size)
{
    if (out && size)
        *out = 0;
    /* A loaded variant remains fixed until successful unload flips it. */
    if (!c || c->state != VCO_FIXTURE_READY || !out || !size || !valid(c))
        return -1;
    int n = snprintf(
        out, size,
        "# Generated dynamic fixture; valid only for generation %u, UID %08X.\n"
        "# Title ID: VCOCH0001\n# Title: VCO Device Harness\n# Version: " VCO_HARNESS_VERSION "\n"
        "_V0 D01 Module data A200 - ON 88 / OFF %u\n$A200 %08X 00000058\n"
        "_V0 D02 Module RX A100 - ON 9 / OFF %u\n$A100 %08X 00002009\n"
        "_V0 D03 Module B+A100 - ON 9 / OFF %u\n$B2%02X 00000000 00000000\n$A100 %08X 00002009\n",
        (unsigned)c->generation, (unsigned)c->uid, (unsigned)c->reply.data_initial,
        (unsigned)c->reply.data_address, (unsigned)c->reply.function_initial,
        (unsigned)(c->reply.function_address & ~1u), (unsigned)c->reply.function_initial,
        (unsigned)c->info.index, (unsigned)((c->reply.function_address & ~1u) - c->info.text.base));
    if (n < 0 || (size_t)n >= size) {
        *out = 0;
        return -1;
    }
    return n;
}
