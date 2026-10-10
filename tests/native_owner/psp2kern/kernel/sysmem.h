#ifndef TEST_OWNER_SYSMEM_H
#define TEST_OWNER_SYSMEM_H
#include <stdint.h>
typedef int32_t SceUID;
typedef struct {
    int tag;
} SceClass;
typedef struct {
    int tag;
} SceObjectBase;
int ksceGUIDReferObjectWithClass(SceUID, SceClass *, SceObjectBase **);
int ksceGUIDReleaseObject(SceUID);
#endif
