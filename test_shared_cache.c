#define _POSIX_C_SOURCE 200809L
#include "dependency_resident.h"
#include "shared_cache_policy.h"
#include "cache_modulo.h"
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s: %s\n", \
    __LINE__, #x, egtb_last_error()); exit(1); } } while (0)
enum { N = 4099, THREADS = 16, READS = 100000 };
static void cost_policy(void)
{
    uint64_t rng = 12345;
    const uint64_t divisors[] = {1, 2, 3, 5, 7, 31, 65537, UINT32_MAX,
                                 UINT64_C(4294967297), UINT64_MAX};
    for (unsigned d = 0; d < sizeof(divisors)/sizeof(divisors[0]); ++d) {
        CacheModulo m = cache_modulo_init(divisors[d]);
        CHECK(cache_modulo(UINT32_MAX, m) == UINT32_MAX % divisors[d]);
        CHECK(cache_modulo(UINT64_MAX, m) == UINT64_MAX % divisors[d]);
        for (unsigned j = 0; j < 100000; ++j) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            uint64_t page = j & 1 ? (uint32_t)rng : rng;
            CHECK(cache_modulo(page, m) == page % divisors[d]);
        }
    }
    /* 2000 loads/s at 5 us: 1% of one worker, 0.5% of two, 0.0625% of 16. */
    CHECK(shared_cache_cost_eligible(2000, 5000, 16, 1, .005));
    CHECK(shared_cache_cost_eligible(2000, 5000, 16, 2, .005));
    CHECK(!shared_cache_cost_eligible(2000, 5000, 16, 16, .005));
    CHECK(!shared_cache_cost_eligible(2000, 5000, 15, 2, .005));
    CHECK(!shared_cache_cost_eligible(2000, 0, 16, 2, 0));
    CHECK(!shared_cache_cost_eligible(2000, NAN, 16, 2, 0));
    CHECK(!shared_cache_cost_eligible(2000, INFINITY, 16, 2, 0));
    CHECK(!shared_cache_cost_eligible(0, 5000, 16, 2, 0));
    CHECK(!shared_cache_cost_eligible(2000, 5000, 16, 0, 0));
    CHECK(shared_cache_cost_eligible(1, 5000, 16, 16, 0));
    CHECK(shared_cache_cost_score(1000, 20000, 2097152) >
          shared_cache_cost_score(2000, 1000, 1048576));
    CHECK(shared_cache_cost_score(1000, 20000, 1048576) ==
          2 * shared_cache_cost_score(1000, 20000, 2097152));
    CHECK(shared_cache_cost_score(1000, 20000, 0) == 0);
    DependencyResidentPool *pool;
    CHECK(dependency_resident_create(&pool, 0, 0));
    CHECK(dependency_shared_minimum_load(pool, 0));
    CHECK(dependency_shared_minimum_load(pool, 100));
    CHECK(!dependency_shared_minimum_load(pool, -1));
    CHECK(!dependency_shared_minimum_load(pool, 101));
    CHECK(!dependency_shared_minimum_load(pool, NAN));
    dependency_resident_destroy(pool);
    const char *key = "EGTB_DEPENDENCY_SHARED_CACHE_MIN_LOAD_PERCENT";
    const char *old = getenv(key);
    char *saved = old ? strdup(old) : NULL;
    CHECK(!old || saved);
    const char *invalid[] = {"nan", "inf", "-1", "101", "0.5oops", "1e999", " 1"};
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        CHECK(setenv(key, invalid[i], 1) == 0);
        CHECK(!dependency_resident_configure(&pool));
    }
    CHECK(setenv(key, "0.5", 1) == 0);
    CHECK(dependency_resident_configure(&pool));
    dependency_resident_destroy(pool);
    CHECK(saved ? setenv(key, saved, 1) == 0 : unsetenv(key) == 0);
    free(saved);
    key = "EGTB_DEPENDENCY_SHARED_CACHE_GROWTH";
    old = getenv(key); saved = old ? strdup(old) : NULL;
    CHECK(!old || saved);
    const char *bad_growth[] = {"0", "1", "1.09", "4.01", "nan", "inf", "1.5x"};
    for (unsigned i=0; i<sizeof(bad_growth)/sizeof(bad_growth[0]); ++i) {
        CHECK(setenv(key, bad_growth[i], 1) == 0);
        CHECK(!dependency_resident_configure(&pool));
    }
    CHECK(setenv(key, "1.5", 1) == 0);
    CHECK(dependency_resident_configure(&pool));
    dependency_resident_destroy(pool);
    CHECK(saved ? setenv(key, saved, 1) == 0 : unsetenv(key) == 0);
    free(saved);
    puts("measured load admission, worker normalization, confidence and score tests passed");
}
static int16_t expected(uint64_t i, unsigned side)
{
    if (i >= 4032) return -1; /* An implicit all-draw page, including tail. */
    unsigned n = (unsigned)(i * 13 + side * 71) % 16000;
    return (i + side) & 1 ? (int16_t)(2 * n + 1) : (int16_t)(-2 * (int)n);
}
typedef struct {
    EgtbSharedCache *cache;
    DependencyResidentPool *pool;
    Egtb *db;
    pthread_barrier_t *barrier;
    unsigned id;
    EgtbSharedStatistics stats;
} Worker;
static void *run(void *arg)
{
    Worker *w = arg;
    pthread_barrier_wait(w->barrier);
    if (w->pool) {
        const EgtbResident *r;
        CHECK(dependency_resident_acquire(w->pool, w->db, &r) && !r);
        CHECK(dependency_shared_acquire(w->pool, w->db, &w->cache) && w->cache);
    }
    EgtbSharedProbe *p;
    CHECK(egtb_shared_probe_create(&p, w->cache));
    uint64_t state = 123 + w->id;
    for (unsigned j = 0; j < READS; ++j) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        uint64_t i = state % N;
        unsigned side = (unsigned)(state >> 32) & 1;
        int16_t value;
        CHECK(egtb_shared_probe_get(p, i, (EgtbSide)side, &value));
        CHECK(value == expected(i, side));
    }
    egtb_shared_probe_statistics(p, &w->stats);
    CHECK(w->stats.cache.lookups == READS);
    CHECK(w->stats.cache.hits + w->stats.cache.misses == READS);
    egtb_shared_probe_destroy(p);
    return NULL;
}
static void stress(Egtb *db, EgtbSharedCache *cache, DependencyResidentPool *pool)
{
    pthread_t threads[THREADS]; Worker w[THREADS] = {0};
    pthread_barrier_t barrier;
    CHECK(!pthread_barrier_init(&barrier, NULL, THREADS));
    for (unsigned t = 0; t < THREADS; ++t) {
        w[t] = (Worker){.cache=cache, .pool=pool, .db=db, .barrier=&barrier, .id=t};
        CHECK(!pthread_create(&threads[t], NULL, run, &w[t]));
    }
    uint64_t hits=0, conflicts=0, invalid=0;
    for (unsigned t = 0; t < THREADS; ++t) {
        CHECK(!pthread_join(threads[t], NULL));
        CHECK(w[t].cache == w[0].cache);
        hits += w[t].stats.cache.hits;
        conflicts += w[t].stats.publication_conflicts;
        invalid += w[t].stats.invalidated_reads;
    }
    CHECK(!pthread_barrier_destroy(&barrier));
    printf("shared stress: hits=%" PRIu64 " conflicts=%" PRIu64
           " invalidated=%" PRIu64 "\n", hits, conflicts, invalid);
}
static void adaptive(Egtb *db)
{
    double now = 0;
    EgtbSharedCache *c;
    EgtbSharedProbe *p;
    int16_t v;
    CHECK(egtb_shared_cache_create(&c, db, 256));
    CHECK(egtb_shared_probe_create(&p, c));
    CHECK(egtb_shared_probe_get(p, 0, EGTB_WHITE_TO_MOVE, &v));
    CHECK(v == expected(0, 0));
    CHECK(egtb_shared_probe_get(p, 64, EGTB_BLACK_TO_MOVE, &v));
    EgtbSharedStatistics before, after;
    egtb_shared_probe_statistics(p, &before);
    CHECK(egtb_shared_cache_grow(c, 512));
    CHECK(egtb_shared_cache_bytes(c) == 512 && !egtb_shared_cache_dense(c));
    CHECK(egtb_shared_probe_get(p, 0, EGTB_WHITE_TO_MOVE, &v));
    CHECK(egtb_shared_probe_get(p, 64, EGTB_BLACK_TO_MOVE, &v));
    CHECK(v == expected(64, 1));
    CHECK(egtb_shared_cache_grow(c, 100000));
    CHECK(egtb_shared_cache_dense(c));
    CHECK(egtb_shared_probe_get(p, 0, EGTB_WHITE_TO_MOVE, &v));
    egtb_shared_probe_statistics(p, &after);
    CHECK(after.cache.misses == before.cache.misses); /* Migrated, not discarded. */
    CHECK(after.cache.decompressions == before.cache.decompressions);
    CHECK(egtb_shared_probe_get(p, N-1, EGTB_BLACK_TO_MOVE, &v) && v == -1);
    egtb_shared_probe_destroy(p); egtb_shared_cache_destroy(c);

    /* A doubled allocation would fit alone, but old+new exceeds this budget. */
    DependencyResidentPool *pool; const EgtbResident *r;
    CHECK(dependency_resident_create(&pool, 0, 0));
    CHECK(dependency_shared_configure(pool, 256, 1100));
    CHECK(dependency_resident_acquire(pool, db, &r) && !r);
    CHECK(dependency_shared_acquire(pool, db, &c) && c);
    CHECK(egtb_shared_probe_create(&p, c));
    for (unsigned pass=0; pass<3; ++pass) {
        for (unsigned j=0; j<100001; ++j)
            CHECK(egtb_shared_probe_get(p, (j&1) * 64, EGTB_WHITE_TO_MOVE, &v));
        dependency_shared_maintain_at(pool, ++now);
    }
    CHECK(egtb_shared_cache_bytes(c) == 256);
    /* Pool destruction before inactive probe destruction is supported. */
    dependency_resident_destroy(pool); egtb_shared_probe_destroy(p);

    CHECK(dependency_resident_create(&pool, 0, 0));
    CHECK(dependency_shared_configure(pool, 256, 100));
    CHECK(dependency_resident_acquire(pool, db, &r) && !r);
    CHECK(dependency_shared_acquire(pool, db, &c) && !c);
    dependency_resident_destroy(pool);

    CHECK(dependency_resident_create(&pool, 0, 0));
    CHECK(dependency_shared_configure(pool, 256, 50000));
    CHECK(dependency_resident_acquire(pool, db, &r) && !r);
    CHECK(dependency_shared_acquire(pool, db, &c) && c);
    CHECK(egtb_shared_probe_create(&p, c));
    /* A hot workload must not grow the cache after warm-up. */
    for (unsigned pass=0; pass<3; ++pass) {
        for (unsigned j=0; j<100001; ++j)
            CHECK(egtb_shared_probe_get(p, 0, EGTB_WHITE_TO_MOVE, &v));
        dependency_shared_maintain_at(pool, ++now);
        CHECK(egtb_shared_cache_bytes(c) == 256);
    }
    /* Draw-only page misses perform no decompression and must not cause growth. */
    for (unsigned pass=0; pass<3; ++pass) {
        for (unsigned j=0; j<100001; ++j)
            CHECK(egtb_shared_probe_get(p, (j&1) ? 4032 : 4096, EGTB_WHITE_TO_MOVE, &v));
        dependency_shared_maintain_at(pool, ++now);
        CHECK(egtb_shared_cache_bytes(c) == 256);
    }
    /* More than 99% hits must still qualify when decompression pressure is high. */
    for (unsigned sample=0; sample<2; ++sample) {
        egtb_shared_probe_statistics(p, &before);
        for (unsigned j=0; j<2000000; ++j)
            CHECK(egtb_shared_probe_get(p, (j%1000 == 0) ? 64 : 0, EGTB_WHITE_TO_MOVE, &v));
        egtb_shared_probe_statistics(p, &after);
        CHECK(after.cache.misses - before.cache.misses < 20000);
        CHECK(after.cache.decompressions - before.cache.decompressions >= 1000);
        dependency_shared_maintain_at(pool, ++now);
        if (!sample) CHECK(egtb_shared_cache_bytes(c) == 256);
    }
    CHECK(egtb_shared_cache_bytes(c) >= 512);
    unsigned since_growth = 0;
    for (unsigned pass=0; pass<60 && !egtb_shared_cache_dense(c); ++pass) {
        uint64_t old_allocation = egtb_shared_cache_allocation(c);
        uint64_t old_payload = egtb_shared_cache_bytes(c);
        uint64_t per_side = old_payload / (2 * egtb_cache_page_size(db));
        /* Pages 63 and 64 are implicit draws: no decompression pressure. */
        if (per_side >= 63) break;
        for (unsigned j=0; j<100001; ++j) {
            uint64_t index = (j & 2) ? per_side * 64 : 0;
            CHECK(egtb_shared_probe_get(p, index, (EgtbSide)(j&1), &v));
            CHECK(v == expected(index, j&1));
        }
        dependency_shared_maintain_at(pool, ++now);
        ++since_growth;
        /* First post-grow workload is worse; subsequent ones are unchanged.
         * Warm-up + 2 assessment + 2 extra cooldown + decision window. */
        if (since_growth < 6) CHECK(egtb_shared_cache_bytes(c) == old_payload);
        if (egtb_shared_cache_bytes(c) > old_payload) {
            CHECK(since_growth == 6);
            since_growth = 0;
        }
        if (egtb_shared_cache_bytes(c) > old_payload)
            CHECK(old_allocation + egtb_shared_cache_allocation(c) <= 50000);
    }
    CHECK(egtb_shared_cache_bytes(c) > 512);
    /* Explicit dense transition also covers the draw-only tail. */
    CHECK(egtb_shared_cache_allocation(c) +
          egtb_shared_cache_planned_allocation(db, 100000) <= 50000);
    CHECK(egtb_shared_cache_grow(c, 100000));
    CHECK(egtb_shared_cache_dense(c));
    for (unsigned pass=0; pass<2; ++pass) {
        egtb_shared_probe_statistics(p, &before);
        for (unsigned i=0; i<N; ++i)
            for (unsigned side=0; side<2; ++side) {
                CHECK(egtb_shared_probe_get(p, i, (EgtbSide)side, &v));
                CHECK(v == expected(i,side));
            }
        egtb_shared_probe_statistics(p, &after);
        if (pass) CHECK(after.cache.misses == before.cache.misses);
    }
    stress(db, c, NULL);
    egtb_shared_probe_destroy(p);
    dependency_resident_destroy(pool);
    puts("adaptive growth/dense migration/budget tests passed");
}

