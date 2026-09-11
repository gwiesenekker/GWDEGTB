#define _POSIX_C_SOURCE 200809L
#include "dependency_resident.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s / %s\n", \
    __LINE__, egtb_last_error(), dependency_resident_error()); exit(1); } } while (0)

enum { N = 4097, THREADS = 16 };
typedef struct {
    DependencyResidentPool *pool;
    Egtb *db;
    pthread_barrier_t *barrier;
    const EgtbResident *resident;
    bool success;
} Worker;

static void *acquire(void *arg)
{
    Worker *w = arg;
    pthread_barrier_wait(w->barrier);
    w->success = dependency_resident_acquire(w->pool, w->db, &w->resident);
    if (w->success && w->resident) {
        for (uint64_t i = 0; i < N; ++i) {
            int16_t white, black;
            CHECK(egtb_resident_get_pair(w->resident, i, &white, &black));
            CHECK(white == (i == 0 ? 305 : i == N - 1 ? 1 : -1));
            CHECK(black == (i == 0 ? -304 : i == N - 1 ? 0 : -1));
        }
    }
    return NULL;
}

static void concurrent(DependencyResidentPool *pool, Egtb *a, Egtb *b,
                        bool expected_success, bool expected_resident)
{
    Worker w[THREADS] = {0}; pthread_t threads[THREADS];
    pthread_barrier_t barrier;
    CHECK(pthread_barrier_init(&barrier, NULL, THREADS) == 0);
    for (unsigned t = 0; t < THREADS; ++t) {
        w[t].pool = pool; w[t].db = t & 1 ? a : b; w[t].barrier = &barrier;
        CHECK(pthread_create(&threads[t], NULL, acquire, &w[t]) == 0);
    }
    for (unsigned t = 0; t < THREADS; ++t) {
        CHECK(pthread_join(threads[t], NULL) == 0);
        CHECK(w[t].success == expected_success);
        CHECK((w[t].resident != NULL) == expected_resident);
        CHECK(w[t].resident == w[0].resident);
    }
    CHECK(pthread_barrier_destroy(&barrier) == 0);
}

