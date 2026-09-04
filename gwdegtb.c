#define _POSIX_C_SOURCE 200809L

#include "gwdegtb.h"

#include "endgame_index.h"
#include "egtb.h"
#include "material.h"
#include "wdl.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zstd.h>

#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#define GWDEGTB_HAVE_BMI2 1
#else
#define GWDEGTB_HAVE_BMI2 0
#endif

#define MATERIAL_DIMENSION (EGTB_MAX_PIECES + 1)
#define DEFAULT_WDL_COMPRESSION_LEVEL 3
#define DEFAULT_WDL_DECOMPRESSION_THREADS 4
#define DEFAULT_DTM_CACHE_PAGES 16384

typedef struct {
    unsigned char *bitmap;
    size_t bytes;
    EgIndexer indexer;
} ResidentWdl;

typedef struct {
    Egtb *database;
    EgIndexer indexer;
} DiskDtm;

typedef struct {
    WdlImage *image;
    EgIndexer indexer;
    uint32_t material_id;
} CompressedWdl;

typedef struct {
    const CompressedWdl *database;
    uint64_t page;
    unsigned char data[WDL_PAGE_SIZE];
} CompressedCacheEntry;

struct GwdegtbWdlProbe {
    CompressedCacheEntry *entries;
    size_t capacity;
    size_t mask;
    ZSTD_DCtx *decompressor;
    GwdegtbWdlProbeStatistics statistics;
};

static _Atomic(ResidentWdl *)
    resident_wdls[MATERIAL_DIMENSION][MATERIAL_DIMENSION]
                  [MATERIAL_DIMENSION][MATERIAL_DIMENSION];
static _Atomic(DiskDtm *)
    disk_dtms[MATERIAL_DIMENSION][MATERIAL_DIMENSION]
             [MATERIAL_DIMENSION][MATERIAL_DIMENSION];
static _Atomic(CompressedWdl *)
    compressed_wdls[MATERIAL_DIMENSION][MATERIAL_DIMENSION]
                   [MATERIAL_DIMENSION][MATERIAL_DIMENSION];
static bool
    dtm_open_attempted[MATERIAL_DIMENSION][MATERIAL_DIMENSION]
                      [MATERIAL_DIMENSION][MATERIAL_DIMENSION];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char last_error[256];

static bool fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_error, sizeof(last_error), format, arguments);
    va_end(arguments);
    return false;
}

const char *gwdegtb_last_error(void)
{
    return last_error;
}

#if !GWDEGTB_HAVE_BMI2
static unsigned first_bit(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(value);
#else
    unsigned bit = 0;
    while ((value & 1) == 0) {
        value >>= 1;
        ++bit;
    }
    return bit;
#endif
}
#endif

static unsigned popcount(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_popcountll(value);
#else
    unsigned count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
#endif
}

uint64_t gwdegtb_gwd_to_compact(uint64_t padded)
{
#if GWDEGTB_HAVE_BMI2
    return _pext_u64(padded, GWDEGTB_GWD_BOARD_MASK);
#else
    uint64_t compact = 0;
    padded &= GWDEGTB_GWD_BOARD_MASK;
    while (padded != 0) {
        uint64_t bit = padded & (UINT64_C(0) - padded);
        unsigned field = first_bit(bit);
        unsigned relative = field - 6;
        unsigned square = relative - relative / 11;
        compact |= UINT64_C(1) << square;
        padded &= padded - 1;
    }
    return compact;
#endif
}

static _Atomic(ResidentWdl *) *registry_slot(const EgtbMaterial *material)
{
    return &resident_wdls[material->white_kings][material->white_men]
                          [material->black_kings][material->black_men];
}

static _Atomic(DiskDtm *) *dtm_registry_slot(const EgtbMaterial *material)
{
    return &disk_dtms[material->white_kings][material->white_men]
                     [material->black_kings][material->black_men];
}

static _Atomic(CompressedWdl *) *compressed_registry_slot(
    const EgtbMaterial *material)
{
    return &compressed_wdls[material->white_kings][material->white_men]
                            [material->black_kings][material->black_men];
}

static char *database_path(const char *directory,
                           const EgtbMaterial *material,
                           const char *extension)
{
    char filename[96];
    size_t directory_length = directory == NULL ? 0 : strlen(directory);
    bool separator = directory_length != 0 &&
                     directory[directory_length - 1] != '/';
    size_t filename_length;
    char *path;

    if (!egtb_material_filename(filename, sizeof(filename),
                                material->white_kings,
                                material->white_men,
                                material->black_kings,
                                material->black_men, extension))
        return NULL;
    filename_length = strlen(filename);
    if (directory_length > SIZE_MAX - filename_length - separator - 1)
        return NULL;
    path = malloc(directory_length + separator + filename_length + 1);
    if (path == NULL)
        return NULL;
    if (directory_length != 0)
        memcpy(path, directory, directory_length);
    if (separator)
        path[directory_length++] = '/';
    memcpy(path + directory_length, filename, filename_length + 1);
    return path;
}

