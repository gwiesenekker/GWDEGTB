#ifndef EGTB_PROGRESS_H
#define EGTB_PROGRESS_H

#include <stdbool.h>
#include <stdint.h>

/* Optional process-wide CLI reporter. Enable before starting workers; stop
 * after joining them. Library users get no output unless they opt in.
 * Phases are not nested: the coordinator begins/ends one phase at a time.
 * total=UINT64_MAX means unknown work; zero means an empty phase. */
bool egtb_progress_start(unsigned interval_seconds);
bool egtb_progress_enabled(void);
void egtb_progress_stop(void);
void egtb_progress_begin(const char *label, uint64_t total, const char *unit);
void egtb_progress_end(bool success);
void egtb_progress_add(uint64_t completed);
void egtb_progress_log(const char *format, ...);

/* Batch hot-loop updates: no clock reads or locks per position. Each worker
 * owns its pending count and must flush it before returning. */
static inline void egtb_progress_tick(uint64_t *pending)
{
    if (++*pending == 1024) {
        egtb_progress_add(*pending);
        *pending = 0;
    }
}
static inline void egtb_progress_flush(uint64_t *pending)
{
    egtb_progress_add(*pending);
    *pending = 0;
}

#endif
