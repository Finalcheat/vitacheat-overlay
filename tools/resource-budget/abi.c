/* Compile-only ARM probe. Never linked into or deployed with any plugin. */
#include "agent/vita_allocator.h"
#include "agent/vita_memory.h"
#include "agent/vita_worker.h"
#include "catalog/catalog.h"
#include <stddef.h>

struct BudgetValue {
    char name[40];
    uint32_t value;
};
typedef char record_size_must_be_44[sizeof(struct BudgetValue) == 44 ? 1 : -1];

__attribute__((used, section(".vco.resource.abi"))) const struct BudgetValue vco_resource_abi[] = {
    {"schema", 1},
    {"pointer_bytes", sizeof(void *)},
    {"size_t_bytes", sizeof(size_t)},
    {"wchar_t_bytes", sizeof(wchar_t)},
    {"entry_bytes", sizeof(VcoCheatEntry)},
    {"code_bytes", sizeof(VcoCode)},
    {"op_bytes", sizeof(VcoCheatOp)},
    {"patch_bytes", sizeof(VcoPatch)},
    {"catalog_workspace_bytes", sizeof(VcoCatalogWorkspace)},
    {"catalog_snapshot_bytes", sizeof(VcoCatalog)},
    {"game_executor_bytes", sizeof(VcoGameExecutor)},
    {"game_native_memory_bytes", sizeof(VcoGameNativeMemory)},
    {"game_pool_bytes", sizeof(VcoGamePool)},
    {"game_worker_bytes", sizeof(VcoGameWorker)},
    {"upload_max_bytes", VCO_UPLOAD_MAX},
    {"game_allocation_max_bytes", VCO_GAME_ALLOCATION_MAX},
    {"game_blocks", VCO_GAME_BLOCKS},
    {"entries_limit", VCO_ENGINE_ENTRIES},
    {"codes_limit", VCO_UPLOAD_CODES},
    {"active_limit", VCO_ENGINE_ACTIVE},
    {"claims_limit", VCO_ENGINE_CLAIMS},
};
