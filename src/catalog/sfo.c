#include "sfo.h"
#include "text.h"
#include <stdint.h>
#include <string.h>
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int vco_sfo_string(const void *data, size_t length, const char *key, char *out, size_t capacity)
{
    if (!out || !capacity)
        return -1;
    out[0] = 0;
    if (!data || !key || length < 20)
        return -1;
    const uint8_t *bytes = data;
    if (memcmp(bytes, "\0PSF", 4) != 0 || le32(bytes + 4) != 0x101)
        return -1;
    uint32_t keys = le32(bytes + 8), values = le32(bytes + 12), count = le32(bytes + 16);
    if (count > (length - 20) / 16 || keys < 20 + count * 16 || keys > values || values > length)
        return -1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = bytes + 20 + i * 16;
        uint32_t offset = (uint32_t)entry[0] | (uint32_t)entry[1] << 8;
        if (offset >= values - keys)
            return -1;
        const char *name = (const char *)bytes + keys + offset;
        if (!memchr(name, 0, values - keys - offset))
            return -1;
        if (strcmp(name, key))
            continue;
        uint32_t size = le32(entry + 4), reserved = le32(entry + 8), start = le32(entry + 12);
        if (entry[2] != 4 || entry[3] != 2 || size > reserved || start > length - values ||
            reserved > length - values - start || !size)
            return -1;
        const char *value = (const char *)bytes + values + start;
        if (!memchr(value, 0, size))
            return -1;
        vco_text_copy(out, capacity, value);
        return 1;
    }
    return 0;
}
