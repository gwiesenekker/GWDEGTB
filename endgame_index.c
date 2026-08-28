#include "endgame_index.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum Piece {
    EMPTY = 0,
    WHITE_MAN,
    BLACK_MAN,
    WHITE_KING,
    BLACK_KING,
    PIECE_KIND_COUNT
};

#define BOARD_MASK ((UINT64_C(1) << 50) - 1)
#define BLACK_MAN_MASK ((UINT64_C(1) << 45) - 1)
#define WHITE_MAN_MASK (BOARD_MASK & ~((UINT64_C(1) << 5) - 1))

static uint64_t table_index(const EgIndexer *e, unsigned square,
                            unsigned wm, unsigned bm,
                            unsigned wk, unsigned bk)
{
    return (uint64_t)square * e->square_stride +
           (uint64_t)wm * e->piece_stride[0] +
           (uint64_t)bm * e->piece_stride[1] +
           (uint64_t)wk * e->piece_stride[2] + bk;
}

static bool allowed(enum Piece piece, unsigned square)
{
    switch (piece) {
    case EMPTY:      return true;
    case WHITE_MAN:  return square >= 5;
    case BLACK_MAN:  return square <= 44;
    case WHITE_KING:
    case BLACK_KING: return true;
    default:         return false;
    }
}

static unsigned *remaining_for(enum Piece piece, unsigned remaining[4])
{
    return piece == EMPTY ? NULL : &remaining[(unsigned)piece - 1];
}

bool eg_indexer_init(EgIndexer *e, unsigned wm, unsigned bm,
                     unsigned wk, unsigned bk)
{
    uint64_t states;
    unsigned square, a, b, c, d, piece_index;

    if (e == NULL)
        return false;
    memset(e, 0, sizeof(*e));
    if (wm > 45 || bm > 45 || wk > 50 || bk > 50 ||
        wm + bm + wk + bk > 50)
        return false;

    e->white_men = wm;
    e->black_men = bm;
    e->white_kings = wk;
    e->black_kings = bk;

    e->piece_stride[3] = 1;
    e->piece_stride[2] = (uint64_t)bk + 1;
    e->piece_stride[1] = ((uint64_t)wk + 1) * e->piece_stride[2];
    e->piece_stride[0] = ((uint64_t)bm + 1) * e->piece_stride[1];
    e->square_stride = ((uint64_t)wm + 1) * e->piece_stride[0];

    states = 51;
#define MULTIPLY_DIM(n) do {                                      \
        if (states > UINT64_MAX / ((uint64_t)(n) + 1)) return false; \
        states *= (uint64_t)(n) + 1;                              \
    } while (0)
    MULTIPLY_DIM(wm);
    MULTIPLY_DIM(bm);
    MULTIPLY_DIM(wk);
    MULTIPLY_DIM(bk);
#undef MULTIPLY_DIM
    if (states > SIZE_MAX / sizeof(*e->ways) ||
        states > SIZE_MAX / (4 * sizeof(*e->rank_add)))
        return false;

    e->ways = calloc((size_t)states, sizeof(*e->ways));
    e->rank_add = calloc((size_t)states * 4, sizeof(*e->rank_add));
    e->overflow = calloc((size_t)states, sizeof(*e->overflow));
    if (e->ways == NULL || e->rank_add == NULL || e->overflow == NULL) {
        eg_indexer_destroy(e);
        return false;
    }
    e->table_size = states;
    e->ways[table_index(e, 50, 0, 0, 0, 0)] = 1;

    for (square = 50; square-- > 0;) {
        for (a = 0; a <= wm; ++a)
        for (b = 0; b <= bm; ++b)
        for (c = 0; c <= wk; ++c)
        for (d = 0; d <= bk; ++d) {
            uint64_t out = table_index(e, square, a, b, c, d);
            enum Piece p;

            for (p = EMPTY; p < PIECE_KIND_COUNT; ++p) {
                unsigned next[4] = {a, b, c, d};
                unsigned *count = remaining_for(p, next);
                uint64_t in, add;
                if (!allowed(p, square) || (count != NULL && *count == 0))
                    continue;
                if (count != NULL)
                    --*count;
                in = table_index(e, square + 1, next[0], next[1],
                                 next[2], next[3]);
                add = e->ways[in];
                if (e->overflow[in] || UINT64_MAX - e->ways[out] < add) {
                    e->ways[out] = UINT64_MAX;
                    e->overflow[out] = 1;
                } else {
                    e->ways[out] += add;
                }
            }
        }
    }

    {
        uint64_t root = table_index(e, 0, wm, bm, wk, bk);
        if (e->overflow[root] || e->ways[root] == 0) {
            eg_indexer_destroy(e);
            return false;
        }
        e->position_count = e->ways[root];
    }

    for (square = 0; square < 50; ++square) {
        e->square_base[square] =
            ((uint64_t)square + 1) * e->square_stride;
        for (a = 0; a <= wm; ++a)
        for (b = 0; b <= bm; ++b)
        for (c = 0; c <= wk; ++c)
        for (d = 0; d <= bk; ++d) {
            uint64_t state = table_index(e, square + 1, a, b, c, d);
            uint64_t rank = e->ways[state];

            for (piece_index = 0; piece_index < 4; ++piece_index) {
                if (piece_index == 1 && square >= 5 && a != 0) {
                    uint64_t add = e->ways[state - e->piece_stride[0]];
                    rank = UINT64_MAX - rank < add ? UINT64_MAX : rank + add;
                }
                if (piece_index == 2 && square <= 44 && b != 0) {
                    uint64_t add = e->ways[state - e->piece_stride[1]];
                    rank = UINT64_MAX - rank < add ? UINT64_MAX : rank + add;
                }
                if (piece_index == 3 && c != 0) {
                    uint64_t add = e->ways[state - e->piece_stride[2]];
                    rank = UINT64_MAX - rank < add ? UINT64_MAX : rank + add;
                }
                e->rank_add[state * 4 + piece_index] = rank;
            }
        }
    }
    return true;
}

