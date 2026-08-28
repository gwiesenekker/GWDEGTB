#include "combinatorial_index.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#define COMB_HAVE_BMI2 1
#else
#define COMB_HAVE_BMI2 0
#endif

enum {
    COMB_WHITE_MAN,
    COMB_BLACK_MAN,
    COMB_WHITE_KING,
    COMB_BLACK_KING,
    COMB_MAX_PIECES = 7
};

#define TOP_MASK UINT64_C(0x1f)
#define MIDDLE_MASK (((UINT64_C(1) << 40) - 1) << 5)
#define BOTTOM_MASK (UINT64_C(0x1f) << 45)
#define BOARD_MASK ((UINT64_C(1) << 50) - 1)

static uint64_t choose_table[51][COMB_MAX_PIECES + 1];
static atomic_uint choose_state;
static const uint64_t factorial[COMB_MAX_PIECES + 1] = {
    1, 1, 2, 6, 24, 120, 720, 5040
};

static void initialize_choose(void)
{
    unsigned n, k;
    unsigned expected = 0;
    if (atomic_load_explicit(&choose_state, memory_order_acquire) == 2)
        return;
    if (!atomic_compare_exchange_strong_explicit(
            &choose_state, &expected, 1, memory_order_acq_rel,
            memory_order_acquire)) {
        while (atomic_load_explicit(&choose_state, memory_order_acquire) != 2)
            ;
        return;
    }
    choose_table[0][0] = 1;
    for (n = 1; n <= 50; ++n) {
        choose_table[n][0] = 1;
        for (k = 1; k <= COMB_MAX_PIECES; ++k) {
            uint64_t left = choose_table[n - 1][k - 1];
            uint64_t right = choose_table[n - 1][k];
            choose_table[n][k] = UINT64_MAX - left < right
                                     ? UINT64_MAX : left + right;
        }
    }
    atomic_store_explicit(&choose_state, 2, memory_order_release);
}

static uint64_t multinomial(const uint8_t counts[COMB_PIECE_COUNT])
{
    unsigned piece;
    unsigned total = 0;
    uint64_t result;
    for (piece = 0; piece < COMB_PIECE_COUNT; ++piece)
        total += counts[piece];
    result = factorial[total];
    for (piece = 0; piece < COMB_PIECE_COUNT; ++piece)
        result /= factorial[counts[piece]];
    return result;
}

static uint64_t compress_bits(uint64_t value, uint64_t mask)
{
#if COMB_HAVE_BMI2
    return _pext_u64(value, mask);
#else
    uint64_t compressed = 0;
    uint64_t output = 1;
    while (mask != 0) {
        uint64_t bit = mask & (UINT64_C(0) - mask);
        if ((value & bit) != 0)
            compressed |= output;
        output <<= 1;
        mask &= mask - 1;
    }
    return compressed;
#endif
}

static uint64_t deposit_bits(uint64_t value, uint64_t mask)
{
#if COMB_HAVE_BMI2
    return _pdep_u64(value, mask);
#else
    uint64_t deposited = 0;
    uint64_t input = 1;
    while (mask != 0) {
        uint64_t bit = mask & (UINT64_C(0) - mask);
        if ((value & input) != 0)
            deposited |= bit;
        input <<= 1;
        mask &= mask - 1;
    }
    return deposited;
#endif
}

typedef struct {
    CombLabelScheme *scheme;
    uint8_t remaining[COMB_PIECE_COUNT];
    uint16_t next_rank;
} LabelBuild;

static void build_label_codes(LabelBuild *build, unsigned position,
                              uint16_t black, uint16_t kings)
{
    unsigned piece;
    if (position == build->scheme->occupied) {
        uint16_t code = black | (uint16_t)(kings << build->scheme->occupied);
        build->scheme->rank_by_code[code] = build->next_rank;
        build->scheme->code_by_rank[build->next_rank++] = code;
        return;
    }
    for (piece = 0; piece < COMB_PIECE_COUNT; ++piece) {
        uint16_t bit = (uint16_t)(UINT16_C(1) << position);
        if (build->remaining[piece] == 0)
            continue;
        --build->remaining[piece];
        build_label_codes(build, position + 1,
                          piece == COMB_BLACK_MAN ||
                                  piece == COMB_BLACK_KING
                              ? (uint16_t)(black | bit) : black,
                          piece == COMB_WHITE_KING ||
                                  piece == COMB_BLACK_KING
                              ? (uint16_t)(kings | bit) : kings);
        ++build->remaining[piece];
    }
}

