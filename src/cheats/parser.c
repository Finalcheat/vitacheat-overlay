#include "cheats.h"
#include <string.h>

#define MAX_FILE_BYTES (256u * 1024u)
static int space(char c)
{
    return c == ' ' || c == '\t';
}
static int utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint32_t v = s[i++], min;
        unsigned more;
        if (v == 0)
            return 0;
        if (v < 0x80)
            continue;
        if (v >= 0xC2 && v <= 0xDF) {
            more = 1;
            min = 0x80;
            v &= 0x1F;
        } else if (v >= 0xE0 && v <= 0xEF) {
            more = 2;
            min = 0x800;
            v &= 0x0F;
        } else if (v >= 0xF0 && v <= 0xF4) {
            more = 3;
            min = 0x10000;
            v &= 7;
        } else
            return 0;
        if (n - i < more)
            return 0;
        while (more--) {
            unsigned b = s[i++];
            if ((b & 0xC0) != 0x80)
                return 0;
            v = (v << 6) | (b & 63);
        }
        if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
            return 0;
    }
    return 1;
}
static int hex(const char **p, const char *end, unsigned digits, uint32_t *v)
{
    *v = 0;
    if ((size_t)(end - *p) < digits)
        return 0;
    for (unsigned i = 0; i < digits; ++i) {
        unsigned c = (unsigned char)*(*p)++, x;
        if (c >= '0' && c <= '9')
            x = c - '0';
        else if (c >= 'a' && c <= 'f')
            x = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            x = c - 'A' + 10;
        else
            return 0;
        *v = (*v << 4) | x;
    }
    return 1;
}
static int separator(const char **p, const char *end)
{
    if (*p == end || !space(**p))
        return 0;
    while (*p < end && space(**p))
        ++*p;
    return 1;
}
static int code_line(const char *p, const char *end, VcoCode *c)
{
    uint32_t op;
    ++p;
    if (!hex(&p, end, 4, &op) || !separator(&p, end) || !hex(&p, end, 8, &c->a) ||
        !separator(&p, end) || !hex(&p, end, 8, &c->b))
        return 0;
    while (p < end && space(*p))
        ++p;
    c->code = (uint16_t)op;
    return p == end;
}
VcoError vco_cheat_parse(const char *text, size_t length, VcoCheatFile *out, VcoDiagnostic *d)
{
    VcoError error = VCO_OK;
    uint32_t line = 1;
    size_t pos = 0;
    if (d)
        *d = (VcoDiagnostic){VCO_OK, 0};
    if (!out)
        return VCO_E_ARGUMENT;
    out->entry_count = out->code_count = out->name_size = 0;
    if (!text || !out->entries || !out->codes || !out->names) {
        error = VCO_E_ARGUMENT;
        goto fail;
    }
    if (length > MAX_FILE_BYTES) {
        error = VCO_E_LIMIT;
        goto fail;
    }
    if (!utf8((const unsigned char *)text, length)) {
        error = VCO_E_ENCODING;
        goto fail;
    }
    if (length >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0)
        pos = 3;
    while (pos < length) {
        size_t start = pos;
        while (pos < length && text[pos] != '\n' && text[pos] != '\r')
            ++pos;
        const char *p = text + start, *end = text + pos;
        if (pos < length && text[pos++] == '\r' && pos < length && text[pos] == '\n')
            ++pos;
        while (p < end && space(*p))
            ++p;
        while (end > p && space(end[-1]))
            --end;
        if (p == end || *p == '#') {
            ++line;
            continue;
        }
        if (*p == '_') {
            if (end - p < 5 || p[1] != 'V' || (p[2] != '0' && p[2] != '1') || !space(p[3])) {
                error = VCO_E_SYNTAX;
                goto fail;
            }
            int enabled = p[2] == '1';
            p += 4;
            while (p < end && space(*p))
                ++p;
            if (p == end) {
                error = VCO_E_SYNTAX;
                goto fail;
            }
            size_t size = (size_t)(end - p) + 1;
            if (out->entry_count >= out->entry_capacity ||
                size > out->name_capacity - out->name_size) {
                error = VCO_E_LIMIT;
                goto fail;
            }
            VcoCheatEntry *e = &out->entries[out->entry_count++];
            *e = (VcoCheatEntry){out->name_size, out->code_count, 0, enabled, {VCO_OK, 0}};
            memcpy(out->names + out->name_size, p, size - 1);
            out->names[out->name_size + size - 1] = 0;
            out->name_size += size;
        } else {
            if (!out->entry_count) {
                error = VCO_E_SYNTAX;
                goto fail;
            }
            VcoCheatEntry *e = &out->entries[out->entry_count - 1];
            VcoCode c = {0, 0, 0, line};
            if (*p != '$' || !code_line(p, end, &c)) {
                if (e->diagnostic.code == VCO_OK)
                    e->diagnostic = (VcoDiagnostic){VCO_E_SYNTAX, line};
            } else {
                if (out->code_count >= out->code_capacity) {
                    error = VCO_E_LIMIT;
                    goto fail;
                }
                out->codes[out->code_count++] = c;
                ++e->count;
            }
        }
        ++line;
    }
    return VCO_OK;
fail:
    out->entry_count = out->code_count = out->name_size = 0;
    if (d)
        *d = (VcoDiagnostic){error, line};
    return error;
}
const char *vco_cheat_error_string(VcoError e)
{
    static const char *const names[] = {"ok",
                                        "invalid argument",
                                        "resource limit",
                                        "invalid UTF-8",
                                        "invalid syntax",
                                        "invalid code format",
                                        "unverified semantics",
                                        "invalid address",
                                        "memory access failed",
                                        "stale session",
                                        "execution budget exceeded",
                                        "patch failed",
                                        "restore failed",
                                        "cheat file does not match game metadata"};
    return (unsigned)e < sizeof(names) / sizeof(names[0]) ? names[e] : "unknown error";
}
