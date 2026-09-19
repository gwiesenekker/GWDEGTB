#define _POSIX_C_SOURCE 200809L
#include "dependency_resident.h"
#include "progress.h"
#include "shared_cache_policy.h"
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct ResidentEntry {
    Egtb *backing;
    EgtbResident *resident;
    EgtbSharedCache *shared;
    bool loading, failed;
    bool shared_decided, sampled, stopped;
    uint64_t previous_lookups, previous_decompressions;
    double previous_time, before_growth_rate, before_growth_density;
    double baseline_rate, baseline_density;
    bool pending_measurement;
    unsigned pressure_windows, measurement_windows, cooldown_windows;
    uint64_t measured_lookups, measured_decompressions;
    double measured_seconds;
    double pressure_rate, last_window_seconds, decode_ns;
    double growth_factor; /* Per-cache policy state; initialized from pool default. */
    bool activity_seen;
    uint64_t activity_lookups;
    uint64_t window_lookups;
    double activity_time, activity_elapsed;
    double last_activity, resize_after;
    uint64_t last_window_lookups, last_window_decompressions;
    uint64_t previous_samples, previous_ns;
    uint64_t cost_samples;
    CachePolicyLog policy_log;
    char error[256];
    struct ResidentEntry *next;
} ResidentEntry;

struct DependencyResidentPool {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint64_t budget, maximum_database, used;
    unsigned loaded, cached;
    uint64_t shared_bytes, shared_used;
    unsigned shared_count;
    uint64_t shared_budget, shared_allocated;
    unsigned growths;
    double growth_seconds;
    double minimum_load_share;
    double growth_factor;
    unsigned workers;
    CachePolicy policy;
    const char *phase;
    CachePolicyLog selection_log;
    ResidentEntry *entries;
    ResidentEntry *donor, *receiver;
    uint64_t donor_bytes, receiver_bytes, donor_lookups, recovery_reserve;
    uint64_t donor_decompressions;
    double donor_observed_at;
    double trial_deadline;
    bool rollback_requested;
    bool rebalance;
    unsigned shrinks, transfers, rollbacks;
};