static bool get_label_scheme(CombinatorialIndexer *indexer,
                             const uint8_t counts[COMB_PIECE_COUNT],
                             uint16_t *scheme_index)
{
    size_t index;
    unsigned piece;
    for (index = 0; index < indexer->label_scheme_count; ++index) {
        if (memcmp(indexer->label_schemes[index].counts, counts,
                   COMB_PIECE_COUNT) == 0) {
            *scheme_index = (uint16_t)index;
            return true;
        }
    }
    if (indexer->label_scheme_count == indexer->label_scheme_capacity ||
        indexer->label_scheme_count > UINT16_MAX)
        return false;
    {
        CombLabelScheme *scheme =
            &indexer->label_schemes[indexer->label_scheme_count];
        LabelBuild build;
        size_t code_count;
        memset(scheme, 0, sizeof(*scheme));
        memcpy(scheme->counts, counts, COMB_PIECE_COUNT);
        for (piece = 0; piece < COMB_PIECE_COUNT; ++piece)
            scheme->occupied += counts[piece];
        scheme->label_count = (uint16_t)multinomial(counts);
        code_count = (size_t)1 << (2 * scheme->occupied);
        scheme->rank_by_code = malloc(code_count * sizeof(*scheme->rank_by_code));
        scheme->code_by_rank =
            malloc((size_t)scheme->label_count * sizeof(*scheme->code_by_rank));
        if (scheme->rank_by_code == NULL || scheme->code_by_rank == NULL)
            return false;
        memset(scheme->rank_by_code, 0xff,
               code_count * sizeof(*scheme->rank_by_code));
        memset(&build, 0, sizeof(build));
        build.scheme = scheme;
        memcpy(build.remaining, counts, COMB_PIECE_COUNT);
        build_label_codes(&build, 0, 0, 0);
        if (build.next_rank != scheme->label_count)
            return false;
    }
    *scheme_index = (uint16_t)indexer->label_scheme_count++;
    return true;
}

static uint64_t arrangement_count(unsigned squares,
                                  const uint8_t counts[COMB_PIECE_COUNT],
                                  uint16_t *labels)
{
    unsigned piece;
    unsigned occupied = 0;
    uint64_t label_count;
    for (piece = 0; piece < COMB_PIECE_COUNT; ++piece)
        occupied += counts[piece];
    label_count = multinomial(counts);
    *labels = (uint16_t)label_count;
    return choose_table[squares][occupied] * label_count;
}

static size_t distribution_key(const CombinatorialIndexer *indexer,
                               unsigned top_bm, unsigned top_wk,
                               unsigned top_bk, unsigned bottom_wm,
                               unsigned bottom_wk, unsigned bottom_bk)
{
    size_t key = top_bm;
    key = key * (indexer->white_kings + 1) + top_wk;
    key = key * (indexer->black_kings + 1) + top_bk;
    key = key * (indexer->white_men + 1) + bottom_wm;
    key = key * (indexer->white_kings + 1) + bottom_wk;
    key = key * (indexer->black_kings + 1) + bottom_bk;
    return key;
}

