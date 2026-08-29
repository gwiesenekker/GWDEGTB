#define _POSIX_C_SOURCE 200809L

#include "gwdegtb.h"

#include "endgame_index.h"
#include "material.h"
#include "wdl.h"

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
    if (result == GWDEGTB_WDL_DRAW)
        return EGTB_DRAW;
    return result == GWDEGTB_WDL_WIN ? 1 : -2;
}

int main(void)
{
    char directory[] = "/tmp/gwdegtb-resident-XXXXXX";
    char dtm_path[256] = {0}, wdl_path[256] = {0};
    EgtbCreateOptions options = {8, 20, 3};
    Egtb *dtm = NULL;
    EgIndexer indexer;
    bool indexer_ready = false;
    WdlStatistics wdl_statistics;
    WdlStorageStatistics storage_statistics;
    unsigned char *bitmap = NULL;
    unsigned char *shared_bitmap = NULL;
    size_t bitmap_bytes = 0, mirror_bytes = 0;
    uint64_t position_count, maximum_index, mirror_maximum, index;
    bool ok = false;

    memset(&indexer, 0, sizeof(indexer));
    if (mkdtemp(directory) == NULL)
        goto done;
    snprintf(dtm_path, sizeof(dtm_path),
             "%s/1wX-0wO-0bX-1bO.dtm", directory);
    snprintf(wdl_path, sizeof(wdl_path),
             "%s/1wX-0wO-0bX-1bO.wdl", directory);
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
    if (!wdl_compile(dtm_path, wdl_path, 3, 8, &wdl_statistics,
                     &storage_statistics))
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
        !gwdegtb_wdl_decompress(directory, "1wX-0wO-0bX-1bO",
                                bitmap, bitmap_bytes) ||
        gwdegtb_wdl_is_loaded(1, 0, 0, 1) ||
        !gwdegtb_wdl_attach("1wX-0wO-0bX-1bO", bitmap, bitmap_bytes) ||
        !gwdegtb_wdl_is_loaded(1, 0, 0, 1) ||
        !gwdegtb_wdl_is_loaded(0, 1, 1, 0))
        goto done;

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
            if (direct != expected || mirror != expected)
                goto done;
        }
    }

    if (gwdegtb_wdl_lookup(UINT64_C(1), 0, 0, 0,
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_WDL_UNAVAILABLE ||
        gwdegtb_wdl_lookup(compact_to_gwd(UINT64_C(1) << 10), 0,
                           compact_to_gwd(UINT64_C(1) << 20), 0,
                           GWDEGTB_WHITE_TO_MOVE) !=
            GWDEGTB_WDL_UNAVAILABLE)
        goto done;

    gwdegtb_wdl_unload_all();
    free(bitmap);
    free(shared_bitmap);
    shared_bitmap = NULL;
    if (gwdegtb_wdl_is_loaded(1, 0, 0, 1))
        goto done;
    ok = true;

done:
    gwdegtb_wdl_unload_all();
    free(bitmap);
    free(shared_bitmap);
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
    printf("GWD resident WDL lookup and mirroring tests passed\n");
    return EXIT_SUCCESS;
}
