#define _POSIX_C_SOURCE 200809L

#include "egtb.h"
#include "endgame_index.h"
#include "generator.h"
#include "material.h"
#include "progress.h"
#include "revision.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GIBIBYTE (UINT64_C(1024) * 1024 * 1024)
#define DEFAULT_RESIDENT_LIMIT_BYTES (UINT64_C(32) * GIBIBYTE)
#define DEFAULT_VERIFICATION_CACHE_BYTES (UINT64_C(32) * GIBIBYTE)
#define DEPENDENCY_CACHE_BYTES (UINT64_C(64) * 1024 * 1024)

typedef struct {
    EgtbMaterialKind kind;
    EgtbMaterial canonical;
    Egtb *database;
    EgtbView *view;
    EgIndexer indexer;
    bool indexer_initialized;
} CatalogEntry;

typedef struct {
    CatalogEntry entry[EGTB_MAX_PIECES + 1][EGTB_MAX_PIECES + 1]
                      [EGTB_MAX_PIECES + 1][EGTB_MAX_PIECES + 1];
    size_t cache_bytes;
    char error[256];
} DatabaseCatalog;

static CatalogEntry *catalog_entry(DatabaseCatalog *catalog,
                                   const EgtbMaterial *material)
{
    return &catalog->entry[material->white_kings][material->white_men]
                          [material->black_kings][material->black_men];
}

