#include "movegen.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#define DRAUGHTS_HAVE_BMI2 1
#else
#define DRAUGHTS_HAVE_BMI2 0
#endif

#define INVALID_SQUARE UINT8_MAX
#define BIT(square) (UINT64_C(1) << (square))

enum {
    DIR_NW,
    DIR_NE,
    DIR_SW,
    DIR_SE,
    DIRECTION_COUNT
};

typedef struct {
    DraughtsMove *moves;
    DraughtsMove *allocated;
    size_t count;
    size_t capacity;
    bool fixed;
} CaptureBuffer;

typedef struct {
    const DraughtsPosition *position;
    EgtbSide side;
    uint64_t opponents;
    uint64_t fixed_occupied;
    uint8_t start;
    unsigned maximum;
    CaptureBuffer *buffer;
    bool ok;
} CaptureSearch;

static uint8_t next_square[50][DIRECTION_COUNT];
static uint8_t jump_square[50][DIRECTION_COUNT];
static uint64_t ray_mask[50][DIRECTION_COUNT];
static uint64_t between_mask[50][50];
static atomic_uint geometry_state;
static char last_error[256];

static bool fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_error, sizeof(last_error), format, arguments);
    va_end(arguments);
    return false;
}

static bool record_capture(CaptureBuffer *buffer, unsigned *maximum,
                           const DraughtsMove *move)
{
    if (move->capture_count < *maximum)
        return true;
    if (move->capture_count > *maximum) {
        *maximum = move->capture_count;
        buffer->count = 0;
    }
    if (buffer->count == buffer->capacity) {
        if (buffer->fixed)
            return fail("more than %zu maximum capture moves",
                        buffer->capacity);
        size_t capacity = buffer->capacity * 2;
        DraughtsMove *moves;
        if (capacity < buffer->capacity ||
            capacity > SIZE_MAX / sizeof(*moves))
            return fail("too many maximum capture moves");
        moves = malloc(capacity * sizeof(*moves));
        if (moves == NULL)
            return fail("cannot grow maximum capture move buffer");
        memcpy(moves, buffer->moves, buffer->count * sizeof(*moves));
        free(buffer->allocated);
        buffer->allocated = moves;
        buffer->moves = moves;
        buffer->capacity = capacity;
    }
    buffer->moves[buffer->count++] = *move;
    return true;
}

static bool visit_captures(const CaptureBuffer *buffer,
                           DraughtsMoveVisitor visitor, void *context)
{
    size_t index;
    if (visitor == NULL)
        return true;
    for (index = 0; index < buffer->count; ++index)
        if (!visitor(&buffer->moves[index], context))
            return fail("move visitor rejected a generated capture");
    return true;
}

const char *draughts_movegen_last_error(void)
{
    return last_error;
}

static int row_col_to_square(int row, int column)
{
    if (row < 0 || row >= 10 || column < 0 || column >= 10 ||
        ((row + column) & 1) == 0)
        return -1;
    return row * 5 + column / 2;
}

static void initialize_geometry(void)
{
    static const int row_delta[DIRECTION_COUNT] = {-1, -1, 1, 1};
    static const int column_delta[DIRECTION_COUNT] = {-1, 1, -1, 1};
    unsigned square, direction;
    unsigned expected = 0;
    if (atomic_load_explicit(&geometry_state, memory_order_acquire) == 2)
        return;
    if (!atomic_compare_exchange_strong_explicit(
            &geometry_state, &expected, 1, memory_order_acq_rel,
            memory_order_acquire)) {
        while (atomic_load_explicit(&geometry_state, memory_order_acquire) != 2)
            ;
        return;
    }
    for (square = 0; square < 50; ++square) {
        int row = (int)square / 5;
        int column = (int)(square % 5) * 2 + 1 - (row & 1);
        for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
            int next = row_col_to_square(row + row_delta[direction],
                                         column + column_delta[direction]);
            int jump = row_col_to_square(row + 2 * row_delta[direction],
                                         column + 2 * column_delta[direction]);
            next_square[square][direction] =
                next < 0 ? INVALID_SQUARE : (uint8_t)next;
            jump_square[square][direction] =
                jump < 0 ? INVALID_SQUARE : (uint8_t)jump;
        }
    }
    for (square = 0; square < 50; ++square) {
        for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
            uint8_t scan = next_square[square][direction];
            uint64_t between = 0;
            while (scan != INVALID_SQUARE) {
                between_mask[square][scan] = between;
                ray_mask[square][direction] |= BIT(scan);
                between |= BIT(scan);
                scan = next_square[scan][direction];
            }
        }
    }
    atomic_store_explicit(&geometry_state, 2, memory_order_release);
}

static uint64_t occupied(const DraughtsPosition *position)
{
    return position->white_men | position->black_men |
           position->white_kings | position->black_kings;
}

static uint64_t side_men(const DraughtsPosition *position, EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE ? position->white_men :
                                        position->black_men;
}

static uint64_t side_kings(const DraughtsPosition *position, EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE ? position->white_kings :
                                        position->black_kings;
}

