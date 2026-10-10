#ifndef VCO_METADATA_H
#define VCO_METADATA_H
#include <stddef.h>
#include <stdint.h>

#define VCO_META_TITLE_ID 1u
#define VCO_META_TITLE 2u
#define VCO_META_VERSION 4u
typedef struct {
    char title_id[10], title[256], version[16];
    uint32_t fields, invalid, conflicts, source_line, title_id_line, version_line;
} VcoFileMetadata;
typedef enum {
    VCO_COMPAT_UNVERIFIED,
    VCO_COMPAT_MATCH,
    VCO_COMPAT_MISMATCH,
    VCO_COMPAT_CONFLICT
} VcoCompatibility;

/* Only the header comments before the first non-comment record are metadata.
 * Recognize explicit keys and the official '# ID Name 1.02' example. Free-form
 * comments never become evidence that a version matches. Input is UTF-8 already
 * validated by the cheat parser; no strings are borrowed from its workspace. */
void vco_metadata_parse(const char *text, size_t length, VcoFileMetadata *out);
/* 1 equal, 0 different, -1 unknown syntax. Major may have leading zeros;
 * minor must contain exactly two decimal digits. No ranges or suffix guessing. */
int vco_version_equal(const char *a, const char *b);
VcoCompatibility vco_metadata_compatibility(const VcoFileMetadata *metadata, const char *title_id,
                                            const char *version);
#endif
