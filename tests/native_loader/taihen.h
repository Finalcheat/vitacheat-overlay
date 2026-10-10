#include <stdint.h>
typedef struct { unsigned size, module_nid; } tai_module_info_t;
int taiGetModuleInfo(const char *, tai_module_info_t *);
int taiGetModuleExportFunc(const char *, unsigned, unsigned, uintptr_t *);