static _Thread_local char error_text[256];
static const char *entry_name(const ResidentEntry *e)
{
    if (!e) return "none";
    const char *path = egtb_path(e->backing);
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
static double monotonic_seconds(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (double)t.tv_sec + t.tv_nsec * 1e-9;
}
const char *dependency_resident_error(void) { return error_text; }

bool dependency_resident_create(DependencyResidentPool **out,
                                 uint64_t budget, uint64_t maximum_database)
{
    if (!out) return false;
    *out = NULL;
    DependencyResidentPool *p = calloc(1, sizeof(*p));
    if (!p) goto failed;
    if (pthread_mutex_init(&p->mutex, NULL)) { free(p); goto failed; }
    if (pthread_cond_init(&p->changed, NULL)) {
        pthread_mutex_destroy(&p->mutex); free(p); goto failed;
    }
    p->budget = budget; p->maximum_database = maximum_database;
    p->minimum_load_share = 0.005;
    p->growth_factor = 1.5;
    p->rebalance = true;
    p->workers = 1;
    p->phase = "setup";
    *out = p;
    return true;
failed:
    snprintf(error_text, sizeof(error_text), "cannot allocate dependency resident pool");
    return false;
}

static bool setting(const char *name, uint64_t unit, uint64_t fallback, uint64_t *out)
{
    const char *s = getenv(name);
    if (!s || !*s) { *out = fallback; return true; }
    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (*s < '0' || *s > '9' || *end || errno || n > UINT64_MAX / unit) {
        snprintf(error_text, sizeof(error_text), "invalid %s: expected a nonnegative integer", name);
        return false;
    }
    *out = n * unit;
    return true;
}

bool dependency_resident_configure(DependencyResidentPool **out)
{
    const uint64_t mib = UINT64_C(1024) * 1024;
    uint64_t budget, maximum, shared, shared_budget;
    double percent = 0.5;
    const char *threshold = getenv("EGTB_DEPENDENCY_SHARED_CACHE_MIN_LOAD_PERCENT");
    if (threshold && *threshold) {
        char *end;
        errno = 0;
        percent = strtod(threshold, &end);
        if (*threshold < '0' || *threshold > '9' || *end || errno ||
            !isfinite(percent) || percent < 0 || percent > 100) {
            snprintf(error_text, sizeof(error_text),
                "invalid EGTB_DEPENDENCY_SHARED_CACHE_MIN_LOAD_PERCENT: expected 0..100");
            return false;
        }
    }
    if (!setting("EGTB_DEPENDENCY_RESIDENT_GIB", 1024 * mib, 2048 * mib, &budget) ||
        !setting("EGTB_DEPENDENCY_RESIDENT_MAX_MIB", mib, 256 * mib, &maximum) ||
        !setting("EGTB_DEPENDENCY_SHARED_CACHE_MIB", mib, 0, &shared) ||
        !setting("EGTB_DEPENDENCY_SHARED_CACHE_GIB", 1024 * mib, 0, &shared_budget))
        return false;
    if (shared > SIZE_MAX) {
        snprintf(error_text, sizeof(error_text), "shared cache exceeds address space");
        return false;
    }
    double factor = 1.5;
    const char *growth = getenv("EGTB_DEPENDENCY_SHARED_CACHE_GROWTH");
    if (growth && *growth) {
        char *end;
        errno = 0;
        factor = strtod(growth, &end);
        if (*growth < '0' || *growth > '9' || *end || errno ||
            !isfinite(factor) || factor < 1.1 || factor > 4) {
            snprintf(error_text, sizeof(error_text),
                "invalid EGTB_DEPENDENCY_SHARED_CACHE_GROWTH: expected 1.1..4");
            return false;
        }
    }
    if (!dependency_resident_create(out, budget, maximum)) return false;
    const char *policy = getenv("EGTB_DEPENDENCY_CACHE_POLICY");
    if (policy && *policy && !dependency_shared_policy(*out, policy)) {
        dependency_resident_destroy(*out); *out = NULL;
        return false;
    }
    dependency_shared_configure(*out, (size_t)shared, shared_budget);
    (*out)->minimum_load_share = percent / 100;
    (*out)->growth_factor = factor;
    const char *rebalance = getenv("EGTB_DEPENDENCY_SHARED_CACHE_REBALANCE");
    if (rebalance && strcmp(rebalance, "0") && strcmp(rebalance, "1")) {
        dependency_resident_destroy(*out); *out = NULL;
        snprintf(error_text, sizeof(error_text), "invalid EGTB_DEPENDENCY_SHARED_CACHE_REBALANCE: expected 0 or 1");
        return false;
    }
    (*out)->rebalance = !rebalance || !strcmp(rebalance, "1");
    return true;
}

bool dependency_shared_minimum_load(DependencyResidentPool *p, double percent)
{
    if (!p || p->entries || !isfinite(percent) || percent < 0 || percent > 100)
        return false;
    p->minimum_load_share = percent / 100;
    return true;
}

bool dependency_shared_policy(DependencyResidentPool *p, const char *name)
{
    CachePolicy policy;
    if (!p || p->entries || !cache_policy_parse(name, &policy)) {
        snprintf(error_text, sizeof(error_text),
            "invalid cache policy: select cost-gated-v1, spare-budget-v1 or idle-reclaim-v1 before acquisitions");
        return false;
    }
    p->policy = policy;
    return true;
}

bool dependency_shared_configure(DependencyResidentPool *p, size_t initial, uint64_t total)
{
    if (!p || p->entries) {
        snprintf(error_text, sizeof(error_text), "configure shared caches before opening dependencies");
        return false;
    }
    p->shared_bytes = initial; p->shared_budget = total;
    return true;
}

bool dependency_shared_acquire(DependencyResidentPool *p, Egtb *backing,
                               EgtbSharedCache **out)
{
    if (!p || !backing || !out) {
        snprintf(error_text, sizeof(error_text), "invalid shared dependency request");
        return false;
    }
    *out = NULL;
    if (!p->shared_bytes) return true;
    pthread_mutex_lock(&p->mutex);
    ResidentEntry *e;
    for (e = p->entries; e && e->backing != backing; e = e->next) {}
    if (!e) {
        pthread_mutex_unlock(&p->mutex);
        snprintf(error_text, sizeof(error_text), "resident admission must precede shared cache admission");
        return false;
    }
    while (e->loading) pthread_cond_wait(&p->changed, &p->mutex);
    if (e->failed || e->resident || e->shared_decided) {
        bool ok = !e->failed;
        if (!ok) snprintf(error_text, sizeof(error_text), "%s", e->error);
        *out = e->shared;
        pthread_mutex_unlock(&p->mutex);
        return ok;
    }
    e->shared_decided = true;
    e->growth_factor = p->growth_factor;
    uint64_t allocation = egtb_shared_cache_planned_allocation(backing, (size_t)p->shared_bytes);
    if (p->shared_budget && allocation > p->shared_budget - p->shared_allocated - p->recovery_reserve) {
        pthread_mutex_unlock(&p->mutex);
        return true; /* Explicit private-cache fallback when admission cannot fit. */
    }
    p->shared_allocated += allocation; /* Reserve before concurrent allocation. */
    e->loading = true;
    pthread_mutex_unlock(&p->mutex);
    EgtbSharedCache *shared = NULL;
    bool ok = egtb_shared_cache_create(&shared, backing, (size_t)p->shared_bytes);
    char message[256] = "";
    if (!ok) snprintf(message, sizeof(message), "%s", egtb_last_error());
    pthread_mutex_lock(&p->mutex);
    e->loading = false; e->failed = !ok; e->shared = shared;
    if (ok) { ++p->shared_count; p->shared_used += egtb_shared_cache_bytes(shared); }
    else {
        p->shared_allocated -= allocation;
        snprintf(e->error, sizeof(e->error), "%s", message);
        snprintf(error_text, sizeof(error_text), "%s", message);
    }
    pthread_cond_broadcast(&p->changed);
    *out = shared;
    pthread_mutex_unlock(&p->mutex);
    return ok;
}

bool dependency_shared_adaptive(DependencyResidentPool *p)
{
    return p && p->shared_bytes && p->shared_budget;
}

/* All callers hold the pool mutex and have quiesced every probe. */
static bool coordinated_resize(DependencyResidentPool *p, ResidentEntry *e,
                               uint64_t bytes)
{
    uint64_t old = egtb_shared_cache_bytes(e->shared);
    uint64_t allocation = egtb_shared_cache_allocation(e->shared);
    uint64_t next = egtb_shared_cache_planned_allocation(e->backing, (size_t)bytes);
    if (next == allocation) return true;
    if (next > p->shared_budget - p->shared_allocated) return false;
    double started = monotonic_seconds();
    if (!egtb_shared_cache_resize(e->shared, (size_t)bytes)) return false;
    p->growth_seconds += monotonic_seconds() - started;
    p->shared_used = p->shared_used - old + egtb_shared_cache_bytes(e->shared);
    p->shared_allocated = p->shared_allocated - allocation +
                          egtb_shared_cache_allocation(e->shared);
    e->sampled = false;
    e->pending_measurement = false;
    e->pressure_windows = e->cooldown_windows = 0;
    e->last_window_seconds = 0;
    e->last_window_lookups = e->last_window_decompressions = 0;
    return true;
}

static void finish_transfer(DependencyResidentPool *p, double now, bool rollback,
                            const char *reason)
{
    if (rollback) {
        p->rollback_requested = true;
        uint64_t peak = p->shared_allocated + p->recovery_reserve;
        /* Undo receiver first: the preflight reserved this exact recovery path. */
        if (p->receiver && !coordinated_resize(p, p->receiver, p->receiver_bytes)) {
            egtb_progress_log("cache rebalance: recovery deferred (receiver=%s): %s\n", entry_name(p->receiver), egtb_last_error());
            return;
        }
        p->recovery_reserve = peak - p->shared_allocated;
        if (!coordinated_resize(p, p->donor, p->donor_bytes)) {
            egtb_progress_log("cache rebalance: recovery deferred (donor=%s): %s\n", entry_name(p->donor), egtb_last_error());
            return;
        }
        ++p->rollbacks;
    }
    egtb_progress_log("cache rebalance: %s; donor=%s receiver=%s reason=%s; donor-index=%" PRIu64
                     " allocated=%.2f MiB recovery-reserve=0\n",
                     rollback ? "rolled back" : "accepted",
                     entry_name(p->donor), entry_name(p->receiver), reason,
                     egtb_maximum_index(p->donor->backing),
                     (double)p->shared_allocated / 1048576);
    p->donor->resize_after = now + 300;
    if (p->receiver) p->receiver->resize_after = now + 60;
    p->donor = p->receiver = NULL;
    p->recovery_reserve = 0;
    p->rollback_requested = false;
}

static bool start_transfer(DependencyResidentPool *p, ResidentEntry *receiver,
                           uint64_t target, double now)
{
    if (!p->rebalance) return false;
    /* Idle is the only donor evidence in stage one. No high-hit-rate shrinking. */
    for (ResidentEntry *d = p->entries; d; d = d->next) {
        if (!d->shared || d == receiver || !d->activity_seen ||
            now - d->last_activity < 60 || now < d->resize_after ||
            d->pending_measurement) continue;
        uint64_t old = egtb_shared_cache_bytes(d->shared);
        uint64_t floor = p->shared_bytes < 1048576 ? p->shared_bytes : 1048576;
        uint64_t small = (uint64_t)(old / d->growth_factor);
        if (small < floor) small = floor;
        uint64_t da = egtb_shared_cache_allocation(d->shared);
        uint64_t dn = egtb_shared_cache_planned_allocation(d->backing, (size_t)small);
        if (!dn || dn >= da) continue;
        uint64_t ra = receiver ? egtb_shared_cache_allocation(receiver->shared) : 0;
        uint64_t rn = receiver ? egtb_shared_cache_planned_allocation(receiver->backing, (size_t)target) : 0;
        uint64_t used = p->shared_allocated;
        /* Check forward and inverse allocation peaks, without overflowing. */
        if (dn > p->shared_budget - used) continue;
        uint64_t after_shrink = used - da + dn;
        if (rn > p->shared_budget - after_shrink) continue;
        uint64_t final = after_shrink - ra + rn;
        if (ra > p->shared_budget - final) continue;
        uint64_t peak = used + dn;
        if (after_shrink + rn > peak) peak = after_shrink + rn;
        if (final + ra > peak) peak = final + ra;
        uint64_t old_receiver = receiver ? egtb_shared_cache_bytes(receiver->shared) : 0;
        double density = receiver ? receiver->baseline_density : 0;
        if (!coordinated_resize(p, d, small)) continue;
        p->donor = d; p->receiver = NULL;
        p->donor_bytes = old; p->receiver_bytes = old_receiver;
        p->donor_lookups = d->activity_lookups;
        EgtbCacheStatistics donor_stats;
        egtb_shared_cache_statistics(d->shared, &donor_stats);
        p->donor_decompressions = donor_stats.decompressions;
        p->donor_observed_at = now;
        p->recovery_reserve = peak - p->shared_allocated;
        p->trial_deadline = now + 120;
        ++p->shrinks;
        if (receiver) {
            if (!coordinated_resize(p, receiver, target)) {
                finish_transfer(p, now, true, "receiver allocation failed");
                return true;
            }
            p->receiver = receiver;
            p->recovery_reserve = peak - p->shared_allocated;
            ++p->growths; ++p->transfers;
            receiver->before_growth_density = density;
            receiver->before_growth_rate = receiver->baseline_rate;
            receiver->pending_measurement = true;
            receiver->measurement_windows = 0;
            receiver->measured_lookups = receiver->measured_decompressions = 0;
            receiver->measured_seconds = 0;
        }
        egtb_progress_log("cache rebalance: experiment; donor=%s receiver-database=%s; idle donor-index=%" PRIu64
            " %.2f -> %.2f MiB; receiver=%s index=%" PRIu64 " %.2f -> %.2f MiB; peak=%.2f MiB reserve=%.2f MiB\n",
            entry_name(d), entry_name(receiver),
            egtb_maximum_index(d->backing), (double)old / 1048576,
            (double)egtb_shared_cache_bytes(d->shared) / 1048576,
            receiver ? "grow" : "none", receiver ? egtb_maximum_index(receiver->backing) : 0,
            (double)old_receiver / 1048576,
            receiver ? (double)egtb_shared_cache_bytes(receiver->shared) / 1048576 : 0,
            (double)peak / 1048576,
            (double)p->recovery_reserve / 1048576);
        return true;
    }
    return false;
}

/* Experimental, irreversible idle reclamation. No claim of measured benefit:
 * a returning donor refills and competes for growth under the ordinary policy. */
static const char *idle_reclaim_reason(DependencyResidentPool *p, ResidentEntry *e, double now)
{
    uint64_t floor = p->shared_bytes < 1048576 ? p->shared_bytes : 1048576;
    if (!p->rebalance) return "disabled";
    if (p->donor) return "outstanding-trial";
    if (!e->activity_seen || now < e->last_activity || now - e->last_activity < 60)
        return "recent-lookups-or-phase-grace";
    if (now < e->resize_after) return "resize-protection";
    if (e->pending_measurement) return "assessing-growth";
    if (egtb_shared_cache_bytes(e->shared) <= floor) return "at-floor";
    uint64_t minimum = egtb_shared_cache_planned_allocation(e->backing,
                          2 * (size_t)egtb_cache_page_size(e->backing));
    if (minimum > p->shared_budget - p->shared_allocated) return "fallback-budget";
    return "eligible-if-receiver-needs-memory";
}

static bool reclaim_idle(DependencyResidentPool *p, ResidentEntry *receiver, double now)
{
    ResidentEntry *best = NULL;
    uint64_t largest = 0;
    for (ResidentEntry *e = p->entries; e; e = e->next) {
        if (!e->shared || e == receiver ||
            strcmp(idle_reclaim_reason(p,e,now), "eligible-if-receiver-needs-memory")) continue;
        uint64_t allocation = egtb_shared_cache_allocation(e->shared);
        if (allocation > largest) { best = e; largest = allocation; }
    }
    if (!best) return false;
    uint64_t floor = p->shared_bytes < 1048576 ? p->shared_bytes : 1048576;
    uint64_t old = egtb_shared_cache_bytes(best->shared);
    double started = monotonic_seconds();
    bool ok = egtb_shared_cache_discard_resize(best->shared, (size_t)floor);
    p->growth_seconds += monotonic_seconds() - started;
    uint64_t actual = egtb_shared_cache_bytes(best->shared);
    p->shared_used = p->shared_used - old + actual;
    p->shared_allocated = p->shared_allocated - largest + egtb_shared_cache_allocation(best->shared);
    best->sampled = best->pending_measurement = false;
    best->pressure_windows = best->cooldown_windows = 0;
    best->pressure_rate = best->last_window_seconds = 0;
    best->resize_after = now + 300; /* Protect against repeated shrink/regrow. */
    if (actual < old) ++p->shrinks;
    egtb_progress_log("cache idle reclamation: policy=idle-reclaim-v1 donor=%s receiver=%s "
        "idle-seconds=%.1f payload-MiB=%.2f->%.2f allocated-MiB=%.2f "
        "result=%s recovery=lazy-refill-no-reservation\n",entry_name(best),entry_name(receiver),
        now-best->last_activity,(double)old/1048576,(double)actual/1048576,
        (double)p->shared_allocated/1048576,ok ? "reclaimed" : "allocation-failed-fallback");
    return true;
}

/* Called by the coordinator with all dependency probes quiescent. No mutex,
 * counter write, clock query or resize check is added to the lookup path. */
void dependency_shared_maintain(void *context, unsigned workers)
{
    DependencyResidentPool *p = context;
    if (!p || !workers) return;
    p->workers = workers;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return;
    dependency_shared_maintain_at(context, (double)now.tv_sec + now.tv_nsec * 1e-9);
}

/* Explicit quiescent phase boundary, including each slice's initialization and
 * verification. Never carry an experiment or an idle interval across workloads. */
void dependency_shared_phase_at(DependencyResidentPool *p, double now)
{
    if (!p) return;
    pthread_mutex_lock(&p->mutex);
    if (p->donor) finish_transfer(p, now, true, "phase boundary");
    for (ResidentEntry *e = p->entries; e; e = e->next) {
        if (!e->shared) continue;
        EgtbCacheStatistics s;
        egtb_shared_cache_statistics(e->shared, &s);
        e->last_activity = now;
        e->activity_seen = true;
        e->activity_lookups = s.lookups;
        e->activity_time = now;
        e->window_lookups = 0; e->activity_elapsed = 0;
        e->previous_lookups = s.lookups;
        e->previous_decompressions = s.decompressions;
        e->sampled = e->pending_measurement = false;
        e->pressure_windows = e->cooldown_windows = 0;
        e->pressure_rate = e->last_window_seconds = 0;
        e->last_window_lookups = e->last_window_decompressions = 0;
        e->baseline_rate = e->baseline_density = 0;
        egtb_shared_cache_timing(e->shared, &e->previous_samples, &e->previous_ns);
    }
    /* Failed recovery keeps rollback_requested and its reservation intact. */
    pthread_mutex_unlock(&p->mutex);
}

void dependency_shared_phase(void *context, const char *phase)
{
    DependencyResidentPool *p = context;
    if (!p) return;
    /* Phase strings are generator-owned static literals. Selection remains fixed. */
    p->phase = phase;
    for (ResidentEntry *e = p->entries; e; e = e->next)
        memset(&e->policy_log, 0, sizeof(e->policy_log));
    memset(&p->selection_log, 0, sizeof(p->selection_log));
    egtb_progress_log("cache policy phase: phase=%s active=%s shadows=all-other-policies\n",
                      phase, cache_policy_name(p->policy));
    dependency_shared_phase_at(context, monotonic_seconds());
}

typedef struct {
    uint64_t payload, allocation, unfunded_payload;
    double floor, score, unfunded_score;
    const char *reason;
} CacheDecision;

/* Pure decision function: all alternatives see identical measurements and
 * allocation state. Only the selected policy's result reaches the executor. */
static CacheDecision policy_decide(DependencyResidentPool *p, ResidentEntry *e,
                                   CachePolicy policy)
{
    CacheDecision d = {.floor = p->minimum_load_share, .reason = "below-load-threshold"};
    uint64_t current = egtb_shared_cache_allocation(e->shared);
    uint64_t old = egtb_shared_cache_bytes(e->shared);
    uint64_t available = p->shared_budget > p->shared_allocated ?
                         p->shared_budget - p->shared_allocated : 0;
    uint64_t quantum = 2 * (uint64_t)egtb_cache_page_size(e->backing);
    /* Ordinary growth may discard old contents at the quiescent checkpoint.
     * Reserve a tiny fallback so allocation failure never strands probes. The
     * coordinator's reversible transfers keep their stricter overlap rules. */
    uint64_t fallback = egtb_shared_cache_planned_allocation(e->backing, (size_t)quantum);
    uint64_t capacity_budget = available >= fallback ? available + current - fallback : available;
    if (old > SIZE_MAX - quantum) { d.reason = "size-limit"; return d; }
    bool funded = egtb_shared_cache_planned_allocation(e->backing, (size_t)(old + quantum)) <= capacity_budget;
    d.floor = cache_policy_floor(policy, p->minimum_load_share, funded);
    if (e->cost_samples < 16) { d.reason = "insufficient-timed-loads"; return d; }
    if (!shared_cache_cost_eligible(e->pressure_rate, e->decode_ns,
            e->cost_samples, p->workers, d.floor)) return d;
    double factor = shared_cache_growth_multiplier(e->growth_factor,
        shared_cache_load_share(e->pressure_rate, e->decode_ns, p->workers), d.floor);
    long double target = (long double)old * factor;
    if (target < old + quantum) target = old + quantum;
    uint64_t payload = target >= SIZE_MAX ? SIZE_MAX : (uint64_t)target;
    /* A dense jump is allowed only if the complete new allocation consumes at
     * most half the currently free pool; old storage still coexists with it. */
    if (policy != CACHE_COST_GATED && funded) {
        uint64_t dense = egtb_shared_cache_planned_allocation(e->backing, SIZE_MAX);
        if (dense <= available / 2) payload = SIZE_MAX;
    }
    uint64_t desired = egtb_shared_cache_planned_allocation(e->backing, (size_t)payload);
    /* Redistribution must always clear the original, stricter floor. */
    if (desired > current && shared_cache_cost_eligible(e->pressure_rate, e->decode_ns,
            e->cost_samples, p->workers, p->minimum_load_share)) {
        d.unfunded_payload = payload;
        d.unfunded_score = shared_cache_cost_score(e->pressure_rate, e->decode_ns, desired - current);
    }
    uint64_t lo = 0, hi = payload;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2 + (hi - lo) % 2;
        if (egtb_shared_cache_planned_allocation(e->backing, (size_t)mid) <= capacity_budget)
            lo = mid;
        else hi = mid - 1;
    }
    d.payload = lo;
    d.allocation = egtb_shared_cache_planned_allocation(e->backing, (size_t)lo);
    if (d.allocation <= current) { d.reason = "capacity-budget"; d.payload = 0; return d; }
    d.score = shared_cache_cost_score(e->pressure_rate, e->decode_ns, d.allocation - current);
    d.reason = policy != CACHE_COST_GATED && funded ? "spare-budget" : "load-cost";
    /* Report a bounded actual payload, even for the SIZE_MAX dense request. */
    uint64_t full = egtb_shared_cache_planned_allocation(e->backing, SIZE_MAX);
    if (d.allocation == full) {
        /* Find the minimum request yielding this allocation. */
        uint64_t a = old, b = lo;
        while (a < b) {
            uint64_t mid = a + (b - a) / 2;
            if (egtb_shared_cache_planned_allocation(e->backing, (size_t)mid) >= full) b = mid;
            else a = mid + 1;
        }
        d.payload = a;
    }
    return d;
}

