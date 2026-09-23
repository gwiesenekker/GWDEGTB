#define _POSIX_C_SOURCE 200809L
#include "egtb_platform.h"
#include "progress.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static my_mutex_t mutex = COMPAT_MUTEX_INITIALIZER;
static compat_cond_t wake;
static my_thread_t reporter;
static bool enabled, stopping, active;
static unsigned interval;
static char phase[160], units[32];
static uint64_t total_work;
static atomic_uint_fast64_t completed_work;
static double started, last_report;

static double now_seconds(void)
{
    struct timespec now = {0};
    if (compat_monotonic(&now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void timestamp(void)
{
    time_t now = time(NULL);
    struct tm local = {0};
    char text[48];
    if (compat_localtime(&now, &local) != 0) {
        printf("[timestamp unavailable] ");
        return;
    }
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %z", &local);
    printf("[%s] ", text);
}

/* Called with mutex held. Use monotonic wall time for rates and ETA: CPU
 * time is misleading with many workers or time spent waiting for I/O. */
static void report(const char *status)
{
    uint64_t done = atomic_load_explicit(&completed_work, memory_order_relaxed);
    double elapsed = now_seconds() - started;
    double rate = elapsed > 0.0 ? (double)done / elapsed : 0.0;
    timestamp();
    printf("%s: %s; elapsed=%.1fs", phase, status, elapsed);
    if (total_work != UINT64_MAX) {
        printf("; %" PRIu64 "/%" PRIu64 " %s (%.1f%%)", done,
               total_work, units,
               total_work ? 100.0 * (double)done / (double)total_work : 100.0);
        if (total_work == 0) {
            printf("; ETA=0.0s remaining");
        } else if (rate > 0.0) {
            double remaining = done < total_work
                                   ? (double)(total_work - done) / rate : 0.0;
            printf("; %.0f %s/s; ETA=%.1fs remaining", rate, units, remaining);
        } else {
            printf("; ETA=pending");
        }
    } else {
        printf("; ETA=unknown");
    }
    putchar('\n');
    fflush(stdout);
}

static void *run_reporter(void *unused)
{
    (void)unused;
    compat_mutex_lock(&mutex);
    while (!stopping) {
        int error = compat_cond_wait_ms(&wake, &mutex, interval * 1000u);
        if (error != 0 && error != ETIMEDOUT) break;
        if (!stopping && active && now_seconds() - last_report >= interval) {
            report("running");
            last_report = now_seconds();
        }
    }
    compat_mutex_unlock(&mutex);
    return NULL;
}

bool egtb_progress_start(unsigned interval_seconds)
{
    if (enabled || interval_seconds == 0) return true;
    if (interval_seconds > 4294967u) return false;
    int error = compat_cond_init(&wake);
    if (error) return false;
    interval = interval_seconds;
    stopping = active = false;
    error = compat_thread_create(&reporter, run_reporter, NULL);
    if (error != 0) {
        compat_cond_destroy(&wake);
        return false;
    }
    enabled = true;
    return true;
}

void egtb_progress_begin(const char *label, uint64_t total, const char *unit)
{
    if (!enabled)
        return;
    compat_mutex_lock(&mutex);
    snprintf(phase, sizeof(phase), "%s", label);
    snprintf(units, sizeof(units), "%s", unit);
    total_work = total;
    atomic_store_explicit(&completed_work, 0, memory_order_relaxed);
    started = now_seconds();
    last_report = started;
    active = true;
    report("started");
    compat_cond_signal(&wake);
    compat_mutex_unlock(&mutex);
}

void egtb_progress_add(uint64_t completed)
{
    if (enabled && completed != 0)
        atomic_fetch_add_explicit(&completed_work, completed, memory_order_relaxed);
}

bool egtb_progress_enabled(void)
{
    return enabled;
}

void egtb_progress_end(bool success)
{
    if (!enabled)
        return;
    compat_mutex_lock(&mutex);
    if (active) {
        report(success ? "completed" : "failed");
        active = false;
    }
    compat_mutex_unlock(&mutex);
}

void egtb_progress_log(const char *format, ...)
{
    va_list arguments;
    compat_mutex_lock(&mutex);
    timestamp();
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    fflush(stdout);
    compat_mutex_unlock(&mutex);
}

void egtb_progress_stop(void)
{
    if (!enabled)
        return;
    compat_mutex_lock(&mutex);
    stopping = true;
    compat_cond_signal(&wake);
    compat_mutex_unlock(&mutex);
    compat_thread_join(reporter);
    enabled = false;
    compat_cond_destroy(&wake);
}
