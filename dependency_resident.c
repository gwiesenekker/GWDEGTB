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
    double last_activity, resize_after;
    uint64_t last_window_lookups, last_window_decompressions;
    uint64_t previous_samples, previous_ns;
    uint64_t cost_samples;
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

void dependency_shared_phase(void *context)
{
    dependency_shared_phase_at(context, monotonic_seconds());
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
    for (ResidentEntry *e = p->entries; e; e = e->next) {
        if (!e->shared || e->stopped ||
            (egtb_shared_cache_dense(e->shared) && !e->pending_measurement)) continue;
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
            continue;
        }
        if (lookups < 100000 || !(elapsed > 0)) continue;
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
            if (++e->measurement_windows < 2) continue;
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
                continue;
            }
        }
        if (e->cooldown_windows) {
            --e->cooldown_windows;
            continue;
        }
        /* Ignore small/cold samples and implicit-draw misses, regardless of hit rate. */
        if (e->pressure_windows < 2 ||
            !shared_cache_cost_eligible(e->pressure_rate, e->decode_ns,
                e->cost_samples, p->workers, p->minimum_load_share) ||
            egtb_shared_cache_dense(e->shared) || now < e->resize_after) continue;
        uint64_t payload = egtb_shared_cache_bytes(e->shared);
        double factor = shared_cache_growth_multiplier(e->growth_factor,
            shared_cache_load_share(e->pressure_rate, e->decode_ns, p->workers),
            p->minimum_load_share);
        long double target = (long double)payload * factor;
        /* At least one additional page per side after rounding. */
        uint64_t quantum = 2 * (uint64_t)egtb_cache_page_size(e->backing);
        if (payload > SIZE_MAX - quantum) continue;
        if (target < payload + quantum) target = payload + quantum;
        payload = target >= SIZE_MAX ? SIZE_MAX : (uint64_t)target;
        uint64_t desired = egtb_shared_cache_planned_allocation(e->backing, (size_t)payload);
        uint64_t current = egtb_shared_cache_allocation(e->shared);
        if (desired > current) {
            double score = shared_cache_cost_score(e->pressure_rate, e->decode_ns, desired - current);
            if (score > unfunded_score) {
                unfunded = e; unfunded_payload = payload; unfunded_score = score;
            }
        }
        /* Reduce the requested growth to fit remaining migration headroom.
         * This is a monotone cold-path search, never part of a cache hit. */
        uint64_t lo = 0, hi = payload;
        uint64_t available = p->shared_budget - p->shared_allocated;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2 + (hi - lo) % 2;
            if (egtb_shared_cache_planned_allocation(e->backing, (size_t)mid) <= available)
                lo = mid;
            else hi = mid - 1;
        }
        payload = lo;
        uint64_t allocation = egtb_shared_cache_planned_allocation(e->backing, (size_t)payload);
        uint64_t old_allocation = egtb_shared_cache_allocation(e->shared);
        if (allocation <= old_allocation) continue;
        /* Both old and new storage coexist during migration. */
        if (allocation > p->shared_budget - p->shared_allocated) continue;
        double score = shared_cache_cost_score(e->pressure_rate, e->decode_ns,
                                               allocation - old_allocation);
        if (score > best_score) {
            best = e; best_score = score; best_payload = payload;
            best_allocation = allocation; best_rate = before_rate; best_density = before_density;
        }
    }
    if (p->donor) {
        pthread_mutex_unlock(&p->mutex);
        return; /* One reversible experiment at a time. */
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
        double started = monotonic_seconds();
        if (egtb_shared_cache_grow(best->shared, (size_t)best_payload)) {
            double growth_seconds = monotonic_seconds() - started;
            if (growth_seconds < 0) growth_seconds = 0;
            p->growth_seconds += growth_seconds;
            uint64_t actual = egtb_shared_cache_bytes(best->shared);
            p->shared_used += actual - old;
            p->shared_allocated += best_allocation - old_allocation;
            ++p->growths; best->sampled = false;
            best->pending_measurement = true;
            best->pressure_windows = best->measurement_windows = best->cooldown_windows = 0;
            best->measured_lookups = best->measured_decompressions = 0;
            best->measured_seconds = 0;
            best->last_window_lookups = best->last_window_decompressions = 0;
            best->last_window_seconds = 0;
            best->before_growth_rate = best_rate;
            best->before_growth_density = best_density;
            egtb_progress_log("shared dependency cache: database=%s maximum-index=%" PRIu64 " grew %.2f -> %.2f MiB; mode=%s; decompressions/s=%.0f; pressure=%.6f load-worker-seconds/s/additional-MiB; grow-seconds=%.6f; sampled-load-us=%.3f; estimated-load-worker-seconds/s=%.3f; estimated-load-share=%.3f%%; minimum=%.3f%%; workers=%u\n",
                entry_name(best), egtb_maximum_index(best->backing),
                (double)old / 1048576, (double)actual / 1048576,
                egtb_shared_cache_dense(best->shared) ? "dense (lazy)" : "cached",
                best_rate, best_score, growth_seconds, best->decode_ns / 1000,
                best->pressure_rate * best->decode_ns / 1e9,
                100 * shared_cache_load_share(best->pressure_rate, best->decode_ns, p->workers),
                100 * p->minimum_load_share, p->workers);
        } else {
            best->stopped = true; /* Preserve old cache; allocation failure is recoverable. */
            egtb_progress_log("shared dependency cache growth skipped: database=%s: %s\n", entry_name(best), egtb_last_error());
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
