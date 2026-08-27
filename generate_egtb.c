#include "egtb.h"
#include "endgame_index.h"
#include "generator.h"
#include "material.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    GENERATION_PAGE_SIZE = 1024,
    GENERATION_CACHE_BYTES = 256 * 1024 * 1024,
    READONLY_CACHE_BYTES = 16 * 1024 * 1024,
    GENERATION_CACHE_PAGES = GENERATION_CACHE_BYTES / GENERATION_PAGE_SIZE,
    READONLY_CACHE_PAGES = READONLY_CACHE_BYTES / GENERATION_PAGE_SIZE
};

typedef struct {
    EgtbMaterialKind kind;
    EgtbMaterial canonical;
    Egtb *database;
    EgtbView *view;
    EgIndexer indexer;
    bool indexer_initialized;
} CatalogEntry;

typedef struct {
    CatalogEntry entry[8][8][8][8];
    size_t cache_pages;
    char error[256];
} DatabaseCatalog;

static CatalogEntry *catalog_entry(DatabaseCatalog *catalog,
                                   const EgtbMaterial *material)
{
    return &catalog->entry[material->white_kings][material->white_men]
                          [material->black_kings][material->black_men];
}

static void initialize_catalog(DatabaseCatalog *catalog, size_t cache_pages)
{
    unsigned wk, wm, bk, bm;
    memset(catalog, 0, sizeof(*catalog));
    catalog->cache_pages = cache_pages;
    for (wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
        for (wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
            for (bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                for (bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                    EgtbMaterial material = {wk, wm, bk, bm};
                    CatalogEntry *entry = catalog_entry(catalog, &material);
                    entry->kind =
                        egtb_material_resolve(&material, &entry->canonical);
                }
}

static void close_catalog(DatabaseCatalog *catalog)
{
    unsigned wk, wm, bk, bm;
    for (wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
        for (wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
            for (bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                for (bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                    CatalogEntry *entry = &catalog->entry[wk][wm][bk][bm];
                    if (entry->view != NULL) {
                        egtb_view_close(entry->view);
                        entry->view = NULL;
                    }
                    if (entry->database != NULL) {
                        egtb_close(entry->database);
                        entry->database = NULL;
                    }
                    if (entry->indexer_initialized) {
                        eg_indexer_destroy(&entry->indexer);
                        entry->indexer_initialized = false;
                    }
                }
}

static bool open_catalog_database(DatabaseCatalog *catalog,
                                  CatalogEntry *entry)
{
    char path[128];
    if (entry->database != NULL)
        return true;
    if (!egtb_material_filename(
            path, sizeof(path), entry->canonical.white_kings,
            entry->canonical.white_men, entry->canonical.black_kings,
            entry->canonical.black_men, "dtm") ||
        !eg_indexer_init(&entry->indexer, entry->canonical.white_men,
                         entry->canonical.black_men,
                         entry->canonical.white_kings,
                         entry->canonical.black_kings)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot initialize dependency material");
        return false;
    }
    entry->indexer_initialized = true;
    if (!egtb_open_readonly(&entry->database, path, 1)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot open dependency %.100s: %.100s", path,
                 egtb_last_error());
        return false;
    }
    if (!egtb_view_create(&entry->view, entry->database,
                          catalog->cache_pages, false)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot create dependency view for %.100s: %.100s", path,
                 egtb_last_error());
        return false;
    }
    if (egtb_maximum_index(entry->database) != eg_max_index(&entry->indexer)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "dependency %s has the wrong maximum index", path);
        return false;
    }
    return true;
}

static bool catalog_probe(const DraughtsPosition *position, EgtbSide side,
                          void *opaque, int16_t *value)
{
    DatabaseCatalog *catalog = opaque;
    EgtbMaterial requested = egtb_position_material(position);
    EgtbMaterial canonical;
    EgtbMaterialKind kind = egtb_material_resolve(&requested, &canonical);
    CatalogEntry *entry;
    DraughtsPosition transformed;
    EgPosition indexed;
    uint64_t index;
    if (kind == EGTB_MATERIAL_TERMINAL) {
        uint64_t pieces = side == EGTB_WHITE_TO_MOVE
                              ? position->white_men | position->white_kings
                              : position->black_men | position->black_kings;
        if (pieces == 0) {
            *value = 0;
            return true;
        }
        snprintf(catalog->error, sizeof(catalog->error),
                 "unsupported terminal position with pieces for side to move");
        return false;
    }
    if (kind == EGTB_MATERIAL_INVALID) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "successor material is invalid");
        return false;
    }
    entry = catalog_entry(catalog, &canonical);
    if (!open_catalog_database(catalog, entry))
        return false;
    if (kind == EGTB_MATERIAL_MIRROR) {
        egtb_mirror_position(position, &transformed);
        side = egtb_mirror_side(side);
    } else {
        transformed = *position;
    }
    indexed.white_men = transformed.white_men;
    indexed.black_men = transformed.black_men;
    indexed.white_kings = transformed.white_kings;
    indexed.black_kings = transformed.black_kings;
    if (!eg_position_to_index(&entry->indexer, &indexed, &index) ||
        !egtb_view_get(entry->view, index, side, value)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot query dependency: %.200s", egtb_last_error());
        return false;
    }
    return true;
}

static void report_correction(uint64_t index, EgtbSide side,
                              const DraughtsPosition *position,
                              int16_t old_value, int16_t new_value,
                              void *context)
{
    (void)context;
    if ((old_value > 0 && new_value > 0 && new_value < old_value) ||
        (old_value != EGTB_DRAW && old_value <= 0 &&
         new_value < old_value))
        printf("self-consistency correction: index=%" PRIu64 " %s "
               "WM=%013" PRIx64 " BM=%013" PRIx64
               " WK=%013" PRIx64 " BK=%013" PRIx64
               " was=%d now=%d\n",
               index, side == EGTB_WHITE_TO_MOVE ? "WTM" : "BTM",
               position->white_men, position->black_men,
               position->white_kings, position->black_kings,
               old_value, new_value);
}

static bool parse_count(const char *text, unsigned *count)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value > EGTB_MAX_PIECES)
        return false;
    *count = (unsigned)value;
    return true;
}