static uint64_t opponent_pieces(const DraughtsPosition *position,
                                EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE
               ? position->black_men | position->black_kings
               : position->white_men | position->white_kings;
}

static EgtbSide opposite_side(EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE ? EGTB_BLACK_TO_MOVE :
                                        EGTB_WHITE_TO_MOVE;
}

static bool is_promotion_square(EgtbSide side, unsigned square)
{
    return side == EGTB_WHITE_TO_MOVE ? square < 5 : square >= 45;
}

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

static unsigned first_square(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(value);
#else
    unsigned square = 0;
    while ((value & 1) == 0) {
        value >>= 1;
        ++square;
    }
    return square;
#endif
}

static unsigned last_square(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return 63U - (unsigned)__builtin_clzll(value);
#else
    unsigned square = 0;
    while (value >>= 1)
        ++square;
    return square;
#endif
}

static unsigned nearest_square(uint64_t squares, unsigned direction)
{
    return direction < DIR_SW ? last_square(squares) : first_square(squares);
}

static unsigned take_nearest_square(uint64_t *squares, unsigned direction)
{
    unsigned square = nearest_square(*squares, direction);
    *squares &= ~BIT(square);
    return square;
}

bool draughts_position_is_valid(const DraughtsPosition *position)
{
    uint64_t all;
    if (position == NULL)
        return false;
    all = occupied(position);
    if ((all & ~DRAUGHTS_BOARD_MASK) != 0 ||
        bit_count(all) != bit_count(position->white_men) +
                          bit_count(position->black_men) +
                          bit_count(position->white_kings) +
                          bit_count(position->black_kings))
        return false;
    if ((position->white_men & UINT64_C(0x1f)) != 0 ||
        (position->black_men & (UINT64_C(0x1f) << 45)) != 0)
        return false;
    return true;
}

static bool emit_move(CaptureSearch *search, uint8_t current,
                      uint64_t captured, unsigned count)
{
    DraughtsMove move;
    move.from = search->start;
    move.to = current;
    move.captured = captured;
    move.capture_count = (uint8_t)count;
    search->ok = record_capture(search->buffer, &search->maximum, &move);
    return search->ok;
}

static void search_man_captures(CaptureSearch *search, uint8_t current,
                                uint64_t captured, unsigned count)
{
    uint64_t current_occupied = search->fixed_occupied | BIT(current);
    bool found = false;
    unsigned direction;
    for (direction = 0; direction < DIRECTION_COUNT && search->ok;
         ++direction) {
        uint8_t victim = next_square[current][direction];
        uint8_t landing = jump_square[current][direction];
        if (victim != INVALID_SQUARE && landing != INVALID_SQUARE &&
            (search->opponents & BIT(victim)) != 0 &&
            (captured & BIT(victim)) == 0 &&
            (current_occupied & BIT(landing)) == 0) {
            found = true;
            search_man_captures(search, landing, captured | BIT(victim),
                                count + 1);
        }
    }
    if (!found && count != 0)
        emit_move(search, current, captured, count);
}

static void search_king_captures(CaptureSearch *search, uint8_t current,
                                 uint64_t captured, unsigned count)
{
    uint64_t current_occupied = search->fixed_occupied | BIT(current);
    bool found = false;
    unsigned direction;
    for (direction = 0; direction < DIRECTION_COUNT && search->ok;
         ++direction) {
        uint64_t blockers = ray_mask[current][direction] & current_occupied;
        uint64_t landings;
        unsigned victim, blocker;
        if (blockers == 0)
            continue;
        victim = nearest_square(blockers, direction);
        if ((search->opponents & BIT(victim)) == 0 ||
            (captured & BIT(victim)) != 0)
            continue;
        blockers = ray_mask[victim][direction] & current_occupied;
        if (blockers == 0)
            landings = ray_mask[victim][direction];
        else {
            blocker = nearest_square(blockers, direction);
            landings = between_mask[victim][blocker];
        }
        while (landings != 0) {
            unsigned landing = take_nearest_square(&landings, direction);
            found = true;
            search_king_captures(search, (uint8_t)landing,
                                 captured | BIT(victim),
                                 count + 1);
        }
    }
    if (!found && count != 0)
        emit_move(search, current, captured, count);
}

static void search_all_captures(const DraughtsPosition *position,
                                EgtbSide side, CaptureSearch *search)
{
    uint64_t men = side_men(position, side);
    uint64_t kings = side_kings(position, side);
    uint64_t all = occupied(position);
    while (men != 0 && search->ok) {
        unsigned square = first_square(men);
        men &= men - 1;
        search->start = (uint8_t)square;
        search->fixed_occupied = all & ~BIT(square);
        search_man_captures(search, (uint8_t)square, 0, 0);
    }
    while (kings != 0 && search->ok) {
        unsigned square = first_square(kings);
        kings &= kings - 1;
        search->start = (uint8_t)square;
        search->fixed_occupied = all & ~BIT(square);
        search_king_captures(search, (uint8_t)square, 0, 0);
    }
}

