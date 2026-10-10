#include "source.h"
/* FNV-1a identifies ordinary file edits; it is not a security authenticator. */
void vco_source_begin(VcoSourceStamp *stamp, VcoListSource source)
{
    if (stamp)
        *stamp = (VcoSourceStamp){UINT64_C(14695981039346656037), 0, VCO_FILE_OK, source};
}
bool vco_source_add(VcoSourceStamp *stamp, const void *data, size_t length)
{
    if (!stamp)
        return false;
    if ((!data && length) || stamp->length > VCO_CATALOG_TEXT) {
        stamp->result = VCO_FILE_ERROR;
        return false;
    }
    if (stamp->result != VCO_FILE_OK)
        return false;
    if (length > VCO_CATALOG_TEXT - stamp->length) {
        stamp->result = VCO_FILE_TOO_LARGE;
        return false;
    }
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        stamp->hash ^= bytes[i];
        stamp->hash *= UINT64_C(1099511628211);
    }
    stamp->length += (uint32_t)length;
    return true;
}
VcoSourceCheckState vco_source_compare(const VcoSourceStamp *loaded,
                                       const VcoSourceStamp *observed)
{
    if (!loaded || !observed || (unsigned)loaded->result > VCO_FILE_ERROR ||
        (unsigned)observed->result > VCO_FILE_ERROR ||
        (unsigned)loaded->source > VCO_SOURCE_VITACHEAT ||
        (unsigned)observed->source > VCO_SOURCE_VITACHEAT || loaded->result == VCO_FILE_ERROR ||
        observed->result == VCO_FILE_ERROR ||
        (loaded->result == VCO_FILE_OK &&
         (loaded->source == VCO_SOURCE_NONE || loaded->length > VCO_CATALOG_TEXT)) ||
        (observed->result == VCO_FILE_OK &&
         (observed->source == VCO_SOURCE_NONE || observed->length > VCO_CATALOG_TEXT)))
        return VCO_SOURCE_UNAVAILABLE;
    if (loaded->result == VCO_FILE_MISSING && observed->result == VCO_FILE_MISSING)
        return VCO_SOURCE_UNCHANGED;
    if (loaded->result != observed->result)
        return VCO_SOURCE_CHANGED;
    if (loaded->result != VCO_FILE_OK || loaded->source == VCO_SOURCE_NONE ||
        observed->source == VCO_SOURCE_NONE)
        return VCO_SOURCE_UNAVAILABLE;
    return loaded->source == observed->source && loaded->length == observed->length &&
                   loaded->hash == observed->hash
               ? VCO_SOURCE_UNCHANGED
               : VCO_SOURCE_CHANGED;
}
