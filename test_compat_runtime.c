#define _POSIX_C_SOURCE 200809L
#include "compat.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); } } while (0)
static compat_once_t once = COMPAT_ONCE_INITIALIZER;
static atomic_int initialized;
static void initialize(void) { atomic_fetch_add(&initialized, 1); }
typedef struct { int fd; unsigned id; int failed; } Worker;
static void *io_worker(void *arg)
{
    Worker *w = arg;
    for (unsigned i = 0; i < 1000; ++i) {
        uint64_t value = ((uint64_t)w->id << 32) | i, readback = 0;
        int64_t offset = INT64_C(4294967296) + 8192 * w->id + 8 * i;
        if (compat_once(&once, initialize) ||
            compat_pwrite(w->fd, &value, sizeof(value), offset) != sizeof(value) ||
            compat_pread(w->fd, &readback, sizeof(readback), offset) != sizeof(readback) ||
            value != readback) { w->failed = 1; break; }
    }
    return NULL;
}
int main(void)
{
    REQUIRE(compat_thread_create(NULL, io_worker, NULL) == EINVAL);
    REQUIRE(compat_pread(-1, NULL, 0, -1) == -1 && errno == EINVAL);
    REQUIRE(compat_ftruncate(-1, -1) == -1 && errno == EINVAL);
    REQUIRE(compat_aligned_alloc(3, 64) == NULL && errno == EINVAL);
    REQUIRE(compat_getrandom_u64(NULL) == -1 && errno == EINVAL);
    REQUIRE(compat_sleep(-1) == -1 && errno == EINVAL);
    REQUIRE(compat_mulhi_u64(UINT64_MAX, UINT64_MAX) == UINT64_MAX - 1);
    REQUIRE(compat_mulhi_u64(UINT64_C(1) << 63, 2) == 1);
    REQUIRE(compat_mulhi_u64(0, UINT64_MAX) == 0);
    void *aligned = compat_aligned_alloc(64, 129);
    REQUIRE(aligned && (uintptr_t)aligned % 64 == 0);
    compat_aligned_free(aligned);

    my_mutex_t mutex = COMPAT_MUTEX_INITIALIZER;
    compat_cond_t condition;
    REQUIRE(compat_cond_init(&condition) == 0);
    REQUIRE(compat_mutex_lock(&mutex) == 0);
    REQUIRE(compat_mutex_trylock(&mutex) != 0);
    struct timespec begin, end;
    REQUIRE(compat_monotonic(&begin) == 0);
    REQUIRE(compat_cond_wait_ms(&condition, &mutex, 20) == ETIMEDOUT);
    REQUIRE(compat_monotonic(&end) == 0);
    REQUIRE(end.tv_sec > begin.tv_sec || (end.tv_sec == begin.tv_sec && end.tv_nsec >= begin.tv_nsec));
    REQUIRE(compat_mutex_unlock(&mutex) == 0);
    REQUIRE(compat_cond_destroy(&condition) == 0);
    REQUIRE(compat_mutex_destroy(&mutex) == 0);

    char path[] = "compat-test-XXXXXX", target[] = "compat-target-XXXXXX";
    int fd = compat_mkstemp(path);
    REQUIRE(fd >= 0);
    my_thread_t threads[4]; Worker workers[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (Worker){fd, i, 0};
        REQUIRE(compat_thread_create(&threads[i], io_worker, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE(compat_thread_join(threads[i]) == 0);
        REQUIRE(!workers[i].failed);
    }
    REQUIRE(atomic_load(&initialized) == 1);
    CompatStat st;
    REQUIRE(compat_fstat(fd, &st) == 0 && st.st_size > INT64_C(4294967296));
    REQUIRE(compat_lseek(fd, INT64_C(4294967296), SEEK_SET) == INT64_C(4294967296));
    REQUIRE(compat_ftruncate(fd, 3) == 0);
    REQUIRE(compat_pwrite(fd, "\r\n\032", 3, 0) == 3);
    REQUIRE(compat_fsync(fd) == 0);
    REQUIRE(compat_close(fd) == 0);
    int destination = compat_mkstemp(target);
    REQUIRE(destination >= 0 && compat_close(destination) == 0);
    REQUIRE(compat_publish_file(path, target) == 0);
    REQUIRE(compat_stat(path, &st) == -1 && errno == ENOENT);
    REQUIRE(compat_stat(target, &st) == 0 && st.st_size == 3);
    FILE *file = fopen(target, "rb");
    REQUIRE(file);
    REQUIRE(compat_fseeko(file, 2, SEEK_SET) == 0 && compat_ftello(file) == 2);
    REQUIRE(fgetc(file) == 26);
    REQUIRE(fclose(file) == 0 && compat_unlink(target) == 0);
    puts("compat runtime tests passed");
    return 0;
}
