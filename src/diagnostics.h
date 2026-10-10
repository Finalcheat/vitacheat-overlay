#include "storage_paths.h"
#ifndef VCO_DIAGNOSTICS_H
#define VCO_DIAGNOSTICS_H
#define VCO_LOG_FILE_LIMIT (64u * 1024u)
#ifndef VCO_LOG_PATH
#ifdef __VITA_KERNEL__
#define VCO_LOG_PATH VCO_KERNEL_LOG
#else
#define VCO_LOG_PATH VCO_SHELL_LOG
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { VCO_DIAGNOSTICS_OK = 0, VCO_DIAGNOSTICS_ERROR = -1, VCO_DIAGNOSTICS_BUSY = -2 };
/* Best-effort, bounded records and files. One shared implementation per plugin
 * serializes all translation units without waiting: a busy writer drops the
 * new record. Full files are truncated before storing the latest record.
 * No APPEND, allocation, thread creation or periodic hook-path logging.
 * A failed close retains its ambiguous descriptor and disables further I/O;
 * resident modules must not reset this ownership state before reboot.
 * BUSY means no I/O was attempted; a caller retaining its record can retry on
 * a later worker iteration. ERROR is an invalid argument or storage failure. */
int vco_diagnostics_write(const char *path, const char *event, unsigned value, int reset);
#ifdef __cplusplus
}
#endif
static inline void vco_log(const char *event, unsigned value, int reset)
{
    (void)vco_diagnostics_write(VCO_LOG_PATH, event, value, reset);
}
#endif
