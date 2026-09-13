#ifndef CACHE_MODULO_H
#define CACHE_MODULO_H
#include <stdint.h>

typedef struct { uint64_t divisor, reciprocal; int power_two; } CacheModulo;
static inline CacheModulo cache_modulo_init(uint64_t n)
{
    CacheModulo m = {n, 0, (n & (n - 1)) == 0};
    if (!m.power_two && n <= UINT32_MAX) m.reciprocal = UINT64_MAX / n + 1;
    return m;
}
static inline uint64_t cache_modulo(uint64_t page, CacheModulo m)
{
    if (m.power_two) return page & (m.divisor - 1);
    /* Exact remainder for a 32-bit numerator/divisor. The first multiplication
     * deliberately wraps to 64 bits; multiply-high alone is NOT modulo. */
    if (page <= UINT32_MAX && m.divisor <= UINT32_MAX) {
        uint64_t low = m.reciprocal * page;
        return (uint64_t)(((__uint128_t)low * m.divisor) >> 64);
    }
    /* Do not silently truncate wider page numbers in future/large formats. */
    return page % m.divisor;
}
#endif
