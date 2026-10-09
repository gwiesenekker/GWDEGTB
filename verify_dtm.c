#define _POSIX_C_SOURCE 200809L

#include "egtb.h"
#include "dependency_resident.h"
#include "endgame_index.h"
#include "generator.h"
#include "material.h"
#include "progress.h"
#include "revision.h"
#include "crc32c.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define GIBIBYTE (UINT64_C(1024) * 1024 * 1024)
#define DEFAULT_RESIDENT_LIMIT_BYTES (UINT64_C(32) * GIBIBYTE)
#define DEFAULT_VERIFICATION_CACHE_BYTES (UINT64_C(32) * GIBIBYTE)
#define DEPENDENCY_CACHE_BYTES (UINT64_C(64) * 1024 * 1024)
#define DEFAULT_CHECKPOINT_PAGES UINT64_C(1048576)

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || !*text || *end || !parsed) return false;
    *value = (uint64_t)parsed;
    return true;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static uint64_t fingerprint_dtm_directory(void)
{
    DIR *dir = opendir(".");
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0, capacity = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    if (!dir) return 0;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 4 || strcmp(entry->d_name + length - 4, ".dtm")) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            char **grown = realloc(names, next * sizeof(*names));
            if (!grown) { hash = 0; goto done; }
            names = grown; capacity = next;
        }
        names[count] = strdup(entry->d_name);
        if (!names[count]) { hash = 0; goto done; }
        ++count;
    }
    if (closedir(dir) != 0) { dir = NULL; hash = 0; goto done; }
    dir = NULL;
    qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; ++i) {
        struct stat st;
        char row[512];
        int length;
        if (stat(names[i], &st) != 0 || !S_ISREG(st.st_mode)) { hash = 0; break; }
        length = snprintf(row, sizeof(row), "%s:%ju:%ju:%ju:%jd:%ld:%jd:%ld\n",
                          names[i], (uintmax_t)st.st_dev, (uintmax_t)st.st_ino,
                          (uintmax_t)st.st_size, (intmax_t)st.st_mtim.tv_sec,
                          st.st_mtim.tv_nsec, (intmax_t)st.st_ctim.tv_sec,
                          st.st_ctim.tv_nsec);
        if (length < 0 || (size_t)length >= sizeof(row)) { hash = 0; break; }
        for (int j = 0; j < length; ++j) {
            hash ^= (unsigned char)row[j];
            hash *= UINT64_C(1099511628211);
        }
    }
done:
    if (dir) closedir(dir);
    for (size_t i = 0; i < count; ++i) free(names[i]);
    free(names);
    return hash;
}

static bool checkpoint_text(char *text, size_t capacity, const char *name,
                            uint64_t positions, uint64_t page_entries,
                            uint64_t chunk_pages, uint64_t signature,
                            uint64_t next_index)
{
    int length = snprintf(text, capacity,
        "GWDEGTB-VERIFY-1\n%s\n%" PRIu64 " %" PRIu64 " %" PRIu64
        " %016" PRIx64 " %" PRIu64 "\n",
        name, positions, page_entries, chunk_pages, signature, next_index);
    if (length < 0 || (size_t)length >= capacity - 16) return false;
    uint32_t crc = crc32c(text, (size_t)length);
    length += snprintf(text + length, capacity - (size_t)length,
                       "%08" PRIx32 "\n", crc);
    return length > 0 && (size_t)length < capacity;
}

static bool checkpoint_read(const char *path, const char *name,
                            uint64_t positions, uint64_t page_entries,
                            uint64_t chunk_pages, uint64_t signature,
                            uint64_t *next_index)
{
    char text[512], expected[512], stored_name[128], magic[32];
    uint64_t stored_positions, stored_entries, stored_chunk, stored_signature, stored_next;
    uint32_t stored_crc;
    FILE *file = fopen(path, "rb");
    size_t length;
    if (!file) return false;
    length = fread(text, 1, sizeof(text) - 1, file);
    bool read_ok = !ferror(file) && feof(file);
    if (fclose(file) != 0 || !read_ok) return false;
    text[length] = 0;
    if (sscanf(text, "%31s\n%127s\n%" SCNu64 " %" SCNu64 " %" SCNu64
               " %" SCNx64 " %" SCNu64 "\n%" SCNx32,
               magic, stored_name, &stored_positions, &stored_entries,
               &stored_chunk, &stored_signature, &stored_next, &stored_crc) != 8 ||
        strcmp(magic, "GWDEGTB-VERIFY-1") || strcmp(name, stored_name) ||
        stored_positions != positions || stored_entries != page_entries ||
        stored_chunk != chunk_pages || stored_signature != signature ||
        stored_next > positions ||
        (stored_next != positions && stored_next % page_entries)) return false;
    if (!checkpoint_text(expected, sizeof(expected), name, positions,
                         page_entries, chunk_pages, signature, stored_next) ||
        strcmp(text, expected)) return false;
    *next_index = stored_next;
    return true;
}