void eg_indexer_destroy(EgIndexer *e)
{
    if (e == NULL)
        return;
    free(e->ways);
    free(e->rank_add);
    free(e->overflow);
    memset(e, 0, sizeof(*e));
}

uint64_t eg_position_count(const EgIndexer *e)
{
    return e == NULL ? 0 : e->position_count;
}

uint64_t eg_max_index(const EgIndexer *e)
{
    return e == NULL || e->position_count == 0 ? 0 : e->position_count - 1;
}

#ifndef NDEBUG
static bool valid_position(const EgIndexer *e, const EgPosition *p)
{
    uint64_t occupied;
    if (p == NULL || ((p->white_men | p->black_men | p->white_kings |
                       p->black_kings) & ~BOARD_MASK) != 0)
        return false;
    occupied = p->white_men | p->black_men | p->white_kings | p->black_kings;
    if (__builtin_popcountll(occupied) !=
        __builtin_popcountll(p->white_men) +
        __builtin_popcountll(p->black_men) +
        __builtin_popcountll(p->white_kings) +
        __builtin_popcountll(p->black_kings))
        return false;
    return (p->white_men & ~WHITE_MAN_MASK) == 0 &&
           (p->black_men & ~BLACK_MAN_MASK) == 0 &&
           (unsigned)__builtin_popcountll(p->white_men) == e->white_men &&
           (unsigned)__builtin_popcountll(p->black_men) == e->black_men &&
           (unsigned)__builtin_popcountll(p->white_kings) == e->white_kings &&
           (unsigned)__builtin_popcountll(p->black_kings) == e->black_kings;
}
#endif

bool eg_position_to_index(const EgIndexer *e, const EgPosition *position,
                          uint64_t *index)
{
    uint64_t rank = 0;
    uint64_t remaining_state;
    uint64_t occupied;
    uint64_t black;
    uint64_t kings;

#ifndef NDEBUG
    if (e == NULL || e->ways == NULL || e->rank_add == NULL ||
        index == NULL || position == NULL ||
        !valid_position(e, position))
        return false;
#endif
    remaining_state =
        (uint64_t)e->white_men * e->piece_stride[0] +
        (uint64_t)e->black_men * e->piece_stride[1] +
        (uint64_t)e->white_kings * e->piece_stride[2] + e->black_kings;
    occupied = position->white_men | position->black_men |
               position->white_kings | position->black_kings;
    black = position->black_men | position->black_kings;
    kings = position->white_kings | position->black_kings;

    while (occupied != 0) {
        uint64_t bit = occupied & (UINT64_C(0) - occupied);
        unsigned square = (unsigned)__builtin_ctzll(occupied);
        unsigned piece_index =
            (unsigned)((black & bit) != 0) |
            ((unsigned)((kings & bit) != 0) << 1);
        uint64_t state = e->square_base[square] + remaining_state;

        rank += e->rank_add[state * 4 + piece_index];
        remaining_state -= e->piece_stride[piece_index];
        occupied &= occupied - 1;
    }
    *index = rank;
    return true;
}