static bool parse_database_name(const char *database_name,
                                EgtbMaterial *requested,
                                EgtbMaterial *canonical,
                                EgtbMaterialKind *kind)
{
    int consumed = 0;
    const char *suffix;

    if (database_name == NULL || requested == NULL || canonical == NULL ||
        kind == NULL || strchr(database_name, '/') != NULL ||
        sscanf(database_name, "%uwX-%uwO-%ubX-%ubO%n",
               &requested->white_kings, &requested->white_men,
               &requested->black_kings, &requested->black_men,
               &consumed) != 4)
        return fail("invalid WDL database name");
    suffix = database_name + consumed;
    if (*suffix != '\0' && strcmp(suffix, ".wdl") != 0)
        return fail("invalid WDL database name: %s", database_name);
    *kind = egtb_material_resolve(requested, canonical);
    if (*kind != EGTB_MATERIAL_CANONICAL &&
        *kind != EGTB_MATERIAL_MIRROR)
        return fail("invalid or terminal WDL database name: %s",
                    database_name);
    return true;
}

static bool material_info(const EgtbMaterial *canonical,
                          uint64_t *maximum_index, size_t *size)
{
    EgIndexer indexer;
    uint64_t positions, bytes;
    if (!eg_indexer_init(&indexer,
                         canonical->white_men, canonical->black_men,
                         canonical->white_kings, canonical->black_kings))
        return fail("cannot initialize resident WDL indexer");
    positions = eg_position_count(&indexer);
    bytes = positions / 2 + (positions % 2 != 0);
    if (bytes > SIZE_MAX) {
        eg_indexer_destroy(&indexer);
        return fail("resident WDL is too large for this platform");
    }
    *maximum_index = eg_max_index(&indexer);
    *size = (size_t)bytes;
    eg_indexer_destroy(&indexer);
    return true;
}

bool gwdegtb_wdl_info(const char *database_name, uint64_t *maximum_index,
                      size_t *size)
{
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;

    if (maximum_index == NULL || size == NULL)
        return fail("invalid resident WDL information argument");
    *maximum_index = 0;
    *size = 0;
    if (!parse_database_name(database_name, &requested, &canonical, &kind))
        return false;
    return material_info(&canonical, maximum_index, size);
}

bool gwdegtb_wdl_decompress(const char *directory,
                            const char *database_name,
                            void *data, size_t size)
{
    return gwdegtb_wdl_decompress_threads(
        directory, database_name, data, size,
        DEFAULT_WDL_DECOMPRESSION_THREADS);
}

bool gwdegtb_wdl_decompress_threads(const char *directory,
                                    const char *database_name,
                                    void *data, size_t size,
                                    unsigned thread_count)
{
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;
    Wdl *wdl = NULL;
    char *path = NULL;
    uint64_t maximum_index;
    size_t expected_size;
    bool ok = false;

    if (data == NULL || thread_count == 0 ||
        !parse_database_name(database_name, &requested, &canonical, &kind) ||
        !material_info(&canonical, &maximum_index, &expected_size))
        return false;
    if (size != expected_size)
        return fail("resident WDL buffer is %zu bytes; expected %zu",
                    size, expected_size);
    path = database_path(directory, &canonical, "wdl");
    if (path == NULL)
        return fail("cannot allocate resident WDL path");
    if (!wdl_open(&wdl, path, 1, DEFAULT_WDL_COMPRESSION_LEVEL,
                  DEFAULT_DTM_CACHE_PAGES)) {
        fail("cannot open or generate %s: %s", path, wdl_last_error());
        goto done;
    }
    if (wdl_maximum_index(wdl) != maximum_index) {
        fail("WDL maximum index does not match material for %s", path);
        goto done;
    }
    if (!wdl_decompress_into_threaded(wdl, data, size, thread_count)) {
        fail("cannot decompress %s: %s", path, wdl_last_error());
        goto done;
    }
    if (!wdl_close(wdl)) {
        wdl = NULL;
        fail("cannot close %s: %s", path, wdl_last_error());
        goto done;
    }
    wdl = NULL;
    ok = true;

done:
    if (wdl != NULL)
        wdl_close(wdl);
    free(path);
    return ok;
}

