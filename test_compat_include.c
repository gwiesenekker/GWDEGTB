/* Header coexistence and standalone linkage: deliberately no compat calls. */
#include "compat.h"
#include "egtb.h"
#include "gwdegtb.h"
#include <stdint.h>

#if defined(read) || defined(write) || defined(close) || defined(clock)
#error "compat.h must not rename standard functions by default"
#endif

_Static_assert(sizeof(int64_t) == 8, "64-bit file API required");

int main(void)
{
    return 0;
}
