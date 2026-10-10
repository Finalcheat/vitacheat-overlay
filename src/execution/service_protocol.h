#ifndef VCO_SERVICE_PROTOCOL_H
#define VCO_SERVICE_PROTOCOL_H
#include "upload.h"
#include "mailbox.h"
#include "trace.h"
#include "../platform/foreground_probe.h"
#define VCO_SERVICE_ABI 0x00010001u
typedef enum {
    VCO_SERVICE_STARTING,
    VCO_SERVICE_READY,
    VCO_SERVICE_BACKGROUND,
    VCO_SERVICE_NO_GAME,
    VCO_SERVICE_ERROR,
    VCO_SERVICE_CONFLICT, /* Reserved old state; VitaCheat coexistence is allowed. */
    VCO_SERVICE_LOADING
} VcoServiceState;
typedef struct {
    uint64_t session, revision, load_request, content_key;
    uint32_t abi, size, state, error, count, catalog_revision, event_epoch, reserved;
    int32_t pid;
    char title_id[10];
    uint8_t padding[2];
} VcoServiceStatus;
/* Entire upload is at most VCO_UPLOAD_MAX bytes. Nonblocking admission and
 * copied ownership; compilation/binding happen on the execution worker.
 * All calls return VcoMailboxStatus. Load(NULL, 0) queues a list withdrawal:
 * after cleanup, revision/count/catalog_revision/content_key become zero while
 * load_request keeps its last processed value. This is serialized with uploads. */
#ifdef __cplusplus
extern "C" {
#endif
int vcoServiceObserve(const VcoForegroundSample *, uint32_t length);
int vcoServiceLoad(const void *packet, uint32_t length);
int vcoServiceStatus(VcoServiceStatus *, uint32_t length);
int vcoServiceSubmit(const VcoControlRequest *, uint32_t length, uint64_t *ticket);
int vcoServiceReceive(uint64_t ticket, VcoControlReply *, uint32_t length);
int vcoServiceAck(uint64_t ticket);
/* VcoControlResume layout is ticket then request, fixed 64 bytes. */
int vcoServiceResume(void *resume, uint32_t length);
/* Read-only diagnostic snapshot. Does not start/restart execution or consume
 * records; CopyToUser failures can repeat the same cursor. */
int vcoServiceTrace(uint64_t after, VcoTraceBatch *, uint32_t length);
#ifdef __cplusplus
}
#endif
#endif