static void fractional_budget(Egtb *db)
{
    DependencyResidentPool *pool;
    EgtbSharedCache *cache;
    EgtbSharedProbe *probe;
    CHECK(dependency_resident_create(&pool, 0, 0));
    CHECK(dependency_shared_configure(pool, 1024, 3456));
    CHECK(dependency_shared_minimum_load(pool, 0));
    const EgtbResident *resident;
    CHECK(dependency_resident_acquire(pool, db, &resident) && !resident);
    CHECK(dependency_shared_acquire(pool, db, &cache) && cache);
    CHECK(egtb_shared_probe_create(&probe, cache));
    for (unsigned window = 0; window < 3; ++window) {
        for (unsigned j = 0; j < 100001; ++j) {
            uint64_t index = j & 1 ? 256 : 0;
            int16_t value;
            CHECK(egtb_shared_probe_get(probe, index, EGTB_WHITE_TO_MOVE, &value));
            CHECK(value == expected(index, 0));
        }
        dependency_shared_maintain_at(pool, window + 1);
    }
    /* Requested 1536 cannot coexist with 1024 in this budget, but 1280 can. */
    CHECK(egtb_shared_cache_bytes(cache) == 1280);
    CHECK(egtb_shared_cache_planned_allocation(db, 1024) +
          egtb_shared_cache_allocation(cache) == 3456);
    egtb_shared_probe_destroy(probe);
    dependency_resident_destroy(pool);
}

