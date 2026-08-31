#include "endgame_index.h"
#include "movegen.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIT(square) (UINT64_C(1) << (square))

typedef struct {
    DraughtsPosition original;
    EgtbSide side;
    size_t moves;
    unsigned capture_count;
    bool saw_capture;
    bool failed;
    uint64_t destination_mask;
    uint64_t fingerprint_xor;
    uint64_t fingerprint_sum;
} MoveCheck;

typedef struct {
    DraughtsPosition current;
    EgtbSide mover;
    size_t predecessors;
    bool failed;
    uint64_t source_mask;
    uint64_t fingerprint_xor;
    uint64_t fingerprint_sum;
} PredecessorCheck;

typedef bool (*GenerateMovesFunction)(
    const DraughtsPosition *, EgtbSide, DraughtsMoveVisitor, void *, size_t *);
typedef bool (*GeneratePredecessorsFunction)(
    const DraughtsPosition *, EgtbSide, DraughtsPredecessorVisitor, void *,
    size_t *);

static int square_at(int row, int column)
{
    if (row < 0 || row >= 10 || column < 0 || column >= 10 ||
        ((row + column) & 1) == 0)
        return -1;
    return row * 5 + column / 2;
}

static bool same_position(const DraughtsPosition *a,
                          const DraughtsPosition *b)
{
    return a->white_men == b->white_men &&
           a->black_men == b->black_men &&
           a->white_kings == b->white_kings &&
           a->black_kings == b->black_kings;
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

static uint64_t move_fingerprint(const DraughtsMove *move)
{
    uint64_t value = move->captured;
    value ^= (uint64_t)move->from << 50;
    value ^= (uint64_t)move->to << 56;
    value ^= (uint64_t)move->capture_count * UINT64_C(0x9e3779b97f4a7c15);
    return mix64(value);
}

static uint64_t predecessor_fingerprint(
    const DraughtsPosition *position, const DraughtsMove *move)
{
    uint64_t value = mix64(position->white_men);
    value ^= mix64(position->black_men + UINT64_C(0x9e3779b97f4a7c15));
    value ^= mix64(position->white_kings + UINT64_C(0x3c6ef372fe94f82a));
    value ^= mix64(position->black_kings + UINT64_C(0xdaa66d2c7ddef743));
    return mix64(value ^ move_fingerprint(move));
}

static unsigned test_bit_count(uint64_t bits)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_popcountll(bits);
#else
    unsigned count = 0;
    while (bits != 0) {
        bits &= bits - 1;
        ++count;
    }
    return count;
#endif
}

static unsigned position_pieces(const DraughtsPosition *position)
{
    return test_bit_count(position->white_men | position->black_men |
                          position->white_kings | position->black_kings);
}

static bool same_material(const DraughtsPosition *a,
                          const DraughtsPosition *b)
{
    return test_bit_count(a->white_men) == test_bit_count(b->white_men) &&
           test_bit_count(a->black_men) == test_bit_count(b->black_men) &&
           test_bit_count(a->white_kings) == test_bit_count(b->white_kings) &&
           test_bit_count(a->black_kings) == test_bit_count(b->black_kings);
}

static bool check_move(const DraughtsMove *move, void *opaque)
{
    MoveCheck *check = opaque;
    DraughtsPosition changed = check->original;
    DraughtsUndo undo;
    unsigned before = position_pieces(&changed);
    unsigned after;

    if (move->capture_count != 0) {
        if (check->saw_capture &&
            check->capture_count != move->capture_count)
            check->failed = true;
        check->saw_capture = true;
        check->capture_count = move->capture_count;
#if defined(__GNUC__) || defined(__clang__)
        if ((unsigned)__builtin_popcountll(move->captured) !=
            move->capture_count)
            check->failed = true;
#endif
    } else if (check->saw_capture) {
        check->failed = true;
    }
    if (!draughts_do_move(&changed, check->side, move, &undo) ||
        !draughts_position_is_valid(&changed)) {
        check->failed = true;
        return true;
    }
    after = position_pieces(&changed);
    if (before - after != move->capture_count)
        check->failed = true;
    draughts_undo_move(&changed, &undo);
    if (!same_position(&changed, &check->original))
        check->failed = true;
    check->destination_mask |= BIT(move->to);
    check->fingerprint_xor ^= move_fingerprint(move);
    check->fingerprint_sum += move_fingerprint(move);
    ++check->moves;
    return true;
}

