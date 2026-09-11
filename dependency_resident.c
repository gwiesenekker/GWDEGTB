#define _POSIX_C_SOURCE 200809L
#include "dependency_resident.h"
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ResidentEntry {
    Egtb *backing;
    EgtbResident *resident;
    bool loading, failed;
    char error[256];
    struct ResidentEntry *next;
} ResidentEntry;

struct DependencyResidentPool {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint64_t budget, maximum_database, used;
    unsigned loaded, cached;
    ResidentEntry *entries;
};

static _Thread_local char error_text[256];
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
    uint64_t budget, maximum;
    if (!setting("EGTB_DEPENDENCY_RESIDENT_GIB", 1024 * mib, 2048 * mib, &budget) ||
        !setting("EGTB_DEPENDENCY_RESIDENT_MAX_MIB", mib, 256 * mib, &maximum))
        return false;
    return dependency_resident_create(out, budget, maximum);
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
    pthread_mutex_unlock(&p->mutex);
}

void dependency_resident_destroy(DependencyResidentPool *p)
{
    if (!p) return;
    while (p->entries) {
        ResidentEntry *e = p->entries; p->entries = e->next;
        egtb_resident_destroy(e->resident); free(e);
    }
    pthread_cond_destroy(&p->changed);
    pthread_mutex_destroy(&p->mutex);
    free(p);
}
