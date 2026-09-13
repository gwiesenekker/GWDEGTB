#ifndef SHARED_CACHE_POLICY_H
#define SHARED_CACHE_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* Estimated worker elapsed load time, not CPU usage or predicted savings.
 * Require 16 timed loads (normally about 4096 actual misses) before admission. */
static inline double shared_cache_load_share(double rate, double ns, unsigned workers)
{
    return workers ? rate * ns / 1e9 / workers : 0;
}

static inline bool shared_cache_cost_eligible(double rate, double ns,
    uint64_t samples, unsigned workers, double minimum_share)
{
    return workers && samples >= 16 && rate > 0 && ns > 0 &&
        isfinite(rate) && isfinite(ns) &&
        shared_cache_load_share(rate, ns, workers) >= minimum_share;
}

static inline double shared_cache_cost_score(double rate, double ns, uint64_t extra_bytes)
{
    return extra_bytes ? rate * ns / 1e9 / ((double)extra_bytes / 1048576) : 0;
}

/* At most three fractional steps. A zero admission floor (tests/override)
 * disables acceleration rather than dividing by zero. */
static inline double shared_cache_growth_multiplier(double factor, double share, double floor)
{
    double result = factor;
    if (floor > 0 && isfinite(share)) {
        for (unsigned k = 1; k < 3 && share / floor > result; ++k)
            result *= factor;
    }
    /* Never accelerate beyond 3.375x, but honor an explicitly larger base. */
    double cap = factor > 3.375 ? factor : 3.375;
    return result > cap ? cap : result;
}
#endif
