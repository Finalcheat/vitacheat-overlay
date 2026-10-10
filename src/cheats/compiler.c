#include "cheats.h"
#include <string.h>

static int chain(const VcoCode *c, size_t available, unsigned family, unsigned x, unsigned depth,
                 unsigned marker)
{
    if (!depth || depth > VCO_MAX_POINTER_DEPTH || available < depth + 1u)
        return 0;
    for (unsigned k = 1; k < depth; ++k)
        if (c[k].code != (uint16_t)((family << 12) | (x << 8)) || c[k].a)
            return 0;
    return c[depth].code == marker && c[depth].a == 0;
}
VcoError vco_cheat_compile(const VcoCheatFile *file, size_t entry, VcoCheatProgram *out,
                           VcoDiagnostic *d)
{
    VcoError error = VCO_E_FORMAT;
    uint32_t line = 0;
    if (d)
        *d = (VcoDiagnostic){VCO_OK, 0};
    if (!out)
        return VCO_E_ARGUMENT;
    out->count = 0;
    out->codes = NULL;
    out->needs_review = 0;
    if (!file || !out->ops || !file->codes || !file->entries || entry >= file->entry_count) {
        error = VCO_E_ARGUMENT;
        goto fail;
    }
    const VcoCheatEntry *e = &file->entries[entry];
    if (e->diagnostic.code) {
        error = e->diagnostic.code;
        line = e->diagnostic.line;
        goto fail;
    }
    if (e->first > file->code_count || e->count > file->code_count - e->first) {
        error = VCO_E_ARGUMENT;
        goto fail;
    }
    if (!e->count)
        goto fail;
    size_t end = e->first + e->count;
    for (size_t i = e->first; i < end;) {
        const VcoCode *c = &file->codes[i];
        size_t n = 1;
        unsigned family = c->code >> 12, x = (c->code >> 8) & 15, low = c->code & 255;
        VcoCheatOp op;
        memset(&op, 0, sizeof(op));
        op.line = line = c->line;
        op.a = c->a;
        op.b = c->b;
        op.chain = i;
        op.width = (uint8_t)(x <= 2 ? 1u << x : 0);
        op.count = 1;
        if (out->count >= out->capacity) {
            error = VCO_E_LIMIT;
            goto fail;
        }
        switch (family) {
        case 0:
        case 5:
        case 10:
            if (x > 2 || low)
                goto fail;
            /* Z06 parses a full value word, then stores only the selected
             * width. Keep high bits: repeat value arithmetic is also a word. */
            op.kind = family == 0 ? VCO_OP_WRITE : family == 5 ? VCO_OP_COPY : VCO_OP_PATCH;
            break;
        case 4:
            if (x > 2 || low != 1 || end - i < 2)
                goto fail;
            op.kind = VCO_OP_REPEAT;
            n = 2;
            op.count = c[1].code;
            op.address_step = c[1].a;
            op.value_step = c[1].b;
            break;
        case 3:
            if (x > 2 || !chain(c, end - i, 3, x, low, 0x3300))
                goto fail;
            op.kind = VCO_OP_POINTER;
            op.depth = (uint16_t)low;
            op.b = c[low].b;
            n = low + 1u;
            break;
        case 8: {
            if (x > 2 || !chain(c, end - i, 8, x, low, 0x8800) || c[low].b)
                goto fail;
            size_t second = low + 1u;
            if (end - i <= second)
                goto fail;
            unsigned source_x = (c[second].code >> 8) & 15;
            /* Z06 takes the transfer width from the destination header. Its
             * source header selects a source chain, not a second copy width.
             * Existing files may mix 82/85; require a valid 84..86 chain. */
            if (source_x < 4 || source_x > 6 ||
                c[second].code != (uint16_t)(0x8000 | (source_x << 8) | low) ||
                !chain(c + second, end - i - second, 8, source_x, low, 0x8900) || c[second + low].b)
                goto fail;
            op.kind = VCO_OP_POINTER_COPY;
            op.depth = (uint16_t)low;
            op.source_chain = i + second;
            n = 2 * (low + 1u);
            break;
        }
        case 7:
            if (x > 2 || !low || low > VCO_MAX_POINTER_DEPTH || end - i < low + 2u)
                goto fail;
            /* 77ZZ is a level selector, not the fixed pointer-write marker. */
            if (!chain(c, end - i, 7, x, low, c[low].code) || (c[low].code & 0xFF00) != 0x7700 ||
                (c[low].code & 255) > low)
                goto fail;
            op.kind = VCO_OP_POINTER_REPEAT;
            op.depth = (uint16_t)low;
            op.repeat_level = c[low].code & 255;
            op.b = c[low].b;
            op.count = c[low + 1].code;
            op.address_step = c[low + 1].a;
            op.value_step = c[low + 1].b;
            n = low + 2u;
            break;
        case 11:
            if (x != 2 || c->a > 1 || c->b)
                goto fail;
            op.kind = VCO_OP_MODULE;
            op.a = low;
            op.b = c->a;
            break;
        case 12:
            if (x != 2 || c->a > UINT16_MAX)
                goto fail;
            op.kind = VCO_OP_PAD;
            op.next = low >= end - i ? end : i + 1u + low;
            break;
        case 13:
            if (x > 11)
                goto fail;
            op.kind = VCO_OP_COMPARE;
            op.width = (uint8_t)(1u << (x % 3));
            op.comparison = (uint8_t)(x / 3);
            op.next = low >= end - i ? end : i + 1u + low;
            /* Z06 uses UXTB/UXTH before narrow comparisons. */
            if (op.width < 4)
                op.b &= (1u << (op.width * 8)) - 1u;
            break;
        default:
            goto fail;
        }
        out->ops[out->count++] = op;
        i += n;
    }
    /* Resolve physical code-line spans into instruction boundaries. A condition
     * must not land inside a pointer chain or batch parameter record. */
    for (size_t i = 0; i < out->count; ++i) {
        VcoCheatOp *op = &out->ops[i];
        if (op->kind != VCO_OP_PAD && op->kind != VCO_OP_COMPARE)
            continue;
        line = op->line;
        size_t target = op->next, j = i + 1;
        while (j < out->count && out->ops[j].chain < target)
            ++j;
        if (j == out->count ? target != end : out->ops[j].chain != target)
            goto fail;
        op->next = j;
    }
    out->codes = file->codes;
    return VCO_OK;
fail:
    out->count = 0;
    out->codes = NULL;
    out->needs_review = 0;
    if (d)
        *d = (VcoDiagnostic){error, line};
    return error;
}
