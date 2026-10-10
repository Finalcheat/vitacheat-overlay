#include "close_key.h"
#include <string.h>
int vco_close_key_valid(const VcoEngineCloseKey *key)
{
    if (!key || !key->transaction || !key->session || !key->revision ||
        !vco_identity_valid(&key->identity))
        return 0;
    if (key->scope == VCO_CLOSE_MODULE)
        return key->process_object && key->module_object && key->module_uid > 0 &&
               key->module_generation;
    if (key->scope != VCO_CLOSE_LIST && key->scope != VCO_CLOSE_PROCESS)
        return 0;
    return (key->scope != VCO_CLOSE_PROCESS || key->process_object) && !key->module_object &&
           !key->module_uid && !key->module_generation;
}
int vco_close_key_equal(const VcoEngineCloseKey *a, const VcoEngineCloseKey *b)
{
    return a && b && a->identity.pid == b->identity.pid &&
           a->identity.instance == b->identity.instance &&
           !memcmp(a->identity.title_id, b->identity.title_id, sizeof(a->identity.title_id)) &&
           a->session == b->session && a->revision == b->revision &&
           a->transaction == b->transaction && a->scope == b->scope &&
           a->process_object == b->process_object && a->module_object == b->module_object &&
           a->module_uid == b->module_uid && a->module_generation == b->module_generation;
}
