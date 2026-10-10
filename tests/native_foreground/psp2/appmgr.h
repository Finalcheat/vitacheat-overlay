#ifndef TEST_FOREGROUND_APPMGR_H
#define TEST_FOREGROUND_APPMGR_H
#include <stdint.h>
typedef int32_t SceUID;
typedef int32_t SceInt32;
#define SCE_APPMGR_MAX_APP_NAME_LENGTH 31
SceUID sceAppMgrGetProcessIdByAppIdForShell(SceInt32 app_id);
int sceAppMgrGetNameById(SceUID pid, char *name);
#endif
