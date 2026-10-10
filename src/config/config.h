#ifndef VCO_CONFIG_H
#define VCO_CONFIG_H
#include "../settings.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VCO_CONFIG_CAPACITY 192
#define VCO_CONFIG_VERSION 6
/* Bounded INI codec: migrate v1-v5 in memory, write v6 with five languages.
 * Decode commits only a complete valid record. */
bool vco_config_decode(const char *data, size_t length, VcoSettings *out);
size_t vco_config_encode(const VcoSettings *settings, char *data, size_t capacity);
/* Storage callbacks: 0 success, 1 missing, -1 error. Write includes flush/close.
 * IDs: 0 current, 1 staging, 2 previous. Single worker owns all calls. */
typedef struct {
    void *context;
    int (*read)(void *, unsigned, char *, size_t, size_t *);
    int (*write)(void *, unsigned, const char *, size_t);
    int (*remove)(void *, unsigned);
    int (*rename)(void *, unsigned, unsigned);
} VcoConfigIO;
typedef enum {
    VCO_CONFIG_LOADED,
    VCO_CONFIG_DEFAULT,
    VCO_CONFIG_RECOVERED,
    VCO_CONFIG_INVALID
} VcoConfigLoad;
VcoConfigLoad vco_config_load(const VcoConfigIO *io, VcoSettings *out);
bool vco_config_save(const VcoConfigIO *io, const VcoSettings *value);
#ifdef __cplusplus
}
#endif
#endif