static bool append_distribution(CombinatorialIndexer *indexer,
                                size_t capacity,
                                unsigned top_bm, unsigned top_wk,
                                unsigned top_bk, unsigned bottom_wm,
                                unsigned bottom_wk, unsigned bottom_bk)
{
    CombDistribution *distribution;
    unsigned region;
    uint64_t size = 1;
    size_t key;
    if (indexer->distribution_count == capacity)
        return false;
    distribution = &indexer->distributions[indexer->distribution_count];
    memset(distribution, 0, sizeof(*distribution));

    distribution->counts[COMB_REGION_TOP][COMB_BLACK_MAN] =
        (uint8_t)top_bm;
    distribution->counts[COMB_REGION_TOP][COMB_WHITE_KING] =
        (uint8_t)top_wk;
    distribution->counts[COMB_REGION_TOP][COMB_BLACK_KING] =
        (uint8_t)top_bk;
    distribution->counts[COMB_REGION_BOTTOM][COMB_WHITE_MAN] =
        (uint8_t)bottom_wm;
    distribution->counts[COMB_REGION_BOTTOM][COMB_WHITE_KING] =
        (uint8_t)bottom_wk;
    distribution->counts[COMB_REGION_BOTTOM][COMB_BLACK_KING] =
        (uint8_t)bottom_bk;

    distribution->counts[COMB_REGION_MIDDLE][COMB_WHITE_MAN] =
        (uint8_t)(indexer->white_men - bottom_wm);
    distribution->counts[COMB_REGION_MIDDLE][COMB_BLACK_MAN] =
        (uint8_t)(indexer->black_men - top_bm);
    distribution->counts[COMB_REGION_MIDDLE][COMB_WHITE_KING] =
        (uint8_t)(indexer->white_kings - top_wk - bottom_wk);
    distribution->counts[COMB_REGION_MIDDLE][COMB_BLACK_KING] =
        (uint8_t)(indexer->black_kings - top_bk - bottom_bk);

    distribution->offset = indexer->position_count;
    for (region = 0; region < COMB_REGION_COUNT; ++region) {
        unsigned squares = region == COMB_REGION_MIDDLE ? 40 : 5;
        distribution->arrangements[region] = arrangement_count(
            squares, distribution->counts[region],
            &distribution->label_count[region]);
        if (!get_label_scheme(indexer, distribution->counts[region],
                              &distribution->label_scheme[region]))
            return false;
        if (distribution->arrangements[region] == 0 ||
            size > UINT64_MAX / distribution->arrangements[region])
            return false;
        size *= distribution->arrangements[region];
    }
    if (UINT64_MAX - indexer->position_count < size)
        return false;
    distribution->size = size;
    indexer->position_count += size;
    key = distribution_key(indexer, top_bm, top_wk, top_bk,
                           bottom_wm, bottom_wk, bottom_bk);
    if (key >= indexer->key_count ||
        indexer->distribution_by_key[key] != UINT32_MAX)
        return false;
    indexer->distribution_by_key[key] =
        (uint32_t)indexer->distribution_count;
    ++indexer->distribution_count;
    return true;
}

bool comb_indexer_init(CombinatorialIndexer *indexer,
                       unsigned wm, unsigned bm, unsigned wk, unsigned bk)
{
    size_t capacity;
    size_t key_count;
    unsigned top_bm, top_wk, top_bk, bottom_wm, bottom_wk, bottom_bk;
    if (indexer == NULL)
        return false;
    memset(indexer, 0, sizeof(*indexer));
    if (wm + bm + wk + bk > COMB_MAX_PIECES)
        return false;
    initialize_choose();
    indexer->white_men = wm;
    indexer->black_men = bm;
    indexer->white_kings = wk;
    indexer->black_kings = bk;

    capacity = (size_t)(bm + 1) * (wk + 1) * (bk + 1) *
               (wm + 1) * (wk + 1) * (bk + 1);
    key_count = capacity;
    if (capacity == 0 || capacity > SIZE_MAX / sizeof(*indexer->distributions) ||
        key_count > SIZE_MAX / sizeof(*indexer->distribution_by_key))
        return false;
    indexer->distributions = calloc(capacity, sizeof(*indexer->distributions));
    indexer->label_scheme_capacity = capacity * COMB_REGION_COUNT;
    indexer->label_schemes =
        calloc(indexer->label_scheme_capacity, sizeof(*indexer->label_schemes));
    indexer->distribution_by_key =
        malloc(key_count * sizeof(*indexer->distribution_by_key));
    if (indexer->distributions == NULL || indexer->label_schemes == NULL ||
        indexer->distribution_by_key == NULL) {
        comb_indexer_destroy(indexer);
        return false;
    }
    indexer->key_count = key_count;
    memset(indexer->distribution_by_key, 0xff,
           key_count * sizeof(*indexer->distribution_by_key));

    for (top_bm = 0; top_bm <= bm; ++top_bm)
    for (top_wk = 0; top_wk <= wk; ++top_wk)
    for (top_bk = 0; top_bk <= bk; ++top_bk) {
        if (top_bm + top_wk + top_bk > 5)
            continue;
        for (bottom_wm = 0; bottom_wm <= wm; ++bottom_wm)
        for (bottom_wk = 0; bottom_wk + top_wk <= wk; ++bottom_wk)
        for (bottom_bk = 0; bottom_bk + top_bk <= bk; ++bottom_bk) {
            if (bottom_wm + bottom_wk + bottom_bk > 5)
                continue;
            if (!append_distribution(indexer, capacity, top_bm, top_wk,
                                     top_bk, bottom_wm, bottom_wk,
                                     bottom_bk)) {
                comb_indexer_destroy(indexer);
                return false;
            }
        }
    }
    return indexer->position_count != 0;
}