static bool man_has_capture(uint8_t square, uint64_t opponents,
                            uint64_t all)
{
    unsigned direction;
    for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
        uint8_t victim = next_square[square][direction];
        uint8_t landing = jump_square[square][direction];
        if (victim != INVALID_SQUARE && landing != INVALID_SQUARE &&
            (opponents & BIT(victim)) != 0 &&
            (all & BIT(landing)) == 0)
            return true;
    }
    return false;
}

static bool king_has_capture(uint8_t square, uint64_t opponents,
                             uint64_t all)
{
    unsigned direction;
    for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
        uint64_t blockers = ray_mask[square][direction] & all;
        if (blockers != 0) {
            unsigned scan = nearest_square(blockers, direction);
            uint8_t landing = next_square[scan][direction];
            if ((opponents & BIT(scan)) == 0)
                continue;
            if (landing != INVALID_SQUARE && (all & BIT(landing)) == 0)
                return true;
        }
    }
    return false;
}

bool draughts_has_capture(const DraughtsPosition *position, EgtbSide side)
{
    uint64_t men, kings, opponents, all;
    initialize_geometry();
#ifndef NDEBUG
    if (!draughts_position_is_valid(position) ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return false;
#endif
    men = side_men(position, side);
    kings = side_kings(position, side);
    opponents = opponent_pieces(position, side);
    all = occupied(position);
    while (men != 0) {
        unsigned square = first_square(men);
        men &= men - 1;
        if (man_has_capture((uint8_t)square, opponents, all))
            return true;
    }
    while (kings != 0) {
        unsigned square = first_square(kings);
        kings &= kings - 1;
        if (king_has_capture((uint8_t)square, opponents, all))
            return true;
    }
    return false;
}

static bool visit_quiet_move(uint8_t from, uint8_t to,
                             DraughtsMoveVisitor visitor, void *context,
                             size_t *count)
{
    DraughtsMove move;
    move.from = from;
    move.to = to;
    move.captured = 0;
    move.capture_count = 0;
    if (visitor != NULL && !visitor(&move, context))
        return fail("move visitor rejected a generated quiet move");
    ++*count;
    return true;
}

static bool generate_quiet_moves(const DraughtsPosition *position,
                                 EgtbSide side, DraughtsMoveVisitor visitor,
                                 void *context, size_t *count)
{
    uint64_t men = side_men(position, side);
    uint64_t kings = side_kings(position, side);
    uint64_t all = occupied(position);
    unsigned first_direction =
        side == EGTB_WHITE_TO_MOVE ? DIR_NW : DIR_SW;
    while (men != 0) {
        unsigned square = first_square(men);
        unsigned direction;
        men &= men - 1;
        for (direction = first_direction; direction < first_direction + 2;
             ++direction) {
            uint8_t target = next_square[square][direction];
            if (target != INVALID_SQUARE && (all & BIT(target)) == 0 &&
                !visit_quiet_move((uint8_t)square, target, visitor, context,
                                  count))
                return false;
        }
    }
    while (kings != 0) {
        unsigned square = first_square(kings);
        unsigned direction;
        kings &= kings - 1;
        for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
            uint64_t blockers = ray_mask[square][direction] & all;
            uint64_t targets = ray_mask[square][direction];
            if (blockers != 0) {
                unsigned blocker = nearest_square(blockers, direction);
                targets = between_mask[square][blocker];
            }
            while (targets != 0) {
                unsigned target = take_nearest_square(&targets, direction);
                if (!visit_quiet_move((uint8_t)square, (uint8_t)target, visitor,
                                      context, count))
                    return false;
            }
        }
    }
    return true;
}

