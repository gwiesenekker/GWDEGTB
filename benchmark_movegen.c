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

int main(int argc, char **argv)
{
    EgIndexer indexer;
    DraughtsPosition *positions;
    uint64_t samples = DEFAULT_SAMPLES;
    uint64_t state = UINT64_C(0xd1b54a32d192ed03);
    uint64_t database_count;
    uint64_t sample, operations, captures = 0, moves = 0, predecessors = 0;
    uint64_t checksum = 0;
    unsigned maximum_capture = 0;
    double start, capture_seconds, generate_seconds, apply_seconds;
    double predecessor_seconds;

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

    for (sample = 0; sample < samples && sample < 10000; ++sample) {
        size_t ignored;
        draughts_generate_moves(&positions[sample], EGTB_WHITE_TO_MOVE,
                                NULL, NULL, &ignored);
        draughts_generate_moves(&positions[sample], EGTB_BLACK_TO_MOVE,
                                NULL, NULL, &ignored);
    }

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            if (draughts_has_capture(&positions[sample], (EgtbSide)side))
                ++captures;
        }
    }
    capture_seconds = now_seconds() - start;

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            size_t count;
            if (!draughts_generate_moves(&positions[sample], (EgtbSide)side,
                                         NULL, NULL, &count)) {
                fprintf(stderr, "move generation failed: %s\n",
                        draughts_movegen_last_error());
                free(positions);
                return EXIT_FAILURE;
            }
            moves += count;
        }
    }
    generate_seconds = now_seconds() - start;

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            ApplyContext context = {positions[sample], (EgtbSide)side,
                                    0, 0, 0, false};
            size_t count;
            if (!draughts_generate_moves(&positions[sample], (EgtbSide)side,
                                         apply_move, &context, &count) ||
                context.failed) {
                fprintf(stderr, "move/apply benchmark failed: %s\n",
                        draughts_movegen_last_error());
                free(positions);
                return EXIT_FAILURE;
            }
            checksum += context.checksum + count;
            if (context.maximum_capture > maximum_capture)
                maximum_capture = context.maximum_capture;
        }
    }
    apply_seconds = now_seconds() - start;

    start = now_seconds();
    for (sample = 0; sample < samples; ++sample) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            size_t count;
            if (!draughts_generate_quiet_predecessors(
                    &positions[sample], (EgtbSide)side, NULL, NULL, &count)) {
                fprintf(stderr, "predecessor generation failed: %s\n",
                        draughts_movegen_last_error());
                free(positions);
                return EXIT_FAILURE;
            }
            predecessors += count;
        }
    }
    predecessor_seconds = now_seconds() - start;

    printf("Move-generation benchmark: WM=1 BM=2 WK=2 BK=2\n");
    printf("positions=%" PRIu64 " side-to-move positions=%" PRIu64 "\n",
           samples, operations);
    printf("capture test:       %10.3f positions/s  capture positions=%.2f%%\n",
           (double)operations / capture_seconds,
           100.0 * (double)captures / (double)operations);
    printf("legal generation:   %10.3f positions/s  %10.3f moves/s  "
           "average=%.3f\n",
           (double)operations / generate_seconds,
           (double)moves / generate_seconds,
           (double)moves / (double)operations);
    printf("generation+do/undo: %10.3f positions/s  %10.3f moves/s  "
           "max-capture=%u\n",
           (double)operations / apply_seconds,
           (double)moves / apply_seconds, maximum_capture);
    printf("quiet predecessors: %10.3f positions/s  %10.3f predecessors/s  "
           "average=%.3f\n",
           (double)operations / predecessor_seconds,
           (double)predecessors / predecessor_seconds,
           (double)predecessors / (double)operations);
    printf("checksum=%" PRIu64 "\n", checksum);
    free(positions);
    return EXIT_SUCCESS;
}