static void policy_log(DependencyResidentPool *p, ResidentEntry *e,
                       const CacheDecision decisions[CACHE_POLICY_COUNT], double now)
{
    unsigned mask = 0;
    for (unsigned i=0; i<CACHE_POLICY_COUNT; ++i)
        if (decisions[i].payload) mask |= 1u << i;
    if (!cache_policy_log_due(&e->policy_log,now,mask,egtb_shared_cache_bytes(e->shared))) return;
    egtb_progress_log("cache policy evaluation: phase=%s database=%s active=%s "
        "current-MiB=%.2f lookups-window=%" PRIu64 " window-seconds=%.3f lookups/s=%.0f idle-seconds=%.1f decompressions/s=%.0f sampled-load-us=%.3f "
        "load-share=%.4f%% timed-loads=%" PRIu64
        " pressure-windows=%u cooldown=%u free-MiB=%.2f suppressed=%u\n",
        p->phase,entry_name(e),cache_policy_name(p->policy),
        (double)egtb_shared_cache_bytes(e->shared)/1048576,
        e->window_lookups,e->activity_elapsed,
        e->activity_elapsed > 0 ? e->window_lookups / e->activity_elapsed : 0,
        now > e->last_activity ? now - e->last_activity : 0,
        e->pressure_rate, e->decode_ns/1000,
        100*shared_cache_load_share(e->pressure_rate,e->decode_ns,p->workers),
        e->cost_samples,e->pressure_windows,e->cooldown_windows,
        (double)(p->shared_budget-p->shared_allocated)/1048576,e->policy_log.suppressed);
    for (unsigned i=0; i<CACHE_POLICY_COUNT; ++i)
        egtb_progress_log("  cache policy decision: policy=%s mode=%s decision=%s reason=%s "
            "target-MiB=%.2f threshold=%.4f%%\n",cache_policy_name((CachePolicy)i),
            i == (unsigned)p->policy ? "active" : "shadow",
            decisions[i].payload ? "candidate-grow" : "keep",decisions[i].reason,
            (double)decisions[i].payload/1048576,100*decisions[i].floor);
    egtb_progress_log("  cache idle donor: policy=idle-reclaim-v1 mode=%s reason=%s\n",
        p->policy == CACHE_IDLE_RECLAIM ? "active" : "shadow",idle_reclaim_reason(p,e,now));
    e->policy_log.suppressed = 0;
}

