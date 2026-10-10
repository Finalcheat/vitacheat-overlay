#include "cheats.h"
#include <string.h>

typedef struct {
    VcoCheatRuntime *runtime;
    uint32_t remaining, base, size;
    int relative;
    int pad_sampled, pad_status;
    uint32_t buttons;
} Frame;
static VcoError access_error(int result, VcoError fallback)
{
    return result == VCO_ACCESS_SESSION ? VCO_E_SESSION : result ? fallback : VCO_OK;
}
static VcoError charge(Frame *f)
{
    if (!f->remaining)
        return VCO_E_BUDGET;
    --f->remaining;
    return f->runtime->memory.alive(f->runtime->memory.context, f->runtime->session)
               ? VCO_OK
               : VCO_E_SESSION;
}
static VcoError add(uint32_t a, uint64_t b, uint32_t *out)
{
    if (b > UINT32_MAX || (uint64_t)a + b > UINT32_MAX)
        return VCO_E_ADDRESS;
    *out = a + (uint32_t)b;
    return VCO_OK;
}
/* Offsets/strides are 32-bit ARM words; negative two's-complement offsets are
 * common in pointer codes. Widen before arithmetic and reject leaving the
 * address domain instead of reproducing the original unchecked wraparound. */
static int64_t signed_word(uint32_t word)
{
    return word <= INT32_MAX ? (int64_t)word : (int64_t)word - INT64_C(0x100000000);
}
static VcoError offset(uint32_t base, int64_t delta, uint32_t *out)
{
    int64_t result = (int64_t)base + delta;
    if (result < 0 || result > UINT32_MAX)
        return VCO_E_ADDRESS;
    *out = (uint32_t)result;
    return VCO_OK;
}
static VcoError span(uint32_t address, unsigned width)
{
    return (uint64_t)address + width > UINT64_C(0x100000000) ? VCO_E_ADDRESS : VCO_OK;
}
static VcoError address(Frame *f, uint32_t raw, unsigned width, uint32_t *out)
{
    if (f->relative) {
        if ((uint64_t)raw + width > f->size)
            return VCO_E_ADDRESS;
        VcoError e = add(f->base, raw, out);
        if (e)
            return e;
    } else
        *out = raw;
    return span(*out, width);
}
static void pack(uint32_t v, uint8_t bytes[4])
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(v >> (i * 8));
}
static VcoError read_value(Frame *f, uint32_t a, unsigned width, uint32_t *v)
{
    VcoError e = span(a, width);
    if (e)
        return e;
    e = charge(f);
    if (e)
        return e;
    uint8_t bytes[4] = {0};
    VcoCheatRuntime *r = f->runtime;
    if (!r->memory.read)
        return VCO_E_IO;
    e = access_error(r->memory.read(r->memory.context, r->session, a, bytes, width), VCO_E_IO);
    if (e)
        return e;
    *v = 0;
    for (unsigned i = 0; i < width; ++i)
        *v |= (uint32_t)bytes[i] << (8 * i);
    return VCO_OK;
}
static int overlaps(uint32_t a, unsigned size, const VcoPatch *p)
{
    return p->active && (uint64_t)a < (uint64_t)p->address + p->width &&
           (uint64_t)p->address < (uint64_t)a + size;
}
static VcoError write_value(Frame *f, uint32_t a, unsigned width, uint32_t v)
{
    VcoError e = span(a, width);
    if (e)
        return e;
    e = charge(f);
    if (e)
        return e;
    VcoCheatRuntime *r = f->runtime;
    uint8_t bytes[4];
    pack(v, bytes);
    for (size_t i = 0; i < r->program->count; ++i)
        if (overlaps(a, width, &r->patches[i]))
            return VCO_E_PATCH;
    if (!r->memory.write)
        return VCO_E_IO;
    return access_error(r->memory.write(r->memory.context, r->session, a, bytes, width), VCO_E_IO);
}
/* Z06 skips dormant chains, rather than disabling their entry. Families 3/7
 * check 0x81000000..0xA0000000 at each loaded pointee and the final target;
 * family 8 checks only the lower bound. These are format semantics, never
 * authorization: an in-domain pointer still needs a successful backend access. */
