#include "generator.h"

#include "movegen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    DraughtsPosition position;
    EgtbSide side;
    const EgIndexer *indexer;
    EgtbExternalProbe external_probe;
    void *external_context;
    uint16_t shortest_loss;
    uint16_t longest_win;
    bool has_loss;
    bool has_draw;
    bool has_internal;
    bool failed;
} SuccessorContext;

typedef struct {
    Egtb *database;
    const EgIndexer *indexer;
    EgtbSide mover;
    EgtbSide successor_side;
    EgtbLossBacktrackStatistics *statistics;
    int16_t won_distance;
    int16_t loss_value;
    EgtbExternalProbe external_probe;
    void *external_context;
    bool failed;
} BacktrackContext;

typedef struct {
    Egtb *database;
    const EgIndexer *indexer;
    EgtbSide mover;
    EgtbSide successor_side;
    DraughtsPosition predecessor;
    int16_t maximum_win;
    EgtbExternalProbe external_probe;
    void *external_context;
    bool all_winning;
    bool failed;
} ForwardCheckContext;

typedef struct {
    Egtb *database;
    const EgIndexer *indexer;
    EgtbSide mover;
    EgtbSide successor_side;
    EgtbGenericWinBacktrackStatistics *statistics;
    int16_t loss_value;
    int16_t win_value;
    EgtbExternalProbe external_probe;
    void *external_context;
    bool failed;
} WinBacktrackContext;

typedef struct {
    Egtb *database;
    const EgIndexer *indexer;
    EgtbSide mover;
    EgtbSide successor_side;
    DraughtsPosition predecessor;
    int16_t loss_value;
    EgtbExternalProbe external_probe;
    void *external_context;
    bool reaches_target_loss;
    bool failed;
} WinningForwardContext;

static char last_error[256];

static bool fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_error, sizeof(last_error), format, arguments);
    va_end(arguments);
    return false;
}

const char *egtb_generator_last_error(void)
{
    return last_error;
}

static EgtbSide opposite_side(EgtbSide side)
{
    return side == EGTB_WHITE_TO_MOVE ? EGTB_BLACK_TO_MOVE :
                                        EGTB_WHITE_TO_MOVE;
}

static unsigned generator_bit_count(uint64_t value);
static bool has_indexer_material(const EgIndexer *indexer,
                                 const DraughtsPosition *position);
static bool valid_dtm(int16_t value);

static bool examine_successor(const DraughtsMove *move, void *opaque)
{
    SuccessorContext *context = opaque;
    DraughtsPosition successor = context->position;
    EgtbSide successor_side = opposite_side(context->side);
    uint64_t friendly;
    int16_t value;
    if (!draughts_do_move(&successor, context->side, move, NULL)) {
        context->failed = true;
        return false;
    }
    friendly = successor_side == EGTB_WHITE_TO_MOVE
                   ? successor.white_men | successor.white_kings
                   : successor.black_men | successor.black_kings;
    if (friendly == 0) {
        value = 0;
    } else if (has_indexer_material(context->indexer, &successor)) {
        context->has_internal = true;
        return true;
    } else if (context->external_probe == NULL ||
               !context->external_probe(&successor, successor_side,
                                        context->external_context, &value) ||
               !valid_dtm(value)) {
        context->failed = true;
        return false;
    }
    if (value == EGTB_DRAW) {
        context->has_draw = true;
    } else if (value <= 0) {
        uint16_t distance = (uint16_t)-value;
        if (!context->has_loss || distance < context->shortest_loss)
            context->shortest_loss = distance;
        context->has_loss = true;
    } else if ((uint16_t)value > context->longest_win) {
        context->longest_win = (uint16_t)value;
    }
    return true;
}

