#include "metadata.h"
#include <string.h>

typedef struct {
    const char *begin, *end;
} Span;
static int space(char c)
{
    return c == ' ' || c == '\t';
}
static Span trim(Span s)
{
    while (s.begin < s.end && space(*s.begin))
        ++s.begin;
    while (s.end > s.begin && space(s.end[-1]))
        --s.end;
    return s;
}
static int copy(char *out, size_t capacity, Span s)
{
    size_t size = (size_t)(s.end - s.begin);
    if (!size || size >= capacity)
        return 0;
    memcpy(out, s.begin, size);
    out[size] = 0;
    return 1;
}
static int version(const char *s, unsigned *value)
{
    unsigned major = 0, digits = 0;
    if (!s)
        return 0;
    while (*s >= '0' && *s <= '9') {
        if (++digits > 3)
            return 0;
        major = major * 10 + (unsigned)(*s++ - '0');
    }
    if (!digits || *s++ != '.')
        return 0;
    unsigned minor = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (*s < '0' || *s > '9')
            return 0;
        minor = minor * 10 + (unsigned)(*s++ - '0');
    }
    if (*s)
        return 0;
    *value = major * 100 + minor;
    return 1;
}
int vco_version_equal(const char *a, const char *b)
{
    unsigned x, y;
    return version(a, &x) && version(b, &y) ? x == y : -1;
}
static int valid_id(const char *id)
{
    if (strlen(id) != 9)
        return 0;
    for (unsigned i = 0; i < 9; ++i)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
            return 0;
    return 1;
}
static void record(VcoFileMetadata *out, unsigned field, Span s, uint32_t line)
{
    char value[256];
    char *target = field == VCO_META_TITLE_ID  ? out->title_id
                   : field == VCO_META_VERSION ? out->version
                                               : out->title;
    size_t capacity = field == VCO_META_TITLE_ID  ? sizeof(out->title_id)
                      : field == VCO_META_VERSION ? sizeof(out->version)
                                                  : sizeof(out->title);
    unsigned unused;
    if (!copy(value, capacity, s) || (field == VCO_META_TITLE_ID && !valid_id(value)) ||
        (field == VCO_META_VERSION && !version(value, &unused))) {
        out->invalid |= field;
        return;
    }
    if (out->fields & field) {
        int equal = field == VCO_META_VERSION ? vco_version_equal(target, value) == 1
                                              : !strcmp(target, value);
        if (!equal) {
            out->conflicts |= field;
            out->source_line = line;
        }
        return;
    }
    memcpy(target, value, strlen(value) + 1);
    out->fields |= field;
    if (!out->source_line)
        out->source_line = line;
    if (field == VCO_META_TITLE_ID)
        out->title_id_line = line;
    if (field == VCO_META_VERSION)
        out->version_line = line;
}
static unsigned key(Span s)
{
    char normalized[24];
    size_t n = 0;
    s = trim(s);
    for (const char *p = s.begin; p < s.end; ++p) {
        char c = *p;
        if (space(c) || c == '_')
            continue;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (n + 1 >= sizeof(normalized))
            return 0;
        normalized[n++] = c;
    }
    normalized[n] = 0;
    if (!strcmp(normalized, "titleid") || !strcmp(normalized, "gameid"))
        return VCO_META_TITLE_ID;
    if (!strcmp(normalized, "title") || !strcmp(normalized, "game") ||
        !strcmp(normalized, "gamename"))
        return VCO_META_TITLE;
    if (!strcmp(normalized, "version") || !strcmp(normalized, "appver") ||
        !strcmp(normalized, "gameversion"))
        return VCO_META_VERSION;
    return 0;
}
static void comment(Span s, uint32_t line, VcoFileMetadata *out)
{
    s = trim(s);
    const char *colon = memchr(s.begin, ':', (size_t)(s.end - s.begin));
    if (colon) {
        unsigned field = key((Span){s.begin, colon});
        if (field) {
            record(out, field, trim((Span){colon + 1, s.end}), line);
            return;
        }
    }
    const char *end = s.begin;
    while (end < s.end && !space(*end))
        ++end;
    char id[10];
    if (!copy(id, sizeof(id), (Span){s.begin, end}) || !valid_id(id))
        return;
    /* Avoid treating an arbitrary nine-letter English word as an identifier. */
    int have_digit = 0;
    for (unsigned i = 0; i < 9; ++i)
        have_digit |= id[i] >= '0' && id[i] <= '9';
    if (!have_digit)
        return;
    record(out, VCO_META_TITLE_ID, (Span){s.begin, end}, line);
    Span name = trim((Span){end, s.end});
    if (name.begin == name.end)
        return;
    const char *last = name.end;
    while (last > name.begin && !space(last[-1]))
        --last;
    char tail[16];
    unsigned unused;
    if (copy(tail, sizeof(tail), (Span){last, name.end}) && version(tail, &unused)) {
        record(out, VCO_META_VERSION, (Span){last, name.end}, line);
        name = trim((Span){name.begin, last});
    }
    if (name.begin != name.end)
        record(out, VCO_META_TITLE, name, line);
}
void vco_metadata_parse(const char *text, size_t length, VcoFileMetadata *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!text)
        return;
    size_t pos = length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3) ? 3 : 0;
    uint32_t line = 1;
    while (pos < length) {
        size_t start = pos;
        while (pos < length && text[pos] != '\r' && text[pos] != '\n')
            ++pos;
        Span s = trim((Span){text + start, text + pos});
        if (s.begin != s.end) {
            if (*s.begin != '#')
                break;
            comment((Span){s.begin + 1, s.end}, line, out);
        }
        if (pos < length && text[pos++] == '\r' && pos < length && text[pos] == '\n')
            ++pos;
        ++line;
    }
}
VcoCompatibility vco_metadata_compatibility(const VcoFileMetadata *metadata, const char *title_id,
                                            const char *app_version)
{
    if (!metadata || !title_id)
        return VCO_COMPAT_UNVERIFIED;
    unsigned identity = VCO_META_TITLE_ID | VCO_META_VERSION;
    if (metadata->conflicts & identity)
        return VCO_COMPAT_CONFLICT;
    if ((metadata->fields & VCO_META_TITLE_ID) && strcmp(title_id, metadata->title_id))
        return VCO_COMPAT_MISMATCH;
    if (metadata->fields & VCO_META_VERSION) {
        int equal = vco_version_equal(metadata->version, app_version);
        if (equal == 0)
            return VCO_COMPAT_MISMATCH;
        if (equal == 1 && !(metadata->invalid & identity))
            return VCO_COMPAT_MATCH;
    }
    return VCO_COMPAT_UNVERIFIED;
}