bool gwdegtb_wdl_attach(const char *database_name,
                        const void *data, size_t size)
{
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;
    ResidentWdl *resident = NULL, *old;
    _Atomic(ResidentWdl *) *slot;
    uint64_t positions, expected_bytes;

    if (data == NULL ||
        !parse_database_name(database_name, &requested, &canonical, &kind))
        return false;
    resident = calloc(1, sizeof(*resident));
    if (resident == NULL)
        return fail("cannot allocate attached WDL registry entry");
    if (!eg_indexer_init(&resident->indexer,
                         canonical.white_men, canonical.black_men,
                         canonical.white_kings, canonical.black_kings)) {
        free(resident);
        return fail("cannot initialize attached WDL indexer");
    }
    positions = eg_position_count(&resident->indexer);
    expected_bytes = positions / 2 + (positions % 2 != 0);
    if (expected_bytes > SIZE_MAX || size != (size_t)expected_bytes) {
        eg_indexer_destroy(&resident->indexer);
        free(resident);
        return fail("attached WDL size is %zu bytes; expected %llu",
                    size, (unsigned long long)expected_bytes);
    }
    resident->bitmap = (unsigned char *)data;
    resident->bytes = size;

    pthread_mutex_lock(&registry_mutex);
    slot = registry_slot(&canonical);
    old = atomic_exchange_explicit(slot, resident, memory_order_acq_rel);
    pthread_mutex_unlock(&registry_mutex);
    if (old != NULL) {
        eg_indexer_destroy(&old->indexer);
        free(old);
    }
    return true;
}

bool gwdegtb_wdl_is_loaded(unsigned white_kings, unsigned white_men,
                           unsigned black_kings, unsigned black_men)
{
    EgtbMaterial requested = {white_kings, white_men,
                              black_kings, black_men};
    EgtbMaterial canonical;
    EgtbMaterialKind kind = egtb_material_resolve(&requested, &canonical);
    if (kind != EGTB_MATERIAL_CANONICAL && kind != EGTB_MATERIAL_MIRROR)
        return false;
    return atomic_load_explicit(registry_slot(&canonical),
                                memory_order_acquire) != NULL;
}

void gwdegtb_wdl_unload_all(void)
{
    unsigned wk, wm, bk, bm;
    pthread_mutex_lock(&registry_mutex);
    for (wk = 0; wk < MATERIAL_DIMENSION; ++wk)
        for (wm = 0; wm < MATERIAL_DIMENSION; ++wm)
            for (bk = 0; bk < MATERIAL_DIMENSION; ++bk)
                for (bm = 0; bm < MATERIAL_DIMENSION; ++bm) {
                    _Atomic(ResidentWdl *) *slot =
                        &resident_wdls[wk][wm][bk][bm];
                    ResidentWdl *resident = atomic_exchange_explicit(
                        slot, NULL, memory_order_acq_rel);
                    if (resident == NULL)
                        continue;
                    eg_indexer_destroy(&resident->indexer);
                    free(resident);
                }
    pthread_mutex_unlock(&registry_mutex);
}

static inline bool prepare_compact_position(
    uint64_t white_kings, uint64_t white_men,
    uint64_t black_kings, uint64_t black_men,
    GwdegtbSide side, DraughtsPosition *position,
    EgtbSide *lookup_side)
{
    uint64_t all = white_kings | white_men | black_kings | black_men;

    if ((side != GWDEGTB_WHITE_TO_MOVE &&
         side != GWDEGTB_BLACK_TO_MOVE) ||
        (all & ~GWDEGTB_COMPACT_BOARD_MASK) != 0 ||
        ((white_kings & white_men) | (white_kings & black_kings) |
         (white_kings & black_men) | (white_men & black_kings) |
         (white_men & black_men) | (black_kings & black_men)) != 0)
        return false;
    position->white_kings = white_kings;
    position->white_men = white_men;
    position->black_kings = black_kings;
    position->black_men = black_men;
    if ((white_men & UINT64_C(0x1f)) != 0 ||
        (black_men & (UINT64_C(0x1f) << 45)) != 0)
        return false;
    *lookup_side = side == GWDEGTB_WHITE_TO_MOVE
                       ? EGTB_WHITE_TO_MOVE : EGTB_BLACK_TO_MOVE;
    return true;
}

