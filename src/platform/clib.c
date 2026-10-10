/* Portable-core libc subset mapped to the already loaded SceLibKernel.
 * Do not pull a second libc heap/runtime into SceShell. */
#include <stddef.h>
#include <psp2/kernel/clib.h>
void *memcpy(void *out, const void *in, size_t size)
{
    return sceClibMemcpy(out, in, size);
}
void *memset(void *out, int value, size_t size)
{
    return sceClibMemset(out, value, size);
}
int memcmp(const void *a, const void *b, size_t size)
{
    return sceClibMemcmp(a, b, size);
}
int strcmp(const char *a, const char *b)
{
    return sceClibStrcmp(a, b);
}
size_t strlen(const char *s)
{
    return sceClibStrnlen(s, (SceSize)-1);
}
void *memchr(const void *s, int c, size_t size)
{
    return sceClibMemchr(s, c, size);
}