bool draughts_generate_moves(const DraughtsPosition *position, EgtbSide side,
                             DraughtsMoveVisitor visitor, void *context,
                             size_t *move_count)
{
    DraughtsMove stack_moves[256];
    CaptureBuffer buffer = {stack_moves, NULL, 0, 256, false};
    CaptureSearch search;
    size_t quiet_count = 0;
    initialize_geometry();
    if (move_count != NULL)
        *move_count = 0;
#ifndef NDEBUG
    if (!draughts_position_is_valid(position) ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid move-generation position or side");
#endif
    memset(&search, 0, sizeof(search));
    search.position = position;
    search.side = side;
    search.opponents = opponent_pieces(position, side);
    search.buffer = &buffer;
    search.ok = true;
    search_all_captures(position, side, &search);
    if (!search.ok) {
        free(buffer.allocated);
        return false;
    }
    if (search.maximum != 0) {
        if (!visit_captures(&buffer, visitor, context)) {
            free(buffer.allocated);
            return false;
        }
        if (move_count != NULL)
            *move_count = buffer.count;
        free(buffer.allocated);
        return true;
    }
    free(buffer.allocated);
    if (!generate_quiet_moves(position, side, visitor, context, &quiet_count))
        return false;
    if (move_count != NULL)
        *move_count = quiet_count;
    return true;
}

bool draughts_do_move(DraughtsPosition *position, EgtbSide side,
                      const DraughtsMove *move, DraughtsUndo *undo)
{
    uint64_t from_bit, to_bit;
    uint64_t *men, *kings, *opponent_men, *opponent_kings;
    bool moving_king;
#ifndef NDEBUG
    uint64_t all, opponents;
    if (!draughts_position_is_valid(position) || move == NULL ||
        move->from >= 50 || move->to >= 50 ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid move application argument");
#endif
    from_bit = BIT(move->from);
    to_bit = BIT(move->to);
#ifndef NDEBUG
    all = occupied(position);
    opponents = opponent_pieces(position, side);
    if (move->capture_count != bit_count(move->captured) ||
        (move->captured & ~opponents) != 0 ||
        ((all & to_bit) != 0 && move->to != move->from))
        return fail("invalid generated move data");
#endif
    if (side == EGTB_WHITE_TO_MOVE) {
        men = &position->white_men;
        kings = &position->white_kings;
        opponent_men = &position->black_men;
        opponent_kings = &position->black_kings;
    } else {
        men = &position->black_men;
        kings = &position->black_kings;
        opponent_men = &position->white_men;
        opponent_kings = &position->white_kings;
    }
    moving_king = (*kings & from_bit) != 0;
#ifndef NDEBUG
    if (!moving_king && (*men & from_bit) == 0)
        return fail("move source does not contain a friendly piece");
#endif
    if (undo != NULL) {
        undo->white_men = position->white_men;
        undo->black_men = position->black_men;
        undo->white_kings = position->white_kings;
        undo->black_kings = position->black_kings;
    }
    *men &= ~from_bit;
    *kings &= ~from_bit;
    *opponent_men &= ~move->captured;
    *opponent_kings &= ~move->captured;
    if (moving_king || is_promotion_square(side, move->to))
        *kings |= to_bit;
    else
        *men |= to_bit;
    return true;
}

void draughts_undo_move(DraughtsPosition *position, const DraughtsUndo *undo)
{
    if (position == NULL || undo == NULL)
        return;
    position->white_men = undo->white_men;
    position->black_men = undo->black_men;
    position->white_kings = undo->white_kings;
    position->black_kings = undo->black_kings;
}

static bool emit_predecessor(const DraughtsPosition *current,
                             DraughtsPosition *predecessor, EgtbSide mover,
                             uint8_t from, uint8_t to,
                             DraughtsPredecessorVisitor visitor, void *context,
                             size_t *count)
{
    DraughtsMove move = {0, from, to, 0};
    (void)current;
    if (draughts_has_capture(predecessor, mover))
        return true;
    if (visitor != NULL && !visitor(predecessor, &move, context))
        return fail("predecessor visitor rejected a generated position");
    ++*count;
    return true;
}

static bool generate_piece_predecessors(
    const DraughtsPosition *position, EgtbSide mover, uint8_t to,
    bool king, DraughtsPredecessorVisitor visitor, void *context,
    size_t *count)
{
    uint64_t all_without_to = occupied(position) & ~BIT(to);
    unsigned direction;
    if (!king) {
        unsigned first_direction =
            mover == EGTB_WHITE_TO_MOVE ? DIR_SW : DIR_NW;
        if (is_promotion_square(mover, to))
            return true;
        for (direction = first_direction; direction < first_direction + 2;
             ++direction) {
            uint8_t from = next_square[to][direction];
            DraughtsPosition predecessor;
            uint64_t *men;
            if (from == INVALID_SQUARE || is_promotion_square(mover, from) ||
                (all_without_to & BIT(from)) != 0)
                continue;
            predecessor = *position;
            men = mover == EGTB_WHITE_TO_MOVE ? &predecessor.white_men :
                                                &predecessor.black_men;
            *men &= ~BIT(to);
            *men |= BIT(from);
            if (!emit_predecessor(position, &predecessor, mover, from, to,
                                  visitor, context, count))
                return false;
        }
        return true;
    }

    for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
        uint8_t from = next_square[to][direction];
        while (from != INVALID_SQUARE &&
               (all_without_to & BIT(from)) == 0) {
            DraughtsPosition predecessor = *position;
            uint64_t *kings =
                mover == EGTB_WHITE_TO_MOVE ? &predecessor.white_kings :
                                              &predecessor.black_kings;
            *kings &= ~BIT(to);
            *kings |= BIT(from);
            if (!emit_predecessor(position, &predecessor, mover, from, to,
                                  visitor, context, count))
                return false;
            from = next_square[from][direction];
        }
    }

    return true;
}

bool draughts_generate_quiet_predecessors(
    const DraughtsPosition *position, EgtbSide current_side,
    DraughtsPredecessorVisitor visitor, void *context,
    size_t *predecessor_count)
{
    EgtbSide mover;
    uint64_t men, kings;
    size_t count = 0;
    initialize_geometry();
    if (predecessor_count != NULL)
        *predecessor_count = 0;
#ifndef NDEBUG
    if (!draughts_position_is_valid(position) ||
        (current_side != EGTB_WHITE_TO_MOVE &&
         current_side != EGTB_BLACK_TO_MOVE))
        return fail("invalid predecessor-generation position or side");
#endif
    mover = opposite_side(current_side);
    men = side_men(position, mover);
    kings = side_kings(position, mover);
    while (men != 0) {
        unsigned to = first_square(men);
        men &= men - 1;
        if (!generate_piece_predecessors(position, mover, (uint8_t)to, false,
                                         visitor, context, &count))
            return false;
    }
    while (kings != 0) {
        unsigned to = first_square(kings);
        kings &= kings - 1;
        if (!generate_piece_predecessors(position, mover, (uint8_t)to, true,
                                         visitor, context, &count))
            return false;
    }
    if (predecessor_count != NULL)
        *predecessor_count = count;
    return true;
}

/*
 * GWD-compatible padded move generator.
 *
 * Compact squares 0..49 are deposited into fields
 *
 *   6..15, 17..26, 28..37, 39..48, 50..59
 *
 * leaving a guard bit between each pair of board rows. Diagonal steps are
 * therefore fixed shifts by 5 or 6. Always shift an existing uint64_t word;
 * never form 1ULL << (field + offset), because the latter would be undefined
 * for fields whose sum reaches 64.
 */
#define PADDED_BOARD_MASK UINT64_C(0x0ffdffbff7feffc0)

bool draughts_padded_backend_uses_bmi2(void)
{
    return DRAUGHTS_HAVE_BMI2 != 0;
}

typedef struct {
    uint64_t white_men;
    uint64_t black_men;
    uint64_t white_kings;
    uint64_t black_kings;
} PaddedPosition;

typedef struct {
    uint64_t opponents;
    uint64_t fixed_occupied;
    uint64_t start;
    unsigned maximum;
    CaptureBuffer *buffer;
    bool ok;
} PaddedCaptureSearch;

static inline uint64_t padded_step(uint64_t bits, unsigned direction)
{
    switch (direction) {
    case DIR_NW: return (bits >> 6) & PADDED_BOARD_MASK;
    case DIR_NE: return (bits >> 5) & PADDED_BOARD_MASK;
    case DIR_SW: return (bits << 5) & PADDED_BOARD_MASK;
    default:     return (bits << 6) & PADDED_BOARD_MASK;
    }
}

static inline unsigned padded_field_from_square(unsigned square)
{
    return square + 6 + square / 10;
}

static uint8_t padded_square[64];

static inline unsigned padded_square_from_field(unsigned field)
{
    return padded_square[field];
}

static inline unsigned padded_square_from_bit(uint64_t bit)
{
    return padded_square_from_field(first_square(bit));
}

static uint64_t padded_ray_mask[64][DIRECTION_COUNT];
static uint64_t padded_between_mask[64][64];
static atomic_uint padded_geometry_state;

static void initialize_padded_geometry(void)
{
    unsigned square, direction;
    unsigned expected = 0;
    if (atomic_load_explicit(&padded_geometry_state,
                             memory_order_acquire) == 2)
        return;
    if (!atomic_compare_exchange_strong_explicit(
            &padded_geometry_state, &expected, 1, memory_order_acq_rel,
            memory_order_acquire)) {
        while (atomic_load_explicit(&padded_geometry_state,
                                    memory_order_acquire) != 2)
            ;
        return;
    }
    for (square = 0; square < 50; ++square) {
        unsigned field = padded_field_from_square(square);
        uint64_t start = BIT(field);
        padded_square[field] = (uint8_t)square;
        for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
            uint64_t scan = padded_step(start, direction);
            uint64_t between = 0;
            while (scan != 0) {
                unsigned scan_field = first_square(scan);
                padded_between_mask[field][scan_field] = between;
                padded_ray_mask[field][direction] |= scan;
                between |= scan;
                scan = padded_step(scan, direction);
            }
        }
    }
    atomic_store_explicit(&padded_geometry_state, 2, memory_order_release);
}