void comb_indexer_destroy(CombinatorialIndexer *indexer)
{
    if (indexer == NULL)
        return;
    free(indexer->distributions);
    if (indexer->label_schemes != NULL) {
        size_t scheme;
        for (scheme = 0; scheme < indexer->label_scheme_capacity; ++scheme) {
            free(indexer->label_schemes[scheme].rank_by_code);
            free(indexer->label_schemes[scheme].code_by_rank);
        }
    }
    free(indexer->label_schemes);
    free(indexer->distribution_by_key);
    memset(indexer, 0, sizeof(*indexer));
}

uint64_t comb_position_count(const CombinatorialIndexer *indexer)
{
    return indexer == NULL ? 0 : indexer->position_count;
}

static uint64_t combination_rank(uint64_t bits)
{
    uint64_t rank = 0;
    unsigned selected = 1;
    while (bits != 0) {
        unsigned square = (unsigned)__builtin_ctzll(bits);
        rank += choose_table[square][selected++];
        bits &= bits - 1;
    }
    return rank;
}

static uint64_t combination_unrank(unsigned squares, unsigned occupied,
                                   uint64_t rank)
{
    uint64_t bits = 0;
    unsigned limit = squares;
    while (occupied != 0) {
        unsigned square = limit - 1;
        while (choose_table[square][occupied] > rank)
            --square;
        bits |= UINT64_C(1) << square;
        rank -= choose_table[square][occupied];
        limit = square;
        --occupied;
    }
    return bits;
}

static uint64_t label_rank(const CombinatorialIndexer *indexer,
                           const CombDistribution *distribution,
                           unsigned region, uint64_t occupied,
                           uint64_t black, uint64_t kings)
{
    const CombLabelScheme *scheme =
        &indexer->label_schemes[distribution->label_scheme[region]];
    uint64_t black_code = compress_bits(black, occupied);
    uint64_t king_code = compress_bits(kings, occupied);
    uint64_t code = black_code | (king_code << scheme->occupied);
    return scheme->rank_by_code[code];
}

static void label_unrank(const CombinatorialIndexer *indexer,
                         const CombDistribution *distribution,
                         unsigned region, uint64_t occupied, uint64_t rank,
                         uint64_t pieces[COMB_PIECE_COUNT])
{
    const CombLabelScheme *scheme =
        &indexer->label_schemes[distribution->label_scheme[region]];
    uint64_t code = scheme->code_by_rank[rank];
    uint64_t compressed_mask =
        (UINT64_C(1) << scheme->occupied) - 1;
    uint64_t black = deposit_bits(code & compressed_mask, occupied);
    uint64_t kings = deposit_bits(code >> scheme->occupied, occupied);
    pieces[COMB_WHITE_MAN] |= occupied & ~black & ~kings;
    pieces[COMB_BLACK_MAN] |= occupied & black & ~kings;
    pieces[COMB_WHITE_KING] |= occupied & ~black & kings;
    pieces[COMB_BLACK_KING] |= occupied & black & kings;
}

static uint64_t region_rank(const CombinatorialIndexer *indexer,
                            uint64_t occupied, uint64_t black,
                            uint64_t kings,
                            const CombDistribution *distribution,
                            unsigned region)
{
    return combination_rank(occupied) * distribution->label_count[region] +
           label_rank(indexer, distribution, region, occupied, black, kings);
}

