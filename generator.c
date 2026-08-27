#include "generator.h"

#include "bitmap.h"
#include "movegen.h"

#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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
                if (context.shortest_loss >= EGTB_MAX_WIN_DTM)
                    return fail("initialized winning DTM exceeds byte storage");
                value = (int16_t)(context.shortest_loss + 1);
                if (value == 1)
                    ++local.won_in_one[side];
                else
                    ++local.external_wins[side];
            } else if (!context.has_internal && !context.has_draw) {
                if (context.longest_win >= EGTB_MAX_LOSS_DTM)
                    return fail("initialized losing DTM exceeds byte storage");
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
    } else if (has_indexer_material(context->indexer, &successor)) {
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
    if (!position_index(context->indexer, predecessor, &predecessor_index)) {
        context->failed = true;
        return false;
    }
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
    if (!draughts_generate_moves(predecessor, context->mover,
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
            if (!draughts_generate_quiet_predecessors(
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

static bool check_winning_predecessor(
    const DraughtsPosition *predecessor,
    const DraughtsMove *forward_move, void *opaque)
{
    WinBacktrackContext *context = opaque;
    uint64_t predecessor_index;
    int16_t old_value;
    (void)forward_move;
    if (!position_index(context->indexer, predecessor, &predecessor_index)) {
        context->failed = true;
        return false;
    }
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
            if (!draughts_generate_quiet_predecessors(
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
            if (!draughts_generate_quiet_predecessors(
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
            if (!draughts_generate_quiet_predecessors(
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
        if (won_distance > EGTB_MAX_WIN_DTM)
            return fail("DTM exceeds the byte storage representation");
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
            return fail("DTM exceeds the byte storage representation");
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
            return fail("DTM exceeds the byte storage representation");
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

bool egtb_generate_threaded(Egtb *database, const EgIndexer *indexer,
                            EgtbExternalProbe external_probe,
                            void *external_context,
                            EgtbConsistencyReporter reporter,
                            void *reporter_context,
                            const EgtbThreadOptions *options,
                            EgtbGenerationStatistics *statistics)
{
    EgtbGenerationStatistics local = {0};
    EgtbConsistencyStatistics consistency;
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
    if ((egtb_page_size(database) / sizeof(EgtbEntry)) % 64 != 0)
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
            first_page * (egtb_page_size(database) / sizeof(EgtbEntry));
        workers[i].end_index =
            end_page * (egtb_page_size(database) / sizeof(EgtbEntry));
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
        EgtbLossBacktrackStatistics losses;
        EgtbGenericWinBacktrackStatistics wins;
        uint64_t loss_updates = 0, win_updates = 0;
        int16_t loss_distance;
        if (won_distance > EGTB_MAX_WIN_DTM) {
            fail("DTM exceeds the byte storage representation");
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
            fail("DTM exceeds the byte storage representation");
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
            fail("DTM exceeds the byte storage representation");
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