static inline uint64_t padded_ray(uint64_t square, unsigned direction)
{
    return padded_ray_mask[first_square(square)][direction];
}

static inline uint64_t padded_nearest_bit(uint64_t bits,
                                          unsigned direction)
{
    unsigned field = direction < DIR_SW ? last_square(bits) :
                                           first_square(bits);
    return BIT(field);
}

static inline uint64_t padded_take_nearest_bit(uint64_t *bits,
                                               unsigned direction)
{
    uint64_t bit = padded_nearest_bit(*bits, direction);
    *bits &= ~bit;
    return bit;
}

static inline uint64_t compact_to_padded(uint64_t compact)
{
#if DRAUGHTS_HAVE_BMI2
    return _pdep_u64(compact, PADDED_BOARD_MASK);
#else
    uint64_t padded = 0;
    while (compact != 0) {
        unsigned square = first_square(compact);
        compact &= compact - 1;
        padded |= UINT64_C(1) << padded_field_from_square(square);
    }
    return padded;
#endif
}

static inline uint64_t padded_to_compact(uint64_t padded)
{
#if DRAUGHTS_HAVE_BMI2
    return _pext_u64(padded, PADDED_BOARD_MASK);
#else
    uint64_t compact = 0;
    while (padded != 0) {
        uint64_t bit = padded & (UINT64_C(0) - padded);
        unsigned square = padded_square_from_bit(bit);
        padded &= padded - 1;
        compact |= BIT(square);
    }
    return compact;
#endif
}

