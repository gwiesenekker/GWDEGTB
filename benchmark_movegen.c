#define _POSIX_C_SOURCE 200809L

#include "endgame_index.h"
#include "movegen.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SAMPLES UINT64_C(1000000)

typedef struct {
    DraughtsPosition position;
    EgtbSide side;
    uint64_t checksum;
    uint64_t captures;
    unsigned maximum_capture;
    bool failed;
} ApplyContext;

typedef bool (*HasCaptureFunction)(const DraughtsPosition *, EgtbSide);
typedef bool (*GenerateMovesFunction)(
    const DraughtsPosition *, EgtbSide, DraughtsMoveVisitor, void *, size_t *);
typedef bool (*GeneratePredecessorsFunction)(
    const DraughtsPosition *, EgtbSide, DraughtsPredecessorVisitor, void *,
    size_t *);

typedef struct {
    const char *name;
    HasCaptureFunction has_capture;
    GenerateMovesFunction generate_moves;
    GeneratePredecessorsFunction generate_predecessors;
} MovegenBackend;

typedef struct {
    uint64_t captures;
    uint64_t moves;
    uint64_t predecessors;
    uint64_t checksum;
    unsigned maximum_capture;
    double capture_seconds;
    double generate_seconds;
    double apply_seconds;
    double predecessor_seconds;
} BenchmarkResult;

static double now_seconds(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)time.tv_sec + (double)time.tv_nsec * 1e-9;
}

static uint64_t next_random(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint64_t position_hash(const DraughtsPosition *position)
{
    return position->white_men ^ (position->black_men << 1) ^
           (position->white_kings << 2) ^ (position->black_kings << 3);
}

static bool apply_move(const DraughtsMove *move, void *opaque)
{
    ApplyContext *context = opaque;
    DraughtsPosition changed = context->position;
    DraughtsUndo undo;
    if (!draughts_do_move(&changed, context->side, move, &undo)) {
        context->failed = true;
        return false;
    }
    context->checksum += position_hash(&changed);
    if (move->capture_count != 0) {
        ++context->captures;
        if (move->capture_count > context->maximum_capture)
            context->maximum_capture = move->capture_count;
    }
    draughts_undo_move(&changed, &undo);
    context->checksum += position_hash(&changed);
    return true;
}

static bool parse_samples(const char *text, uint64_t *samples)
{
    char *end;
    uintmax_t value;
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value == 0 ||
        value > SIZE_MAX / sizeof(DraughtsPosition))
        return false;
    *samples = (uint64_t)value;
    return true;
}

static bool benchmark_backend(const MovegenBackend *backend,
                              const DraughtsPosition *positions,
                              uint64_t samples, BenchmarkResult *result)
{
    uint64_t sample;
    double start;
    memset(result, 0, sizeof(*result));

    for (sample = 0; sample < samples && sample < 10000; ++sample) {
        size_t ignored;
        backend->generate_moves(&positions[sample], EGTB_WHITE_TO_MOVE,
                                NULL, NULL, &ignored);
        backend->generate_moves(&positions[sample], EGTB_BLACK_TO_MOVE,
                                NULL, NULL, &ignored);
    }

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            if (backend->has_capture(&positions[sample], (EgtbSide)side))
                ++result->captures;
        }
    }
    result->capture_seconds = now_seconds() - start;

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            size_t count;
            if (!backend->generate_moves(&positions[sample], (EgtbSide)side,
                                         NULL, NULL, &count))
                return false;
            result->moves += count;
        }
    }
    result->generate_seconds = now_seconds() - start;

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            ApplyContext context = {positions[sample], (EgtbSide)side,
                                    0, 0, 0, false};
            size_t count;
            if (!backend->generate_moves(&positions[sample], (EgtbSide)side,
                                         apply_move, &context, &count) ||
                context.failed)
                return false;
            result->checksum += context.checksum + count;
            if (context.maximum_capture > result->maximum_capture)
                result->maximum_capture = context.maximum_capture;
        }
    }
    result->apply_seconds = now_seconds() - start;

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            size_t count;
            if (!backend->generate_predecessors(
                    &positions[sample], (EgtbSide)side, NULL, NULL, &count))
                return false;
            result->predecessors += count;
        }
    }
    result->predecessor_seconds = now_seconds() - start;
    return true;
}