static void policy_guard_log(DependencyResidentPool *p, ResidentEntry *e,
                             const char *reason, double now)
{
    CacheDecision decisions[CACHE_POLICY_COUNT];
    for (unsigned i=0; i<CACHE_POLICY_COUNT; ++i)
        decisions[i] = (CacheDecision){.reason=reason,.floor=p->minimum_load_share};
    policy_log(p,e,decisions,now);
}

static bool donor_under_pressure(DependencyResidentPool *p, double now)
{
    EgtbCacheStatistics s;
    egtb_shared_cache_statistics(p->donor->shared, &s);
    if (s.decompressions < p->donor_decompressions || now < p->donor_observed_at) {
        p->donor_decompressions = s.decompressions;
        p->donor_observed_at = now;
        return false;
    }
    uint64_t count = s.decompressions - p->donor_decompressions;
    double elapsed = now - p->donor_observed_at;
    if (!count) p->donor_observed_at = now;
    if (count < 256 || elapsed <= 0) return false;
    /* Preserve historical measured cost after shrink; absent that, use a
     * conservative 5 us/load fallback, still normalized by elapsed worker time. */
    double ns = p->donor->decode_ns > 0 ? p->donor->decode_ns : 5000;
    bool pressure = shared_cache_load_share(count / elapsed, ns, p->workers) >=
                    p->minimum_load_share;
    p->donor_decompressions = s.decompressions;
    p->donor_observed_at = now;
    return pressure;
}