static inline bool resolve_compact_material(DraughtsPosition *position,
                                            EgtbMaterial *canonical,
                                            EgtbSide *lookup_side)
{
    DraughtsPosition mirrored;
    EgtbMaterial requested;
    EgtbMaterialKind kind;

    requested.white_kings = popcount(position->white_kings);
    requested.white_men = popcount(position->white_men);
    requested.black_kings = popcount(position->black_kings);
    requested.black_men = popcount(position->black_men);
    kind = egtb_material_resolve(&requested, canonical);
    if (kind != EGTB_MATERIAL_CANONICAL && kind != EGTB_MATERIAL_MIRROR)
        return false;
    if (kind == EGTB_MATERIAL_MIRROR) {
        egtb_mirror_position(position, &mirrored);
        *position = mirrored;
        *lookup_side = egtb_mirror_side(*lookup_side);
    }
    return true;
}

int16_t gwdegtb_wdl_lookup_compact(uint64_t white_kings,
                                   uint64_t white_men,
                                   uint64_t black_kings,
                                   uint64_t black_men,
                                   GwdegtbSide side)
{
    DraughtsPosition position;
    EgPosition indexed_position;
    EgtbMaterial canonical;
    ResidentWdl *resident;
    EgtbSide lookup_side;
    uint64_t index;
    unsigned char packed, value;

    if (!prepare_compact_position(
            white_kings, white_men, black_kings, black_men, side,
            &position, &lookup_side) ||
        !resolve_compact_material(&position, &canonical, &lookup_side))
        return GWDEGTB_WDL_UNAVAILABLE;
    resident = atomic_load_explicit(registry_slot(&canonical),
                                    memory_order_acquire);
    if (resident == NULL)
        return GWDEGTB_WDL_UNAVAILABLE;
    indexed_position.white_men = position.white_men;
    indexed_position.black_men = position.black_men;
    indexed_position.white_kings = position.white_kings;
    indexed_position.black_kings = position.black_kings;
    if (!eg_position_to_index(&resident->indexer, &indexed_position, &index))
        return GWDEGTB_WDL_UNAVAILABLE;
    packed = resident->bitmap[index / 2];
    value = (packed >> ((index % 2) * 4 + lookup_side * 2)) & 3;
    if (value == WDL_WIN)
        return GWDEGTB_WDL_WIN;
    if (value == WDL_LOSS)
        return GWDEGTB_WDL_LOSS;
    if (value == WDL_DRAW)
        return GWDEGTB_WDL_DRAW;
    return GWDEGTB_WDL_UNAVAILABLE;
}

int16_t gwdegtb_wdl_lookup(uint64_t white_kings, uint64_t white_men,
                           uint64_t black_kings, uint64_t black_men,
                           GwdegtbSide side)
{
    uint64_t all = white_kings | white_men | black_kings | black_men;
    if ((all & ~GWDEGTB_GWD_BOARD_MASK) != 0)
        return GWDEGTB_WDL_UNAVAILABLE;
    return gwdegtb_wdl_lookup_compact(
        gwdegtb_gwd_to_compact(white_kings),
        gwdegtb_gwd_to_compact(white_men),
        gwdegtb_gwd_to_compact(black_kings),
        gwdegtb_gwd_to_compact(black_men), side);
}

static bool ensure_compressed_wdl(const char *directory,
                                  const EgtbMaterial *canonical,
                                  char **path)
{
    Wdl *wdl = NULL;
    *path = database_path(directory, canonical, "wdl");
    if (*path == NULL)
        return fail("cannot allocate compressed WDL path");
    if (access(*path, F_OK) == 0)
        return true;
    if (!wdl_open(&wdl, *path, 1, DEFAULT_WDL_COMPRESSION_LEVEL,
                  DEFAULT_DTM_CACHE_PAGES))
        return fail("cannot open or generate %s: %s", *path,
                    wdl_last_error());
    if (!wdl_close(wdl))
        return fail("cannot close %s: %s", *path, wdl_last_error());
    return true;
}

static bool compressed_wdl_can_be_provided(const char *directory,
                                            const EgtbMaterial *canonical,
                                            bool *available)
{
    const char *base = directory != NULL && directory[0] != '\0'
                           ? directory : ".";
    char *wdl_path = NULL, *dtm_path = NULL;
    struct stat status;
    bool ok = false;

    *available = false;
    if (stat(base, &status) != 0)
        return fail("cannot access WDL directory %s: %s", base,
                    strerror(errno));
    if (!S_ISDIR(status.st_mode))
        return fail("WDL directory is not a directory: %s", base);
    wdl_path = database_path(directory, canonical, "wdl");
    if (wdl_path == NULL) {
        fail("cannot allocate compressed WDL path");
        goto done;
    }
    if (access(wdl_path, F_OK) == 0) {
        *available = true;
        ok = true;
        goto done;
    }
    if (errno != ENOENT) {
        fail("cannot access %s: %s", wdl_path, strerror(errno));
        goto done;
    }
    dtm_path = database_path(directory, canonical, "dtm");
    if (dtm_path == NULL) {
        fail("cannot allocate DTM path");
        goto done;
    }
    if (access(dtm_path, F_OK) == 0)
        *available = true;
    else if (errno != ENOENT) {
        fail("cannot access %s: %s", dtm_path, strerror(errno));
        goto done;
    }
    ok = true;
done:
    free(wdl_path);
    free(dtm_path);
    return ok;
}

