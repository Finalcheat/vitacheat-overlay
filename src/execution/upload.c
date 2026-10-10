#include "upload.h"
#include <string.h>
typedef char header_size_check[sizeof(VcoUploadHeader) == 64 ? 1 : -1];
typedef char entry_size_check[sizeof(VcoUploadEntry) == 16 ? 1 : -1];
typedef char code_size_check[sizeof(VcoUploadCode) == 16 ? 1 : -1];
static bool title_valid(const char id[10])
{
    VcoIdentity identity = {1, 1, {0}};
    if (!id)
        return false;
    memcpy(identity.title_id, id, 10);
    return vco_identity_valid(&identity);
}
size_t vco_upload_size(unsigned entries, unsigned codes)
{
    if (entries > VCO_ENGINE_ENTRIES || codes > VCO_UPLOAD_CODES)
        return 0;
    return sizeof(VcoUploadHeader) + entries * sizeof(VcoUploadEntry) +
           codes * sizeof(VcoUploadCode);
}
static uint64_t hash(const uint8_t *bytes, size_t size)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        value *= UINT64_C(1099511628211);
    }
    return value ? value : 1;
}
VcoError vco_upload_encode(void *out, size_t capacity, uint64_t request, uint64_t target_session,
                           uint32_t revision, int32_t pid, const char title_id[10],
                           const VcoCheatFile *file, size_t *size)
{
    if (!out || !size || !file || !file->entries || !file->codes || !request || !target_session ||
        !revision || pid <= 0 || !title_valid(title_id) || file->entry_count > VCO_ENGINE_ENTRIES ||
        file->code_count > VCO_UPLOAD_CODES || file->entry_count > file->entry_capacity ||
        file->code_count > file->code_capacity)
        return VCO_E_ARGUMENT;
    size_t needed = vco_upload_size((unsigned)file->entry_count, (unsigned)file->code_count);
    if (capacity < needed)
        return VCO_E_LIMIT;
    /* Validate before publishing any packet. Entry ranges remain ordered and
     * disjoint; diagnostics preserve locally malformed entries without executing. */
    size_t previous_end = 0;
    for (size_t i = 0; i < file->entry_count; ++i) {
        const VcoCheatEntry *entry = &file->entries[i];
        if (entry->first < previous_end || entry->first > file->code_count ||
            entry->count > file->code_count - entry->first ||
            (unsigned)entry->diagnostic.code > VCO_E_METADATA)
            return VCO_E_ARGUMENT;
        previous_end = entry->first + entry->count;
    }
    for (size_t i = 0; i < file->code_count; ++i)
        if (!file->codes[i].line)
            return VCO_E_ARGUMENT;
    VcoUploadHeader header = {.request = request,
                              .target_session = target_session,
                              .abi = VCO_UPLOAD_ABI,
                              .size = (uint32_t)needed,
                              .pid = (uint32_t)pid,
                              .entries = (uint32_t)file->entry_count,
                              .codes = (uint32_t)file->code_count,
                              .catalog_revision = revision};
    memcpy(header.title_id, title_id, 10);
    uint8_t *payload = (uint8_t *)out + sizeof(header);
    for (size_t i = 0; i < file->entry_count; ++i) {
        const VcoCheatEntry *entry = &file->entries[i];
        VcoUploadEntry wire = {(uint32_t)entry->first, (uint32_t)entry->count,
                               (uint32_t)entry->diagnostic.code, entry->diagnostic.line};
        memcpy(payload + i * sizeof(wire), &wire, sizeof(wire));
    }
    uint8_t *codes = payload + file->entry_count * sizeof(VcoUploadEntry);
    for (size_t i = 0; i < file->code_count; ++i) {
        const VcoCode *code = &file->codes[i];
        VcoUploadCode wire = {code->code, code->a, code->b, code->line};
        memcpy(codes + i * sizeof(wire), &wire, sizeof(wire));
    }
    header.content_key = hash(payload, needed - sizeof(header));
    memcpy(out, &header, sizeof(header));
    *size = needed;
    return VCO_OK;
}
VcoError vco_upload_decode(const void *packet, size_t length, VcoCheatFile *out,
                           VcoUploadHeader *head)
{
    if (!out)
        return VCO_E_ARGUMENT;
    out->entry_count = out->code_count = out->name_size = 0;
    if (!packet || !head || !out->entries || !out->codes || length < sizeof(*head))
        return VCO_E_ARGUMENT;
    VcoUploadHeader header;
    memcpy(&header, packet, sizeof(header));
    if (header.abi != VCO_UPLOAD_ABI || header.size != length || !header.request ||
        !header.target_session || !header.catalog_revision || header.pid > INT32_MAX ||
        !header.pid || !title_valid(header.title_id) ||
        length != vco_upload_size(header.entries, header.codes))
        return VCO_E_ARGUMENT;
    for (unsigned i = 0; i < sizeof(header.reserved); ++i)
        if (header.reserved[i])
            return VCO_E_ARGUMENT;
    if (header.entries > out->entry_capacity || header.codes > out->code_capacity)
        return VCO_E_LIMIT;
    const uint8_t *payload = (const uint8_t *)packet + sizeof(header);
    if (header.content_key != hash(payload, length - sizeof(header)))
        return VCO_E_ARGUMENT;
    uint32_t previous_end = 0;
    for (unsigned i = 0; i < header.entries; ++i) {
        VcoUploadEntry wire;
        memcpy(&wire, payload + i * sizeof(wire), sizeof(wire));
        if (wire.first < previous_end || wire.first > header.codes ||
            wire.count > header.codes - wire.first || wire.error > VCO_E_METADATA)
            return VCO_E_ARGUMENT;
        out->entries[i] =
            (VcoCheatEntry){0, wire.first, wire.count, 0, {(VcoError)wire.error, wire.line}};
        previous_end = wire.first + wire.count;
    }
    const uint8_t *codes = payload + header.entries * sizeof(VcoUploadEntry);
    for (unsigned i = 0; i < header.codes; ++i) {
        VcoUploadCode wire;
        memcpy(&wire, codes + i * sizeof(wire), sizeof(wire));
        if (wire.opcode > UINT16_MAX || !wire.line)
            return VCO_E_ARGUMENT;
        out->codes[i] = (VcoCode){(uint16_t)wire.opcode, wire.a, wire.b, wire.line};
    }
    out->entry_count = header.entries;
    out->code_count = header.codes;
    *head = header;
    return VCO_OK;
}