static void fractional_growth(Egtb *db)
{
    EgtbSharedCache *c; EgtbSharedProbe *p; int16_t value;
    CHECK(egtb_shared_cache_create(&c, db, 512));
    CHECK(egtb_shared_probe_create(&p, c));
    CHECK(egtb_shared_probe_get(p, 0, EGTB_WHITE_TO_MOVE, &value));
    CHECK(egtb_shared_probe_get(p, 192, EGTB_WHITE_TO_MOVE, &value));
    /* Pages 0 and 3 are distinct modulo 2, but collide modulo 3. */
    CHECK(egtb_shared_cache_grow(c, 768));
    CHECK(egtb_shared_cache_bytes(c) == 768);
    EgtbSharedStatistics before, after;
    egtb_shared_probe_statistics(p, &before);
    CHECK(egtb_shared_probe_get(p, 0, EGTB_WHITE_TO_MOVE, &value));
    CHECK(value == expected(0, 0));
    egtb_shared_probe_statistics(p, &after);
    CHECK(after.cache.misses == before.cache.misses);
    CHECK(egtb_shared_probe_get(p, 192, EGTB_WHITE_TO_MOVE, &value));
    CHECK(value == expected(192, 0));
    egtb_shared_probe_statistics(p, &after);
    CHECK(after.cache.misses == before.cache.misses + 1);
    const size_t sizes[] = {1152, 1536, 2304, 100000};
    for (unsigned n=0; n<sizeof(sizes)/sizeof(sizes[0]); ++n) {
        CHECK(egtb_shared_cache_grow(c, sizes[n]));
        CHECK(egtb_shared_cache_allocation(c) == egtb_shared_cache_planned_allocation(db, sizes[n]));
        for (unsigned i=0; i<N; ++i) for (unsigned side=0; side<2; ++side) {
            CHECK(egtb_shared_probe_get(p, i, (EgtbSide)side, &value));
            CHECK(value == expected(i, side));
        }
        if (n == 1) stress(db, c, NULL);
    }
    CHECK(egtb_shared_cache_dense(c));
    CHECK(egtb_shared_cache_resize(c, 768));
    CHECK(!egtb_shared_cache_dense(c) && egtb_shared_cache_bytes(c) == 768);
    CHECK(!egtb_shared_cache_resize(c, 1));
    CHECK(egtb_shared_cache_bytes(c) == 768);
    for (unsigned i=0; i<N; ++i) for (unsigned side=0; side<2; ++side) {
        CHECK(egtb_shared_probe_get(p, i, (EgtbSide)side, &value));
        CHECK(value == expected(i, side));
    }
    egtb_shared_probe_destroy(p); egtb_shared_cache_destroy(c);
    puts("fractional growth: collision, remainder, dense migration and threaded probes passed");
}

