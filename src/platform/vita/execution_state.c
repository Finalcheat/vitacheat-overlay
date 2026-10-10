#include "patches.h"

/* Native ownership and the engine's public patch list are separate: failed
 * injection may own references without ever publishing a handle to the engine.
 * Keep this observation pure so publishing an error never triggers SDK cleanup. */
VcoError vco_vita_execution_error(const VcoVitaExecution *e)
{
    if (!e)
        return VCO_E_ARGUMENT;
    const VcoVitaMemory *native = e->data.transport.context;
    if (e->poisoned || (native && native->poisoned))
        return VCO_E_RESTORE;
    bool owns_resources = false;
    for (unsigned i = 0; i < VCO_VITA_PATCH_LIMIT; ++i) {
        const VcoVitaPatch *p = &e->patches[i];
        if (p->quarantined)
            return VCO_E_RESTORE;
        owns_resources |=
            p->active || p->referenced || p->block_referenced || p->object || p->block_object;
    }
    if (e->blocked && owns_resources)
        return VCO_E_RESTORE;
    return e->blocked && !e->retired && native && native->bound ? VCO_E_PATCH : VCO_OK;
}
