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
    bool pending_measurement;
    unsigned pressure_windows, measurement_windows, cooldown_windows;
    uint64_t measured_lookups, measured_decompressions;
    double measured_seconds;
    double pressure_rate, last_window_seconds, decode_ns;
    double growth_factor; /* Per-cache policy state; initialized from pool default. */
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
};

static _Thread_local char error_text[256];
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
    if (p->shared_budget && allocation > p->shared_budget - p->shared_allocated) {
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

void dependency_shared_maintain_at(DependencyResidentPool *p, double now)
{
    if (!dependency_shared_adaptive(p)) return;
    pthread_mutex_lock(&p->mutex);
    ResidentEntry *best = NULL;
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
            e->pressure_windows = e->cooldown_windows = 0;
            e->pressure_rate = e->last_window_seconds = 0;
            e->last_window_lookups = e->last_window_decompressions = 0;
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
            egtb_progress_log("shared dependency cache measurement: maximum-index=%" PRIu64
                " decompressions/s=%.0f -> %.0f; decompressions/million-lookups=%.1f -> %.1f; extra-cooldown=%u windows (observed, workload may differ)\n",
                egtb_maximum_index(e->backing), e->before_growth_rate, measured_rate,
                e->before_growth_density, measured_density, weak ? 2u : 0u);
            e->pending_measurement = false;
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
            egtb_shared_cache_dense(e->shared)) continue;
        uint64_t payload = egtb_shared_cache_bytes(e->shared);
        long double target = (long double)payload * e->growth_factor;
        /* At least one additional page per side after rounding. */
        uint64_t quantum = 2 * (uint64_t)egtb_cache_page_size(e->backing);
        if (payload > SIZE_MAX - quantum) continue;
        if (target < payload + quantum) target = payload + quantum;
        payload = target >= SIZE_MAX ? SIZE_MAX : (uint64_t)target;
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
            egtb_progress_log("shared dependency cache: maximum-index=%" PRIu64 " grew %.2f -> %.2f MiB; mode=%s; decompressions/s=%.0f; pressure=%.6f load-worker-seconds/s/additional-MiB; grow-seconds=%.6f; sampled-load-us=%.3f; estimated-load-worker-seconds/s=%.3f; estimated-load-share=%.3f%%; minimum=%.3f%%; workers=%u\n",
                egtb_maximum_index(best->backing),
                (double)old / 1048576, (double)actual / 1048576,
                egtb_shared_cache_dense(best->shared) ? "dense (lazy)" : "cached",
                best_rate, best_score, growth_seconds, best->decode_ns / 1000,
                best->pressure_rate * best->decode_ns / 1e9,
                100 * shared_cache_load_share(best->pressure_rate, best->decode_ns, p->workers),
                100 * p->minimum_load_share, p->workers);
        } else {
            best->stopped = true; /* Preserve old cache; allocation failure is recoverable. */
            egtb_progress_log("shared dependency cache growth skipped: %s\n", egtb_last_error());
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

void dependency_resident_report(DependencyResidentPool *p)
{
    pthread_mutex_lock(&p->mutex);
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