void dependency_shared_maintain_at(DependencyResidentPool *p, double now)
{
    if (!dependency_shared_adaptive(p)) return;
    pthread_mutex_lock(&p->mutex);
    /* Track dense and cold entries as well. Counter resets restart idle grace. */
    for (ResidentEntry *e = p->entries; e; e = e->next) {
        if (!e->shared) continue;
        EgtbCacheStatistics s;
        egtb_shared_cache_statistics(e->shared, &s);
        bool valid_window = e->activity_seen && s.lookups >= e->activity_lookups && now > e->activity_time;
        e->window_lookups = valid_window ? s.lookups - e->activity_lookups : 0;
        e->activity_elapsed = valid_window ? now - e->activity_time : 0;
        e->activity_time = now;
        if (!e->activity_seen || s.lookups != e->activity_lookups || now < e->last_activity) {
            e->last_activity = now;
            e->activity_seen = true;
        }
        e->activity_lookups = s.lookups;
    }
    if (p->donor && (p->rollback_requested || donor_under_pressure(p, now))) {
        finish_transfer(p, now, true, p->rollback_requested ? "deferred recovery" : "donor pressure");
        pthread_mutex_unlock(&p->mutex);
        return;
    }
    if (p->donor && now >= p->trial_deadline) {
        /* No evidence for a transfer by the deadline: undo it. Idle-only
         * reclamation may be accepted after the complete observation period. */
        finish_transfer(p, now, p->receiver != NULL,
                        p->receiver ? "inconclusive deadline" : "idle observation complete");
        pthread_mutex_unlock(&p->mutex);
        return;
    }
    ResidentEntry *best = NULL;
    ResidentEntry *unfunded = NULL;
    uint64_t unfunded_payload = 0;
    double unfunded_score = 0;
    uint64_t best_payload = 0, best_allocation = 0;
    double best_score = 0, best_rate = 0, best_density = 0;
    double best_floor = p->minimum_load_share;
    ResidentEntry *policy_best[CACHE_POLICY_COUNT] = {0};
    CacheDecision policy_choice[CACHE_POLICY_COUNT] = {0};
    for (ResidentEntry *e = p->entries; e; e = e->next) {
        if (!e->shared) continue;
        if (e->stopped || (egtb_shared_cache_dense(e->shared) && !e->pending_measurement)) {
            policy_guard_log(p,e,e->stopped ? "allocation-failed" : "dense",now);
            continue;
        }
        EgtbCacheStatistics s;
        egtb_shared_cache_statistics(e->shared, &s);
        if (s.lookups < e->previous_lookups || s.decompressions < e->previous_decompressions ||
            (e->sampled && now < e->previous_time)) {
            e->sampled = false;
            e->previous_lookups = 0; e->previous_decompressions = 0;
            e->pending_measurement = false;
            if (p->receiver == e) {
                finish_transfer(p, now, true, "measurement counters reset");
                pthread_mutex_unlock(&p->mutex);
                return;
            }
            e->pressure_windows = e->cooldown_windows = 0;
            e->pressure_rate = e->last_window_seconds = 0;
            e->last_window_lookups = e->last_window_decompressions = 0;
            e->baseline_rate = e->baseline_density = 0;
            e->previous_samples = e->previous_ns = e->cost_samples = 0;
            e->decode_ns = 0;
        }
        if (!e->sampled) {
            /* First interval is warm-up, also after a resize. */
            policy_guard_log(p,e,"warm-up",now);
            if (s.lookups == e->previous_lookups) continue;
            e->previous_lookups = s.lookups; e->previous_decompressions = s.decompressions;
            e->previous_time = now;
            e->sampled = true;
            continue;
        }
        uint64_t lookups = s.lookups - e->previous_lookups;
        uint64_t decompressions = s.decompressions - e->previous_decompressions;
        double elapsed = now - e->previous_time;
        if (!lookups) {
            if (elapsed > 0) e->pressure_rate *= 5.0 / (5.0 + elapsed);
            e->previous_time = now;
            policy_guard_log(p,e,"idle",now);
            continue;
        }
        if (lookups < 100000 || !(elapsed > 0)) {
            policy_guard_log(p,e,"insufficient-observations",now); continue;
        }
        double rate = (double)decompressions / elapsed;
        /* Time-aware exponential smoothing approximation (5-second time constant).
         * Quiet windows decay history rather than deleting a burst immediately. */
        double weight = elapsed / (5.0 + elapsed);
        e->pressure_rate = e->pressure_windows ?
            e->pressure_rate + weight * (rate - e->pressure_rate) : rate;
        double before_rate = (double)(decompressions + e->last_window_decompressions) /
                             (elapsed + e->last_window_seconds);
        double before_density = (double)(decompressions + e->last_window_decompressions) * 1000000 /
                                (lookups + e->last_window_lookups);
        e->baseline_rate = before_rate;
        e->baseline_density = before_density;
        e->last_window_seconds = elapsed;
        e->last_window_lookups = lookups;
        e->last_window_decompressions = decompressions;
        e->previous_lookups = s.lookups; e->previous_decompressions = s.decompressions;
        e->previous_time = now;
        /* Two substantial observations, not necessarily adjacent. Smoothing
         * retains bursts across quiet windows; the cost floor rejects old ones. */
        if (decompressions >= 1000 && e->pressure_windows < 2) ++e->pressure_windows;
        uint64_t samples, ns;
        egtb_shared_cache_timing(e->shared, &samples, &ns);
        if (samples < e->previous_samples || ns < e->previous_ns) {
            e->previous_samples = e->previous_ns = e->cost_samples = 0;
            e->decode_ns = 0;
        }
        if (samples > e->previous_samples && ns >= e->previous_ns) {
            double cost = (double)(ns - e->previous_ns) / (samples - e->previous_samples);
            e->decode_ns = e->decode_ns ? e->decode_ns + weight * (cost - e->decode_ns) : cost;
            e->cost_samples = samples - e->previous_samples >= 16 - e->cost_samples ?
                16 : e->cost_samples + samples - e->previous_samples;
        }
        e->previous_samples = samples; e->previous_ns = ns;
        if (e->pending_measurement) {
            e->measured_lookups += lookups;
            e->measured_decompressions += decompressions;
            e->measured_seconds += elapsed;
            if (++e->measurement_windows < 2) {
                policy_guard_log(p,e,"assessing-growth",now); continue;
            }
            double measured_rate = e->measured_decompressions / e->measured_seconds;
            double measured_density = (double)e->measured_decompressions * 1000000 / e->measured_lookups;
            bool weak = measured_density > e->before_growth_density * 0.90 &&
                        !egtb_shared_cache_dense(e->shared);
            egtb_progress_log("shared dependency cache measurement: database=%s maximum-index=%" PRIu64
                " decompressions/s=%.0f -> %.0f; decompressions/million-lookups=%.1f -> %.1f; extra-cooldown=%u windows (observed, workload may differ)\n",
                entry_name(e), egtb_maximum_index(e->backing), e->before_growth_rate, measured_rate,
                e->before_growth_density, measured_density, weak ? 2u : 0u);
            e->pending_measurement = false;
            if (p->receiver == e) {
                finish_transfer(p, now, weak, weak ? "weak receiver benefit" : "receiver benefit confirmed");
                pthread_mutex_unlock(&p->mutex);
                return;
            }
            if (weak) {
                e->cooldown_windows = 2;
                policy_guard_log(p,e,"weak-growth-cooldown",now);
                continue;
            }
        }
        if (e->cooldown_windows) {
            --e->cooldown_windows;
            policy_guard_log(p,e,"cooldown",now);
            continue;
        }
        /* Ignore small/cold samples and implicit-draw misses, regardless of hit rate. */
        const char *guard = e->pressure_windows < 2 ? "insufficient-pressure-windows" :
            egtb_shared_cache_dense(e->shared) ? "dense" :
            now < e->resize_after && p->policy != CACHE_IDLE_RECLAIM ? "resize-protection" : NULL;
        if (guard) { policy_guard_log(p,e,guard,now); continue; }
        CacheDecision decisions[CACHE_POLICY_COUNT];
        for (unsigned i=0; i<CACHE_POLICY_COUNT; ++i) {
            CacheDecision d = policy_decide(p,e,(CachePolicy)i);
            decisions[i] = d;
            if (d.score > policy_choice[i].score) {
                policy_best[i] = e; policy_choice[i] = d;
            }
            if (i != (unsigned)p->policy) continue;
            if (d.unfunded_score > unfunded_score) {
                unfunded = e; unfunded_payload = d.unfunded_payload; unfunded_score = d.unfunded_score;
            }
            if (d.score > best_score) {
                best = e; best_score = d.score; best_payload = d.payload;
                best_allocation = d.allocation; best_rate = before_rate; best_density = before_density;
                best_floor = d.floor;
            }
        }
        policy_log(p,e,decisions,now);
    }
    bool have_choice = false;
    for (unsigned i=0; i<CACHE_POLICY_COUNT; ++i) have_choice |= policy_best[i] != NULL;
    bool log_selection = have_choice && cache_policy_log_due(&p->selection_log,now,0,0);
    for (unsigned i=0; log_selection && i<CACHE_POLICY_COUNT; ++i) {
        if (policy_best[i])
            egtb_progress_log("cache policy selection: policy=%s mode=%s phase=%s database=%s "
                "decision=%s target-MiB=%.2f snapshot=active-cache-state\n",
                cache_policy_name((CachePolicy)i), i == (unsigned)p->policy ? "active" : "shadow",
                p->phase,entry_name(policy_best[i]),p->donor ? "defer-outstanding-trial" : "grow",
                (double)policy_choice[i].payload/1048576);
    }
    if (p->donor) {
        pthread_mutex_unlock(&p->mutex);
        return; /* One reversible experiment at a time. */
    }
    if (!best && p->policy == CACHE_IDLE_RECLAIM) {
        if (unfunded) reclaim_idle(p, unfunded, now);
        pthread_mutex_unlock(&p->mutex);
        return; /* No reversible idle-only trials under this experimental policy. */
    }
    if (!best && unfunded && start_transfer(p, unfunded, unfunded_payload, now)) {
        pthread_mutex_unlock(&p->mutex);
        return;
    }
    if (!best && !unfunded && p->shared_allocated > p->shared_budget / 10 * 9)
        start_transfer(p, NULL, 0, now);
    if (best) {
        uint64_t old = egtb_shared_cache_bytes(best->shared);
        uint64_t old_allocation = egtb_shared_cache_allocation(best->shared);
        uint64_t free_bytes = p->shared_budget > p->shared_allocated ?
                              p->shared_budget - p->shared_allocated : 0;
        bool discard = best_allocation > free_bytes;
        double started = monotonic_seconds();
        bool grown = discard ? egtb_shared_cache_discard_grow(best->shared, (size_t)best_payload) :
                               egtb_shared_cache_grow(best->shared, (size_t)best_payload);
        /* Failure in discard mode may leave the minimal fallback cache. */
        uint64_t actual_bytes = egtb_shared_cache_bytes(best->shared);
        p->shared_used = p->shared_used - old + actual_bytes;
        p->shared_allocated = p->shared_allocated - old_allocation +
                              egtb_shared_cache_allocation(best->shared);
        if (grown) {
            double growth_seconds = monotonic_seconds() - started;
            if (growth_seconds < 0) growth_seconds = 0;
            p->growth_seconds += growth_seconds;
            uint64_t actual = egtb_shared_cache_bytes(best->shared);
            ++p->growths; best->sampled = false;
            best->pending_measurement = true;
            best->pressure_windows = best->measurement_windows = best->cooldown_windows = 0;
            best->measured_lookups = best->measured_decompressions = 0;
            best->measured_seconds = 0;
            best->last_window_lookups = best->last_window_decompressions = 0;
            best->last_window_seconds = 0;
            best->before_growth_rate = best_rate;
            best->before_growth_density = best_density;
            egtb_progress_log("shared dependency cache resize: database=%s strategy=%s; old contents=%s\n",
                entry_name(best), discard ? "discard-and-grow" : "migrate",
                discard ? "discarded; lazy refill" : "migrated");
            egtb_progress_log("shared dependency cache: database=%s maximum-index=%" PRIu64 " grew %.2f -> %.2f MiB; mode=%s; decompressions/s=%.0f; pressure=%.6f load-worker-seconds/s/additional-MiB; grow-seconds=%.6f; sampled-load-us=%.3f; estimated-load-worker-seconds/s=%.3f; estimated-load-share=%.3f%%; minimum=%.3f%%; workers=%u\n",
                entry_name(best), egtb_maximum_index(best->backing),
                (double)old / 1048576, (double)actual / 1048576,
                egtb_shared_cache_dense(best->shared) ? "dense (lazy)" : "cached",
                best_rate, best_score, growth_seconds, best->decode_ns / 1000,
                best->pressure_rate * best->decode_ns / 1e9,
                100 * shared_cache_load_share(best->pressure_rate, best->decode_ns, p->workers),
                100 * best_floor, p->workers);
        } else {
            best->stopped = true; /* Usable old or minimal fallback cache remains. */
            egtb_progress_log("shared dependency cache growth skipped: database=%s remaining-MiB=%.2f: %s\n",
                entry_name(best), (double)actual_bytes / 1048576, egtb_last_error());
        }
    }
    pthread_mutex_unlock(&p->mutex);
}

