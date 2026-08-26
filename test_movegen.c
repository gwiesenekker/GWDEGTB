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
} MoveCheck;

typedef struct {
    DraughtsPosition current;
    EgtbSide mover;
    size_t predecessors;
    bool failed;
    uint64_t source_mask;
} PredecessorCheck;

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
        !draughts_do_move(&reconstructed, check->mover, forward_move, NULL) ||
        !same_position(&reconstructed, &check->current))
        check->failed = true;
    check->source_mask |= BIT(forward_move->from);
    ++check->predecessors;
    return true;
}

static bool generate_and_check(const DraughtsPosition *position, EgtbSide side,
                               MoveCheck *check)
{
    size_t generated;
    memset(check, 0, sizeof(*check));
    check->original = *position;
    check->side = side;
    return draughts_generate_moves(position, side, check_move, check,
                                   &generated) &&
           generated == check->moves && !check->failed;
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
    PredecessorCheck check;
    size_t generated;
    position.white_men = BIT(square_at(4, 3));
    position.black_men = BIT(square_at(4, 1));
    memset(&check, 0, sizeof(check));
    check.current = position;
    check.mover = EGTB_WHITE_TO_MOVE;
    if (!draughts_generate_quiet_predecessors(
            &position, EGTB_BLACK_TO_MOVE, check_predecessor, &check,
            &generated) ||
        check.failed || generated != check.predecessors ||
        check.predecessors != 1)
        return false;
    return check.source_mask == BIT(square_at(5, 4));
}

static bool test_inverse_king_same_material(void)
{
    DraughtsPosition position = {0};
    PredecessorCheck check;
    size_t generated;
    position.white_kings = BIT(0);
    memset(&check, 0, sizeof(check));
    check.current = position;
    check.mover = EGTB_WHITE_TO_MOVE;
    return draughts_generate_quiet_predecessors(
               &position, EGTB_BLACK_TO_MOVE, check_predecessor, &check,
               &generated) &&
           !check.failed && generated == check.predecessors &&
           check.predecessors == 9;
}

static bool test_random_positions(void)
{
    EgIndexer indexer;
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t count;
    unsigned sample;
    if (!eg_indexer_init(&indexer, 1, 2, 2, 2))
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
            bool has_capture =
                draughts_has_capture(&position, (EgtbSide)side);
            if (!generate_and_check(&position, (EgtbSide)side, &check) ||
                has_capture != check.saw_capture) {
                fprintf(stderr, "random move test failed at sample %u side %u\n",
                        sample, side);
                eg_indexer_destroy(&indexer);
                return false;
            }
            if (sample < 10000) {
                PredecessorCheck predecessor_check;
                size_t generated;
                memset(&predecessor_check, 0, sizeof(predecessor_check));
                predecessor_check.current = position;
                predecessor_check.mover =
                    side == EGTB_WHITE_TO_MOVE ? EGTB_BLACK_TO_MOVE :
                                                 EGTB_WHITE_TO_MOVE;
                if (!draughts_generate_quiet_predecessors(
                        &position, (EgtbSide)side, check_predecessor,
                        &predecessor_check, &generated) ||
                    predecessor_check.failed ||
                    generated != predecessor_check.predecessors) {
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
    if (!test_random_positions()) {
        fprintf(stderr, "randomized move-generation test failed: %s\n",
                draughts_movegen_last_error());
        return EXIT_FAILURE;
    }
    printf("move-generation tests: PASS (100000 random seven-piece positions, "
           "both sides)\n");
    return EXIT_SUCCESS;
}