static void pressure_priority(Egtb *a, Egtb *b)
{
    DependencyResidentPool *pool;
    const EgtbResident *resident;
    EgtbSharedCache *c[2];
    EgtbSharedProbe *p[2];
    Egtb *backings[2] = {a, b};
    CHECK(a != b);
    CHECK(dependency_resident_create(&pool, 0, 0));
    CHECK(dependency_shared_minimum_load(pool, 0));
    CHECK(dependency_shared_configure(pool, 256, 50000));
    for (unsigned k=0; k<2; ++k) {
        CHECK(dependency_resident_acquire(pool, backings[k], &resident) && !resident);
        CHECK(dependency_shared_acquire(pool, backings[k], &c[k]) && c[k]);
        CHECK(egtb_shared_probe_create(&p[k], c[k]));
    }
    /* Grow A first, then compare unequal growth costs at the same checkpoint.
     * A has more decompressions, B has more decompressions per additional MiB.
     * Actual sampled costs can differ: cost_policy() tests deterministic ranking;
     * here either winner is legal, but only one may grow per checkpoint. */
    for (unsigned pass=0; pass<6; ++pass) {
        uint64_t decoded[2];
        for (unsigned k=0; k<2; ++k) {
            EgtbSharedStatistics before, after;
            egtb_shared_probe_statistics(p[k], &before);
            unsigned churn = (pass == 1 || pass == 2) && k == 0 ? 20000 :
                             pass >= 4 ? (k == 0 ? 12000 : 8000) : 0;
            for (unsigned j=0; j<100001; ++j) {
                uint64_t index = j < churn && (j&1) ? N-1 : 0;
                int16_t value;
                CHECK(egtb_shared_probe_get(p[k], index, EGTB_WHITE_TO_MOVE, &value));
                CHECK(value == expected(index, 0));
            }
            egtb_shared_probe_statistics(p[k], &after);
            decoded[k] = after.cache.decompressions - before.cache.decompressions;
        }
        if (pass == 5) {
            CHECK(decoded[0] > decoded[1]);
            CHECK(decoded[0] < 2 * decoded[1]);
        }
        dependency_shared_maintain_at(pool, pass + 1);
        if (pass < 5) {
            CHECK(egtb_shared_cache_bytes(c[0]) == (pass >= 2 ? 512 : 256));
            CHECK(egtb_shared_cache_bytes(c[1]) == 256);
        } else {
            CHECK((egtb_shared_cache_bytes(c[0]) == 768 && egtb_shared_cache_bytes(c[1]) == 256) ||
                  (egtb_shared_cache_bytes(c[0]) == 512 && egtb_shared_cache_bytes(c[1]) == 512));
        }
    }
    for (unsigned k=0; k<2; ++k) egtb_shared_probe_destroy(p[k]);
    dependency_resident_destroy(pool);
    puts("decompression pressure per additional MiB priority test passed");
}

