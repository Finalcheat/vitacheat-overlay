#include "encoding.h"
#include <stdbool.h>
#include <string.h>

#include "cp936_table.inc"

size_t vco_encoding_scratch_size(size_t length)
{
    return length / 8 + (length % 8 != 0);
}
static VcoError fail(VcoError error, uint32_t line, VcoDiagnostic *diagnostic)
{
    if (diagnostic)
        *diagnostic = (VcoDiagnostic){error, line};
    return error;
}
static bool valid_utf8(const unsigned char *data, size_t length, uint32_t *error_line)
{
    uint32_t line = 1;
    for (size_t i = 0; i < length;) {
        unsigned c = data[i++], count = 0, value = 0, minimum = 0;
        if (c && c < 0x80) {
            if (c == '\n')
                ++line;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            value = c & 31u;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            value = c & 15u;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            value = c & 7u;
            minimum = 0x10000;
        } else
            goto invalid;
        if (count > length - i)
            goto invalid;
        while (count--) {
            c = data[i++];
            if ((c & 0xc0u) != 0x80)
                goto invalid;
            value = (value << 6) | (c & 63u);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            goto invalid;
    }
    return true;
invalid:
    *error_line = line;
    return false;
}
static uint16_t pair(unsigned lead, unsigned trail)
{
    if (lead < 0x81 || lead > 0xfe || trail < 0x40 || trail > 0xfe || trail == 0x7f)
        return 0;
    return cp936_table[(lead - 0x81) * 190 + trail - 0x40 - (trail > 0x7f)];
}
VcoError vco_encoding_decode(char *text, size_t *length, size_t capacity, VcoEncoding policy,
                             void *scratch, size_t scratch_capacity, VcoEncoding *detected,
                             VcoDiagnostic *diagnostic)
{
    if (detected)
        *detected = VCO_ENCODING_AUTO;
    if (diagnostic)
        *diagnostic = (VcoDiagnostic){0, 0};
    if (!text || !length || (unsigned)policy > VCO_ENCODING_GBK)
        return fail(VCO_E_ARGUMENT, 0, diagnostic);
    size_t size = *length;
    if (size > capacity)
        return fail(VCO_E_LIMIT, 0, diagnostic);
    unsigned char *data = (unsigned char *)text;
    bool bom = size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf;
    /* A BOM is an explicit encoding declaration, not a GBK heuristic. */
    if ((size >= 2 &&
         ((data[0] == 0xff && data[1] == 0xfe) || (data[0] == 0xfe && data[1] == 0xff))) ||
        (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0xfe && data[3] == 0xff) ||
        (bom && policy == VCO_ENCODING_GBK))
        return fail(VCO_E_ENCODING, 1, diagnostic);
    uint32_t line = 1;
    if (policy != VCO_ENCODING_GBK) {
        if (valid_utf8(data, size, &line)) {
            if (detected)
                *detected = VCO_ENCODING_UTF8;
            return VCO_OK;
        }
        if (policy == VCO_ENCODING_UTF8 || bom)
            return fail(VCO_E_ENCODING, line, diagnostic);
    }
    size_t bits = vco_encoding_scratch_size(size);
    if (bits > scratch_capacity || (bits && !scratch))
        return fail(VCO_E_LIMIT, 0, diagnostic);
    unsigned char *starts = scratch;
    if (bits)
        memset(starts, 0, bits);
    size_t expanded = size;
    line = 1;
    for (size_t i = 0; i < size;) {
        unsigned c = data[i];
        if (c && c < 0x80) {
            if (c == '\n')
                ++line;
            ++i;
            continue;
        }
        uint16_t value;
        size_t extra;
        if (c == 0x80) {
            extra = 2; /* CP936 Euro: one input byte becomes three UTF-8 bytes. */
            ++i;
        } else {
            if (size - i < 2 || !(value = pair(c, data[i + 1])))
                return fail(VCO_E_ENCODING, line, diagnostic);
            starts[i / 8] |= (unsigned char)(1u << (i % 8));
            extra = value >= 0x800;
            i += 2;
        }
        if (extra > capacity - expanded)
            return fail(VCO_E_LIMIT, line, diagnostic);
        expanded += extra;
    }
    /* Every token retains or expands. Backward writes never overwrite unread
     * input; the bitset distinguishes a trail from an ASCII/Euro single byte. */
    size_t input = size, output = expanded;
    while (input) {
        uint16_t value;
        if (input >= 2 && (starts[(input - 2) / 8] & (1u << ((input - 2) % 8)))) {
            value = pair(data[input - 2], data[input - 1]);
            input -= 2;
        } else {
            unsigned c = data[--input];
            value = c == 0x80 ? 0x20ac : (uint16_t)c;
        }
        if (value < 0x80)
            data[--output] = (unsigned char)value;
        else {
            data[--output] = (unsigned char)(0x80 | (value & 63u));
            if (value >= 0x800) {
                data[--output] = (unsigned char)(0x80 | ((value >> 6) & 63u));
                data[--output] = (unsigned char)(0xe0 | (value >> 12));
            } else
                data[--output] = (unsigned char)(0xc0 | (value >> 6));
        }
    }
    *length = expanded;
    if (detected)
        *detected = VCO_ENCODING_GBK;
    return VCO_OK;
}
