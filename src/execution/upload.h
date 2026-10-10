#ifndef VCO_EXECUTION_UPLOAD_H
#define VCO_EXECUTION_UPLOAD_H
#include "engine.h"
#define VCO_UPLOAD_ABI 0x00010001u
#define VCO_UPLOAD_CODES 8192u
/* Bounded canonical code file, not pointers or trusted precompiled operations.
 * Kernel compiles and validates every entry again after receiving its own copy. */
typedef struct {
    uint64_t request, content_key, target_session;
    uint32_t abi, size, pid, entries, codes, catalog_revision;
    char title_id[10];
    uint8_t reserved[6];
} VcoUploadHeader;
typedef struct {
    uint32_t first, count, error, line;
} VcoUploadEntry;
typedef struct {
    uint32_t opcode, a, b, line;
} VcoUploadCode;
#define VCO_UPLOAD_MAX                                                                             \
    (sizeof(VcoUploadHeader) + VCO_ENGINE_ENTRIES * sizeof(VcoUploadEntry) +                       \
     VCO_UPLOAD_CODES * sizeof(VcoUploadCode))
size_t vco_upload_size(unsigned entries, unsigned codes);
VcoError vco_upload_encode(void *out, size_t capacity, uint64_t request, uint64_t target_session,
                           uint32_t catalog_revision, int32_t pid, const char title_id[10],
                           const VcoCheatFile *, size_t *size);
/* Output file supplies entries/codes capacities; names are unnecessary for
 * execution. Failed decode clears public counts and leaves header untouched. */
VcoError vco_upload_decode(const void *packet, size_t length, VcoCheatFile *out, VcoUploadHeader *);
#endif