static void coordinator(Egtb *a, Egtb *b)
{
    for (unsigned outcome = 0; outcome < 7; ++outcome) {
        DependencyResidentPool *pool;
        EgtbSharedCache *c[2]; EgtbSharedProbe *p[2];
        Egtb *backing[2] = {a,b}; const EgtbResident *r;
        CHECK(dependency_resident_create(&pool, 0, 0));
        CHECK(dependency_shared_configure(pool, 256, 1800));
        CHECK(dependency_shared_minimum_load(pool, 0));
        for (unsigned k=0; k<2; ++k) {
            CHECK(dependency_resident_acquire(pool, backing[k], &r) && !r);
            CHECK(dependency_shared_acquire(pool, backing[k], &c[k]) && c[k]);
            CHECK(egtb_shared_probe_create(&p[k], c[k]));
        }
        int16_t v;
        for (unsigned pass=0; pass<6; ++pass) {
            for (unsigned j=0; j<100001; ++j)
                CHECK(egtb_shared_probe_get(p[0], pass < 3 && (j&1) ? 64 : 0,
                                           EGTB_WHITE_TO_MOVE, &v));
            dependency_shared_maintain_at(pool, pass+1);
        }
        CHECK(egtb_shared_cache_bytes(c[0]) == 512);
        if (outcome == 6) dependency_shared_phase_at(pool, 67);
        for (unsigned pass=0; pass<3; ++pass) {
            for (unsigned j=0; j<100001; ++j)
                CHECK(egtb_shared_probe_get(p[1], j&1 ? (outcome == 2 ? 128 : 64) : 0,
                                           EGTB_WHITE_TO_MOVE, &v));
            dependency_shared_maintain_at(pool, 67+pass);
        }
        DependencyCoordinatorStatistics s;
        dependency_shared_coordinator_statistics(pool, &s);
        if (outcome == 6) {
            CHECK(!s.assessing && s.shrinks == 0);
            CHECK(egtb_shared_cache_bytes(c[0]) == 512);
            for (unsigned k=0; k<2; ++k) egtb_shared_probe_destroy(p[k]);
            dependency_resident_destroy(pool);
            continue;
        }
        CHECK(s.assessing && s.transfers == 1);
        CHECK(s.allocated + s.recovery_reserve <= 1800);
        CHECK(egtb_shared_cache_bytes(c[0]) == 256);
        CHECK(egtb_shared_cache_bytes(c[1]) == 512);
        if (outcome == 3) dependency_shared_maintain_at(pool, 190);
        if (outcome == 5) dependency_shared_phase_at(pool, 70);
        for (unsigned pass=0; outcome != 3 && outcome != 5 && pass<3; ++pass) {
            if (!outcome) {
                for (unsigned j=0; j<100001; ++j)
                    CHECK(egtb_shared_probe_get(p[0], j&1 ? 64 : 0, EGTB_WHITE_TO_MOVE, &v));
            }
            if (outcome == 4) CHECK(egtb_shared_probe_get(p[0], 0, EGTB_WHITE_TO_MOVE, &v));
            for (unsigned j=0; j<100001; ++j)
                CHECK(egtb_shared_probe_get(p[1], j&1 ? (outcome == 2 ? 128 : 64) : 0,
                                           EGTB_WHITE_TO_MOVE, &v));
            dependency_shared_maintain_at(pool, 70+pass);
        }
        dependency_shared_coordinator_statistics(pool, &s);
        CHECK(!s.assessing && !s.recovery_reserve);
        CHECK(s.rollbacks == (outcome == 1 || outcome == 4 ? 0u : 1u));
        CHECK(egtb_shared_cache_bytes(c[0]) == (outcome == 1 || outcome == 4 ? 256 : 512));
        if (outcome == 1) dependency_resident_report(pool);
        for (unsigned k=0; k<2; ++k) {
            for (unsigned i=0; i<N; ++i) {
                CHECK(egtb_shared_probe_get(p[k], i, EGTB_WHITE_TO_MOVE, &v));
                CHECK(v == expected(i,0));
            }
            egtb_shared_probe_destroy(p[k]);
        }
        dependency_resident_destroy(pool);
    }
    puts("coordinator transfer, acceptance, returning donor and weak-benefit rollback passed");
}

