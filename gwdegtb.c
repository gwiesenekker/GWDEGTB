#define _POSIX_C_SOURCE 200809L

#include "gwdegtb.h"

#include "endgame_index.h"
#include "material.h"
#include "wdl.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#define GWDEGTB_HAVE_BMI2 1
#else
#define GWDEGTB_HAVE_BMI2 0
#endif

#define MATERIAL_DIMENSION (EGTB_MAX_PIECES + 1)
#define DEFAULT_WDL_COMPRESSION_LEVEL 3
#define DEFAULT_DTM_CACHE_PAGES 16384

typedef struct {
    unsigned char *bitmap;
    size_t bytes;
    EgIndexer indexer;
} ResidentWdl;

static _Atomic(ResidentWdl *)
    resident_wdls[MATERIAL_DIMENSION][MATERIAL_DIMENSION]
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

static char *database_path(const char *directory,
                           const EgtbMaterial *material)
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
                                material->black_men, "wdl"))
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
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;
    Wdl *wdl = NULL;
    char *path = NULL;
    uint64_t maximum_index;
    size_t expected_size;
    bool ok = false;

    if (data == NULL ||
        !parse_database_name(database_name, &requested, &canonical, &kind) ||
        !material_info(&canonical, &maximum_index, &expected_size))
        return false;
    if (size != expected_size)
        return fail("resident WDL buffer is %zu bytes; expected %zu",
                    size, expected_size);
    path = database_path(directory, &canonical);
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
    if (!wdl_decompress_into(wdl, data, size)) {
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

int16_t gwdegtb_wdl_lookup(uint64_t white_kings, uint64_t white_men,
                           uint64_t black_kings, uint64_t black_men,
                           GwdegtbSide side)
{
    uint64_t all = white_kings | white_men | black_kings | black_men;
    DraughtsPosition position, canonical_position;
    EgPosition indexed_position;
    EgtbMaterial requested, canonical;
    EgtbMaterialKind kind;
    ResidentWdl *resident;
    EgtbSide lookup_side;
    uint64_t index;
    unsigned char packed, value;

    if ((side != GWDEGTB_WHITE_TO_MOVE &&
         side != GWDEGTB_BLACK_TO_MOVE) ||
        (all & ~GWDEGTB_GWD_BOARD_MASK) != 0 ||
        ((white_kings & white_men) | (white_kings & black_kings) |
         (white_kings & black_men) | (white_men & black_kings) |
         (white_men & black_men) | (black_kings & black_men)) != 0)
        return GWDEGTB_WDL_UNAVAILABLE;
    lookup_side = side == GWDEGTB_WHITE_TO_MOVE
                      ? EGTB_WHITE_TO_MOVE : EGTB_BLACK_TO_MOVE;

    position.white_kings = gwdegtb_gwd_to_compact(white_kings);
    position.white_men = gwdegtb_gwd_to_compact(white_men);
    position.black_kings = gwdegtb_gwd_to_compact(black_kings);
    position.black_men = gwdegtb_gwd_to_compact(black_men);
    if ((position.white_men & UINT64_C(0x1f)) != 0 ||
        (position.black_men & (UINT64_C(0x1f) << 45)) != 0)
        return GWDEGTB_WDL_UNAVAILABLE;

    requested.white_kings = popcount(white_kings);
    requested.white_men = popcount(white_men);
    requested.black_kings = popcount(black_kings);
    requested.black_men = popcount(black_men);
    kind = egtb_material_resolve(&requested, &canonical);
    if (kind != EGTB_MATERIAL_CANONICAL && kind != EGTB_MATERIAL_MIRROR)
        return GWDEGTB_WDL_UNAVAILABLE;
    resident = atomic_load_explicit(registry_slot(&canonical),
                                    memory_order_acquire);
    if (resident == NULL)
        return GWDEGTB_WDL_UNAVAILABLE;

    if (kind == EGTB_MATERIAL_MIRROR) {
        egtb_mirror_position(&position, &canonical_position);
        position = canonical_position;
        lookup_side = egtb_mirror_side(lookup_side);
    }
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
