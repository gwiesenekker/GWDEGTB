#include "egtb.h"
#include "endgame_index.h"
#include "generator.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void report_correction(uint64_t index, EgtbSide side,
                              const DraughtsPosition *position,
                              int16_t old_value, int16_t new_value,
                              void *context)
{
    (void)context;
    printf("consistency correction: index=%" PRIu64 " %s "
           "WM=%013" PRIx64 " BM=%013" PRIx64
           " WK=%013" PRIx64 " BK=%013" PRIx64
           " was=%d now=%d\n",
           index, side == EGTB_WHITE_TO_MOVE ? "WTM" : "BTM",
           position->white_men, position->black_men,
           position->white_kings, position->black_kings,
           old_value, new_value);
}

int main(int argc, char **argv)
{
    const char *path = "0wO-0bO-1wX-1bX.dtm";
    EgtbCreateOptions options = {64, 20, 9};
    EgtbGenerationStatistics generation;
    EgtbStorageStatistics storage;
    EgIndexer indexer;
    Egtb *database = NULL;
    uint64_t positions;
    uint64_t *histogram = NULL;
    bool created = false;
    bool ok = false;
    if (argc == 2)
        path = argv[1];
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [OUTPUT.dtm]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!eg_indexer_init(&indexer, 0, 0, 1, 1)) {
        fprintf(stderr, "cannot initialize WK-BK indexer\n");
        return EXIT_FAILURE;
    }
    positions = eg_position_count(&indexer);
    if (!egtb_create(&database, path, positions - 1, 1024, &options)) {
        fprintf(stderr, "cannot create %s: %s\n", path, egtb_last_error());
        goto done;
    }
    created = true;
    if (!egtb_generate(database, &indexer, report_correction, NULL,
                       &generation)) {
        fprintf(stderr, "cannot generate %s: %s\n", path,
                egtb_generator_last_error());
        goto done;
    }
    /* This is the first explicit save: every retrograde pass has converged. */
    if (!egtb_flush(database)) {
        fprintf(stderr, "cannot finalize %s: %s\n", path, egtb_last_error());
        goto done;
    }
    if (!egtb_close(database)) {
        database = NULL;
        fprintf(stderr, "cannot close %s: %s\n", path, egtb_last_error());
        goto done;
    }
    database = NULL;
    if (!egtb_compact(path, 9, 16) ||
        !egtb_open_readonly(&database, path, 16) ||
        !egtb_storage_statistics(database, &storage)) {
        fprintf(stderr, "cannot compact/reopen %s: %s\n", path,
                egtb_last_error());
        goto done;
    }
    histogram = calloc((size_t)2 * (UINT16_MAX + 1u), sizeof(*histogram));
    if (histogram == NULL) {
        fprintf(stderr, "cannot allocate DTM histogram\n");
        goto done;
    }
    for (uint64_t index = 0; index < positions; ++index) {
        for (unsigned side = 0; side < 2; ++side) {
            int16_t value;
            if (!egtb_get(database, index, (EgtbSide)side, &value)) {
                fprintf(stderr, "cannot collect final DTM statistics: %s\n",
                        egtb_last_error());
                goto done;
            }
            ++histogram[(size_t)side * (UINT16_MAX + 1u) + (uint16_t)value];
        }
    }
    printf("generated %s: positions=%" PRIu64 " maximum-index=%" PRIu64
           " passes=%" PRIu64 " maximum-dtm=%u\n",
           path, positions, positions - 1, generation.retrograde_passes,
           generation.maximum_dtm);
    printf("consistency: passes=%" PRIu64 " updates=%" PRIu64 "/%" PRIu64
           "\n", generation.consistency_passes,
           generation.consistency_updates[0],
           generation.consistency_updates[1]);
    for (unsigned side = 0; side < 2; ++side) {
        uint64_t wins = 0, losses = 0, draws = 0;
        for (int value = INT16_MIN; value <= INT16_MAX; ++value) {
            uint64_t count = histogram[(size_t)side * (UINT16_MAX + 1u) +
                                       (uint16_t)(int16_t)value];
            if (value == EGTB_DRAW)
                draws += count;
            else if (value > 0)
                wins += count;
            else
                losses += count;
        }
        printf("%s: wins=%" PRIu64 " losses=%" PRIu64
               " draws=%" PRIu64 "\n",
               side == EGTB_WHITE_TO_MOVE ? "WTM" : "BTM",
               wins, losses, draws);
        printf("%s DTM:", side == EGTB_WHITE_TO_MOVE ? "WTM" : "BTM");
        for (int numeric = INT16_MIN; numeric <= INT16_MAX; ++numeric) {
            int16_t value = (int16_t)numeric;
            uint64_t count = histogram[(size_t)side * (UINT16_MAX + 1u) +
                                       (uint16_t)value];
            if (count != 0)
                printf(" %d=%" PRIu64, value, count);
        }
        putchar('\n');
    }
    printf("storage: raw=%" PRIu64 " payload=%" PRIu64
           " file=%" PRIu64 " bytes overall=%.2f%% (%.2f:1)\n",
           storage.logical_uncompressed_bytes,
           storage.compressed_payload_bytes, storage.file_bytes,
           100.0 * (double)storage.file_bytes /
               (double)storage.logical_uncompressed_bytes,
           (double)storage.logical_uncompressed_bytes /
               (double)storage.file_bytes);
    ok = true;
done:
    if (database != NULL && !egtb_close(database))
        ok = false;
    free(histogram);
    eg_indexer_destroy(&indexer);
    if (!ok && created)
        unlink(path);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
