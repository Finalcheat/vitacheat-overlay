#include "text.h"
#include <string.h>
static uint32_t decode(const unsigned char **cursor)
{
    const unsigned char *s = *cursor;
    uint32_t v = *s++;
    unsigned n = 0;
    uint32_t minimum = 0;
    if (v < 0x80) {
        *cursor = s;
        return v;
    }
    if (v >= 0xC2 && v <= 0xDF) {
        n = 1;
        minimum = 0x80;
        v &= 31;
    } else if (v >= 0xE0 && v <= 0xEF) {
        n = 2;
        minimum = 0x800;
        v &= 15;
    } else if (v >= 0xF0 && v <= 0xF4) {
        n = 3;
        minimum = 0x10000;
        v &= 7;
    } else {
        *cursor = s;
        return 0xFFFD;
    }
    for (unsigned i = 0; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            ++*cursor;
            return 0xFFFD;
        }
        v = (v << 6) | (s[i] & 63);
    }
    if (v < minimum || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        ++*cursor;
        return 0xFFFD;
    }
    *cursor = s + n;
    return v;
}
size_t vco_utf8_to_utf16(const char *src, uint16_t *out, size_t capacity)
{
    if (!out || !capacity)
        return 0;
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p) {
        uint32_t v = decode(&p);
        size_t units = v > 0xFFFF ? 2 : 1;
        if (n + units >= capacity) {
            if (capacity > 1) {
                if (n == capacity - 1) {
                    --n;
                    if (n && out[n] >= 0xDC00 && out[n] <= 0xDFFF)
                        --n;
                }
                out[n++] = 0x2026;
            }
            break;
        }
        if (units == 2) {
            v -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (v >> 10));
            out[n++] = (uint16_t)(0xDC00 + (v & 1023));
        } else
            out[n++] = (uint16_t)v;
    }
    out[n] = 0;
    return n;
}
void vco_text_copy(char *out, size_t capacity, const char *src)
{
    if (!out || !capacity)
        return;
    if (!src)
        src = "";
    size_t n = strlen(src);
    if (n < capacity) {
        memcpy(out, src, n + 1);
        return;
    }
    if (capacity < 4) {
        out[0] = 0;
        return;
    }
    n = capacity - 4;
    while (n && ((unsigned char)src[n] & 0xC0) == 0x80)
        --n;
    memcpy(out, src, n);
    memcpy(out + n, "\xE2\x80\xA6", 4);
}