bool gwdegtb_wdl_compressed_info(const char *directory,
                                 const char *database_name,
                                 size_t *size)
{
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;
    char *path = NULL;
    bool available;
    bool ok = false;
    if (size == NULL)
        return fail("invalid compressed WDL size output");
    *size = 0;
    if (!parse_database_name(database_name, &requested, &canonical, &kind) ||
        !compressed_wdl_can_be_provided(directory, &canonical, &available))
        goto done;
    if (!available) {
        ok = true;
        goto done;
    }
    if (!ensure_compressed_wdl(directory, &canonical, &path))
        goto done;
    if (!wdl_file_size(path, size)) {
        fail("cannot size %s: %s", path, wdl_last_error());
        goto done;
    }
    ok = true;
done:
    free(path);
    return ok;
}

bool gwdegtb_wdl_compressed_load(const char *directory,
                                 const char *database_name,
                                 void *data, size_t size)
{
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;
    char *path = NULL;
    bool ok = false;
    if (data == NULL)
        return fail("invalid compressed WDL destination");
    if (!parse_database_name(database_name, &requested, &canonical, &kind) ||
        !ensure_compressed_wdl(directory, &canonical, &path))
        goto done;
    if (!wdl_file_load_into(path, data, size)) {
        fail("cannot load %s: %s", path, wdl_last_error());
        goto done;
    }
    ok = true;
done:
    free(path);
    return ok;
}

static void destroy_compressed_wdl(CompressedWdl *compressed)
{
    if (compressed == NULL)
        return;
    wdl_image_destroy(compressed->image);
    eg_indexer_destroy(&compressed->indexer);
    free(compressed);
}

bool gwdegtb_wdl_compressed_attach(const char *database_name,
                                   const void *data, size_t size)
{
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;
    CompressedWdl *compressed = NULL, *old;
    _Atomic(CompressedWdl *) *slot;
    if (data == NULL ||
        !parse_database_name(database_name, &requested, &canonical, &kind))
        return false;
    compressed = calloc(1, sizeof(*compressed));
    if (compressed == NULL)
        return fail("cannot allocate compressed WDL registry entry");
    if (!wdl_image_attach(&compressed->image, data, size)) {
        fail("cannot attach compressed WDL image: %s", wdl_last_error());
        goto failure;
    }
    if (!eg_indexer_init(&compressed->indexer,
                         canonical.white_men, canonical.black_men,
                         canonical.white_kings, canonical.black_kings)) {
        fail("cannot initialize compressed WDL indexer");
        goto failure;
    }
    if (wdl_image_maximum_index(compressed->image) !=
        eg_max_index(&compressed->indexer)) {
        fail("compressed WDL maximum index does not match its material");
        goto failure;
    }
    compressed->material_id = canonical.white_kings |
                              canonical.white_men << 4 |
                              canonical.black_kings << 8 |
                              canonical.black_men << 12;
    pthread_mutex_lock(&registry_mutex);
    slot = compressed_registry_slot(&canonical);
    old = atomic_exchange_explicit(slot, compressed, memory_order_acq_rel);
    pthread_mutex_unlock(&registry_mutex);
    destroy_compressed_wdl(old);
    return true;
failure:
    destroy_compressed_wdl(compressed);
    return false;
}

bool gwdegtb_wdl_compressed_is_loaded(unsigned white_kings,
                                      unsigned white_men,
                                      unsigned black_kings,
                                      unsigned black_men)
{
    EgtbMaterial requested = {white_kings, white_men,
                              black_kings, black_men};
    EgtbMaterial canonical;
    EgtbMaterialKind kind = egtb_material_resolve(&requested, &canonical);
    if (kind != EGTB_MATERIAL_CANONICAL && kind != EGTB_MATERIAL_MIRROR)
        return false;
    return atomic_load_explicit(compressed_registry_slot(&canonical),
                                memory_order_acquire) != NULL;
}

