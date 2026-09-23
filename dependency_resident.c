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

typedef struct PrivateRegistration {
    EgtbView *view;
    EgtbSharedProbe **output, *prepared;
    struct PrivateRegistration *next;
} PrivateRegistration;

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
    uint64_t recovery_payload; /* Capacity discarded by idle reclamation, not a reservation. */
    uint64_t loan_base; /* Non-borrowed payload; zero means no outstanding loan. */
    bool activity_seen;
    uint64_t activity_lookups;
    uint64_t window_lookups;
    double activity_time, activity_elapsed;
    double last_activity, resize_after;
    uint64_t last_window_lookups, last_window_decompressions;
    uint64_t previous_samples, previous_ns;
    uint64_t cost_samples;
    CachePolicyLog policy_log;
    PrivateRegistration *private_views;
    uint64_t private_lookups, private_decodes;
    uint64_t private_samples, private_ns;
    double private_time, admission_retry_after;
    unsigned private_windows;
    bool private_sampled;
    CachePolicyLog private_log;
    CachePolicyLog active_log;
    char error[256];
    struct ResidentEntry *next;
} ResidentEntry;

typedef struct ActiveHistory {
    ResidentEntry *donor, *receiver;
    uint64_t donor_bytes, receiver_bytes;
    double pressure, retry_after;
    bool failed;
    struct ActiveHistory *next;
} ActiveHistory;

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
    bool active_trial;
    ActiveHistory *active_history, *trial_history;
    double resize_seconds_per_byte;
    unsigned trial_windows;
    double trial_time, trial_cost[2], trial_rate[2], trial_resize_seconds;
    uint64_t trial_lookups[2], trial_decodes[2], trial_samples[2], trial_ns[2];
    uint64_t trial_total_lookups[2], trial_total_decodes[2];
    uint64_t trial_total_samples[2], trial_total_ns[2];
    bool rebalance;
    double borrow_after;
    unsigned loan_repayments;
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

