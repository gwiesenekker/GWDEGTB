#ifndef DEPENDENCY_RESIDENT_H
#define DEPENDENCY_RESIDENT_H

#include "egtb.h"

typedef struct DependencyResidentPool DependencyResidentPool;
bool dependency_resident_create(DependencyResidentPool **out,
                                 uint64_t budget, uint64_t maximum_database);
/* Defaults: 2 GiB total, 256 MiB per database. Whole-number environment
 * settings: EGTB_DEPENDENCY_RESIDENT_GIB / EGTB_DEPENDENCY_RESIDENT_MAX_MIB. */
bool dependency_resident_configure(DependencyResidentPool **out);
/* Cold path only. Thread-safe, loads once per shared read-only backing.
 * true + NULL means private-cache fallback. False means a fatal load error.
 * Backings must outlive the pool; destroy only after all probes have joined.
 * A returned array is immutable and may be queried without synchronization. */
bool dependency_resident_acquire(DependencyResidentPool *pool, Egtb *backing,
                                  const EgtbResident **out);
/* After resident admission returns NULL. Experimental, disabled by default;
 * EGTB_DEPENDENCY_SHARED_CACHE_MIB sets payload budget per cached database. */
bool dependency_shared_acquire(DependencyResidentPool *pool, Egtb *backing,
                               EgtbSharedCache **out);
/* Register private fallback before using it. At quiescent checkpoints the pool
 * may set *shared_probe to a new shared probe. Caller must prefer that probe,
 * retain the private view for cumulative statistics, and unregister before
 * destroying either view/probe or the output pointer. No hit-path hook needed. */
bool dependency_private_register(DependencyResidentPool *pool, Egtb *backing,
                                  EgtbView *view, EgtbSharedProbe **shared_probe);
void dependency_private_unregister(DependencyResidentPool *pool,
                                    EgtbSharedProbe **shared_probe);
/* A nonzero SHARED_CACHE_GIB enables fractional growth, bounds combined allocations
 * including migration/slot metadata; MIB remains the initial per-DB size.
 * Maintain must run on the coordinator with ALL probes quiescent. */
bool dependency_shared_adaptive(DependencyResidentPool *pool);
/* Configure before any acquisitions. Useful for embedding/tests; zero total
 * keeps fixed-size mode. Total includes slot metadata and migration storage. */
bool dependency_shared_configure(DependencyResidentPool *pool,
                                  size_t initial_bytes, uint64_t total_bytes);
/* Before acquisitions. Default 0.5%; zero disables the cost threshold but
 * still requires timing confidence and the existing hysteresis safeguards. */
bool dependency_shared_minimum_load(DependencyResidentPool *pool, double percent);
/* Select before acquisitions; every other registered policy is shadow-logged. */
bool dependency_shared_policy(DependencyResidentPool *pool, const char *name);
void dependency_shared_maintain(void *pool, unsigned workers);
/* Same quiescent maintenance with an explicit monotonic time in seconds,
 * for deterministic policy tests. Do not mix clock domains within a pool. */
void dependency_shared_maintain_at(DependencyResidentPool *pool, double now);
/* Called only with all probes quiescent; cancels trials and resets idle grace. */
void dependency_shared_phase(void *pool, const char *phase);
void dependency_shared_phase_at(DependencyResidentPool *pool, double now);
typedef struct {
    uint64_t allocated, recovery_reserve;
    unsigned shrinks, transfers, rollbacks;
    bool assessing;
} DependencyCoordinatorStatistics;
void dependency_shared_coordinator_statistics(DependencyResidentPool *pool,
                                              DependencyCoordinatorStatistics *out);
void dependency_resident_destroy(DependencyResidentPool *pool);
void dependency_resident_report(DependencyResidentPool *pool);
uint64_t dependency_resident_used(DependencyResidentPool *pool);
const char *dependency_resident_error(void);

#endif