int main(int argc, char **argv)
{
    /* Optional end-to-end comparison of cache-only versus resident runs. */
    if (argc == 3) {
        Egtb *a, *b; EgtbView *av, *bv;
        EgtbSequentialReader ar, br;
        CHECK(egtb_open_readonly(&a, argv[1], 1));
        CHECK(egtb_open_readonly(&b, argv[2], 1));
        CHECK(egtb_maximum_index(a) == egtb_maximum_index(b));
        uint64_t count = egtb_maximum_index(a) + 1;
        CHECK(egtb_view_create(&av, a, 1, false));
        CHECK(egtb_view_create(&bv, b, 1, false));
        CHECK(egtb_sequential_reader_init(&ar, av, 0, count));
        CHECK(egtb_sequential_reader_init(&br, bv, 0, count));
        for (uint64_t i = 0; i < count; ++i) {
            int16_t aw, ab, bw, bb;
            CHECK(egtb_sequential_reader_next(&ar, &aw, &ab));
            CHECK(egtb_sequential_reader_next(&br, &bw, &bb));
            CHECK(aw == bw && ab == bb);
        }
        CHECK(egtb_view_close(av) && egtb_view_close(bv));
        CHECK(egtb_close(a) && egtb_close(b));
        printf("dependency residency end-to-end: %llu paired values identical\n",
               (unsigned long long)count);
        return 0;
    }
    CHECK(argc == 1);
    char dir[] = "/tmp/gwdegtb-dependency-test-XXXXXX", path[256], other[256];
    CHECK(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/a.dtm", dir);
    snprintf(other, sizeof(other), "%s/b.dtm", dir);
    EgtbCreateOptions options = {2, 20, 1};
    Egtb *db, *a, *alias, *b;
    CHECK(egtb_create(&db, path, N - 1, 2048, &options));
    CHECK(egtb_set(db, 0, EGTB_WHITE_TO_MOVE, 305));
    CHECK(egtb_set(db, 0, EGTB_BLACK_TO_MOVE, -304));
    CHECK(egtb_set(db, N - 1, EGTB_WHITE_TO_MOVE, 1));
    CHECK(egtb_set(db, N - 1, EGTB_BLACK_TO_MOVE, 0));
    CHECK(egtb_close(db));
    CHECK(egtb_create(&db, other, N - 1, 2048, &options));
    CHECK(egtb_set(db, 0, EGTB_WHITE_TO_MOVE, 305));
    CHECK(egtb_set(db, 0, EGTB_BLACK_TO_MOVE, -304));
    CHECK(egtb_set(db, N - 1, EGTB_WHITE_TO_MOVE, 1));
    CHECK(egtb_set(db, N - 1, EGTB_BLACK_TO_MOVE, 0));
    CHECK(egtb_close(db));
    CHECK(egtb_open_readonly(&a, path, 1));
    CHECK(egtb_open_readonly(&alias, path, 1));
    CHECK(a == alias); /* Shared backing registry is the pool identity. */
    CHECK(egtb_open_readonly(&b, other, 1));
    DependencyResidentPool *pool;
    uint64_t bytes = N * sizeof(EgtbEntry);
    CHECK(dependency_resident_create(&pool, bytes, bytes));
    concurrent(pool, a, alias, true, true);
    CHECK(dependency_resident_used(pool) == bytes);
    const EgtbResident *r;
    CHECK(dependency_resident_acquire(pool, b, &r) && r == NULL);
    CHECK(dependency_resident_used(pool) == bytes);
    dependency_resident_destroy(pool);
    CHECK(dependency_resident_create(&pool, 0, bytes));
    concurrent(pool, a, alias, true, false);
    CHECK(dependency_resident_used(pool) == 0);
    dependency_resident_destroy(pool);
    CHECK(dependency_resident_create(&pool, bytes, bytes - 1));
    concurrent(pool, a, alias, true, false);
    dependency_resident_destroy(pool);
    CHECK(dependency_resident_create(&pool, bytes, bytes));
    {
        pthread_barrier_t barrier; pthread_t threads[2];
        CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
        Worker workers[2] = {{pool, a, &barrier, NULL, false},
                             {pool, b, &barrier, NULL, false}};
        for (unsigned i = 0; i < 2; ++i)
            CHECK(pthread_create(&threads[i], NULL, acquire, &workers[i]) == 0);
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(pthread_join(threads[i], NULL) == 0);
            CHECK(workers[i].success);
        }
        CHECK((workers[0].resident != NULL) + (workers[1].resident != NULL) == 1);
        CHECK(dependency_resident_used(pool) == bytes);
        CHECK(pthread_barrier_destroy(&barrier) == 0);
    }
    dependency_resident_destroy(pool);
    CHECK(egtb_close(alias)); CHECK(egtb_close(a)); CHECK(egtb_close(b));

    /* Damage the first block's CRC: all waiters must fail without publishing
     * a partial array or silently caching. The reservation must be released. */
    FILE *f = fopen(path, "r+b"); CHECK(f);
    unsigned char offset_bytes[8]; uint64_t offset = 0;
    CHECK(fseek(f, 64, SEEK_SET) == 0 && fread(offset_bytes, 1, 8, f) == 8);
    for (unsigned i = 0; i < 8; ++i) offset |= (uint64_t)offset_bytes[i] << (8 * i);
    CHECK(offset && fseek(f, (long)offset, SEEK_SET) == 0);
    int value = fgetc(f); CHECK(value != EOF);
    CHECK(fseek(f, (long)offset, SEEK_SET) == 0 && fputc(value ^ 1, f) != EOF);
    CHECK(fclose(f) == 0);
    CHECK(egtb_open_readonly(&a, path, 1));
    CHECK(dependency_resident_create(&pool, bytes, bytes));
    concurrent(pool, a, a, false, false);
    CHECK(dependency_resident_used(pool) == 0);
    dependency_resident_destroy(pool);
    CHECK(egtb_close(a));
    CHECK(unlink(path) == 0 && unlink(other) == 0 && rmdir(dir) == 0);
    puts("shared dependency residency: concurrent load, limits, wide DTM and corruption PASS");
    return 0;
}
