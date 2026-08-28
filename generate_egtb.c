#include "egtb.h"
#include "endgame_index.h"
#include "generator.h"
#include "material.h"
#include "revision.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GIBIBYTE (UINT64_C(1024) * 1024 * 1024)
#define DEFAULT_RESIDENT_LIMIT_BYTES (UINT64_C(32) * GIBIBYTE)
#define DEFAULT_VERIFICATION_CACHE_BYTES (UINT64_C(32) * GIBIBYTE)

enum {
    GENERATION_PAGE_SIZE = 1024,
    GENERATION_CACHE_BYTES = 1024 * 1024 * 1024,
    READONLY_CACHE_BYTES = 16 * 1024 * 1024,
    DEPENDENCY_CACHE_BYTES = 64 * 1024 * 1024,
    GENERATION_CACHE_PAGES = GENERATION_CACHE_BYTES / GENERATION_PAGE_SIZE,
    READONLY_CACHE_PAGES = READONLY_CACHE_BYTES / GENERATION_PAGE_SIZE,
    DEPENDENCY_CACHE_PAGES =
        DEPENDENCY_CACHE_BYTES / GENERATION_PAGE_SIZE
};

static double wall_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

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

static void add_cache_statistics(EgtbCacheStatistics *total,
                                 const EgtbCacheStatistics *part)
{
    total->lookups += part->lookups;
    total->hits += part->hits;
    total->misses += part->misses;
    total->decompressions += part->decompressions;
    total->dirty_evictions += part->dirty_evictions;
    total->compressed_writes += part->compressed_writes;
}

