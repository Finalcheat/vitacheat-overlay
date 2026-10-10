#ifndef VCO_CATALOG_H
#define VCO_CATALOG_H
#include "cheats.h"
#include "metadata.h"
#include "encoding.h"
#define VCO_CATALOG_ENTRIES 256u
#define VCO_CATALOG_CODES 8192u
#define VCO_CATALOG_TEXT (256u * 1024u)
#define VCO_CATALOG_NAMES (64u * 1024u)
#define VCO_LABEL_BYTES 192u

typedef struct {
    int32_t app_id, pid;
    char title_id[16], title[256], version[32];
    int32_t main_thread_hint; /* Diagnostic only, not a verified lifecycle identity. */
} VcoGame;
typedef enum {
    VCO_LIST_LOADING,
    VCO_LIST_READY,
    VCO_LIST_NO_GAME,
    VCO_LIST_AMBIGUOUS,
    VCO_LIST_DISCOVERY_ERROR,
    VCO_LIST_MISSING,
    VCO_LIST_IO_ERROR,
    VCO_LIST_PARSE_ERROR,
    VCO_LIST_LIMIT
} VcoListState;
/* OWN is retained for earlier snapshots; new catalogs use only VITACHEAT. */
typedef enum { VCO_SOURCE_NONE, VCO_SOURCE_OWN, VCO_SOURCE_VITACHEAT } VcoListSource;
typedef struct {
    char name[VCO_LABEL_BYTES];
    VcoDiagnostic diagnostic;
    uint32_t needs_review;
    unsigned file_enabled;
} VcoCatalogRow;
typedef struct {
    uint32_t revision;
    uint64_t execution_session; /* Trusted kernel token sampled around this file load. */
    VcoGame game;
    VcoListState state;
    VcoListSource source;
    VcoEncoding encoding_policy, encoding; /* AUTO encoding means not yet identified. */
    VcoDiagnostic diagnostic;
    uint32_t count;
    VcoFileMetadata metadata;
    VcoCompatibility compatibility;
    VcoCatalogRow rows[VCO_CATALOG_ENTRIES];
} VcoCatalog;
typedef struct {
    /* Parsed ownership retained until the catalog worker copies a canonical
     * upload. Zero after every failed load; never infer counts from stale rows. */
    size_t entry_count, code_count, name_size;
    char text[VCO_CATALOG_TEXT], names[VCO_CATALOG_NAMES];
    VcoCheatEntry entries[VCO_CATALOG_ENTRIES];
    VcoCode codes[VCO_CATALOG_CODES];
    VcoCheatOp ops[VCO_CATALOG_CODES];
} VcoCatalogWorkspace;
typedef enum { VCO_FILE_OK, VCO_FILE_MISSING, VCO_FILE_TOO_LARGE, VCO_FILE_ERROR } VcoFileResult;
typedef VcoFileResult (*VcoReadFile)(void *ctx, const char *path, char *out, size_t capacity,
                                     size_t *length);
/* Presentation-only snapshot: there is deliberately no enabled bool that could
 * impersonate a confirmed execution state. file_enabled is source metadata. */
void vco_catalog_load(VcoCatalog *out, const VcoGame *game, VcoCatalogWorkspace *work,
                      VcoReadFile read_file, void *context);
void vco_catalog_load_encoded(VcoCatalog *out, const VcoGame *game, VcoCatalogWorkspace *work,
                              VcoReadFile read_file, void *context, VcoEncoding policy);
int vco_title_id_valid(const char *title_id);
/* Exactly one unique eligible candidate; never guess from enumeration order. */
VcoListState vco_game_select(const VcoGame *games, size_t count, VcoGame *out);
#endif