static PaddedPosition make_padded_position(
    const DraughtsPosition *position)
{
    PaddedPosition padded;
    initialize_padded_geometry();
    padded.white_men = compact_to_padded(position->white_men);
    padded.black_men = compact_to_padded(position->black_men);
    padded.white_kings = compact_to_padded(position->white_kings);
    padded.black_kings = compact_to_padded(position->black_kings);
    return padded;
}

static inline uint64_t padded_occupied(const PaddedPosition *position)
{
    return position->white_men | position->black_men |
           position->white_kings | position->black_kings;
}

static inline uint64_t padded_side_men(const PaddedPosition *position,
                                       EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE ? position->white_men :
                                        position->black_men;
}

static inline uint64_t padded_side_kings(const PaddedPosition *position,
                                         EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE ? position->white_kings :
                                        position->black_kings;
}

static inline uint64_t padded_opponents(const PaddedPosition *position,
                                        EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE
               ? position->black_men | position->black_kings
               : position->white_men | position->white_kings;
}

static uint64_t padded_man_capture_starts(uint64_t men,
                                          uint64_t opponents,
                                          uint64_t all)
{
    uint64_t empty = PADDED_BOARD_MASK & ~all;
    uint64_t starts = 0;
    unsigned direction;

    for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
        unsigned opposite = direction ^ 3U;
        uint64_t victims = opponents & padded_step(empty, opposite);
        starts |= men & padded_step(victims, opposite);
    }
    return starts;
}

static bool padded_emit_capture(PaddedCaptureSearch *search,
                                uint64_t current, uint64_t captured,
                                unsigned count)
{
    DraughtsMove move;
    move.from = (uint8_t)padded_square_from_bit(search->start);
    move.to = (uint8_t)padded_square_from_bit(current);
    move.captured = padded_to_compact(captured);
    move.capture_count = (uint8_t)count;
    search->ok = record_capture(search->buffer, &search->maximum, &move);
    return search->ok;
}

static void padded_search_man_captures(PaddedCaptureSearch *search,
                                       uint64_t current,
                                       uint64_t captured, unsigned count)
{
    uint64_t current_occupied = search->fixed_occupied | current;
    bool found = false;
    unsigned direction;
    for (direction = 0; direction < DIRECTION_COUNT && search->ok;
         ++direction) {
        uint64_t victim = padded_step(current, direction);
        uint64_t landing = padded_step(victim, direction);
        if ((victim & search->opponents & ~captured) != 0 && landing != 0 &&
            (current_occupied & landing) == 0) {
            found = true;
            padded_search_man_captures(search, landing, captured | victim,
                                       count + 1);
        }
    }
    if (!found && count != 0)
        padded_emit_capture(search, current, captured, count);
}

static void padded_search_king_captures(PaddedCaptureSearch *search,
                                        uint64_t current,
                                        uint64_t captured, unsigned count)
{
    uint64_t current_occupied = search->fixed_occupied | current;
    bool found = false;
    unsigned direction;
    for (direction = 0; direction < DIRECTION_COUNT && search->ok;
         ++direction) {
        uint64_t blockers = padded_ray(current, direction) & current_occupied;
        uint64_t victim, landings;
        if (blockers == 0)
            continue;
        victim = padded_nearest_bit(blockers, direction);
        if ((victim & search->opponents & ~captured) == 0)
            continue;
        blockers = padded_ray(victim, direction) & current_occupied;
        if (blockers == 0)
            landings = padded_ray(victim, direction);
        else {
            uint64_t blocker = padded_nearest_bit(blockers, direction);
            landings = padded_between_mask[first_square(victim)]
                                           [first_square(blocker)];
        }
        while (landings != 0) {
            uint64_t landing = padded_take_nearest_bit(&landings, direction);
            found = true;
            padded_search_king_captures(search, landing,
                                        captured | victim, count + 1);
        }
    }
    if (!found && count != 0)
        padded_emit_capture(search, current, captured, count);
}

static void padded_search_all_captures(const PaddedPosition *position,
                                       EgtbSide side,
                                       PaddedCaptureSearch *search)
{
    uint64_t men = padded_side_men(position, side);
    uint64_t kings = padded_side_kings(position, side);
    uint64_t all = padded_occupied(position);
    men = padded_man_capture_starts(men, search->opponents, all);
    while (men != 0 && search->ok) {
        uint64_t piece = men & (UINT64_C(0) - men);
        men &= men - 1;
        search->start = piece;
        search->fixed_occupied = all & ~piece;
        padded_search_man_captures(search, piece, 0, 0);
    }
    while (kings != 0 && search->ok) {
        uint64_t piece = kings & (UINT64_C(0) - kings);
        kings &= kings - 1;
        search->start = piece;
        search->fixed_occupied = all & ~piece;
        padded_search_king_captures(search, piece, 0, 0);
    }
}