bool dependency_resident_acquire(DependencyResidentPool *p, Egtb *backing,
                                  const EgtbResident **out)
{
    if (!p || !backing || !out) return false;
    *out = NULL;
    pthread_mutex_lock(&p->mutex);
    ResidentEntry *e;
    for (e = p->entries; e && e->backing != backing; e = e->next) {}
    if (e) {
        while (e->loading) pthread_cond_wait(&p->changed, &p->mutex);
        bool ok = !e->failed;
        if (!ok) snprintf(error_text, sizeof(error_text), "%s", e->error);
        *out = e->resident;
        pthread_mutex_unlock(&p->mutex);
        return ok;
    }
    e = calloc(1, sizeof(*e));
    if (!e) {
        pthread_mutex_unlock(&p->mutex);
        snprintf(error_text, sizeof(error_text), "cannot allocate dependency pool entry");
        return false;
    }
    e->backing = backing; e->next = p->entries; p->entries = e;
    uint64_t maximum = egtb_maximum_index(backing);
    bool fits = maximum < UINT64_MAX / sizeof(EgtbEntry);
    uint64_t bytes = fits ? (maximum + 1) * sizeof(EgtbEntry) : UINT64_MAX;
    if (!fits || bytes > SIZE_MAX || bytes > p->maximum_database ||
        bytes > p->budget - p->used) {
        ++p->cached;
        pthread_mutex_unlock(&p->mutex);
        return true;
    }
    /* Reserve before dropping the lock, so concurrent loads cannot overspend.
     * Admit once, never evict: borrowed pointers stay valid for the whole run. */
    p->used += bytes;
    e->loading = true;
    pthread_mutex_unlock(&p->mutex);
    EgtbResident *resident = NULL;
    bool ok = egtb_resident_load_quiet(&resident, backing);
    char message[256] = "";
    if (!ok) snprintf(message, sizeof(message), "%s", egtb_last_error());
    pthread_mutex_lock(&p->mutex);
    e->resident = resident; e->failed = !ok; e->loading = false;
    if (ok) ++p->loaded;
    else {
        p->used -= bytes;
        snprintf(e->error, sizeof(e->error), "%s", message);
        snprintf(error_text, sizeof(error_text), "%s", message);
    }
    pthread_cond_broadcast(&p->changed);
    *out = resident;
    pthread_mutex_unlock(&p->mutex);
    return ok;
}