static bool checkpoint_write(const char *path, const char *name,
                             uint64_t positions, uint64_t page_entries,
                             uint64_t chunk_pages, uint64_t signature,
                             uint64_t next_index)
{
    char text[512], temporary[4096], parent[4096];
    const char *slash;
    int fd, parent_fd;
    size_t length, offset = 0;
    if (!checkpoint_text(text, sizeof(text), name, positions, page_entries,
                         chunk_pages, signature, next_index) ||
        snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >=
            (int)sizeof(temporary)) return false;
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    length = strlen(text);
    while (offset < length) {
        ssize_t written = write(fd, text + offset, length - offset);
        if (written <= 0) break;
        offset += (size_t)written;
    }
    bool ok = offset == length && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) ok = rename(temporary, path) == 0;
    if (!ok) { unlink(temporary); return false; }
    slash = strrchr(path, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == path) strcpy(parent, "/");
    else {
        size_t size = (size_t)(slash - path);
        if (size >= sizeof(parent)) return false;
        memcpy(parent, path, size); parent[size] = 0;
    }
    parent_fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (parent_fd < 0) return false;
    ok = fsync(parent_fd) == 0;
    if (close(parent_fd) != 0) ok = false;
    return ok;
}

typedef struct {
    EgtbMaterialKind kind;
    EgtbMaterial canonical;
    Egtb *database;
    EgtbView *view;
    const EgtbResident *resident;
    EgtbSharedProbe *shared_probe;
    uint64_t resident_lookups;
    EgIndexer indexer;
    bool indexer_initialized;
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
    memset(catalog, 0, sizeof(*catalog));
    catalog->cache_bytes = cache_bytes;
    catalog->resident_pool = pool;
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

static void print_shared_statistics(const DatabaseCatalog *catalogs, unsigned count)
{
    uint64_t busy=0, invalidated=0, conflicts=0, published=0;
    unsigned probes=0;
    for (unsigned c=0; c<count; ++c)
        for (unsigned wk=0; wk<=EGTB_MAX_PIECES; ++wk)
            for (unsigned wm=0; wm<=EGTB_MAX_PIECES; ++wm)
                for (unsigned bk=0; bk<=EGTB_MAX_PIECES; ++bk)
                    for (unsigned bm=0; bm<=EGTB_MAX_PIECES; ++bm) {
                        EgtbSharedProbe *p = catalogs[c].entry[wk][wm][bk][bm].shared_probe;
                        if (!p) continue;
                        EgtbSharedStatistics s;
                        egtb_shared_probe_statistics(p, &s);
                        ++probes; busy += s.busy_reads; invalidated += s.invalidated_reads;
                        conflicts += s.publication_conflicts; published += s.publications;
                    }
    if (probes)
        printf("optimistic dependency cache activity (whole run):\n"
               "  Busy reads              %20" PRIu64 "\n"
               "  Invalidated reads       %20" PRIu64 "\n"
               "  Publication conflicts   %20" PRIu64 "\n"
               "  Pages published         %20" PRIu64 "\n",
               busy, invalidated, conflicts, published);
}

static void close_catalog(DatabaseCatalog *catalog)
{
    for (unsigned wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
        for (unsigned wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
            for (unsigned bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                for (unsigned bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                    CatalogEntry *entry = &catalog->entry[wk][wm][bk][bm];
                    dependency_private_unregister(catalog->resident_pool, &entry->shared_probe);
                    egtb_shared_probe_destroy(entry->shared_probe);
                    entry->shared_probe = NULL;
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
    if (entry->resident != NULL || entry->view != NULL || entry->shared_probe != NULL)
        return true;
    if (entry->database != NULL) return false;
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
    if (entry->resident) return true;
    EgtbSharedCache *shared = NULL;
    if (!dependency_shared_acquire(catalog->resident_pool, entry->database, &shared)) {
        snprintf(catalog->error, sizeof(catalog->error), "%s", dependency_resident_error());
        return false;
    }
    if (shared) {
        if (egtb_shared_probe_create(&entry->shared_probe, shared)) return true;
        snprintf(catalog->error, sizeof(catalog->error), "%s", egtb_last_error());
        return false;
    }
    if (!egtb_view_create(&entry->view, entry->database,
                          pages == 0 ? 1 : pages, false)) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot create dependency view for %.100s: %.100s", path,
                 egtb_last_error());
        return false;
    }
    if (!dependency_private_register(catalog->resident_pool, entry->database,
                                     entry->view, &entry->shared_probe)) {
        snprintf(catalog->error,sizeof(catalog->error),"%s",dependency_resident_error());
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
        !(entry->resident ? egtb_resident_get(entry->resident, index, side, value)
                           : entry->shared_probe ? egtb_shared_probe_get(entry->shared_probe, index, side, value)
                           : egtb_view_get(entry->view, index, side, value))) {
        snprintf(catalog->error, sizeof(catalog->error),
                 "cannot query dependency: %.200s", egtb_last_error());
        return false;
    }
    if (entry->resident) ++entry->resident_lookups;
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
                        if (entry->database == NULL)
                            continue;
                        egtb_view_cache_statistics(entry->view, &part);
                        if (entry->shared_probe) {
                            EgtbSharedStatistics shared;
                            egtb_shared_probe_statistics(entry->shared_probe, &shared);
                            add_cache_statistics(&part, &shared.cache);
                        }
                        part.lookups += entry->resident_lookups;
                        part.hits += entry->resident_lookups;
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
    const char *checkpoint = NULL;
    uint64_t chunk_pages = DEFAULT_CHECKPOINT_PAGES;
    uint64_t max_chunks = 0, completed_chunks = 0;
    uint64_t signature = 0, next_index = 0, resumed_index = 0, positions = 0;
    int argument = 1;
    unsigned thread_count = 4, progress_seconds = 60;
    EgtbMaterial requested, material;
    EgtbMaterialKind kind;
    EgIndexer indexer;
    bool indexer_initialized = false;
    Egtb *database = NULL;
    EgtbResident *resident = NULL;
    DatabaseCatalog *catalogs = NULL;
    DependencyResidentPool *dependency_pool = NULL;
    void **contexts = NULL;
    EgtbConsistencyStatistics verification = {0};
    EgtbCacheStatistics dependency_statistics = {0};
    EgtbCacheStatistics current_statistics = {0};
    uint64_t resident_limit, verification_cache_bytes, resident_bytes;
    size_t verification_cache_pages;
    char filename[128];
    char checkpoint_lock_path[4096];
    int checkpoint_lock_fd = -1;
    bool ok = false;
    bool paused = false;

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
        } else if (strcmp(argv[argument], "--checkpoint") == 0 &&
                   argument + 1 < argc) {
            checkpoint = argv[argument + 1];
            argument += 2;
        } else if (strcmp(argv[argument], "--chunk-pages") == 0 &&
                   argument + 1 < argc) {
            if (!parse_u64(argv[argument + 1], &chunk_pages)) {
                fprintf(stderr, "chunk pages must be positive\n");
                return EXIT_FAILURE;
            }
            argument += 2;
        } else if (strcmp(argv[argument], "--max-chunks") == 0 &&
                   argument + 1 < argc) {
            if (!parse_u64(argv[argument + 1], &max_chunks)) {
                fprintf(stderr, "max chunks must be positive\n");
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
                        "[--checkpoint FILE] [--chunk-pages N] [--max-chunks N] "
                        "NWHITE_KINGS NWHITE_MEN NBLACK_KINGS NBLACK_MEN\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    kind = egtb_material_resolve(&requested, &material);
    if (max_chunks && !checkpoint) {
        fprintf(stderr, "--max-chunks requires --checkpoint\n");
        return EXIT_FAILURE;
    }
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
    if (checkpoint) {
        if (snprintf(checkpoint_lock_path, sizeof(checkpoint_lock_path),
                     "%s.lock", checkpoint) >= (int)sizeof(checkpoint_lock_path) ||
            (checkpoint_lock_fd = open(checkpoint_lock_path,
                                       O_RDWR | O_CREAT, 0600)) < 0 ||
            flock(checkpoint_lock_fd, LOCK_EX | LOCK_NB) != 0) {
            fprintf(stderr, "cannot lock checkpoint %s: %s\n", checkpoint,
                    strerror(errno));
            if (checkpoint_lock_fd >= 0) close(checkpoint_lock_fd);
            return EXIT_FAILURE;
        }
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
    positions = eg_position_count(&indexer);
    if (checkpoint) {
        signature = fingerprint_dtm_directory();
        if (!signature) {
            fprintf(stderr, "cannot fingerprint DTM directory\n");
            goto done;
        }
        if (access(checkpoint, F_OK) == 0) {
            if (!checkpoint_read(checkpoint, filename, positions,
                                 egtb_positions_per_page(database), chunk_pages,
                                 signature, &next_index)) {
                fprintf(stderr, "checkpoint is corrupt or its DTM inputs changed: %s\n",
                        checkpoint);
                goto done;
            }
            printf("resuming verification at index %" PRIu64 "/%" PRIu64 "\n",
                   next_index, positions);
        } else if (errno != ENOENT) {
            fprintf(stderr, "cannot inspect checkpoint %s: %s\n", checkpoint,
                    strerror(errno));
            goto done;
        }
        resumed_index = next_index;
    }
    if (!egtb_progress_start(progress_seconds)) {
        fprintf(stderr, "cannot start progress reporter\n");
        goto done;
    }
    egtb_progress_log("GWDEGTB revision %s verifying %s with %u threads\n",
                      gwdegtb_revision, filename, thread_count);
    if (next_index == positions) {
        printf("VERIFIED %s: checkpoint covers all %" PRIu64 " DTM values\n",
               filename, positions * 2);
        ok = true;
        goto done;
    }
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
    if (!dependency_resident_configure(&dependency_pool)) {
        fprintf(stderr, "%s\n", dependency_resident_error());
        goto done;
    }
    if (dependency_shared_adaptive(dependency_pool))
        egtb_generator_quiescent_hook(dependency_shared_maintain, dependency_pool);
    egtb_generator_phase_hook(dependency_shared_phase, dependency_pool);
    for (unsigned worker = 0; worker < thread_count; ++worker) {
        initialize_catalog(&catalogs[worker], DEPENDENCY_CACHE_BYTES, dependency_pool);
        contexts[worker] = &catalogs[worker];
    }
    {
        EgtbVerificationOptions options = {
            thread_count, verification_cache_pages, contexts, resident, 0, 0
        };
        uint64_t verified_values = 0;
        do {
            if (checkpoint) {
                uint64_t page_entries = egtb_positions_per_page(database);
                uint64_t remaining_pages = (positions - next_index + page_entries - 1) / page_entries;
                uint64_t pages = chunk_pages < remaining_pages ? chunk_pages : remaining_pages;
                options.first_index = next_index;
                options.end_index = next_index + pages * page_entries;
                if (options.end_index > positions) options.end_index = positions;
            }
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
            verified_values += verification.positions_checked;
            current_statistics.lookups += verification.cache.lookups;
            current_statistics.hits += verification.cache.hits;
            current_statistics.misses += verification.cache.misses;
            current_statistics.decompressions += verification.cache.decompressions;
            current_statistics.dirty_evictions += verification.cache.dirty_evictions;
            current_statistics.compressed_writes += verification.cache.compressed_writes;
            if (!checkpoint) break;
            next_index = options.end_index;
            if (fingerprint_dtm_directory() != signature) {
                fprintf(stderr, "DTM inputs changed during verification; checkpoint not advanced\n");
                goto done;
            }
            if (!checkpoint_write(checkpoint, filename, positions,
                                  egtb_positions_per_page(database), chunk_pages,
                                  signature, next_index)) {
                fprintf(stderr, "cannot save checkpoint %s: %s\n", checkpoint,
                        strerror(errno));
                goto done;
            }
            printf("verified through index %" PRIu64 "/%" PRIu64 "\n",
                   next_index, positions);
            fflush(stdout);
            ++completed_chunks;
            if (max_chunks && completed_chunks == max_chunks && next_index < positions) {
                paused = true;
                goto done;
            }
        } while (next_index < positions);
        verification.positions_checked = verified_values;
        verification.cache = current_statistics;
    }
    catalog_cache_statistics(catalogs, thread_count,
                             &dependency_statistics);
    printf("VERIFIED %s: positions=%" PRIu64
           " DTM-values-checked=%" PRIu64 " newly-checked=%" PRIu64
           " checkpointed=%" PRIu64 " threads=%u mode=%s\n",
           filename, eg_position_count(&indexer),
           verification.positions_checked + resumed_index * 2,
           verification.positions_checked, resumed_index * 2, thread_count,
           resident != NULL ? "resident" : "disk-cache");
    print_cache_statistics("current-DTM cache", &verification.cache);
    print_shared_statistics(catalogs, thread_count);
    print_cache_statistics("dependency-DTM caches", &dependency_statistics);
    dependency_resident_report(dependency_pool);
    ok = true;

done:
    egtb_generator_quiescent_hook(NULL, NULL);
    egtb_generator_phase_hook(NULL, NULL);
    egtb_progress_stop();
    dependency_resident_destroy(dependency_pool);
    if (catalogs != NULL) {
        for (unsigned worker = 0; worker < thread_count; ++worker) {
            catalogs[worker].resident_pool = NULL;
            close_catalog(&catalogs[worker]);
        }
    }
    free(contexts);
    free(catalogs);
    egtb_resident_destroy(resident);
    if (database != NULL && !egtb_close(database))
        ok = false;
    if (indexer_initialized)
        eg_indexer_destroy(&indexer);
    if (checkpoint_lock_fd >= 0) close(checkpoint_lock_fd);
    egtb_progress_log("GWDEGTB revision %s verification %s\n",
                      gwdegtb_revision, paused ? "paused" : ok ? "completed" : "failed");
    return paused ? 2 : ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
