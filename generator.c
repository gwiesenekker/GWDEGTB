#include "generator.h"

#include "bitmap.h"
#include "frontier.h"
#include "movegen.h"

#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(EGTB_PADDED_MOVEGEN)
#define GENERATE_MOVES draughts_generate_moves_padded
#define GENERATE_QUIET_PREDECESSORS \
    draughts_generate_quiet_predecessors_padded
#else
#define GENERATE_MOVES draughts_generate_moves
#define GENERATE_QUIET_PREDECESSORS draughts_generate_quiet_predecessors
#endif

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void add_cache_statistics(EgtbCacheStatistics *total,
                                 const EgtbCacheStatistics *part)
{
    total->lookups += part->lookups;
    total->hits += part->hits;
    total->misses += part->misses;
    total->decompressions += part->decompressions;
    total->dirty_evictions += part->dirty_evictions;
    total->compressed_writes += part->compressed_writes;
}

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
    EgtbView *view;
    const EgIndexer *indexer;
    EgtbSide mover;
    EgtbSide successor_side;
    EgtbLossBacktrackStatistics *statistics;
    int16_t won_distance;
    int16_t loss_value;
    EgtbExternalProbe external_probe;
    void *external_context;
    const Bitmap *won_exact;
    const Bitmap *won_at_most;
    uint64_t first_index;
    uint64_t end_index;
    bool failed;
} BacktrackContext;

typedef struct {
    const EgIndexer *indexer;
    EgtbSide mover;
    EgtbSide successor_side;
    DraughtsPosition predecessor;
    int16_t won_distance;
    EgtbExternalProbe external_probe;
    void *external_context;
    const Bitmap *won_exact;
    const Bitmap *won_at_most;
    bool all_winning;
    bool reaches_exact_win;
    bool failed;
} ForwardCheckContext;

