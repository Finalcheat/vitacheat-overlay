#ifndef VCO_TEXT_H
#define VCO_TEXT_H
#include <stddef.h>
#include <stdint.h>
/* Invalid UTF-8 is replaced, truncation adds an ellipsis. Never splits a UTF-16
 * surrogate pair or leaves an unterminated destination. Capacity includes NUL. */
size_t vco_utf8_to_utf16(const char *src, uint16_t *out, size_t capacity);
void vco_text_copy(char *out, size_t capacity, const char *src);
#endif
