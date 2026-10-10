#ifndef VCO_HARNESS_CODES_H
#define VCO_HARNESS_CODES_H
#include <stddef.h>
#include <stdint.h>
#define VCO_HARNESS_VERSION "00.95"
#define VCO_HARNESS_BULK_PATCHES 117u
#define VCO_HARNESS_BULK_ORIGINAL 0x1000u
#define VCO_HARNESS_BULK_PATCHED 0x7000u
typedef struct {
    volatile uint32_t direct32, direct16, direct8, copy, repeat[3];
    volatile uint32_t pointer, source_pointer, pointer_copy, pointer_repeat[3];
    volatile uint32_t condition, pad, patch, source, target_a[3], target_b[3];
    volatile uint32_t bulk[VCO_HARNESS_BULK_PATCHES];
} VcoHarnessData;
/* Output is a complete .psv for this exact process lifetime; a short buffer or
 * invalid address returns -1 and clears the output instead of publishing a
 * truncated list. thumb32_function starts on a four-byte boundary; its middle
 * Thumb-2 instruction is patched at +2, intentionally not word-aligned. */
int vco_harness_codes(char *, size_t, uint32_t data, uint32_t function, uint32_t thumb32_function,
                      unsigned module_index, uint32_t text_base, uint32_t module_data,
                      uint32_t data_base);
/* Each volatile word is sampled once. Counts are an observation, not an atomic
 * snapshot of all words; check the stable result after confirmed ON/OFF. */
void vco_harness_bulk_counts(const VcoHarnessData *, unsigned counts[3]);
/* Recognizes the controlled nop; mov.w r0,#7/#9; bx lr vector before a call.
 * Returns 7/9 or -1 for unexpected bytes; not a cache/lifetime guarantee. */
int vco_harness_thumb32_expected(const uint16_t words[4]);
#endif