void gwdegtb_wdl_compressed_unload_all(void)
{
    pthread_mutex_lock(&registry_mutex);
    for (unsigned wk = 0; wk < MATERIAL_DIMENSION; ++wk)
        for (unsigned wm = 0; wm < MATERIAL_DIMENSION; ++wm)
            for (unsigned bk = 0; bk < MATERIAL_DIMENSION; ++bk)
                for (unsigned bm = 0; bm < MATERIAL_DIMENSION; ++bm) {
                    _Atomic(CompressedWdl *) *slot =
                        &compressed_wdls[wk][wm][bk][bm];
                    CompressedWdl *compressed = atomic_exchange_explicit(
                        slot, NULL, memory_order_acq_rel);
                    destroy_compressed_wdl(compressed);
                }
    pthread_mutex_unlock(&registry_mutex);
}

bool gwdegtb_wdl_probe_create(size_t cache_bytes,
                              GwdegtbWdlProbe **out)
{
    GwdegtbWdlProbe *probe = NULL;
    size_t requested, capacity = 1;
    if (out == NULL || cache_bytes < sizeof(CompressedCacheEntry))
        return fail("compressed WDL probe cache is too small");
    *out = NULL;
    requested = cache_bytes / sizeof(CompressedCacheEntry);
    while (capacity <= requested / 2)
        capacity *= 2;
    probe = calloc(1, sizeof(*probe));
    if (probe == NULL)
        return fail("cannot allocate compressed WDL probe");
    probe->entries = calloc(capacity, sizeof(*probe->entries));
    probe->decompressor = ZSTD_createDCtx();
    if (probe->entries == NULL || probe->decompressor == NULL) {
        gwdegtb_wdl_probe_destroy(probe);
        return fail("cannot allocate compressed WDL probe cache");
    }
    probe->capacity = capacity;
    probe->mask = capacity - 1;
    probe->statistics.requested_cache_bytes = cache_bytes;
    probe->statistics.allocated_cache_bytes =
        capacity * sizeof(*probe->entries);
    probe->statistics.cache_entries = capacity;
    for (size_t entry = 0; entry < capacity; ++entry)
        probe->entries[entry].page = UINT64_MAX;
    *out = probe;
    return true;
}

void gwdegtb_wdl_probe_destroy(GwdegtbWdlProbe *probe)
{
    if (probe == NULL)
        return;
    ZSTD_freeDCtx(probe->decompressor);
    free(probe->entries);
    free(probe);
}

static size_t compressed_cache_index(const GwdegtbWdlProbe *probe,
                                     const CompressedWdl *database,
                                     uint64_t page)
{
    uint64_t database_offset =
        (uint64_t)database->material_id * UINT64_C(0x9e3779b97f4a7c15);
    return (size_t)(database_offset + page) & probe->mask;
}

static bool load_compressed_cache_page(GwdegtbWdlProbe *probe,
                                       const CompressedWdl *database,
                                       uint64_t page,
                                       CompressedCacheEntry *entry)
{
    const void *compressed;
    size_t compressed_size, decompressed;
    uint32_t checksum;
    if (!wdl_image_page(database->image, page, &compressed,
                        &compressed_size, &checksum))
        return fail("cannot read compressed WDL directory: %s",
                    wdl_last_error());
    (void)checksum;
    if (compressed == NULL) {
        memset(entry->data, 0, sizeof(entry->data));
    } else {
        decompressed = ZSTD_decompressDCtx(
            probe->decompressor, entry->data, sizeof(entry->data),
            compressed, compressed_size);
        if (ZSTD_isError(decompressed) || decompressed != sizeof(entry->data))
            return fail("compressed WDL decompression failed for page %llu: %s",
                        (unsigned long long)page,
                        ZSTD_isError(decompressed)
                            ? ZSTD_getErrorName(decompressed)
                            : "invalid decompressed size");
        if (!wdl_image_validate_page(database->image, page, entry->data,
                                     sizeof(entry->data)))
            return fail("compressed WDL page validation failed: %s",
                        wdl_last_error());
        ++probe->statistics.decompressions;
    }
    entry->database = database;
    entry->page = page;
    return true;
}