static void burst_pressure(Egtb *db)
{
    for (unsigned idle=0; idle<2; ++idle) {
        DependencyResidentPool *pool; const EgtbResident *r;
        EgtbSharedCache *c; EgtbSharedProbe *p;
        CHECK(dependency_resident_create(&pool, 0, 0));
        CHECK(dependency_shared_configure(pool, 256, 50000));
        CHECK(dependency_resident_acquire(pool, db, &r) && !r);
        CHECK(dependency_shared_acquire(pool, db, &c) && c);
        CHECK(egtb_shared_probe_create(&p, c));
        double now=0;
        for (unsigned pass=0; pass<4; ++pass) {
            unsigned churn = pass == 1 ? 100000 : pass == 3 ? (idle ? 2000 : 100000) : 0;
            for (unsigned j=0; j<100001; ++j) {
                int16_t value;
                CHECK(egtb_shared_probe_get(p, j < churn && (j&1) ? N-1 : 0,
                                           EGTB_WHITE_TO_MOVE, &value));
            }
            dependency_shared_maintain_at(pool, ++now);
            if (pass < 3 || idle) CHECK(egtb_shared_cache_bytes(c) == 256);
            if (pass == 1 && idle) {
                now += 1000;
                dependency_shared_maintain_at(pool, now);
            }
        }
        CHECK(idle ? egtb_shared_cache_bytes(c) == 256 : egtb_shared_cache_bytes(c) >= 512);
        uint64_t samples, ns;
        egtb_shared_cache_timing(c, &samples, &ns);
        CHECK(samples > 0 && ns > 0);
        egtb_shared_probe_destroy(p);
        dependency_resident_destroy(pool);
    }
    puts("bursty pressure, idle decay and sampled load timing tests passed");
}

