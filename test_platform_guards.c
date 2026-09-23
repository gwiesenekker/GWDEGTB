/* Include order must not let accidental platform calls bypass the guards. */
#include "compat.h"
#include "egtb_platform.h"

#if !defined(read) || !defined(pread) || !defined(pthread_create) || !defined(clock_gettime)
#error "platform guards are not enabled"
#endif

int main(void)
{
#ifdef TEST_FORBIDDEN_RAW_CALL
    return close(0); /* Compile-only negative test: must never be executable. */
#else
    return 0;
#endif
}
