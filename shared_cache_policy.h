#ifndef SHARED_CACHE_POLICY_H
#define SHARED_CACHE_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

typedef enum { CACHE_COST_GATED, CACHE_SPARE_BUDGET, CACHE_IDLE_RECLAIM, CACHE_POLICY_COUNT } CachePolicy;
typedef struct {
    bool printed;
    double time;
    unsigned disagreement;
    uint64_t disagreement_capacity;
    unsigned suppressed;
} CachePolicyLog;

/* Routine reason/target fluctuations never bypass the minute throttle.
 * Remember disagreements across intervening agreeing/warm-up observations. */
static inline bool cache_policy_log_due(CachePolicyLog *log, double now,
                                        unsigned grow_mask, uint64_t capacity)
{
    unsigned all = (1u << CACHE_POLICY_COUNT) - 1;
    bool disagreement = grow_mask && grow_mask != all;
    bool fresh = disagreement && (log->disagreement != grow_mask ||
                                   log->disagreement_capacity != capacity);
    if (!log->printed || now < log->time || now - log->time >= 60 || fresh) {
        log->printed = true; log->time = now;
        if (disagreement) {
            log->disagreement = grow_mask;
            log->disagreement_capacity = capacity;
        }
        return true;
    }
    if (log->suppressed < UINT32_MAX) ++log->suppressed;
    return false;
}
static inline const char *cache_policy_name(CachePolicy p)
{
    return p == CACHE_IDLE_RECLAIM ? "idle-reclaim-v1" :
           p == CACHE_SPARE_BUDGET ? "spare-budget-v1" : "cost-gated-v1";
}
static inline bool cache_policy_parse(const char *name, CachePolicy *out)
{
    for (unsigned i = 0; i < CACHE_POLICY_COUNT; ++i)
        if (name && !strcmp(name, cache_policy_name((CachePolicy)i))) {
            *out = (CachePolicy)i; return true;
        }
    return false;
}
/* Stateless alternatives evaluate the SAME observed active-cache snapshot.
 * Shadow evaluation never simulates unobserved post-resize hit rates. */
static inline double cache_policy_floor(CachePolicy p, double normal, bool funded)
{
    return p != CACHE_COST_GATED && funded && normal > .001 ? .001 : normal;
}

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
