#ifndef VCO_TEST_GAME_MEMORY_TAIHEN_H
#define VCO_TEST_GAME_MEMORY_TAIHEN_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    size_t size;
    int32_t modid;
    int segidx;
    uint32_t offset;
    const void *source;
    size_t source_size;
} tai_offset_args_t;
int32_t taiInjectAbs(void *, const void *, size_t);
int32_t taiInjectDataForUser(tai_offset_args_t *);
int taiInjectRelease(int32_t);
#endif