static bool check_predecessor(const DraughtsPosition *predecessor,
                              const DraughtsMove *forward_move, void *opaque)
{
    PredecessorCheck *check = opaque;
    DraughtsPosition reconstructed = *predecessor;
    if (!same_material(predecessor, &check->current) ||
        draughts_has_capture(predecessor, check->mover) ||
        draughts_has_capture_padded(predecessor, check->mover) ||
        !draughts_do_move(&reconstructed, check->mover, forward_move, NULL) ||
        !same_position(&reconstructed, &check->current))
        check->failed = true;
    check->source_mask |= BIT(forward_move->from);
    check->fingerprint_xor ^=
        predecessor_fingerprint(predecessor, forward_move);
    check->fingerprint_sum +=
        predecessor_fingerprint(predecessor, forward_move);
    ++check->predecessors;
    return true;
}

static bool generate_and_check_backend(
    const DraughtsPosition *position, EgtbSide side, MoveCheck *check,
    GenerateMovesFunction generate)
{
    size_t generated;
    memset(check, 0, sizeof(*check));
    check->original = *position;
    check->side = side;
    return generate(position, side, check_move, check, &generated) &&
           generated == check->moves && !check->failed;
}

static bool same_move_result(const MoveCheck *a, const MoveCheck *b)
{
    return a->moves == b->moves &&
           a->capture_count == b->capture_count &&
           a->saw_capture == b->saw_capture &&
           a->destination_mask == b->destination_mask &&
           a->fingerprint_xor == b->fingerprint_xor &&
           a->fingerprint_sum == b->fingerprint_sum;
}

static bool generate_and_check(const DraughtsPosition *position, EgtbSide side,
                               MoveCheck *check)
{
    MoveCheck padded;
    return generate_and_check_backend(position, side, check,
                                      draughts_generate_moves) &&
           generate_and_check_backend(position, side, &padded,
                                      draughts_generate_moves_padded) &&
           same_move_result(check, &padded);
}

static bool generate_predecessors_backend(
    const DraughtsPosition *position, EgtbSide side, PredecessorCheck *check,
    GeneratePredecessorsFunction generate, size_t *generated)
{
    memset(check, 0, sizeof(*check));
    check->current = *position;
    check->mover = side == EGTB_WHITE_TO_MOVE ? EGTB_BLACK_TO_MOVE :
                                                EGTB_WHITE_TO_MOVE;
    return generate(position, side, check_predecessor, check, generated) &&
           !check->failed && *generated == check->predecessors;
}

static bool same_predecessor_result(const PredecessorCheck *a,
                                    const PredecessorCheck *b)
{
    return a->predecessors == b->predecessors &&
           a->source_mask == b->source_mask &&
           a->fingerprint_xor == b->fingerprint_xor &&
           a->fingerprint_sum == b->fingerprint_sum;
}

static bool test_quiet_man_and_promotion(void)
{
    DraughtsPosition position = {0};
    MoveCheck check;
    int from = square_at(1, 2);
    position.white_men = BIT(from);
    if (!generate_and_check(&position, EGTB_WHITE_TO_MOVE, &check) ||
        check.moves != 2 || check.saw_capture ||
        (check.destination_mask & (BIT(0) | BIT(1))) !=
            (BIT(0) | BIT(1)))
        return false;
    return true;
}

static bool test_longest_capture(void)
{
    DraughtsPosition position = {0};
    MoveCheck check;
    position.white_men =
        BIT(square_at(3, 8)) | BIT(square_at(7, 4));
    position.black_men =
        BIT(square_at(2, 7)) | BIT(square_at(6, 3)) |
        BIT(square_at(4, 3));
    if (!generate_and_check(&position, EGTB_WHITE_TO_MOVE, &check) ||
        !check.saw_capture || check.capture_count != 2 || check.moves == 0)
        return false;
    return true;
}

static bool test_king_final_landings(void)
{
    DraughtsPosition position = {0};
    MoveCheck check;
    position.white_kings = BIT(square_at(7, 0));
    position.black_men = BIT(square_at(4, 3));
    if (!generate_and_check(&position, EGTB_WHITE_TO_MOVE, &check) ||
        check.moves != 4 || check.capture_count != 1)
        return false;
    return check.destination_mask ==
           (BIT(square_at(3, 4)) | BIT(square_at(2, 5)) |
            BIT(square_at(1, 6)) | BIT(square_at(0, 7)));
}

static bool test_backward_man_capture(void)
{
    DraughtsPosition position = {0};
    MoveCheck check;
    position.white_men = BIT(square_at(3, 2));
    position.black_men = BIT(square_at(4, 3));
    if (!generate_and_check(&position, EGTB_WHITE_TO_MOVE, &check) ||
        check.moves != 1 || check.capture_count != 1)
        return false;
    return check.destination_mask == BIT(square_at(5, 4));
}

static bool test_captured_piece_remains_blocker(void)
{
    DraughtsPosition position = {0};
    MoveCheck check;
    /*
     * If captured pieces were removed during recursion, the king on 0 could
     * incorrectly capture all three black men. With delayed removal, square 6
     * blocks the loop back toward the third piece and the legal maximum is 2.
     */
    position.white_kings = BIT(0);
    position.black_men = BIT(6) | BIT(7) | BIT(16);
    return generate_and_check(&position, EGTB_WHITE_TO_MOVE, &check) &&
           check.moves == 3 && check.capture_count == 2;
}