static bool benchmark_padded_direct(const DraughtsPosition *positions,
                                    uint64_t samples,
                                    BenchmarkResult *result)
{
    DraughtsMove moves[DRAUGHTS_MOVES_MAX];
    uint64_t sample;
    double start;
    memset(result, 0, sizeof(*result));

    for (sample = 0; sample < samples && sample < 10000; ++sample) {
        size_t ignored;
        draughts_generate_moves_padded_into(
            &positions[sample], EGTB_WHITE_TO_MOVE, moves,
            DRAUGHTS_MOVES_MAX, &ignored);
        draughts_generate_moves_padded_into(
            &positions[sample], EGTB_BLACK_TO_MOVE, moves,
            DRAUGHTS_MOVES_MAX, &ignored);
    }
    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            size_t count;
            if (!draughts_generate_moves_padded_into(
                    &positions[sample], (EgtbSide)side, moves,
                    DRAUGHTS_MOVES_MAX, &count))
                return false;
            result->moves += count;
        }
    }
    result->generate_seconds = now_seconds() - start;

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            ApplyContext context = {positions[sample], (EgtbSide)side,
                                    0, 0, 0, false};
            size_t count, index;
            if (!draughts_generate_moves_padded_into(
                    &positions[sample], (EgtbSide)side, moves,
                    DRAUGHTS_MOVES_MAX, &count))
                return false;
            for (index = 0; index < count; ++index)
                if (!apply_move(&moves[index], &context))
                    return false;
            if (context.failed)
                return false;
            result->checksum += context.checksum + count;
            if (context.maximum_capture > result->maximum_capture)
                result->maximum_capture = context.maximum_capture;
        }
    }
    result->apply_seconds = now_seconds() - start;
    return true;
}

static void print_result(const MovegenBackend *backend,
                         const BenchmarkResult *result, uint64_t operations)
{
    printf("%s backend:\n", backend->name);
    printf("  capture test:       %10.3f positions/s  "
           "capture positions=%.2f%%\n",
           (double)operations / result->capture_seconds,
           100.0 * (double)result->captures / (double)operations);
    printf("  legal generation:   %10.3f positions/s  %10.3f moves/s  "
           "average=%.3f\n",
           (double)operations / result->generate_seconds,
           (double)result->moves / result->generate_seconds,
           (double)result->moves / (double)operations);
    printf("  generation+do/undo: %10.3f positions/s  %10.3f moves/s  "
           "max-capture=%u\n",
           (double)operations / result->apply_seconds,
           (double)result->moves / result->apply_seconds,
           result->maximum_capture);
    printf("  quiet predecessors: %10.3f positions/s  "
           "%10.3f predecessors/s  average=%.3f\n",
           (double)operations / result->predecessor_seconds,
           (double)result->predecessors / result->predecessor_seconds,
           (double)result->predecessors / (double)operations);
    printf("  checksum=%" PRIu64 "\n", result->checksum);
}

