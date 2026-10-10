#include "config.h"
#include <string.h>
/* The key superset permits explicit v1-v5 migration; each version must still
 * contain exactly its own fields. Unknown and duplicate keys are rejected. */
static const char *const keys[] = {"version", "language", "opacity",  "paging",
                                   "toggle",  "close",    "encoding", "previous",
                                   "next",    "settings", "open",     "button_mode"};
enum { KEY_COUNT = sizeof(keys) / sizeof(keys[0]) };
bool vco_config_decode(const char *data, size_t length, VcoSettings *out)
{
    if (!data || !out || !length || length >= VCO_CONFIG_CAPACITY)
        return false;
    unsigned values[KEY_COUNT] = {0}, seen = 0;
    size_t pos = 0;
    while (pos < length) {
        size_t start = pos;
        while (pos < length && data[pos] != '=' && data[pos] != '\n')
            ++pos;
        if (pos == length || data[pos] != '=')
            return false;
        unsigned key = 0;
        while (key < KEY_COUNT &&
               (strlen(keys[key]) != pos - start || memcmp(keys[key], data + start, pos - start)))
            ++key;
        if (key == KEY_COUNT || (seen & (1u << key)))
            return false;
        ++pos;
        unsigned n = 0, digits = 0;
        while (pos < length && data[pos] >= '0' && data[pos] <= '9') {
            unsigned digit = (unsigned)(data[pos++] - '0');
            if (++digits > 10 || n > (UINT32_MAX - digit) / 10)
                return false;
            n = n * 10 + digit;
        }
        if (!digits)
            return false;
        if (pos < length && data[pos] == '\r')
            ++pos;
        if (pos < length && data[pos++] != '\n')
            return false;
        values[key] = n;
        seen |= 1u << key;
    }
    if (values[1] > (values[0] == VCO_CONFIG_VERSION ? VCO_LANGUAGE_COUNT : 3u))
        return false;
    VcoSettings value = vco_settings_defaults();
    value.language = (int)values[1] - 1;
    value.opacity = values[2];
    value.encoding = (VcoEncoding)values[6];
    if ((values[0] == 1 && seen == 63) || (values[0] == 2 && seen == 127)) {
        if (values[3] > 1 || values[4] > 3 || values[5] > 3 || values[4] == values[5])
            return false;
        static const uint32_t faces[] = {VCO_SQUARE, VCO_CROSS, VCO_CIRCLE, VCO_TRIANGLE};
        value.bindings.previous = values[3] ? VCO_LEFT : VCO_L;
        value.bindings.next = values[3] ? VCO_RIGHT : VCO_R;
        value.bindings.toggle = faces[values[4]];
        value.bindings.close = faces[values[5]];
        value.bindings.settings = VCO_START; /* Legacy formats had no settings field. */
        /* Migration is in memory only. Apply commits v6 to disk explicitly. */
    } else if ((values[0] == 3 || values[0] == 4) && seen == ((1u << 11) - 1u - (1u << 3))) {
        value.bindings.previous = values[7];
        value.bindings.next = values[8];
        value.bindings.toggle = values[4];
        value.bindings.close = values[5];
        value.bindings.settings = values[9];
        value.bindings.open = values[10];
    } else if ((values[0] == 5 || values[0] == VCO_CONFIG_VERSION) &&
               seen == ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 6) | (1u << 10) | (1u << 11))) {
        if (values[11] > VCO_BUTTON_WESTERN)
            return false;
        value.bindings.open = values[10];
        vco_settings_set_button_mode(&value, (VcoButtonMode)values[11]);
    } else
        return false;
    if (values[0] < 5) {
        if ((values[0] < 4 && !vco_bindings_migrate_settings(&value.bindings)) ||
            !vco_bindings_valid(&value.bindings))
            return false;
        /* Keep a former Circle/Cross return preference. Removed page, toggle
         * and settings choices normalize to the fixed actions without writing
         * the old file; an explicit Apply commits the v6 record. */
        vco_settings_set_button_mode(&value, value.bindings.close == VCO_CIRCLE ? VCO_BUTTON_WESTERN
                                                                                : VCO_BUTTON_ASIAN);
    }
    if (!vco_settings_valid(&value))
        return false;
    *out = value;
    return true;
}
size_t vco_config_encode(const VcoSettings *s, char *data, size_t capacity)
{
    if (!data || capacity < VCO_CONFIG_CAPACITY || !vco_settings_valid(s))
        return 0;
    static const unsigned output_keys[] = {0, 1, 2, 11, 10, 6};
    unsigned values[] = {VCO_CONFIG_VERSION, (unsigned)(s->language + 1),
                         s->opacity,         (unsigned)s->button_mode,
                         s->bindings.open,   (unsigned)s->encoding};
    size_t pos = 0;
    for (unsigned i = 0; i < sizeof(output_keys) / sizeof(output_keys[0]); ++i) {
        size_t n = strlen(keys[output_keys[i]]);
        if (pos + n + 13 >= capacity)
            return 0;
        memcpy(data + pos, keys[output_keys[i]], n);
        pos += n;
        data[pos++] = '=';
        unsigned value = values[i], divisor = 1;
        while (value / divisor >= 10)
            divisor *= 10;
        do {
            data[pos++] = (char)('0' + value / divisor % 10);
            divisor /= 10;
        } while (divisor);
        data[pos++] = '\n';
    }
    data[pos] = 0;
    return pos;
}
static int read_value(const VcoConfigIO *io, unsigned id, VcoSettings *out)
{
    char buffer[VCO_CONFIG_CAPACITY];
    size_t length = 0;
    int result = io->read(io->context, id, buffer, sizeof(buffer), &length);
    return result ? result : vco_config_decode(buffer, length, out) ? 0 : -2;
}
VcoConfigLoad vco_config_load(const VcoConfigIO *io, VcoSettings *out)
{
    *out = vco_settings_defaults();
    int current = read_value(io, 0, out);
    if (!current)
        return VCO_CONFIG_LOADED;
    int previous = read_value(io, 2, out);
    if (!previous)
        return VCO_CONFIG_RECOVERED;
    return current == 1 && previous == 1 ? VCO_CONFIG_DEFAULT : VCO_CONFIG_INVALID;
}
bool vco_config_save(const VcoConfigIO *io, const VcoSettings *value)
{
    char data[VCO_CONFIG_CAPACITY];
    VcoSettings check;
    size_t length = vco_config_encode(value, data, sizeof(data));
    if (!length || io->write(io->context, 1, data, length) != 0 || read_value(io, 1, &check) ||
        !vco_settings_equal(value, &check))
        return false;
    /* Preserve the last valid backup if the primary is missing/corrupt. */
    int current = read_value(io, 0, &check);
    if (current == -1)
        return false; /* A read error is not permission to replace an unknown primary. */
    bool moved = false;
    if (!current) {
        if (io->remove(io->context, 2) < 0 || io->rename(io->context, 0, 2))
            return false;
        moved = true;
    } else if (io->remove(io->context, 0) < 0)
        return false;
    if (io->rename(io->context, 1, 0)) {
        if (moved)
            (void)io->rename(io->context, 2, 0);
        return false;
    }
    return read_value(io, 0, &check) == 0 && vco_settings_equal(value, &check);
}