static bool test_inverse_capture_filter(void)
{
    DraughtsPosition position = {0};
    PredecessorCheck table, padded;
    size_t table_generated, padded_generated;
    position.white_men = BIT(square_at(4, 3));
    position.black_men = BIT(square_at(4, 1));
    if (!generate_predecessors_backend(
            &position, EGTB_BLACK_TO_MOVE, &table,
            draughts_generate_quiet_predecessors, &table_generated) ||
        !generate_predecessors_backend(
            &position, EGTB_BLACK_TO_MOVE, &padded,
            draughts_generate_quiet_predecessors_padded,
            &padded_generated) ||
        !same_predecessor_result(&table, &padded) ||
        table.predecessors != 1)
        return false;
    return table.source_mask == BIT(square_at(5, 4));
}

static bool test_inverse_king_same_material(void)
{
    DraughtsPosition position = {0};
    PredecessorCheck table, padded;
    size_t table_generated, padded_generated;
    position.white_kings = BIT(0);
    return generate_predecessors_backend(
               &position, EGTB_BLACK_TO_MOVE, &table,
               draughts_generate_quiet_predecessors, &table_generated) &&
           generate_predecessors_backend(
               &position, EGTB_BLACK_TO_MOVE, &padded,
               draughts_generate_quiet_predecessors_padded,
               &padded_generated) &&
           same_predecessor_result(&table, &padded) &&
           table.predecessors == 9;
}

static bool test_random_positions(unsigned wm, unsigned bm,
                                  unsigned wk, unsigned bk)
{
    EgIndexer indexer;
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t count;
    unsigned sample;
    if (!eg_indexer_init(&indexer, wm, bm, wk, bk))
        return false;
    count = eg_position_count(&indexer);
    for (sample = 0; sample < 100000; ++sample) {
        EgPosition indexed;
        DraughtsPosition position;
        unsigned side;
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= UINT64_C(2685821657736338717);
        if (!eg_index_to_position(&indexer, state % count, &indexed)) {
            eg_indexer_destroy(&indexer);
            return false;
        }
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        for (side = 0; side < 2; ++side) {
            MoveCheck check;
            bool padded_has_capture;
            bool has_capture =
                draughts_has_capture(&position, (EgtbSide)side);
            padded_has_capture =
                draughts_has_capture_padded(&position, (EgtbSide)side);
            if (!generate_and_check(&position, (EgtbSide)side, &check) ||
                has_capture != padded_has_capture ||
                has_capture != check.saw_capture) {
                fprintf(stderr, "random move test failed at sample %u side %u\n",
                        sample, side);
                eg_indexer_destroy(&indexer);
                return false;
            }
            if (sample < 10000) {
                PredecessorCheck table, padded;
                size_t table_generated, padded_generated;
                if (!generate_predecessors_backend(
                        &position, (EgtbSide)side, &table,
                        draughts_generate_quiet_predecessors,
                        &table_generated) ||
                    !generate_predecessors_backend(
                        &position, (EgtbSide)side, &padded,
                        draughts_generate_quiet_predecessors_padded,
                        &padded_generated) ||
                    !same_predecessor_result(&table, &padded)) {
                    fprintf(stderr,
                            "random predecessor test failed at sample %u "
                            "side %u\n", sample, side);
                    eg_indexer_destroy(&indexer);
                    return false;
                }
            }
        }
    }
    eg_indexer_destroy(&indexer);
    return true;
}

int main(void)
{
    if (!test_quiet_man_and_promotion()) {
        fprintf(stderr, "quiet-man/promotion test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_longest_capture()) {
        fprintf(stderr, "longest-capture test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_king_final_landings()) {
        fprintf(stderr, "king final-landing test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_backward_man_capture()) {
        fprintf(stderr, "backward man-capture test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_captured_piece_remains_blocker()) {
        fprintf(stderr, "captured-piece blocker test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_inverse_capture_filter()) {
        fprintf(stderr, "inverse capture-filter test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_inverse_king_same_material()) {
        fprintf(stderr, "inverse king/material-preservation test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_random_positions(1, 2, 2, 2) ||
        !test_random_positions(2, 2, 2, 2) ||
        !test_random_positions(0, 7, 1, 0)) {
        fprintf(stderr, "randomized move-generation test failed: %s\n",
                draughts_movegen_last_error());
        return EXIT_FAILURE;
    }
    printf("move-generation tests: PASS (table/padded differential, 300000 "
           "random seven/eight-piece positions, both sides)\n");
    return EXIT_SUCCESS;
}
