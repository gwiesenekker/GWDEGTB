#define _POSIX_C_SOURCE 200809L

#include "gwdegtb.h"

#include "endgame_index.h"
#include "material.h"
#include "wdl.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t compact_to_gwd(uint64_t compact)
{
    uint64_t padded = 0;
    unsigned square;
    for (square = 0; square < 50; ++square)
        if ((compact & (UINT64_C(1) << square)) != 0)
            padded |= UINT64_C(1) << (square + 6 + square / 10);
    return padded;
}

static int16_t expected_result(uint64_t index, EgtbSide side)
{
    switch ((index + (uint64_t)side) % 3) {
    case 0: return GWDEGTB_WDL_DRAW;
    case 1: return GWDEGTB_WDL_WIN;
    default: return GWDEGTB_WDL_LOSS;
    }
}

static int16_t source_dtm(uint64_t index, EgtbSide side)
{
    int16_t result = expected_result(index, side);
    int16_t distance = (int16_t)((index / 3 + (uint64_t)side) % 5);
    if (result == GWDEGTB_WDL_DRAW)
        return EGTB_DRAW;
    return result == GWDEGTB_WDL_WIN
               ? (int16_t)(2 * distance + 1)
               : (int16_t)(-2 * distance);
}

typedef struct {
    const EgIndexer *indexer;
    GwdegtbWdlProbe *probe;
    uint64_t first;
    uint64_t end;
    bool failed;
} ProbeWorker;

static void *probe_worker(void *argument)
{
    ProbeWorker *worker = argument;
    for (uint64_t index = worker->first; index < worker->end; ++index) {
        EgPosition indexed;
        DraughtsPosition position, mirrored;
        if (!eg_index_to_position(worker->indexer, index, &indexed)) {
            worker->failed = true;
            break;
        }
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        egtb_mirror_position(&position, &mirrored);
        for (unsigned side = 0; side < 2; ++side) {
            EgtbSide canonical_side = (EgtbSide)side;
            EgtbSide mirrored_side = egtb_mirror_side(canonical_side);
            int16_t expected = expected_result(index, canonical_side);
            int16_t direct = gwdegtb_wdl_lookup_probe(
                worker->probe,
                compact_to_gwd(position.white_kings),
                compact_to_gwd(position.white_men),
                compact_to_gwd(position.black_kings),
                compact_to_gwd(position.black_men),
                (GwdegtbSide)canonical_side);
            int16_t mirror = gwdegtb_wdl_lookup_probe(
                worker->probe,
                compact_to_gwd(mirrored.white_kings),
                compact_to_gwd(mirrored.white_men),
                compact_to_gwd(mirrored.black_kings),
                compact_to_gwd(mirrored.black_men),
                (GwdegtbSide)mirrored_side);
            int16_t direct_compact = gwdegtb_wdl_lookup_probe_compact(
                worker->probe, position.white_kings, position.white_men,
                position.black_kings, position.black_men,
                (GwdegtbSide)canonical_side);
            int16_t mirror_compact = gwdegtb_wdl_lookup_probe_compact(
                worker->probe, mirrored.white_kings, mirrored.white_men,
                mirrored.black_kings, mirrored.black_men,
                (GwdegtbSide)mirrored_side);
            if (direct != expected || mirror != expected ||
                direct_compact != expected || mirror_compact != expected) {
                worker->failed = true;
                return NULL;
            }
        }
    }
    return NULL;
}

