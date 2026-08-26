#include "material.h"

#include <stdint.h>

static unsigned bit_count(uint64_t value)
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

static uint64_t rotate_180(uint64_t value)
{
    uint64_t result = 0;
    while (value != 0) {
        unsigned square;
#if defined(__GNUC__) || defined(__clang__)
        square = (unsigned)__builtin_ctzll(value);
#else
        uint64_t bit = value & (~value + 1);
        square = 0;
        while ((bit >> square) != 1)
            ++square;
#endif
        result |= UINT64_C(1) << (49 - square);
        value &= value - 1;
    }
    return result;
}

EgtbMaterialKind egtb_material_resolve(const EgtbMaterial *requested,
                                       EgtbMaterial *canonical)
{
    unsigned white, black, total;
    bool keep_white;
    if (requested == NULL || canonical == NULL)
        return EGTB_MATERIAL_INVALID;
    white = requested->white_kings + requested->white_men;
    black = requested->black_kings + requested->black_men;
    total = white + black;
    if (requested->white_kings > EGTB_MAX_PIECES ||
        requested->white_men > EGTB_MAX_PIECES ||
        requested->black_kings > EGTB_MAX_PIECES ||
        requested->black_men > EGTB_MAX_PIECES || total > EGTB_MAX_PIECES)
        return EGTB_MATERIAL_INVALID;
    if (white == 0 || black == 0) {
        *canonical = *requested;
        return EGTB_MATERIAL_TERMINAL;
    }
    keep_white = white > black ||
                 (white == black &&
                  requested->white_kings >= requested->black_kings);
    if (keep_white) {
        *canonical = *requested;
        return EGTB_MATERIAL_CANONICAL;
    }
    canonical->white_kings = requested->black_kings;
    canonical->white_men = requested->black_men;
    canonical->black_kings = requested->white_kings;
    canonical->black_men = requested->white_men;
    return EGTB_MATERIAL_MIRROR;
}

EgtbMaterial egtb_position_material(const DraughtsPosition *position)
{
    EgtbMaterial material;
    material.white_kings = bit_count(position->white_kings);
    material.white_men = bit_count(position->white_men);
    material.black_kings = bit_count(position->black_kings);
    material.black_men = bit_count(position->black_men);
    return material;
}

void egtb_mirror_position(const DraughtsPosition *source,
                          DraughtsPosition *destination)
{
    DraughtsPosition mirrored;
    mirrored.white_men = rotate_180(source->black_men);
    mirrored.black_men = rotate_180(source->white_men);
    mirrored.white_kings = rotate_180(source->black_kings);
    mirrored.black_kings = rotate_180(source->white_kings);
    *destination = mirrored;
}

EgtbSide egtb_mirror_side(EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE ? EGTB_BLACK_TO_MOVE :
                                        EGTB_WHITE_TO_MOVE;
}

int egtb_material_generation_compare(const EgtbMaterial *a,
                                     const EgtbMaterial *b)
{
    unsigned a_white = a->white_kings + a->white_men;
    unsigned a_black = a->black_kings + a->black_men;
    unsigned b_white = b->white_kings + b->white_men;
    unsigned b_black = b->black_kings + b->black_men;
    unsigned a_total = a_white + a_black;
    unsigned b_total = b_white + b_black;
#define COMPARE(field_a, field_b) \
    do { if ((field_a) != (field_b)) return (field_a) < (field_b) ? -1 : 1; } while (0)
    COMPARE(a_total, b_total);
    /* GWD lists 6x1 before 5x2, then kings from high to low. */
    COMPARE(b_white, a_white);
    COMPARE(b->white_kings, a->white_kings);
    COMPARE(b->black_kings, a->black_kings);
#undef COMPARE
    return 0;
}