int main(int argc, char **argv)
{
    EgIndexer indexer;
    DraughtsPosition *positions;
    uint64_t samples = DEFAULT_SAMPLES;
    uint64_t state = UINT64_C(0xd1b54a32d192ed03);
    uint64_t database_count;
    uint64_t sample, operations;
    const MovegenBackend table = {
        "table", draughts_has_capture, draughts_generate_moves,
        draughts_generate_quiet_predecessors};
    const MovegenBackend padded = {
        "padded", draughts_has_capture_padded,
        draughts_generate_moves_padded,
        draughts_generate_quiet_predecessors_padded};
    BenchmarkResult table_result, padded_result, direct_result;

    if (argc == 3 && strcmp(argv[1], "--samples") == 0) {
        if (!parse_samples(argv[2], &samples)) {
            fprintf(stderr, "invalid sample count: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--samples COUNT]\n", argv[0]);
        return EXIT_FAILURE;
    }
    positions = malloc((size_t)samples * sizeof(*positions));
    if (positions == NULL) {
        fprintf(stderr, "cannot allocate benchmark positions\n");
        return EXIT_FAILURE;
    }
    if (!eg_indexer_init(&indexer, 1, 2, 2, 2)) {
        fprintf(stderr, "cannot initialize seven-piece indexer\n");
        free(positions);
        return EXIT_FAILURE;
    }
    database_count = eg_position_count(&indexer);
    for (sample = 0; sample < samples; ++sample) {
        EgPosition position;
        if (!eg_index_to_position(&indexer,
                                  next_random(&state) % database_count,
                                  &position)) {
            fprintf(stderr, "cannot create benchmark position\n");
            eg_indexer_destroy(&indexer);
            free(positions);
            return EXIT_FAILURE;
        }
        positions[sample].white_men = position.white_men;
        positions[sample].black_men = position.black_men;
        positions[sample].white_kings = position.white_kings;
        positions[sample].black_kings = position.black_kings;
    }
    eg_indexer_destroy(&indexer);
    operations = samples * 2;

    if (!benchmark_backend(&table, positions, samples, &table_result) ||
        !benchmark_backend(&padded, positions, samples, &padded_result) ||
        !benchmark_padded_direct(positions, samples, &direct_result)) {
        fprintf(stderr, "move-generation benchmark failed: %s\n",
                draughts_movegen_last_error());
        free(positions);
        return EXIT_FAILURE;
    }
    if (table_result.captures != padded_result.captures ||
        table_result.moves != padded_result.moves ||
        table_result.predecessors != padded_result.predecessors ||
        table_result.checksum != padded_result.checksum ||
        table_result.maximum_capture != padded_result.maximum_capture ||
        padded_result.moves != direct_result.moves ||
        padded_result.checksum != direct_result.checksum ||
        padded_result.maximum_capture != direct_result.maximum_capture) {
        fprintf(stderr, "move-generation backends produced different results\n");
        free(positions);
        return EXIT_FAILURE;
    }

    printf("Move-generation benchmark: WM=1 BM=2 WK=2 BK=2\n");
    printf("positions=%" PRIu64 " side-to-move positions=%" PRIu64 "\n",
           samples, operations);
    printf("padded conversion: %s\n",
           draughts_padded_backend_uses_bmi2() ? "BMI2 PDEP/PEXT" :
                                                 "portable fallback");
    print_result(&table, &table_result, operations);
    print_result(&padded, &padded_result, operations);
    printf("padded direct-output path:\n");
    printf("  legal generation:   %10.3f positions/s  %10.3f moves/s\n",
           (double)operations / direct_result.generate_seconds,
           (double)direct_result.moves / direct_result.generate_seconds);
    printf("  generation+do/undo: %10.3f positions/s  %10.3f moves/s\n",
           (double)operations / direct_result.apply_seconds,
           (double)direct_result.moves / direct_result.apply_seconds);
    printf("  callback/direct speedup: generation=%.3fx "
           "generation+do/undo=%.3fx\n",
           padded_result.generate_seconds / direct_result.generate_seconds,
           padded_result.apply_seconds / direct_result.apply_seconds);
    printf("padded/table speedup: capture=%.3fx generation=%.3fx "
           "generation+do/undo=%.3fx predecessors=%.3fx\n",
           table_result.capture_seconds / padded_result.capture_seconds,
           table_result.generate_seconds / padded_result.generate_seconds,
           table_result.apply_seconds / padded_result.apply_seconds,
           table_result.predecessor_seconds /
               padded_result.predecessor_seconds);
    free(positions);
    return EXIT_SUCCESS;
}