int main(void)
{
    char directory[] = "/tmp/gwdegtb-resident-XXXXXX";
    char dtm_path[256] = {0}, wdl_path[256] = {0};
    EgtbCreateOptions options = {8, 20, 3};
    Egtb *dtm = NULL;
    EgIndexer indexer;
    bool indexer_ready = false;
    unsigned char *bitmap = NULL;
    unsigned char *reference_bitmap = NULL;
    unsigned char *shared_bitmap = NULL;
    unsigned char *compressed_image = NULL;
    GwdegtbWdlProbe *probes[2] = {NULL, NULL};
    pthread_t probe_threads[2];
    ProbeWorker probe_workers[2];
    unsigned probe_threads_created = 0;
    unsigned probe_threads_joined = 0;
    size_t bitmap_bytes = 0, mirror_bytes = 0;
    size_t compressed_bytes = 0;
    uint64_t position_count, maximum_index, mirror_maximum, index;
    bool ok = false;

    memset(&indexer, 0, sizeof(indexer));
    if (mkdtemp(directory) == NULL)
        goto done;
    snprintf(dtm_path, sizeof(dtm_path),
             "%s/1wX-0wO-0bX-1bO.dtm", directory);
    snprintf(wdl_path, sizeof(wdl_path),
             "%s/1wX-0wO-0bX-1bO.wdl", directory);
    compressed_bytes = (size_t)-1;
    if (!gwdegtb_wdl_compressed_info(directory,
                                     "1wX-0wO-0bX-1bO",
                                     &compressed_bytes) ||
        compressed_bytes != 0 || access(wdl_path, F_OK) == 0)
        goto done;
    if (!eg_indexer_init(&indexer, 0, 1, 1, 0))
        goto done;
    indexer_ready = true;
    position_count = eg_position_count(&indexer);
    if (!egtb_create(&dtm, dtm_path, position_count - 1, 1024, &options))
        goto done;
    for (index = 0; index < position_count; ++index) {
        if (!egtb_set(dtm, index, EGTB_WHITE_TO_MOVE,
                      source_dtm(index, EGTB_WHITE_TO_MOVE)) ||
            !egtb_set(dtm, index, EGTB_BLACK_TO_MOVE,
                      source_dtm(index, EGTB_BLACK_TO_MOVE)))
            goto done;
    }
    if (!egtb_close(dtm))
        goto done;
    dtm = NULL;
    if (access(wdl_path, F_OK) == 0)
        goto done;

    if (gwdegtb_wdl_is_loaded(1, 0, 0, 1) ||
        gwdegtb_wdl_info("1wX-0wO-0bX-1bO.bad",
                         &maximum_index, &bitmap_bytes) ||
        !gwdegtb_wdl_info("1wX-0wO-0bX-1bO",
                          &maximum_index, &bitmap_bytes) ||
        !gwdegtb_wdl_info("0wX-1wO-1bX-0bO.wdl",
                          &mirror_maximum, &mirror_bytes) ||
        maximum_index != position_count - 1 ||
        mirror_maximum != maximum_index || mirror_bytes != bitmap_bytes ||
        bitmap_bytes != (position_count + 1) / 2)
        goto done;
    bitmap = malloc(bitmap_bytes);
    if (bitmap == NULL ||
        gwdegtb_wdl_decompress(directory, "1wX-0wO-0bX-1bO",
                               bitmap, bitmap_bytes - 1) ||
        access(wdl_path, F_OK) == 0 ||
        !gwdegtb_wdl_decompress(directory, "0wX-1wO-1bX-0bO.wdl",
                                bitmap, bitmap_bytes) ||
        access(wdl_path, R_OK) != 0 ||
        gwdegtb_wdl_is_loaded(1, 0, 0, 1) ||
        !gwdegtb_wdl_attach("1wX-0wO-0bX-1bO", bitmap, bitmap_bytes) ||
        !gwdegtb_wdl_is_loaded(1, 0, 0, 1) ||
        !gwdegtb_wdl_is_loaded(0, 1, 1, 0))
        goto done;

    reference_bitmap = malloc(bitmap_bytes);
    if (reference_bitmap == NULL ||
        gwdegtb_wdl_decompress_threads(directory, "1wX-0wO-0bX-1bO",
                                       reference_bitmap, bitmap_bytes, 0) ||
        !gwdegtb_wdl_decompress_threads(directory, "1wX-0wO-0bX-1bO",
                                        reference_bitmap, bitmap_bytes, 1) ||
        memcmp(bitmap, reference_bitmap, bitmap_bytes) != 0)
        goto done;
    free(reference_bitmap);
    reference_bitmap = NULL;

    shared_bitmap = malloc(bitmap_bytes);
    if (shared_bitmap == NULL)
        goto done;
    memcpy(shared_bitmap, bitmap, bitmap_bytes);
    if (gwdegtb_wdl_attach("1wX-0wO-0bX-1bO", shared_bitmap,
                           bitmap_bytes - 1) ||
        !gwdegtb_wdl_attach("0wX-1wO-1bX-0bO", shared_bitmap,
                            bitmap_bytes))
        goto done;
    free(bitmap);
    bitmap = NULL;

    if (!gwdegtb_wdl_compressed_info(directory, "1wX-0wO-0bX-1bO",
                                     &compressed_bytes) ||
        compressed_bytes == 0)
        goto done;
    compressed_image = malloc(compressed_bytes);
    if (compressed_image == NULL ||
        gwdegtb_wdl_compressed_load(directory, "1wX-0wO-0bX-1bO",
                                    compressed_image,
                                    compressed_bytes - 1) ||
        !gwdegtb_wdl_compressed_load(directory, "0wX-1wO-1bX-0bO.wdl",
                                     compressed_image, compressed_bytes) ||
        !gwdegtb_wdl_compressed_attach("0wX-1wO-1bX-0bO",
                                       compressed_image,
                                       compressed_bytes) ||
        !gwdegtb_wdl_compressed_is_loaded(1, 0, 0, 1) ||
        !gwdegtb_wdl_compressed_is_loaded(0, 1, 1, 0) ||
        gwdegtb_wdl_compressed_is_loaded(1, 0, 1, 0))
        goto done;
    for (unsigned worker = 0; worker < 2; ++worker) {
        if (!gwdegtb_wdl_probe_create(2 * WDL_PAGE_SIZE,
                                      &probes[worker]))
            goto done;
        probe_workers[worker].indexer = &indexer;
        probe_workers[worker].probe = probes[worker];
        probe_workers[worker].first = worker * position_count / 2;
        probe_workers[worker].end = (worker + 1) * position_count / 2;
        probe_workers[worker].failed = false;
        if (pthread_create(&probe_threads[worker], NULL, probe_worker,
                           &probe_workers[worker]) != 0)
            goto done;
        ++probe_threads_created;
    }
    for (unsigned worker = 0; worker < probe_threads_created; ++worker)
        if (pthread_join(probe_threads[worker], NULL) != 0)
            goto done;
        else
            ++probe_threads_joined;
    probe_threads_created = 0;
    probe_threads_joined = 0;
    for (unsigned worker = 0; worker < 2; ++worker) {
        GwdegtbWdlProbeStatistics statistics;
        gwdegtb_wdl_probe_statistics(probes[worker], &statistics);
        if (probe_workers[worker].failed ||
            statistics.requested_cache_bytes != 2 * WDL_PAGE_SIZE ||
            statistics.allocated_cache_bytes == 0 ||
            statistics.allocated_cache_bytes >
                statistics.requested_cache_bytes ||
            statistics.cache_entries == 0 || statistics.lookups == 0 ||
            statistics.misses == 0 || statistics.decompressions == 0 ||
            statistics.lookups != statistics.hits + statistics.misses)
            goto done;
    }

    if (gwdegtb_dtm_lookup(directory, 0,
                           compact_to_gwd(UINT64_C(1) << 10), 0,
                           0, compact_to_gwd(UINT64_C(1) << 20),
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_DTM_UNAVAILABLE ||
        gwdegtb_dtm_lookup_compact(
            directory, 0, UINT64_C(1) << 10, 0,
            0, UINT64_C(1) << 20, GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_DTM_UNAVAILABLE ||
        gwdegtb_dtm_lookup(directory, 4096,
                           compact_to_gwd(UINT64_C(1) << 10), 0,
                           compact_to_gwd(UINT64_C(1) << 20), 0,
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_DTM_UNAVAILABLE)
        goto done;

    for (index = 0; index < position_count; ++index) {
        EgPosition indexed;
        DraughtsPosition position, mirrored;
        unsigned side;
        if (!eg_index_to_position(&indexer, index, &indexed))
            goto done;
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        egtb_mirror_position(&position, &mirrored);
        for (side = 0; side < 2; ++side) {
            EgtbSide canonical_side = (EgtbSide)side;
            EgtbSide mirrored_side = egtb_mirror_side(canonical_side);
            int16_t expected = expected_result(index, canonical_side);
            int16_t expected_dtm = source_dtm(index, canonical_side);
            int16_t direct = gwdegtb_wdl_lookup(
                compact_to_gwd(position.white_kings),
                compact_to_gwd(position.white_men),
                compact_to_gwd(position.black_kings),
                compact_to_gwd(position.black_men),
                (GwdegtbSide)canonical_side);
            int16_t mirror = gwdegtb_wdl_lookup(
                compact_to_gwd(mirrored.white_kings),
                compact_to_gwd(mirrored.white_men),
                compact_to_gwd(mirrored.black_kings),
                compact_to_gwd(mirrored.black_men),
                (GwdegtbSide)mirrored_side);
            int16_t direct_compact = gwdegtb_wdl_lookup_compact(
                position.white_kings, position.white_men,
                position.black_kings, position.black_men,
                (GwdegtbSide)canonical_side);
            int16_t mirror_compact = gwdegtb_wdl_lookup_compact(
                mirrored.white_kings, mirrored.white_men,
                mirrored.black_kings, mirrored.black_men,
                (GwdegtbSide)mirrored_side);
            int16_t direct_dtm = gwdegtb_dtm_lookup(directory, 4096,
                compact_to_gwd(position.white_kings),
                compact_to_gwd(position.white_men),
                compact_to_gwd(position.black_kings),
                compact_to_gwd(position.black_men),
                (GwdegtbSide)canonical_side);
            int16_t mirror_dtm = gwdegtb_dtm_lookup(directory, 4096,
                compact_to_gwd(mirrored.white_kings),
                compact_to_gwd(mirrored.white_men),
                compact_to_gwd(mirrored.black_kings),
                compact_to_gwd(mirrored.black_men),
                (GwdegtbSide)mirrored_side);
            int16_t direct_dtm_compact = gwdegtb_dtm_lookup_compact(
                directory, 4096, position.white_kings, position.white_men,
                position.black_kings, position.black_men,
                (GwdegtbSide)canonical_side);
            int16_t mirror_dtm_compact = gwdegtb_dtm_lookup_compact(
                directory, 4096, mirrored.white_kings, mirrored.white_men,
                mirrored.black_kings, mirrored.black_men,
                (GwdegtbSide)mirrored_side);
            if (direct != expected || mirror != expected ||
                direct_compact != expected || mirror_compact != expected ||
                direct_dtm != expected_dtm || mirror_dtm != expected_dtm ||
                direct_dtm_compact != expected_dtm ||
                mirror_dtm_compact != expected_dtm)
                goto done;
        }
    }

    gwdegtb_dtm_close_all();
    if (gwdegtb_dtm_lookup("/no/such/GWDEGTB/directory", 4096,
                           compact_to_gwd(UINT64_C(1) << 10), 0,
                           0, compact_to_gwd(UINT64_C(1) << 20),
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_DTM_UNAVAILABLE ||
        gwdegtb_dtm_lookup(directory, 4096,
                           compact_to_gwd(UINT64_C(1) << 10), 0,
                           0, compact_to_gwd(UINT64_C(1) << 20),
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_DTM_UNAVAILABLE ||
        gwdegtb_dtm_lookup(directory, 4096, 0, 0,
                           compact_to_gwd(UINT64_C(1) << 20), 0,
                           GWDEGTB_WHITE_TO_MOVE) != 0 ||
        gwdegtb_dtm_lookup_compact(
            directory, 4096, 0, 0, UINT64_C(1) << 20, 0,
            GWDEGTB_WHITE_TO_MOVE) != 0 ||
        gwdegtb_dtm_lookup(directory, 4096,
                           compact_to_gwd(UINT64_C(1) << 10), 0,
                           0, 0, GWDEGTB_BLACK_TO_MOVE) != 0)
        goto done;
    gwdegtb_dtm_close_all();

    if (gwdegtb_wdl_lookup(UINT64_C(1), 0, 0, 0,
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_WDL_UNAVAILABLE ||
        gwdegtb_wdl_lookup_compact(UINT64_C(1) << 50, 0, 0, 0,
                                   GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_WDL_UNAVAILABLE ||
        gwdegtb_wdl_lookup_compact(0, UINT64_C(1),
                                   UINT64_C(1) << 20, 0,
                                   GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_WDL_UNAVAILABLE ||
        gwdegtb_wdl_lookup_probe_compact(
            probes[0], UINT64_C(1) << 50, 0, 0, 0,
            GWDEGTB_WHITE_TO_MOVE) != GWDEGTB_WDL_UNAVAILABLE ||
        gwdegtb_wdl_lookup(compact_to_gwd(UINT64_C(1) << 10), 0,
                           compact_to_gwd(UINT64_C(1) << 20), 0,
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_WDL_UNAVAILABLE)
        goto done;

    gwdegtb_wdl_unload_all();
    for (unsigned worker = 0; worker < 2; ++worker) {
        gwdegtb_wdl_probe_destroy(probes[worker]);
        probes[worker] = NULL;
    }
    gwdegtb_wdl_compressed_unload_all();
    free(compressed_image);
    compressed_image = NULL;
    free(bitmap);
    free(reference_bitmap);
    free(shared_bitmap);
    shared_bitmap = NULL;
    if (gwdegtb_wdl_is_loaded(1, 0, 0, 1))
        goto done;
    ok = true;

done:
    while (probe_threads_joined < probe_threads_created)
        pthread_join(probe_threads[probe_threads_joined++], NULL);
    for (unsigned worker = 0; worker < 2; ++worker)
        gwdegtb_wdl_probe_destroy(probes[worker]);
    gwdegtb_wdl_compressed_unload_all();
    gwdegtb_dtm_close_all();
    gwdegtb_wdl_unload_all();
    free(bitmap);
    free(shared_bitmap);
    free(compressed_image);
    if (dtm != NULL && !egtb_close(dtm))
        ok = false;
    if (indexer_ready)
        eg_indexer_destroy(&indexer);
    if (wdl_path[0] != '\0')
        unlink(wdl_path);
    if (dtm_path[0] != '\0')
        unlink(dtm_path);
    rmdir(directory);
    if (!ok) {
        fprintf(stderr, "resident GWD WDL test failed: %s / %s\n",
                gwdegtb_last_error(), wdl_last_error());
        return EXIT_FAILURE;
    }
    printf("GWD resident/compressed WDL and disk-cached DTM lookup/mirroring tests passed\n");
    return EXIT_SUCCESS;
}
