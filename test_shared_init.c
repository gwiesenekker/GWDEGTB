#include "egtb.h"
#include "compat.h"
#include "progress.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef EGTB_TEST_THREAD_FAILURE
static unsigned creates;
int __real_compat_thread_create(my_thread_t *, my_thread_func_t, void *);
int __wrap_compat_thread_create(my_thread_t *thread, my_thread_func_t fn, void *arg)
{
    if (++creates % 2) return EAGAIN;
    return __real_compat_thread_create(thread,fn,arg);
}
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s: %s\n", __LINE__, #x, egtb_last_error()); goto done; } } while (0)
static bool check_probes(EgtbSharedProbe *probe, uint64_t maximum)
{
    for (unsigned repeat = 0; repeat < 2; ++repeat)
        for (unsigned i = 0; i < 129; ++i) {
            uint64_t index = maximum * i / 128;
            for (unsigned side = 0; side < 2; ++side) {
                int16_t value;
                if (!egtb_shared_probe_get(probe,index,(EgtbSide)side,&value) ||
                    value != (side ? -2 : 3)) return false;
            }
        }
    return true;
}

static bool check_pages(EgtbSharedProbe *probe, uint64_t maximum, bool seeded)
{
    for (uint64_t index=0; index<=maximum; index+=1024)
        for (unsigned side=0; side<2; ++side) {
            int16_t value;
            int16_t expected=seeded ? (int16_t)(1+2*((index/1024+side)%63)) : -1;
            if (!egtb_shared_probe_get(probe,index+7,(EgtbSide)side,&value) || value!=expected)
                return false;
        }
    return true;
}

/* Optional benchmark: test_shared_init MiB. Three rounds, reversed middle
 * order. Uses only a temporary sparse/all-draw backing, never a user's DTM. */
int main(int argc, char **argv)
{
    bool bench = argc == 2;
    size_t mib = bench ? (size_t)strtoul(argv[1],NULL,10) : 32;
    if (argc > 2 || mib < 16 || mib > 16384) return 2;
    size_t bytes = mib * 1024 * 1024;
    uint64_t maximum = 2 * (uint64_t)bytes / sizeof(EgtbEntry) - 1;
    char path[] = "shared-init-XXXXXX";
    Egtb *db = NULL; EgtbSharedCache *cache = NULL; EgtbSharedProbe *probe = NULL;
    int result = 1, fd = compat_mkstemp(path);
    if (fd < 0) return 1;
    compat_close(fd); compat_unlink(path);
    EgtbCreateOptions options = {2, 20, 1};
    CHECK(egtb_create(&db,path,maximum,2048,&options));
    if (!bench)
        for (uint64_t index=0; index<=maximum; index+=1024)
            for (unsigned side=0; side<2; ++side)
                CHECK(egtb_set(db,index+7,(EgtbSide)side,
                              (int16_t)(1+2*((index/1024+side)%63))));
    if (!bench)
        for (unsigned i=0; i<129; ++i) {
            CHECK(egtb_set(db,maximum*i/128,EGTB_WHITE_TO_MOVE,3));
            CHECK(egtb_set(db,maximum*i/128,EGTB_BLACK_TO_MOVE,-2));
        }
    CHECK(egtb_close(db)); db=NULL;
    CHECK(egtb_open_readonly(&db,path,1));
    unsigned counts[] = {1,8,16,32,64,7}; /* Seven tests uneven partitions. */
    if (bench) CHECK(egtb_progress_start(60));
    for (unsigned round=0; round<(bench ? 3u : 1u); ++round) {
        for (unsigned n=0; n<(bench ? 5u : 6u); ++n) {
            unsigned threads=counts[round==1 ? 4-n : n];
            CHECK(egtb_shared_cache_create_threads(&cache,db,bytes,threads));
            EgtbSharedInitTiming timing;
            egtb_shared_cache_init_timing(cache,&timing);
            CHECK(timing.initialization_threads <= threads);
            if (threads > 1) CHECK(timing.initialization_threads > 1);
            if (bench) {
                printf("round=%u requested=%u actual=%u MiB=%zu allocate=%.6f initialize=%.6f\n",
                    round+1,threads,timing.initialization_threads,mib,
                    timing.allocation_seconds,timing.initialization_seconds);
                CHECK(egtb_shared_probe_create(&probe,cache));
                CHECK(check_pages(probe,maximum,false));
                CHECK(egtb_shared_cache_resize(cache,bytes*3/2));
                CHECK(check_pages(probe,maximum,false));
                CHECK(egtb_shared_cache_resize(cache,bytes*3/4));
                CHECK(check_pages(probe,maximum,false));
                egtb_shared_probe_destroy(probe); probe=NULL;
            } else {
                CHECK(egtb_shared_probe_create(&probe,cache));
                CHECK(check_pages(probe,maximum,true));
                CHECK(check_probes(probe,maximum));
                CHECK(egtb_shared_cache_resize(cache,bytes*3/2));
                CHECK(check_pages(probe,maximum,true));
                CHECK(egtb_shared_cache_resize(cache,bytes*3/4));
                CHECK(check_pages(probe,maximum,true));
                CHECK(egtb_shared_cache_resize(cache,bytes*2));
                CHECK(egtb_shared_cache_dense(cache));
                CHECK(check_pages(probe,maximum,true));
                CHECK(check_probes(probe,maximum));
                CHECK(egtb_shared_cache_discard_resize(cache,bytes/2));
                CHECK(check_probes(probe,maximum));
                egtb_shared_cache_set_threads(cache,7);
                CHECK(egtb_shared_cache_discard_grow(cache,bytes*2));
                egtb_shared_cache_init_timing(cache,&timing);
#ifndef EGTB_TEST_THREAD_FAILURE
                CHECK(timing.initialization_threads == 7);
#endif
                CHECK(egtb_shared_cache_dense(cache));
                CHECK(check_probes(probe,maximum));
                CHECK(egtb_shared_cache_discard_resize(cache,4096));
                egtb_shared_cache_init_timing(cache,&timing);
                CHECK(timing.initialization_threads == 1);
                CHECK(check_probes(probe,maximum));
                egtb_shared_probe_destroy(probe); probe=NULL;
            }
            egtb_shared_cache_destroy(cache); cache=NULL;
        }
    }
    result=0;
done:
    egtb_progress_stop();
    if (probe) egtb_shared_probe_destroy(probe);
    if (cache) egtb_shared_cache_destroy(cache);
    if (db) egtb_close(db);
    compat_unlink(path);
    return result;
}
