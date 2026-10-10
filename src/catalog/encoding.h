#ifndef VCO_ENCODING_H
#define VCO_ENCODING_H
#include "cheats.h"
#include "../file_encoding.h"
#ifdef __cplusplus
extern "C" {
#endif

/* One scratch bit per input byte. No heap, external tables, I/O or lossy repair.
 * Conversion is transactional: on failure data and *length are unchanged, and
 * detected is AUTO (unknown). Scratch is separate from data and may be changed.
 * Input and decoded output both fit capacity; no NUL terminator is required.
 * AUTO validates UTF-8 first; explicit GBK resolves ambiguous byte streams. */
size_t vco_encoding_scratch_size(size_t length);
VcoError vco_encoding_decode(char *data, size_t *length, size_t capacity, VcoEncoding policy,
                             void *scratch, size_t scratch_capacity, VcoEncoding *detected,
                             VcoDiagnostic *diagnostic);
#ifdef __cplusplus
}
#endif
#endif