static int pointer_available(const VcoCheatOp *op, uint32_t value)
{
    return value >= UINT32_C(0x81000000) &&
           (op->kind == VCO_OP_POINTER_COPY || value <= UINT32_C(0xA0000000));
}
static VcoError pointer(Frame *f, const VcoCheatOp *op, size_t chain, uint32_t iteration,
                        uint32_t *result, int *available)
{
    *available = 0;
    const VcoCode *c = f->runtime->program->codes + chain;
    uint32_t raw = c->a, a;
    int64_t delta = (int64_t)iteration * signed_word(op->address_step);
    VcoError e;
    if (op->kind == VCO_OP_POINTER_REPEAT && op->repeat_level == 0) {
        e = offset(raw, delta, &raw);
        if (e)
            return e;
    }
    e = address(f, raw, 4, &a);
    if (e)
        return e;
    for (unsigned level = 0; level < op->depth; ++level) {
        uint32_t base;
        e = read_value(f, a, 4, &base);
        if (e)
            return e;
        if (!pointer_available(op, base))
            return VCO_OK;
        int64_t off = signed_word(c[level].b);
        if (op->kind == VCO_OP_POINTER_REPEAT && op->repeat_level == level + 1)
            off += delta;
        e = offset(base, off, &a);
        if (e)
            return e;
    }
    if (!pointer_available(op, a))
        return VCO_OK;
    e = span(a, op->width);
    if (e)
        return e;
    *result = a;
    *available = 1;
    return VCO_OK;
}
static VcoError consume_patch(VcoCheatRuntime *r, VcoPatch *p)
{
    int result =
        !r->memory.release ? -1 : r->memory.release(r->memory.context, r->session, p->handle);
    /* Keep the receipt observable during the callback so an engine can retire
     * its matching claim. Consume it even if the adapter reports failure. */
    p->active = 0;
    return result ? VCO_E_RESTORE : VCO_OK;
}
static VcoError release_all(VcoCheatRuntime *r)
{
    VcoError error = VCO_OK;
    for (size_t i = r->program->count; i > 0; --i) {
        VcoPatch *p = &r->patches[i - 1];
        if (!p->active)
            continue;
        if (consume_patch(r, p))
            error = VCO_E_RESTORE;
    }
    return error;
}
static VcoError release_guarded(Frame *f, size_t first, size_t end)
{
    VcoCheatRuntime *r = f->runtime;
    VcoError error = VCO_OK;
    /* Only C's false branch releases skipped A codes in Z06. D's false branch
     * merely skips them; existing patches then live until C or disable. */
    for (size_t i = first; i < end; ++i) {
        VcoPatch *p = &r->patches[i];
        if (!p->active)
            continue;
        VcoError e = charge(f);
        if (e)
            return error ? error : e;
        if (consume_patch(r, p))
            error = VCO_E_RESTORE;
    }
    return error;
}
static VcoError patch(Frame *f, const VcoCheatOp *op, size_t index, uint32_t a)
{
    VcoCheatRuntime *r = f->runtime;
    VcoPatch *p = &r->patches[index];
    if (p->active)
        return p->address == a ? VCO_OK : VCO_E_PATCH;
    /* A patches are byte spans copied by the native injector, not scalar ARM
     * loads/stores. Thumb-2 instructions commonly start at address % 4 == 2;
     * Z06 also permits other residues. address() already checks the full span. */
    for (size_t i = 0; i < r->program->count; ++i)
        if (overlaps(a, op->width, &r->patches[i]))
            return VCO_E_PATCH;
    VcoError e = charge(f);
    if (e)
        return e;
    uint8_t bytes[4];
    int32_t handle;
    pack(op->b, bytes);
    if (!r->memory.inject || !r->memory.release)
        return VCO_E_PATCH;
    e = access_error(r->memory.inject(r->memory.context, r->session, a, bytes, op->width, &handle),
                     VCO_E_PATCH);
    if (e)
        return e;
    *p = (VcoPatch){1, handle, a, op->width};
    return VCO_OK;
}
static VcoError execute(Frame *f, const VcoCheatOp *op, size_t index, size_t *next)
{
    VcoCheatRuntime *r = f->runtime;
    VcoError e;
    uint32_t a, b, v;
    e = charge(f);
    if (e)
        return e;
    if (op->kind == VCO_OP_MODULE) {
        if (!r->memory.module)
            return VCO_E_ADDRESS;
        e = access_error(
            r->memory.module(r->memory.context, r->session, op->a, op->b, &f->base, &f->size),
            VCO_E_ADDRESS);
        if (e)
            return e;
        if (!f->size || (uint64_t)f->base + f->size > UINT64_C(0x100000000))
            return VCO_E_ADDRESS;
        f->relative = 1;
        return VCO_OK;
    }
    if (op->kind == VCO_OP_PAD) {
        if (!r->memory.pad)
            return VCO_E_IO;
        if (!f->pad_sampled) {
            f->pad_status = r->memory.pad(r->memory.context, r->session, &a, &f->buttons);
            f->pad_sampled = 1;
        }
        if (f->pad_status && f->pad_status != VCO_PAD_SUPPRESSED)
            return access_error(f->pad_status, VCO_E_IO);
        /* Z06 0xbd14 compares the complete sampled word, ignores device, and
         * 0xbd38 releases active patches in the skipped physical-line range. */
        if (f->pad_status == VCO_PAD_SUPPRESSED || f->buttons != op->b) {
            *next = op->next;
            return release_guarded(f, index + 1, op->next);
        }
        return VCO_OK;
    }
    if (op->kind == VCO_OP_COMPARE) {
        e = address(f, op->a, op->width, &a);
        if (e)
            return e;
        e = read_value(f, a, op->width, &v);
        if (e)
            return e;
        int yes = op->comparison == 0   ? v == op->b
                  : op->comparison == 1 ? v != op->b
                  : op->comparison == 2 ? v > op->b
                                        : v < op->b;
        if (!yes)
            *next = op->next;
        return VCO_OK;
    }
    for (uint32_t i = 0; i < op->count; ++i) {
        int available = 1;
        if (op->kind == VCO_OP_POINTER || op->kind == VCO_OP_POINTER_COPY ||
            op->kind == VCO_OP_POINTER_REPEAT)
            e = pointer(f, op, op->chain, i, &a, &available);
        else {
            e = offset(op->a, (int64_t)i * signed_word(op->address_step), &a);
            if (!e)
                e = address(f, a, op->width, &a);
        }
        if (e)
            return e;
        if (!available)
            continue;
        v = op->b + i * op->value_step; /* Unsigned fixed-width data arithmetic. */
        if (op->kind == VCO_OP_COPY || op->kind == VCO_OP_POINTER_COPY) {
            e = op->kind == VCO_OP_COPY ? address(f, op->b, op->width, &b)
                                        : pointer(f, op, op->source_chain, 0, &b, &available);
            if (e)
                return e;
            if (!available)
                continue;
            e = read_value(f, b, op->width, &v);
            if (e)
                return e;
        }
        e = op->kind == VCO_OP_PATCH ? patch(f, op, index, a) : write_value(f, a, op->width, v);
        if (e)
            return e;
    }
    return VCO_OK;
}
VcoError vco_cheat_runtime_init(VcoCheatRuntime *r, const VcoCheatProgram *p, VcoCheatMemory memory,
                                uint64_t session, VcoPatch *patches, size_t capacity,
                                uint32_t budget, uint32_t review_opt_in)
{
    if (!r || !p || !p->ops || !p->codes || !p->count || !patches || capacity < p->count ||
        !memory.alive || !budget || !session)
        return VCO_E_ARGUMENT;
    memset(r, 0, sizeof(*r));
    memset(patches, 0, p->count * sizeof(*patches));
    r->program = p;
    r->memory = memory;
    r->session = session;
    r->patches = patches;
    r->patch_capacity = capacity;
    r->budget = budget;
    r->review_opt_in = review_opt_in;
    return VCO_OK;
}
VcoError vco_cheat_tick(VcoCheatRuntime *r)
{
    if (!r || !r->program)
        return VCO_E_ARGUMENT;
    if (r->state != VCO_CHEAT_ON)
        return VCO_OK;
    Frame frame = {.runtime = r, .remaining = r->budget};
    for (size_t i = 0; i < r->program->count;) {
        size_t next = i + 1;
        VcoError e = execute(&frame, &r->program->ops[i], i, &next);
        if (e) {
            VcoError cleanup = release_all(r);
            r->state = VCO_CHEAT_ERROR;
            r->diagnostic = (VcoDiagnostic){cleanup ? cleanup : e, r->program->ops[i].line};
            return r->diagnostic.code;
        }
        i = next;
    }
    return VCO_OK;
}
VcoError vco_cheat_enable(VcoCheatRuntime *r)
{
    if (!r || !r->program)
        return VCO_E_ARGUMENT;
    if (r->diagnostic.code == VCO_E_RESTORE)
        return VCO_E_RESTORE;
    if (r->state == VCO_CHEAT_ON)
        return VCO_OK;
    if (r->program->needs_review & ~r->review_opt_in) {
        r->diagnostic = (VcoDiagnostic){VCO_E_UNVERIFIED, 0};
        r->state = VCO_CHEAT_ERROR;
        return VCO_E_UNVERIFIED;
    }
    r->diagnostic = (VcoDiagnostic){VCO_OK, 0};
    r->state = VCO_CHEAT_ON;
    return vco_cheat_tick(r);
}
VcoError vco_cheat_disable(VcoCheatRuntime *r)
{
    if (!r || !r->program)
        return VCO_E_ARGUMENT;
    if (r->diagnostic.code == VCO_E_RESTORE)
        return VCO_E_RESTORE;
    VcoError e = release_all(r);
    r->state = e ? VCO_CHEAT_ERROR : VCO_CHEAT_OFF;
    r->diagnostic = (VcoDiagnostic){e, 0};
    return e;
}