uint64_t dependency_resident_used(DependencyResidentPool *p)
{
    pthread_mutex_lock(&p->mutex);
    uint64_t used = p->used;
    pthread_mutex_unlock(&p->mutex);
    return used;
}

void dependency_shared_coordinator_statistics(DependencyResidentPool *p,
                                              DependencyCoordinatorStatistics *out)
{
    pthread_mutex_lock(&p->mutex);
    *out = (DependencyCoordinatorStatistics){p->shared_allocated, p->recovery_reserve,
        p->shrinks, p->transfers, p->rollbacks, p->donor != NULL};
    pthread_mutex_unlock(&p->mutex);
}

void dependency_resident_report(DependencyResidentPool *p)
{
    pthread_mutex_lock(&p->mutex);
    if (p->entries) {
        printf("dependency memory at report time (shared allocations counted once):\n");
        printf("  %-27s %12s %12s %14s %10s %s\n", "Database", "Full MiB",
               "Payload MiB", "Allocated MiB", "Coverage %", "Mode");
        for (ResidentEntry *e = p->entries; e; e = e->next) {
            double full = ((double)egtb_maximum_index(e->backing) + 1) * 4;
            if (!e->shared && !e->resident) {
                printf("  %-27s %12.2f %12s %14s %10s private\n",
                       entry_name(e), full / 1048576, "-", "-", "-");
                continue;
            }
            uint64_t payload = e->shared ? egtb_shared_cache_bytes(e->shared) :
                                         egtb_resident_bytes(e->resident);
            uint64_t allocation = e->shared ? egtb_shared_cache_allocation(e->shared) : payload;
            double coverage = full > 0 ? 100 * payload / full : 0;
            if (coverage > 100) coverage = 100;
            printf("  %-27s %12.2f %12.2f %14.2f %10.2f %s\n", entry_name(e),
                   full / 1048576, (double)payload / 1048576,
                   (double)allocation / 1048576, coverage,
                   e->resident ? "resident" : egtb_shared_cache_dense(e->shared) ? "dense-lazy" : "cached");
        }
        puts("  Coverage is capacity/full size, not occupancy or hit rate. Allocated includes shared-slot metadata; excludes contexts and private views.");
    }
    printf("shared dependency residency: budget=%" PRIu64 " MiB maximum-database=%" PRIu64
           " MiB used=%.2f MiB loaded=%u cached=%u\n",
           p->budget / 1048576, p->maximum_database / 1048576,
           (double)p->used / 1048576, p->loaded, p->cached);
    if (p->shared_bytes)
        printf("optimistic dependency caches: %s=%" PRIu64
               " MiB used=%.2f MiB databases=%u\n",
               p->shared_budget ? "initial-database" : "maximum-database", p->shared_bytes / 1048576,
               (double)p->shared_used / 1048576, p->shared_count);
    if (p->shared_budget)
        printf("adaptive shared-cache budget: %" PRIu64 " MiB allocated=%.2f MiB growths=%u (metadata included)\n",
               p->shared_budget / 1048576, (double)p->shared_allocated / 1048576, p->growths);
    if (p->shared_budget)
        printf("shared-cache growth stalls: %.6f s total\n", p->growth_seconds);
    if (p->shared_budget)
        printf("shared-cache coordinator: shrinks=%u transfers=%u rollbacks=%u recovery-reserve=%.2f MiB assessing=%s\n",
               p->shrinks, p->transfers, p->rollbacks,
               (double)p->recovery_reserve / 1048576, p->donor ? "yes" : "no");
    if (p->shared_budget)
        printf("shared-cache policy: active=%s shadows=all-other-policies snapshot=active-cache-state "
               "spare-floor=%.3f%% dense-jump=half-free-budget\n",
               cache_policy_name(p->policy),100*cache_policy_floor(CACHE_SPARE_BUDGET,p->minimum_load_share,true));
    if (p->shared_budget)
        printf("shared-cache growth admission: minimum-load-share=%.3f%% minimum-timed-loads=16 growth-factor=%.3f one-growth-per-checkpoint\n",
               100 * p->minimum_load_share, p->growth_factor);
    pthread_mutex_unlock(&p->mutex);
}

void dependency_resident_destroy(DependencyResidentPool *p)
{
    if (!p) return;
    while (p->entries) {
        ResidentEntry *e = p->entries; p->entries = e->next;
        egtb_resident_destroy(e->resident);
        egtb_shared_cache_destroy(e->shared); free(e);
    }
    pthread_cond_destroy(&p->changed);
    pthread_mutex_destroy(&p->mutex);
    free(p);
}