bool eg_index_to_position(const EgIndexer *e, uint64_t index,
                          EgPosition *position)
{
    unsigned rem[4];
    uint64_t state;
    uint64_t bit = 1;
    uint64_t white_men = 0, black_men = 0;
    uint64_t white_kings = 0, black_kings = 0;
    unsigned pieces_left;
    unsigned square;

#ifndef NDEBUG
    if (e == NULL || e->ways == NULL || position == NULL ||
        index >= e->position_count)
        return false;
#endif
    rem[0] = e->white_men;
    rem[1] = e->black_men;
    rem[2] = e->white_kings;
    rem[3] = e->black_kings;
    pieces_left = rem[0] + rem[1] + rem[2] + rem[3];
    state = e->square_stride +
            (uint64_t)rem[0] * e->piece_stride[0] +
            (uint64_t)rem[1] * e->piece_stride[1] +
            (uint64_t)rem[2] * e->piece_stride[2] + rem[3];

    for (square = 0; square < 50; ++square, bit <<= 1) {
        uint64_t block = e->ways[state];
        enum Piece piece = EMPTY;

        if (index < block)
            goto selected;
        index -= block;
        if (square >= 5 && rem[0] != 0) {
            block = e->ways[state - e->piece_stride[0]];
            if (index < block) {
                piece = WHITE_MAN;
                goto selected;
            }
            index -= block;
        }
        if (square <= 44 && rem[1] != 0) {
            block = e->ways[state - e->piece_stride[1]];
            if (index < block) {
                piece = BLACK_MAN;
                goto selected;
            }
            index -= block;
        }
        if (rem[2] != 0) {
            block = e->ways[state - e->piece_stride[2]];
            if (index < block) {
                piece = WHITE_KING;
                goto selected;
            }
            index -= block;
        }
        if (rem[3] != 0) {
            block = e->ways[state - e->piece_stride[3]];
            if (index < block) {
                piece = BLACK_KING;
                goto selected;
            }
            index -= block;
        }

        return false; /* Unreachable for an in-range index. */

selected:
        state += e->square_stride;
        if (piece != EMPTY) {
            unsigned piece_index = (unsigned)piece - 1;
            --rem[piece_index];
            state -= e->piece_stride[piece_index];
            switch (piece) {
            case WHITE_MAN:  white_men |= bit; break;
            case BLACK_MAN:  black_men |= bit; break;
            case WHITE_KING: white_kings |= bit; break;
            case BLACK_KING: black_kings |= bit; break;
            default: break;
            }
            if (--pieces_left == 0)
                break;
        }
    }
    position->white_men = white_men;
    position->black_men = black_men;
    position->white_kings = white_kings;
    position->black_kings = black_kings;
#ifndef NDEBUG
    return index == 0 && rem[0] == 0 && rem[1] == 0 &&
           rem[2] == 0 && rem[3] == 0;
#else
    return true;
#endif
}

typedef struct {
    const EgIndexer *indexer;
    uint64_t tested;
    bool ok;
} TestState;

static bool same_position(const EgPosition *a, const EgPosition *b)
{
    return a->white_men == b->white_men &&
           a->black_men == b->black_men &&
           a->white_kings == b->white_kings &&
           a->black_kings == b->black_kings;
}

static void enumerate(TestState *test, unsigned square, unsigned rem[4],
                      EgPosition *position)
{
    enum Piece piece;
    if (!test->ok)
        return;
    if (square == 50) {
        EgPosition decoded;
        uint64_t index;
        if (rem[0] || rem[1] || rem[2] || rem[3])
            return;
        if (!eg_position_to_index(test->indexer, position, &index) ||
            index != test->tested ||
            !eg_index_to_position(test->indexer, index, &decoded) ||
            !same_position(position, &decoded)) {
            test->ok = false;
            return;
        }
        ++test->tested;
        return;
    }

    for (piece = EMPTY; piece < PIECE_KIND_COUNT; ++piece) {
        unsigned *count = remaining_for(piece, rem);
        if (!allowed(piece, square) || (count != NULL && *count == 0))
            continue;
        if (count != NULL) {
            --*count;
            switch (piece) {
            case WHITE_MAN:  position->white_men |= UINT64_C(1) << square; break;
            case BLACK_MAN:  position->black_men |= UINT64_C(1) << square; break;
            case WHITE_KING: position->white_kings |= UINT64_C(1) << square; break;
            case BLACK_KING: position->black_kings |= UINT64_C(1) << square; break;
            default: break;
            }
        }
        enumerate(test, square + 1, rem, position);
        if (count != NULL) {
            uint64_t bit = UINT64_C(1) << square;
            position->white_men &= ~bit;
            position->black_men &= ~bit;
            position->white_kings &= ~bit;
            position->black_kings &= ~bit;
            ++*count;
        }
    }
}

bool eg_test_material(unsigned wm, unsigned bm, unsigned wk, unsigned bk,
                      uint64_t *positions_tested)
{
    EgIndexer indexer;
    EgPosition position = {0, 0, 0, 0};
    unsigned rem[4] = {wm, bm, wk, bk};
    TestState test;

    if (positions_tested != NULL)
        *positions_tested = 0;
    if (!eg_indexer_init(&indexer, wm, bm, wk, bk))
        return false;
    test.indexer = &indexer;
    test.tested = 0;
    test.ok = true;
    enumerate(&test, 0, rem, &position);
    test.ok = test.ok && test.tested == indexer.position_count;
    if (positions_tested != NULL)
        *positions_tested = test.tested;
    eg_indexer_destroy(&indexer);
    return test.ok;
}
