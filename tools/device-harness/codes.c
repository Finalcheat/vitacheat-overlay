#include "codes.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#define AT(field) ((unsigned)(data + offsetof(VcoHarnessData, field)))
static int append(char *out, size_t size, size_t *used, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(out + *used, size - *used, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= size - *used)
        return -1;
    *used += (size_t)length;
    return 0;
}
int vco_harness_codes(char *out, size_t size, uint32_t data, uint32_t function,
                      uint32_t thumb32_function, unsigned index, uint32_t text_base,
                      uint32_t module_data, uint32_t data_base)
{
    if (!out || !size)
        return -1;
    out[0] = '\0';
    if (!data || (data & 3) || (uint64_t)data + sizeof(VcoHarnessData) > UINT64_C(0x100000000) ||
        !function || (function & 1) || (uint64_t)function + 4 > UINT64_C(0x100000000) ||
        !thumb32_function || (thumb32_function & 3) ||
        (uint64_t)thumb32_function + 8 > UINT64_C(0x100000000) || index > 255 ||
        function < text_base || thumb32_function < text_base || module_data < data_base ||
        (uint64_t)module_data + 4 > UINT64_C(0x100000000))
        return -1;
    size_t used = 0;
    if (append(
            out, size, &used,
            "# Generated for VCOCH0001; valid only until this app exits.\n"
            "# Title ID: VCOCH0001\n# Title: VCO Device Harness\n"
            "# Version: " VCO_HARNESS_VERSION "\n"
            "_V1 01 Direct 32 - expect 100 (starts OFF)\n$0200 %08X 00000064\n"
            "_V0 02 Direct 16 - expect 25\n$0100 %08X 00000019\n"
            "_V0 03 Direct 8 - expect 7\n$0000 %08X 00000007\n"
            "_V0 04 Copy 32 - expect source 1234\n$5200 %08X %08X\n"
            "_V0 05 Repeat - expect 10 11 12\n$4201 %08X 0000000A\n$0003 00000004 00000001\n"
            "_V0 06 Pointer write - expect target 55\n$3201 %08X 00000000\n$3300 00000000 "
            "00000037\n"
            "_V0 07 Pointer copy - expect 1234\n$8201 %08X 00000000\n$8800 00000000 00000000\n"
            "$8601 %08X 00000000\n$8900 00000000 00000000\n"
            "_V0 08 Pointer repeat - expect target 20 21 22\n$7201 %08X 00000000\n"
            "$7701 00000000 00000014\n$0003 00000004 00000001\n"
            "_V0 09 Compare source==1234 - expect 66\n$D201 %08X 000004D2\n$0200 %08X 00000042\n"
            "_V0 10 Hold R only after closing - expect 77\n$C201 00000000 00000200\n$0200 %08X "
            "0000004D\n"
            "_V0 11 A200 data - 88; OFF restores 5\n$A200 %08X 00000058\n"
            "_V0 12 A100 RX function - 9; OFF restores 7\n$A100 %08X 00002009\n"
            "_V0 13 B relative RX patch - 9; OFF restores 7\n$B2%02X 00000000 00000000\n$A100 %08X "
            "00002009\n"
            "_V0 14 B relative data - expect 99\n$B2%02X 00000001 00000000\n$0200 %08X 00000063\n",
            AT(direct32), AT(direct16), AT(direct8), AT(copy), AT(source), AT(repeat), AT(pointer),
            AT(pointer), AT(source_pointer), AT(pointer), AT(source), AT(condition), AT(pad),
            AT(patch), (unsigned)function, index, (unsigned)(function - text_base), index,
            (unsigned)(module_data - data_base)))
        goto invalid;
    if (append(out, size, &used,
               "_V0 15 A200 Thumb-2 at +2 - 9; OFF restores 7\n$A200 %08X 0009F04F\n"
               "_V0 16 A200 x117 - all patched; OFF restores all\n",
               (unsigned)(thumb32_function + 2)))
        goto invalid;
    for (unsigned i = 0; i < VCO_HARNESS_BULK_PATCHES; ++i)
        if (append(out, size, &used, "$A200 %08X %08X\n", AT(bulk) + 4 * i,
                   VCO_HARNESS_BULK_PATCHED + i))
            goto invalid;
    if (used > INT_MAX)
        goto invalid;
    return (int)used;
invalid:
    out[0] = '\0';
    return -1;
}
void vco_harness_bulk_counts(const VcoHarnessData *data, unsigned counts[3])
{
    counts[0] = counts[1] = counts[2] = 0;
    for (unsigned i = 0; i < VCO_HARNESS_BULK_PATCHES; ++i) {
        uint32_t value = data->bulk[i];
        unsigned kind = value == VCO_HARNESS_BULK_ORIGINAL + i  ? 0
                        : value == VCO_HARNESS_BULK_PATCHED + i ? 1
                                                                : 2;
        ++counts[kind];
    }
}
int vco_harness_thumb32_expected(const uint16_t words[4])
{
    if (!words || words[0] != 0xBF00 || words[1] != 0xF04F || words[3] != 0x4770 ||
        (words[2] != 7 && words[2] != 9))
        return -1;
    return words[2];
}