static void initialize_catalog(DatabaseCatalog *catalog, size_t cache_bytes)
{
    memset(catalog, 0, sizeof(*catalog));
    catalog->cache_bytes = cache_bytes;
    for (unsigned wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
        for (unsigned wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
            for (unsigned bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                for (unsigned bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                    EgtbMaterial material = {wk, wm, bk, bm};
                    CatalogEntry *entry = catalog_entry(catalog, &material);
                    entry->kind =
                        egtb_material_resolve(&material, &entry->canonical);
                }
}

static void close_catalog(DatabaseCatalog *catalog)
{
    for (unsigned wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
        for (unsigned wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
            for (unsigned bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                for (unsigned bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                    CatalogEntry *entry = &catalog->entry[wk][wm][bk][bm];
                    if (entry->view != NULL)
                        egtb_view_close(entry->view);
                    if (entry->database != NULL)
                        egtb_close(entry->database);
                    if (entry->indexer_initialized)
                        eg_indexer_destroy(&entry->indexer);
                }
}

static bool open_catalog_database(DatabaseCatalog *catalog,
                                  CatalogEntry *entry)
{
    char path[128];
    size_t pages;
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
    pages = catalog->cache_bytes /
            egtb_cache_page_size(entry->database);
    if (!egtb_view_create(&entry->view, entry->database,
                          pages == 0 ? 1 : pages, false)) {
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
                 "unsupported terminal successor");
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
                                     unsigned count,
                                     EgtbCacheStatistics *statistics)
{
    memset(statistics, 0, sizeof(*statistics));
    for (unsigned catalog = 0; catalog < count; ++catalog)
        for (unsigned wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
            for (unsigned wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
                for (unsigned bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                    for (unsigned bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                        const CatalogEntry *entry =
                            &catalogs[catalog].entry[wk][wm][bk][bm];
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
}

static bool parse_unsigned(const char *text, unsigned maximum,
                           unsigned *value)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed > maximum)
        return false;
    *value = (unsigned)parsed;
    return true;
}

static bool configured_gibibytes(const char *name, uint64_t default_value,
                                 bool allow_zero, uint64_t *bytes)
{
    const char *text = getenv(name);
    char *end;
    unsigned long long value;
    if (text == NULL || *text == '\0') {
        *bytes = default_value;
        return true;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        (!allow_zero && value == 0) || value > UINT64_MAX / GIBIBYTE)
        return false;
    *bytes = (uint64_t)value * GIBIBYTE;
    return true;
}

int main(int argc, char **argv)
{
    const char *directory = ".";
    int argument = 1;
    unsigned thread_count = 4, progress_seconds = 60;
    EgtbMaterial requested, material;
    EgtbMaterialKind kind;
    EgIndexer indexer;
    bool indexer_initialized = false;
    Egtb *database = NULL;
    EgtbResident *resident = NULL;
    DatabaseCatalog *catalogs = NULL;
    void **contexts = NULL;
    EgtbConsistencyStatistics verification = {0};
    EgtbCacheStatistics dependency_statistics = {0};
    uint64_t resident_limit, verification_cache_bytes, resident_bytes;
    size_t verification_cache_pages;
    char filename[128];
    bool ok = false;

    if (argc == 2 && strcmp(argv[1], "--revision") == 0) {
        printf("GWDEGTB revision %s\n", gwdegtb_revision);
        return EXIT_SUCCESS;
    }
    {
        const char *setting = getenv("EGTB_THREADS");
        if (setting != NULL && *setting != '\0' &&
            (!parse_unsigned(setting, EGTB_MAX_THREADS, &thread_count) ||
             thread_count == 0)) {
            fprintf(stderr, "invalid EGTB_THREADS\n");
            return EXIT_FAILURE;
        }
        setting = getenv("EGTB_PROGRESS_SECONDS");
        if (setting != NULL && *setting != '\0' &&
            !parse_unsigned(setting, 3600, &progress_seconds)) {
            fprintf(stderr, "invalid EGTB_PROGRESS_SECONDS\n");
            return EXIT_FAILURE;
        }
    }
    while (argument < argc) {
        if (strcmp(argv[argument], "-d") == 0 && argument + 1 < argc) {
            directory = argv[argument + 1];
            argument += 2;
        } else if (strcmp(argv[argument], "-j") == 0 &&
                   argument + 1 < argc) {
            if (!parse_unsigned(argv[argument + 1], EGTB_MAX_THREADS,
                                &thread_count) || thread_count == 0) {
                fprintf(stderr, "thread count must be 1..%u\n",
                        EGTB_MAX_THREADS);
                return EXIT_FAILURE;
            }
            argument += 2;
        } else {
            break;
        }
    }
    if (argc != argument + 4 ||
        !parse_unsigned(argv[argument], EGTB_MAX_PIECES,
                        &requested.white_kings) ||
        !parse_unsigned(argv[argument + 1], EGTB_MAX_PIECES,
                        &requested.white_men) ||
        !parse_unsigned(argv[argument + 2], EGTB_MAX_PIECES,
                        &requested.black_kings) ||
        !parse_unsigned(argv[argument + 3], EGTB_MAX_PIECES,
                        &requested.black_men)) {
        fprintf(stderr, "usage: %s [-d DIRECTORY] [-j THREADS] "
                        "NWHITE_KINGS NWHITE_MEN NBLACK_KINGS NBLACK_MEN\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    kind = egtb_material_resolve(&requested, &material);
    if (kind != EGTB_MATERIAL_CANONICAL && kind != EGTB_MATERIAL_MIRROR) {
        fprintf(stderr, "material must contain 2..%u pieces and at least "
                        "one piece for each side\n", EGTB_MAX_PIECES);
        return EXIT_FAILURE;
    }
    if (!configured_gibibytes("EGTB_RESIDENT_LIMIT_GIB",
                              DEFAULT_RESIDENT_LIMIT_BYTES, true,
                              &resident_limit) ||
        !configured_gibibytes("EGTB_VERIFICATION_CACHE_GIB",
                              DEFAULT_VERIFICATION_CACHE_BYTES, false,
                              &verification_cache_bytes)) {
        fprintf(stderr, "invalid verification memory configuration\n");
        return EXIT_FAILURE;
    }
    if (chdir(directory) != 0) {
        fprintf(stderr, "cannot enter %s: %s\n", directory, strerror(errno));
        return EXIT_FAILURE;
    }
    if (!egtb_material_filename(filename, sizeof(filename),
                                material.white_kings, material.white_men,
                                material.black_kings, material.black_men,
                                "dtm") ||
        !eg_indexer_init(&indexer, material.white_men, material.black_men,
                         material.white_kings, material.black_kings)) {
        fprintf(stderr, "cannot initialize requested material\n");
        goto done;
    }
    indexer_initialized = true;
    if (kind == EGTB_MATERIAL_MIRROR)
        printf("requested material is mirrored; verifying canonical %s\n",
               filename);
    if (!egtb_open_readonly(&database, filename, 1)) {
        fprintf(stderr, "cannot open %s: %s\n", filename, egtb_last_error());
        goto done;
    }
    if (egtb_maximum_index(database) != eg_max_index(&indexer)) {
        fprintf(stderr, "%s maximum index does not match its material\n",
                filename);
        goto done;
    }
    if (!egtb_progress_start(progress_seconds)) {
        fprintf(stderr, "cannot start progress reporter\n");
        goto done;
    }
    egtb_progress_log("GWDEGTB revision %s verifying %s with %u threads\n",
                      gwdegtb_revision, filename, thread_count);
    resident_bytes = (egtb_maximum_index(database) + 1) * sizeof(EgtbEntry);
    if (resident_limit != 0 && resident_bytes <= resident_limit &&
        !egtb_resident_load(&resident, database, thread_count)) {
        fprintf(stderr, "cannot checksum/decompress %s: %s\n", filename,
                egtb_last_error());
        goto done;
    }
    verification_cache_pages =
        (size_t)(verification_cache_bytes / egtb_cache_page_size(database));
    if (verification_cache_pages == 0)
        verification_cache_pages = 1;
    catalogs = calloc(thread_count, sizeof(*catalogs));
    contexts = calloc(thread_count, sizeof(*contexts));
    if (catalogs == NULL || contexts == NULL) {
        fprintf(stderr, "cannot allocate verification catalogs\n");
        goto done;
    }
    for (unsigned worker = 0; worker < thread_count; ++worker) {
        initialize_catalog(&catalogs[worker], DEPENDENCY_CACHE_BYTES);
        contexts[worker] = &catalogs[worker];
    }
    {
        EgtbVerificationOptions options = {
            thread_count, verification_cache_pages, contexts, resident
        };
        if (!egtb_verify_consistent_threaded(
                database, &indexer, catalog_probe, &catalogs[0], &options,
                NULL, &verification)) {
            const char *dependency_error = "";
            for (unsigned worker = 0; worker < thread_count; ++worker)
                if (catalogs[worker].error[0] != '\0') {
                    dependency_error = catalogs[worker].error;
                    break;
                }
            fprintf(stderr, "VERIFICATION FAILED for %s: %s%s%s\n",
                    filename, egtb_generator_last_error(),
                    *dependency_error ? ": " : "", dependency_error);
            goto done;
        }
    }
    catalog_cache_statistics(catalogs, thread_count,
                             &dependency_statistics);
    printf("VERIFIED %s: positions=%" PRIu64
           " DTM-values-checked=%" PRIu64 " threads=%u mode=%s\n",
           filename, eg_position_count(&indexer),
           verification.positions_checked, thread_count,
           resident != NULL ? "resident" : "disk-cache");
    print_cache_statistics("current-DTM cache", &verification.cache);
    print_cache_statistics("dependency-DTM caches", &dependency_statistics);
    ok = true;

done:
    egtb_progress_stop();
    if (catalogs != NULL) {
        for (unsigned worker = 0; worker < thread_count; ++worker)
            close_catalog(&catalogs[worker]);
    }
    free(contexts);
    free(catalogs);
    egtb_resident_destroy(resident);
    if (database != NULL && !egtb_close(database))
        ok = false;
    if (indexer_initialized)
        eg_indexer_destroy(&indexer);
    egtb_progress_log("GWDEGTB revision %s verification %s\n",
                      gwdegtb_revision, ok ? "completed" : "failed");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