bool egtb_initialize_terminal_positions_with_probe(
    Egtb *database, const EgIndexer *indexer,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbInitializationStatistics *statistics)
{
    EgtbInitializationStatistics local = {0};
    uint64_t position_count;
    uint64_t index;
    if (database == NULL || indexer == NULL)
        return fail("invalid EGTB initialization argument");
    position_count = eg_position_count(indexer);
    if (position_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    local.positions = position_count;
    for (index = 0; index < position_count; ++index) {
        EgPosition indexed;
        DraughtsPosition position;
        unsigned side;
        if (!eg_index_to_position(indexer, index, &indexed))
            return fail("cannot invert index %llu",
                        (unsigned long long)index);
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        for (side = 0; side < 2; ++side) {
            SuccessorContext context;
            size_t move_count;
            int16_t value = EGTB_DRAW;
            memset(&context, 0, sizeof(context));
            context.position = position;
            context.side = (EgtbSide)side;
            context.indexer = indexer;
            context.external_probe = external_probe;
            context.external_context = external_context;
            if (!draughts_generate_moves(&position, (EgtbSide)side,
                                         examine_successor, &context,
                                         &move_count) || context.failed)
                return fail("move generation failed at index %llu, side %u: %s",
                            (unsigned long long)index, side,
                            draughts_movegen_last_error());
            local.legal_moves[side] += move_count;
            if (move_count == 0) {
                value = 0;
                ++local.lost_in_zero[side];
            } else if (context.has_loss) {
                if (context.shortest_loss >= INT16_MAX)
                    return fail("initialized winning DTM is too large");
                value = (int16_t)(context.shortest_loss + 1);
                if (value == 1)
                    ++local.won_in_one[side];
                else
                    ++local.external_wins[side];
            } else if (!context.has_internal && !context.has_draw) {
                if (context.longest_win >= INT16_MAX)
                    return fail("initialized losing DTM is too large");
                value = (int16_t)-(context.longest_win + 1);
                ++local.external_losses[side];
            } else {
                ++local.unknown[side];
            }
            if (value != EGTB_DRAW &&
                !egtb_set(database, index, (EgtbSide)side, value))
                return fail("cannot write index %llu, side %u: %s",
                            (unsigned long long)index, side,
                            egtb_last_error());
        }
    }
    if (statistics != NULL)
        *statistics = local;
    return true;
}

bool egtb_initialize_terminal_positions(
    Egtb *database, const EgIndexer *indexer,
    EgtbInitializationStatistics *statistics)
{
    return egtb_initialize_terminal_positions_with_probe(
        database, indexer, NULL, NULL, statistics);
}

static bool position_index(const EgIndexer *indexer,
                           const DraughtsPosition *position, uint64_t *index)
{
    EgPosition indexed;
    indexed.white_men = position->white_men;
    indexed.black_men = position->black_men;
    indexed.white_kings = position->white_kings;
    indexed.black_kings = position->black_kings;
    return eg_position_to_index(indexer, &indexed, index);
}

static bool check_forward_successor(const DraughtsMove *move, void *opaque)
{
    ForwardCheckContext *context = opaque;
    DraughtsPosition successor = context->predecessor;
    uint64_t index;
    uint64_t friendly;
    int16_t value;
    if (!draughts_do_move(&successor, context->mover, move, NULL)) {
        context->failed = true;
        return false;
    }
    friendly = context->successor_side == EGTB_WHITE_TO_MOVE
                   ? successor.white_men | successor.white_kings
                   : successor.black_men | successor.black_kings;
    if (friendly == 0) {
        value = 0;
    } else if (has_indexer_material(context->indexer, &successor)) {
        if (!position_index(context->indexer, &successor, &index) ||
            !egtb_get(context->database, index, context->successor_side,
                      &value)) {
            context->failed = true;
            return false;
        }
    } else if (context->external_probe == NULL ||
               !context->external_probe(&successor, context->successor_side,
                                        context->external_context, &value) ||
               !valid_dtm(value)) {
        context->failed = true;
        return false;
    }
    if (value <= 0) {
        context->all_winning = false;
    } else if (value > context->maximum_win) {
        context->maximum_win = value;
    }
    return true;
}

static bool check_predecessor(const DraughtsPosition *predecessor,
                              const DraughtsMove *forward_move, void *opaque)
{
    BacktrackContext *context = opaque;
    ForwardCheckContext forward;
    uint64_t predecessor_index;
    size_t move_count;
    int16_t old_value;
    (void)forward_move;
    ++context->statistics->predecessor_candidates[context->mover];
    memset(&forward, 0, sizeof(forward));
    forward.database = context->database;
    forward.indexer = context->indexer;
    forward.mover = context->mover;
    forward.successor_side = context->successor_side;
    forward.predecessor = *predecessor;
    forward.external_probe = context->external_probe;
    forward.external_context = context->external_context;
    forward.all_winning = true;
    if (!draughts_generate_moves(predecessor, context->mover,
                                 check_forward_successor, &forward,
                                 &move_count) || forward.failed) {
        context->failed = true;
        return false;
    }
    if (move_count == 0 || !forward.all_winning ||
        forward.maximum_win != context->won_distance)
        return true;
    if (!position_index(context->indexer, predecessor, &predecessor_index) ||
        !egtb_get(context->database, predecessor_index, context->mover,
                  &old_value)) {
        context->failed = true;
        return false;
    }
    if (old_value == context->loss_value ||
        (old_value != EGTB_DRAW && old_value < 0 &&
         old_value > context->loss_value))
        return true;
    if (old_value != EGTB_DRAW && old_value >= 0) {
        context->failed = true;
        return false;
    }
    if (!egtb_set(context->database, predecessor_index, context->mover,
                  context->loss_value)) {
        context->failed = true;
        return false;
    }
    ++context->statistics->losses[context->mover];
    if (old_value < context->loss_value && old_value != EGTB_DRAW)
        ++context->statistics->shortened_losses[context->mover];
    return true;
}

bool egtb_backtrack_wins_to_losses_with_probe(
    Egtb *database, const EgIndexer *indexer, int16_t won_distance,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbLossBacktrackStatistics *statistics)
{
    EgtbLossBacktrackStatistics local = {0};
    uint64_t position_count;
    uint64_t index;
    if (database == NULL || indexer == NULL || won_distance <= 0 ||
        won_distance % 2 == 0 || won_distance == INT16_MAX)
        return fail("invalid win-to-loss backtrack argument");
    position_count = eg_position_count(indexer);
    if (position_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    for (index = 0; index < position_count; ++index) {
        EgPosition indexed;
        DraughtsPosition position;
        unsigned successor_side;
        if (!eg_index_to_position(indexer, index, &indexed))
            return fail("cannot invert index %llu",
                        (unsigned long long)index);
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        for (successor_side = 0; successor_side < 2; ++successor_side) {
            BacktrackContext context;
            EgtbSide mover = opposite_side((EgtbSide)successor_side);
            int16_t value;
            size_t predecessor_count;
            if (!egtb_get(database, index, (EgtbSide)successor_side, &value))
                return fail("cannot read index %llu, side %u: %s",
                            (unsigned long long)index, successor_side,
                            egtb_last_error());
            if (value != won_distance)
                continue;
            ++local.won_sources[mover];
            memset(&context, 0, sizeof(context));
            context.database = database;
            context.indexer = indexer;
            context.mover = mover;
            context.successor_side = (EgtbSide)successor_side;
            context.statistics = &local;
            context.won_distance = won_distance;
            context.loss_value = (int16_t)-(won_distance + 1);
            context.external_probe = external_probe;
            context.external_context = external_context;
            if (!draughts_generate_quiet_predecessors(
                    &position, (EgtbSide)successor_side, check_predecessor,
                    &context, &predecessor_count) || context.failed)
                return fail("predecessor backtrack failed at index %llu, side %u",
                            (unsigned long long)index, successor_side);
        }
    }
    if (statistics != NULL)
        *statistics = local;
    return true;
}

bool egtb_backtrack_wins_to_losses(
    Egtb *database, const EgIndexer *indexer, int16_t won_distance,
    EgtbLossBacktrackStatistics *statistics)
{
    return egtb_backtrack_wins_to_losses_with_probe(
        database, indexer, won_distance, NULL, NULL, statistics);
}

bool egtb_backtrack_won_in_one(Egtb *database, const EgIndexer *indexer,
                               EgtbBacktrackStatistics *statistics)
{
    EgtbLossBacktrackStatistics generic;
    unsigned side;
    if (!egtb_backtrack_wins_to_losses(database, indexer, 1, &generic))
        return false;
    if (statistics != NULL) {
        memset(statistics, 0, sizeof(*statistics));
        for (side = 0; side < 2; ++side) {
            statistics->won_in_one_sources[side] =
                generic.won_sources[side];
            statistics->predecessor_candidates[side] =
                generic.predecessor_candidates[side];
            statistics->lost_in_two[side] = generic.losses[side];
        }
    }
    return true;
}

static bool check_winning_successor(const DraughtsMove *move, void *opaque)
{
    WinningForwardContext *context = opaque;
    DraughtsPosition successor = context->predecessor;
    uint64_t index;
    uint64_t friendly;
    int16_t value;
    if (!draughts_do_move(&successor, context->mover, move, NULL)) {
        context->failed = true;
        return false;
    }
    friendly = context->successor_side == EGTB_WHITE_TO_MOVE
                   ? successor.white_men | successor.white_kings
                   : successor.black_men | successor.black_kings;
    if (friendly == 0) {
        value = 0;
    } else if (has_indexer_material(context->indexer, &successor)) {
        if (!position_index(context->indexer, &successor, &index) ||
            !egtb_get(context->database, index, context->successor_side,
                      &value)) {
            context->failed = true;
            return false;
        }
    } else if (context->external_probe == NULL ||
               !context->external_probe(&successor, context->successor_side,
                                        context->external_context, &value) ||
               !valid_dtm(value)) {
        context->failed = true;
        return false;
    }
    if (value == context->loss_value)
        context->reaches_target_loss = true;
    return true;
}

static bool check_winning_predecessor(
    const DraughtsPosition *predecessor,
    const DraughtsMove *forward_move, void *opaque)
{
    WinBacktrackContext *context = opaque;
    WinningForwardContext forward;
    uint64_t predecessor_index;
    size_t move_count;
    int16_t old_value;
    (void)forward_move;
    ++context->statistics->predecessor_candidates[context->mover];
    memset(&forward, 0, sizeof(forward));
    forward.database = context->database;
    forward.indexer = context->indexer;
    forward.mover = context->mover;
    forward.successor_side = context->successor_side;
    forward.predecessor = *predecessor;
    forward.loss_value = context->loss_value;
    forward.external_probe = context->external_probe;
    forward.external_context = context->external_context;
    if (!draughts_generate_moves(predecessor, context->mover,
                                 check_winning_successor, &forward,
                                 &move_count) || forward.failed) {
        context->failed = true;
        return false;
    }
    if (move_count == 0 || !forward.reaches_target_loss)
        return true;
    if (!position_index(context->indexer, predecessor, &predecessor_index) ||
        !egtb_get(context->database, predecessor_index, context->mover,
                  &old_value)) {
        context->failed = true;
        return false;
    }
    if (old_value > 0 && old_value <= context->win_value)
        return true;
    if (old_value != EGTB_DRAW && old_value <= 0) {
        context->failed = true;
        return false;
    }
    if (!egtb_set(context->database, predecessor_index, context->mover,
                  context->win_value)) {
        context->failed = true;
        return false;
    }
    ++context->statistics->wins[context->mover];
    if (old_value > context->win_value)
        ++context->statistics->shortened_wins[context->mover];
    return true;
}

bool egtb_backtrack_losses_to_wins_with_probe(
    Egtb *database, const EgIndexer *indexer, int16_t loss_distance,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbGenericWinBacktrackStatistics *statistics)
{
    EgtbGenericWinBacktrackStatistics local = {0};
    uint64_t position_count;
    uint64_t index;
    if (database == NULL || indexer == NULL || loss_distance <= 0 ||
        loss_distance % 2 != 0 || loss_distance == INT16_MAX)
        return fail("invalid loss-to-win backtrack argument");
    position_count = eg_position_count(indexer);
    if (position_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    for (index = 0; index < position_count; ++index) {
        EgPosition indexed;
        DraughtsPosition position;
        unsigned successor_side;
        if (!eg_index_to_position(indexer, index, &indexed))
            return fail("cannot invert index %llu",
                        (unsigned long long)index);
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        for (successor_side = 0; successor_side < 2; ++successor_side) {
            WinBacktrackContext context;
            EgtbSide mover = opposite_side((EgtbSide)successor_side);
            int16_t value;
            size_t predecessor_count;
            if (!egtb_get(database, index, (EgtbSide)successor_side, &value))
                return fail("cannot read index %llu, side %u: %s",
                            (unsigned long long)index, successor_side,
                            egtb_last_error());
            if (value != -loss_distance)
                continue;
            ++local.loss_sources[mover];
            memset(&context, 0, sizeof(context));
            context.database = database;
            context.indexer = indexer;
            context.mover = mover;
            context.successor_side = (EgtbSide)successor_side;
            context.statistics = &local;
            context.loss_value = (int16_t)-loss_distance;
            context.win_value = (int16_t)(loss_distance + 1);
            context.external_probe = external_probe;
            context.external_context = external_context;
            if (!draughts_generate_quiet_predecessors(
                    &position, (EgtbSide)successor_side,
                    check_winning_predecessor, &context,
                    &predecessor_count) || context.failed)
                return fail("winning predecessor backtrack failed at index %llu, side %u",
                            (unsigned long long)index, successor_side);
        }
    }
    if (statistics != NULL)
        *statistics = local;
    return true;
}

bool egtb_backtrack_losses_to_wins(
    Egtb *database, const EgIndexer *indexer, int16_t loss_distance,
    EgtbGenericWinBacktrackStatistics *statistics)
{
    return egtb_backtrack_losses_to_wins_with_probe(
        database, indexer, loss_distance, NULL, NULL, statistics);
}

bool egtb_backtrack_lost_in_two(Egtb *database, const EgIndexer *indexer,
                                EgtbWinBacktrackStatistics *statistics)
{
    EgtbGenericWinBacktrackStatistics generic;
    unsigned side;
    if (!egtb_backtrack_losses_to_wins(database, indexer, 2, &generic))
        return false;
    if (statistics != NULL) {
        memset(statistics, 0, sizeof(*statistics));
        for (side = 0; side < 2; ++side) {
            statistics->lost_in_two_sources[side] =
                generic.loss_sources[side];
            statistics->predecessor_candidates[side] =
                generic.predecessor_candidates[side];
            statistics->won_in_three[side] = generic.wins[side];
            statistics->shortened_wins[side] =
                generic.shortened_wins[side];
        }
    }
    return true;
}

typedef struct {
    Egtb *database;
    const EgIndexer *indexer;
    DraughtsPosition position;
    EgtbSide mover;
    EgtbSide successor_side;
    EgtbExternalProbe external_probe;
    void *external_context;
    uint16_t shortest_loss;
    uint16_t longest_win;
    bool has_loss;
    bool has_draw;
    bool failed;
} ConsistencyMoveContext;

static unsigned generator_bit_count(uint64_t value)
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

static bool has_indexer_material(const EgIndexer *indexer,
                                 const DraughtsPosition *position)
{
    return generator_bit_count(position->white_men) == indexer->white_men &&
           generator_bit_count(position->black_men) == indexer->black_men &&
           generator_bit_count(position->white_kings) == indexer->white_kings &&
           generator_bit_count(position->black_kings) == indexer->black_kings;
}

static bool valid_dtm(int16_t value)
{
    return value == EGTB_DRAW || (value > 0 && value % 2 != 0) ||
           (value <= 0 && value % 2 == 0);
}

static bool resolve_successor(ConsistencyMoveContext *context,
                              const DraughtsPosition *successor,
                              int16_t *value)
{
    uint64_t friendly = context->successor_side == EGTB_WHITE_TO_MOVE
                            ? successor->white_men | successor->white_kings
                            : successor->black_men | successor->black_kings;
    if (friendly == 0) {
        *value = 0;
        return true;
    }
    if (has_indexer_material(context->indexer, successor)) {
        uint64_t index;
        if (!position_index(context->indexer, successor, &index) ||
            !egtb_get(context->database, index, context->successor_side,
                      value))
            return false;
        return true;
    }
    return context->external_probe != NULL &&
           context->external_probe(successor, context->successor_side,
                                   context->external_context, value) &&
           valid_dtm(*value);
}

static bool evaluate_consistency_move(const DraughtsMove *move, void *opaque)
{
    ConsistencyMoveContext *context = opaque;
    DraughtsPosition successor = context->position;
    int16_t value;
    if (!draughts_do_move(&successor, context->mover, move, NULL) ||
        !resolve_successor(context, &successor, &value)) {
        context->failed = true;
        return false;
    }
    if (value == EGTB_DRAW) {
        context->has_draw = true;
    } else if (value <= 0) {
        uint16_t distance = (uint16_t)-value;
        if (!context->has_loss || distance < context->shortest_loss)
            context->shortest_loss = distance;
        context->has_loss = true;
    } else if ((uint16_t)value > context->longest_win) {
        context->longest_win = (uint16_t)value;
    }
    return true;
}

static bool expected_dtm(Egtb *database, const EgIndexer *indexer,
                         const DraughtsPosition *position, EgtbSide side,
                         EgtbExternalProbe external_probe,
                         void *external_context, int16_t *value)
{
    ConsistencyMoveContext context;
    size_t move_count;
    memset(&context, 0, sizeof(context));
    context.database = database;
    context.indexer = indexer;
    context.position = *position;
    context.mover = side;
    context.successor_side = opposite_side(side);
    context.external_probe = external_probe;
    context.external_context = external_context;
    if (!draughts_generate_moves(position, side, evaluate_consistency_move,
                                 &context, &move_count) || context.failed)
        return false;
    if (move_count == 0) {
        *value = 0;
    } else if (context.has_loss) {
        if (context.shortest_loss >= INT16_MAX)
            return false;
        *value = (int16_t)(context.shortest_loss + 1);
    } else if (context.has_draw) {
        *value = EGTB_DRAW;
    } else {
        if (context.longest_win >= INT16_MAX)
            return false;
        *value = (int16_t)-(context.longest_win + 1);
    }
    return valid_dtm(*value);
}

bool egtb_make_consistent(
    Egtb *database, const EgIndexer *indexer,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbConsistencyReporter reporter, void *reporter_context,
    EgtbConsistencyStatistics *statistics)
{
    EgtbConsistencyStatistics local = {0};
    uint64_t position_count;
    if (database == NULL || indexer == NULL)
        return fail("invalid consistency-pass argument");
    position_count = eg_position_count(indexer);
    if (position_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    for (;;) {
        uint64_t updates_this_pass = 0;
        uint64_t index;
        ++local.passes;
        for (index = 0; index < position_count; ++index) {
            EgPosition indexed;
            DraughtsPosition position;
            unsigned side;
            if (!eg_index_to_position(indexer, index, &indexed))
                return fail("cannot invert index %llu",
                            (unsigned long long)index);
            position.white_men = indexed.white_men;
            position.black_men = indexed.black_men;
            position.white_kings = indexed.white_kings;
            position.black_kings = indexed.black_kings;
            for (side = 0; side < 2; ++side) {
                int16_t old_value, new_value;
                if (!egtb_get(database, index, (EgtbSide)side, &old_value) ||
                    !expected_dtm(database, indexer, &position,
                                  (EgtbSide)side, external_probe,
                                  external_context, &new_value))
                    return fail("cannot verify index %llu, side %u",
                                (unsigned long long)index, side);
                ++local.positions_checked;
                if (old_value == new_value)
                    continue;
                if (reporter != NULL)
                    reporter(index, (EgtbSide)side, &position, old_value,
                             new_value, reporter_context);
                if (old_value > 0 && new_value > 0 &&
                    new_value < old_value)
                    ++local.shorter_wins[side];
                else if (old_value != EGTB_DRAW && old_value <= 0 &&
                         new_value <= 0 && new_value < old_value)
                    ++local.longer_losses[side];
                else
                    ++local.other_updates[side];
                if (!egtb_set(database, index, (EgtbSide)side, new_value))
                    return fail("cannot correct index %llu, side %u: %s",
                                (unsigned long long)index, side,
                                egtb_last_error());
                ++local.updates[side];
                ++updates_this_pass;
            }
        }
        if (updates_this_pass == 0)
            break;
    }
    if (statistics != NULL)
        *statistics = local;
    return true;
}

bool egtb_generate(Egtb *database, const EgIndexer *indexer,
                   EgtbExternalProbe external_probe,
                   void *external_context,
                   EgtbConsistencyReporter reporter,
                   void *reporter_context,
                   EgtbGenerationStatistics *statistics)
{
    EgtbGenerationStatistics local;
    EgtbConsistencyStatistics consistency;
    int16_t won_distance = 1;
    unsigned side;
    memset(&local, 0, sizeof(local));
    if (!egtb_initialize_terminal_positions_with_probe(
            database, indexer, external_probe, external_context,
            &local.initialization))
        return false;
    if (local.initialization.won_in_one[0] != 0 ||
        local.initialization.won_in_one[1] != 0)
        local.maximum_dtm = 1;
    for (;;) {
        EgtbLossBacktrackStatistics losses;
        EgtbGenericWinBacktrackStatistics wins;
        uint64_t loss_updates = 0;
        uint64_t win_updates = 0;
        int16_t loss_distance;
        if (won_distance == INT16_MAX)
            return fail("DTM exceeds the signed 16-bit representation");
        loss_distance = (int16_t)(won_distance + 1);
        if (!egtb_backtrack_wins_to_losses_with_probe(
                database, indexer, won_distance, external_probe,
                external_context, &losses))
            return false;
        ++local.retrograde_passes;
        for (side = 0; side < 2; ++side) {
            loss_updates += losses.losses[side];
            local.shortened_losses[side] += losses.shortened_losses[side];
            local.new_losses[side] +=
                losses.losses[side] - losses.shortened_losses[side];
        }
        if (loss_updates == 0)
            break;
        local.maximum_dtm = (uint16_t)loss_distance;
        if (!egtb_backtrack_losses_to_wins_with_probe(
                database, indexer, loss_distance, external_probe,
                external_context, &wins))
            return false;
        ++local.retrograde_passes;
        for (side = 0; side < 2; ++side) {
            win_updates += wins.wins[side];
            local.shortened_wins[side] += wins.shortened_wins[side];
            local.new_wins[side] +=
                wins.wins[side] - wins.shortened_wins[side];
        }
        if (win_updates == 0)
            break;
        local.maximum_dtm = (uint16_t)(loss_distance + 1);
        if (won_distance > INT16_MAX - 2)
            return fail("DTM exceeds the signed 16-bit representation");
        won_distance = (int16_t)(won_distance + 2);
    }
    if (!egtb_make_consistent(database, indexer, external_probe,
                              external_context, reporter, reporter_context,
                              &consistency))
        return false;
    local.consistency_passes = consistency.passes;
    for (side = 0; side < 2; ++side)
        local.consistency_updates[side] = consistency.updates[side];
    local.maximum_dtm = 0;
    for (uint64_t index = 0; index < eg_position_count(indexer); ++index) {
        for (side = 0; side < 2; ++side) {
            int16_t value;
            uint16_t distance;
            if (!egtb_get(database, index, (EgtbSide)side, &value))
                return false;
            if (value == EGTB_DRAW)
                continue;
            distance = value < 0 ? (uint16_t)-value : (uint16_t)value;
            if (distance > local.maximum_dtm)
                local.maximum_dtm = distance;
        }
    }
    if (statistics != NULL)
        *statistics = local;
    return true;
}