int main(void)
{
    CHECK(shared_cache_growth_multiplier(1.5, .006, .005) == 1.5);
    CHECK(shared_cache_growth_multiplier(1.5, .01, .005) == 2.25);
    CHECK(shared_cache_growth_multiplier(1.5, .10, .005) == 3.375);
    CHECK(shared_cache_growth_multiplier(1.5, .10, 0) == 1.5);
    CHECK(shared_cache_growth_multiplier(4, 1, .005) == 4);
    cost_policy();
    char dir[] = "/tmp/gwdegtb-shared-XXXXXX", path[256];
    CHECK(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/test.dtm", dir);
    EgtbCreateOptions options = {4, 20, 1};
    Egtb *db; EgtbSharedCache *cache;
    CHECK(egtb_create(&db, path, N - 1, 128, &options));
    CHECK(strcmp(egtb_path(db), path) == 0);
    CHECK(!egtb_shared_cache_create(&cache, db, 256));
    for (uint64_t i = 0; i < N; ++i)
        for (unsigned s = 0; s < 2; ++s)
            CHECK(egtb_set(db, i, (EgtbSide)s, expected(i,s)));
    CHECK(egtb_close(db));
    CHECK(egtb_open_readonly(&db, path, 1));
    fractional_growth(db);
    fractional_budget(db);
    adaptive(db);
    burst_pressure(db);
    char alias[256]; Egtb *other;
    snprintf(alias, sizeof(alias), "%s/./test.dtm", dir);
    CHECK(egtb_open_readonly(&other, alias, 1));
    pressure_priority(db, other);
    coordinator(db, other);
    CHECK(egtb_close(other));
    CHECK(!egtb_shared_cache_create(&cache, db, 1));
    CHECK(egtb_shared_cache_create(&cache, db, 256)); /* One slot per side. */
    stress(db, cache, NULL);
    egtb_shared_cache_destroy(cache);
    CHECK(egtb_shared_cache_create(&cache, db, 1024 * 1024));
    EgtbSharedProbe *p;
    CHECK(egtb_shared_probe_create(&p, cache));
    for (unsigned pass = 0; pass < 2; ++pass) {
        EgtbSharedStatistics before, after;
        egtb_shared_probe_statistics(p, &before);
        for (uint64_t i = 0; i < N; ++i)
            for (unsigned s = 0; s < 2; ++s) {
                int16_t value;
                CHECK(egtb_shared_probe_get(p, i, (EgtbSide)s, &value));
                CHECK(value == expected(i,s));
            }
        egtb_shared_probe_statistics(p, &after);
        if (pass) CHECK(after.cache.misses == before.cache.misses);
    }
    stress(db, cache, NULL);
    egtb_shared_probe_destroy(p);
    egtb_shared_cache_destroy(cache);
    CHECK(!setenv("EGTB_DEPENDENCY_RESIDENT_GIB", "0", 1));
    CHECK(!setenv("EGTB_DEPENDENCY_SHARED_CACHE_MIB", "1", 1));
    DependencyResidentPool *pool;
    CHECK(dependency_resident_configure(&pool));
    stress(db, NULL, pool);
    dependency_resident_destroy(pool);
    CHECK(egtb_close(db));
    /* Corrupt first physical page CRC. Failed decodes must never publish. */
    FILE *f = fopen(path, "r+b"); CHECK(f);
    unsigned char offset[8];
    CHECK(!fseek(f, 64, SEEK_SET) && fread(offset, 1, 8, f) == 8);
    uint64_t pos=0;
    for (unsigned i=0; i<8; ++i) pos |= (uint64_t)offset[i] << (8*i);
    CHECK(pos && !fseek(f, (long)pos, SEEK_SET));
    int byte=fgetc(f); CHECK(byte != EOF);
    CHECK(!fseek(f, (long)pos, SEEK_SET) && fputc(byte ^ 1, f) != EOF);
    CHECK(!fclose(f));
    CHECK(egtb_open_readonly(&db, path, 1));
    CHECK(egtb_shared_cache_create(&cache, db, 256));
    CHECK(egtb_shared_probe_create(&p, cache));
    int16_t v;
    for (unsigned i=0; i<3; ++i)
        CHECK(!egtb_shared_probe_get(p, 0, EGTB_WHITE_TO_MOVE, &v));
    EgtbSharedStatistics stats;
    egtb_shared_probe_statistics(p, &stats);
    CHECK(stats.publications == 0 && stats.cache.hits == 0);
    egtb_shared_probe_destroy(p); egtb_shared_cache_destroy(cache);
    CHECK(egtb_close(db));
    CHECK(!unlink(path) && !rmdir(dir));
    puts("optimistic shared cache tests passed");
    return 0;
}
