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
#endif