static bool padded_king_has_capture(uint64_t king, uint64_t opponents,
                                    uint64_t all)
{
    unsigned direction;
    for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
        uint64_t blockers = padded_ray(king, direction) & all;
        if (blockers != 0) {
            uint64_t scan = padded_nearest_bit(blockers, direction);
            uint64_t landing = padded_step(scan, direction);
            if ((scan & opponents) == 0)
                continue;
            if (landing != 0 && (all & landing) == 0)
                return true;
        }
    }
    return false;
}

static bool padded_position_has_capture(const PaddedPosition *position,
                                        EgtbSide side)
{
    uint64_t men = padded_side_men(position, side);
    uint64_t kings = padded_side_kings(position, side);
    uint64_t opponents = padded_opponents(position, side);
    uint64_t all = padded_occupied(position);
    if (padded_man_capture_starts(men, opponents, all) != 0)
        return true;
    while (kings != 0) {
        uint64_t king = kings & (UINT64_C(0) - kings);
        kings &= kings - 1;
        if (padded_king_has_capture(king, opponents, all))
            return true;
    }
    return false;
}

bool draughts_has_capture_padded(const DraughtsPosition *position,
                                 EgtbSide side)
{
    PaddedPosition padded;
#ifndef NDEBUG
    if (!draughts_position_is_valid(position) ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return false;
#endif
    padded = make_padded_position(position);
    return padded_position_has_capture(&padded, side);
}

static bool padded_store_quiet_move(uint64_t from, uint64_t to,
                                    DraughtsMove *moves, size_t capacity,
                                    size_t *count)
{
    DraughtsMove move;
    if (*count == capacity)
        return fail("more than %zu quiet moves", capacity);
    move.from = (uint8_t)padded_square_from_bit(from);
    move.to = (uint8_t)padded_square_from_bit(to);
    move.captured = 0;
    move.capture_count = 0;
    moves[(*count)++] = move;
    return true;
}

static bool padded_generate_quiet_moves_into(
    const PaddedPosition *position, EgtbSide side, DraughtsMove *moves,
    size_t capacity, size_t *count)
{
    uint64_t men = padded_side_men(position, side);
    uint64_t kings = padded_side_kings(position, side);
    uint64_t all = padded_occupied(position);
    unsigned first_direction =
        side == EGTB_WHITE_TO_MOVE ? DIR_NW : DIR_SW;
    unsigned direction;

    for (direction = first_direction; direction < first_direction + 2;
         ++direction) {
        uint64_t targets = padded_step(men, direction) & ~all;
        while (targets != 0) {
            uint64_t target = targets & (UINT64_C(0) - targets);
            uint64_t from = padded_step(target, 3 - direction);
            targets &= targets - 1;
            if (!padded_store_quiet_move(from, target, moves, capacity,
                                         count))
                return false;
        }
    }
    while (kings != 0) {
        uint64_t king = kings & (UINT64_C(0) - kings);
        kings &= kings - 1;
        for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
            uint64_t blockers = padded_ray(king, direction) & all;
            uint64_t targets = padded_ray(king, direction);
            if (blockers != 0) {
                uint64_t blocker = padded_nearest_bit(blockers, direction);
                targets = padded_between_mask[first_square(king)]
                                             [first_square(blocker)];
            }
            while (targets != 0) {
                uint64_t target =
                    padded_take_nearest_bit(&targets, direction);
                if (!padded_store_quiet_move(king, target, moves, capacity,
                                             count))
                    return false;
            }
        }
    }
    return true;
}