static void catalog_cache_statistics(const DatabaseCatalog *catalogs,
                                     unsigned catalog_count,
                                     EgtbCacheStatistics *statistics)
{
    unsigned catalog_index, wk, wm, bk, bm;
    memset(statistics, 0, sizeof(*statistics));
    for (catalog_index = 0; catalog_index < catalog_count; ++catalog_index)
        for (wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
            for (wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
                for (bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                    for (bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                        const CatalogEntry *entry =
                            &catalogs[catalog_index].entry[wk][wm][bk][bm];
                        EgtbCacheStatistics part;
                        if (entry->view == NULL)
                            continue;
                        egtb_view_cache_statistics(entry->view, &part);
                        add_cache_statistics(statistics, &part);
                    }
}

static void print_cache_statistics(const char *label,
                                   const EgtbCacheStatistics *statistics)
{
    double hit_rate = statistics->lookups == 0
                          ? 0.0
                          : 100.0 * (double)statistics->hits /
                                (double)statistics->lookups;
    printf("%s:\n", label);
    printf("  %-22s %20s\n", "Metric", "Value");
    printf("  %-22s %20" PRIu64 "\n", "Lookups", statistics->lookups);
    printf("  %-22s %20" PRIu64 "\n", "Hits", statistics->hits);
    printf("  %-22s %20" PRIu64 "\n", "Misses", statistics->misses);
    printf("  %-22s %19.2f%%\n", "Hit rate", hit_rate);
    printf("  %-22s %20" PRIu64 "\n", "Decompressions",
           statistics->decompressions);
    printf("  %-22s %20" PRIu64 "\n", "Dirty evictions",
           statistics->dirty_evictions);
    printf("  %-22s %20" PRIu64 "\n", "Compressed writes",
           statistics->compressed_writes);
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

static bool configuration_bytes(const char *name, uint64_t default_bytes,
                                bool allow_zero, uint64_t *bytes)
{
    const char *text = getenv(name);
    char *end;
    unsigned long long gibibytes;
    if (text == NULL || *text == '\0') {
        *bytes = default_bytes;
        return true;
    }
    errno = 0;
    gibibytes = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || (!allow_zero && gibibytes == 0) ||
        gibibytes > UINT64_MAX / GIBIBYTE)
        return false;
    *bytes = (uint64_t)gibibytes * GIBIBYTE;
    return true;
}

int main(int argc, char **argv)
{
    EgtbMaterial requested, material;
    EgtbMaterialKind kind;
    char path[128];
    EgtbCreateOptions options = {GENERATION_CACHE_PAGES, 20, 9};
    EgtbGenerationStatistics generation;
    EgtbConsistencyStatistics final_verification = {0};
    EgtbStorageStatistics storage;
    EgtbCacheStatistics generation_dependencies = {0};
    EgtbCacheStatistics verification_dependencies = {0};
    Bitmap verified_positions = {0};
    DatabaseCatalog *catalogs = NULL;
    void **probe_contexts = NULL;
    EgIndexer indexer;
    Egtb *database = NULL;
    EgtbResident *resident = NULL;
    uint64_t *histogram = NULL;
    uint64_t positions, resident_limit_bytes, verification_cache_bytes;
    size_t verification_cache_pages;
    bool indexer_initialized = false;
    bool created = false;
    bool ok = false;
    unsigned thread_count = 1;
    int material_argument = 1;
    double program_started = wall_seconds();
    double generation_started, phase_started;
    double setup_seconds, generation_seconds, finalize_seconds;
    double compact_seconds, resident_load_seconds = 0.0;
    double verification_seconds, statistics_seconds;
    double total_seconds;
    if (argc == 2 && strcmp(argv[1], "--revision") == 0) {
        printf("GWDEGTB revision %s\n", gwdegtb_revision);
        return EXIT_SUCCESS;
    }
    printf("GWDEGTB revision %s\n", gwdegtb_revision);
    if (!configuration_bytes("EGTB_RESIDENT_LIMIT_GIB",
                             DEFAULT_RESIDENT_LIMIT_BYTES, true,
                             &resident_limit_bytes) ||
        !configuration_bytes("EGTB_VERIFICATION_CACHE_GIB",
                             DEFAULT_VERIFICATION_CACHE_BYTES, false,
                             &verification_cache_bytes) ||
        verification_cache_bytes / GENERATION_PAGE_SIZE > SIZE_MAX) {
        fprintf(stderr, "invalid resident/cache GiB configuration\n");
        return EXIT_FAILURE;
    }
    verification_cache_pages =
        (size_t)(verification_cache_bytes / GENERATION_PAGE_SIZE);
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
        initialize_catalog(&catalogs[worker], DEPENDENCY_CACHE_PAGES);
        probe_contexts[worker] = &catalogs[worker];
    }
    if (!egtb_create(&database, path, positions - 1,
                     GENERATION_PAGE_SIZE, &options)) {
        fprintf(stderr, "cannot create %s: %s\n", path, egtb_last_error());
        goto done;
    }
    created = true;
    printf("generating %s with %u thread%s, %u MiB writable cache total, "
           "%u MiB dependency cache per worker/database\n",
           path, thread_count, thread_count == 1 ? "" : "s",
           GENERATION_CACHE_BYTES / (1024 * 1024),
           DEPENDENCY_CACHE_BYTES / (1024 * 1024));
    fflush(stdout);
    generation_started = wall_seconds();
    setup_seconds = generation_started - program_started;
    {
        EgtbThreadOptions thread_options = {
            thread_count, GENERATION_CACHE_PAGES, probe_contexts,
            &verified_positions
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
    generation_seconds = wall_seconds() - generation_started;
    catalog_cache_statistics(catalogs, thread_count,
                             &generation_dependencies);
    phase_started = wall_seconds();
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
    finalize_seconds = wall_seconds() - phase_started;
    phase_started = wall_seconds();
    if (!egtb_compact(path, 9, READONLY_CACHE_PAGES) ||
        !egtb_open_readonly(&database, path, READONLY_CACHE_PAGES)) {
        fprintf(stderr, "cannot compact/reopen %s: %s\n", path,
                egtb_last_error());
        goto done;
    }
    compact_seconds = wall_seconds() - phase_started;
    phase_started = wall_seconds();
    {
        uint64_t resident_bytes =
            egtb_page_count(database) * (uint64_t)egtb_page_size(database);
        if (resident_limit_bytes != 0 &&
            resident_bytes <= resident_limit_bytes) {
            if (!egtb_resident_load(&resident, database, thread_count)) {
                fprintf(stderr, "cannot load resident %s: %s\n", path,
                        egtb_last_error());
                goto done;
            }
            resident_load_seconds = wall_seconds() - phase_started;
        }
    }
    phase_started = wall_seconds();
    for (unsigned worker = 0; worker < thread_count; ++worker) {
        initialize_catalog(&catalogs[worker], DEPENDENCY_CACHE_PAGES);
        probe_contexts[worker] = &catalogs[worker];
    }
    {
        EgtbVerificationOptions verify_options = {
            thread_count, verification_cache_pages, probe_contexts, resident
        };
        if (!egtb_verify_consistent_threaded(
                database, &indexer, catalog_probe, &catalogs[0],
                &verify_options,
                verified_positions.words != NULL ? &verified_positions : NULL,
                &final_verification)) {
            const char *catalog_error = "";
            for (unsigned worker = 0; worker < thread_count; ++worker) {
                if (catalogs[worker].error[0] != '\0') {
                    catalog_error = catalogs[worker].error;
                    break;
                }
            }
            fprintf(stderr, "fatal: compacted database %s failed final "
                            "consistency verification: %s%s%s\n",
                    path, egtb_generator_last_error(),
                    *catalog_error ? ": " : "", catalog_error);
            goto done;
        }
    }
    verification_seconds = wall_seconds() - phase_started;
    catalog_cache_statistics(catalogs, thread_count,
                             &verification_dependencies);
    phase_started = wall_seconds();
    if (!egtb_storage_statistics(database, &storage)) {
        fprintf(stderr, "cannot read storage statistics for %s: %s\n",
                path, egtb_last_error());
        goto done;
    }
    histogram = calloc((size_t)2 * (UINT16_MAX + 1u), sizeof(*histogram));
    if (histogram == NULL)
        goto done;
    if (resident != NULL) {
        if (!egtb_resident_dtm_histogram(resident, histogram,
                                         UINT16_MAX + 1u))
            goto done;
    } else {
        for (uint64_t index = 0; index < positions; ++index)
            for (unsigned side = 0; side < 2; ++side) {
                int16_t value;
                if (!egtb_get(database, index, (EgtbSide)side, &value))
                    goto done;
                ++histogram[(size_t)side * (UINT16_MAX + 1u) +
                            (uint16_t)value];
            }
    }
    statistics_seconds = wall_seconds() - phase_started;
    printf("generated %s: material=%u %u %u %u positions=%" PRIu64
           " maximum-index=%" PRIu64 " passes=%" PRIu64
           " maximum-dtm=%u threads=%u\n", path, material.white_kings,
           material.white_men, material.black_kings, material.black_men,
           positions, positions - 1, generation.retrograde_passes,
           generation.maximum_dtm, thread_count);
    printf("self-consistency: passes=%" PRIu64 " updates=%" PRIu64
           "/%" PRIu64 "\n", generation.consistency_passes,
           generation.consistency_updates[0], generation.consistency_updates[1]);
    if (resident != NULL)
        printf("final read-only consistency verification: threads=%u "
               "resident=%" PRIu64 " MiB positions-checked=%" PRIu64
               " positions-skipped=%" PRIu64 "\n",
               thread_count, egtb_resident_bytes(resident) / (1024 * 1024),
               final_verification.positions_checked,
               final_verification.positions_skipped);
    else
        printf("final read-only consistency verification: threads=%u "
               "cache=%" PRIu64 " MiB total positions-checked=%" PRIu64
               " positions-skipped=%" PRIu64 "\n",
               thread_count, verification_cache_bytes / (1024 * 1024),
               final_verification.positions_checked,
               final_verification.positions_skipped);
    print_cache_statistics("consistency current-DB cache",
                           &generation.consistency_cache);
    print_cache_statistics("generator dependency caches",
                           &generation_dependencies);
    print_cache_statistics("final verification current-DB cache",
                           &final_verification.cache);
    print_cache_statistics("final verification dependency caches",
                           &verification_dependencies);
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
        printf("%s DTM statistics:\n",
               side == EGTB_WHITE_TO_MOVE ? "WTM" : "BTM");
        printf("%8s %20s\n", "DTM", "Frequency");
        for (int numeric = INT16_MIN; numeric <= INT16_MAX; ++numeric) {
            int16_t value = (int16_t)numeric;
            uint64_t count = histogram[(size_t)side * (UINT16_MAX + 1u) +
                                       (uint16_t)value];
            if (count != 0)
                printf("%8d %20" PRIu64 "\n", value, count);
        }
    }
    printf("storage: raw=%" PRIu64 " payload=%" PRIu64 " file=%" PRIu64
           " bytes overall=%.2f%% (%.2f:1)\n",
           storage.logical_uncompressed_bytes,
           storage.compressed_payload_bytes, storage.file_bytes,
           100.0 * (double)storage.file_bytes /
               (double)storage.logical_uncompressed_bytes,
           (double)storage.logical_uncompressed_bytes /
               (double)storage.file_bytes);
    total_seconds = wall_seconds() - program_started;
    printf("wall-clock timings:\n");
    printf("  %-28s %10.3f s\n", "setup/create", setup_seconds);
    printf("  %-28s %10.3f s\n", "initialization",
           generation.initialization_seconds);
    printf("  %-28s %10.3f s\n", "backpropagation",
           generation.backpropagation_seconds);
    printf("  %-28s %10.3f s\n", "frontier compilation",
           generation.compilation_seconds);
    printf("  %-28s %10.3f s\n", "consistency repair",
           generation.consistency_seconds);
    printf("  %-28s %10.3f s\n", "final DTM scan",
           generation.final_scan_seconds);
    printf("  %-28s %10.3f s\n", "generator total", generation_seconds);
    printf("  %-28s %10.3f s\n", "finalize/close", finalize_seconds);
    printf("  %-28s %10.3f s\n", "compact/reopen", compact_seconds);
    printf("  %-28s %10.3f s\n", "resident load", resident_load_seconds);
    printf("  %-28s %10.3f s\n", "final verification",
           verification_seconds);
    printf("  %-28s %10.3f s\n",
           resident != NULL ? "statistics extraction" : "statistics scan",
           statistics_seconds);
    printf("  %-28s %10.3f s\n", "total", total_seconds);
    ok = true;
done:
    egtb_resident_destroy(resident);
    if (database != NULL && !egtb_close(database))
        ok = false;
    if (catalogs != NULL) {
        for (unsigned worker = 0; worker < thread_count; ++worker)
            close_catalog(&catalogs[worker]);
        free(catalogs);
    }
    free(probe_contexts);
    bitmap_destroy(&verified_positions);
    if (indexer_initialized)
        eg_indexer_destroy(&indexer);
    free(histogram);
    if (!ok && created)
        unlink(path);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
