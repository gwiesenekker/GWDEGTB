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
void dependency_resident_destroy(DependencyResidentPool *pool);
void dependency_resident_report(DependencyResidentPool *pool);
uint64_t dependency_resident_used(DependencyResidentPool *pool);
const char *dependency_resident_error(void);

#endif
