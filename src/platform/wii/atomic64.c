/* The 64-bit atomics GCC calls out of line on the Wii's 32-bit PowerPC,
 * where devkitPPC ships no libatomic. The Wii is a single core and libogc's
 * threads only switch on an interrupt, so masking interrupts around the
 * two-word access makes it atomic -- which is how libogc guards its own. */
#include <stdint.h>
#include <ogc/machine/processor.h>

#define LOCK   u32 od_level; _CPU_ISR_Disable(od_level)
#define UNLOCK _CPU_ISR_Restore(od_level)

uint64_t __atomic_load_8(const volatile void *p, int order)
{
    (void)order;
    LOCK;
    uint64_t v = *(const volatile uint64_t *)p;
    UNLOCK;
    return v;
}

void __atomic_store_8(volatile void *p, uint64_t v, int order)
{
    (void)order;
    LOCK;
    *(volatile uint64_t *)p = v;
    UNLOCK;
}

uint64_t __atomic_exchange_8(volatile void *p, uint64_t v, int order)
{
    (void)order;
    LOCK;
    uint64_t old = *(volatile uint64_t *)p;
    *(volatile uint64_t *)p = v;
    UNLOCK;
    return old;
}

uint64_t __atomic_fetch_add_8(volatile void *p, uint64_t v, int order)
{
    (void)order;
    LOCK;
    uint64_t old = *(volatile uint64_t *)p;
    *(volatile uint64_t *)p = old + v;
    UNLOCK;
    return old;
}

uint64_t __atomic_fetch_sub_8(volatile void *p, uint64_t v, int order)
{
    (void)order;
    LOCK;
    uint64_t old = *(volatile uint64_t *)p;
    *(volatile uint64_t *)p = old - v;
    UNLOCK;
    return old;
}

_Bool __atomic_compare_exchange_8(volatile void *p, void *expected, uint64_t desired,
                                  _Bool weak, int success, int failure)
{
    (void)weak; (void)success; (void)failure;
    LOCK;
    uint64_t cur = *(volatile uint64_t *)p;
    _Bool ok = cur == *(uint64_t *)expected;
    if (ok)
        *(volatile uint64_t *)p = desired;
    UNLOCK;
    if (!ok)
        *(uint64_t *)expected = cur;
    return ok;
}