bool comb_position_to_index(const CombinatorialIndexer *indexer,
                            const EgPosition *position, uint64_t *index)
{
    uint64_t top_occupied, middle_occupied, bottom_occupied;
    uint64_t top_black, middle_black, bottom_black;
    uint64_t top_kings, middle_kings, bottom_kings;
    uint64_t black, kings, occupied;
    size_t key;
    uint32_t distribution_index;
    const CombDistribution *distribution;
    uint64_t rank;
#ifndef NDEBUG
    if (indexer == NULL || position == NULL || index == NULL)
        return false;
#endif
    occupied = position->white_men | position->black_men |
               position->white_kings | position->black_kings;
    black = position->black_men | position->black_kings;
    kings = position->white_kings | position->black_kings;
    top_occupied = occupied & TOP_MASK;
    middle_occupied = (occupied & MIDDLE_MASK) >> 5;
    bottom_occupied = (occupied & BOTTOM_MASK) >> 45;
    top_black = black & TOP_MASK;
    middle_black = (black & MIDDLE_MASK) >> 5;
    bottom_black = (black & BOTTOM_MASK) >> 45;
    top_kings = kings & TOP_MASK;
    middle_kings = (kings & MIDDLE_MASK) >> 5;
    bottom_kings = (kings & BOTTOM_MASK) >> 45;

    key = distribution_key(
        indexer,
        (unsigned)__builtin_popcountll(position->black_men & TOP_MASK),
        (unsigned)__builtin_popcountll(position->white_kings & TOP_MASK),
        (unsigned)__builtin_popcountll(position->black_kings & TOP_MASK),
        (unsigned)__builtin_popcountll(position->white_men & BOTTOM_MASK),
        (unsigned)__builtin_popcountll(position->white_kings & BOTTOM_MASK),
        (unsigned)__builtin_popcountll(position->black_kings & BOTTOM_MASK));
#ifndef NDEBUG
    if (key >= indexer->key_count)
        return false;
#endif
    distribution_index = indexer->distribution_by_key[key];
#ifndef NDEBUG
    if (distribution_index == UINT32_MAX)
        return false;
#endif
    distribution = &indexer->distributions[distribution_index];
    rank = region_rank(indexer, top_occupied, top_black, top_kings,
                       distribution, COMB_REGION_TOP);
    rank = rank * distribution->arrangements[COMB_REGION_MIDDLE] +
           region_rank(indexer, middle_occupied, middle_black, middle_kings,
                       distribution, COMB_REGION_MIDDLE);
    rank = rank * distribution->arrangements[COMB_REGION_BOTTOM] +
           region_rank(indexer, bottom_occupied, bottom_black, bottom_kings,
                       distribution, COMB_REGION_BOTTOM);
    *index = distribution->offset + rank;
    return true;
}

static const CombDistribution *find_distribution(
    const CombinatorialIndexer *indexer, uint64_t index)
{
    size_t low = 0;
    size_t high = indexer->distribution_count;
    while (low + 1 < high) {
        size_t middle = low + (high - low) / 2;
        if (indexer->distributions[middle].offset <= index)
            low = middle;
        else
            high = middle;
    }
    return &indexer->distributions[low];
}

static void region_unrank(const CombinatorialIndexer *indexer, uint64_t rank,
                          const CombDistribution *distribution,
                          unsigned region, unsigned shift,
                          uint64_t pieces[COMB_PIECE_COUNT])
{
    const uint8_t *counts = distribution->counts[region];
    uint64_t occupied;
    uint64_t local_pieces[COMB_PIECE_COUNT] = {0, 0, 0, 0};
    unsigned piece;
    unsigned occupied_count = counts[0] + counts[1] + counts[2] + counts[3];
    uint64_t combination = rank / distribution->label_count[region];
    uint64_t labels = rank % distribution->label_count[region];
    unsigned squares = region == COMB_REGION_MIDDLE ? 40 : 5;
    occupied = combination_unrank(squares, occupied_count, combination);
    label_unrank(indexer, distribution, region, occupied, labels,
                 local_pieces);
    for (piece = 0; piece < COMB_PIECE_COUNT; ++piece)
        pieces[piece] |= local_pieces[piece] << shift;
}

bool comb_index_to_position(const CombinatorialIndexer *indexer,
                            uint64_t index, EgPosition *position)
{
    const CombDistribution *distribution;
    uint64_t pieces[COMB_PIECE_COUNT] = {0, 0, 0, 0};
    uint64_t local, top_rank, middle_rank, bottom_rank;
#ifndef NDEBUG
    if (indexer == NULL || position == NULL ||
        index >= indexer->position_count)
        return false;
#endif
    distribution = find_distribution(indexer, index);
    local = index - distribution->offset;
    bottom_rank = local % distribution->arrangements[COMB_REGION_BOTTOM];
    local /= distribution->arrangements[COMB_REGION_BOTTOM];
    middle_rank = local % distribution->arrangements[COMB_REGION_MIDDLE];
    top_rank = local / distribution->arrangements[COMB_REGION_MIDDLE];
    region_unrank(indexer, top_rank, distribution, COMB_REGION_TOP, 0, pieces);
    region_unrank(indexer, middle_rank, distribution, COMB_REGION_MIDDLE, 5,
                  pieces);
    region_unrank(indexer, bottom_rank, distribution, COMB_REGION_BOTTOM, 45,
                  pieces);
    position->white_men = pieces[COMB_WHITE_MAN];
    position->black_men = pieces[COMB_BLACK_MAN];
    position->white_kings = pieces[COMB_WHITE_KING];
    position->black_kings = pieces[COMB_BLACK_KING];
    return true;
}