static bool parse_thread_count(const char *text, unsigned *count)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value == 0 ||
        value > EGTB_MAX_THREADS)
        return false;
    *count = (unsigned)value;
    return true;
}

int main(int argc, char **argv)
{
    EgtbMaterial requested, material;
    EgtbMaterialKind kind;
    char path[128];
    EgtbCreateOptions options = {GENERATION_CACHE_PAGES, 20, 9};
    EgtbGenerationStatistics generation;
    EgtbStorageStatistics storage;
    DatabaseCatalog *catalogs = NULL;
    void **probe_contexts = NULL;
    EgIndexer indexer;
    Egtb *database = NULL;
    uint64_t *histogram = NULL;
    uint64_t positions;
    bool indexer_initialized = false;
    bool created = false;
    bool ok = false;
    unsigned thread_count = 1;
    int material_argument = 1;
    if (argc == 7 && strcmp(argv[1], "-j") == 0) {
        if (!parse_thread_count(argv[2], &thread_count)) {
            fprintf(stderr, "thread count must be 1..%u\n",
                    EGTB_MAX_THREADS);
            return EXIT_FAILURE;
        }
        material_argument = 3;
    }
    if (argc != material_argument + 4 ||
        !parse_count(argv[material_argument], &requested.white_kings) ||
        !parse_count(argv[material_argument + 1], &requested.white_men) ||
        !parse_count(argv[material_argument + 2], &requested.black_kings) ||
        !parse_count(argv[material_argument + 3], &requested.black_men)) {
        fprintf(stderr, "usage: %s [-j THREADS] NWHITE_KINGS NWHITE_MEN "
                        "NBLACK_KINGS NBLACK_MEN\n", argv[0]);
        return EXIT_FAILURE;
    }
    kind = egtb_material_resolve(&requested, &material);
    if (kind == EGTB_MATERIAL_INVALID || kind == EGTB_MATERIAL_TERMINAL) {
        fprintf(stderr, "material must contain 2..7 pieces and at least one "
                        "piece for each side\n");
        return EXIT_FAILURE;
    }
    if (kind == EGTB_MATERIAL_MIRROR)
        printf("requested material is mirrored; generating canonical "
               "%u %u %u %u\n", material.white_kings,
               material.white_men, material.black_kings,
               material.black_men);
    if (!egtb_material_filename(path, sizeof(path), material.white_kings,
                                material.white_men, material.black_kings,
                                material.black_men, "dtm") ||
        !eg_indexer_init(&indexer, material.white_men, material.black_men,
                         material.white_kings, material.black_kings)) {
        fprintf(stderr, "cannot initialize requested material\n");
        return EXIT_FAILURE;
    }
    indexer_initialized = true;
    positions = eg_position_count(&indexer);
    catalogs = calloc(thread_count, sizeof(*catalogs));
    probe_contexts = calloc(thread_count, sizeof(*probe_contexts));
    if (catalogs == NULL || probe_contexts == NULL) {
        fprintf(stderr, "cannot allocate EGTB catalog\n");
        goto done;
    }
    for (unsigned worker = 0; worker < thread_count; ++worker) {
        initialize_catalog(&catalogs[worker], READONLY_CACHE_PAGES);
        probe_contexts[worker] = &catalogs[worker];
    }
    if (!egtb_create(&database, path, positions - 1,
                     GENERATION_PAGE_SIZE, &options)) {
        fprintf(stderr, "cannot create %s: %s\n", path, egtb_last_error());
        goto done;
    }
    created = true;
    printf("generating %s with %u thread%s, %zu writable-cache pages total\n",
           path, thread_count, thread_count == 1 ? "" : "s",
           (size_t)GENERATION_CACHE_PAGES);
    fflush(stdout);
    {
        EgtbThreadOptions thread_options = {
            thread_count, GENERATION_CACHE_PAGES, probe_contexts
        };
        if (!egtb_generate_threaded(database, &indexer, catalog_probe,
                                    &catalogs[0], report_correction, NULL,
                                    &thread_options, &generation)) {
            const char *catalog_error = "";
            for (unsigned worker = 0; worker < thread_count; ++worker) {
                if (catalogs[worker].error[0] != '\0') {
                    catalog_error = catalogs[worker].error;
                    break;
                }
            }
        fprintf(stderr, "cannot generate %s: %s%s%s\n", path,
                    egtb_generator_last_error(), *catalog_error ? ": " : "",
                    catalog_error);
            goto done;
        }
    }
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
    for (unsigned worker = 0; worker < thread_count; ++worker)
        close_catalog(&catalogs[worker]);
    if (!egtb_compact(path, 9, READONLY_CACHE_PAGES) ||
        !egtb_open_readonly(&database, path, READONLY_CACHE_PAGES) ||
        !egtb_storage_statistics(database, &storage)) {
        fprintf(stderr, "cannot compact/reopen %s: %s\n", path,
                egtb_last_error());
        goto done;
    }
    histogram = calloc((size_t)2 * (UINT16_MAX + 1u), sizeof(*histogram));
    if (histogram == NULL)
        goto done;
    for (uint64_t index = 0; index < positions; ++index)
        for (unsigned side = 0; side < 2; ++side) {
            int16_t value;
            if (!egtb_get(database, index, (EgtbSide)side, &value))
                goto done;
            ++histogram[(size_t)side * (UINT16_MAX + 1u) + (uint16_t)value];
        }
    printf("generated %s: material=%u %u %u %u positions=%" PRIu64
           " maximum-index=%" PRIu64 " passes=%" PRIu64
           " maximum-dtm=%u threads=%u\n", path, material.white_kings,
           material.white_men, material.black_kings, material.black_men,
           positions, positions - 1, generation.retrograde_passes,
           generation.maximum_dtm, thread_count);
    printf("self-consistency: passes=%" PRIu64 " updates=%" PRIu64
           "/%" PRIu64 "\n", generation.consistency_passes,
           generation.consistency_updates[0], generation.consistency_updates[1]);
    for (unsigned side = 0; side < 2; ++side) {
        uint64_t wins = 0, losses = 0, draws = 0;
        for (int numeric = INT16_MIN; numeric <= INT16_MAX; ++numeric) {
            int16_t value = (int16_t)numeric;
            uint64_t count = histogram[(size_t)side * (UINT16_MAX + 1u) +
                                       (uint16_t)value];
            if (value == EGTB_DRAW)
                draws += count;
            else if (value > 0)
                wins += count;
            else
                losses += count;
        }
        printf("%s: wins=%" PRIu64 " losses=%" PRIu64 " draws=%" PRIu64
               "\n", side == EGTB_WHITE_TO_MOVE ? "WTM" : "BTM",
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
    printf("storage: raw=%" PRIu64 " payload=%" PRIu64 " file=%" PRIu64
           " bytes overall=%.2f%% (%.2f:1)\n",
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
    if (catalogs != NULL) {
        for (unsigned worker = 0; worker < thread_count; ++worker)
            close_catalog(&catalogs[worker]);
        free(catalogs);
    }
    free(probe_contexts);
    if (indexer_initialized)
        eg_indexer_destroy(&indexer);
    free(histogram);
    if (!ok && created)
        unlink(path);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