int16_t gwdegtb_wdl_lookup_probe_compact(GwdegtbWdlProbe *probe,
                                         uint64_t white_kings,
                                         uint64_t white_men,
                                         uint64_t black_kings,
                                         uint64_t black_men,
                                         GwdegtbSide side)
{
    DraughtsPosition position;
    EgPosition indexed_position;
    EgtbMaterial canonical;
    CompressedWdl *database;
    CompressedCacheEntry *entry;
    EgtbSide lookup_side;
    uint64_t index, page;
    uint32_t position_in_page;
    unsigned char packed, value;
    if (probe == NULL || !prepare_compact_position(
            white_kings, white_men, black_kings, black_men, side,
            &position, &lookup_side) ||
        !resolve_compact_material(&position, &canonical, &lookup_side))
        return GWDEGTB_WDL_UNAVAILABLE;
    database = atomic_load_explicit(compressed_registry_slot(&canonical),
                                    memory_order_acquire);
    if (database == NULL)
        return GWDEGTB_WDL_UNAVAILABLE;
    indexed_position.white_men = position.white_men;
    indexed_position.black_men = position.black_men;
    indexed_position.white_kings = position.white_kings;
    indexed_position.black_kings = position.black_kings;
    if (!eg_position_to_index(&database->indexer, &indexed_position, &index))
        return GWDEGTB_WDL_UNAVAILABLE;
    ++probe->statistics.lookups;
    page = index / (WDL_PAGE_SIZE * 2);
    position_in_page = (uint32_t)(index % (WDL_PAGE_SIZE * 2));
    entry = &probe->entries[
        compressed_cache_index(probe, database, page)];
    if (entry->database == database && entry->page == page) {
        ++probe->statistics.hits;
    } else {
        ++probe->statistics.misses;
        if (!load_compressed_cache_page(probe, database, page, entry))
            return GWDEGTB_WDL_UNAVAILABLE;
    }
    packed = entry->data[position_in_page / 2];
    value = (packed >> ((position_in_page % 2) * 4 + lookup_side * 2)) & 3;
    if (value == WDL_WIN)
        return GWDEGTB_WDL_WIN;
    if (value == WDL_LOSS)
        return GWDEGTB_WDL_LOSS;
    if (value == WDL_DRAW)
        return GWDEGTB_WDL_DRAW;
    fail("invalid compressed WDL value at index %llu",
         (unsigned long long)index);
    return GWDEGTB_WDL_UNAVAILABLE;
}

int16_t gwdegtb_wdl_lookup_probe(GwdegtbWdlProbe *probe,
                                 uint64_t white_kings,
                                 uint64_t white_men,
                                 uint64_t black_kings,
                                 uint64_t black_men,
                                 GwdegtbSide side)
{
    uint64_t all = white_kings | white_men | black_kings | black_men;
    if ((all & ~GWDEGTB_GWD_BOARD_MASK) != 0)
        return GWDEGTB_WDL_UNAVAILABLE;
    return gwdegtb_wdl_lookup_probe_compact(
        probe, gwdegtb_gwd_to_compact(white_kings),
        gwdegtb_gwd_to_compact(white_men),
        gwdegtb_gwd_to_compact(black_kings),
        gwdegtb_gwd_to_compact(black_men), side);
}

void gwdegtb_wdl_probe_statistics(
    const GwdegtbWdlProbe *probe,
    GwdegtbWdlProbeStatistics *statistics)
{
    if (statistics == NULL)
        return;
    if (probe == NULL)
        memset(statistics, 0, sizeof(*statistics));
    else
        *statistics = probe->statistics;
}

static void destroy_disk_dtm(DiskDtm *dtm)
{
    if (dtm == NULL)
        return;
    if (dtm->database != NULL)
        egtb_close(dtm->database);
    eg_indexer_destroy(&dtm->indexer);
    free(dtm);
}

static DiskDtm *open_disk_dtm(const char *directory,
                              const EgtbMaterial *canonical,
                              size_t cache_bytes)
{
    DiskDtm *dtm = NULL;
    char *path = NULL;
    uint32_t page_bytes;
    size_t cache_pages;

    dtm = calloc(1, sizeof(*dtm));
    if (dtm == NULL) {
        fail("cannot allocate on-disk DTM registry entry");
        return NULL;
    }
    if (!eg_indexer_init(&dtm->indexer,
                         canonical->white_men, canonical->black_men,
                         canonical->white_kings, canonical->black_kings)) {
        fail("cannot initialize on-disk DTM indexer");
        goto done;
    }
    path = database_path(directory, canonical, "dtm");
    if (path == NULL) {
        fail("cannot allocate on-disk DTM path");
        goto done;
    }
    if (!egtb_open_readonly(&dtm->database, path, 2)) {
        fail("cannot open %s: %s", path, egtb_last_error());
        goto done;
    }
    if (egtb_maximum_index(dtm->database) != eg_max_index(&dtm->indexer)) {
        fail("DTM maximum index does not match material for %s", path);
        goto done;
    }
    page_bytes = egtb_cache_page_size(dtm->database);
    cache_pages = cache_bytes / page_bytes;
    if (cache_pages < 2)
        cache_pages = 2;
    if (!egtb_resize_cache(dtm->database, cache_pages)) {
        fail("cannot size DTM cache for %s: %s", path, egtb_last_error());
        goto done;
    }

    free(path);
    return dtm;

done:
    free(path);
    destroy_disk_dtm(dtm);
    return NULL;
}

