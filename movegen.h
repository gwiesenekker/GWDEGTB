#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "egtb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRAUGHTS_BOARD_MASK ((UINT64_C(1) << 50) - 1)

typedef struct {
    uint64_t white_men;
    uint64_t black_men;
    uint64_t white_kings;
    uint64_t black_kings;
} DraughtsPosition;

typedef struct {
    uint64_t captured;
    uint8_t from;
    uint8_t to;
    uint8_t capture_count;
} DraughtsMove;

typedef struct {
    uint64_t white_men;
    uint64_t black_men;
    uint64_t white_kings;
    uint64_t black_kings;
} DraughtsUndo;

typedef bool (*DraughtsMoveVisitor)(const DraughtsMove *move, void *context);
typedef bool (*DraughtsPredecessorVisitor)(
    const DraughtsPosition *predecessor, const DraughtsMove *forward_move,
    void *context);

const char *draughts_movegen_last_error(void);
bool draughts_position_is_valid(const DraughtsPosition *position);

/*
 * Generate forced maximum captures, or quiet moves when no capture exists.
 * Capture paths with identical final effects are intentionally not deduplicated.
 * A NULL visitor performs a count-only generation.
 * Position, side, and generated-move validation is debug-only in hot functions.
 */
bool draughts_generate_moves(const DraughtsPosition *position, EgtbSide side,
                             DraughtsMoveVisitor visitor, void *context,
                             size_t *move_count);

/*
 * Alternative move generator using GWD's padded 64-bit board layout. The
 * public position and move representation remains the compact 0..49 layout.
 * BMI2 builds use PDEP/PEXT for whole-board conversion; other builds use a
 * portable set-bit fallback.
 */
bool draughts_generate_moves_padded(
    const DraughtsPosition *position, EgtbSide side,
    DraughtsMoveVisitor visitor, void *context, size_t *move_count);
bool draughts_padded_backend_uses_bmi2(void);

/* True when at least one capture is available; maximum length is not computed. */
bool draughts_has_capture(const DraughtsPosition *position, EgtbSide side);
bool draughts_has_capture_padded(const DraughtsPosition *position,
                                 EgtbSide side);

/* Apply a generated move and optionally save the four bitboards for undo. */
bool draughts_do_move(DraughtsPosition *position, EgtbSide side,
                      const DraughtsMove *move, DraughtsUndo *undo);
void draughts_undo_move(DraughtsPosition *position, const DraughtsUndo *undo);

/*
 * Generate non-capturing predecessors. current_side is the side to move in the
 * current position; the opposite side made the forward move. Candidates in
 * which that mover had a forced capture are rejected. The material signature
 * is preserved: neither the reversed move nor its forward replay may promote,
 * and inverse captures are not generated.
 */
bool draughts_generate_quiet_predecessors(
    const DraughtsPosition *position, EgtbSide current_side,
    DraughtsPredecessorVisitor visitor, void *context,
    size_t *predecessor_count);
bool draughts_generate_quiet_predecessors_padded(
    const DraughtsPosition *position, EgtbSide current_side,
    DraughtsPredecessorVisitor visitor, void *context,
    size_t *predecessor_count);

#endif