typedef struct {
    Egtb *database;
    EgtbView *view;
    const EgIndexer *indexer;
    EgtbSide mover;
    EgtbGenericWinBacktrackStatistics *statistics;
    int16_t win_value;
    uint64_t first_index;
    uint64_t end_index;
    bool failed;
} WinBacktrackContext;

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
static bool contains_position(const EgIndexer *indexer,
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
    } else if (contains_position(context->indexer, &successor)) {
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
            if (!GENERATE_MOVES(&position, (EgtbSide)side,
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
                if (context.shortest_loss >= EGTB_MAX_WIN_DTM)
                    return fail("initialized winning DTM exceeds 16-bit storage");
                value = (int16_t)(context.shortest_loss + 1);
                if (value == 1)
                    ++local.won_in_one[side];
                else
                    ++local.external_wins[side];
            } else if (!context.has_internal && !context.has_draw) {
                if (context.longest_win >= EGTB_MAX_LOSS_DTM)
                    return fail("initialized losing DTM exceeds 16-bit storage");
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
    if (!eg_indexer_contains_position(indexer, &indexed))
        return false;
    return eg_position_to_index(indexer, &indexed, index);
}

static bool contains_position(const EgIndexer *indexer,
                              const DraughtsPosition *position)
{
    EgPosition indexed;
    indexed.white_men = position->white_men;
    indexed.black_men = position->black_men;
    indexed.white_kings = position->white_kings;
    indexed.black_kings = position->black_kings;
    return eg_indexer_contains_position(indexer, &indexed);
}

static bool generator_get(Egtb *database, EgtbView *view, uint64_t index,
                          EgtbSide side, int16_t *value)
{
    return view != NULL ? egtb_view_get(view, index, side, value) :
                          egtb_get(database, index, side, value);
}

static bool generator_set(Egtb *database, EgtbView *view, uint64_t index,
                          EgtbSide side, int16_t value)
{
    return view != NULL ? egtb_view_set(view, index, side, value) :
                          egtb_set(database, index, side, value);
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
    } else if (contains_position(context->indexer, &successor)) {
        if (!position_index(context->indexer, &successor, &index)) {
            context->failed = true;
            return false;
        }
        if (!bitmap_test(context->won_at_most, index))
            context->all_winning = false;
        if (bitmap_test(context->won_exact, index))
            context->reaches_exact_win = true;
        return true;
    } else if (context->external_probe == NULL ||
               !context->external_probe(&successor, context->successor_side,
                                        context->external_context, &value) ||
               !valid_dtm(value)) {
        context->failed = true;
        return false;
    }
    if (value <= 0 || value > context->won_distance) {
        context->all_winning = false;
    } else if (value == context->won_distance)
        context->reaches_exact_win = true;
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
    if (!position_index(context->indexer, predecessor, &predecessor_index))
        return true;
    if (predecessor_index < context->first_index ||
        predecessor_index >= context->end_index)
        return true;
    ++context->statistics->predecessor_candidates[context->mover];
    memset(&forward, 0, sizeof(forward));
    forward.indexer = context->indexer;
    forward.mover = context->mover;
    forward.successor_side = context->successor_side;
    forward.predecessor = *predecessor;
    forward.won_distance = context->won_distance;
    forward.external_probe = context->external_probe;
    forward.external_context = context->external_context;
    forward.won_exact = context->won_exact;
    forward.won_at_most = context->won_at_most;
    forward.all_winning = true;
    if (!GENERATE_MOVES(predecessor, context->mover,
                                 check_forward_successor, &forward,
                                 &move_count) || forward.failed) {
        context->failed = true;
        return false;
    }
    if (move_count == 0 || !forward.all_winning ||
        !forward.reaches_exact_win)
        return true;
    if (!generator_get(context->database, context->view, predecessor_index,
                       context->mover, &old_value)) {
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
    if (!generator_set(context->database, context->view, predecessor_index,
                       context->mover, context->loss_value)) {
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
    Bitmap won_exact = {0};
    Bitmap won_at_most = {0};
    uint64_t position_count;
    unsigned successor_side;
    bool success = false;
    if (database == NULL || indexer == NULL || won_distance <= 0 ||
        won_distance % 2 == 0 || won_distance > EGTB_MAX_WIN_DTM)
        return fail("invalid win-to-loss backtrack argument");
    position_count = eg_position_count(indexer);
    if (position_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    if (!bitmap_create(&won_exact, position_count) ||
        !bitmap_create(&won_at_most, position_count)) {
        fail("cannot allocate win-frontier bitmaps");
        goto done;
    }
    for (successor_side = 0; successor_side < 2; ++successor_side) {
        uint64_t index;
        uint64_t first;
        bitmap_clear(&won_exact);
        bitmap_clear(&won_at_most);
        for (index = 0; index < position_count; ++index) {
            int16_t value;
            if (!egtb_get(database, index, (EgtbSide)successor_side,
                          &value)) {
                fail("cannot read index %llu, side %u: %s",
                     (unsigned long long)index, successor_side,
                     egtb_last_error());
                goto done;
            }
            if (value > 0 && value <= won_distance)
                bitmap_set(&won_at_most, index);
            if (value == won_distance)
                bitmap_set(&won_exact, index);
        }
        first = 0;
        while (bitmap_find_next(&won_exact, first, &index)) {
            EgPosition indexed;
            DraughtsPosition position;
            BacktrackContext context;
            EgtbSide mover = opposite_side((EgtbSide)successor_side);
            size_t predecessor_count;
            first = index + 1;
            if (!eg_index_to_position(indexer, index, &indexed)) {
                fail("cannot invert index %llu", (unsigned long long)index);
                goto done;
            }
            position.white_men = indexed.white_men;
            position.black_men = indexed.black_men;
            position.white_kings = indexed.white_kings;
            position.black_kings = indexed.black_kings;
            ++local.won_sources[mover];
            memset(&context, 0, sizeof(context));
            context.database = database;
            context.view = NULL;
            context.indexer = indexer;
            context.mover = mover;
            context.successor_side = (EgtbSide)successor_side;
            context.statistics = &local;
            context.won_distance = won_distance;
            context.loss_value = (int16_t)-(won_distance + 1);
            context.external_probe = external_probe;
            context.external_context = external_context;
            context.won_exact = &won_exact;
            context.won_at_most = &won_at_most;
            context.first_index = 0;
            context.end_index = position_count;
            if (!GENERATE_QUIET_PREDECESSORS(
                    &position, (EgtbSide)successor_side, check_predecessor,
                    &context, &predecessor_count) || context.failed) {
                fail("predecessor backtrack failed at index %llu, side %u",
                     (unsigned long long)index, successor_side);
                goto done;
            }
        }
    }
    success = true;
    if (statistics != NULL)
        *statistics = local;
done:
    bitmap_destroy(&won_at_most);
    bitmap_destroy(&won_exact);
    return success;
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
    EgtbLossBacktrackStatistics generic = {0};
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

static bool check_winning_predecessor(
    const DraughtsPosition *predecessor,
    const DraughtsMove *forward_move, void *opaque)
{
    WinBacktrackContext *context = opaque;
    uint64_t predecessor_index;
    int16_t old_value;
    (void)forward_move;
    if (!position_index(context->indexer, predecessor, &predecessor_index))
        return true;
    if (predecessor_index < context->first_index ||
        predecessor_index >= context->end_index)
        return true;
    ++context->statistics->predecessor_candidates[context->mover];
    if (!generator_get(context->database, context->view, predecessor_index,
                       context->mover, &old_value)) {
        context->failed = true;
        return false;
    }
    if (old_value > 0 && old_value <= context->win_value)
        return true;
    if (old_value != EGTB_DRAW && old_value <= 0) {
        context->failed = true;
        return false;
    }
    if (!generator_set(context->database, context->view, predecessor_index,
                       context->mover, context->win_value)) {
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
    Bitmap lost_exact = {0};
    uint64_t position_count;
    unsigned successor_side;
    bool success = false;
    (void)external_probe;
    (void)external_context;
    if (database == NULL || indexer == NULL || loss_distance <= 0 ||
        loss_distance % 2 != 0 ||
        loss_distance >= EGTB_MAX_WIN_DTM)
        return fail("invalid loss-to-win backtrack argument");
    position_count = eg_position_count(indexer);
    if (position_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    if (!bitmap_create(&lost_exact, position_count))
        return fail("cannot allocate loss-frontier bitmap");
    for (successor_side = 0; successor_side < 2; ++successor_side) {
        uint64_t index;
        uint64_t first;
        bitmap_clear(&lost_exact);
        for (index = 0; index < position_count; ++index) {
            int16_t value;
            if (!egtb_get(database, index, (EgtbSide)successor_side,
                          &value)) {
                fail("cannot read index %llu, side %u: %s",
                     (unsigned long long)index, successor_side,
                     egtb_last_error());
                goto done;
            }
            if (value == -loss_distance)
                bitmap_set(&lost_exact, index);
        }
        first = 0;
        while (bitmap_find_next(&lost_exact, first, &index)) {
            EgPosition indexed;
            DraughtsPosition position;
            WinBacktrackContext context;
            EgtbSide mover = opposite_side((EgtbSide)successor_side);
            size_t predecessor_count;
            first = index + 1;
            if (!eg_index_to_position(indexer, index, &indexed)) {
                fail("cannot invert index %llu", (unsigned long long)index);
                goto done;
            }
            position.white_men = indexed.white_men;
            position.black_men = indexed.black_men;
            position.white_kings = indexed.white_kings;
            position.black_kings = indexed.black_kings;
            ++local.loss_sources[mover];
            memset(&context, 0, sizeof(context));
            context.database = database;
            context.view = NULL;
            context.indexer = indexer;
            context.mover = mover;
            context.statistics = &local;
            context.win_value = (int16_t)(loss_distance + 1);
            context.first_index = 0;
            context.end_index = position_count;
            if (!GENERATE_QUIET_PREDECESSORS(
                    &position, (EgtbSide)successor_side,
                    check_winning_predecessor, &context,
                    &predecessor_count) || context.failed) {
                fail("winning predecessor backtrack failed at index %llu, side %u",
                     (unsigned long long)index, successor_side);
                goto done;
            }
        }
    }
    success = true;
    if (statistics != NULL)
        *statistics = local;
done:
    bitmap_destroy(&lost_exact);
    return success;
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
    EgtbGenericWinBacktrackStatistics generic = {0};
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

typedef enum {
    WORK_BUILD_WINS,
    WORK_BUILD_LOSSES,
    WORK_PREDECESSOR_LOSSES,
    WORK_PREDECESSOR_WINS
} RetrogradeWork;

typedef struct {
    Egtb *database;
    EgtbView *view;
    const EgIndexer *indexer;
    Bitmap *exact;
    Bitmap *at_most;
    uint64_t first_index;
    uint64_t end_index;
    EgtbSide successor_side;
    int16_t distance;
    EgtbExternalProbe external_probe;
    void *external_context;
    RetrogradeWork work;
    EgtbLossBacktrackStatistics loss_statistics;
    EgtbGenericWinBacktrackStatistics win_statistics;
    uint64_t source_count;
    bool failed;
    char error[256];
} RetrogradeWorker;

static void worker_error(RetrogradeWorker *worker, const char *message)
{
    worker->failed = true;
    snprintf(worker->error, sizeof(worker->error), "%s: %.180s", message,
             egtb_last_error());
}

static void *run_retrograde_worker(void *opaque)
{
    RetrogradeWorker *worker = opaque;
    uint64_t index;
    if (worker->work == WORK_BUILD_WINS ||
        worker->work == WORK_BUILD_LOSSES) {
        for (index = worker->first_index; index < worker->end_index; ++index) {
            int16_t value;
            if (!egtb_view_get(worker->view, index, worker->successor_side,
                               &value)) {
                worker_error(worker, "frontier lookup failed");
                return NULL;
            }
            if (worker->work == WORK_BUILD_WINS) {
                if (value > 0 && value <= worker->distance)
                    bitmap_set(worker->at_most, index);
                if (value != worker->distance)
                    continue;
            } else if (value != -worker->distance) {
                continue;
            }
            bitmap_set(worker->exact, index);
            ++worker->source_count;
        }
        return NULL;
    }

    index = 0;
    while (bitmap_find_next(worker->exact, index, &index)) {
        EgPosition indexed;
        DraughtsPosition position;
        size_t predecessor_count;
        uint64_t source = index++;
        if (!eg_index_to_position(worker->indexer, source, &indexed)) {
            worker_error(worker, "frontier index inversion failed");
            return NULL;
        }
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        if (worker->work == WORK_PREDECESSOR_LOSSES) {
            BacktrackContext context;
            memset(&context, 0, sizeof(context));
            context.database = worker->database;
            context.view = worker->view;
            context.indexer = worker->indexer;
            context.mover = opposite_side(worker->successor_side);
            context.successor_side = worker->successor_side;
            context.statistics = &worker->loss_statistics;
            context.won_distance = worker->distance;
            context.loss_value = (int16_t)-(worker->distance + 1);
            context.external_probe = worker->external_probe;
            context.external_context = worker->external_context;
            context.won_exact = worker->exact;
            context.won_at_most = worker->at_most;
            context.first_index = worker->first_index;
            context.end_index = worker->end_index;
            if (!GENERATE_QUIET_PREDECESSORS(
                    &position, worker->successor_side, check_predecessor,
                    &context, &predecessor_count) || context.failed) {
                worker_error(worker, "loss predecessor generation failed");
                return NULL;
            }
        } else {
            WinBacktrackContext context;
            memset(&context, 0, sizeof(context));
            context.database = worker->database;
            context.view = worker->view;
            context.indexer = worker->indexer;
            context.mover = opposite_side(worker->successor_side);
            context.statistics = &worker->win_statistics;
            context.win_value = (int16_t)(worker->distance + 1);
            context.first_index = worker->first_index;
            context.end_index = worker->end_index;
            if (!GENERATE_QUIET_PREDECESSORS(
                    &position, worker->successor_side,
                    check_winning_predecessor, &context,
                    &predecessor_count) || context.failed) {
                worker_error(worker, "winning predecessor generation failed");
                return NULL;
            }
        }
    }
    return NULL;
}

static bool run_workers(RetrogradeWorker *workers, pthread_t *threads,
                        unsigned thread_count)
{
    unsigned created = 0;
    unsigned i;
    for (i = 0; i < thread_count; ++i) {
        int error = pthread_create(&threads[i], NULL, run_retrograde_worker,
                                   &workers[i]);
        if (error != 0) {
            fail("cannot create generator thread: %s", strerror(error));
            break;
        }
        ++created;
    }
    for (i = 0; i < created; ++i) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0)
            return fail("cannot join generator thread: %s", strerror(error));
    }
    if (created != thread_count)
        return false;
    for (i = 0; i < thread_count; ++i) {
        if (workers[i].failed)
            return fail("worker %u failed: %s", i, workers[i].error);
    }
    return true;
}

static bool parallel_backtrack_wins(
    RetrogradeWorker *workers, pthread_t *threads, unsigned thread_count,
    Bitmap *exact, Bitmap *at_most, int16_t distance,
    EgtbLossBacktrackStatistics *statistics)
{
    EgtbLossBacktrackStatistics local = {0};
    unsigned side, i;
    for (side = 0; side < 2; ++side) {
        EgtbSide mover = opposite_side((EgtbSide)side);
        bitmap_clear(exact);
        bitmap_clear(at_most);
        for (i = 0; i < thread_count; ++i) {
            workers[i].work = WORK_BUILD_WINS;
            workers[i].successor_side = (EgtbSide)side;
            workers[i].distance = distance;
            workers[i].source_count = 0;
            workers[i].failed = false;
        }
        if (!run_workers(workers, threads, thread_count))
            return false;
        for (i = 0; i < thread_count; ++i)
            local.won_sources[mover] += workers[i].source_count;
        for (i = 0; i < thread_count; ++i) {
            workers[i].work = WORK_PREDECESSOR_LOSSES;
            memset(&workers[i].loss_statistics, 0,
                   sizeof(workers[i].loss_statistics));
            workers[i].failed = false;
        }
        if (!run_workers(workers, threads, thread_count))
            return false;
        for (i = 0; i < thread_count; ++i) {
            local.predecessor_candidates[mover] +=
                workers[i].loss_statistics.predecessor_candidates[mover];
            local.losses[mover] += workers[i].loss_statistics.losses[mover];
            local.shortened_losses[mover] +=
                workers[i].loss_statistics.shortened_losses[mover];
        }
    }
    *statistics = local;
    return true;
}

static bool parallel_backtrack_losses(
    RetrogradeWorker *workers, pthread_t *threads, unsigned thread_count,
    Bitmap *exact, int16_t distance,
    EgtbGenericWinBacktrackStatistics *statistics)
{
    EgtbGenericWinBacktrackStatistics local = {0};
    unsigned side, i;
    for (side = 0; side < 2; ++side) {
        EgtbSide mover = opposite_side((EgtbSide)side);
        bitmap_clear(exact);
        for (i = 0; i < thread_count; ++i) {
            workers[i].work = WORK_BUILD_LOSSES;
            workers[i].successor_side = (EgtbSide)side;
            workers[i].distance = distance;
            workers[i].source_count = 0;
            workers[i].failed = false;
        }
        if (!run_workers(workers, threads, thread_count))
            return false;
        for (i = 0; i < thread_count; ++i)
            local.loss_sources[mover] += workers[i].source_count;
        for (i = 0; i < thread_count; ++i) {
            workers[i].work = WORK_PREDECESSOR_WINS;
            memset(&workers[i].win_statistics, 0,
                   sizeof(workers[i].win_statistics));
            workers[i].failed = false;
        }
        if (!run_workers(workers, threads, thread_count))
            return false;
        for (i = 0; i < thread_count; ++i) {
            local.predecessor_candidates[mover] +=
                workers[i].win_statistics.predecessor_candidates[mover];
            local.wins[mover] += workers[i].win_statistics.wins[mover];
            local.shortened_wins[mover] +=
                workers[i].win_statistics.shortened_wins[mover];
        }
    }
    *statistics = local;
    return true;
}

typedef struct {
    Egtb *database;
    EgtbView *view;
    const EgtbResident *resident;
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
    return value == EGTB_DRAW ||
           (value > 0 && value <= EGTB_MAX_WIN_DTM && value % 2 != 0) ||
           (value <= 0 && value >= -EGTB_MAX_LOSS_DTM && value % 2 == 0);
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
    if (contains_position(context->indexer, successor)) {
        uint64_t index;
        if (!position_index(context->indexer, successor, &index) ||
            (context->resident != NULL
                 ? !egtb_resident_get(context->resident, index,
                                      context->successor_side, value)
                 : !generator_get(context->database, context->view, index,
                                  context->successor_side, value)))
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
                         EgtbView *view, const EgtbResident *resident,
                         const DraughtsPosition *position, EgtbSide side,
                         EgtbExternalProbe external_probe,
                         void *external_context, int16_t *value)
{
    ConsistencyMoveContext context;
    size_t move_count;
    memset(&context, 0, sizeof(context));
    context.database = database;
    context.view = view;
    context.resident = resident;
    context.indexer = indexer;
    context.position = *position;
    context.mover = side;
    context.successor_side = opposite_side(side);
    context.external_probe = external_probe;
    context.external_context = external_context;
    if (!GENERATE_MOVES(position, side, evaluate_consistency_move,
                                 &context, &move_count) || context.failed)
        return false;
    if (move_count == 0) {
        *value = 0;
    } else if (context.has_loss) {
        if (context.shortest_loss >= EGTB_MAX_WIN_DTM)
            return false;
        *value = (int16_t)(context.shortest_loss + 1);
    } else if (context.has_draw) {
        *value = EGTB_DRAW;
    } else {
        if (context.longest_win >= EGTB_MAX_LOSS_DTM)
            return false;
        *value = (int16_t)-(context.longest_win + 1);
    }
    return valid_dtm(*value);
}

typedef struct {
    const EgIndexer *indexer;
    Bitmap *affected;
    DraughtsPosition position;
    EgtbSide mover;
    bool failed;
} ConsistencyAffectedContext;

static bool mark_affected_position(ConsistencyAffectedContext *context,
                                   const DraughtsPosition *position)
{
    uint64_t index;
    if (!position_index(context->indexer, position, &index))
        return true;
    bitmap_set(context->affected, index);
    return true;
}

static bool mark_affected_predecessor(
    const DraughtsPosition *predecessor,
    const DraughtsMove *forward_move, void *opaque)
{
    ConsistencyAffectedContext *context = opaque;
    (void)forward_move;
    return mark_affected_position(context, predecessor);
}

static bool mark_affected_successor(const DraughtsMove *move, void *opaque)
{
    ConsistencyAffectedContext *context = opaque;
    DraughtsPosition successor;
    if (move->capture_count != 0)
        return true;
    successor = context->position;
    if (!draughts_do_move(&successor, context->mover, move, NULL)) {
        context->failed = true;
        return false;
    }
    /* A promotion changes material and belongs to another EGTB. */
    if (!has_indexer_material(context->indexer, &successor))
        return true;
    return mark_affected_position(context, &successor);
}

static bool mark_consistency_neighbours(
    const EgIndexer *indexer, const DraughtsPosition *position,
    Bitmap *affected)
{
    ConsistencyAffectedContext context;
    unsigned side;
    memset(&context, 0, sizeof(context));
    context.indexer = indexer;
    context.affected = affected;
    context.position = *position;
    for (side = 0; side < 2; ++side) {
        size_t count;
        context.mover = (EgtbSide)side;
        if (!GENERATE_QUIET_PREDECESSORS(
                position, (EgtbSide)side, mark_affected_predecessor,
                &context, &count) || context.failed ||
            !GENERATE_MOVES(position, (EgtbSide)side,
                            mark_affected_successor, &context, &count) ||
            context.failed)
            return false;
    }
    return true;
}

typedef struct {
    const EgIndexer *indexer;
    Bitmap *verified_positions;
    bool failed;
} ConsistencyUnverifyContext;

static bool mark_unverified_predecessor(
    const DraughtsPosition *predecessor,
    const DraughtsMove *forward_move, void *opaque)
{
    ConsistencyUnverifyContext *context = opaque;
    uint64_t index;
    (void)forward_move;
    if (!position_index(context->indexer, predecessor, &index))
        return true;
    bitmap_unset(context->verified_positions, index);
    return true;
}

static bool mark_consistency_unverified(
    const EgIndexer *indexer, uint64_t index,
    const DraughtsPosition *position, Bitmap *verified_positions)
{
    ConsistencyUnverifyContext context;
    unsigned side;
    if (verified_positions == NULL)
        return true;
    bitmap_unset(verified_positions, index);
    memset(&context, 0, sizeof(context));
    context.indexer = indexer;
    context.verified_positions = verified_positions;
    for (side = 0; side < 2; ++side) {
        size_t count;
        if (!GENERATE_QUIET_PREDECESSORS(
                position, (EgtbSide)side, mark_unverified_predecessor,
                &context, &count) || context.failed)
            return false;
    }
    return true;
}

static bool repair_consistency_position(
    Egtb *database, const EgIndexer *indexer, uint64_t index,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbConsistencyReporter reporter, void *reporter_context,
    Bitmap *affected, EgtbConsistencyStatistics *statistics,
    uint64_t *updates_this_pass)
{
    EgPosition indexed;
    DraughtsPosition position;
    bool changed = false;
    unsigned side;
    if (!eg_index_to_position(indexer, index, &indexed))
        return fail("cannot invert index %llu", (unsigned long long)index);
    position.white_men = indexed.white_men;
    position.black_men = indexed.black_men;
    position.white_kings = indexed.white_kings;
    position.black_kings = indexed.black_kings;
    for (side = 0; side < 2; ++side) {
        int16_t old_value = EGTB_DRAW, new_value = EGTB_DRAW;
        if (!egtb_get(database, index, (EgtbSide)side, &old_value) ||
            !expected_dtm(database, indexer, NULL, NULL, &position,
                          (EgtbSide)side, external_probe,
                          external_context, &new_value))
            return fail("cannot verify index %llu, side %u",
                        (unsigned long long)index, side);
        ++statistics->positions_checked;
        if (old_value == new_value)
            continue;
        if (reporter != NULL)
            reporter(index, (EgtbSide)side, &position, old_value,
                     new_value, reporter_context);
        if (old_value > 0 && new_value > 0 && new_value < old_value)
            ++statistics->shorter_wins[side];
        else if (old_value != EGTB_DRAW && old_value <= 0 &&
                 new_value <= 0 && new_value < old_value)
            ++statistics->longer_losses[side];
        else
            ++statistics->other_updates[side];
        if (!egtb_set(database, index, (EgtbSide)side, new_value))
            return fail("cannot correct index %llu, side %u: %s",
                        (unsigned long long)index, side, egtb_last_error());
        ++statistics->updates[side];
        ++*updates_this_pass;
        changed = true;
    }
    if (changed && !mark_consistency_neighbours(indexer, &position, affected))
        return fail("cannot collect affected positions at index %llu: %s",
                    (unsigned long long)index,
                    draughts_movegen_last_error());
    return true;
}

bool egtb_make_consistent(
    Egtb *database, const EgIndexer *indexer,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbConsistencyReporter reporter, void *reporter_context,
    EgtbConsistencyStatistics *statistics)
{
    EgtbConsistencyStatistics local = {0};
    Bitmap pending = {0}, affected = {0};
    uint64_t position_count;
    bool full_pass = true;
    bool ok = false;
    if (database == NULL || indexer == NULL)
        return fail("invalid consistency-pass argument");
    position_count = eg_position_count(indexer);
    if (position_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    if (egtb_is_readonly(database))
        return fail("cannot repair a read-only database");
    if (!bitmap_create(&pending, position_count) ||
        !bitmap_create(&affected, position_count)) {
        fail("cannot allocate consistency worklists");
        goto done;
    }
    for (;;) {
        uint64_t updates_this_pass = 0;
        uint64_t index = 0;
        ++local.passes;
        bitmap_clear(&affected);
        if (full_pass) {
            for (index = 0; index < position_count; ++index)
                if (!repair_consistency_position(
                        database, indexer, index, external_probe,
                        external_context, reporter, reporter_context,
                        &affected, &local, &updates_this_pass))
                    goto done;
            full_pass = false;
        } else {
            while (bitmap_find_next(&pending, index, &index)) {
                if (!repair_consistency_position(
                        database, indexer, index, external_probe,
                        external_context, reporter, reporter_context,
                        &affected, &local, &updates_this_pass))
                    goto done;
                ++index;
            }
        }
        if (updates_this_pass == 0)
            break;
        {
            Bitmap swap = pending;
            pending = affected;
            affected = swap;
        }
    }
    if (statistics != NULL)
        *statistics = local;
    ok = true;
done:
    bitmap_destroy(&affected);
    bitmap_destroy(&pending);
    return ok;
}

typedef struct {
    uint64_t index;
    EgtbSide side;
    int16_t old_value;
    int16_t new_value;
} ConsistencyCorrection;

typedef struct {
    Egtb *database;
    EgtbView *scan_view;
    EgtbView *successor_view;
    const EgIndexer *indexer;
    EgtbExternalProbe external_probe;
    void *external_context;
    const Bitmap *pending;
    Bitmap *verified_positions;
    bool full_pass;
    uint64_t first_index;
    uint64_t end_index;
    uint64_t positions_checked;
    ConsistencyCorrection *corrections;
    size_t correction_count;
    size_t correction_capacity;
    bool failed;
} ConsistencyRepairWorker;

static bool append_consistency_correction(
    ConsistencyRepairWorker *worker, uint64_t index, EgtbSide side,
    int16_t old_value, int16_t new_value)
{
    ConsistencyCorrection *correction;
    if (worker->correction_count == worker->correction_capacity) {
        size_t capacity = worker->correction_capacity == 0
                              ? 64 : worker->correction_capacity * 2;
        ConsistencyCorrection *corrections;
        if (capacity > SIZE_MAX / sizeof(*corrections))
            return false;
        corrections = realloc(worker->corrections,
                              capacity * sizeof(*corrections));
        if (corrections == NULL)
            return false;
        worker->corrections = corrections;
        worker->correction_capacity = capacity;
    }
    correction = &worker->corrections[worker->correction_count++];
    correction->index = index;
    correction->side = side;
    correction->old_value = old_value;
    correction->new_value = new_value;
    return true;
}

static bool inspect_consistency_position(ConsistencyRepairWorker *worker,
                                         uint64_t index,
                                         const int16_t old_values[2])
{
    EgPosition indexed;
    DraughtsPosition position;
    size_t corrections_before = worker->correction_count;
    unsigned side;
    if (!eg_index_to_position(worker->indexer, index, &indexed))
        return false;
    position.white_men = indexed.white_men;
    position.black_men = indexed.black_men;
    position.white_kings = indexed.white_kings;
    position.black_kings = indexed.black_kings;
    for (side = 0; side < 2; ++side) {
        int16_t old_value = old_values[side], new_value = EGTB_DRAW;
        if (!expected_dtm(worker->database, worker->indexer,
                          worker->successor_view, NULL,
                          &position, (EgtbSide)side,
                          worker->external_probe, worker->external_context,
                          &new_value))
            return false;
        ++worker->positions_checked;
        if (old_value != new_value &&
            !append_consistency_correction(
                worker, index, (EgtbSide)side, old_value, new_value))
            return false;
    }
    if (worker->full_pass && worker->verified_positions != NULL &&
        worker->correction_count == corrections_before)
        bitmap_set(worker->verified_positions, index);
    return true;
}

static void *run_consistency_repair_worker(void *opaque)
{
    ConsistencyRepairWorker *worker = opaque;
    uint64_t index;
    worker->positions_checked = 0;
    worker->correction_count = 0;
    worker->failed = false;
    if (worker->full_pass) {
        EgtbSequentialReader reader;
        if (!egtb_sequential_reader_init(&reader, worker->scan_view,
                                         worker->first_index,
                                         worker->end_index)) {
            worker->failed = true;
            return NULL;
        }
        for (index = worker->first_index; index < worker->end_index; ++index) {
            int16_t old_values[2];
            if (!egtb_sequential_reader_next(&reader, &old_values[0],
                                             &old_values[1]) ||
                !inspect_consistency_position(worker, index, old_values)) {
                worker->failed = true;
                return NULL;
            }
        }
    } else {
        index = worker->first_index;
        while (bitmap_find_next(worker->pending, index, &index) &&
               index < worker->end_index) {
            uint64_t current = index++;
            int16_t old_values[2];
            if (!egtb_view_get_pair(worker->scan_view, current,
                                    &old_values[0], &old_values[1]) ||
                !inspect_consistency_position(worker, current, old_values)) {
                worker->failed = true;
                return NULL;
            }
        }
    }
    return NULL;
}

static bool close_consistency_repair_views(ConsistencyRepairWorker *workers,
                                           unsigned thread_count,
                                           EgtbCacheStatistics *statistics)
{
    unsigned i;
    bool ok = true;
    for (i = 0; i < thread_count; ++i) {
        EgtbView **views[2] = {&workers[i].scan_view,
                              &workers[i].successor_view};
        unsigned view_index;
        for (view_index = 0; view_index < 2; ++view_index) {
            if (*views[view_index] != NULL) {
                EgtbCacheStatistics cache;
                if (statistics != NULL) {
                    egtb_view_cache_statistics(*views[view_index], &cache);
                    add_cache_statistics(statistics, &cache);
                }
                if (!egtb_view_close(*views[view_index]))
                    ok = false;
                *views[view_index] = NULL;
            }
        }
    }
    return ok;
}

static bool apply_consistency_corrections(
    Egtb *database, const EgIndexer *indexer,
    ConsistencyRepairWorker *workers, unsigned thread_count,
    EgtbConsistencyReporter reporter, void *reporter_context,
    Bitmap *affected, Bitmap *verified_positions,
    EgtbConsistencyStatistics *statistics,
    uint64_t *updates_this_pass)
{
    unsigned worker_index;
    uint64_t last_index = UINT64_MAX;
    DraughtsPosition position = {0};
    for (worker_index = 0; worker_index < thread_count; ++worker_index) {
        ConsistencyRepairWorker *worker = &workers[worker_index];
        size_t correction_index;
        for (correction_index = 0;
             correction_index < worker->correction_count;
             ++correction_index) {
            const ConsistencyCorrection *correction =
                &worker->corrections[correction_index];
            unsigned side = (unsigned)correction->side;
            if (correction->index != last_index) {
                EgPosition indexed;
                if (last_index != UINT64_MAX &&
                    (!mark_consistency_neighbours(indexer, &position,
                                                  affected) ||
                     !mark_consistency_unverified(
                         indexer, last_index, &position,
                         verified_positions)))
                    return fail("cannot collect consistency neighbours");
                if (!eg_index_to_position(indexer, correction->index,
                                          &indexed))
                    return fail("cannot invert correction index %llu",
                                (unsigned long long)correction->index);
                position.white_men = indexed.white_men;
                position.black_men = indexed.black_men;
                position.white_kings = indexed.white_kings;
                position.black_kings = indexed.black_kings;
                last_index = correction->index;
            }
            if (reporter != NULL)
                reporter(correction->index, correction->side, &position,
                         correction->old_value, correction->new_value,
                         reporter_context);
            if (correction->old_value > 0 && correction->new_value > 0 &&
                correction->new_value < correction->old_value)
                ++statistics->shorter_wins[side];
            else if (correction->old_value != EGTB_DRAW &&
                     correction->old_value <= 0 &&
                     correction->new_value <= 0 &&
                     correction->new_value < correction->old_value)
                ++statistics->longer_losses[side];
            else
                ++statistics->other_updates[side];
            if (!egtb_set(database, correction->index, correction->side,
                          correction->new_value))
                return fail("cannot apply correction at index %llu: %s",
                            (unsigned long long)correction->index,
                            egtb_last_error());
            ++statistics->updates[side];
            ++*updates_this_pass;
        }
    }
    if (last_index != UINT64_MAX &&
        (!mark_consistency_neighbours(indexer, &position, affected) ||
         !mark_consistency_unverified(indexer, last_index, &position,
                                      verified_positions)))
        return fail("cannot collect consistency neighbours");
    return true;
}

bool egtb_make_consistent_threaded(
    Egtb *database, const EgIndexer *indexer,
    EgtbExternalProbe external_probe, void *external_context,
    EgtbConsistencyReporter reporter, void *reporter_context,
    const EgtbVerificationOptions *options,
    Bitmap *verified_positions,
    EgtbConsistencyStatistics *statistics)
{
    EgtbConsistencyStatistics local = {0};
    ConsistencyRepairWorker *workers = NULL;
    pthread_t *threads = NULL;
    Bitmap pending = {0}, affected = {0};
    uint64_t position_count, page_count, pages_per_worker, extra_pages;
    uint64_t entries_per_page;
    size_t original_cache_pages = 0;
    unsigned thread_count, i, created_threads = 0;
    bool full_pass = true;
    bool cache_shrunk = false;
    bool ok = false;
    if (database == NULL || indexer == NULL || options == NULL ||
        options->thread_count == 0 ||
        options->thread_count > EGTB_MAX_THREADS ||
        options->cache_pages == 0 || egtb_is_readonly(database))
        return fail("invalid threaded consistency-repair options");
    position_count = eg_position_count(indexer);
    page_count = egtb_page_count(database);
    if (position_count == 0 || page_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    if (verified_positions != NULL &&
        (verified_positions->words == NULL ||
         verified_positions->bit_count != position_count))
        return fail("verified-position bitmap has the wrong size");
    if (verified_positions != NULL &&
        egtb_positions_per_page(database) % 64 != 0)
        return fail("verified-position bitmap requires a multiple of 64 "
                    "entries per page");
    if (verified_positions != NULL)
        bitmap_clear(verified_positions);
    thread_count = options->thread_count;
    if (page_count < thread_count)
        thread_count = (unsigned)page_count;
    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (workers == NULL || threads == NULL ||
        !bitmap_create(&pending, position_count) ||
        !bitmap_create(&affected, position_count)) {
        fail("cannot allocate threaded consistency workspace");
        goto done;
    }
    entries_per_page = egtb_positions_per_page(database);
    pages_per_worker = page_count / thread_count;
    extra_pages = page_count % thread_count;
    for (i = 0; i < thread_count; ++i) {
        uint64_t first_page = i * pages_per_worker +
                              (i < extra_pages ? i : extra_pages);
        uint64_t owned_pages = pages_per_worker + (i < extra_pages);
        workers[i].database = database;
        workers[i].indexer = indexer;
        workers[i].external_probe = external_probe;
        workers[i].external_context = options->external_contexts != NULL
                                          ? options->external_contexts[i]
                                          : external_context;
        workers[i].verified_positions = verified_positions;
        workers[i].first_index = first_page * entries_per_page;
        workers[i].end_index =
            (first_page + owned_pages) * entries_per_page;
        if (workers[i].end_index > position_count)
            workers[i].end_index = position_count;
    }
    original_cache_pages = egtb_cache_pages(database);
    if (!egtb_resize_cache(database, 1)) {
        fail("cannot release shared cache for consistency repair: %s",
             egtb_last_error());
        goto done;
    }
    cache_shrunk = true;
    for (;;) {
        uint64_t updates_this_pass = 0;
        bitmap_clear(&affected);
        ++local.passes;
        if (!egtb_flush(database)) {
            fail("cannot flush consistency snapshot: %s", egtb_last_error());
            goto done;
        }
        for (i = 0; i < thread_count; ++i) {
            size_t cache_pages = options->cache_pages / thread_count +
                                 (i < options->cache_pages % thread_count);
            uint64_t first_page = workers[i].first_index / entries_per_page;
            uint64_t end_page = (workers[i].end_index +
                                 entries_per_page - 1) / entries_per_page;
            if (cache_pages == 0)
                cache_pages = 1;
            workers[i].pending = &pending;
            workers[i].full_pass = full_pass;
            if (!egtb_view_create_range(&workers[i].scan_view, database, 1,
                                        false, first_page, end_page) ||
                !egtb_view_create(&workers[i].successor_view, database,
                                  cache_pages, false)) {
                fail("cannot create consistency views %u: %s", i,
                     egtb_last_error());
                goto done;
            }
        }
        created_threads = 0;
        for (i = 0; i < thread_count; ++i) {
            int error = pthread_create(&threads[i], NULL,
                                       run_consistency_repair_worker,
                                       &workers[i]);
            if (error != 0) {
                fail("cannot create consistency worker %u: %s", i,
                     strerror(error));
                break;
            }
            ++created_threads;
        }
        for (i = 0; i < created_threads; ++i) {
            int error = pthread_join(threads[i], NULL);
            if (error != 0) {
                fail("cannot join consistency worker %u: %s", i,
                     strerror(error));
                goto done;
            }
        }
        if (created_threads != thread_count)
            goto done;
        for (i = 0; i < thread_count; ++i) {
            local.positions_checked += workers[i].positions_checked;
            if (workers[i].failed) {
                fail("consistency worker %u failed", i);
                goto done;
            }
        }
        if (!close_consistency_repair_views(workers, thread_count,
                                            &local.cache)) {
            fail("cannot close consistency views: %s", egtb_last_error());
            goto done;
        }
        if (!apply_consistency_corrections(
                database, indexer, workers, thread_count, reporter,
                reporter_context, &affected, verified_positions, &local,
                &updates_this_pass))
            goto done;
        if (updates_this_pass == 0)
            break;
        {
            Bitmap swap = pending;
            pending = affected;
            affected = swap;
        }
        full_pass = false;
    }
    if (!egtb_resize_cache(database, original_cache_pages)) {
        fail("cannot restore shared cache after consistency repair: %s",
             egtb_last_error());
        goto done;
    }
    cache_shrunk = false;
    if (statistics != NULL)
        *statistics = local;
    ok = true;
done:
    if (workers != NULL &&
        !close_consistency_repair_views(workers, thread_count, NULL))
        ok = false;
    if (cache_shrunk && !egtb_resize_cache(database, original_cache_pages))
        ok = false;
    if (workers != NULL)
        for (i = 0; i < thread_count; ++i)
            free(workers[i].corrections);
    bitmap_destroy(&affected);
    bitmap_destroy(&pending);
    free(threads);
    free(workers);
    return ok;
}

typedef struct {
    Egtb *database;
    EgtbView *scan_view;
    EgtbView *successor_view;
    const EgtbResident *resident;
    const EgIndexer *indexer;
    EgtbExternalProbe external_probe;
    void *external_context;
    const Bitmap *verified_positions;
    uint64_t first_index;
    uint64_t end_index;
    uint64_t positions_checked;
    uint64_t positions_skipped;
    uint64_t mismatch_index;
    EgtbSide mismatch_side;
    int16_t stored_value;
    int16_t expected_value;
    bool mismatch;
    bool failed;
} ConsistencyVerifyWorker;

static void *run_consistency_verify_worker(void *opaque)
{
    ConsistencyVerifyWorker *worker = opaque;
    EgtbSequentialReader reader;
    bool sequential = worker->resident == NULL &&
                      worker->verified_positions == NULL;
    uint64_t index;
    if (sequential &&
        !egtb_sequential_reader_init(&reader, worker->scan_view,
                                     worker->first_index,
                                     worker->end_index)) {
        worker->failed = true;
        return NULL;
    }
    for (index = worker->first_index; index < worker->end_index; ++index) {
        EgPosition indexed;
        DraughtsPosition position;
        int16_t stored_values[2];
        unsigned side;
        if (worker->verified_positions != NULL &&
            bitmap_test(worker->verified_positions, index)) {
            worker->positions_skipped += 2;
            continue;
        }
        if ((worker->resident != NULL &&
             !egtb_resident_get_pair(worker->resident, index,
                                     &stored_values[0], &stored_values[1])) ||
            (worker->resident == NULL && sequential &&
             !egtb_sequential_reader_next(&reader, &stored_values[0],
                                          &stored_values[1])) ||
            (worker->resident == NULL && !sequential &&
             !egtb_view_get_pair(worker->scan_view, index,
                                 &stored_values[0], &stored_values[1]))) {
            worker->failed = true;
            return NULL;
        }
        if (!eg_index_to_position(worker->indexer, index, &indexed)) {
            worker->failed = true;
            return NULL;
        }
        position.white_men = indexed.white_men;
        position.black_men = indexed.black_men;
        position.white_kings = indexed.white_kings;
        position.black_kings = indexed.black_kings;
        for (side = 0; side < 2; ++side) {
            int16_t stored = stored_values[side], expected;
            if (!expected_dtm(worker->database, worker->indexer,
                              worker->successor_view, worker->resident,
                              &position,
                              (EgtbSide)side,
                              worker->external_probe,
                              worker->external_context, &expected)) {
                worker->failed = true;
                return NULL;
            }
            ++worker->positions_checked;
            if (stored != expected) {
                worker->mismatch = true;
                worker->mismatch_index = index;
                worker->mismatch_side = (EgtbSide)side;
                worker->stored_value = stored;
                worker->expected_value = expected;
                return NULL;
            }
        }
    }
    return NULL;
}

bool egtb_verify_consistent_threaded(
    Egtb *database, const EgIndexer *indexer,
    EgtbExternalProbe external_probe, void *external_context,
    const EgtbVerificationOptions *options,
    const Bitmap *verified_positions,
    EgtbConsistencyStatistics *statistics)
{
    EgtbConsistencyStatistics local = {0};
    ConsistencyVerifyWorker *workers = NULL;
    pthread_t *threads = NULL;
    uint64_t position_count, page_count, pages_per_worker, extra_pages;
    uint64_t entries_per_page;
    unsigned thread_count, created_threads = 0, i;
    bool ok = false;
    if (database == NULL || indexer == NULL || options == NULL ||
        options->thread_count == 0 ||
        options->thread_count > EGTB_MAX_THREADS ||
        options->cache_pages == 0 || !egtb_is_readonly(database))
        return fail("invalid read-only consistency verification options");
    position_count = eg_position_count(indexer);
    page_count = egtb_page_count(database);
    if (position_count == 0 || page_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    if (verified_positions != NULL &&
        (verified_positions->words == NULL ||
         verified_positions->bit_count != position_count))
        return fail("verified-position bitmap has the wrong size");
    if (options->resident != NULL &&
        !egtb_resident_matches(options->resident, database))
        return fail("resident EGTB does not match verification database");
    thread_count = options->thread_count;
    if (page_count < thread_count)
        thread_count = (unsigned)page_count;
    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (workers == NULL || threads == NULL) {
        fail("cannot allocate consistency verification workers");
        goto done;
    }
    entries_per_page = egtb_positions_per_page(database);
    pages_per_worker = page_count / thread_count;
    extra_pages = page_count % thread_count;
    for (i = 0; i < thread_count; ++i) {
        uint64_t first_page = i * pages_per_worker +
                              (i < extra_pages ? i : extra_pages);
        uint64_t owned_pages = pages_per_worker + (i < extra_pages);
        size_t cache_pages = options->cache_pages / thread_count +
                             (i < options->cache_pages % thread_count);
        if (cache_pages == 0)
            cache_pages = 1;
        workers[i].database = database;
        workers[i].indexer = indexer;
        workers[i].external_probe = external_probe;
        workers[i].external_context = options->external_contexts != NULL
                                          ? options->external_contexts[i]
                                          : external_context;
        workers[i].verified_positions = verified_positions;
        workers[i].resident = options->resident;
        workers[i].first_index = first_page * entries_per_page;
        workers[i].end_index = (first_page + owned_pages) * entries_per_page;
        if (workers[i].end_index > position_count)
            workers[i].end_index = position_count;
        if (options->resident == NULL) {
            /* Keep the linear old-value stream out of the random probe cache. */
            if (!egtb_view_create_range(&workers[i].scan_view, database, 1,
                                        false, first_page,
                                        first_page + owned_pages) ||
                !egtb_view_create(&workers[i].successor_view, database,
                                  cache_pages, false)) {
                fail("cannot create verification views %u: %s", i,
                     egtb_last_error());
                goto done;
            }
        }
    }
    for (i = 0; i < thread_count; ++i) {
        int error = pthread_create(&threads[i], NULL,
                                   run_consistency_verify_worker,
                                   &workers[i]);
        if (error != 0) {
            fail("cannot create consistency verification thread %u: %s",
                 i, strerror(error));
            goto join;
        }
        ++created_threads;
    }
join:
    for (i = 0; i < created_threads; ++i) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0) {
            fail("cannot join consistency verification thread %u: %s",
                 i, strerror(error));
            goto done;
        }
    }
    if (created_threads != thread_count)
        goto done;
    local.passes = 1;
    for (i = 0; i < thread_count; ++i) {
        EgtbCacheStatistics cache;
        local.positions_checked += workers[i].positions_checked;
        local.positions_skipped += workers[i].positions_skipped;
        egtb_view_cache_statistics(workers[i].scan_view, &cache);
        add_cache_statistics(&local.cache, &cache);
        egtb_view_cache_statistics(workers[i].successor_view, &cache);
        add_cache_statistics(&local.cache, &cache);
        if (workers[i].failed) {
            fail("consistency verification worker %u failed", i);
            goto done;
        }
        if (workers[i].mismatch) {
            fail("final consistency mismatch at index %llu, side %s: "
                 "stored=%d expected=%d",
                 (unsigned long long)workers[i].mismatch_index,
                 workers[i].mismatch_side == EGTB_WHITE_TO_MOVE
                     ? "WTM" : "BTM",
                 workers[i].stored_value, workers[i].expected_value);
            goto done;
        }
    }
    if (statistics != NULL)
        *statistics = local;
    ok = true;
done:
    if (workers != NULL)
        for (i = 0; i < thread_count; ++i) {
            if (workers[i].scan_view != NULL &&
                !egtb_view_close(workers[i].scan_view))
                ok = false;
            if (workers[i].successor_view != NULL &&
                !egtb_view_close(workers[i].successor_view))
                ok = false;
        }
    free(threads);
    free(workers);
    return ok;
}

bool egtb_generate(Egtb *database, const EgIndexer *indexer,
                   EgtbExternalProbe external_probe,
                   void *external_context,
                   EgtbConsistencyReporter reporter,
                   void *reporter_context,
                   EgtbGenerationStatistics *statistics)
{
    EgtbGenerationStatistics local;
    EgtbConsistencyStatistics consistency = {0};
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
        EgtbLossBacktrackStatistics losses = {0};
        EgtbGenericWinBacktrackStatistics wins = {0};
        uint64_t loss_updates = 0;
        uint64_t win_updates = 0;
        int16_t loss_distance;
        if (won_distance > EGTB_MAX_WIN_DTM)
            return fail("DTM exceeds the 16-bit storage representation");
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
        if (loss_distance >= EGTB_MAX_WIN_DTM)
            return fail("DTM exceeds the 16-bit storage representation");
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
        if (won_distance > EGTB_MAX_WIN_DTM - 2)
            return fail("DTM exceeds the 16-bit storage representation");
        won_distance = (int16_t)(won_distance + 2);
    }
    if (!egtb_make_consistent(database, indexer, external_probe,
                              external_context, reporter, reporter_context,
                              &consistency))
        return false;
    local.consistency_passes = consistency.passes;
    for (side = 0; side < 2; ++side)
        local.consistency_updates[side] = consistency.updates[side];
    local.consistency_cache = consistency.cache;
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

typedef enum {
    FRONTIER_WORK_INITIALIZE,
    FRONTIER_WORK_WON_SOURCES,
    FRONTIER_WORK_LOST_SOURCES,
    FRONTIER_WORK_LOSS_CANDIDATES,
    FRONTIER_WORK_WIN_CANDIDATES,
    FRONTIER_WORK_COMPILE
} FrontierWork;

typedef struct {
    unsigned owner;
    FrontierWork work;
    FrontierStore *frontiers;
    Egtb *database;
    EgtbView *view;
    const EgIndexer *indexer;
    Bitmap *won[2];
    Bitmap *lost[2];
    Bitmap *candidates;
    EgtbExternalProbe external_probe;
    void *external_context;
    uint64_t first_index;
    uint64_t end_index;
    EgtbSide successor_side;
    int16_t distance;
    uint64_t source_count;
    uint64_t candidate_count;
    uint64_t update_count;
    size_t compilation_entries;
    EgtbEntry *compilation_buffer;
    uint64_t compilation_first;
    uint64_t compilation_end;
    int16_t compilation_value;
    EgtbInitializationStatistics initialization;
    bool failed;
    char error[256];
} FrontierWorker;

static void frontier_worker_error(FrontierWorker *worker,
                                  const char *format, ...)
{
    va_list arguments;
    worker->failed = true;
    va_start(arguments, format);
    vsnprintf(worker->error, sizeof(worker->error), format, arguments);
    va_end(arguments);
}

static bool initialize_frontier_range(FrontierWorker *worker)
{
    EgtbInitializationStatistics local = {0};
    uint64_t index;
    local.positions = worker->end_index - worker->first_index;
    for (index = worker->first_index; index < worker->end_index; ++index) {
        EgPosition indexed;
        DraughtsPosition position;
        unsigned side;
        if (!eg_index_to_position(worker->indexer, index, &indexed)) {
            frontier_worker_error(worker,
                                  "cannot invert initialization index %llu",
                                  (unsigned long long)index);
            return false;
        }
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
            context.indexer = worker->indexer;
            context.external_probe = worker->external_probe;
            context.external_context = worker->external_context;
            if (!GENERATE_MOVES(&position, (EgtbSide)side,
                                examine_successor, &context, &move_count) ||
                context.failed) {
                frontier_worker_error(
                    worker, "move generation failed at index %llu, side %u: %s",
                    (unsigned long long)index, side,
                    draughts_movegen_last_error());
                return false;
            }
            local.legal_moves[side] += move_count;
            if (move_count == 0) {
                value = 0;
                ++local.lost_in_zero[side];
            } else if (context.has_loss) {
                if (context.shortest_loss >= EGTB_MAX_WIN_DTM) {
                    frontier_worker_error(
                        worker, "initialized winning DTM exceeds 16-bit storage");
                    return false;
                }
                value = (int16_t)(context.shortest_loss + 1);
                if (value == 1)
                    ++local.won_in_one[side];
                else
                    ++local.external_wins[side];
            } else if (!context.has_internal && !context.has_draw) {
                if (context.longest_win >= EGTB_MAX_LOSS_DTM) {
                    frontier_worker_error(
                        worker, "initialized losing DTM exceeds 16-bit storage");
                    return false;
                }
                value = (int16_t)-(context.longest_win + 1);
                ++local.external_losses[side];
            } else {
                ++local.unknown[side];
            }
            if (value != EGTB_DRAW &&
                !frontier_store_append(worker->frontiers, worker->owner,
                                       (EgtbSide)side, value, index)) {
                frontier_worker_error(
                    worker, "cannot append initialization frontier: %s",
                    frontier_last_error());
                return false;
            }
        }
    }
    worker->initialization = local;
    return true;
}

typedef struct {
    const EgIndexer *indexer;
    Bitmap *candidates;
    bool failed;
} FrontierCandidateContext;

static bool mark_frontier_candidate(const DraughtsPosition *predecessor,
                                    const DraughtsMove *forward_move,
                                    void *opaque)
{
    FrontierCandidateContext *context = opaque;
    uint64_t index;
    (void)forward_move;
    if (!position_index(context->indexer, predecessor, &index))
        return true;
    bitmap_set_atomic(context->candidates, index);
    return true;
}

typedef struct {
    FrontierWorker *worker;
    Bitmap *result;
    Bitmap *opposite;
} FrontierSourceContext;

static bool process_frontier_source(uint64_t index, void *opaque)
{
    FrontierSourceContext *context = opaque;
    FrontierWorker *worker = context->worker;
    EgPosition indexed;
    DraughtsPosition position;
    FrontierCandidateContext candidates;
    size_t predecessor_count;
    if (index < worker->first_index || index >= worker->end_index) {
        frontier_worker_error(worker, "frontier index is owned by another worker");
        return false;
    }
    if (bitmap_test(context->result, index))
        return true;
    if (bitmap_test(context->opposite, index)) {
        frontier_worker_error(worker,
                              "position has conflicting frontier outcomes");
        return false;
    }
    bitmap_set(context->result, index);
    if (!eg_index_to_position(worker->indexer, index, &indexed)) {
        frontier_worker_error(worker, "cannot invert frontier index");
        return false;
    }
    position.white_men = indexed.white_men;
    position.black_men = indexed.black_men;
    position.white_kings = indexed.white_kings;
    position.black_kings = indexed.black_kings;
    memset(&candidates, 0, sizeof(candidates));
    candidates.indexer = worker->indexer;
    candidates.candidates = worker->candidates;
    if (!GENERATE_QUIET_PREDECESSORS(
            &position, worker->successor_side, mark_frontier_candidate,
            &candidates, &predecessor_count) || candidates.failed) {
        frontier_worker_error(worker, "cannot generate frontier predecessors");
        return false;
    }
    ++worker->source_count;
    return true;
}

typedef struct {
    const EgIndexer *indexer;
    const Bitmap *won;
    DraughtsPosition position;
    EgtbSide mover;
    EgtbSide successor_side;
    int16_t won_distance;
    EgtbExternalProbe external_probe;
    void *external_context;
    bool all_winning;
    bool failed;
} FrontierForwardContext;

static bool check_frontier_successor(const DraughtsMove *move, void *opaque)
{
    FrontierForwardContext *context = opaque;
    DraughtsPosition successor = context->position;
    uint64_t friendly, index;
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
    } else if (contains_position(context->indexer, &successor)) {
        if (!position_index(context->indexer, &successor, &index)) {
            context->failed = true;
            return false;
        }
        if (!bitmap_test(context->won, index))
            context->all_winning = false;
        return true;
    } else if (context->external_probe == NULL ||
               !context->external_probe(&successor, context->successor_side,
                                        context->external_context, &value) ||
               !valid_dtm(value)) {
        context->failed = true;
        return false;
    }
    if (value <= 0 || value > context->won_distance)
        context->all_winning = false;
    return true;
}

static bool compile_frontier_entry(uint64_t index, void *opaque)
{
    FrontierWorker *worker = opaque;
    if (index < worker->first_index || index >= worker->end_index) {
        frontier_worker_error(worker, "frontier index outside worker range");
        return false;
    }
    if (index >= worker->compilation_first && index < worker->compilation_end) {
        EgtbEntry *entry = worker->compilation_buffer +
                           (size_t)(index - worker->compilation_first);
        if (worker->successor_side == EGTB_WHITE_TO_MOVE)
            entry->white_to_move = worker->compilation_value;
        else
            entry->black_to_move = worker->compilation_value;
    }
    return true;
}

static void compile_frontier_range(FrontierWorker *worker)
{
    uint32_t per_page = egtb_positions_per_page(worker->database);
    EgtbEntry *buffer = malloc(worker->compilation_entries * sizeof(*buffer));
    if (buffer == NULL) {
        frontier_worker_error(worker, "cannot allocate frontier assembly buffer");
        return;
    }
    worker->compilation_buffer = buffer;
    for (uint64_t first = worker->first_index; first < worker->end_index;) {
        uint64_t count = worker->end_index - first;
        if (count > worker->compilation_entries)
            count = worker->compilation_entries;
        worker->compilation_first = first;
        worker->compilation_end = first + count;
        for (size_t i = 0; i < (size_t)count; ++i)
            buffer[i] = (EgtbEntry){EGTB_STORED_DRAW, EGTB_STORED_DRAW};
        /* Replay only this owner's streams, retaining the old longest-to-
         * shortest overwrite order for stale transposition records. */
        for (int distance = frontier_store_maximum_distance(worker->frontiers);
             distance >= 0; --distance) {
            int16_t value = (distance & 1) != 0 ? (int16_t)distance
                                               : (int16_t)-distance;
            if (!egtb_encode_dtm(value, &worker->compilation_value)) {
                frontier_worker_error(worker, "invalid compilation DTM");
                goto done;
            }
            for (unsigned side = 0; side < 2; ++side) {
                worker->successor_side = (EgtbSide)side;
                if (!frontier_store_visit(worker->frontiers, worker->owner,
                                          (EgtbSide)side, value,
                                          compile_frontier_entry, worker)) {
                    if (!worker->failed)
                        frontier_worker_error(worker,
                            "cannot read compile frontier: %s",
                            frontier_last_error());
                    goto done;
                }
            }
        }
        for (uint64_t offset = 0; offset < count; offset += per_page) {
            size_t page_count = count - offset < per_page
                                    ? (size_t)(count - offset) : per_page;
            if (!egtb_view_write_page(worker->view, (first + offset) / per_page,
                                      buffer + (size_t)offset, page_count)) {
                frontier_worker_error(worker, "cannot write compiled page: %s",
                                      egtb_last_error());
                goto done;
            }
        }
        first += count;
    }
done:
    free(buffer);
    worker->compilation_buffer = NULL;
}

static void *run_frontier_worker(void *opaque)
{
    FrontierWorker *worker = opaque;
    if (worker->work == FRONTIER_WORK_INITIALIZE) {
        initialize_frontier_range(worker);
        return NULL;
    }
    if (worker->work == FRONTIER_WORK_WON_SOURCES ||
        worker->work == FRONTIER_WORK_LOST_SOURCES) {
        FrontierSourceContext context;
        bool won = worker->work == FRONTIER_WORK_WON_SOURCES;
        context.worker = worker;
        context.result = won ? worker->won[worker->successor_side]
                             : worker->lost[worker->successor_side];
        context.opposite = won ? worker->lost[worker->successor_side]
                               : worker->won[worker->successor_side];
        if (!frontier_store_visit(worker->frontiers, worker->owner,
                                  worker->successor_side,
                                  won ? worker->distance : -worker->distance,
                                  process_frontier_source, &context) &&
            !worker->failed)
            frontier_worker_error(worker, "cannot read frontier: %s",
                                  frontier_last_error());
        return NULL;
    }
    if (worker->work == FRONTIER_WORK_LOSS_CANDIDATES ||
        worker->work == FRONTIER_WORK_WIN_CANDIDATES) {
        uint64_t index = worker->first_index;
        EgtbSide mover = opposite_side(worker->successor_side);
        while (bitmap_find_next(worker->candidates, index, &index) &&
               index < worker->end_index) {
            uint64_t candidate = index++;
            ++worker->candidate_count;
            if (worker->work == FRONTIER_WORK_LOSS_CANDIDATES) {
                EgPosition indexed;
                DraughtsPosition position;
                FrontierForwardContext forward;
                size_t move_count;
                if (bitmap_test(worker->lost[mover], candidate) ||
                    bitmap_test(worker->won[mover], candidate))
                    continue;
                if (!eg_index_to_position(worker->indexer, candidate,
                                          &indexed)) {
                    frontier_worker_error(worker,
                                          "cannot invert loss candidate");
                    return NULL;
                }
                position.white_men = indexed.white_men;
                position.black_men = indexed.black_men;
                position.white_kings = indexed.white_kings;
                position.black_kings = indexed.black_kings;
                memset(&forward, 0, sizeof(forward));
                forward.indexer = worker->indexer;
                forward.won = worker->won[worker->successor_side];
                forward.position = position;
                forward.mover = mover;
                forward.successor_side = worker->successor_side;
                forward.won_distance = worker->distance;
                forward.external_probe = worker->external_probe;
                forward.external_context = worker->external_context;
                forward.all_winning = true;
                if (!GENERATE_MOVES(&position, mover,
                                    check_frontier_successor, &forward,
                                    &move_count) || forward.failed) {
                    frontier_worker_error(worker,
                                          "cannot check loss candidate");
                    return NULL;
                }
                if (move_count == 0 || !forward.all_winning)
                    continue;
                if (!frontier_store_append(
                        worker->frontiers, worker->owner, mover,
                        (int16_t)-(worker->distance + 1), candidate)) {
                    frontier_worker_error(worker,
                                          "cannot append loss frontier: %s",
                                          frontier_last_error());
                    return NULL;
                }
            } else {
                if (bitmap_test(worker->won[mover], candidate))
                    continue;
                if (bitmap_test(worker->lost[mover], candidate)) {
                    frontier_worker_error(worker,
                                          "losing position also proved won");
                    return NULL;
                }
                if (!frontier_store_append(
                        worker->frontiers, worker->owner, mover,
                        (int16_t)(worker->distance + 1), candidate)) {
                    frontier_worker_error(worker,
                                          "cannot append win frontier: %s",
                                          frontier_last_error());
                    return NULL;
                }
            }
            ++worker->update_count;
        }
        return NULL;
    }
    if (worker->work == FRONTIER_WORK_COMPILE) {
        compile_frontier_range(worker);
    }
    return NULL;
}

static bool run_frontier_workers(FrontierWorker *workers, pthread_t *threads,
                                 unsigned thread_count)
{
    unsigned i, created = 0;
    for (i = 0; i < thread_count; ++i) {
        int error;
        workers[i].failed = false;
        workers[i].error[0] = '\0';
        error = pthread_create(&threads[i], NULL, run_frontier_worker,
                               &workers[i]);
        if (error != 0) {
            fail("cannot create frontier worker %u: %s", i, strerror(error));
            break;
        }
        ++created;
    }
    for (i = 0; i < created; ++i) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0)
            return fail("cannot join frontier worker %u: %s", i,
                        strerror(error));
    }
    if (created != thread_count)
        return false;
    for (i = 0; i < thread_count; ++i)
        if (workers[i].failed)
            return fail("frontier worker %u failed: %s", i,
                        workers[i].error);
    return true;
}

static bool initialize_frontier_store_parallel(
    FrontierWorker *workers, pthread_t *threads, unsigned thread_count,
    EgtbInitializationStatistics *statistics)
{
    EgtbInitializationStatistics total = {0};
    unsigned i, side;
    for (i = 0; i < thread_count; ++i) {
        workers[i].work = FRONTIER_WORK_INITIALIZE;
        memset(&workers[i].initialization, 0,
               sizeof(workers[i].initialization));
    }
    if (!run_frontier_workers(workers, threads, thread_count))
        return false;
    for (i = 0; i < thread_count; ++i) {
        const EgtbInitializationStatistics *part =
            &workers[i].initialization;
        total.positions += part->positions;
        for (side = 0; side < 2; ++side) {
            total.legal_moves[side] += part->legal_moves[side];
            total.lost_in_zero[side] += part->lost_in_zero[side];
            total.won_in_one[side] += part->won_in_one[side];
            total.external_wins[side] += part->external_wins[side];
            total.external_losses[side] += part->external_losses[side];
            total.unknown[side] += part->unknown[side];
        }
    }
    if (statistics != NULL)
        *statistics = total;
    return true;
}

typedef struct {
    Bitmap *result;
    Bitmap *opposite;
    bool failed;
} FrontierActivationContext;

static bool activate_frontier_entry(uint64_t index, void *opaque)
{
    FrontierActivationContext *context = opaque;
    if (bitmap_test(context->result, index))
        return true;
    if (bitmap_test(context->opposite, index)) {
        context->failed = true;
        return false;
    }
    bitmap_set(context->result, index);
    return true;
}

static bool activate_terminal_losses(FrontierStore *frontiers,
                                     Bitmap *won[2], Bitmap *lost[2],
                                     unsigned thread_count)
{
    unsigned side, owner;
    for (side = 0; side < 2; ++side)
        for (owner = 0; owner < thread_count; ++owner) {
            FrontierActivationContext context = {
                lost[side], won[side], false
            };
            if (!frontier_store_visit(frontiers, owner, (EgtbSide)side, 0,
                                      activate_frontier_entry, &context))
                return fail("cannot activate terminal losses: %s",
                            context.failed ? "conflicting outcome" :
                                             frontier_last_error());
        }
    return true;
}

static bool frontier_backtrack_layer(
    FrontierWorker *workers, pthread_t *threads, unsigned thread_count,
    Bitmap *candidates, EgtbSide successor_side, int16_t distance,
    bool won_sources, uint64_t *source_count, uint64_t *candidate_count,
    uint64_t *update_count)
{
    unsigned i;
    bitmap_clear(candidates);
    for (i = 0; i < thread_count; ++i) {
        workers[i].successor_side = successor_side;
        workers[i].distance = distance;
        workers[i].source_count = 0;
        workers[i].candidate_count = 0;
        workers[i].update_count = 0;
        workers[i].work = won_sources ? FRONTIER_WORK_WON_SOURCES
                                      : FRONTIER_WORK_LOST_SOURCES;
    }
    if (!run_frontier_workers(workers, threads, thread_count))
        return false;
    for (i = 0; i < thread_count; ++i)
        *source_count += workers[i].source_count;
    for (i = 0; i < thread_count; ++i)
        workers[i].work = won_sources ? FRONTIER_WORK_LOSS_CANDIDATES
                                      : FRONTIER_WORK_WIN_CANDIDATES;
    if (!run_frontier_workers(workers, threads, thread_count))
        return false;
    for (i = 0; i < thread_count; ++i) {
        *candidate_count += workers[i].candidate_count;
        *update_count += workers[i].update_count;
    }
    return true;
}

static bool egtb_generate_threaded_legacy(
                            Egtb *database, const EgIndexer *indexer,
                            EgtbExternalProbe external_probe,
                            void *external_context,
                            EgtbConsistencyReporter reporter,
                            void *reporter_context,
                            const EgtbThreadOptions *options,
                            EgtbGenerationStatistics *statistics)
{
    EgtbGenerationStatistics local = {0};
    EgtbConsistencyStatistics consistency = {0};
    RetrogradeWorker *workers = NULL;
    pthread_t *threads = NULL;
    Bitmap exact = {0}, at_most = {0};
    uint64_t position_count;
    uint64_t page_count;
    uint64_t pages_per_worker, extra_pages;
    size_t original_cache_pages;
    unsigned thread_count;
    unsigned created_views = 0;
    unsigned side, i;
    int16_t won_distance = 1;
    bool cache_shrunk = false;
    bool ok = false;
    void *initial_context;

    if (database == NULL || indexer == NULL || options == NULL ||
        options->thread_count == 0 ||
        options->thread_count > EGTB_MAX_THREADS ||
        options->writable_cache_pages == 0)
        return fail("invalid threaded-generation options");
    initial_context = options->external_contexts != NULL
                          ? options->external_contexts[0]
                          : external_context;
    if (options->thread_count == 1)
        return egtb_generate(database, indexer, external_probe,
                             initial_context, reporter, reporter_context,
                             statistics);
    position_count = eg_position_count(indexer);
    page_count = egtb_page_count(database);
    if (position_count == 0 || page_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    if (egtb_positions_per_page(database) % 64 != 0)
        return fail("threaded generation requires a multiple of 64 entries per page");
    thread_count = options->thread_count;
    if (page_count < thread_count)
        thread_count = (unsigned)page_count;
    if (options->writable_cache_pages < thread_count)
        return fail("writable cache must provide at least one page per thread");

    if (!egtb_initialize_terminal_positions_with_probe(
            database, indexer, external_probe, initial_context,
            &local.initialization))
        return false;
    if (local.initialization.won_in_one[0] != 0 ||
        local.initialization.won_in_one[1] != 0)
        local.maximum_dtm = 1;

    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (workers == NULL || threads == NULL ||
        !bitmap_create(&exact, position_count) ||
        !bitmap_create(&at_most, position_count)) {
        fail("cannot allocate threaded generator workspace");
        goto done;
    }
    original_cache_pages = egtb_cache_pages(database);
    if (!egtb_resize_cache(database, 1)) {
        fail("cannot release the shared writable cache: %s", egtb_last_error());
        goto done;
    }
    cache_shrunk = true;
    pages_per_worker = page_count / thread_count;
    extra_pages = page_count % thread_count;
    for (i = 0; i < thread_count; ++i) {
        uint64_t first_page = i * pages_per_worker +
                              (i < extra_pages ? i : extra_pages);
        uint64_t owned_pages = pages_per_worker + (i < extra_pages);
        uint64_t end_page = first_page + owned_pages;
        size_t cache_pages = options->writable_cache_pages / thread_count +
                             (i < options->writable_cache_pages % thread_count);
        workers[i].database = database;
        workers[i].indexer = indexer;
        workers[i].exact = &exact;
        workers[i].at_most = &at_most;
        workers[i].first_index =
            first_page * egtb_positions_per_page(database);
        workers[i].end_index =
            end_page * egtb_positions_per_page(database);
        if (workers[i].end_index > position_count)
            workers[i].end_index = position_count;
        workers[i].external_probe = external_probe;
        workers[i].external_context = options->external_contexts != NULL
                                          ? options->external_contexts[i]
                                          : external_context;
        if (!egtb_view_create_range(&workers[i].view, database, cache_pages,
                                    true, first_page, end_page)) {
            fail("cannot create writable view %u: %s", i,
                 egtb_last_error());
            goto done;
        }
        ++created_views;
    }

    for (;;) {
        EgtbLossBacktrackStatistics losses = {0};
        EgtbGenericWinBacktrackStatistics wins = {0};
        uint64_t loss_updates = 0, win_updates = 0;
        int16_t loss_distance;
        if (won_distance > EGTB_MAX_WIN_DTM) {
            fail("DTM exceeds the 16-bit storage representation");
            goto done;
        }
        loss_distance = (int16_t)(won_distance + 1);
        if (!parallel_backtrack_wins(workers, threads, thread_count,
                                     &exact, &at_most, won_distance,
                                     &losses))
            goto done;
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
        if (loss_distance >= EGTB_MAX_WIN_DTM) {
            fail("DTM exceeds the 16-bit storage representation");
            goto done;
        }
        if (!parallel_backtrack_losses(workers, threads, thread_count,
                                       &exact, loss_distance, &wins))
            goto done;
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
        if (won_distance > EGTB_MAX_WIN_DTM - 2) {
            fail("DTM exceeds the 16-bit storage representation");
            goto done;
        }
        won_distance = (int16_t)(won_distance + 2);
    }

    for (i = 0; i < created_views; ++i) {
        if (!egtb_view_close(workers[i].view)) {
            workers[i].view = NULL;
            fail("cannot close writable view %u: %s", i, egtb_last_error());
            goto done;
        }
        workers[i].view = NULL;
    }
    created_views = 0;
    if (!egtb_resize_cache(database, original_cache_pages)) {
        fail("cannot restore the shared writable cache: %s", egtb_last_error());
        goto done;
    }
    cache_shrunk = false;
    if (!egtb_make_consistent(database, indexer, external_probe,
                              initial_context, reporter, reporter_context,
                              &consistency))
        goto done;
    local.consistency_passes = consistency.passes;
    for (side = 0; side < 2; ++side)
        local.consistency_updates[side] = consistency.updates[side];
    local.consistency_cache = consistency.cache;
    local.maximum_dtm = 0;
    for (uint64_t index = 0; index < position_count; ++index) {
        for (side = 0; side < 2; ++side) {
            int16_t value;
            uint16_t distance;
            if (!egtb_get(database, index, (EgtbSide)side, &value))
                goto done;
            if (value == EGTB_DRAW)
                continue;
            distance = value < 0 ? (uint16_t)-value : (uint16_t)value;
            if (distance > local.maximum_dtm)
                local.maximum_dtm = distance;
        }
    }
    if (statistics != NULL)
        *statistics = local;
    ok = true;
done:
    for (i = 0; i < created_views; ++i) {
        if (workers[i].view != NULL) {
            if (!egtb_view_close(workers[i].view))
                ok = false;
            workers[i].view = NULL;
        }
    }
    if (cache_shrunk && !egtb_resize_cache(database, original_cache_pages))
        ok = false;
    bitmap_destroy(&at_most);
    bitmap_destroy(&exact);
    free(threads);
    free(workers);
    return ok;
}

bool egtb_generate_threaded(Egtb *database, const EgIndexer *indexer,
                            EgtbExternalProbe external_probe,
                            void *external_context,
                            EgtbConsistencyReporter reporter,
                            void *reporter_context,
                            const EgtbThreadOptions *options,
                            EgtbGenerationStatistics *statistics)
{
    EgtbGenerationStatistics local = {0};
    EgtbConsistencyStatistics consistency = {0};
    FrontierStore *frontiers = NULL;
    FrontierWorker *workers = NULL;
    pthread_t *threads = NULL;
    Bitmap won[2] = {{0}, {0}};
    Bitmap lost[2] = {{0}, {0}};
    Bitmap candidates = {0};
    Bitmap *won_ptrs[2] = {&won[0], &won[1]};
    Bitmap *lost_ptrs[2] = {&lost[0], &lost[1]};
    uint64_t position_count, page_count, pages_per_worker, extra_pages;
    size_t original_cache_pages = 0;
    unsigned thread_count, side, i, created_views = 0;
    int16_t won_distance = 1;
    bool cache_shrunk = false;
    bool ok = false;
    void *initial_context;
    double generation_started, phase_started;

    /* Retained temporarily for controlled A/B performance comparisons. */
    if (getenv("EGTB_LEGACY_SCAN") != NULL)
        return egtb_generate_threaded_legacy(
            database, indexer, external_probe, external_context, reporter,
            reporter_context, options, statistics);

    if (database == NULL || indexer == NULL || options == NULL ||
        options->thread_count == 0 ||
        options->thread_count > EGTB_MAX_THREADS ||
        options->writable_cache_pages == 0)
        return fail("invalid threaded-generation options");
    position_count = eg_position_count(indexer);
    page_count = egtb_page_count(database);
    if (position_count == 0 || page_count == 0 ||
        egtb_maximum_index(database) != position_count - 1)
        return fail("database and indexer sizes do not match");
    if (egtb_positions_per_page(database) % 64 != 0)
        return fail("threaded generation requires a multiple of 64 entries per page");
    thread_count = options->thread_count;
    if (page_count < thread_count)
        thread_count = (unsigned)page_count;
    if (options->writable_cache_pages < thread_count)
        return fail("writable cache must provide at least one page per thread");
    initial_context = options->external_contexts != NULL
                          ? options->external_contexts[0]
                          : external_context;
    generation_started = monotonic_seconds();

    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (workers == NULL || threads == NULL ||
        !bitmap_create(&won[0], position_count) ||
        !bitmap_create(&won[1], position_count) ||
        !bitmap_create(&lost[0], position_count) ||
        !bitmap_create(&lost[1], position_count) ||
        !bitmap_create(&candidates, position_count) ||
        !frontier_store_create(&frontiers, thread_count, 1)) {
        fail("cannot allocate frontier generator workspace: %s",
             frontier_last_error());
        goto done;
    }
    pages_per_worker = page_count / thread_count;
    extra_pages = page_count % thread_count;
    for (i = 0; i < thread_count; ++i) {
        uint64_t first_page = i * pages_per_worker +
                              (i < extra_pages ? i : extra_pages);
        uint64_t owned_pages = pages_per_worker + (i < extra_pages);
        uint64_t end_page = first_page + owned_pages;
        workers[i].owner = i;
        workers[i].frontiers = frontiers;
        workers[i].database = database;
        workers[i].indexer = indexer;
        workers[i].won[0] = &won[0];
        workers[i].won[1] = &won[1];
        workers[i].lost[0] = &lost[0];
        workers[i].lost[1] = &lost[1];
        workers[i].candidates = &candidates;
        workers[i].first_index =
            first_page * egtb_positions_per_page(database);
        workers[i].end_index =
            end_page * egtb_positions_per_page(database);
        if (workers[i].end_index > position_count)
            workers[i].end_index = position_count;
        workers[i].external_probe = external_probe;
        workers[i].external_context = options->external_contexts != NULL
                                          ? options->external_contexts[i]
                                          : external_context;
    }

    phase_started = monotonic_seconds();
    if (!initialize_frontier_store_parallel(
            workers, threads, thread_count, &local.initialization) ||
        !activate_terminal_losses(frontiers, won_ptrs, lost_ptrs,
                                  thread_count))
        goto done;
    local.initialization_seconds = monotonic_seconds() - phase_started;
    if (local.initialization.won_in_one[0] != 0 ||
        local.initialization.won_in_one[1] != 0)
        local.maximum_dtm = 1;

    phase_started = monotonic_seconds();
    for (;;) {
        int16_t loss_distance = (int16_t)(won_distance + 1);
        uint64_t loss_updates = 0, win_updates = 0;
        if (won_distance > EGTB_MAX_WIN_DTM) {
            fail("DTM exceeds the 16-bit storage representation");
            goto done;
        }
        for (side = 0; side < 2; ++side) {
            uint64_t sources = 0, candidate_count = 0, updates = 0;
            if (!frontier_backtrack_layer(
                    workers, threads, thread_count, &candidates,
                    (EgtbSide)side, won_distance, true, &sources,
                    &candidate_count, &updates))
                goto done;
            local.new_losses[opposite_side((EgtbSide)side)] += updates;
            loss_updates += updates;
        }
        ++local.retrograde_passes;
        if (loss_updates != 0 && (uint16_t)loss_distance > local.maximum_dtm)
            local.maximum_dtm = (uint16_t)loss_distance;

        if (frontier_store_maximum_distance(frontiers) >=
            (uint16_t)loss_distance) {
            if (loss_distance >= EGTB_MAX_WIN_DTM) {
                fail("DTM exceeds the 16-bit storage representation");
                goto done;
            }
            for (side = 0; side < 2; ++side) {
                uint64_t sources = 0, candidate_count = 0, updates = 0;
                if (!frontier_backtrack_layer(
                        workers, threads, thread_count, &candidates,
                        (EgtbSide)side, loss_distance, false, &sources,
                        &candidate_count, &updates))
                    goto done;
                local.new_wins[opposite_side((EgtbSide)side)] += updates;
                win_updates += updates;
            }
            ++local.retrograde_passes;
            if (win_updates != 0 &&
                (uint16_t)(loss_distance + 1) > local.maximum_dtm)
                local.maximum_dtm = (uint16_t)(loss_distance + 1);
        }
        if (frontier_store_maximum_distance(frontiers) <=
                (uint16_t)won_distance &&
            loss_updates == 0 && win_updates == 0)
            break;
        if (won_distance > EGTB_MAX_WIN_DTM - 2) {
            fail("DTM exceeds the 16-bit storage representation");
            goto done;
        }
        won_distance = (int16_t)(won_distance + 2);
    }
    local.backpropagation_seconds = monotonic_seconds() - phase_started;

    phase_started = monotonic_seconds();
    if (!frontier_store_finish(frontiers)) {
        fail("cannot finish frontier streams: %s", frontier_last_error());
        goto done;
    }
    original_cache_pages = egtb_cache_pages(database);
    if (!egtb_resize_cache(database, 1)) {
        fail("cannot release shared cache for compilation: %s",
             egtb_last_error());
        goto done;
    }
    cache_shrunk = true;
    for (i = 0; i < thread_count; ++i) {
        uint64_t first_page = workers[i].first_index /
                              egtb_positions_per_page(database);
        uint64_t end_page =
            (workers[i].end_index +
             egtb_positions_per_page(database) - 1) /
            egtb_positions_per_page(database);
        size_t total_bytes = options->compilation_buffer_bytes;
        size_t page_entries = egtb_positions_per_page(database);
        size_t buffer_entries;
        if (total_bytes == 0) {
            if (options->writable_cache_pages >
                SIZE_MAX / egtb_page_size(database)) {
                fail("compilation buffer size overflows size_t");
                goto done;
            }
            total_bytes = options->writable_cache_pages * egtb_page_size(database);
        }
        buffer_entries = (total_bytes / thread_count / sizeof(EgtbEntry) /
                          page_entries) * page_entries;
        if (buffer_entries < page_entries)
            buffer_entries = page_entries;
        if (workers[i].end_index - workers[i].first_index < buffer_entries)
            buffer_entries = (size_t)(workers[i].end_index - workers[i].first_index);
        workers[i].compilation_entries = buffer_entries;
        /* Only the output page(s) are cached; assembly has its own bounded
         * paired-entry buffer instead of a large random-write cache. */
        if (!egtb_view_create_range(&workers[i].view, database, 2,
                                    true, first_page, end_page)) {
            fail("cannot create compilation view %u: %s", i,
                 egtb_last_error());
            goto done;
        }
        ++created_views;
        workers[i].work = FRONTIER_WORK_COMPILE;
    }
    if (!run_frontier_workers(workers, threads, thread_count))
        goto done;
    for (i = 0; i < created_views; ++i) {
        if (!egtb_view_close(workers[i].view)) {
            workers[i].view = NULL;
            fail("cannot close compilation view %u: %s", i,
                 egtb_last_error());
            goto done;
        }
        workers[i].view = NULL;
    }
    created_views = 0;
    if (!egtb_resize_cache(database, original_cache_pages)) {
        fail("cannot restore shared cache after compilation: %s",
             egtb_last_error());
        goto done;
    }
    cache_shrunk = false;
    local.compilation_seconds = monotonic_seconds() - phase_started;

    phase_started = monotonic_seconds();
    frontier_store_destroy(frontiers);
    frontiers = NULL;
    bitmap_destroy(&candidates);
    bitmap_destroy(&lost[1]);
    bitmap_destroy(&lost[0]);
    bitmap_destroy(&won[1]);
    bitmap_destroy(&won[0]);
    if (options->verified_positions != NULL) {
        if (options->verified_positions->words != NULL ||
            !bitmap_create(options->verified_positions, position_count)) {
            fail("cannot allocate verified-position bitmap");
            goto done;
        }
    }
    {
        EgtbVerificationOptions repair_options = {
            thread_count, options->writable_cache_pages,
            options->external_contexts, NULL
        };
        if (!egtb_make_consistent_threaded(
                database, indexer, external_probe, initial_context,
                reporter, reporter_context, &repair_options,
                options->verified_positions,
                &consistency))
            goto done;
    }
    local.consistency_passes = consistency.passes;
    for (side = 0; side < 2; ++side)
        local.consistency_updates[side] = consistency.updates[side];
    local.consistency_cache = consistency.cache;
    local.consistency_seconds = monotonic_seconds() - phase_started;
    phase_started = monotonic_seconds();
    local.maximum_dtm = 0;
    for (uint64_t index = 0; index < position_count; ++index)
        for (side = 0; side < 2; ++side) {
            int16_t value;
            uint16_t distance;
            if (!egtb_get(database, index, (EgtbSide)side, &value))
                goto done;
            if (value == EGTB_DRAW)
                continue;
            distance = value < 0 ? (uint16_t)-value : (uint16_t)value;
            if (distance > local.maximum_dtm)
                local.maximum_dtm = distance;
        }
    local.final_scan_seconds = monotonic_seconds() - phase_started;
    local.total_seconds = monotonic_seconds() - generation_started;
    if (statistics != NULL)
        *statistics = local;
    ok = true;
done:
    for (i = 0; i < created_views; ++i)
        if (workers[i].view != NULL) {
            if (!egtb_view_close(workers[i].view))
                ok = false;
            workers[i].view = NULL;
        }
    if (cache_shrunk && !egtb_resize_cache(database, original_cache_pages))
        ok = false;
    frontier_store_destroy(frontiers);
    bitmap_destroy(&candidates);
    bitmap_destroy(&lost[1]);
    bitmap_destroy(&lost[0]);
    bitmap_destroy(&won[1]);
    bitmap_destroy(&won[0]);
    free(threads);
    free(workers);
    return ok;
}
