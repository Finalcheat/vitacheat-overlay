#ifndef VCO_SFO_H
#define VCO_SFO_H
#include <stddef.h>
/* Extract a bounded NUL-terminated UTF-8 string from a PSF table.
 * 1 found, 0 missing, -1 malformed. No packed struct/alignment assumptions. */
int vco_sfo_string(const void *data, size_t length, const char *key, char *out, size_t capacity);
#endif
