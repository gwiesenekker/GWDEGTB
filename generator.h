#ifndef GENERATOR_H
#define GENERATOR_H

#include "egtb.h"
#include "endgame_index.h"
#include "movegen.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t positions;
    uint64_t legal_moves[2];
    uint64_t lost_in_zero[2];
    uint64_t won_in_one[2];
    uint64_t external_wins[2];
    uint64_t external_losses[2];
    uint64_t unknown[2];
} EgtbInitializationStatistics;

typedef struct {
    /* Indexed by the side to move in the newly found lost-in-two position. */
    uint64_t won_in_one_sources[2];
    uint64_t predecessor_candidates[2];
    uint64_t lost_in_two[2];
} EgtbBacktrackStatistics;

typedef struct {
    /* Indexed by the side to move in the newly found losing position. */
    uint64_t won_sources[2];
    uint64_t predecessor_candidates[2];
    uint64_t losses[2];
    uint64_t shortened_losses[2];
} EgtbLossBacktrackStatistics;

typedef struct {
    /* Indexed by the side to move in the newly found won-in-three position. */
    uint64_t lost_in_two_sources[2];
    uint64_t predecessor_candidates[2];
    uint64_t won_in_three[2];
    uint64_t shortened_wins[2];
} EgtbWinBacktrackStatistics;

typedef struct {
    /* Indexed by the side to move in the newly found winning position. */
    uint64_t loss_sources[2];
    uint64_t predecessor_candidates[2];
    uint64_t wins[2];
    uint64_t shortened_wins[2];
} EgtbGenericWinBacktrackStatistics;

typedef struct {
    EgtbInitializationStatistics initialization;
    uint64_t retrograde_passes;
    uint64_t new_losses[2];
    uint64_t new_wins[2];
    uint64_t shortened_losses[2];
    uint64_t shortened_wins[2];
    uint64_t consistency_passes;
    uint64_t consistency_updates[2];
    uint16_t maximum_dtm;
} EgtbGenerationStatistics;

typedef bool (*EgtbExternalProbe)(
    const DraughtsPosition *position, EgtbSide side, void *context,
    int16_t *value);

typedef void (*EgtbConsistencyReporter)(
    uint64_t index, EgtbSide side, const DraughtsPosition *position,
    int16_t old_value, int16_t new_value, void *context);

typedef struct {
    uint64_t passes;
    uint64_t positions_checked;
    uint64_t updates[2];
    uint64_t shorter_wins[2];
    uint64_t longer_losses[2];
    uint64_t other_updates[2];
} EgtbConsistencyStatistics;

const char *egtb_generator_last_error(void);

/*
 * Initialize both side-to-move values of an all-draw database. Positions with
 * no legal move are lost in zero; positions with a move to such a position are
 * won in one. Every other value remains draw/unknown. This function neither
 * flushes nor closes the database; the same open handle is used by later
 * retrograde passes and is finalized only after convergence.
 */
bool egtb_initialize_terminal_positions(
    Egtb *database, const EgIndexer *indexer,
    EgtbInitializationStatistics *statistics);

bool egtb_initialize_terminal_positions_with_probe(
    Egtb *database, const EgIndexer *indexer,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbInitializationStatistics *statistics);

/*
 * Backtrack won-in-one entries through legal inverse quiet moves. A
 * predecessor is stored as lost in two only when all of its legal forward
 * moves lead to an opponent won-in-one entry. The database remains open and
 * is not flushed.
 */
bool egtb_backtrack_won_in_one(Egtb *database, const EgIndexer *indexer,
                               EgtbBacktrackStatistics *statistics);

/*
 * General form of the preceding pass. won_distance must be positive and odd.
 * A predecessor is lost in won_distance + 1 when every successor is a known
 * win no longer than won_distance and at least one is exactly won_distance.
 */
bool egtb_backtrack_wins_to_losses(
    Egtb *database, const EgIndexer *indexer, int16_t won_distance,
    EgtbLossBacktrackStatistics *statistics);

/*
 * Backtrack lost-in-two entries through legal inverse quiet moves. A
 * predecessor is won in three when at least one legal forward move reaches an
 * opponent lost-in-two entry. Unknown entries and wins longer than three are
 * updated; shorter wins are preserved. The database is not flushed.
 */
bool egtb_backtrack_lost_in_two(Egtb *database, const EgIndexer *indexer,
                                EgtbWinBacktrackStatistics *statistics);

/* General lost-to-win pass. loss_distance must be nonzero and even. */
bool egtb_backtrack_losses_to_wins(
    Egtb *database, const EgIndexer *indexer, int16_t loss_distance,
    EgtbGenericWinBacktrackStatistics *statistics);

/*
 * Initialize and alternate win-to-loss and loss-to-win layers until a layer
 * creates no new value. The caller supplies a new all-draw database. It stays
 * open and unflushed on return so finalization remains the caller's decision.
 */
bool egtb_generate(Egtb *database, const EgIndexer *indexer,
                   EgtbExternalProbe external_probe,
                   void *external_context,
                   EgtbConsistencyReporter reporter,
                   void *reporter_context,
                   EgtbGenerationStatistics *statistics);

/*
 * Recompute every value from its legal successors until a fixed point. Moves
 * that leave the current material signature are resolved through external_probe;
 * a NULL probe is sufficient only when every such move is an immediate
 * zero-piece terminal. Corrections are reported before they are written.
 */
bool egtb_make_consistent(
    Egtb *database, const EgIndexer *indexer,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbConsistencyReporter reporter, void *reporter_context,
    EgtbConsistencyStatistics *statistics);

#endif
