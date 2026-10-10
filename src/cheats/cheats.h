#ifndef VCO_CHEATS_H
#define VCO_CHEATS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Portable, allocation-free core. All buffers are caller-owned. No Vita headers,
 * file I/O, threads, global state, or direct game-pointer dereferences here. */
typedef enum {
    VCO_OK = 0,
    VCO_E_ARGUMENT,
    VCO_E_LIMIT,
    VCO_E_ENCODING,
    VCO_E_SYNTAX,
    VCO_E_FORMAT,
    VCO_E_UNVERIFIED,
    VCO_E_ADDRESS,
    VCO_E_IO,
    VCO_E_SESSION,
    VCO_E_BUDGET,
    VCO_E_PATCH,
    VCO_E_RESTORE,
    VCO_E_METADATA
} VcoError;
typedef struct {
    VcoError code;
    uint32_t line;
} VcoDiagnostic;
typedef struct {
    uint16_t code;
    uint32_t a, b, line;
} VcoCode;
typedef struct {
    size_t name, first, count; /* name offset in text arena; code range */
    int file_enabled;          /* source metadata only; never starts execution */
    VcoDiagnostic diagnostic;
} VcoCheatEntry;
typedef struct {
    VcoCheatEntry *entries;
    size_t entry_capacity, entry_count;
    VcoCode *codes;
    size_t code_capacity, code_count;
    char *names;
    size_t name_capacity, name_size;
} VcoCheatFile;
/* Fatal errors clear public counts. Entry-local errors preserve other entries.
 * Input is length-delimited UTF-8; it need not be NUL terminated. */
VcoError vco_cheat_parse(const char *text, size_t length, VcoCheatFile *out,
                         VcoDiagnostic *diagnostic);

typedef enum {
    VCO_OP_WRITE,
    VCO_OP_COPY,
    VCO_OP_REPEAT,
    VCO_OP_POINTER,
    VCO_OP_POINTER_COPY,
    VCO_OP_POINTER_REPEAT,
    VCO_OP_PATCH,
    VCO_OP_PAD,
    VCO_OP_COMPARE,
    VCO_OP_MODULE
} VcoOpKind;
typedef struct {
    VcoOpKind kind;
    uint8_t width, comparison;
    uint16_t depth, repeat_level;
    uint32_t a, b, count, address_step, value_step, line;
    size_t chain, source_chain, next; /* code offsets; next = false branch op */
} VcoCheatOp;
#define VCO_REVIEW_CONDITIONS 1u
#define VCO_REVIEW_MODULE_COMBINATIONS 2u
#define VCO_REVIEW_A000 4u
#define VCO_MAX_POINTER_DEPTH 255u
/* pad(): zero publishes device/buttons; SUPPRESSED skips the guarded code
 * block and releases its active A patches without reading either output.
 * All other nonzero results are errors. Neutral buttons cannot represent
 * suppression because a zero mask matches. Z06 compares all button bits for
 * equality and ignores the device field. One sample is shared by a tick. */
#define VCO_PAD_SUPPRESSED 1
/* Access callbacks may return this only for lost session/authority, separately
 * from an actual backend failure. inject may use it only before acquiring native
 * ownership. release failures always remain restoration failures. */
#define VCO_ACCESS_SESSION (-3)
/* Immutable compiled program borrows file.codes. Keep both alive until all
 * runtimes have stopped. Review bits remain available for unverified extensions;
 * the documented formats verified against Z06 no longer emit the legacy bits
 * above. Native memory/module lifetime checks still apply independently. */
typedef struct {
    VcoCheatOp *ops;
    size_t capacity, count;
    const VcoCode *codes;
    uint32_t needs_review;
} VcoCheatProgram;
VcoError vco_cheat_compile(const VcoCheatFile *file, size_t entry, VcoCheatProgram *out,
                           VcoDiagnostic *diagnostic);

typedef struct {
    void *context;
    /* Every callback must bind to this session; native implementations must
     * validate mappings/permissions again at the actual access boundary.
     * Callbacks must not reenter the runtime/engine or mutate its buffers. */
    int (*alive)(void *, uint64_t session);
    int (*read)(void *, uint64_t, uint32_t, uint8_t *, size_t);
    int (*write)(void *, uint64_t, uint32_t, const uint8_t *, size_t);
    int (*module)(void *, uint64_t, unsigned index, unsigned segment, uint32_t *base,
                  uint32_t *size);
    int (*pad)(void *, uint64_t, uint32_t *device, uint32_t *buttons);
    int (*inject)(void *, uint64_t, uint32_t, const uint8_t *, size_t, int32_t *handle);
    /* Release consumes a handle EVEN ON FAILURE. Adapter owns any recovery.
     * It must not restore stale-session addresses. */
    int (*release)(void *, uint64_t, int32_t handle);
} VcoCheatMemory;
typedef struct {
    int active;
    int32_t handle;
    uint32_t address;
    uint8_t width;
} VcoPatch;
typedef enum { VCO_CHEAT_OFF, VCO_CHEAT_ON, VCO_CHEAT_ERROR } VcoCheatState;
typedef struct {
    const VcoCheatProgram *program;
    VcoCheatMemory memory;
    VcoPatch *patches;
    size_t patch_capacity;
    uint64_t session;
    uint32_t review_opt_in, budget;
    VcoCheatState state;
    VcoDiagnostic diagnostic;
} VcoCheatRuntime;
/* Single-worker API, not thread safe. Queue/session arbitration belongs above
 * this layer. patches requires program.count slots, initialized by init. */
VcoError vco_cheat_runtime_init(VcoCheatRuntime *runtime, const VcoCheatProgram *program,
                                VcoCheatMemory memory, uint64_t session, VcoPatch *patches,
                                size_t capacity, uint32_t budget, uint32_t review_opt_in);
VcoError vco_cheat_enable(VcoCheatRuntime *runtime);
VcoError vco_cheat_tick(VcoCheatRuntime *runtime);
VcoError vco_cheat_disable(VcoCheatRuntime *runtime);
const char *vco_cheat_error_string(VcoError error);
#ifdef __cplusplus
}
#endif
#endif