/* Keep the tiny discard-resize fallback fundable while a loan exists. */
static uint64_t admission_available(DependencyResidentPool *p)
{
    uint64_t available=p->shared_budget-p->shared_allocated-p->recovery_reserve;
    for (ResidentEntry *e=p->entries; e; e=e->next) {
        if (!e->loan_base || !e->shared) continue;
        uint64_t fallback=egtb_shared_cache_planned_allocation(e->backing,
            2*(size_t)egtb_cache_page_size(e->backing));
        available=available>fallback ? available-fallback : 0;
    }
    return available;
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
    if (p->shared_budget && allocation > admission_available(p)) {
        egtb_progress_log("cache admission: database=%s mode=private reason=shared-budget retry=quiescent-pressure\n", entry_name(e));
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

bool dependency_private_register(DependencyResidentPool *p, Egtb *backing,
                                  EgtbView *view, EgtbSharedProbe **output)
{
    if (!p || !p->shared_bytes || !p->shared_budget) return true;
    PrivateRegistration *r = calloc(1,sizeof(*r));
    if (!r) { snprintf(error_text,sizeof(error_text),"cannot register private dependency"); return false; }
    pthread_mutex_lock(&p->mutex);
    ResidentEntry *e;
    for (e=p->entries;e && e->backing!=backing;e=e->next) {}
    if (!e || !view || !output) {
        pthread_mutex_unlock(&p->mutex); free(r);
        snprintf(error_text,sizeof(error_text),"invalid private dependency registration"); return false;
    }
    r->view=view; r->output=output; r->next=e->private_views; e->private_views=r;
    e->private_sampled=false; /* registry totals changed */
    egtb_view_enable_timing(view);
    pthread_mutex_unlock(&p->mutex);
    return true;
}

void dependency_private_unregister(DependencyResidentPool *p, EgtbSharedProbe **output)
{
    if (!p) return;
    pthread_mutex_lock(&p->mutex);
    for (ResidentEntry *e=p->entries;e;e=e->next) {
        PrivateRegistration **link=&e->private_views;
        while (*link) {
            PrivateRegistration *r=*link;
            if (r->output==output) {
                *link=r->next; free(r); e->private_sampled=false;
                pthread_mutex_unlock(&p->mutex); return;
            }
            link=&r->next;
        }
    }
    pthread_mutex_unlock(&p->mutex);
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
    bool discard = p->active_trial && next > p->shared_budget - p->shared_allocated;
    uint64_t fallback = egtb_shared_cache_planned_allocation(e->backing,
        2 * (size_t)egtb_cache_page_size(e->backing));
    if (discard) {
        uint64_t free = p->shared_budget - p->shared_allocated;
        if (fallback > free || next > free + allocation - fallback) return false;
    } else if (next > p->shared_budget - p->shared_allocated) return false;
    double started = monotonic_seconds();
    bool ok = discard ? egtb_shared_cache_discard_resize(e->shared, (size_t)bytes) :
                        egtb_shared_cache_resize(e->shared, (size_t)bytes);
    double seconds = monotonic_seconds() - started;
    p->growth_seconds += seconds;
    /* Ignore tiny allocations dominated by timer/allocator startup noise. */
    if (allocation + next >= 1048576 && seconds > 0) {
        double cost = seconds / ((double)allocation + next);
        if (cost > p->resize_seconds_per_byte) p->resize_seconds_per_byte = cost;
    }
    p->shared_used = p->shared_used - old + egtb_shared_cache_bytes(e->shared);
    p->shared_allocated = p->shared_allocated - allocation +
                          egtb_shared_cache_allocation(e->shared);
    e->sampled = false;
    e->pending_measurement = false;
    e->pressure_windows = e->cooldown_windows = 0;
    e->last_window_seconds = 0;
    e->last_window_lookups = e->last_window_decompressions = 0;
    return ok;
}

static void finish_transfer(DependencyResidentPool *p, double now, bool rollback,
                            const char *reason)
{
    if (rollback) {
        p->rollback_requested = true;
        uint64_t peak = p->shared_allocated + p->recovery_reserve;
        /* Undo receiver first: the preflight reserved this exact recovery path. */
        if (p->receiver && !coordinated_resize(p, p->receiver, p->receiver_bytes)) {
            p->recovery_reserve = peak - p->shared_allocated;
            egtb_progress_log("cache rebalance: recovery deferred (receiver=%s): %s\n", entry_name(p->receiver), egtb_last_error());
            return;
        }
        p->recovery_reserve = peak - p->shared_allocated;
        if (!coordinated_resize(p, p->donor, p->donor_bytes)) {
            p->recovery_reserve = peak - p->shared_allocated;
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
    if (p->active_trial) {
        if (p->trial_history) {
            p->trial_history->failed = rollback;
            p->trial_history->retry_after = now + 1800;
        }
        p->donor->sampled = false;
        p->donor->last_window_seconds = p->donor->pressure_rate = 0;
        p->donor->pressure_windows = 0;
        if (p->receiver) {
            p->receiver->sampled = false;
            p->receiver->last_window_seconds = p->receiver->pressure_rate = 0;
            p->receiver->pressure_windows = 0;
        }
    }
    p->donor = p->receiver = NULL;
    p->recovery_reserve = 0;
    p->rollback_requested = false;
    p->active_trial = false;
    p->trial_history = NULL;
}

static bool capacity_changed(uint64_t now, uint64_t before)
{
    uint64_t delta = now > before ? now-before : before-now;
    return before && (long double)delta >= (long double)before / 4;
}

static ActiveHistory *active_history(DependencyResidentPool *p, ResidentEntry *d,
                                     ResidentEntry *r)
{
    for (ActiveHistory *h=p->active_history;h;h=h->next)
        if (h->donor==d && h->receiver==r) return h;
    return NULL;
}

static bool active_retry_allowed(const ActiveHistory *h, uint64_t donor,
                                  uint64_t receiver, double pressure, double now)
{
    return !h || !h->failed || (now>=h->retry_after &&
        (capacity_changed(donor,h->donor_bytes) ||
         capacity_changed(receiver,h->receiver_bytes) ||
         (h->pressure>0 && pressure>=2*h->pressure)));
}

static void active_rejection(DependencyResidentPool *p, ResidentEntry *d,
                              ResidentEntry *r, const char *reason, double now,
                              double seconds, double savings)
{
    uint64_t signature = !strcmp(reason,"failed-pair-unchanged") ? 1 : 2;
    if (!cache_policy_log_due(&r->active_log,now,signature,0)) return;
    egtb_progress_log("cache active admission: phase=%s donor=%s receiver=%s decision=keep "
        "reason=%s estimated-roundtrip-seconds=%.3f projected-saving-worker-s/s=%.6f "
        "payback-window=300s (estimate, not measured benefit)\n",
        p->phase,entry_name(d),entry_name(r),reason,seconds,savings);
}

static bool active_resize_affordable(DependencyResidentPool *p, uint64_t da,
    uint64_t dn, uint64_t ra, uint64_t rn, uint64_t receiver_bytes,
    uint64_t amount, double pressure, double *seconds, double *savings)
{
    /* Cold-start estimate: 4 GiB/s of old+new allocation footprint. Keep the
     * larger measured cost once available, and budget a complete rollback too.
     * This is deliberately a heuristic, not a miss-curve prediction. */
    double cost=1.0/(4.0*1024*1024*1024);
    if (p->resize_seconds_per_byte>cost) cost=p->resize_seconds_per_byte;
    *seconds=2*((double)da+dn+ra+rn)*cost;
    double fraction=(double)amount/receiver_bytes;
    if (fraction>0.25) fraction=0.25;
    *savings=pressure*fraction;
    return *savings>0 && *savings*300 > *seconds*p->workers;
}

/* Active transfers use discard-resize when overlap cannot fit. Reserve the
 * larger endpoint plus a minimal fallback for either cache, including metadata.
 * No admissions or other resizing run while this experiment is outstanding. */
static bool start_active_transfer(DependencyResidentPool *p, ResidentEntry *r, double now)
{
    if (!p->rebalance || p->donor || !r || now < r->resize_after ||
        r->pending_measurement || r->last_window_seconds <= 0) return false;
    ResidentEntry *best = NULL;
    double lowest = 0;
    for (ResidentEntry *d=p->entries; d; d=d->next) {
        if (!d->shared || d==r || !d->sampled || d->pending_measurement ||
            now<d->resize_after || !d->window_lookups || d->last_window_seconds<=0 ||
            d->cost_samples<16 || d->last_window_lookups<100000) continue;
        uint64_t bytes=egtb_shared_cache_bytes(d->shared);
        uint64_t step=bytes/20;
        uint64_t q=2*(uint64_t)egtb_cache_page_size(d->backing);
        if (step>p->shared_budget/100) step=p->shared_budget/100;
        if (step>egtb_shared_cache_bytes(r->shared)/2)
            step=egtb_shared_cache_bytes(r->shared)/2;
        step=step/q*q;
        /* A tiny reclaimed cache must not hide a usable large donor. */
        if (!step || bytes-step<p->shared_bytes) continue;
        double load=d->baseline_rate*d->decode_ns;
        if (load*4 >= r->baseline_rate*r->decode_ns) continue;
        double pressure=r->baseline_rate*r->decode_ns/1e9;
        uint64_t rb=egtb_shared_cache_bytes(r->shared);
        if (!active_retry_allowed(active_history(p,d,r),bytes,rb,pressure,now)) {
            active_rejection(p,d,r,"failed-pair-unchanged",now,0,0);
            continue;
        }
        double seconds,savings;
        if (!active_resize_affordable(p,egtb_shared_cache_allocation(d->shared),
                egtb_shared_cache_planned_allocation(d->backing,(size_t)(bytes-step)),
                egtb_shared_cache_allocation(r->shared),
                egtb_shared_cache_planned_allocation(r->backing,(size_t)(rb+step)),
                rb,step,pressure,&seconds,&savings)) {
            active_rejection(p,d,r,"resize-cost-payback",now,seconds,savings);
            continue;
        }
        if (!best || load<lowest) { best=d; lowest=load; }
    }
    if (!best) return false;
    uint64_t old=egtb_shared_cache_bytes(best->shared);
    uint64_t rold=egtb_shared_cache_bytes(r->shared);
    uint64_t amount=old/20;
    if (amount>p->shared_budget/100) amount=p->shared_budget/100;
    if (amount>rold/2) amount=rold/2;
    uint64_t quantum=2*(uint64_t)egtb_cache_page_size(best->backing);
    amount=amount/quantum*quantum;
    if (!amount || old-amount<p->shared_bytes || rold>SIZE_MAX-amount) return false;
    uint64_t da=egtb_shared_cache_allocation(best->shared);
    uint64_t ra=egtb_shared_cache_allocation(r->shared);
    uint64_t dn=egtb_shared_cache_planned_allocation(best->backing,(size_t)(old-amount));
    uint64_t rn=egtb_shared_cache_planned_allocation(r->backing,(size_t)(rold+amount));
    if (dn>=da || rn<=ra) return false;
    uint64_t final=p->shared_allocated-da+dn-ra+rn;
    uint64_t peak=final>p->shared_allocated ? final : p->shared_allocated;
    uint64_t fallback=egtb_shared_cache_planned_allocation(best->backing,(size_t)quantum);
    uint64_t rf=egtb_shared_cache_planned_allocation(r->backing,
        2*(size_t)egtb_cache_page_size(r->backing));
    if (rf>fallback) fallback=rf;
    if (peak>p->shared_budget || fallback>p->shared_budget-peak) return false;
    peak+=fallback;
    ActiveHistory *history=active_history(p,best,r);
    if (!history) {
        history=calloc(1,sizeof(*history));
        if (!history) return false; /* No mutation unless outcome can be remembered. */
        history->donor=best; history->receiver=r;
        history->next=p->active_history; p->active_history=history;
    }
    history->donor_bytes=old; history->receiver_bytes=rold;
    history->pressure=r->baseline_rate*r->decode_ns/1e9;
    p->trial_history=history;
    p->active_trial=true;
    p->donor=best; p->receiver=r;
    p->donor_bytes=old; p->receiver_bytes=rold;
    p->trial_windows=0; p->trial_time=now; p->trial_deadline=now+180;
    ResidentEntry *entries[2]={best,r};
    for (unsigned k=0;k<2;++k) {
        ResidentEntry *e=entries[k];
        p->trial_cost[k]=e->baseline_density*e->decode_ns/1e6;
        p->trial_rate[k]=e->last_window_lookups/e->last_window_seconds;
        p->trial_total_lookups[k]=p->trial_total_decodes[k]=0;
        p->trial_total_samples[k]=p->trial_total_ns[k]=0;
        EgtbCacheStatistics s;
        egtb_shared_cache_statistics(e->shared,&s);
        p->trial_lookups[k]=s.lookups; p->trial_decodes[k]=s.decompressions;
        egtb_shared_cache_timing(e->shared,&p->trial_samples[k],&p->trial_ns[k]);
    }
    double started=p->growth_seconds;
    bool ok=coordinated_resize(p,best,old-amount);
    p->recovery_reserve=peak-p->shared_allocated;
    if (ok) ok=coordinated_resize(p,r,rold+amount);
    p->recovery_reserve=peak-p->shared_allocated;
    p->trial_resize_seconds=p->growth_seconds-started;
    if (!ok) { finish_transfer(p,now,true,"active allocation failed"); return true; }
    ++p->shrinks; ++p->growths; ++p->transfers;
    egtb_progress_log("cache active transfer: phase=%s donor=%s receiver=%s donor-MiB=%.2f->%.2f "
        "receiver-MiB=%.2f->%.2f reserve-MiB=%.2f deadline=180s\n",p->phase,
        entry_name(best),entry_name(r),(double)old/1048576,
        (double)egtb_shared_cache_bytes(best->shared)/1048576,(double)rold/1048576,
        (double)egtb_shared_cache_bytes(r->shared)/1048576,(double)p->recovery_reserve/1048576);
    return true;
}

static void measure_active_transfer(DependencyResidentPool *p, double now)
{
    if (p->rollback_requested || now>=p->trial_deadline) {
        finish_transfer(p,now,true,p->rollback_requested ? "deferred recovery" : "active trial inconclusive");
        return;
    }
    ResidentEntry *entries[2]={p->donor,p->receiver};
    EgtbCacheStatistics s[2]; uint64_t samples[2],ns[2];
    for (unsigned k=0;k<2;++k) {
        egtb_shared_cache_statistics(entries[k]->shared,&s[k]);
        egtb_shared_cache_timing(entries[k]->shared,&samples[k],&ns[k]);
        if (s[k].lookups<p->trial_lookups[k] || s[k].decompressions<p->trial_decodes[k] ||
            samples[k]<p->trial_samples[k] || ns[k]<p->trial_ns[k] || now<p->trial_time) {
            finish_transfer(p,now,true,"active counters reset"); return;
        }
    }
    if (now-p->trial_time<5 || s[0].lookups-p->trial_lookups[0]<100000 ||
        s[1].lookups-p->trial_lookups[1]<100000) return;
    for (unsigned k=0;k<2;++k) {
        if (p->trial_windows) {
            p->trial_total_lookups[k]+=s[k].lookups-p->trial_lookups[k];
            p->trial_total_decodes[k]+=s[k].decompressions-p->trial_decodes[k];
            p->trial_total_samples[k]+=samples[k]-p->trial_samples[k];
            p->trial_total_ns[k]+=ns[k]-p->trial_ns[k];
        }
        p->trial_lookups[k]=s[k].lookups; p->trial_decodes[k]=s[k].decompressions;
        p->trial_samples[k]=samples[k]; p->trial_ns[k]=ns[k];
    }
    p->trial_time=now;
    if (++p->trial_windows<3) return; /* One refill window, then two measurement windows. */
    double after[2];
    for (unsigned k=0;k<2;++k) {
        if (p->trial_total_decodes[k]>=256 && p->trial_total_samples[k]<16) {
            finish_transfer(p,now,true,"active timing inconclusive"); return;
        }
        double cost=p->trial_total_samples[k] ?
            (double)p->trial_total_ns[k]/p->trial_total_samples[k] : entries[k]->decode_ns;
        after[k]=(double)p->trial_total_decodes[k]/p->trial_total_lookups[k]*cost;
    }
    /* Normalize to pre-trial traffic: faster lookup throughput is not a cost
     * regression. This remains observational evidence, not a controlled replay. */
    double penalty=(after[0]-p->trial_cost[0])*p->trial_rate[0]/1e9;
    double saving=(p->trial_cost[1]-after[1])*p->trial_rate[1]/1e9;
    if (penalty<0) penalty=0; /* Do not credit an unrelated donor improvement. */
    bool accept=saving>penalty*1.1 && saving>0 &&
        (saving-penalty)*300>p->trial_resize_seconds*p->workers;
    egtb_progress_log("cache active measurement: donor=%s receiver=%s added-worker-s/s=%.6f "
        "saved-worker-s/s=%.6f resize-seconds=%.6f decision=%s (observed workload)\n",
        entry_name(p->donor),entry_name(p->receiver),penalty,saving,p->trial_resize_seconds,
        accept ? "accept" : "rollback");
    if (accept) {
        if (p->donor->loan_base>=egtb_shared_cache_bytes(p->donor->shared))
            p->donor->loan_base=0;
        /* A transferred allocation is owned capacity, not a headroom loan. */
        p->receiver->loan_base=0;
    }
    finish_transfer(p,now,!accept,accept ? "active net benefit" : "active net benefit insufficient");
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

static bool reclaim_idle_entry(DependencyResidentPool *p, ResidentEntry *best,
                               ResidentEntry *receiver, double now)
{
    uint64_t largest = egtb_shared_cache_allocation(best->shared);
    uint64_t floor = p->shared_bytes < 1048576 ? p->shared_bytes : 1048576;
    uint64_t old = egtb_shared_cache_bytes(best->shared);
    uint64_t remembered = best->loan_base ? best->loan_base : old;
    double started = monotonic_seconds();
    bool ok = egtb_shared_cache_discard_resize(best->shared, (size_t)floor);
    p->growth_seconds += monotonic_seconds() - started;
    uint64_t actual = egtb_shared_cache_bytes(best->shared);
    if (best->loan_base && actual <= best->loan_base) best->loan_base = 0;
    if (actual < remembered && remembered > best->recovery_payload)
        best->recovery_payload = remembered;
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
    return best ? reclaim_idle_entry(p,best,receiver,now) : false;
}

/* Repay only explicit loans, never an arbitrary active cache. All probes are
 * quiescent. Migration preserves surviving entries when the overlap fits;
 * otherwise discard-resize needs only its minimal failure fallback. */
static bool repay_headroom(DependencyResidentPool *p, ResidentEntry *receiver, double now)
{
    if (p->policy != CACHE_IDLE_RECLAIM || p->donor) return false;
    ResidentEntry *e = NULL;
    for (ResidentEntry *it=p->entries; it; it=it->next)
        if (it != receiver && it->shared && it->loan_base) { e=it; break; }
    if (!e) return false;
    /* Avoid rebuilding the protected base only to reclaim it immediately.
     * The normal idle grace, assessment and resize guards still apply. */
    if (!strcmp(idle_reclaim_reason(p,e,now), "eligible-if-receiver-needs-memory")) {
        uint64_t protected_base=e->loan_base;
        bool acted=reclaim_idle_entry(p,e,receiver,now);
        if (acted && !e->loan_base) {
            ++p->loan_repayments;
            p->borrow_after=now+60;
        }
        egtb_progress_log("cache headroom idle repayment: database=%s receiver=%s "
            "strategy=direct-idle-reclaim protected-MiB=%.2f payload-MiB=%.2f "
            "result=%s\n",entry_name(e),entry_name(receiver),
            (double)protected_base/1048576,(double)egtb_shared_cache_bytes(e->shared)/1048576,
            e->loan_base ? "loan-retained" : "repaid");
        return acted;
    }
    uint64_t old=egtb_shared_cache_bytes(e->shared);
    uint64_t allocation=egtb_shared_cache_allocation(e->shared);
    uint64_t target=e->loan_base;
    uint64_t free_bytes=p->shared_budget-p->shared_allocated;
    uint64_t minimum=egtb_shared_cache_planned_allocation(e->backing,
        2*(size_t)egtb_cache_page_size(e->backing));
    if (free_bytes < minimum) return false;
    bool migrate=egtb_shared_cache_planned_allocation(e->backing,(size_t)target)<=free_bytes;
    double started=monotonic_seconds();
    bool ok=migrate ? egtb_shared_cache_resize(e->shared,(size_t)target) :
                      egtb_shared_cache_discard_resize(e->shared,(size_t)target);
    double seconds=monotonic_seconds()-started;
    uint64_t actual=egtb_shared_cache_bytes(e->shared);
    p->shared_used=p->shared_used-old+actual;
    p->shared_allocated=p->shared_allocated-allocation+egtb_shared_cache_allocation(e->shared);
    p->growth_seconds+=seconds;
    if (actual<=target) { e->loan_base=0; ++p->loan_repayments; }
    /* A failed replacement can leave the minimal fallback; remember only the
     * protected base, not the loan, for subsequent recovery. */
    if (actual<target && e->recovery_payload<target) e->recovery_payload=target;
    e->sampled=e->pending_measurement=false;
    e->pressure_windows=e->cooldown_windows=0;
    p->borrow_after=now+60;
    egtb_progress_log("cache headroom repayment: database=%s receiver=%s payload-MiB=%.2f->%.2f strategy=%s result=%s seconds=%.6f borrow-cooldown=60s\n",
        entry_name(e),entry_name(receiver),(double)old/1048576,(double)actual/1048576,
        migrate ? "migrate" : "discard-refill",ok ? "repaid" : "allocation-failed",seconds);
    return true;
}

/* All registered catalog probes are quiescent here. Prepare every replacement
 * before publishing any pointer, so allocation failure leaves private lookup
 * intact. Private views remain alive for their cumulative statistics. */
static bool admit_private(DependencyResidentPool *p, ResidentEntry *e, double now)
{
    EgtbSharedCache *cache = NULL;
    double started = monotonic_seconds();
    bool ok = egtb_shared_cache_create(&cache, e->backing, (size_t)p->shared_bytes);
    for (PrivateRegistration *r=e->private_views; ok && r; r=r->next)
        ok = egtb_shared_probe_create(&r->prepared, cache);
    if (!ok) {
        for (PrivateRegistration *r=e->private_views; r; r=r->next) {
            egtb_shared_probe_destroy(r->prepared); r->prepared=NULL;
        }
        egtb_shared_cache_destroy(cache);
        e->admission_retry_after=now+60;
        egtb_progress_log("cache admission: database=%s mode=private reason=allocation-failed retry-seconds=60\n",entry_name(e));
    } else {
        e->shared=cache;
        e->growth_factor=p->growth_factor;
        p->shared_allocated+=egtb_shared_cache_allocation(cache);
        p->shared_used+=egtb_shared_cache_bytes(cache);
        ++p->shared_count;
        for (PrivateRegistration *r=e->private_views; r; r=r->next) {
            *r->output=r->prepared; r->prepared=NULL;
        }
        e->last_activity=now; e->activity_seen=true; e->activity_time=now;
        egtb_progress_log("cache admission: phase=%s database=%s mode=private->shared payload-MiB=%.2f allocated-MiB=%.2f\n",
            p->phase ? p->phase : "unknown",entry_name(e),
            (double)egtb_shared_cache_bytes(cache)/1048576,(double)p->shared_allocated/1048576);
    }
    p->growth_seconds+=monotonic_seconds()-started;
    return ok;
}

static bool maintain_private(DependencyResidentPool *p, double now)
{
    ResidentEntry *best=NULL;
    double best_score=-1;
    for (ResidentEntry *e=p->entries; e; e=e->next) {
        if (e->shared || !e->private_views) continue;
        uint64_t lookups=0, decodes=0, samples=0, ns=0;
        for (PrivateRegistration *r=e->private_views; r; r=r->next) {
            EgtbCacheStatistics s; uint64_t n,t;
            egtb_view_cache_statistics(r->view,&s);
            egtb_view_load_timing(r->view,&n,&t);
            lookups+=s.lookups; decodes+=s.decompressions; samples+=n; ns+=t;
        }
        if (!e->private_sampled || now<=e->private_time || lookups<e->private_lookups ||
            decodes<e->private_decodes || samples<e->private_samples || ns<e->private_ns) {
            e->private_sampled=true; e->private_windows=0;
            e->private_time=now; e->private_lookups=lookups; e->private_decodes=decodes;
            e->private_samples=samples; e->private_ns=ns;
            continue;
        }
        uint64_t dl=lookups-e->private_lookups, dd=decodes-e->private_decodes;
        uint64_t ds=samples-e->private_samples;
        double elapsed=now-e->private_time;
        /* Accumulate sparse checkpoint intervals, but expire quiet windows. */
        if (dl<100000 && elapsed<60) continue;
        double cost=ds ? (double)(ns-e->private_ns)/ds : 0;
        double rate=dd/elapsed;
        bool pressure=shared_cache_cost_eligible(rate,cost,ds,p->workers,p->minimum_load_share);
        e->private_windows=pressure ? e->private_windows+1 : 0;
        uint64_t allocation=egtb_shared_cache_planned_allocation(e->backing,(size_t)p->shared_bytes);
        bool qualified=e->private_windows>=2 && now>=e->admission_retry_after;
        const char *reason = ds<16 ? "insufficient-timed-loads" :
            !pressure ? "below-load-floor" : e->private_windows<2 ? "collecting-pressure-windows" :
            now<e->admission_retry_after ? "allocation-cooldown" :
            allocation>p->shared_budget ? "initial-cache-exceeds-budget" :
            allocation>admission_available(p) ? "capacity-blocked" : "funded";
        if (cache_policy_log_due(&e->private_log,now,qualified ? 1 : 0,allocation))
            egtb_progress_log("cache private pressure: phase=%s database=%s lookups=%" PRIu64
                " decompressions=%" PRIu64 " elapsed=%.3f load-share=%.3f%% timed-loads=%" PRIu64
                " admission=%s reason=%s active=%s\n",p->phase ? p->phase : "unknown",entry_name(e),
                dl,dd,elapsed,100*shared_cache_load_share(rate,cost,p->workers),ds,
                qualified ? "candidate" : "measuring-or-below-floor",reason,cache_policy_name(p->policy));
        if (qualified && allocation<=p->shared_budget && p->policy==CACHE_IDLE_RECLAIM) {
            double score=shared_cache_cost_score(rate,cost,allocation);
            if (score>best_score) {best=e; best_score=score;}
        }
        e->private_time=now; e->private_lookups=lookups; e->private_decodes=decodes;
        e->private_samples=samples; e->private_ns=ns;
    }
    if (!best || p->donor) return false;
    uint64_t needed=egtb_shared_cache_planned_allocation(best->backing,(size_t)p->shared_bytes);
    if (needed>admission_available(p)) {
        if (!repay_headroom(p,best,now) && !reclaim_idle(p,best,now)) return false;
        if (needed>admission_available(p)) return true;
    }
    admit_private(p,best,now);
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
        e->private_sampled=false;
        e->private_windows=0;
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
        e->pending_measurement = false;
        /* A reclaimed cache has a known target capacity. Start measuring at
         * the boundary instead of discarding its first interval as warm-up. */
        e->sampled = p->policy == CACHE_IDLE_RECLAIM && e->recovery_payload >
                     egtb_shared_cache_bytes(e->shared);
        e->previous_time = now;
        e->cost_samples = 0; /* Recovery must use fresh timed loads. */
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
    bool borrowed;
    double floor, score, unfunded_score;
    const char *reason;
} CacheDecision;

/* Pure decision function: all alternatives see identical measurements and
 * allocation state. Only the selected policy's result reaches the executor. */
/* Allocation budgets include metadata; round down to a realizable cache size.
 * Comparing an actual allocation against the unrounded budget can otherwise
 * recall a loan forever for a fraction of a page that cannot be allocated. */
static uint64_t returning_bootstrap(DependencyResidentPool *p, ResidentEntry *e)
{
    uint64_t budget=p->shared_budget/80;
    uint64_t initial=egtb_shared_cache_planned_allocation(e->backing,(size_t)p->shared_bytes);
    uint64_t minimum=egtb_shared_cache_planned_allocation(e->backing,
        4*(size_t)egtb_cache_page_size(e->backing));
    if (budget<initial) budget=initial;
    if (budget<minimum) budget=minimum;
    uint64_t lo=0, hi=e->recovery_payload;
    while (lo<hi) {
        uint64_t mid=lo+(hi-lo)/2+(hi-lo)%2;
        if (egtb_shared_cache_planned_allocation(e->backing,(size_t)mid)<=budget) lo=mid;
        else hi=mid-1;
    }
    return egtb_shared_cache_planned_allocation(e->backing,(size_t)lo);
}

static CacheDecision policy_decide(DependencyResidentPool *p, ResidentEntry *e,
                                   CachePolicy policy, double now)
{
    CacheDecision d = {.floor = p->minimum_load_share, .reason = "below-load-threshold"};
    uint64_t current = egtb_shared_cache_allocation(e->shared);
    uint64_t old = egtb_shared_cache_bytes(e->shared);
    uint64_t available = admission_available(p);
    uint64_t physical_available = available;
    /* Admission can use the whole pool; growth cannot consume its last 5%.
     * This is allocation (metadata included), not a second memory allocation. */
    uint64_t admission_headroom = policy == CACHE_IDLE_RECLAIM ? p->shared_budget / 20 : 0;
    available = available > admission_headroom ? available - admission_headroom : 0;
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
    /* A returning idle donor should not repeat its entire cold-start ramp.
     * Historical capacity is only a target hint: fresh measured pressure must
     * clear the normal floor, and the budget clamp below remains authoritative. */
    bool recovery = policy == CACHE_IDLE_RECLAIM && e->recovery_payload > payload &&
        shared_cache_cost_eligible(e->pressure_rate, e->decode_ns,
            e->cost_samples, p->workers, p->minimum_load_share);
    if (recovery) payload = e->recovery_payload;
    bool recovery_headroom = false;
    if (recovery) {
        /* Reclaimed caches are already admitted, but need the same bootstrap
         * opportunity as new admissions. Bound the allocation ceiling so one
         * returning cache cannot take all the reserved headroom. */
        uint64_t bootstrap = returning_bootstrap(p,e);
        uint64_t physical_capacity = physical_available >= fallback ?
            physical_available + current - fallback : physical_available;
        if (bootstrap > physical_capacity) bootstrap = physical_capacity;
        if (bootstrap > capacity_budget) {
            capacity_budget = bootstrap;
            recovery_headroom = true;
        }
    }
    uint64_t protected_capacity = capacity_budget;
    bool loan_available = policy == CACHE_IDLE_RECLAIM && p->rebalance &&
        now >= p->borrow_after && !p->donor &&
        egtb_shared_cache_planned_allocation(e->backing,(size_t)(old+quantum))>protected_capacity;
    for (ResidentEntry *it=p->entries; loan_available && it; it=it->next)
        if ((it != e && it->loan_base) ||
            (!it->shared && it->private_windows>=2 && now>=it->admission_retry_after)) loan_available=false;
    /* Loans require the normal cost floor, not the relaxed spare-budget floor.
     * A single borrower bounds repayment work at a checkpoint. */
    if (loan_available && shared_cache_cost_eligible(e->pressure_rate,e->decode_ns,
            e->cost_samples,p->workers,p->minimum_load_share)) {
        uint64_t physical_capacity=physical_available>=fallback ?
            physical_available+current-fallback : physical_available;
        if (physical_capacity>capacity_budget) capacity_budget=physical_capacity;
    }
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
    d.borrowed = d.allocation > protected_capacity;
    d.reason = d.borrowed ? "borrow-admission-headroom" : recovery_headroom ? "returning-cache-admission-headroom" : recovery ? "returning-idle-cache" :
        policy != CACHE_COST_GATED && funded ? "spare-budget" : "load-cost";
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
    if (p->active_trial) {
        measure_active_transfer(p,now);
        pthread_mutex_unlock(&p->mutex);
        return;
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
    if (maintain_private(p,now)) {
        pthread_mutex_unlock(&p->mutex);
        return;
    }
    ResidentEntry *best = NULL;
    ResidentEntry *unfunded = NULL;
    uint64_t unfunded_payload = 0;
    double unfunded_score = 0;
    uint64_t best_payload = 0, best_allocation = 0;
    bool best_borrowed = false;
    ResidentEntry *returning_waiter = NULL;
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
        bool returning = p->policy == CACHE_IDLE_RECLAIM &&
            e->recovery_payload > egtb_shared_cache_bytes(e->shared);
        const char *guard = e->pressure_windows < (returning ? 1u : 2u) ? "insufficient-pressure-windows" :
            egtb_shared_cache_dense(e->shared) ? "dense" :
            now < e->resize_after && p->policy != CACHE_IDLE_RECLAIM ? "resize-protection" : NULL;
        if (guard) { policy_guard_log(p,e,guard,now); continue; }
        CacheDecision decisions[CACHE_POLICY_COUNT];
        for (unsigned i=0; i<CACHE_POLICY_COUNT; ++i) {
            CacheDecision d = policy_decide(p,e,(CachePolicy)i,now);
            decisions[i] = d;
            if (d.score > policy_choice[i].score) {
                policy_best[i] = e; policy_choice[i] = d;
            }
            if (i != (unsigned)p->policy) continue;
            /* A returning cache below its bootstrap ceiling has priority over
             * a loan, even when some partial growth would already fit. */
            uint64_t bootstrap=returning_bootstrap(p,e);
            uint64_t fallback=egtb_shared_cache_planned_allocation(e->backing,
                2*(size_t)egtb_cache_page_size(e->backing));
            uint64_t free_now=admission_available(p);
            uint64_t capacity=free_now>=fallback ?
                free_now+egtb_shared_cache_allocation(e->shared)-fallback : free_now;
            uint64_t after_repayment=capacity;
            for (ResidentEntry *borrower=p->entries; borrower; borrower=borrower->next) {
                if (borrower==e || !borrower->shared || !borrower->loan_base) continue;
                after_repayment+=egtb_shared_cache_allocation(borrower->shared)-
                    egtb_shared_cache_planned_allocation(borrower->backing,(size_t)borrower->loan_base);
            }
            if (returning && egtb_shared_cache_allocation(e->shared)<bootstrap &&
                capacity<bootstrap && after_repayment>=bootstrap &&
                shared_cache_cost_eligible(e->pressure_rate,e->decode_ns,e->cost_samples,
                    p->workers,p->minimum_load_share)) returning_waiter=e;
            if (d.unfunded_score > unfunded_score) {
                unfunded = e; unfunded_payload = d.unfunded_payload; unfunded_score = d.unfunded_score;
            }
            if (d.score > best_score) {
                best = e; best_score = d.score; best_payload = d.payload;
                best_allocation = d.allocation; best_rate = before_rate; best_density = before_density;
                best_floor = d.floor;
                best_borrowed = d.borrowed;
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
    if (returning_waiter && repay_headroom(p,returning_waiter,now)) {
        pthread_mutex_unlock(&p->mutex);
        return;
    }
    if (!best && p->policy == CACHE_IDLE_RECLAIM) {
        if (unfunded && !reclaim_idle(p, unfunded, now)) start_active_transfer(p,unfunded,now);
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
            if (!best_borrowed && best->loan_base && actual>old)
                best->loan_base+=actual-old; /* Ordinary capacity is not part of the loan. */
            if (best_borrowed && !best->loan_base) best->loan_base=old;
            if (best_borrowed)
                egtb_progress_log("cache headroom loan: database=%s protected-MiB=%.2f payload-MiB=%.2f repay-on=qualified-admission-or-return\n",
                    entry_name(best),(double)best->loan_base/1048576,(double)actual/1048576);
            if (best->recovery_payload > old) {
                egtb_progress_log("cache recovery: database=%s previous-MiB=%.2f current-MiB=%.2f->%.2f result=%s\n",
                    entry_name(best), (double)best->recovery_payload/1048576,
                    (double)old/1048576, (double)actual/1048576,
                    actual >= best->recovery_payload ? "restored" : "partial-budget-or-policy-limited");
                if (actual >= best->recovery_payload) best->recovery_payload = 0;
            }
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
            if (best->loan_base && actual_bytes<=best->loan_base) best->loan_base=0;
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
    *out = (DependencyCoordinatorStatistics){.allocated=p->shared_allocated,
        .recovery_reserve=p->recovery_reserve, .shrinks=p->shrinks,
        .transfers=p->transfers, .rollbacks=p->rollbacks, .assessing=p->donor != NULL,
        .loan_repayments=p->loan_repayments};
    for (ResidentEntry *e=p->entries; e; e=e->next)
        if (e->shared && e->loan_base)
            out->borrowed_payload+=egtb_shared_cache_bytes(e->shared)-e->loan_base;
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
    if (p->shared_budget && p->policy == CACHE_IDLE_RECLAIM)
        printf("shared-cache admission headroom: %.2f MiB (5%%; reclaimable loan after pressure qualification; one borrower; repayment cooldown=60s); returning-cache pressure-windows=1\n",
               (double)(p->shared_budget / 20) / 1048576);
    for (ResidentEntry *e=p->entries; e; e=e->next)
        if (e->shared && e->loan_base)
            printf("shared-cache outstanding loan: database=%s protected-MiB=%.2f borrowed-payload-MiB=%.2f\n",
                entry_name(e),(double)e->loan_base/1048576,
                (double)(egtb_shared_cache_bytes(e->shared)-e->loan_base)/1048576);
    pthread_mutex_unlock(&p->mutex);
}

void dependency_resident_destroy(DependencyResidentPool *p)
{
    if (!p) return;
    while (p->entries) {
        ResidentEntry *e = p->entries; p->entries = e->next;
        while (e->private_views) {
            PrivateRegistration *r=e->private_views;
            e->private_views=r->next; free(r);
        }
        egtb_resident_destroy(e->resident);
        egtb_shared_cache_destroy(e->shared); free(e);
    }
    pthread_cond_destroy(&p->changed);
    pthread_mutex_destroy(&p->mutex);
    while (p->active_history) {
        ActiveHistory *next=p->active_history->next;
        free(p->active_history);
        p->active_history=next;
    }
    free(p);
}
