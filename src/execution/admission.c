#include "admission.h"
#if !defined(__GCC_ATOMIC_INT_LOCK_FREE) || __GCC_ATOMIC_INT_LOCK_FREE != 2
#error "Admission requires lock-free 32-bit integer atomics"
#endif
#define CLOSED UINT32_C(0x80000000)
#define COUNT UINT32_C(0x7fffffff)
typedef char admission_size_check[sizeof(unsigned int) == sizeof(uint32_t) ? 1 : -1];

int vco_admission_enter(VcoAdmission *gate)
{
    if (!gate)
        return 0;
    uint32_t old = __atomic_load_n(&gate->state, __ATOMIC_RELAXED);
    for (;;) {
        if ((old & CLOSED) || (old & COUNT) == COUNT)
            return 0;
        if (__atomic_compare_exchange_n(&gate->state, &old, old + 1, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            return 1;
    }
}
void vco_admission_close(VcoAdmission *gate)
{
    if (gate)
        __atomic_fetch_or(&gate->state, CLOSED, __ATOMIC_ACQ_REL);
}
int vco_admission_leave(VcoAdmission *gate)
{
    if (!gate)
        return 0;
    uint32_t old = __atomic_load_n(&gate->state, __ATOMIC_RELAXED);
    for (;;) {
        if (!(old & COUNT))
            return 0;
        if (__atomic_compare_exchange_n(&gate->state, &old, old - 1, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            return 1;
    }
}
int vco_admission_drained(const VcoAdmission *gate)
{
    return gate && __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) == CLOSED;
}

int vco_admission_closed(const VcoAdmission *gate)
{
    return !gate || (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & CLOSED) != 0;
}
