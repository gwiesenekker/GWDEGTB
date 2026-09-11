#define _POSIX_C_SOURCE 200809L
#include "egtb.h"
#include "dtm_fen.h"
#include "progress.h"
#include "endgame_index.h"
#include "generator.h"
#include "material.h"
#include "revision.h"
#include "sliced.h"
#include "dependency_resident.h"

#include <errno.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

#define GIBIBYTE (UINT64_C(1024) * 1024 * 1024)
#define DEFAULT_RESIDENT_LIMIT_BYTES (UINT64_C(32) * GIBIBYTE)
#define DEFAULT_VERIFICATION_CACHE_BYTES (UINT64_C(32) * GIBIBYTE)

static bool prepare_restart(const char *path, const char *work_path, bool restart)
{
    struct stat st;
    if (lstat(work_path, &st) == 0) {
        if (!restart) {
            fprintf(stderr, "unfinished database exists: %s; use --restart to remove it and retry\n", work_path);
            return false;
        }
        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr, "refusing to remove non-regular unfinished database: %s\n", work_path);
            return false;
        }
        if (unlink(work_path) != 0) {
            fprintf(stderr, "cannot remove %s for restart: %s\n", work_path, strerror(errno));
            return false;
        }
        fprintf(stderr, "restart: removed unfinished database %s\n", work_path);
    } else if (errno != ENOENT) {
        fprintf(stderr, "cannot inspect %s: %s\n", work_path, strerror(errno));
        return false;
    }
    if (lstat(path, &st) == 0) {
        if (!restart) {
            fprintf(stderr, "database already exists: %s\n", path);
            return false;
        }
        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr, "refusing to remove non-regular database: %s\n", path);
            return false;
        }
        if (unlink(path) != 0) {
            fprintf(stderr, "cannot remove %s for restart: %s\n", path, strerror(errno));
            return false;
        }
        fprintf(stderr, "restart: removed previous finished database %s\n", path);
    } else if (errno != ENOENT) {
        fprintf(stderr, "cannot inspect %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

enum {
    DEFAULT_GENERATION_PAGE_SIZE = 2048,
    GENERATION_CACHE_BYTES = 1024 * 1024 * 1024,
    READONLY_CACHE_BYTES = 16 * 1024 * 1024,
    DEPENDENCY_CACHE_BYTES = 64 * 1024 * 1024
};

static double wall_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void print_dtm_example(const char *label, const EgIndexer *indexer,
                              const EgtbDtmExample *example, EgtbSide side)
{
    char fen[256];
    if (example->available &&
        egtb_format_dtm_fen(indexer, example->index, side, example->dtm,
                            fen, sizeof(fen)))
        printf("  %-20s %s\n", label, fen);
    else
        printf("  %-20s unavailable\n", label);
}

typedef struct {
    EgtbMaterialKind kind;
    EgtbMaterial canonical;
    Egtb *database;
    EgtbView *view;
    const EgtbResident *resident;
    uint64_t resident_lookups;
    EgIndexer indexer;
    bool indexer_initialized;
    EgtbCacheStatistics generation_statistics;
} CatalogEntry;

typedef struct {
    CatalogEntry entry[EGTB_MAX_PIECES + 1][EGTB_MAX_PIECES + 1]
                      [EGTB_MAX_PIECES + 1][EGTB_MAX_PIECES + 1];
    size_t cache_bytes;
    DependencyResidentPool *resident_pool;
    char error[256];
} DatabaseCatalog;

static CatalogEntry *catalog_entry(DatabaseCatalog *catalog,
                                   const EgtbMaterial *material)
{
    return &catalog->entry[material->white_kings][material->white_men]
                          [material->black_kings][material->black_men];
}

static void initialize_catalog(DatabaseCatalog *catalog, size_t cache_bytes,
                                DependencyResidentPool *pool)
{
    unsigned wk, wm, bk, bm;
    memset(catalog, 0, sizeof(*catalog));
    catalog->cache_bytes = cache_bytes;
    catalog->resident_pool = pool;
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

/* Resident reads count as hits, but never as page decompressions. Initial
 * checksum-verified resident loading is reported by the pool separately. */
static void entry_statistics(const CatalogEntry *e, EgtbCacheStatistics *s)
{
    egtb_view_cache_statistics(e->view, s);
    s->lookups += e->resident_lookups;
    s->hits += e->resident_lookups;
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
                        if (entry->database == NULL)
                            continue;
                        entry_statistics(entry, &part);
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

/* Private views remain warm; keep a generation snapshot for phase deltas. */
static void snapshot_dependency_statistics(DatabaseCatalog *catalogs, unsigned n)
{
    for (unsigned t = 0; t < n; ++t)
        for (unsigned wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
            for (unsigned wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
                for (unsigned bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                    for (unsigned bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                        CatalogEntry *e = &catalogs[t].entry[wk][wm][bk][bm];
                        if (e->database != NULL)
                            entry_statistics(e, &e->generation_statistics);
                    }
}

static void print_dependency_statistics(const DatabaseCatalog *catalogs,
                                         unsigned n, bool verification)
{
    printf("%s dependency caches by material (summed across workers):\n",
           verification ? "final verification" : "generator");
    printf("  %-27s %15s %15s %15s %8s %13s %s\n", "Database", "Lookups",
           "Misses", "Decompressions", "Hit %", "Full MiB", "Mode");
    for (unsigned wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
        for (unsigned wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
            for (unsigned bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                for (unsigned bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                    EgtbCacheStatistics sum = {0};
                    uint64_t positions = 0;
                    bool resident = false;
                    for (unsigned t = 0; t < n; ++t) {
                        const CatalogEntry *e = &catalogs[t].entry[wk][wm][bk][bm];
                        if (e->database == NULL) continue;
                        resident = e->resident != NULL;
                        EgtbCacheStatistics s = e->generation_statistics;
                        if (verification) {
                            entry_statistics(e, &s);
                            s.lookups -= e->generation_statistics.lookups;
                            s.hits -= e->generation_statistics.hits;
                            s.misses -= e->generation_statistics.misses;
                            s.decompressions -= e->generation_statistics.decompressions;
                            s.dirty_evictions -= e->generation_statistics.dirty_evictions;
                            s.compressed_writes -= e->generation_statistics.compressed_writes;
                        }
                        add_cache_statistics(&sum, &s);
                        positions = egtb_maximum_index(e->database) + 1;
                    }
                    if (sum.lookups == 0) continue;
                    char name[128];
                    if (!egtb_material_filename(name, sizeof(name), wk, wm, bk, bm, "dtm"))
                        continue;
                    printf("  %-27s %15" PRIu64 " %15" PRIu64 " %15" PRIu64
                           " %8.2f %13.2f %s\n", name, sum.lookups, sum.misses,
                           sum.decompressions, 100.0 * (double)sum.hits / sum.lookups,
                           (double)positions * sizeof(EgtbEntry) / (1024.0 * 1024.0),
                           resident ? "resident" : "cached");
                }
}

static bool open_catalog_database(DatabaseCatalog *catalog,
                                  CatalogEntry *entry)
{
    char path[128];
    if (entry->resident != NULL || entry->view != NULL)
        return true;
    if (entry->database != NULL) return false; /* Earlier open/load failed. */
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
    if (egtb_maximum_index(entry->database) != eg_max_index(&entry->indexer)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "dependency %s has the wrong maximum index", path);
        return false;
    }
    if (!dependency_resident_acquire(catalog->resident_pool, entry->database,
                                      &entry->resident)) {
        snprintf(catalog->error, sizeof(catalog->error), "dependency %.100s: %.100s",
                 path, dependency_resident_error());
        return false;
    }
    if (entry->resident != NULL) return true;
    size_t pages = catalog->cache_bytes /
                   egtb_cache_page_size(entry->database);
    if (!egtb_view_create(&entry->view, entry->database,
                          pages == 0 ? 1 : pages, false)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot create dependency view for %.100s: %.100s", path,
                 egtb_last_error());
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
        !(entry->resident ? egtb_resident_get(entry->resident, index, side, value)
                           : egtb_view_get(entry->view, index, side, value))) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot query dependency: %.200s", egtb_last_error());
        return false;
    }
    if (entry->resident) ++entry->resident_lookups;
    return true;
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

static bool configuration_page_size(uint32_t *page_size)
{
    const char *text = getenv("EGTB_PAGE_SIZE");
    char *end;
    unsigned long value;
    if (text == NULL || *text == '\0') {
        *page_size = DEFAULT_GENERATION_PAGE_SIZE;
        return true;
    }
    if (*text < '0' || *text > '9')
        return false;
    errno = 0;
    value = strtoul(text, &end, 10);
    /* At least 64 int16_t values per page for bitmap ownership; the maximum
     * bounds the worst-case escaped stream and its uint16_t Zstd length. */
    if (errno != 0 || *end != '\0' || value < 128 || value > 32768 ||
        (value & (value - 1)) != 0)
        return false;
    *page_size = (uint32_t)value;
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
    char path[128], work_path[160] = {0};
    EgtbCreateOptions options = {0, 0, 1};
    EgtbGenerationStatistics generation;
    EgtbConsistencyStatistics final_verification = {0};
    EgtbStorageStatistics storage;
    EgtbCacheStatistics generation_dependencies = {0};
    EgtbCacheStatistics verification_dependencies = {0};
    DatabaseCatalog *catalogs = NULL;
    DependencyResidentPool *dependency_pool = NULL;
    void **probe_contexts = NULL;
    EgIndexer indexer;
    Egtb *database = NULL;
    EgtbResident *resident = NULL;
    EgtbDtmExamples examples = {0};
    uint64_t resident_bytes_used = 0;
    uint64_t *histogram = NULL;
    uint64_t positions, resident_limit_bytes, verification_cache_bytes;
    uint64_t compilation_buffer_bytes;
    size_t verification_cache_pages, generation_cache_pages, readonly_cache_pages;
    uint32_t page_size;
    bool indexer_initialized = false;
    bool created = false;
    bool sliced = false;
    bool restart = false;
    bool ok = false;
    unsigned thread_count = 1;
    int material_argument = 1;
    double program_started = wall_seconds();
    double generation_started, phase_started;
    double setup_seconds, generation_seconds, finalize_seconds;
    double verification_seconds, statistics_seconds;
    double total_seconds;
    if (argc == 2 && strcmp(argv[1], "--revision") == 0) {
        printf("GWDEGTB revision %s\n", gwdegtb_revision);
        return EXIT_SUCCESS;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    egtb_progress_log("GWDEGTB revision %s starting\n", gwdegtb_revision);
    const char *compression_setting = getenv("EGTB_COMPRESSION_LEVEL");
    if (compression_setting != NULL && *compression_setting != '\0') {
        char *end;
        errno = 0;
        unsigned long level = strtoul(compression_setting, &end, 10);
        if (errno || *end != '\0' || *compression_setting < '0' ||
            *compression_setting > '9' || level < 1 ||
            level > (unsigned long)ZSTD_maxCLevel()) {
            fprintf(stderr, "invalid EGTB_COMPRESSION_LEVEL: expected 1..%d\n",
                    ZSTD_maxCLevel());
            return EXIT_FAILURE;
        }
        options.compression_level = (int)level;
    }
    if (!configuration_page_size(&page_size)) {
        fprintf(stderr, "invalid EGTB_PAGE_SIZE: expected a power of two from 128 to 32768 bytes\n");
        return EXIT_FAILURE;
    }
    generation_cache_pages = GENERATION_CACHE_BYTES / page_size;
    readonly_cache_pages = READONLY_CACHE_BYTES / page_size;
    /* Frontier compilation has its own assembly and compressed-batch buffers. */
    options.cache_pages = 1;
    if (!configuration_bytes("EGTB_RESIDENT_LIMIT_GIB",
                             DEFAULT_RESIDENT_LIMIT_BYTES, true,
                             &resident_limit_bytes) ||
        !configuration_bytes("EGTB_VERIFICATION_CACHE_GIB",
                             DEFAULT_VERIFICATION_CACHE_BYTES, false,
                             &verification_cache_bytes) ||
        !configuration_bytes("EGTB_COMPILATION_BUFFER_GIB",
                             GIBIBYTE, false, &compilation_buffer_bytes) ||
        compilation_buffer_bytes > SIZE_MAX ||
        verification_cache_bytes / page_size > SIZE_MAX) {
        fprintf(stderr, "invalid resident/cache/compilation GiB configuration\n");
        return EXIT_FAILURE;
    }
    verification_cache_pages =
        (size_t)(verification_cache_bytes / page_size);
    while (material_argument < argc) {
        if (strcmp(argv[material_argument], "--sliced") == 0) {
            sliced = true;
            ++material_argument;
        } else if (strcmp(argv[material_argument], "--restart") == 0) {
            restart = true;
            ++material_argument;
        } else if (strcmp(argv[material_argument], "-j") == 0 &&
                   material_argument + 1 < argc) {
            if (!parse_thread_count(argv[material_argument + 1],
                                    &thread_count)) {
                fprintf(stderr, "thread count must be 1..%u\n",
                        EGTB_MAX_THREADS);
                return EXIT_FAILURE;
            }
            material_argument += 2;
        } else {
            break;
        }
    }
    if (argc != material_argument + 4 ||
        !parse_count(argv[material_argument], &requested.white_kings) ||
        !parse_count(argv[material_argument + 1], &requested.white_men) ||
        !parse_count(argv[material_argument + 2], &requested.black_kings) ||
        !parse_count(argv[material_argument + 3], &requested.black_men)) {
        fprintf(stderr, "usage: %s [--restart] [--sliced] [-j THREADS] "
                        "NWHITE_KINGS NWHITE_MEN "
                        "NBLACK_KINGS NBLACK_MEN\n", argv[0]);
        return EXIT_FAILURE;
    }
    kind = egtb_material_resolve(&requested, &material);
    if (kind == EGTB_MATERIAL_INVALID || kind == EGTB_MATERIAL_TERMINAL) {
        fprintf(stderr, "material must contain 2..%u pieces and at least one "
                        "piece for each side\n", EGTB_MAX_PIECES);
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
    snprintf(work_path, sizeof(work_path), "%s.incomplete", path);
    if (!prepare_restart(path, work_path, restart)) goto done;
    positions = eg_position_count(&indexer);
    if (!dependency_resident_configure(&dependency_pool)) {
        fprintf(stderr, "%s\n", dependency_resident_error());
        goto done;
    }
    catalogs = calloc(thread_count, sizeof(*catalogs));
    probe_contexts = calloc(thread_count, sizeof(*probe_contexts));
    if (catalogs == NULL || probe_contexts == NULL) {
        fprintf(stderr, "cannot allocate EGTB catalog\n");
        goto done;
    }
    for (unsigned worker = 0; worker < thread_count; ++worker) {
        initialize_catalog(&catalogs[worker], DEPENDENCY_CACHE_BYTES, dependency_pool);
        probe_contexts[worker] = &catalogs[worker];
    }
    if (sliced && material.white_men == 0 && material.black_men == 0) {
        fprintf(stderr, "warning: ignoring --sliced because the material "
                        "contains no men\n");
        sliced = false;
    }
    printf("generating %s%s with %u thread%s, "
           "%u MiB dependency cache per worker/database\n",
           path, sliced ? " by man-row slices" : "", thread_count,
           thread_count == 1 ? "" : "s",
           DEPENDENCY_CACHE_BYTES / (1024 * 1024));
    printf("DTM pages: %u bytes (%u positions per side)\n",
           page_size, page_size / (unsigned)sizeof(int16_t));
    printf("DTM compression: Zstd level %d\n", options.compression_level);
    dependency_resident_report(dependency_pool);
    printf("frontier compilation: %" PRIu64 " MiB assembly buffer total\n",
           compilation_buffer_bytes / (1024 * 1024));
    fflush(stdout);
    unsigned progress_seconds = 60;
    const char *progress_setting = getenv("EGTB_PROGRESS_SECONDS");
    if (progress_setting != NULL && *progress_setting != '\0') {
        char *end;
        errno = 0;
        unsigned long value = strtoul(progress_setting, &end, 10);
        if (errno || *end != '\0' || *progress_setting < '0' ||
            *progress_setting > '9' || value > 3600) {
            fprintf(stderr, "invalid EGTB_PROGRESS_SECONDS: expected 0..3600\n");
            goto done;
        }
        progress_seconds = (unsigned)value;
    }
    if (!egtb_progress_start(progress_seconds)) {
        fprintf(stderr, "cannot start progress reporter\n");
        goto done;
    }
    generation_started = wall_seconds();
    setup_seconds = generation_started - program_started;
    if (sliced) {
        EgtbSlicedOptions sliced_options = {
            thread_count,
            page_size,
            generation_cache_pages,
            readonly_cache_pages,
            verification_cache_pages,
            0,
            options.compression_level,
            catalog_probe,
            &catalogs[0],
            probe_contexts,
            NULL,
            NULL,
            false,
            (size_t)compilation_buffer_bytes,
            resident_limit_bytes
        };
        if (!egtb_generate_sliced(&database, work_path, &material, &indexer,
                                  &sliced_options, &generation)) {
            fprintf(stderr, "cannot generate sliced %s: %s\n", path,
                    egtb_sliced_last_error());
            goto done;
        }
        created = true;
    } else {
        if (!egtb_create(&database, work_path, positions - 1,
                         page_size, &options)) {
            fprintf(stderr, "cannot create %s: %s\n", path,
                    egtb_last_error());
            goto done;
        }
        created = true;
        EgtbThreadOptions thread_options = {
            thread_count, generation_cache_pages, probe_contexts,
            NULL, (size_t)compilation_buffer_bytes
        };
        if (!egtb_compile_threaded(database, &indexer, catalog_probe,
                                    &catalogs[0],
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
    snapshot_dependency_statistics(catalogs, thread_count);
    /* The first full forward pass also supplies every histogram and example.
     * The compact file remains unpublished throughout verification/repair. */
    finalize_seconds = 0.0;
    histogram = calloc((size_t)2 * 65536, sizeof(*histogram));
    if (histogram == NULL) goto done;
    phase_started = wall_seconds();
    EgtbVerificationOptions verify_options = {
        thread_count, verification_cache_pages, probe_contexts, NULL
    };
    EgtbConsistencyStatistics repair = {0};
    if (!egtb_finish_compiled(&database, work_path, &indexer,
            catalog_probe, &catalogs[0], &verify_options, resident_limit_bytes,
            &resident, &final_verification, &repair, histogram, &examples)) {
        fprintf(stderr, "cannot verify compiled database %s: %s\n",
                work_path, egtb_generator_last_error());
        goto done;
    }
    verification_seconds = wall_seconds() - phase_started;
    generation.consistency_passes += 1 + repair.passes;
    generation.consistency_updates[0] += repair.updates[0];
    generation.consistency_updates[1] += repair.updates[1];
    generation.maximum_dtm = final_verification.maximum_dtm;
    catalog_cache_statistics(catalogs, thread_count, &verification_dependencies);
    /* Dependency views stay warm across compilation and verification. Report
     * this phase's increments, not the cumulative generation counters. */
    verification_dependencies.lookups -= generation_dependencies.lookups;
    verification_dependencies.hits -= generation_dependencies.hits;
    verification_dependencies.misses -= generation_dependencies.misses;
    verification_dependencies.decompressions -= generation_dependencies.decompressions;
    verification_dependencies.dirty_evictions -= generation_dependencies.dirty_evictions;
    verification_dependencies.compressed_writes -= generation_dependencies.compressed_writes;
    phase_started = wall_seconds();
    if (!egtb_storage_statistics(database, &storage)) goto done;
    statistics_seconds = wall_seconds() - phase_started;
    resident_bytes_used = resident != NULL ? egtb_resident_bytes(resident) : 0;
    egtb_resident_destroy(resident);
    resident = NULL;
    if (!egtb_close(database)) { database = NULL; goto done; }
    database = NULL;
    phase_started = wall_seconds();
    if (!egtb_publish(work_path, path)) {
        fprintf(stderr, "cannot publish verified database: %s\n", egtb_last_error());
        goto done;
    }
    finalize_seconds = wall_seconds() - phase_started;
    printf("generated %s: material=%u %u %u %u positions=%" PRIu64
           " maximum-index=%" PRIu64 " passes=%" PRIu64
           " maximum-dtm=%u threads=%u\n", path, material.white_kings,
           material.white_men, material.black_kings, material.black_men,
           positions, positions - 1, generation.retrograde_passes,
           generation.maximum_dtm, thread_count);
    printf("self-consistency: passes=%" PRIu64 " updates=%" PRIu64
           "/%" PRIu64 "\n", generation.consistency_passes,
           generation.consistency_updates[0], generation.consistency_updates[1]);
    if (resident_bytes_used != 0)
        printf("final read-only consistency verification: threads=%u "
               "resident=%" PRIu64 " MiB positions-checked=%" PRIu64
               " positions-skipped=%" PRIu64 "\n",
               thread_count, resident_bytes_used / (1024 * 1024),
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
    print_dependency_statistics(catalogs, thread_count, false);
    print_dependency_statistics(catalogs, thread_count, true);
    dependency_resident_report(dependency_pool);
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
    printf("DTM example positions:\n");
    print_dtm_example("WTM longest win", &indexer,
                      &examples.longest_win[EGTB_WHITE_TO_MOVE],
                      EGTB_WHITE_TO_MOVE);
    print_dtm_example("WTM longest loss", &indexer,
                      &examples.longest_loss[EGTB_WHITE_TO_MOVE],
                      EGTB_WHITE_TO_MOVE);
    print_dtm_example("BTM longest win", &indexer,
                      &examples.longest_win[EGTB_BLACK_TO_MOVE],
                      EGTB_BLACK_TO_MOVE);
    print_dtm_example("BTM longest loss", &indexer,
                      &examples.longest_loss[EGTB_BLACK_TO_MOVE],
                      EGTB_BLACK_TO_MOVE);
    print_dtm_example("draw", &indexer, &examples.draw,
                      examples.draw_side);
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
    if (sliced)
        printf("  slice phases: sums for newly generated slices; %" PRIu64 " slices reused\n",
               generation.resumed_slices);
    printf("  %-28s %10.3f s\n", "setup", setup_seconds);
    printf("  %-28s %10.3f s\n", "initialization",
           generation.initialization_seconds);
    printf("  %-28s %10.3f s\n", "backpropagation",
           generation.backpropagation_seconds);
    printf("  %-28s %10.3f s\n", "frontier compilation",
           generation.compilation_seconds);
    if (sliced) {
        printf("  %-28s %10.3f s\n", "slice verification/fallback",
               generation.consistency_seconds);
        printf("  %-28s %10.3f s\n", "full-index merge",
               generation.slice_merge_seconds);
    }
    printf("  %-28s %10.3f s\n", "generation subtotal", generation_seconds);
    printf("  %-28s %10.3f s\n", "verify + statistics/fallback",
           verification_seconds);
    printf("  %-28s %10.3f s\n",
           "storage metadata",
           statistics_seconds);
    printf("  %-28s %10.3f s\n", "durable publication", finalize_seconds);
    printf("  %-28s %10.3f s\n", "total", total_seconds);
    ok = true;
    if (sliced && getenv("EGTB_KEEP_SLICES") == NULL &&
        !egtb_sliced_cleanup(path, &material))
        fprintf(stderr, "warning: generated database is valid, but the slice "
                        "workspace was retained: %s\n",
                egtb_sliced_last_error());
done:
    egtb_progress_end(ok);
    egtb_progress_stop();
    egtb_resident_destroy(resident);
    dependency_resident_destroy(dependency_pool);
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
    if (!ok && created && access(work_path, F_OK) == 0)
        fprintf(stderr, "unpublished database retained at %s\n", work_path);
    egtb_progress_log("GWDEGTB revision %s %s\n", gwdegtb_revision,
           ok ? "completed" : "failed");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
