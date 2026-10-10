#include "diagnostics.h"
#include <stdint.h>
#ifdef __VITA_KERNEL__
#include <psp2kern/io/fcntl.h>
#define OPEN_FILE ksceIoOpen
#define WRITE_FILE ksceIoWrite
#define CLOSE_FILE ksceIoClose
#define SEEK_FILE ksceIoLseek
#else
#include <psp2/io/fcntl.h>
#define OPEN_FILE sceIoOpen
#define WRITE_FILE sceIoWrite
#define CLOSE_FILE sceIoClose
#define SEEK_FILE sceIoLseek
#endif

/* Shared by every logging TU linked into this plugin. The gate also protects
 * descriptor ownership. A negative close result cannot prove consumption, so
 * retain the identifier and never retry or accumulate more descriptors. */
static unsigned writer_busy;
static int quarantined_fd = -1;
static int close_file(int fd)
{
    if (CLOSE_FILE(fd) >= 0)
        return 0;
    quarantined_fd = fd;
    return -1;
}
int vco_diagnostics_write(const char *path, const char *event, unsigned value, int reset)
{
    if (!path || !*path || !event)
        return VCO_DIAGNOSTICS_ERROR;
    if (__atomic_exchange_n(&writer_busy, 1u, __ATOMIC_ACQUIRE))
        return VCO_DIAGNOSTICS_BUSY;
    int result = VCO_DIAGNOSTICS_ERROR;
    if (quarantined_fd >= 0)
        goto done;
    char record[128];
    unsigned count = 0;
    while (*event && count < 110)
        record[count++] = *event++;
    record[count++] = ' ';
    record[count++] = '0';
    record[count++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        record[count++] = "0123456789abcdef"[(value >> shift) & 15];
    record[count++] = '\n';
    int flags = SCE_O_WRONLY | SCE_O_CREAT;
    int fd = OPEN_FILE(path, flags | (reset ? SCE_O_TRUNC : 0), 0666);
    if (fd < 0)
        goto done;
    if (!reset) {
        int64_t end = SEEK_FILE(fd, 0, SCE_SEEK_END);
        if (end < 0) {
            (void)close_file(fd);
            goto done;
        }
        if ((uint64_t)end > VCO_LOG_FILE_LIMIT - count) {
            if (close_file(fd))
                goto done;
            fd = OPEN_FILE(path, flags | SCE_O_TRUNC, 0666);
            if (fd < 0)
                goto done;
        }
    }
    /* At most count iterations (122), even with one-byte writes. A zero or
     * failed write does not spin. Without APPEND the validated descriptor
     * position bounds our writes even if an independent writer truncates the
     * same file; cross-plugin record ordering is only best effort. */
    unsigned written = 0;
    while (written < count) {
        int amount = WRITE_FILE(fd, record + written, count - written);
        if (amount <= 0 || (unsigned)amount > count - written)
            break;
        written += (unsigned)amount;
    }
    if (written == count)
        result = VCO_DIAGNOSTICS_OK;
    if (close_file(fd))
        result = VCO_DIAGNOSTICS_ERROR;
done:
    __atomic_store_n(&writer_busy, 0u, __ATOMIC_RELEASE);
    return result;
}