static DiskDtm *find_or_open_disk_dtm(const char *directory,
                                      const EgtbMaterial *canonical,
                                      size_t cache_bytes)
{
    _Atomic(DiskDtm *) *slot = dtm_registry_slot(canonical);
    bool *attempted =
        &dtm_open_attempted[canonical->white_kings][canonical->white_men]
                           [canonical->black_kings][canonical->black_men];
    DiskDtm *dtm = atomic_load_explicit(slot, memory_order_acquire);
    if (dtm != NULL)
        return dtm;

    pthread_mutex_lock(&registry_mutex);
    dtm = atomic_load_explicit(slot, memory_order_relaxed);
    if (dtm == NULL && !*attempted) {
        *attempted = true;
        dtm = open_disk_dtm(directory, canonical, cache_bytes);
        if (dtm != NULL)
            atomic_store_explicit(slot, dtm, memory_order_release);
    }
    pthread_mutex_unlock(&registry_mutex);
    return dtm;
}

void gwdegtb_dtm_close_all(void)
{
    unsigned wk, wm, bk, bm;
    pthread_mutex_lock(&registry_mutex);
    for (wk = 0; wk < MATERIAL_DIMENSION; ++wk)
        for (wm = 0; wm < MATERIAL_DIMENSION; ++wm)
            for (bk = 0; bk < MATERIAL_DIMENSION; ++bk)
                for (bm = 0; bm < MATERIAL_DIMENSION; ++bm) {
                    _Atomic(DiskDtm *) *slot =
                        &disk_dtms[wk][wm][bk][bm];
                    DiskDtm *dtm = atomic_exchange_explicit(
                        slot, NULL, memory_order_acq_rel);
                    destroy_disk_dtm(dtm);
                }
    memset(dtm_open_attempted, 0, sizeof(dtm_open_attempted));
    pthread_mutex_unlock(&registry_mutex);
}

int16_t gwdegtb_dtm_lookup_compact(
    const char *directory, size_t cache_bytes,
    uint64_t white_kings, uint64_t white_men,
    uint64_t black_kings, uint64_t black_men,
    GwdegtbSide side)
{
    DraughtsPosition position;
    EgPosition indexed_position;
    EgtbMaterial canonical;
    DiskDtm *dtm;
    EgtbSide lookup_side;
    uint64_t index;
    int16_t value;

    if (!prepare_compact_position(
            white_kings, white_men, black_kings, black_men, side,
            &position, &lookup_side))
        return GWDEGTB_DTM_UNAVAILABLE;
    if ((side == GWDEGTB_WHITE_TO_MOVE &&
         (white_kings | white_men) == 0) ||
        (side == GWDEGTB_BLACK_TO_MOVE &&
         (black_kings | black_men) == 0))
        return 0;
    if (cache_bytes == 0)
        return GWDEGTB_DTM_UNAVAILABLE;
    if (!resolve_compact_material(&position, &canonical, &lookup_side))
        return GWDEGTB_DTM_UNAVAILABLE;
    dtm = find_or_open_disk_dtm(directory, &canonical, cache_bytes);
    if (dtm == NULL)
        return GWDEGTB_DTM_UNAVAILABLE;
    indexed_position.white_men = position.white_men;
    indexed_position.black_men = position.black_men;
    indexed_position.white_kings = position.white_kings;
    indexed_position.black_kings = position.black_kings;
    if (!eg_position_to_index(&dtm->indexer, &indexed_position, &index))
        return GWDEGTB_DTM_UNAVAILABLE;
    if (!egtb_get(dtm->database, index, lookup_side, &value)) {
        fail("cannot read on-disk DTM: %s", egtb_last_error());
        return GWDEGTB_DTM_UNAVAILABLE;
    }
    return value;
}

int16_t gwdegtb_dtm_lookup(const char *directory, size_t cache_bytes,
                           uint64_t white_kings, uint64_t white_men,
                           uint64_t black_kings, uint64_t black_men,
                           GwdegtbSide side)
{
    uint64_t all = white_kings | white_men | black_kings | black_men;
    if ((all & ~GWDEGTB_GWD_BOARD_MASK) != 0)
        return GWDEGTB_DTM_UNAVAILABLE;
    return gwdegtb_dtm_lookup_compact(
        directory, cache_bytes,
        gwdegtb_gwd_to_compact(white_kings),
        gwdegtb_gwd_to_compact(white_men),
        gwdegtb_gwd_to_compact(black_kings),
        gwdegtb_gwd_to_compact(black_men), side);
}
