#include "movegen.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
    const DraughtsPosition *position;
    EgtbSide side;
    uint64_t opponents;
    uint64_t fixed_occupied;
    uint8_t start;
    unsigned maximum;
    DraughtsMoveVisitor visitor;
    void *visitor_context;
    size_t emitted;
    bool emitting;
    bool ok;
} CaptureSearch;

static uint8_t next_square[50][DIRECTION_COUNT];
static uint8_t jump_square[50][DIRECTION_COUNT];
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
    if (!search->emitting) {
        if (count > search->maximum)
            search->maximum = count;
        return true;
    }
    if (count != search->maximum)
        return true;
    move.from = search->start;
    move.to = current;
    move.captured = captured;
    move.capture_count = (uint8_t)count;
    if (search->visitor != NULL &&
        !search->visitor(&move, search->visitor_context)) {
        search->ok = fail("move visitor rejected a generated capture");
        return false;
    }
    ++search->emitted;
    return true;
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
        uint8_t victim = next_square[current][direction];
        uint8_t landing;
        while (victim != INVALID_SQUARE &&
               (current_occupied & BIT(victim)) == 0)
            victim = next_square[victim][direction];
        if (victim == INVALID_SQUARE ||
            (search->opponents & BIT(victim)) == 0 ||
            (captured & BIT(victim)) != 0)
            continue;
        landing = next_square[victim][direction];
        while (landing != INVALID_SQUARE &&
               (current_occupied & BIT(landing)) == 0) {
            found = true;
            search_king_captures(search, landing, captured | BIT(victim),
                                 count + 1);
            landing = next_square[landing][direction];
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
        uint8_t scan = next_square[square][direction];
        while (scan != INVALID_SQUARE && (all & BIT(scan)) == 0)
            scan = next_square[scan][direction];
        if (scan != INVALID_SQUARE && (opponents & BIT(scan)) != 0) {
            uint8_t landing = next_square[scan][direction];
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
            uint8_t target = next_square[square][direction];
            while (target != INVALID_SQUARE && (all & BIT(target)) == 0) {
                if (!visit_quiet_move((uint8_t)square, target, visitor,
                                      context, count))
                    return false;
                target = next_square[target][direction];
            }
        }
    }
    return true;
}

bool draughts_generate_moves(const DraughtsPosition *position, EgtbSide side,
                             DraughtsMoveVisitor visitor, void *context,
                             size_t *move_count)
{
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
    search.ok = true;
    search_all_captures(position, side, &search);
    if (!search.ok)
        return false;
    if (search.maximum != 0) {
        search.emitting = true;
        search.visitor = visitor;
        search.visitor_context = context;
        search_all_captures(position, side, &search);
        if (!search.ok)
            return false;
        if (move_count != NULL)
            *move_count = search.emitted;
        return true;
    }
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