bool draughts_generate_moves_padded_into(
    const DraughtsPosition *position, EgtbSide side, DraughtsMove *moves,
    size_t capacity, size_t *move_count)
{
    CaptureBuffer buffer = {moves, NULL, 0, capacity, true};
    PaddedPosition padded;
    PaddedCaptureSearch search;
    if (move_count == NULL || (moves == NULL && capacity != 0))
        return fail("invalid direct padded move-generation output");
    *move_count = 0;
#ifndef NDEBUG
    if (!draughts_position_is_valid(position) ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid direct padded move-generation position or side");
#endif
    padded = make_padded_position(position);
    memset(&search, 0, sizeof(search));
    search.opponents = padded_opponents(&padded, side);
    search.buffer = &buffer;
    search.ok = true;
    padded_search_all_captures(&padded, side, &search);
    if (!search.ok)
        return false;
    if (search.maximum != 0) {
        *move_count = buffer.count;
        return true;
    }
    return padded_generate_quiet_moves_into(&padded, side, moves, capacity,
                                             move_count);
}

bool draughts_generate_moves_padded(
    const DraughtsPosition *position, EgtbSide side,
    DraughtsMoveVisitor visitor, void *context, size_t *move_count)
{
    DraughtsMove moves[DRAUGHTS_MOVES_MAX];
    size_t count, index;
    if (!draughts_generate_moves_padded_into(
            position, side, moves, DRAUGHTS_MOVES_MAX, &count))
        return false;
    if (visitor != NULL) {
        for (index = 0; index < count; ++index) {
            if (!visitor(&moves[index], context))
                return fail("move visitor rejected a padded generated move");
        }
    }
    if (move_count != NULL)
        *move_count = count;
    return true;
}

static bool padded_emit_predecessor(
    const DraughtsPosition *current, const PaddedPosition *predecessor_padded,
    EgtbSide mover, bool king, uint64_t from, uint64_t to,
    DraughtsPredecessorVisitor visitor, void *context, size_t *count)
{
    DraughtsPosition predecessor;
    DraughtsMove move;
    uint64_t *pieces;
    unsigned from_square, to_square;
    if (padded_position_has_capture(predecessor_padded, mover))
        return true;
    if (visitor == NULL) {
        ++*count;
        return true;
    }
    from_square = padded_square_from_bit(from);
    to_square = padded_square_from_bit(to);
    predecessor = *current;
    if (mover == EGTB_WHITE_TO_MOVE)
        pieces = king ? &predecessor.white_kings : &predecessor.white_men;
    else
        pieces = king ? &predecessor.black_kings : &predecessor.black_men;
    *pieces &= ~BIT(to_square);
    *pieces |= BIT(from_square);
    move.captured = 0;
    move.from = (uint8_t)from_square;
    move.to = (uint8_t)to_square;
    move.capture_count = 0;
    if (!visitor(&predecessor, &move, context))
        return fail("predecessor visitor rejected a padded position");
    ++*count;
    return true;
}

static bool padded_generate_piece_predecessors(
    const DraughtsPosition *position, const PaddedPosition *padded,
    EgtbSide mover, uint64_t to, bool king,
    DraughtsPredecessorVisitor visitor, void *context, size_t *count)
{
    uint64_t all_without_to = padded_occupied(padded) & ~to;
    unsigned to_square = padded_square_from_bit(to);
    unsigned direction;

    if (!king) {
        unsigned first_direction =
            mover == EGTB_WHITE_TO_MOVE ? DIR_SW : DIR_NW;
        if (is_promotion_square(mover, to_square))
            return true;
        for (direction = first_direction; direction < first_direction + 2;
             ++direction) {
            uint64_t from = padded_step(to, direction);
            PaddedPosition predecessor;
            uint64_t *men;
            if (from == 0 ||
                is_promotion_square(mover, padded_square_from_bit(from)) ||
                (all_without_to & from) != 0)
                continue;
            predecessor = *padded;
            men = mover == EGTB_WHITE_TO_MOVE ? &predecessor.white_men :
                                                &predecessor.black_men;
            *men &= ~to;
            *men |= from;
            if (!padded_emit_predecessor(position, &predecessor, mover,
                                         false, from, to, visitor, context,
                                         count))
                return false;
        }
        return true;
    }

    for (direction = 0; direction < DIRECTION_COUNT; ++direction) {
        uint64_t blockers = padded_ray(to, direction) & all_without_to;
        uint64_t sources = padded_ray(to, direction);
        if (blockers != 0) {
            uint64_t blocker = padded_nearest_bit(blockers, direction);
            sources = padded_between_mask[first_square(to)]
                                         [first_square(blocker)];
        }
        while (sources != 0) {
            uint64_t from = padded_take_nearest_bit(&sources, direction);
            PaddedPosition predecessor = *padded;
            uint64_t *kings =
                mover == EGTB_WHITE_TO_MOVE ? &predecessor.white_kings :
                                              &predecessor.black_kings;
            *kings &= ~to;
            *kings |= from;
            if (!padded_emit_predecessor(position, &predecessor, mover,
                                         true, from, to, visitor, context,
                                         count))
                return false;
        }
    }
    return true;
}

bool draughts_generate_quiet_predecessors_padded(
    const DraughtsPosition *position, EgtbSide current_side,
    DraughtsPredecessorVisitor visitor, void *context,
    size_t *predecessor_count)
{
    PaddedPosition padded;
    EgtbSide mover;
    uint64_t men, kings;
    size_t count = 0;
    if (predecessor_count != NULL)
        *predecessor_count = 0;
#ifndef NDEBUG
    if (!draughts_position_is_valid(position) ||
        (current_side != EGTB_WHITE_TO_MOVE &&
         current_side != EGTB_BLACK_TO_MOVE))
        return fail("invalid padded predecessor-generation position or side");
#endif
    padded = make_padded_position(position);
    mover = opposite_side(current_side);
    men = padded_side_men(&padded, mover);
    kings = padded_side_kings(&padded, mover);
    while (men != 0) {
        uint64_t to = men & (UINT64_C(0) - men);
        men &= men - 1;
        if (!padded_generate_piece_predecessors(
                position, &padded, mover, to, false, visitor, context,
                &count))
            return false;
    }
    while (kings != 0) {
        uint64_t to = kings & (UINT64_C(0) - kings);
        kings &= kings - 1;
        if (!padded_generate_piece_predecessors(
                position, &padded, mover, to, true, visitor, context,
                &count))
            return false;
    }
    if (predecessor_count != NULL)
        *predecessor_count = count;
    return true;
}
