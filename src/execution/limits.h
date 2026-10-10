#ifndef VCO_EXECUTION_LIMITS_H
#define VCO_EXECUTION_LIMITS_H
/* Shared bounded execution policy. Native ownership storage must support every
 * protected patch range the engine can admit, without a smaller hidden limit. */
#define VCO_ENGINE_ENTRIES 256u
#define VCO_ENGINE_QUEUE 16u
#define VCO_ENGINE_ACTIVE 16u
#define VCO_ENGINE_PERIOD_US 50000u
#define VCO_ENGINE_ENTRY_BUDGET 256u
/* Preserve the short-entry floor. Three charges per physical code line cover
 * dispatch + one scalar copy, including single-pass pointer-chain reads and
 * guarded patch release. Compressed repetition counts never scale the budget.
 * The absolute cap also bounds callers with a larger arena than the current
 * 8192-line upload contract. This counts work, not elapsed time. */
#define VCO_ENGINE_BUDGET_PER_CODE 3u
#define VCO_ENGINE_ENTRY_BUDGET_MAX 24576u
#define VCO_ENGINE_CLAIMS 2048u
#endif
